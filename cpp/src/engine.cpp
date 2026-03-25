// engine.cpp -- Top-level orchestrator implementation for XOPTrader.
//
// The engine runs a single-threaded event loop driven by boost::asio.
// A native C++20 coroutine loop polls the Chia full node for block height
// every 5 seconds.  When a new block is detected, the 13-step heartbeat
// cycle executes as a co_await chain (no co_spawn/use_future/.get()
// patterns that risk deadlocking the io_context thread).
//
// Error handling strategy:
//   - Transient RPC errors (network, timeout) are caught per-step and
//     logged.  The cycle continues with stale data rather than aborting.
//   - Fatal errors (database corruption, schema mismatch) propagate as
//     exceptions and terminate the engine.
//   - The never-sell-at-loss constraint is checked in step 6 and again
//     in the OfferManager before any wallet RPC call.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- secrets never logged; audit trail via Database
//   ISO/IEC 5055       -- checked return codes, RAII resource management
//   ISO/IEC 25000      -- documented step sequencing, single-responsibility

#include "xop/engine.hpp"

#include "xop/strategy/avellaneda.hpp"
#include "xop/strategy/glft.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
// [CRITICAL-1/HIGH-2] Removed <boost/asio/use_future.hpp> and <future> --
// all blocking use_future/.get() patterns have been replaced with
// co_await (open_connections) or co_spawn(detached) (shutdown).
// ISO/IEC 5055: no dead includes.

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace xop {

namespace asio = boost::asio;

// [H9] Fallback XCH/USD rate when CEX feed is unavailable (Phase 2).
// ISO/IEC 5055: no magic numbers in financial calculations.
static constexpr double kFallbackXchUsdRate = 2.70;

// ===========================================================================
// Construction / destruction
// ===========================================================================

Engine::Engine(const AppConfig& config, bool dry_run)
    : config_(config)
    , dry_run_(dry_run)
    , ioc_()
    , poll_timer_(ioc_)
    , state_(std::make_shared<State>())
{
    spdlog::info("[Engine] Initializing subsystems (dry_run={})", dry_run);

    // -- Database (must be first: other subsystems may query on construction) --
    db_ = std::make_unique<Database>(config_.database.path);

    // -- RPC / API clients ----------------------------------------------------

    // Build full-node RPC config from AppConfig.
    rpc::ChiaRPCConfig fn_cfg;
    fn_cfg.host = config_.chia.full_node_host;
    fn_cfg.port = config_.chia.full_node_port;
    fn_cfg.tls.cert_path = config_.chia.ssl_cert_path;
    fn_cfg.tls.key_path  = config_.chia.ssl_key_path;
    full_node_ = std::make_shared<rpc::ChiaFullNodeRPC>(ioc_, fn_cfg);

    // Build wallet RPC config from AppConfig.
    rpc::ChiaRPCConfig wal_cfg;
    wal_cfg.host = config_.chia.wallet_host;
    wal_cfg.port = config_.chia.wallet_port;
    wal_cfg.tls.cert_path = config_.chia.wallet_cert_path;
    wal_cfg.tls.key_path  = config_.chia.wallet_key_path;
    wallet_ = std::make_shared<rpc::ChiaWalletRPC>(ioc_, wal_cfg);

    // Build dexie client config from AppConfig.
    rpc::DexieConfig dexie_cfg;
    dexie_cfg.base_url                = config_.dexie.api_base;
    dexie_cfg.rate_limit_max_requests = config_.dexie.max_requests_per_10s;
    dexie_ = std::make_shared<rpc::DexieClient>(ioc_, dexie_cfg);

    // -- Execution layer ------------------------------------------------------

    coin_mgr_ = std::make_unique<execution::CoinManager>(
        ioc_, wallet_, config_);

    // Pass the shared dexie client so OfferManager can submit offers to
    // the Dexie aggregator for cross-platform visibility.
    offer_mgr_ = std::make_unique<execution::OfferManager>(
        ioc_, wallet_, dexie_, state_, config_);

    // Market data feed: construct with a MarketDataConfig derived from
    // the top-level config (default thresholds are appropriate for Phase 1).
    MarketDataConfig md_cfg;
    market_data_ = std::make_unique<MarketDataFeed>(md_cfg, *state_);

    // -- Data / analytics (per-pair estimators) --------------------------------

    VolatilityEstimatorConfig vol_cfg;
    vol_cfg.lookback_blocks = config_.volatility.lookback_blocks;
    vol_cfg.yz_alpha        = config_.volatility.yz_alpha;

    AdverseSelectionConfig as_cfg;  // defaults are suitable for Phase 1

    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        vol_estimators_[pair.name] =
            std::make_unique<VolatilityEstimator>(vol_cfg);

        pin_estimators_[pair.name] =
            std::make_unique<AdverseSelectionEstimator>(as_cfg);
    }

    // -- Strategy layer -------------------------------------------------------

    // [T1-11] Construct per-pair Avellaneda-Stoikov strategy instances.
    // Each enabled pair gets its own StrategyBase so that price histories,
    // regime states, and internal estimators are isolated.  This prevents
    // state bleed between pairs with different volatility profiles.
    // A GLFT strategy can be substituted per-pair via config in the future.
    // ISO/IEC 5055: no shared mutable state across independent pairs.
    AvellanedaConfig as_strat_cfg;
    as_strat_cfg.gamma = config_.strategy.gamma;
    as_strat_cfg.kappa = config_.strategy.kappa;
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;
        strategies_[pair.name] =
            std::make_unique<AvellanedaStoikov>(as_strat_cfg);
    }

    // Spread optimizer with config-derived parameters.
    SpreadConfig sp_cfg;
    sp_cfg.gamma = config_.strategy.gamma;
    sp_cfg.s_floor_bps = config_.strategy.min_profit_margin_bps;
    spread_opt_ = std::make_unique<SpreadOptimizer>(sp_cfg);

    // Per-pair liquidity engines.
    LiquidityConfig liq_cfg;
    liq_cfg.num_tiers        = config_.strategy.num_tiers;
    liq_cfg.tier_spacing_bps = config_.strategy.tier_spacing_bps;
    liq_cfg.tier_size_pct    = config_.strategy.tier_size_pct;
    liq_cfg.offer_ttl_blocks = config_.strategy.offer_ttl_blocks;

    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;
        liquidity_engines_[pair.name] =
            std::make_unique<LiquidityEngine>(pair.name, liq_cfg);
    }

    // -- Risk layer -----------------------------------------------------------

    // Initial capital: query from wallet on first run; use 0 as placeholder
    // until the first balance query succeeds.
    inventory_ = std::make_unique<InventoryTracker>(
        config_.risk, Mojo{0}, /*no_loss_constraint=*/true);

    pre_trade_ = std::make_unique<PreTradeCheck>(
        config_.risk, config_.strategy);

    hedging_ = std::make_unique<HedgingManager>(
        config_.strategy, config_.risk);

    // -- Monitoring layer -----------------------------------------------------

    pnl_ = std::make_unique<PnLTracker>(config_.database.path);
    pnl_->init_database();

    metrics_ = std::make_unique<MetricsExporter>();

    alerts_ = std::make_unique<AlertManager>();
    alerts_->init(config_.monitoring.telegram_bot_token,
                  config_.monitoring.telegram_chat_id);

    // -- New strategy modules -----------------------------------------------
    order_book_tactician_ = std::make_unique<OrderBookTactician>(
        OrderBookTacticsConfig{});  // Default config for now

    strategy_portfolio_ = std::make_unique<StrategyPortfolio>(
        PortfolioConfig{});  // Default config with beta=2.0

    // ChiaEdgeOptimizer implements StrategyBase — construct with default config
    chia_edge_ = std::make_unique<ChiaEdgeOptimizer>(
        ChiaEdgeConfig{});

    coin_age_quoting_ = std::make_unique<CoinAgeWeightedQuoting>(
        CoinAgeConfig{});

    block_cadence_ = std::make_unique<BlockCadenceAdaptiveSpread>(
        BlockCadenceConfig{});

    mempool_sentinel_ = std::make_unique<MempoolSentinelStrategy>(
        MempoolSentinelConfig{});

    // -- New risk modules ---------------------------------------------------
    loss_manager_ = std::make_unique<StrategicLossManager>(
        LossManagerConfig{});  // Disabled by default (enabled=false)

    drift_analyzer_ = std::make_unique<InventoryDriftAnalyzer>(
        DriftConfig{});

    // -- Pair config lookup map -----------------------------------------------
    // Build the O(1) pair_config_map_ from config_.pairs so that every step
    // can resolve a pair name to its PairConfig without a linear scan.
    // ISO/IEC 5055: pointers remain valid because config_ is an immutable
    // value member and its pairs vector is never reallocated after construction.
    for (const auto& pc : config_.pairs) {
        pair_config_map_[pc.name] = &pc;
    }

    state_->set_status(BotStatus::Initializing);

    spdlog::info("[Engine] All subsystems initialized successfully");
}

Engine::~Engine()
{
    // Ensure clean shutdown if the caller did not invoke shutdown() explicitly.
    if (is_running()) {
        try {
            shutdown();
        } catch (const std::exception& ex) {
            spdlog::error("[Engine] Exception during destructor shutdown: {}", ex.what());
        }
    }
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void Engine::run()
{
    spdlog::info("[Engine] Opening connections and entering main loop");

    // [CRITICAL-1] Connection opening has been moved into poll_loop_coro()
    // where it is co_awaited with ioc_ already running.  The old call here
    // used co_spawn(use_future).get() which deadlocks because ioc_.run()
    // has not been entered yet at this point.
    // ISO/IEC 5055: removed deadlock-prone blocking call from startup path.

    // Start Prometheus metrics exporter.
    // Extract all configured asset IDs so the cardinality guard rejects
    // unexpected label values.  ISO/IEC 5055: bounded resource allocation.
    std::vector<std::string> asset_ids;
    asset_ids.reserve(config_.pairs.size() * 2);
    for (const auto& pair : config_.pairs) {
        asset_ids.push_back(pair.base_asset_id);
        asset_ids.push_back(pair.quote_asset_id);
    }
    // Deduplicate: sort + unique to avoid registering the same ID twice.
    std::sort(asset_ids.begin(), asset_ids.end());
    asset_ids.erase(std::unique(asset_ids.begin(), asset_ids.end()),
                    asset_ids.end());
    metrics_->init(config_.monitoring.prometheus_port, asset_ids);

    // Transition to Running.
    state_->set_status(BotStatus::Running);

    // Start the block-height polling timer.
    start_polling();

    spdlog::info("[Engine] Main loop started (poll_interval={}s)",
                 kPollInterval.count());

    // Block until shutdown() posts a stop or all work completes.
    ioc_.run();

    spdlog::info("[Engine] Main loop exited");
}

void Engine::shutdown()
{
    // Idempotency guard.
    bool expected = false;
    if (!stop_requested_.compare_exchange_strong(expected, true)) {
        return;  // Already shutting down or stopped.
    }

    spdlog::info("[Engine] Shutdown requested");
    state_->set_status(BotStatus::ShuttingDown);

    // Cancel the polling timer so the loop exits.
    boost::system::error_code ec;
    poll_timer_.cancel(ec);

    // [HIGH-2] Cancel all outstanding offers on-chain (secure cancellation).
    //
    // The previous implementation used promise/future.wait_for() which
    // deadlocks if shutdown() is called from the io_context thread (the
    // .wait_for() blocks the thread that needs to pump completions).
    //
    // New approach: post the entire shutdown-continuation (cancel_all ->
    // unlock -> close -> stop) as a co_spawn(detached) coroutine.  This
    // is safe from any calling thread because asio::co_spawn always posts
    // to the io_context.  The atomic shutdown_cancel_done_ flag lets
    // external observers know when cleanup finished (used only for testing;
    // the ioc_.stop() at the end unblocks ioc_.run() in run()).
    //
    // ISO/IEC 5055: no blocking .get()/.wait() calls; fully async teardown.
    // ISO/IEC 27001:2022: all cancellation outcomes are audit-logged.
    asio::co_spawn(ioc_, [this]() -> asio::awaitable<void> {
        // --- Cancel outstanding offers (skip in dry-run mode) ---
        if (!dry_run_) {
            try {
                co_await offer_mgr_->cancel_all();
                spdlog::info("[Engine] All outstanding offers cancelled");
            } catch (const std::exception& ex) {
                spdlog::error("[Engine] cancel_all exception: {}", ex.what());
            }
        }

        // --- Post-cancel cleanup (runs on io_context, no deadlock) ---
        // Unlock all coins held by cancelled offers.
        coin_mgr_->unlock_all();

        // Close RPC/API connections.
        close_connections();

        // Shut down Prometheus exporter.
        metrics_->shutdown();

        state_->set_status(BotStatus::Stopped);
        spdlog::info("[Engine] Shutdown complete");

        // Signal completion for any external observer.
        shutdown_cancel_done_.store(true, std::memory_order_release);

        // Stop the io_context so ioc_.run() in run() returns.
        ioc_.stop();

        co_return;
    }, asio::detached);
}

bool Engine::is_running() const noexcept
{
    return state_->status() == BotStatus::Running;
}

const State& Engine::state() const noexcept
{
    return *state_;
}

const Database& Engine::database() const noexcept
{
    return *db_;
}

BlockHeight Engine::last_processed_block() const noexcept
{
    return last_block_.load(std::memory_order_relaxed);
}

bool Engine::is_dry_run() const noexcept
{
    return dry_run_;
}

// ===========================================================================
// Main loop -- polling and heartbeat dispatch
// ===========================================================================

// [T1-03] start_polling -- launch the native coroutine polling loop.
// Replaces the old timer-callback + co_spawn(use_future).get() pattern
// with a single co_spawn(detached) that loops internally, eliminating
// the deadlock hazard of blocking .get() on the io_context thread.
// ISO/IEC 5055: no blocking calls on the event loop thread.
void Engine::start_polling()
{
    asio::co_spawn(ioc_, poll_loop_coro(), asio::detached);
}

// [T1-03] Native coroutine polling loop.  All async operations are
// co_awaited directly, keeping the io_context free to process other
// completions.  The loop sleeps for kPollInterval between polls.
asio::awaitable<void> Engine::poll_loop_coro()
{
    // [CRITICAL-1] Open all RPC/API connections as the first action inside
    // the coroutine loop.  At this point ioc_.run() is active on the calling
    // thread, so co_await can complete without deadlocking.
    // ISO/IEC 5055: connection setup runs within the live event loop.
    // ISO/IEC 27001:2022: any connection failure propagates as an exception,
    // which terminates the coroutine and ultimately causes ioc_.run() to
    // return, stopping the engine cleanly.
    co_await open_connections();

    asio::steady_timer timer(ioc_);

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // Sleep for the polling interval via co_await (non-blocking).
        timer.expires_after(kPollInterval);
        boost::system::error_code ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        if (ec == boost::asio::error::operation_aborted) {
            break;  // Timer cancelled during shutdown.
        }
        if (ec) {
            spdlog::error("[Engine] Poll timer error: {}", ec.message());
            continue;
        }
        if (stop_requested_.load(std::memory_order_relaxed)) {
            break;
        }

        try {
            // co_await the full node RPC directly -- no use_future/.get().
            std::int64_t height = co_await full_node_->get_block_height();

            // [MEDIUM-4] Guard against negative block heights returned by a
            // malfunctioning or unreachable full node.  The RPC returns
            // int64_t but BlockHeight is uint32_t; a negative value would
            // wrap to a very large block number, causing the engine to
            // process a phantom "new block" and corrupt cycle state.
            // ISO/IEC 5055: explicit narrowing check prevents undefined
            //   behavior from signed-to-unsigned conversion.
            // ISO/IEC 27001:2022: invalid data from external source is
            //   rejected and logged.
            if (height < 0) {
                spdlog::warn("[Engine] Full node returned negative block "
                             "height ({}); skipping this poll cycle", height);
                continue;
            }
            BlockHeight current_block = static_cast<BlockHeight>(height);

            // If we observed a new block, run the full heartbeat cycle.
            if (current_block > last_block_.load(std::memory_order_relaxed)) {
                co_await on_new_block_coro(current_block);
            }
        } catch (const std::exception& ex) {
            spdlog::error("[Engine] Block height poll failed: {}", ex.what());
            // Continue polling; transient failures are expected.
        }
    }

    co_return;
}

// ===========================================================================
// 13-step heartbeat cycle
// ===========================================================================

// [T1-03] on_new_block_coro -- native coroutine heartbeat cycle.
// Steps 2 and 8 are co_awaited (they contain async RPC calls).
// All other steps remain synchronous within the coroutine frame.
asio::awaitable<void> Engine::on_new_block_coro(BlockHeight block_height)
{
    spdlog::info("[Engine] Processing block {}", block_height);

    auto cycle_start = std::chrono::steady_clock::now();

    // Clear per-cycle working state from the previous block.
    cycle_.clear();

    // [T3-08] Reset NHE accumulators for this cycle.
    nhe_net_inventory_change_ = 0.0;
    nhe_total_volume_         = 0.0;

    // Initialize per-pair cycle state for all enabled pairs.
    // [T3-24] market_data_valid defaults to false; Step 1 sets it true
    // on success per pair.
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;
        PairCycleState pcs;
        pcs.pair_name = pair.name;
        cycle_[pair.name] = std::move(pcs);
    }

    // -- Feed block arrival to cadence analyzer ----------------------------
    if (block_cadence_) {
        auto now = std::chrono::system_clock::now();
        block_cadence_->update_block_arrival(block_height, now);
    }

    // Execute all 13 steps in strict sequence.
    // Each step is wrapped in a try/catch so that a failure in one step
    // does not abort the remaining steps.  The engine logs the error and
    // continues with degraded data.

    try { co_await step_update_market_state(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 1 (market state) failed: {}", e.what());
    }

    // [T1-03] Step 2 is a coroutine (co_awaits detect_fills).
    try { co_await step_process_fills(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 2 (fills) failed: {}", e.what());
    }

    try { step_update_analytics(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 3 (analytics) failed: {}", e.what());
    }

    try { step_compute_quotes(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 4 (quotes) failed: {}", e.what());
    }

    try { step_apply_spread_optimizer(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 5 (spread opt) failed: {}", e.what());
    }

    // [T3-10] Run flash-crash check before Step 6 so that the state
    // machine is updated before risk limits and offer gating apply.
    //
    // [MEDIUM-5] Iterate ALL enabled pairs and use the worst-case (largest
    // drop) to drive the state machine.  The previous code had a `break`
    // after the first enabled pair, leaving all other pairs unmonitored.
    // A flash crash in pair #2+ would go undetected, allowing the engine
    // to continue posting offers into a crashing market.
    //
    // New approach: accumulate a per-pair crash/stable signal, then take
    // the most conservative outcome (any pair crashing -> Crash state;
    // recovery requires ALL pairs to be stable).
    //
    // ISO/IEC 5055: exhaustive iteration prevents unmonitored risk paths.
    // ISO/IEC 27001:2022: all pairs contribute to circuit breaker decision.
    try {
        bool any_pair_crashing = false;   // True if any pair triggers crash.
        bool all_pairs_stable_50 = true;  // True if all pairs meet 50-block stability.
        bool all_pairs_stable_100 = true; // True if all pairs meet 100-block stability.
        std::string crash_pair_name;      // Name of first crashing pair (for logging).

        for (const auto& pair : config_.pairs) {
            if (!pair.enabled) continue;
            auto history = market_data_->get_price_history(pair.name);
            if (history.empty()) continue;

            // Convert PriceHistoryEntry to Mojo vector for check_flash_crash.
            std::vector<Mojo> price_vec;
            price_vec.reserve(history.size());
            for (const auto& entry : history) {
                price_vec.push_back(static_cast<Mojo>(
                    std::llround(entry.price * static_cast<double>(kMojosPerXch))));
            }

            // Check crash signal for this pair.
            if (PreTradeCheck::check_flash_crash(price_vec, 0.20)) {
                any_pair_crashing = true;
                if (crash_pair_name.empty()) crash_pair_name = pair.name;
            }

            // Check stability signals for this pair.
            if (!PreTradeCheck::is_stable_after_crash(
                    price_vec, /*required_stable_blocks=*/50, 0.05)) {
                all_pairs_stable_50 = false;
            }
            if (!PreTradeCheck::is_stable_after_crash(
                    price_vec, /*required_stable_blocks=*/100, 0.05)) {
                all_pairs_stable_100 = false;
            }
            // [MEDIUM-5] No break -- continue checking all enabled pairs.
        }

        // Drive the state machine using the aggregated worst-case signals.
        switch (flash_crash_state_) {
            case FlashCrashState::Normal:
                if (any_pair_crashing) {
                    flash_crash_state_ = FlashCrashState::Crash;
                    spdlog::warn("[Engine] Flash crash DETECTED for {} "
                                 "-- transitioning to Crash state",
                                 crash_pair_name);
                    alerts_->send_alert(AlertRule::FlashCrash,
                        "Flash crash detected for " + crash_pair_name +
                        " -- offer posting suspended");
                }
                break;
            case FlashCrashState::Crash:
                if (any_pair_crashing) {
                    // Still crashing -- remain in Crash state.
                } else if (all_pairs_stable_50) {
                    flash_crash_state_ = FlashCrashState::Recovery;
                    spdlog::info("[Engine] Flash crash entering Recovery "
                                 "state (all pairs stable 50 blocks)");
                }
                break;
            case FlashCrashState::Recovery:
                if (any_pair_crashing) {
                    // Re-crash during recovery.
                    flash_crash_state_ = FlashCrashState::Crash;
                    spdlog::warn("[Engine] Re-crash during Recovery for {} "
                                 "-- back to Crash state", crash_pair_name);
                } else if (all_pairs_stable_100) {
                    flash_crash_state_ = FlashCrashState::Normal;
                    spdlog::info("[Engine] Flash crash Recovery complete "
                                 "(all pairs stable 100 blocks) "
                                 "-- Normal state resumed");
                    alerts_->send_alert(AlertRule::FlashCrash,
                        "Market stability restored across all pairs "
                        "-- offer posting resumed");
                }
                break;
        }
    } catch (const std::exception& e) {
        spdlog::error("[Engine] Flash crash check failed: {}", e.what());
    }

    try { step_apply_risk_limits(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 6 (risk limits) failed: {}", e.what());
    }

    try { step_generate_ladder(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 7 (ladder) failed: {}", e.what());
    }

    // [T1-03] Step 8 is a coroutine (co_awaits cancel_stale + post_quotes).
    // [T3-10] Gate Step 8 during Crash/Recovery states.
    if (flash_crash_state_ == FlashCrashState::Normal) {
        try { co_await step_manage_offers(block_height); }
        catch (const std::exception& e) {
            spdlog::error("[Engine] Step 8 (offers) failed: {}", e.what());
        }
    } else {
        spdlog::warn("[Engine] Step 8 GATED: flash_crash_state={} "
                     "-- no new offers posted",
                     to_string(flash_crash_state_));
    }

    try { step_check_arbitrage(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 9 (arbitrage) failed: {}", e.what());
    }

    try { step_run_hedging(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 10 (hedging) failed: {}", e.what());
    }

    try { step_update_pnl(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 11 (PnL) failed: {}", e.what());
    }

    try { step_export_metrics(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 12 (metrics) failed: {}", e.what());
    }

    try { step_check_alerts(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 13 (alerts) failed: {}", e.what());
    }

    // Record this block as processed.
    last_block_.store(block_height, std::memory_order_relaxed);

    auto elapsed = std::chrono::steady_clock::now() - cycle_start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    spdlog::info("[Engine] Block {} processed in {} ms", block_height, ms.count());

    co_return;
}

// ===========================================================================
// Step implementations
// ===========================================================================

// [T1-02] Step 1: Update market state (prices from DEX + CEX).
// Now a coroutine: co_awaits dexie async methods which dispatch CURL
// transfers to a thread pool (never blocking the io_context).
asio::awaitable<void> Engine::step_update_market_state(BlockHeight block_height)
{
    // Ingest the current block height into the market data feed.
    market_data_->ingest_block_height(block_height);

    // For each enabled pair, fetch the latest dexie ticker data.
    // [T3-24] Track per-pair success for dependency-aware gating.
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        // [T3-24] Assume invalid until successful data ingestion.
        bool pair_data_ok = false;

        auto ticker = co_await dexie_->get_ticker(pair.name);
        if (ticker) {
            market_data_->ingest_dexie(
                pair.name,
                ticker->price_buy,
                ticker->price_sell,
                ticker->price_last,
                ticker->volume_xch_daily);

            // [T3-24] Mark pair data as valid if ticker returned prices.
            if (ticker->price_last > 0.0) {
                pair_data_ok = true;
            }

            // Feed OFI (Order Flow Imbalance) with best bid/ask from the
            // dexie ticker snapshot.  The ticker provides best-level prices;
            // daily volume serves as a top-of-book depth proxy.
            // ISO/IEC 5055: guard against zero/negative prices.
            if (ticker->price_buy > 0.0 && ticker->price_sell > 0.0) {
                market_data_->ingest_book_snapshot_for_ofi(
                    pair.name,
                    ticker->price_buy,
                    ticker->volume_xch_daily,   // bid-side depth proxy
                    ticker->price_sell,
                    ticker->volume_xch_daily);  // ask-side depth proxy
            }
        }

        // Feed competing-offer tracker with the full active order book
        // from dexie.  Own offer IDs are collected from the shared State
        // so the competitor detector can filter them out.
        // ISO/IEC 27001:2022: no secret data exposed in offers.
        try {
            auto offers_page = co_await dexie_->get_offers(
                pair.name,
                /*offered=*/   {},
                /*requested=*/ {},
                /*page=*/      1,
                /*page_size=*/ 100,
                /*sort=*/      "price_asc",
                /*compact=*/   true,
                /*status=*/    0);  // status 0 = active offers

            // Build CompetingOffer vector from dexie OfferRecord data.
            std::vector<CompetingOffer> comp_offers;
            comp_offers.reserve(offers_page.offers.size());
            for (const auto& orec : offers_page.offers) {
                CompetingOffer co;
                co.offer_id         = orec.id;
                co.pair_name        = pair.name;
                // Determine side from the offered/requested arrays:
                // if offered contains the pair's base asset, this is an ask.
                co.side             = (!orec.offered.empty() &&
                                       orec.offered[0].id == pair.base_asset_id)
                                      ? Side::Ask : Side::Bid;
                // ISO/IEC 5055: round instead of truncate for price conversion.
                co.price            = static_cast<Mojo>(std::llround(
                    orec.price * static_cast<double>(kMojosPerXch)));
                co.size             = 0;
                if (!orec.offered.empty()) {
                    // ISO/IEC 5055: round instead of truncate for size conversion.
                    co.size = static_cast<Mojo>(std::llround(
                        orec.offered[0].amount *
                        static_cast<double>(kMojosPerXch)));
                }
                co.first_seen_block = block_height;
                co.last_seen_block  = block_height;
                co.last_seen_ts     = std::chrono::system_clock::now();
                comp_offers.push_back(co);
            }

            // Collect own offer IDs from shared State for exclusion.
            std::unordered_set<std::string> own_ids;
            auto pending = state_->get_all_offers();
            for (const auto& po : pending) {
                own_ids.insert(po.offer_id);
            }

            market_data_->ingest_competing_offers(
                pair.name, comp_offers, own_ids);
        } catch (const std::exception& ex) {
            // Transient dexie errors should not abort the cycle.
            // ISO/IEC 5055: checked error handling with logging.
            spdlog::warn("[Engine] Step 1: competing-offer fetch failed "
                         "for {}: {}", pair.name, ex.what());
        }

        // CEX reference ingestion would go here (Phase 2: OKX/Gate.io).
        // market_data_->ingest_cex_reference(pair.name, cex_mid);

        // [T3-24] Set the market_data_valid flag for this pair.
        // Steps 4-8 are gated on this flag; if Step 1 fails for a pair,
        // those steps will skip it rather than acting on stale data.
        // ISO/IEC 5055: explicit validity tracking per pair.
        auto cycle_it = cycle_.find(pair.name);
        if (cycle_it != cycle_.end()) {
            cycle_it->second.market_data_valid = pair_data_ok;
            if (!pair_data_ok) {
                spdlog::warn("[Engine] Step 1: {} market data invalid -- "
                             "steps 4-8 gated for this pair", pair.name);
            }
        }
    }

    // Build a list of enabled pair names for the refresh call.
    std::vector<std::string> enabled;
    enabled.reserve(config_.pairs.size());
    for (const auto& pair : config_.pairs) {
        if (pair.enabled) {
            enabled.push_back(pair.name);
        }
    }

    // Compute aggregated mid-prices, spreads, arbitrage signals.
    market_data_->refresh(enabled);

    spdlog::debug("[Engine] Step 1 complete: market state updated for {} pairs",
                  enabled.size());
    co_return;
}

// [T1-03] Step 2: Process any fills from this block (coroutine).
// co_awaits detect_fills() directly instead of co_spawn(use_future).get().
// [T2-17] Checks record_sell() return value for no-loss rejection.
// [T3-08] Feeds NHE accumulators with fill data for Step 10.
asio::awaitable<void> Engine::step_process_fills(BlockHeight block_height)
{
    // [T1-03] co_await the fill-detection coroutine directly.
    auto fills = co_await offer_mgr_->detect_fills();

    for (const auto& fill : fills) {
        // Persist the fill to the audit trail.
        DbTradeRecord tr;
        // Convert the fill's wall-clock timestamp to ISO-8601 for the DB record.
        // ISO/IEC 5055: use the actual detection time, not DB-generated time.
        tr.timestamp       = PnLTracker::timestamp_to_iso(fill.timestamp);
        tr.trade_id        = fill.offer_id;
        tr.pair_name       = fill.pair_name;
        tr.side            = (fill.side == Side::Bid) ? "bid" : "ask";
        tr.price_mojos     = fill.price;
        tr.size_mojos      = fill.size;
        tr.fee_mojos       = 0;  // Fee extraction from wallet response (Phase 2).
        tr.block_height    = fill.block_height;

        // Look up pair config for this fill's pair (O(1) map lookup).
        // ISO/IEC 5055: validated lookup before use.
        const PairConfig* fill_pair_cfg = find_pair_config(fill.pair_name);

        // [H3] Guard against fills for unconfigured pairs.
        // ISO/IEC 5055: null-pointer dereference prevention.
        if (!fill_pair_cfg) {
            spdlog::error("[Engine] Fill for unconfigured pair '{}' -- skipping",
                          fill.pair_name);
            continue;
        }

        // Retrieve cost basis from the inventory tracker for PnL calculation.
        // For asks (sells), use the pair's base asset; for bids (buys),
        // use the pair's quote asset.
        auto asset_rec = inventory_->get_record(
            (fill.side == Side::Ask)
                ? AssetId{fill_pair_cfg->base_asset_id}
                : AssetId{fill_pair_cfg->quote_asset_id});
        tr.cost_basis_mojos   = asset_rec.weighted_avg_cost_basis;
        // [C2] PnL unit normalization -- prevent mojos-squared overflow.
        // Price is in "mojos per XCH" (i.e. mojos_quote per kMojosPerXch
        // mojos_base).  To obtain PnL in quote mojos:
        //   pnl = (sell_price - cost_basis) * size / kMojosPerXch
        // We use double intermediates to avoid int64 overflow on the
        // multiply (two 1e12-scale values can exceed 2^63).
        // ISO/IEC 5055: explicit unit normalization for financial calc.
        tr.realized_pnl_mojos = (fill.side == Side::Ask)
            ? static_cast<Mojo>(std::llround(
                  static_cast<double>(fill.price - asset_rec.weighted_avg_cost_basis)
                  * static_cast<double>(fill.size)
                  / static_cast<double>(kMojosPerXch)))
            : 0;

        db_->insert_trade(tr);

        // Record the offer as filled in the offer log.
        db_->update_offer_status(fill.offer_id, "filled", fill.block_height);

        // Update the inventory tracker using the pair's actual base asset.
        auto now = std::chrono::system_clock::now();
        // [H3] fill_pair_cfg is guaranteed non-null (guarded above).
        const std::string& fill_base = fill_pair_cfg->base_asset_id;
        if (fill.side == Side::Bid) {
            inventory_->record_buy(fill_base, fill.size, fill.price,
                                   fill.block_height, now);
        } else {
            // [T2-17] Check record_sell() return value.  record_sell()
            // returns false when the no-loss constraint rejects the sell
            // (sell_price < cost_basis).  This should not happen for
            // confirmed on-chain fills, but if it does, it indicates a
            // state inconsistency between the offer manager's pricing and
            // the inventory tracker's cost basis.
            // ISO/IEC 5055: checked return value on every code path.
            bool sell_ok = inventory_->record_sell(
                fill_base, fill.size, fill.price,
                fill.block_height, now);
            if (!sell_ok) {
                spdlog::error("[Engine] Step 2: record_sell() REJECTED fill "
                              "for {} {} @ {} mojos (block {}) -- "
                              "no-loss constraint violation or state "
                              "inconsistency.  Fill was confirmed on-chain "
                              "but inventory tracker refused it.",
                              fill.pair_name, fill.size, fill.price,
                              fill.block_height);
                // Alert on the inconsistency so the operator can investigate.
                alerts_->send_alert(AlertRule::ExposureBreach,
                    "record_sell() rejected confirmed fill for " +
                    fill.pair_name + " at block " +
                    std::to_string(fill.block_height) +
                    " -- state inconsistency");
            }
        }

        // [T3-08] Accumulate NHE (Natural Hedge Efficiency) data from fills.
        // Buys add positive inventory change; sells subtract.
        // Total volume accumulates the absolute fill size.
        // These accumulators are consumed by step_run_hedging (Step 10).
        // ISO/IEC 5055: double conversion to prevent integer overflow.
        double fill_size_d = static_cast<double>(fill.size);
        if (fill.side == Side::Bid) {
            nhe_net_inventory_change_ += fill_size_d;
        } else {
            nhe_net_inventory_change_ -= fill_size_d;
        }
        nhe_total_volume_ += fill_size_d;

        // Feed the PnL tracker.
        pnl_->record_fill(fill, tr.fee_mojos, tr.cost_basis_mojos);

        // Feed the whale detector with each fill for attribution/calibration.
        // T3-35: is_own_fill=true because these are the bot's own confirmed
        // fills.  Own fills are recorded for calibration only -- they must NOT
        // contribute to whale detection, VPIN, or OFI toxicity signals.
        // Treating own fills as market toxicity creates a self-reinforcing
        // feedback loop: own fill -> toxicity up -> spread widens -> spiral.
        // ISO/IEC 25000: connects the fully-implemented whale subsystem
        // to the engine heartbeat with proper self-fill separation.
        constexpr bool kIsOwnFill = true;
        market_data_->ingest_trade(
            fill.pair_name, fill.side, fill.size, fill.block_height,
            kIsOwnFill);

        // Feed VPIN (Volume-Synchronized Probability of Informed Trading)
        // with each fill's volume.  VPIN accumulates volume bars to detect
        // flow toxicity.
        // T3-35: is_own_fill=true -- own fills excluded from VPIN to prevent
        // self-generated order flow from inflating toxicity metrics.
        // ISO/IEC 5055: convert mojos to base-asset units for VPIN.
        const double fill_volume =
            static_cast<double>(fill.size) / static_cast<double>(kMojosPerXch);
        market_data_->ingest_trade_for_vpin(
            fill.pair_name, fill.side, fill_volume, kIsOwnFill);

        spdlog::info("[Engine] Fill: {} {} {} @ {} (block {})",
                     fill.pair_name,
                     (fill.side == Side::Bid) ? "BUY" : "SELL",
                     fill.size, fill.price, fill.block_height);
    }

    // -- Feed inventory ratio to drift analyzer ----------------------------
    if (drift_analyzer_) {
        for (const auto& pair : config_.pairs) {
            if (!pair.enabled) continue;
            double mid = market_data_->get_mid_price(pair.name);
            if (mid <= 0.0) continue;
            Mojo mid_mojos = static_cast<Mojo>(std::llround(mid * static_cast<double>(kMojosPerXch)));
            double inv_ratio = inventory_->inventory_ratio(
                AssetId{pair.base_asset_id},
                AssetId{pair.quote_asset_id},
                mid_mojos);
            drift_analyzer_->record_observation(inv_ratio, block_height);
        }
    }

    spdlog::debug("[Engine] Step 2 complete: {} fills processed", fills.size());
    co_return;
}

// Step 3: Update volatility, PIN, regime estimates.
void Engine::step_update_analytics(BlockHeight block_height)
{
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        double mid = market_data_->get_mid_price(pair.name);
        if (mid <= 0.0) continue;  // No valid price data.

        // Update the Yang-Zhang volatility estimator with the latest mid.
        auto vol_it = vol_estimators_.find(pair.name);
        if (vol_it != vol_estimators_.end()) {
            vol_it->second->update(mid);
        }

        // [T1-11] Update the per-pair strategy's internal price history.
        // Each pair has its own strategy instance to prevent state bleed.
        auto strat_it = strategies_.find(pair.name);
        if (strat_it != strategies_.end()) {
            strat_it->second->update_price(mid, block_height);
        }

        // PIN estimator updates happen after fills (in step 2, once we
        // have subsequent price data).  Here we just record the latest
        // regime for logging.
        // [H5] Use the vol_it iterator from find() above instead of
        // operator[] which inserts a nullptr on missing keys.
        // ISO/IEC 5055: prevent default-insertion in associative container.
        if (vol_it == vol_estimators_.end()) continue;
        auto& vol_est = *vol_it->second;
        auto regime = vol_est.get_regime();

        spdlog::debug("[Engine] Step 3: {} sigma_block={:.6f} regime={}",
                      pair.name, vol_est.get_sigma_block(),
                      to_string(regime.regime));
    }
}

// Step 4: Compute optimal quotes (A-S / GLFT).
// [T1-11] Uses per-pair strategy instances from strategies_ map.
// [T3-24] Gates on market_data_valid flag from Step 1.
void Engine::step_compute_quotes(BlockHeight block_height)
{
    for (auto& [pair_name, pcs] : cycle_) {
        // [T3-24] Dependency-aware gating: skip pairs where Step 1 failed.
        // Quoting with invalid/stale market data risks adverse selection.
        // ISO/IEC 5055: prevents acting on invalid upstream data.
        if (!pcs.market_data_valid) {
            spdlog::warn("[Engine] Step 4: {} market data invalid (Step 1 "
                         "failed) -- skipping quote generation", pair_name);
            pcs.quote_valid = false;
            continue;
        }

        // -- Staleness gate: suppress quoting when market data is stale ------
        // If the market data feed has not received a fresh update within the
        // staleness threshold (5 minutes), skip quote generation for this
        // pair.  Quoting on stale prices risks adverse selection against us.
        // ISO/IEC 5055: defensive gate prevents acting on unreliable data.
        if (market_data_->is_stale(pair_name)) {
            spdlog::warn("[Engine] Step 4: {} market data is stale -- "
                         "skipping quote generation", pair_name);
            pcs.quote_valid = false;
            continue;
        }

        double mid = market_data_->get_mid_price(pair_name);
        // [M1] Invalidate quote when mid-price is non-positive, so
        // downstream steps do not act on a stale quote_valid flag.
        // ISO/IEC 5055: defensive guard on financial input data.
        if (mid <= 0.0) {
            pcs.quote_valid = false;
            continue;
        }

        // Resolve pair config for this pair (O(1) map lookup).
        const PairConfig* pair_cfg = find_pair_config(pair_name);
        if (!pair_cfg) continue;

        // [T1-11] Look up the per-pair strategy instance.
        // ISO/IEC 5055: validated lookup before use; skip if missing.
        auto strat_it = strategies_.find(pair_name);
        if (strat_it == strategies_.end() || !strat_it->second) {
            spdlog::error("[Engine] Step 4: no strategy instance for {} "
                          "-- skipping", pair_name);
            pcs.quote_valid = false;
            continue;
        }
        auto& strategy = *strat_it->second;

        // Look up the volatility estimate for this pair.
        double sigma = 0.0;
        auto vol_it = vol_estimators_.find(pair_name);
        if (vol_it != vol_estimators_.end() && vol_it->second->is_ready()) {
            sigma = vol_it->second->get_sigma_annual();
        }

        // Compute inventory (signed net position in the pair's base asset).
        double q = static_cast<double>(
            inventory_->net_inventory(AssetId{pair_cfg->base_asset_id}));

        // Set cost basis on the per-pair strategy for the never-sell-at-loss
        // constraint.
        auto rec = inventory_->get_record(AssetId{pair_cfg->base_asset_id});
        double cost_basis = static_cast<double>(rec.weighted_avg_cost_basis);
        strategy.set_cost_basis(
            cost_basis, config_.strategy.min_profit_margin_bps);

        // Invoke the per-pair strategy to produce raw quotes.
        pcs.raw_quote = strategy.compute_quotes(mid, sigma, q, block_height);
        pcs.quote_valid = true;  // Mark as valid for steps 5-8.

        spdlog::debug("[Engine] Step 4: {} bid={:.6f} ask={:.6f} spread={:.1f}bps",
                      pair_name, pcs.raw_quote.bid_price,
                      pcs.raw_quote.ask_price, pcs.raw_quote.spread_bps);
    }
}

// Step 5: Apply spread optimizer adjustments.
void Engine::step_apply_spread_optimizer(BlockHeight block_height)
{
    // Compute current hour and day-of-week for time-of-day adjustments.
    auto now_tp = std::chrono::system_clock::now();
    auto now_tt = std::chrono::system_clock::to_time_t(now_tp);
    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &now_tt);
#else
    gmtime_r(&now_tt, &utc_tm);
#endif
    int hour_utc    = utc_tm.tm_hour;
    int day_of_week = (utc_tm.tm_wday == 0) ? 7 : utc_tm.tm_wday; // ISO: Mon=1..Sun=7

    for (auto& [pair_name, pcs] : cycle_) {
        // [H1] Skip pairs whose quote was invalidated in step 4.
        // ISO/IEC 5055: guard prevents operating on invalid state.
        if (!pcs.quote_valid) continue;

        double mid = market_data_->get_mid_price(pair_name);
        if (mid <= 0.0) continue;

        // Resolve pair config once per iteration (O(1) map lookup).
        const PairConfig* pair_cfg = find_pair_config(pair_name);
        if (!pair_cfg) continue;

        double sigma = 0.0;
        auto vol_it = vol_estimators_.find(pair_name);
        if (vol_it != vol_estimators_.end() && vol_it->second->is_ready()) {
            sigma = vol_it->second->get_sigma_annual();
        }

        // Inventory for spread sizing -- use the pair's actual base asset.
        double q = static_cast<double>(
            inventory_->net_inventory(AssetId{pair_cfg->base_asset_id}));
        double q_max = config_.strategy.q_max;

        // PIN estimate.
        double pin = 0.15;  // default
        auto pin_it = pin_estimators_.find(pair_name);
        if (pin_it != pin_estimators_.end()) {
            pin = pin_it->second->get_pin();
        }

        // Fetch actual best-competing spread from the competitor tracker
        // instead of the previous hardcoded 0.0.  Returns 0.0 if no
        // competitors are tracked (feature disabled or no offers).
        // ISO/IEC 25000: connects the competitor-detection subsystem output.
        double best_competing_bps =
            market_data_->get_best_competing_spread_bps(pair_name);

        // Compute the four-component spread using live competitor data.
        pcs.spread_result = spread_opt_->compute_spread(
            mid, sigma, q, q_max, pin,
            Venue::Dexie, best_competing_bps,
            hour_utc, day_of_week);

        // ---------------------------------------------------------------
        // Apply whale / VPIN / OFI post-multipliers to the base spread.
        //
        // These signals widen the spread when informed-flow risk is elevated:
        //   whale_mult : direct spread multiplier from whale detector (>=1.0)
        //   vpin_mult  : up to 50% widening at max toxicity (VPIN=1.0)
        //   ofi_mult   : up to 30% widening at max order-flow imbalance
        //
        // ISO/IEC 27001:2022: no secret data; all signals are market-derived.
        // ISO/IEC 5055: multipliers are clamped via their source methods.
        // ---------------------------------------------------------------
        double whale_mult = market_data_->get_whale_spread_multiplier(pair_name);
        double vpin       = market_data_->get_vpin(pair_name);
        double vpin_mult  = 1.0 + vpin * 0.5;   // linear scale [1.0, 1.5]
        double ofi        = std::abs(market_data_->get_normalized_ofi(pair_name));
        double ofi_mult   = 1.0 + ofi * 0.3;    // linear scale [1.0, 1.3]

        pcs.spread_result.total_spread_bps *= whale_mult * vpin_mult * ofi_mult;
        pcs.spread_result.half_spread =
            pcs.spread_result.total_spread_bps / 2.0;

        spdlog::debug("[Engine] Step 5: {} spread={:.1f}bps (adverse={:.1f} inv={:.1f} "
                      "cost={:.1f} comp={:.1f} mult={:.2f} "
                      "whale={:.2f} vpin={:.3f} ofi={:.3f})",
                      pair_name,
                      pcs.spread_result.total_spread_bps,
                      pcs.spread_result.s_adverse,
                      pcs.spread_result.s_inventory,
                      pcs.spread_result.s_cost,
                      pcs.spread_result.s_competition,
                      pcs.spread_result.regime_multiplier,
                      whale_mult, vpin, ofi);

        // -- Apply CHIA structural edge multiplier -------------------------
        if (chia_edge_) {
            double edge_mult = chia_edge_->composite_edge_multiplier();
            // Edge multiplier tightens spreads (< 1.0) when structural edges are strong
            pcs.spread_result.total_spread_bps *= edge_mult;
            pcs.spread_result.half_spread = pcs.spread_result.total_spread_bps / 2.0;
            spdlog::debug("[step5] {} chia_edge_mult={:.3f}", pair_name, edge_mult);
        }

        // -- Apply order book tactic adjustment ----------------------------
        if (order_book_tactician_) {
            BookState book_state;
            book_state.mid_price = market_data_->get_mid_price(pair_name);

            // Populate best_bid / best_ask from the market data feed's
            // dexie order book.  The spread in bps divided by 20000 gives
            // the half-spread as a fraction of mid, producing synthetic
            // best-bid/ask that reflect the actual top-of-book levels.
            // ISO/IEC 5055: verified non-zero mid before division.
            {
                const double feed_spread_bps = market_data_->get_spread_bps(pair_name);
                const double half_frac = feed_spread_bps / 20000.0;
                book_state.best_bid = book_state.mid_price * (1.0 - half_frac);
                book_state.best_ask = book_state.mid_price * (1.0 + half_frac);
            }
            book_state.our_spread_bps = pcs.spread_result.total_spread_bps;
            book_state.best_competing_bps = market_data_->get_best_competing_spread_bps(pair_name);
            auto comp_metrics = market_data_->get_competitor_metrics(pair_name);
            book_state.bid_depth = comp_metrics ? comp_metrics->competing_depth_bids : 0;
            book_state.ask_depth = comp_metrics ? comp_metrics->competing_depth_asks : 0;
            book_state.vpin = market_data_->get_vpin(pair_name);
            book_state.normalized_ofi = market_data_->get_normalized_ofi(pair_name);

            // Use the 3-arg inventory_ratio with this pair's asset IDs
            // and current mid-price as the ratio reference.
            // pair_cfg was resolved at the top of this loop iteration.
            Mojo mid_mojos = static_cast<Mojo>(std::llround(mid * static_cast<double>(kMojosPerXch)));
            book_state.inventory_ratio = pair_cfg
                ? std::abs(inventory_->inventory_ratio(
                      AssetId{pair_cfg->base_asset_id},
                      AssetId{pair_cfg->quote_asset_id},
                      mid_mojos))
                : 0.5;

            book_state.fill_rate_24h = 0.30;  // TODO: compute from fill history
            book_state.whale_active = market_data_->is_whale_active(pair_name);
            // [T1-11] Use per-pair strategy for regime classification.
            {
                auto s5_strat_it = strategies_.find(pair_name);
                book_state.regime = (s5_strat_it != strategies_.end()
                                     && s5_strat_it->second)
                    ? s5_strat_it->second->current_regime().regime
                    : MarketRegime::Random;
            }

            auto rec = order_book_tactician_->recommend(book_state);
            auto adj = order_book_tactician_->apply(rec, pcs.spread_result.total_spread_bps);

            pcs.spread_result.total_spread_bps = adj.spread_bps;
            pcs.spread_result.half_spread = adj.spread_bps / 2.0;

            spdlog::debug("[step5] {} tactic={} spread_adj={:.1f}bps bid_size_f={:.2f} ask_size_f={:.2f}",
                pair_name, rec.reason, adj.spread_bps, adj.bid_size_factor, adj.ask_size_factor);
        }

        // ---------------------------------------------------------------
        // Global spread cap: prevent compounding multipliers from causing
        // effective market withdrawal.
        //
        // The multiplicative chain (regime * whale * VPIN * OFI * tactic *
        // chia_edge) can compound to ~14x base spread in worst case.
        // Cap half_spread to max_half_spread_bps (default 250 bps =
        // 500 bps round-trip = 5%).
        //
        // ISO/IEC 5055: bounded output prevents unbounded spread growth.
        // ISO/IEC 25000: configurable via strategy.max_half_spread_bps.
        // ---------------------------------------------------------------
        const double max_hs = config_.strategy.max_half_spread_bps;
        if (pcs.spread_result.half_spread > max_hs) {
            spdlog::warn("[step5] {} spread capped: {:.1f}bps -> {:.1f}bps "
                         "(half_spread exceeded max {}bps)",
                         pair_name,
                         pcs.spread_result.total_spread_bps,
                         max_hs * 2.0,
                         max_hs);
            pcs.spread_result.half_spread      = max_hs;
            pcs.spread_result.total_spread_bps = max_hs * 2.0;
        }
    }
}

// Step 6: Apply risk limits (inventory, Kelly, no-loss).
void Engine::step_apply_risk_limits(BlockHeight block_height)
{
    for (auto& [pair_name, pcs] : cycle_) {
        // [H2] Skip pairs whose quote was invalidated in prior steps.
        // ISO/IEC 5055: guard prevents operating on invalid state.
        if (!pcs.quote_valid) continue;

        // Find the pair config for base/quote asset IDs (O(1) map lookup).
        const PairConfig* pair_cfg = find_pair_config(pair_name);
        if (!pair_cfg) continue;

        // Build a Quote from the strategy output.  The strategy's raw
        // bid_price and ask_price already include the A-S reservation
        // price skew (r = S - q * gamma * sigma^2 * tau), which shifts
        // the quote center away from the market mid to incentivise
        // inventory rebalancing.  Using the raw market mid here would
        // discard that skew and produce symmetric quotes regardless of
        // inventory, defeating the A-S / GLFT models.
        //
        // We use the strategy's reservation mid (midpoint of the raw
        // quote) as the center and apply the spread optimizer's
        // half-spread around it.  The asymmetric multipliers from whale
        // detection further decompose the spread per-side.
        // ISO/IEC 25000: preserves the mathematical intent of the strategy.
        double mid = market_data_->get_mid_price(pair_name);
        double reservation_mid = (pcs.raw_quote.bid_price
                                + pcs.raw_quote.ask_price) / 2.0;
        double half_spread = pcs.spread_result.half_spread / 10000.0 * mid;

        // Apply per-side asymmetric widening from whale/OFI analysis.
        auto asym = market_data_->get_asymmetric_spread_multipliers(pair_name);
        double bid_half = half_spread * asym.bid_multiplier;
        double ask_half = half_spread * asym.ask_multiplier;

        // ISO/IEC 5055: round instead of truncate for price/size conversions.
        Quote quote;
        quote.bid_price  = static_cast<Mojo>(std::llround(
            (reservation_mid - bid_half) * static_cast<double>(kMojosPerXch)));
        quote.ask_price  = static_cast<Mojo>(std::llround(
            (reservation_mid + ask_half) * static_cast<double>(kMojosPerXch)));
        quote.bid_size   = static_cast<Mojo>(std::llround(
            pcs.raw_quote.bid_size * static_cast<double>(kMojosPerXch)));
        quote.ask_size   = static_cast<Mojo>(std::llround(
            pcs.raw_quote.ask_size * static_cast<double>(kMojosPerXch)));
        quote.spread_bps = pcs.spread_result.total_spread_bps;

        // Enforce never-sell-at-loss on the ask price.
        // Use the pair's base asset (not hardcoded "xch") for cost-basis lookup.
        auto rec = inventory_->get_record(AssetId{pair_cfg->base_asset_id});
        quote = pre_trade_->enforce_no_loss(
            quote, rec.weighted_avg_cost_basis, /*enable=*/true);

        // -- Consult strategic loss manager for rebalancing decisions ------
        // The loss manager evaluates whether taking a deliberate loss to
        // rebalance inventory is rational given current market conditions.
        // It is disabled by default (enabled=false) so this block is a
        // no-op unless the operator explicitly enables strategic loss
        // analysis.  The never-sell-at-loss constraint is always the
        // default; this pathway may relax it only when all five EV
        // scenarios indicate that rebalancing yields positive expected
        // value.
        // ISO/IEC 27001:2022: advisory-only; no secrets; audit-logged.
        if (loss_manager_ && loss_manager_->config().enabled) {
            Mojo mid_mojos = static_cast<Mojo>(std::llround(mid * static_cast<double>(kMojosPerXch)));
            double inv_ratio = std::abs(inventory_->inventory_ratio(
                AssetId{pair_cfg->base_asset_id},
                AssetId{pair_cfg->quote_asset_id},
                mid_mojos));

            // Only consult the loss manager when inventory is significantly
            // imbalanced (> 60% one-sided).  Below that threshold the
            // normal skew mechanism suffices.
            if (inv_ratio > 0.60) {
                // Build the price map for the loss manager.
                std::unordered_map<AssetId, Mojo> price_map;
                price_map[pair_cfg->base_asset_id] = mid_mojos;

                // Build market parameters from live analytics.
                MarketParams mkt_params{};
                {
                    auto vol_it = vol_estimators_.find(pair_name);
                    mkt_params.sigma = (vol_it != vol_estimators_.end()
                                        && vol_it->second->is_ready())
                        ? vol_it->second->get_sigma_block()
                        : 0.0;
                }
                mkt_params.fill_rate_per_block = 0.03;  // conservative estimate
                // [HIGH-3] MarketParams::spread_bps is documented as the
                // half-spread (one side).  The loss manager formula
                // internally doubles it to compute the full round-trip
                // capture.  The previous code passed total_spread_bps
                // (the full spread), causing the EV revenue estimate to
                // be 2x the correct value -- overstating profitability
                // of holding and understating rebalance urgency.
                // Fix: divide by 2 to supply the half-spread as expected.
                // ISO/IEC 5055: correct units at module boundary.
                // ISO/IEC 27001:2022: accurate risk calculation prevents
                //   masked inventory buildup.
                mkt_params.spread_bps = pcs.spread_result.total_spread_bps / 2.0;
                mkt_params.vpin = market_data_->get_vpin(pair_name);
                // [T1-11] Use per-pair strategy for variance ratio.
                {
                    auto s6_strat_it = strategies_.find(pair_name);
                    mkt_params.variance_ratio =
                        (s6_strat_it != strategies_.end() && s6_strat_it->second)
                        ? s6_strat_it->second->current_regime().variance_ratio
                        : 1.0;
                }
                mkt_params.current_block = block_height;

                auto decision = loss_manager_->should_rebalance_at_loss(
                    AssetId{pair_cfg->base_asset_id},
                    mid_mojos,
                    loss_manager_->config().target_inventory_ratio,
                    *inventory_,
                    price_map,
                    mkt_params);

                if (decision.should_take_loss) {
                    // The loss manager recommends relaxing the no-loss
                    // constraint for this cycle to rebalance inventory.
                    // Re-apply enforce_no_loss with the constraint disabled
                    // so the ask price is not floored above cost basis.
                    spdlog::info("[step6] {} Loss manager recommends "
                        "rebalancing: rationale='{}', loss={:.1f}bps, "
                        "breakeven={:.0f} blocks",
                        pair_name, decision.rationale,
                        decision.loss_bps, decision.blocks_to_breakeven);
                    quote = pre_trade_->enforce_no_loss(
                        quote, rec.weighted_avg_cost_basis, /*enable=*/false);
                } else {
                    spdlog::debug("[step6] {} Loss manager: hold (inv_ratio="
                        "{:.2f}, rationale='{}')",
                        pair_name, inv_ratio, decision.rationale);
                }
            }
        }

        // Apply inventory limits, Kelly sizing, CAT cap.
        auto checked = pre_trade_->apply_limits(
            quote, pair_name,
            AssetId{pair_cfg->base_asset_id},
            AssetId{pair_cfg->quote_asset_id},
            *state_);

        if (checked) {
            pcs.risk_quote  = *checked;
            pcs.quote_valid = true;
        } else {
            pcs.quote_valid = false;
            spdlog::warn("[Engine] Step 6: {} -- both sides blocked by risk limits",
                         pair_name);
        }
    }
}

// Step 7: Generate multi-tier offer ladder.
void Engine::step_generate_ladder(BlockHeight block_height)
{
    for (auto& [pair_name, pcs] : cycle_) {
        if (!pcs.quote_valid) continue;

        auto liq_it = liquidity_engines_.find(pair_name);
        if (liq_it == liquidity_engines_.end()) continue;

        auto& liq = *liq_it->second;

        // Mid-price in mojos.
        Mojo mid_mojos = (pcs.risk_quote.bid_price + pcs.risk_quote.ask_price) / 2;

        // Volatility.
        double sigma = 0.0;
        auto vol_it = vol_estimators_.find(pair_name);
        if (vol_it != vol_estimators_.end() && vol_it->second->is_ready()) {
            sigma = vol_it->second->get_sigma_annual();
        }

        // Inventory ratio for skew (O(1) pair config lookup).
        double inv_ratio = 0.5;
        const PairConfig* pair_cfg = find_pair_config(pair_name);
        if (pair_cfg) {
            inv_ratio = inventory_->inventory_ratio(
                AssetId{pair_cfg->base_asset_id},
                AssetId{pair_cfg->quote_asset_id},
                mid_mojos);
        }

        // Available capital for bids (quote asset) and asks (base asset).
        Mojo avail_capital   = pcs.risk_quote.bid_size;
        Mojo avail_inventory = pcs.risk_quote.ask_size;

        // Generate the tier ladder.
        pcs.ladder = liq.compute_ladder(
            mid_mojos, sigma, inv_ratio,
            avail_capital, avail_inventory);

        spdlog::debug("[Engine] Step 7: {} generated {} tier quotes",
                      pair_name, pcs.ladder.size());
    }
}

// [T1-03] Step 8: Cancel stale offers, post new ones (coroutine).
// co_awaits cancel_stale() and post_quotes() directly -- no use_future.
// [T2-09] Persists actual wallet-assigned offer IDs to the database
// by reading them from shared State after post_quotes returns.
// [T3-24] Gates on market_data_valid to prevent posting with stale data.
asio::awaitable<void> Engine::step_manage_offers(BlockHeight block_height)
{
    if (dry_run_) {
        spdlog::debug("[Engine] Step 8: dry-run mode -- skipping offer management");
        co_return;
    }

    for (auto& [pair_name, pcs] : cycle_) {
        if (!pcs.quote_valid || pcs.ladder.empty()) continue;

        // [T3-24] Final gate: do not post offers if market data was invalid.
        // This catches pairs that passed step 4 staleness but failed step 1.
        // ISO/IEC 5055: prevents posting with stale upstream data.
        if (!pcs.market_data_valid) {
            spdlog::warn("[Engine] Step 8: {} market data invalid -- "
                         "skipping offer posting", pair_name);
            continue;
        }

        // [T1-03] co_await cancel_stale directly instead of use_future.
        int cancelled = co_await offer_mgr_->cancel_stale(
            pair_name, block_height, config_.strategy.offer_ttl_blocks);
        if (cancelled > 0) {
            spdlog::info("[Engine] Step 8: cancelled {} stale offers for {}",
                         cancelled, pair_name);
        }

        // Both the strategy and execution layers now use the unified
        // xop::TierQuote from types.hpp.  No conversion needed.
        const auto& exec_tiers = pcs.ladder;

        // Find the PairConfig for this pair (O(1) map lookup).
        const PairConfig* pair_cfg = find_pair_config(pair_name);
        if (!pair_cfg) continue;

        // [T1-03] co_await post_quotes directly instead of use_future.
        int posted = co_await offer_mgr_->post_quotes(
            *pair_cfg, exec_tiers, block_height);

        // [T2-09] Persist actual wallet-assigned offer IDs to the database.
        // post_quotes() stores PendingOffers in shared State with the real
        // trade_id from the wallet RPC response.  We query State for the
        // current pending offers for this pair and match them to our ladder
        // tiers by (side, tier_index, price) to get the actual offer IDs.
        // This replaces the old placeholder ID approach.
        // ISO/IEC 5055: no orphaned placeholder IDs in the audit trail.
        {
            auto pending_offers = state_->get_all_offers();
            // Build a lookup from (pair, side, tier) to the real offer_id.
            std::unordered_map<std::string, std::string> tier_to_id;
            for (const auto& po : pending_offers) {
                if (po.pair_name != pair_name) continue;
                if (po.created_at_block != block_height) continue;
                // Key: "side_tier" for matching.
                std::string key = std::to_string(static_cast<int>(po.side))
                    + "_" + std::to_string(po.tier);
                tier_to_id[key] = po.offer_id;
            }

            for (const auto& etq : exec_tiers) {
                DbOfferRecord orec;
                orec.pair_name     = pair_name;
                orec.side          = (etq.side == Side::Bid) ? "bid" : "ask";
                orec.price_mojos   = etq.price;
                orec.size_mojos    = etq.size;
                orec.tier          = etq.tier_index;
                orec.status        = "pending";
                orec.created_block = block_height;

                // Look up the actual offer_id from State.
                std::string key = std::to_string(static_cast<int>(etq.side))
                    + "_" + std::to_string(etq.tier_index);
                auto id_it = tier_to_id.find(key);
                if (id_it != tier_to_id.end()) {
                    // [T2-09] Use the actual wallet-assigned offer ID.
                    orec.offer_id = id_it->second;
                } else {
                    // Fallback: post_quotes may have skipped this tier due
                    // to a wallet RPC error.  Use a placeholder and log.
                    orec.offer_id = pair_name + "_" +
                        std::to_string(block_height) + "_" +
                        std::to_string(etq.tier_index) + "_" +
                        ((etq.side == Side::Bid) ? "bid" : "ask") +
                        "_unresolved";
                    spdlog::warn("[Engine] Step 8: no wallet offer_id for {} "
                                 "{} tier {} -- using placeholder",
                                 pair_name,
                                 (etq.side == Side::Bid) ? "bid" : "ask",
                                 etq.tier_index);
                }
                db_->insert_offer(orec);
            }
        }

        // Record rebalance baseline so the LiquidityEngine can evaluate
        // future rebalance triggers.
        auto& liq = *liquidity_engines_[pair_name];
        Mojo mid_mojos = (pcs.risk_quote.bid_price + pcs.risk_quote.ask_price) / 2;
        double inv_ratio = 0.5;
        if (pair_cfg) {
            inv_ratio = inventory_->inventory_ratio(
                AssetId{pair_cfg->base_asset_id},
                AssetId{pair_cfg->quote_asset_id},
                mid_mojos);
        }
        liq.record_rebalance(block_height, mid_mojos, inv_ratio);

        spdlog::info("[Engine] Step 8: posted {} offers for {} (cancelled {})",
                     posted, pair_name, cancelled);
    }

    co_return;
}

// Step 9: Check arbitrage opportunities.
void Engine::step_check_arbitrage(BlockHeight block_height)
{
    // Arbitrage scanning is driven by the MarketDataFeed's ArbitrageSignal
    // callback.  Here we check for signals that fired during step 1.
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        auto signal = market_data_->get_latest_arb_signal(pair.name);
        if (signal) {
            spdlog::info("[Engine] Step 9: Arbitrage signal for {}: "
                         "dex={:.4f} cex={:.4f} divergence={:.1f}bps dir={}",
                         pair.name,
                         signal->dex_price, signal->cex_price,
                         signal->divergence_bps,
                         to_string(signal->direction));
            // Phase 2: execute the arbitrage via ArbitrageScanner.
        }
    }
}

// Step 10: Run hedging layer (compute skew, NHE).
// [T3-08] Now calls compute_nhe() with actual fill data from Step 2
// accumulators (nhe_net_inventory_change_, nhe_total_volume_) and
// alerts when NHE drops below the 0.70 target.
void Engine::step_run_hedging(BlockHeight block_height)
{
    // Layer 1: inventory-based skew (already applied in step 7 via
    // LiquidityEngine::apply_inventory_skew).

    // [T3-08] Layer 2: compute Natural Hedge Efficiency from fill data.
    // NHE = 1 - |net_inventory_change| / total_volume.
    // A high NHE (close to 1.0) means fills are balanced (buys ~ sells).
    // A low NHE means the engine is accumulating one-sided inventory.
    // ISO/IEC 5055: uses the actual fill accumulators from Step 2.
    double nhe = HedgingManager::compute_nhe(
        nhe_net_inventory_change_, nhe_total_volume_);

    if (nhe_total_volume_ > 0.0) {
        spdlog::info("[Engine] Step 10: NHE={:.3f} (net_inv_change={:.0f}, "
                     "total_vol={:.0f})",
                     nhe, nhe_net_inventory_change_, nhe_total_volume_);

        // [T3-08] Alert when NHE drops below the 0.70 target.
        // Low NHE indicates the engine is consistently accumulating
        // one-sided inventory, which increases adverse selection risk.
        // ISO/IEC 27001:2022: alert for operational visibility.
        if (nhe < HedgingManager::nhe_target()) {
            spdlog::warn("[Engine] Step 10: NHE {:.3f} < target {:.2f} "
                         "-- one-sided inventory accumulation detected",
                         nhe, HedgingManager::nhe_target());
            alerts_->send_alert(AlertRule::ExposureBreach,
                "Natural Hedge Efficiency " + std::to_string(nhe) +
                " below target " +
                std::to_string(HedgingManager::nhe_target()) +
                " -- review inventory balance");
        }
    }

    // Layer 3: portfolio-level netting.
    auto positions = state_->get_all_positions();
    auto exposure = HedgingManager::compute_portfolio_net_exposure(positions);

    // Layer 4: statistical pairs hedging -- suggest rebalancing trades.
    // For Phase 1, we log the suggestions without auto-executing.
    std::unordered_map<AssetId, double> targets;
    // Phase 2: populate targets from the capital allocation config.

    spdlog::debug("[Engine] Step 10: hedging layer complete; {} positions tracked, "
                  "NHE={:.3f}", positions.size(), nhe);
}

// Step 11: Update PnL attribution.
void Engine::step_update_pnl(BlockHeight block_height)
{
    // Mark-to-market all positions.
    pnl_->mark_to_market(
        // get_price callback: return mid-price in mojos for a pair/asset.
        [this](const std::string& pair, const std::string& asset) -> Mojo {
            auto snap = state_->get_market(pair);
            return snap.mid_price;
        },
        // get_balance callback: return balance in mojos for an asset.
        [this](const std::string& asset) -> Mojo {
            auto pos = state_->get_position(AssetId{asset});
            return pos.balance;
        },
        // get_cost_basis callback.
        [this](const std::string& asset) -> Mojo {
            auto rec = inventory_->get_record(AssetId{asset});
            return rec.weighted_avg_cost_basis;
        },
        // [H9] XCH/USD rate -- use named constant instead of magic number.
        // TODO: fetch from CEX feed (Phase 2).
        // ISO/IEC 5055: no magic numbers in financial calculations.
        kFallbackXchUsdRate);

    // Persist a snapshot for each enabled pair.
    std::vector<DbSnapshot> batch;
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        DbSnapshot snap;
        snap.block_height    = block_height;
        snap.pair_name       = pair.name;

        auto mkt = state_->get_market(pair.name);
        snap.mid_price_mojos = mkt.mid_price;
        snap.spread_bps      = mkt.spread_bps;

        snap.inventory_ratio = inventory_->inventory_ratio(
            AssetId{pair.base_asset_id},
            AssetId{pair.quote_asset_id},
            mkt.mid_price);

        auto vol_it = vol_estimators_.find(pair.name);
        snap.sigma_block = (vol_it != vol_estimators_.end())
            ? vol_it->second->get_sigma_block()
            : 0.0;

        auto regime = (vol_it != vol_estimators_.end())
            ? vol_it->second->get_regime()
            : RegimeInfo{MarketRegime::Random, 1.0, 1.0, 1.0};
        snap.regime = to_string(regime.regime);

        auto pnl_summary = pnl_->get_total_pnl();
        snap.pnl_total_mojos = pnl_summary.total_pnl;

        batch.push_back(std::move(snap));
    }

    if (!batch.empty()) {
        db_->insert_snapshots_batch(batch);
    }

    spdlog::debug("[Engine] Step 11: PnL updated and {} snapshots persisted",
                  batch.size());
}

// Step 12: Export metrics to Prometheus.
void Engine::step_export_metrics(BlockHeight block_height)
{
    if (!metrics_->is_running()) return;

    // Dashboard 1: PnL
    auto total = pnl_->get_total_pnl();
    MetricsPnlSnapshot ps;
    ps.total      = total.total_pnl;
    ps.realized   = total.spread_pnl;  // spread PnL = realized
    ps.unrealized = total.inventory_pnl;
    ps.spread     = total.spread_pnl;
    ps.inventory  = total.inventory_pnl;
    metrics_->update_pnl(ps);

    // Dashboard 2: Inventory
    auto positions = state_->get_all_positions();
    std::vector<InventorySnapshot> inv_snaps;
    std::vector<InventorySkewSnapshot> skew_snaps;
    for (const auto& pos : positions) {
        InventorySnapshot is;
        is.asset_id   = pos.asset_id;
        is.balance    = pos.balance;
        is.cost_basis = pos.cost_basis;
        is.underwater = (pos.cost_basis > 0 && pos.balance > 0);
        inv_snaps.push_back(is);
    }
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;
        InventorySkewSnapshot iss;
        iss.pair_name = pair.name;
        iss.skew      = state_->inventory_skew(
            AssetId{pair.base_asset_id},
            AssetId{pair.quote_asset_id});
        skew_snaps.push_back(iss);
    }
    metrics_->update_inventory(inv_snaps, skew_snaps);

    // Dashboard 3: Market data
    auto markets = state_->get_all_markets();
    metrics_->update_market(markets);

    // Dashboard 4: System health
    SystemHealthSnapshot health;
    health.block_height    = block_height;
    health.node_synced     = true;  // Phase 2: check full node sync status.
    health.wallet_connected = wallet_->is_open();
    metrics_->update_system_health(health);

    // Dashboard 5: Offer lifecycle
    metrics_->update_offers(
        state_->offer_count(),
        total.fill_count,
        /*cancel_count=*/0,  // Phase 2: track cumulative cancellations.
        /*expired_count=*/0,
        total.fill_rate_per_hour);

    // Dashboard 6: Risk
    RiskSnapshot risk;
    risk.var_95       = 0.0;  // Phase 2: compute VaR from PnL history.
    risk.max_drawdown = total.max_drawdown;
    metrics_->update_risk(risk, {});

    spdlog::debug("[Engine] Step 12: metrics exported");
}

// Step 13: Check alert rules.
void Engine::step_check_alerts(BlockHeight block_height)
{
    // Assemble the BotState snapshot for the alert manager.
    BotState bs;
    bs.current_block_height = block_height;
    bs.network_block_height = block_height;  // Phase 2: peer info.
    bs.wallet_connected     = wallet_->is_open();
    bs.fill_rate_per_hour   = pnl_->get_total_pnl().fill_rate_per_hour;
    bs.avg_fill_rate_24h    = bs.fill_rate_per_hour;  // Phase 2: 24h rolling.
    bs.consecutive_offer_failures = 0;

    // Per-pair state for alert evaluation.
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        auto mkt = state_->get_market(pair.name);

        BotState::PairState ps;
        ps.pair_name         = pair.name;
        ps.current_spread_bps = mkt.spread_bps;
        ps.normal_spread_bps  = 100.0;  // Phase 2: rolling average.
        ps.mid_price          = mkt.mid_price;
        ps.recent_high        = mkt.mid_price;  // Phase 2: lookback window.
        bs.pairs.push_back(ps);
    }

    // PnL state.
    auto total = pnl_->get_total_pnl();
    bs.total_pnl = total.total_pnl;

    // [MEDIUM-7] Seed peak_pnl_hwm_ on the very first cycle so the drawdown
    // circuit breaker is active from startup.  Previously, peak_pnl_hwm_
    // started at 0 and the drawdown check was gated on peak_pnl_hwm_ > 0,
    // which meant the engine had ZERO drawdown protection until the first
    // profitable cycle.  If the engine started losing money from the first
    // block, the circuit breaker would never fire.
    //
    // Fix: on the first cycle, initialize peak_pnl_hwm_ to total_pnl
    // (which may be 0 or negative).  On subsequent cycles, the existing
    // max() logic ensures monotonic non-decrease.
    // ISO/IEC 27001:2022: continuous risk monitoring from first tick.
    // ISO/IEC 5055: deterministic initialization prevents unprotected window.
    if (!hwm_initialized_) {
        peak_pnl_hwm_    = total.total_pnl;
        hwm_initialized_ = true;
    }
    // [H6] Track the PnL high-water mark across cycles for drawdown alerts.
    // ISO/IEC 5055: monotonically non-decreasing peak prevents false
    // drawdown resets when total_pnl oscillates.
    peak_pnl_hwm_ = std::max(peak_pnl_hwm_, total.total_pnl);
    bs.peak_pnl  = peak_pnl_hwm_;

    // Inventory exposure.
    bs.max_inventory_ratio = 0.5;  // Phase 2: compute max across all pairs.
    bs.hard_limit_pct      = config_.risk.hard_limit_pct;

    // Run all 14 alert rules.
    alerts_->check_and_alert(bs);

    // [T3-09] Max-drawdown global circuit breaker.
    // Compute drawdown = (peak_pnl_hwm_ - total_pnl) / abs(peak_pnl_hwm_).
    // If the drawdown exceeds max_drawdown_pct_, transition to Paused state
    // and alert.  This is a global safety net that prevents runaway losses.
    //
    // [MEDIUM-7] The condition now checks peak_pnl_hwm_ > 0 OR total_pnl < 0.
    // This ensures the circuit breaker fires even when the engine has never
    // been profitable (peak == 0) but is actively losing money.  The division
    // by abs(peak_pnl_hwm_) is guarded: when peak is exactly 0 and PnL is
    // negative, we treat any loss as exceeding the threshold.
    //
    // ISO/IEC 5055: guards against division by zero and sign errors.
    // ISO/IEC 27001:2022: audit-logged state transition; no unprotected
    //   window at startup.
    if (peak_pnl_hwm_ > 0 || total.total_pnl < 0) {
        double drawdown_frac = 0.0;

        if (peak_pnl_hwm_ > 0) {
            // Normal case: we've had profit, measure drop from peak.
            drawdown_frac =
                static_cast<double>(peak_pnl_hwm_ - total.total_pnl)
                / static_cast<double>(std::abs(peak_pnl_hwm_));
        } else {
            // [MEDIUM-7] Edge case: peak is zero or negative (never
            // profitable).  Any negative PnL constitutes a drawdown.
            // Use abs(total_pnl) relative to a nominal unit to produce
            // a meaningful fraction; treat it as 100% drawdown if we
            // are losing money from a zero-profit baseline.
            // ISO/IEC 5055: explicit handling of zero-denominator case.
            drawdown_frac = (total.total_pnl < 0) ? 1.0 : 0.0;
        }

        if (drawdown_frac > max_drawdown_pct_) {
            spdlog::error("[Engine] Step 13: MAX DRAWDOWN BREACHED -- "
                          "drawdown={:.2f}% > threshold={:.2f}% -- "
                          "transitioning to Paused state",
                          drawdown_frac * 100.0,
                          max_drawdown_pct_ * 100.0);

            // Transition to Paused: stops new offer creation in subsequent
            // cycles while keeping connections open for monitoring.
            state_->set_status(BotStatus::Paused);

            alerts_->send_alert(AlertRule::PnlDrawdown,
                "Global max-drawdown circuit breaker triggered: drawdown " +
                std::to_string(drawdown_frac * 100.0) + "% exceeds threshold " +
                std::to_string(max_drawdown_pct_ * 100.0) +
                "% -- engine PAUSED.  Manual intervention required.");
        }
    }

    spdlog::debug("[Engine] Step 13: alert check complete");
}

// ===========================================================================
// Connection management
// ===========================================================================

// [CRITICAL-1] open_connections() -- converted from void (blocking) to
// awaitable<void> (coroutine).  The previous implementation used
// co_spawn(use_future).get() which deadlocks when ioc_ is not yet running
// because .get() blocks the calling thread while ioc_.run() has not been
// entered, so there is no event loop to drive the completion.
//
// Now open_connections() is co_awaited from the start of poll_loop_coro(),
// which runs after ioc_.run() begins.  This guarantees the io_context is
// pumping events while the coroutine awaits the RPC connections.
//
// ISO/IEC 5055: no blocking .get() on any thread; fully async connection
//               lifecycle within the coroutine model.
// ISO/IEC 27001:2022: connection events are audit-logged.
asio::awaitable<void> Engine::open_connections()
{
    // Open the Chia full node RPC (mTLS) -- co_await on running io_context.
    co_await full_node_->open();
    spdlog::info("[Engine] Connected to Chia full node at {}:{}",
                 config_.chia.full_node_host, config_.chia.full_node_port);

    // Open the Chia wallet RPC (mTLS) -- co_await on running io_context.
    co_await wallet_->open();
    spdlog::info("[Engine] Connected to Chia wallet at {}:{}",
                 config_.chia.wallet_host, config_.chia.wallet_port);

    // Open the dexie REST client (synchronous HTTP -- no coroutine needed).
    dexie_->open();
    spdlog::info("[Engine] Connected to dexie API at {}",
                 config_.dexie.api_base);

    co_return;
}

void Engine::close_connections()
{
    full_node_->close();
    wallet_->close();
    dexie_->close();
    spdlog::info("[Engine] All connections closed");
}

}  // namespace xop
