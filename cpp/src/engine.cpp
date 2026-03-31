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
    , max_drawdown_pct_(config.risk.max_drawdown_pct)
{
    spdlog::info("[Engine] Initializing subsystems (dry_run={})", dry_run);
    spdlog::info("[Engine] Circuit breakers: max_drawdown={:.1f}% "
                 "window_loss={:.0f}bps/{} blocks",
                 config_.risk.max_drawdown_pct * 100.0,
                 config_.risk.max_window_loss_bps,
                 config_.risk.loss_window_blocks);

    // -- Database (must be first: other subsystems may query on construction) --
    db_ = std::make_unique<Database>(config_.database.path);

    // -- RPC / API clients ----------------------------------------------------

    // Build full-node RPC config from AppConfig (skipped in wallet_only mode).
    if (config_.chia.mode != ChiaMode::WalletOnly) {
        rpc::ChiaRPCConfig fn_cfg;
        fn_cfg.host = config_.chia.full_node_host;
        fn_cfg.port = config_.chia.full_node_port;
        fn_cfg.tls.cert_path    = config_.chia.ssl_cert_path;
        fn_cfg.tls.key_path     = config_.chia.ssl_key_path;
        fn_cfg.tls.ca_cert_path = config_.chia.ca_cert_path;
        full_node_ = std::make_shared<rpc::ChiaFullNodeRPC>(ioc_, fn_cfg);
    }

    // Build wallet RPC config from AppConfig.
    rpc::ChiaRPCConfig wal_cfg;
    wal_cfg.host = config_.chia.wallet_host;
    wal_cfg.port = config_.chia.wallet_port;
    wal_cfg.tls.cert_path    = config_.chia.wallet_cert_path;
    wal_cfg.tls.key_path     = config_.chia.wallet_key_path;
    wal_cfg.tls.ca_cert_path = config_.chia.ca_cert_path;
    wallet_ = std::make_shared<rpc::ChiaWalletRPC>(ioc_, wal_cfg);

    // Build dexie client config from AppConfig.
    rpc::DexieConfig dexie_cfg;
    dexie_cfg.base_url                = config_.dexie.api_base;
    dexie_cfg.rate_limit_max_requests = config_.dexie.max_requests_per_10s;
    dexie_ = std::make_shared<rpc::DexieClient>(ioc_, dexie_cfg);

    // Build CoinGecko client from AppConfig (external price reference).
    if (config_.coingecko.enabled) {
        coingecko_ = std::make_shared<rpc::CoinGeckoClient>(
            ioc_, config_.coingecko);
    }

    // -- Execution layer ------------------------------------------------------

    coin_mgr_ = std::make_unique<execution::CoinManager>(
        ioc_, wallet_, config_);

    // Pass the shared dexie client so OfferManager can submit offers to
    // the Dexie aggregator for cross-platform visibility.
    offer_mgr_ = std::make_unique<execution::OfferManager>(
        ioc_, wallet_, dexie_, state_, config_);

    // Market data feed: construct with a MarketDataConfig populated from
    // the YAML `market_data:` section (T4-05).
    MarketDataConfig md_cfg;
    md_cfg.whale_trade_threshold        = config_.market_data.whale_trade_threshold;
    md_cfg.whale_volume_fraction        = config_.market_data.whale_volume_fraction;
    md_cfg.whale_window_blocks          = config_.market_data.whale_window_blocks;
    md_cfg.whale_max_spread_multiplier  = config_.market_data.whale_max_spread_multiplier;
    md_cfg.vpin_bucket_size             = config_.market_data.vpin_bucket_size;
    md_cfg.vpin_window_buckets          = config_.market_data.vpin_window_buckets;
    md_cfg.ofi_window_size              = config_.market_data.ofi_window_size;
    md_cfg.enable_competitor_tracking   = config_.market_data.enable_competitor_tracking;
    md_cfg.min_competitor_offer_size    = config_.market_data.min_competitor_offer_size;
    md_cfg.competitor_alert_threshold_bps = config_.market_data.competitor_alert_threshold_bps;
    md_cfg.asymmetric_skew_factor       = config_.market_data.asymmetric_skew_factor;
    md_cfg.cex_freshness_threshold_sec  = config_.market_data.cex_freshness_threshold_sec;
    market_data_ = std::make_unique<MarketDataFeed>(md_cfg, *state_);

    // -- Data / analytics (per-pair estimators) --------------------------------

    VolatilityEstimatorConfig vol_cfg;
    vol_cfg.lookback_blocks = config_.volatility.lookback_blocks;
    vol_cfg.yz_alpha        = config_.volatility.yz_alpha;
    // [T5-CR6] Pass candle aggregation window from user config.
    vol_cfg.candle_aggregation_blocks = config_.volatility.candle_aggregation_blocks;

    AdverseSelectionConfig as_cfg;
    as_cfg.prior_alpha        = config_.adverse_selection.prior_alpha;
    as_cfg.prior_beta         = config_.adverse_selection.prior_beta;
    as_cfg.observation_blocks = config_.adverse_selection.observation_blocks;
    as_cfg.adverse_threshold  = config_.adverse_selection.adverse_threshold;
    as_cfg.max_history        = config_.adverse_selection.max_history;
    as_cfg.decay_factor       = config_.adverse_selection.decay_factor;

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
    // Per-pair strategy overrides allow stablecoin pairs to use tighter
    // spreads and lower risk aversion than volatile pairs.
    // ISO/IEC 5055: no shared mutable state across independent pairs.
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;
        AvellanedaConfig as_strat_cfg;
        as_strat_cfg.gamma = pair.gamma_override.value_or(config_.strategy.gamma);
        as_strat_cfg.kappa = pair.kappa_override.value_or(config_.strategy.kappa);
        as_strat_cfg.q_max = pair.q_max_override.value_or(config_.strategy.q_max);
        as_strat_cfg.min_margin_bps =
            pair.min_profit_margin_bps_override.value_or(
                config_.strategy.min_profit_margin_bps);
        strategies_[pair.name] =
            std::make_unique<AvellanedaStoikov>(as_strat_cfg);
    }

    // Spread optimizer with config-derived parameters.
    SpreadConfig sp_cfg;
    sp_cfg.gamma = config_.strategy.gamma;
    sp_cfg.s_floor_bps = config_.strategy.min_profit_margin_bps;
    spread_opt_ = std::make_unique<SpreadOptimizer>(sp_cfg);

    // Per-pair liquidity engines — use per-pair tier overrides when present.
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;
        LiquidityConfig liq_cfg;
        liq_cfg.num_tiers        = config_.strategy.num_tiers;
        liq_cfg.tier_spacing_bps =
            pair.tier_spacing_bps_override.value_or(
                config_.strategy.tier_spacing_bps);
        liq_cfg.tier_size_pct    =
            pair.tier_size_pct_override.value_or(
                config_.strategy.tier_size_pct);
        liq_cfg.offer_ttl_blocks = config_.strategy.offer_ttl_blocks;
        liquidity_engines_[pair.name] =
            std::make_unique<LiquidityEngine>(pair.name, liq_cfg);
    }

    // -- Depeg detector -------------------------------------------------------
    depeg_detector_ = std::make_unique<DepegDetector>(config_.depeg);
    for (const auto& pair : config_.pairs) {
        depeg_detector_->register_pair(pair);
    }

    // -- Arbitrage detector ---------------------------------------------------
    {
        // Build ArbitrageConfig from the YAML-parsed ArbitrageSettings.
        ArbitrageConfig arb_cfg;
        arb_cfg.triangular_min_profit_bps  = config_.arbitrage.triangular_min_profit_bps;
        arb_cfg.triangular_slippage_bps    = config_.arbitrage.triangular_slippage_bps;
        arb_cfg.triangular_per_leg_fee_bps = config_.arbitrage.triangular_per_leg_fee_bps;
        arb_cfg.triangular_max_legs        = config_.arbitrage.triangular_max_legs;
        arb_cfg.cex_dex_min_edge_bps       = config_.arbitrage.cex_dex_min_edge_bps;
        arb_cfg.cex_dex_max_edge_bps       = config_.arbitrage.cex_dex_max_edge_bps;
        arb_cfg.cex_fee_bps               = config_.arbitrage.cex_fee_bps;
        arb_cfg.bridge_fee_bps            = config_.arbitrage.bridge_fee_bps;
        arb_cfg.cross_dex_min_edge_bps    = config_.arbitrage.cross_dex_min_edge_bps;
        arb_cfg.tibetswap_fee_bps         = config_.arbitrage.tibetswap_fee_bps;
        arb_cfg.dexie_fee_bps             = config_.arbitrage.dexie_fee_bps;
        arb_cfg.cross_bridge_min_edge_bps = config_.arbitrage.cross_bridge_min_edge_bps;
        arb_cfg.bridge_cost_bps           = config_.arbitrage.bridge_cost_bps;
        arb_cfg.max_position_size         = config_.arbitrage.max_position_size;
        arb_cfg.default_confidence        = config_.arbitrage.default_confidence;
        arb_cfg.min_confidence_threshold  = config_.arbitrage.min_confidence_threshold;
        arb_cfg.default_urgency_blocks    = config_.arbitrage.default_urgency_blocks;

        arb_detector_ = std::make_unique<ArbitrageDetector>(arb_cfg);

        // Pre-populate the stablecoin pair set from config.
        StablecoinPairSet stable_pairs;
        for (const auto& pair : config_.pairs) {
            if (pair.is_stablecoin && pair.enabled) {
                stable_pairs.insert(pair.name);
            }
        }
        if (!stable_pairs.empty()) {
            arb_detector_->set_stablecoin_pairs(stable_pairs);
        }
    }

    // -- Fee tracker ----------------------------------------------------------
    fee_tracker_ = std::make_unique<FeeTracker>(config_.fees);

    // -- [T4-16] Online kappa calibrator --------------------------------------
    {
        KappaCalibratorConfig kc_cfg;
        kc_cfg.default_kappa = config_.strategy.kappa;
        kappa_calibrator_ = std::make_unique<KappaCalibrator>(kc_cfg);
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

    // -- Startup market analysis --------------------------------------------
    // Build the list of enabled pair names for the analyzer.
    const uint32_t analysis_blocks = config_.strategy.startup_analysis_blocks;
    if (analysis_blocks > 0) {
        std::vector<std::string> enabled_pairs;
        enabled_pairs.reserve(config_.pairs.size());
        for (const auto& pc : config_.pairs) {
            if (pc.enabled) enabled_pairs.push_back(pc.name);
        }

        MarketAnalyzerConfig ma_cfg;
        ma_cfg.analysis_blocks = analysis_blocks;
        market_analyzer_ = std::make_unique<MarketAnalyzer>(ma_cfg, enabled_pairs);
        spdlog::info("[Engine] Startup analysis enabled: {} blocks (~{}s)",
                     analysis_blocks,
                     static_cast<uint32_t>(analysis_blocks * ma_cfg.block_time_seconds));
    } else {
        spdlog::info("[Engine] Startup analysis disabled (startup_analysis_blocks=0)");
    }

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
    // Guard also covers BotStatus::Analyzing so that destroying the engine
    // during the startup analysis phase correctly cancels the timer and tears
    // down the io_context.
    const BotStatus st = state_->status();
    if (st == BotStatus::Running || st == BotStatus::Analyzing ||
        st == BotStatus::Paused) {
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

    // Transition to Analyzing (if startup analysis is enabled) or Running.
    if (market_analyzer_) {
        state_->set_status(BotStatus::Analyzing);
    } else {
        state_->set_status(BotStatus::Running);
    }

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
    try {
        poll_timer_.cancel();
    } catch (const boost::system::system_error& e) {
        spdlog::warn("[Engine] poll_timer_.cancel() failed: {}", e.what());
    }

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

    // -- Startup market analysis phase ---------------------------------------
    // If configured, observe the market for startup_analysis_blocks before
    // entering active trading.  The engine is in BotStatus::Analyzing during
    // this period; no offers are posted.
    // ISO/IEC 5055: analysis is optional and gracefully skipped on error.
    if (market_analyzer_) {
        try {
            co_await run_startup_analysis();
        } catch (const std::exception& ex) {
            spdlog::warn("[Engine] Startup analysis failed: {}; starting trading",
                         ex.what());
            // Disable further use of the market analyzer and ensure we
            // transition to Running even on analysis failure.
            market_analyzer_.reset();
            state_->set_status(BotStatus::Running);
        }
    }

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
            // -- Wallet circuit breaker: probe for recovery ----------------
            if (wallet_circuit_open_) {
                auto now = std::chrono::steady_clock::now();
                if (now - wallet_last_probe_ >= kWalletProbeInterval) {
                    wallet_last_probe_ = now;
                    try {
                        co_await wallet_->get_sync_status();
                        // Success — wallet is back.
                        wallet_circuit_open_       = false;
                        wallet_consecutive_failures_ = 0;
                        spdlog::info("[Engine] Wallet circuit breaker CLOSED "
                                     "-- wallet is reachable again");
                        // Reconcile offers to catch any orphans from the
                        // outage period.
                        if (offer_mgr_) {
                            try {
                                int fixed = co_await offer_mgr_->reconcile_offers();
                                if (fixed > 0) {
                                    spdlog::info("[Engine] Post-reconnect offer "
                                                 "reconciliation corrected {} "
                                                 "discrepancies", fixed);
                                }
                            } catch (const std::exception& re) {
                                spdlog::warn("[Engine] Post-reconnect offer "
                                             "reconciliation failed: {}",
                                             re.what());
                            }
                        }
                    } catch (...) {
                        spdlog::debug("[Engine] Wallet circuit breaker probe "
                                      "failed -- still unreachable");
                    }
                }
            }

            // co_await the block height -- use wallet RPC in wallet-only mode.
            std::int64_t height = wallet_only_mode_
                ? co_await wallet_->get_height_info()
                : co_await full_node_->get_block_height();

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
// Startup market analysis
// ===========================================================================

// run_startup_analysis -- collect market observations before trading.
//
// The coroutine polls for new blocks (same cadence as the main loop) and,
// for each new block, fetches market data via step_update_market_state and
// feeds the observations to market_analyzer_.  Progress and per-block
// summaries are exported to Prometheus so the GUI can display them.
//
// On completion the analysis summary is logged at INFO level and the bot
// transitions from Analyzing -> Running.
//
// ISO/IEC 5055: no blocking calls; fully async via co_await.
// ISO/IEC 27001:2022: all failure paths log a warning and continue.
asio::awaitable<void> Engine::run_startup_analysis()
{
    if (!market_analyzer_) co_return;

    const uint32_t target = market_analyzer_->analysis_blocks();
    spdlog::info("[Engine] Starting market analysis phase ({} blocks)", target);

    asio::steady_timer timer(ioc_);
    BlockHeight last_analysis_block{0};

    // [TIMEOUT] Maximum total block polls before forcing completion.
    // Prevents the engine from hanging indefinitely if a pair has no
    // market data (e.g. a newly listed pair with zero trading activity).
    // Uses MarketAnalyzerConfig::timeout_block_multiplier (default 3x).
    // Clamp to >= 1 so that a misconfigured 0 doesn't trigger an
    // immediate timeout before any data is collected.
    uint32_t timeout_mult = market_analyzer_->timeout_block_multiplier();
    if (timeout_mult == 0) {
        spdlog::warn("[Engine] MarketAnalyzer timeout_block_multiplier configured "
                     "as 0; clamping to 1 to avoid immediate analysis timeout");
        timeout_mult = 1;
    }
    const uint32_t max_total_polls = target * timeout_mult;
    uint32_t total_polls = 0;

    while (!stop_requested_.load(std::memory_order_relaxed) &&
           !market_analyzer_->is_complete()) {

        // Check timeout: force-complete if we've polled too many blocks
        // without all pairs finishing.
        if (total_polls >= max_total_polls) {
            spdlog::warn("[Engine] Analysis timeout after {} block polls "
                         "(target was {} blocks); forcing completion with "
                         "partial data",
                         total_polls, target);
            market_analyzer_->force_complete();
            break;
        }

        timer.expires_after(kPollInterval);
        boost::system::error_code ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        if (ec == boost::asio::error::operation_aborted) {
            co_return;  // Shutdown during analysis.
        }
        if (stop_requested_.load(std::memory_order_relaxed)) {
            co_return;  // Shutdown requested.
        }
        if (ec) {
            // Non-shutdown timer error — log and fall through to
            // complete analysis with whatever data we have.
            spdlog::warn("[Engine] Analysis timer error: {}; ending analysis early",
                         ec.message());
            break;
        }

        // Fetch current block height.
        std::int64_t height{0};
        try {
            height = wallet_only_mode_
                ? co_await wallet_->get_height_info()
                : co_await full_node_->get_block_height();
        } catch (const std::exception& ex) {
            spdlog::warn("[Engine] Analysis: block height poll failed: {}", ex.what());
            continue;
        }

        if (height <= 0) continue;
        const BlockHeight current_block = static_cast<BlockHeight>(height);
        if (current_block <= last_analysis_block) continue;
        last_analysis_block = current_block;
        ++total_polls;

        // Fetch market data for this block.
        // Initialize per-pair cycle entries so step_update_market_state can write.
        cycle_.clear();
        for (const auto& pair : config_.pairs) {
            if (!pair.enabled) continue;
            PairCycleState pcs;
            pcs.pair_name = pair.name;
            cycle_[pair.name] = std::move(pcs);
        }

        try {
            co_await step_update_market_state(current_block);
        } catch (const std::exception& ex) {
            spdlog::warn("[Engine] Analysis: market state update failed: {}", ex.what());
            continue;
        }

        // Feed observations to the analyzer for each pair.
        // Always call ingest() even with invalid mid_price so that
        // total_poll_attempts is tracked for data-quality reporting.
        // MarketAnalyzer::ingest() will reject invalid data internally.
        for (const auto& pair : config_.pairs) {
            if (!pair.enabled) continue;

            const MarketSnapshot snap = state_->get_market(pair.name);

            const double mid_d    = static_cast<double>(snap.mid_price);
            const double spread_d = snap.spread_bps;
            const double vol_d    = static_cast<double>(snap.volume_24h);

            // Use competing offer depth counts as proxy for book imbalance.
            // competing_depth_bids / asks are size_t counts of resting offers.
            double bid_depth = 0.0;
            double ask_depth = 0.0;
            {
                const auto cm = market_data_->get_competitor_metrics(pair.name);
                if (cm.has_value()) {
                    bid_depth = static_cast<double>(cm->competing_depth_bids);
                    ask_depth = static_cast<double>(cm->competing_depth_asks);
                }
            }

            market_analyzer_->ingest(pair.name, mid_d, spread_d, vol_d,
                                     bid_depth, ask_depth);

            // Export per-pair analysis metrics to Prometheus.
            if (metrics_->is_running()) {
                const auto summary = market_analyzer_->get_summary(pair.name);
                metrics_->update_analysis(
                    pair.name,
                    summary.blocks_collected,
                    target,
                    summary.volatility_annual,
                    summary.mean_spread_bps,
                    summary.spread_cv,
                    summary.variance_ratio,
                    summary.book_imbalance,
                    summary.momentum,
                    static_cast<int>(summary.regime),
                    static_cast<int>(summary.aggressiveness));
            }
        }

        // Log progress.
        const uint32_t min_collected = [&]() -> uint32_t {
            uint32_t m = target;
            for (const auto& pair : config_.pairs) {
                if (!pair.enabled) continue;
                m = std::min(m, market_analyzer_->blocks_collected(pair.name));
            }
            return m;
        }();
        spdlog::info("[Engine] Analysis progress: {}/{} blocks", min_collected, target);
    }

    if (stop_requested_.load(std::memory_order_relaxed)) co_return;

    // Log the completed analysis summaries.
    auto summaries = market_analyzer_->get_summaries();
    for (const auto& s : summaries) {
        spdlog::info("[Engine] Analysis complete for {}: "
                     "vol_ann={:.2f}% spread={:.1f}bps cv={:.2f} "
                     "VR={:.3f} regime={} recommendation={}",
                     s.pair_name,
                     s.volatility_annual * 100.0,
                     s.mean_spread_bps,
                     s.spread_cv,
                     s.variance_ratio,
                     to_string(s.regime),
                     to_string(s.aggressiveness));
    }

    // Apply the analysis recommendation to derive an initial spread
    // multiplier.  This influences how conservatively/aggressively the
    // engine quotes during the first trading blocks.
    //
    // The multiplier is stored both locally (for step_apply_spread_optimizer)
    // and in State (for GUI/monitoring accessibility).
    //
    // Conservative → 1.5  (50% wider spreads: protect against adverse selection)
    // Normal       → 1.0  (no change from configured defaults)
    // Aggressive   → 0.8  (20% tighter: capture spread in stable markets)
    //
    // Compute overall_recommendation() once and derive the multiplier from
    // it, avoiding a second full traversal of all pair summaries.
    const auto overall = market_analyzer_->overall_recommendation();
    switch (overall) {
        case AnalysisAggressiveness::Conservative: analysis_spread_mult_ = 1.5; break;
        case AnalysisAggressiveness::Aggressive:   analysis_spread_mult_ = 0.8; break;
        default:                                   analysis_spread_mult_ = 1.0; break;
    }

    spdlog::info("[Engine] Analysis recommendation: {} → spread multiplier {:.2f}x",
                 to_string(overall), analysis_spread_mult_);

    // Persist summaries and multiplier in State for GUI/monitoring.
    state_->set_analysis_results(std::move(summaries), analysis_spread_mult_);

    // Export spread multiplier to Prometheus.
    if (metrics_->is_running()) {
        metrics_->set_analysis_spread_multiplier(analysis_spread_mult_);
    }

    // Transition to active trading.
    state_->set_status(BotStatus::Running);
    spdlog::info("[Engine] Startup analysis complete; entering trading mode");

    co_return;
}

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
    // Gated by the wallet circuit breaker to avoid timeout cascades.
    if (!wallet_circuit_open_) {
        try {
            co_await step_process_fills(block_height);
            wallet_consecutive_failures_ = 0;  // Reset on success.
        }
        catch (const std::exception& e) {
            spdlog::error("[Engine] Step 2 (fills) failed: {}", e.what());
            ++wallet_consecutive_failures_;
            if (wallet_consecutive_failures_ >= kWalletCircuitBreakerThreshold) {
                wallet_circuit_open_ = true;
                wallet_last_probe_   = std::chrono::steady_clock::now();
                spdlog::warn("[Engine] Wallet circuit breaker OPEN after {} "
                             "consecutive failures -- skipping wallet-dependent "
                             "steps until recovery",
                             wallet_consecutive_failures_);
            }
        }
    } else {
        spdlog::debug("[Engine] Step 2 SKIPPED: wallet circuit breaker open");
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
            if (PreTradeCheck::check_flash_crash(price_vec,
                    config_.risk.flash_crash_threshold_pct)) {
                any_pair_crashing = true;
                if (crash_pair_name.empty()) crash_pair_name = pair.name;
            }

            // Check stability signals for this pair.
            if (!PreTradeCheck::is_stable_after_crash(
                    price_vec,
                    static_cast<int>(config_.risk.recovery_stable_blocks_phase1),
                    config_.risk.recovery_stability_band_pct)) {
                all_pairs_stable_50 = false;
            }
            if (!PreTradeCheck::is_stable_after_crash(
                    price_vec,
                    static_cast<int>(config_.risk.recovery_stable_blocks_phase2),
                    config_.risk.recovery_stability_band_pct)) {
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
    // Also gated by the wallet circuit breaker.
    if (wallet_circuit_open_) {
        spdlog::debug("[Engine] Step 8 SKIPPED: wallet circuit breaker open");
    } else if (flash_crash_state_ == FlashCrashState::Normal) {
        try {
            co_await step_manage_offers(block_height);
            wallet_consecutive_failures_ = 0;  // Reset on success.
        }
        catch (const std::exception& e) {
            spdlog::error("[Engine] Step 8 (offers) failed: {}", e.what());
            ++wallet_consecutive_failures_;
            if (wallet_consecutive_failures_ >= kWalletCircuitBreakerThreshold) {
                wallet_circuit_open_ = true;
                wallet_last_probe_   = std::chrono::steady_clock::now();
                spdlog::warn("[Engine] Wallet circuit breaker OPEN after {} "
                             "consecutive failures -- skipping wallet-dependent "
                             "steps until recovery",
                             wallet_consecutive_failures_);
            }
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

    // -- CoinGecko external price reference (throttled by polling interval) ---
    // Fetch once per polling_interval_ms; cache prices for the per-pair loop.
    if (coingecko_ && coingecko_->is_open()) {
        const auto now = std::chrono::steady_clock::now();
        const auto interval = std::chrono::milliseconds{
            config_.coingecko.polling_interval_ms};
        if (now - coingecko_last_fetch_ >= interval) {
            try {
                coingecko_prices_ = co_await coingecko_->fetch_prices();
                coingecko_last_fetch_ = now;
            } catch (const std::exception& ex) {
                // Transient CoinGecko errors should not abort the cycle.
                spdlog::warn("[Engine] Step 1: CoinGecko fetch failed: {}",
                             ex.what());
            }
        }
    }

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

        // -- CoinGecko-derived CEX reference price ----------------------------
        // Derive a pair-level mid-price from the cached CoinGecko USD prices.
        //
        //   XCH/wUSDC.b     : chia.usd / usd-coin.usd
        //   wmilliETH.b/XCH : (ethereum.usd / 1000) / chia.usd
        //   wmilliETH/XCH   : (ethereum.usd / 1000) / chia.usd
        //   XCH/BYC         : no CoinGecko listing for BYC (skipped)
        //   BYC/wUSDC.b     : no CoinGecko listing for BYC (skipped)
        //
        if (!coingecko_prices_.empty()) {
            double cex_mid = 0.0;
            bool   derived = false;

            if (pair.name == "XCH/wUSDC.b") {
                auto xch_it  = coingecko_prices_.find("chia");
                auto usdc_it = coingecko_prices_.find("usd-coin");
                if (xch_it != coingecko_prices_.end() &&
                    usdc_it != coingecko_prices_.end() &&
                    usdc_it->second > 0.0) {
                    cex_mid = xch_it->second / usdc_it->second;
                    derived = true;
                }
            } else if (pair.name == "wmilliETH.b/XCH" ||
                       pair.name == "wmilliETH/XCH") {
                auto eth_it = coingecko_prices_.find("ethereum");
                auto xch_it = coingecko_prices_.find("chia");
                if (eth_it != coingecko_prices_.end() &&
                    xch_it != coingecko_prices_.end() &&
                    xch_it->second > 0.0) {
                    // wmilliETH = 1/1000 of ETH
                    cex_mid = (eth_it->second / 1000.0) / xch_it->second;
                    derived = true;
                }
            }
            // Pairs with no CoinGecko mapping (BYC) are silently skipped.

            if (derived && cex_mid > 0.0) {
                market_data_->ingest_cex_reference(pair.name, cex_mid);
                spdlog::debug("[Engine] Step 1: {} CoinGecko cex_mid={:.6f}",
                              pair.name, cex_mid);
            }
        }

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

    // -- Adaptive fee estimation (T4-03) ------------------------------------
    // Query the full node's mempool for a fee estimate when adaptive mode
    // is enabled.  Skipped in wallet-only mode (no full node mempool access);
    // falls back to static/historic fees.
    if (fee_tracker_->enabled() && config_.fees.adaptive_enabled
        && !wallet_only_mode_) {
        try {
            auto est = co_await full_node_->get_fee_estimate(/*target_time=*/60);
            if (est > 0) {
                fee_tracker_->update_mempool_estimate(est);
            }
        } catch (const std::exception& e) {
            spdlog::debug("[Engine] Step 1: get_fee_estimate failed: {} "
                          "-- using static/historic fee", e.what());
        }
    }

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
    auto new_fills = co_await offer_mgr_->detect_fills();

    // [T4-02] Reorg protection: confirmation depth gating.
    // Newly detected fills are buffered in pending_unconfirmed_fills_ and
    // only promoted to actual processing once their chain depth exceeds
    // the configured confirmation_depth_blocks threshold.  This prevents
    // a chain reorg from causing the bot to record fills that subsequently
    // disappear, which would corrupt cost-basis tracking.
    const uint32_t conf_depth = config_.strategy.confirmation_depth_blocks;

    // Append newly detected fills to the pending buffer.
    for (auto& f : new_fills) {
        pending_unconfirmed_fills_.push_back(std::move(f));
    }

    // Partition: confirmed fills (sufficient depth) move to processing;
    // unconfirmed fills remain in the buffer.
    std::vector<Fill> confirmed_fills;
    std::vector<Fill> still_pending;
    still_pending.reserve(pending_unconfirmed_fills_.size());

    for (auto& f : pending_unconfirmed_fills_) {
        if (conf_depth == 0 ||
            (block_height >= f.block_height &&
             block_height - f.block_height >= conf_depth)) {
            confirmed_fills.push_back(std::move(f));
        } else {
            still_pending.push_back(std::move(f));
        }
    }
    pending_unconfirmed_fills_ = std::move(still_pending);

    if (!pending_unconfirmed_fills_.empty()) {
        spdlog::debug("[Engine] Step 2: {} fills awaiting confirmation "
                      "(depth requirement: {} blocks)",
                      pending_unconfirmed_fills_.size(), conf_depth);
    }

    // Process only confirmed fills.
    // T4-13: All fill accounting (DB, inventory, PnL, analytics) is
    // consolidated in this single loop.  Each fill is processed atomically:
    // if any sub-step fails, the fill is logged and skipped to prevent
    // partial state corruption.  The processing order is:
    //   1. Build trade record (DB row + cost basis + PnL)
    //   2. VPIN validation (adverse fill classification)
    //   3. Persist to DB (insert_trade + update_offer_status)
    //   4. Update inventory tracker (record_buy / record_sell)
    //   5. Accumulate NHE (hedging efficiency) data
    //   6. Feed PnL tracker
    //   7. Notify strategy (tau reset)
    //   8. Feed market data (whale detector, VPIN)
    //   9. Feed kappa calibrator
    auto& fills = confirmed_fills;

    for (const auto& fill : fills) {
      try {
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

        // [T5-CR1] VPIN validation: check if this fill is adverse.
        // An adverse fill is one where the maker sold below cost basis
        // (realized_pnl < 0) or bought at a price that immediately moved
        // against the position.  For sells, negative realized PnL is a
        // direct signal; for buys, we check if the fill price exceeds
        // the current mid (overpaid).
        // ISO/IEC 5055: well-defined adverse-fill classification.
        bool is_adverse_fill = false;
        if (fill.side == Side::Ask && tr.realized_pnl_mojos < 0) {
            is_adverse_fill = true;
        } else if (fill.side == Side::Bid) {
            double mid = market_data_->get_mid_price(fill.pair_name);
            Mojo mid_mojos = static_cast<Mojo>(std::llround(
                mid * static_cast<double>(kMojosPerXch)));
            if (fill.price > mid_mojos && mid_mojos > 0) {
                is_adverse_fill = true;
            }
        }

        // [T5-CR1] If this fill is adverse, check whether any recent VPIN
        // activation (within kVpinValidationWindow blocks) preceded it.
        // If so, the activation was a true positive -- VPIN correctly
        // predicted elevated toxicity.
        // ISO/IEC 27001:2022: validates signal quality in real time.
        if (is_adverse_fill && !vpin_activation_blocks_.empty()) {
            for (auto it = vpin_activation_blocks_.begin();
                 it != vpin_activation_blocks_.end(); /* advanced below */) {
                if (fill.block_height >= *it &&
                    fill.block_height - *it <= kVpinValidationWindow) {
                    ++vpin_rolling_tp_;
                    ++vpin_rolling_resolved_;
                    it = vpin_activation_blocks_.erase(it);
                    // One adverse fill can validate at most one activation
                    // to avoid inflating the TP count from a single event.
                    break;
                } else {
                    ++it;
                }
            }
        }

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

        // [T5-CR3] Notify the strategy that a fill occurred so that the
        // exponential-decay tau resets to tau_max.  Without this call,
        // last_fill_block_ stays at 0 and tau decays to tau_min permanently.
        // ISO/IEC 5055: strategies_ lookup is O(1); record_fill() is virtual.
        auto fill_strat_it = strategies_.find(fill.pair_name);
        if (fill_strat_it != strategies_.end() && fill_strat_it->second) {
            fill_strat_it->second->record_fill();
        }

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

        // [T4-16] Feed the kappa calibrator with each fill's half-spread.
        if (kappa_calibrator_) {
            double mid = market_data_->get_mid_price(fill.pair_name);
            if (mid > 0.0) {
                const double half_spread_bps =
                    std::abs(static_cast<double>(fill.price) -
                             mid * static_cast<double>(kMojosPerXch))
                    / (mid * static_cast<double>(kMojosPerXch)) * 10'000.0;
                kappa_calibrator_->record_fill(half_spread_bps);
            }
        }
      } catch (const std::exception& ex) {
        // T4-13: If any sub-step fails for this fill, log the error and
        // continue to the next fill.  This prevents a transient failure
        // (e.g., DB write error) from corrupting state by leaving some
        // subsystems updated and others not for the same fill.
        spdlog::error("[Engine] Step 2: fill processing failed for {} "
                      "(block {}): {}",
                      fill.pair_name, fill.block_height, ex.what());
        alerts_->send_alert(AlertRule::ExposureBreach,
            "Fill processing error for " + fill.pair_name + " at block " +
            std::to_string(fill.block_height) + ": " + ex.what());
      }
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

    // [T5-CR1] Expire VPIN activations whose validation window has elapsed
    // without an adverse fill.  These are false positives -- VPIN signalled
    // toxicity but no adverse fill followed within kVpinValidationWindow blocks.
    // ISO/IEC 5055: bounded cleanup prevents unbounded growth of the ring buffer.
    {
        auto it = vpin_activation_blocks_.begin();
        while (it != vpin_activation_blocks_.end()) {
            if (block_height > *it &&
                block_height - *it > kVpinValidationWindow) {
                ++vpin_false_positives_;
                ++vpin_rolling_resolved_;
                it = vpin_activation_blocks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    spdlog::debug("[Engine] Step 2 complete: {} fills processed", fills.size());
    co_return;
}

// Step 3: Update volatility, PIN, regime estimates.
void Engine::step_update_analytics(BlockHeight block_height)
{
    // [T4-15] Adaptive block time: if the block cadence tracker has a
    // stable EMA estimate, propagate it to all volatility estimators so
    // that annualisation uses the observed inter-block interval.
    if (block_cadence_) {
        const double dt_ema = block_cadence_->current_dt_ema();
        if (dt_ema > 0.0) {
            for (auto& [name, vol] : vol_estimators_) {
                vol->set_block_time_seconds(dt_ema);
            }
        }
    }

    // [T4-16] Online kappa calibration: periodically fit the fill-intensity
    // decay parameter from observed fill data.
    if (kappa_calibrator_) {
        const auto& kc_cfg = kappa_calibrator_->config();
        if (kc_cfg.calibration_interval_blocks > 0 &&
            block_height % kc_cfg.calibration_interval_blocks == 0) {
            const double new_kappa = kappa_calibrator_->calibrate();
            spdlog::debug("[Engine] Step 3: kappa calibrated to {:.4f} "
                          "(fills={})", new_kappa,
                          kappa_calibrator_->total_fills());
        }
    }

    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        double mid = market_data_->get_mid_price(pair.name);
        if (mid <= 0.0) continue;  // No valid price data.

        // [T5-CR6] Update the Yang-Zhang volatility estimator using the
        // multi-block candle accumulator.  update_tick() buffers individual
        // prices and produces a proper OHLC candle every
        // candle_aggregation_blocks ticks, recovering meaningful H/L
        // variation that the single-block degenerate path discards.
        auto vol_it = vol_estimators_.find(pair.name);
        if (vol_it != vol_estimators_.end()) {
            vol_it->second->update_tick(mid);
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

        // -- Stablecoin depeg monitoring ------------------------------------
        // Feed the current mid-price to the depeg detector for any pair
        // flagged as a stablecoin.  The detector tracks sustained deviations
        // and transitions through Normal → Warning → Bailed states.
        if (pair.is_stablecoin && depeg_detector_) {
            auto depeg_status = depeg_detector_->update(
                pair.name, mid, block_height);

            if (depeg_status == DepegStatus::Warning) {
                spdlog::warn("[Engine] Step 3: DEPEG WARNING {} price={:.6f} "
                             "peg={:.4f} -- deviation above warn threshold",
                             pair.name, mid, pair.peg_target);
            } else if (depeg_status == DepegStatus::Bailed) {
                spdlog::error("[Engine] Step 3: DEPEG BAIL-OUT {} price={:.6f} "
                              "peg={:.4f} -- pulling all quotes! Suspected "
                              "peg failure (like Stably).",
                              pair.name, mid, pair.peg_target);
            } else if (depeg_status == DepegStatus::SuspectedFailure) {
                spdlog::error("[Engine] Step 3: {} FLAGGED as suspected "
                              "failure -- all quotes suppressed",
                              pair.name);
            }
        }
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

        // Depeg bail-out gate: suppress all quoting for stablecoin pairs
        // where the depeg detector has triggered Bailed or SuspectedFailure.
        // This prevents us from market-making a coin that has lost its peg
        // (e.g. Stably) and accumulating worthless inventory.
        if (depeg_detector_ && depeg_detector_->should_bail(pair_name)) {
            spdlog::error("[Engine] Step 4: {} depeg bail-out active -- "
                          "suppressing all quotes", pair_name);
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
        // Convert from mojos to base-asset display units so that q and q_max
        // are in the same units (T1-12 fix: prevents ~10^12 ratio error).
        double q = static_cast<double>(
            inventory_->net_inventory(AssetId{pair_cfg->base_asset_id}))
            / static_cast<double>(pair_cfg->base_mojos_per_unit);

        // Set cost basis on the per-pair strategy for the never-sell-at-loss
        // constraint.  Uses per-pair min_profit_margin_bps if overridden.
        auto rec = inventory_->get_record(AssetId{pair_cfg->base_asset_id});
        double cost_basis = static_cast<double>(rec.weighted_avg_cost_basis);
        double margin_bps = pair_cfg->min_profit_margin_bps_override.value_or(
            config_.strategy.min_profit_margin_bps);
        strategy.set_cost_basis(cost_basis, margin_bps);

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
        // Convert from mojos to base-asset display units (T1-12 fix).
        double q = static_cast<double>(
            inventory_->net_inventory(AssetId{pair_cfg->base_asset_id}))
            / static_cast<double>(pair_cfg->base_mojos_per_unit);
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

        // [T7-11] Defensive spread widening during regime detector warm-up.
        // When the regime detector has not yet accumulated min_window_size
        // observations, it defaults to Normal regime with unit multipliers.
        // If the bot starts during a momentum period, this exposes us to
        // adverse selection at normal-width spreads.  Apply a defensive 1.3x
        // multiplier until the detector is ready.
        {
            auto warmup_it = strategies_.find(pair_name);
            if (warmup_it != strategies_.end() &&
                !warmup_it->second->is_regime_ready()) {
                constexpr double kWarmupDefensiveMultiplier = 1.3;
                pcs.spread_result.total_spread_bps *= kWarmupDefensiveMultiplier;
                spdlog::debug("[Engine] Step 5: {} regime warm-up defense "
                              "— spread widened by {:.1f}x",
                              pair_name, kWarmupDefensiveMultiplier);
            }
        }

        // ---------------------------------------------------------------
        // Apply whale / VPIN / OFI post-multipliers to the base spread.
        //
        // These signals widen the spread when informed-flow risk is elevated:
        //   whale_mult : direct spread multiplier from whale detector (>=1.0)
        //   vpin_mult  : up to 50% widening at max toxicity (VPIN=1.0)
        //   ofi_mult   : up to 30% widening at max order-flow imbalance
        //
        // COUNTER-RESEARCH NOTE (CR-1, Andersen & Bondarenko 2014):
        //   VPIN may have no incremental predictive power beyond raw
        //   volume and volatility.  It can produce false high-toxicity
        //   signals from correlated noise-trader activity.
        //   VALIDATION GATE (T5-CR1): runtime tracker now records each
        //   activation (vpin_mult > 1.0) and checks whether an adverse
        //   fill follows within kVpinValidationWindow blocks.  Precision
        //   is logged every cycle in Step 10.  If precision < vpin_min_
        //   precision_ after 100+ activations, a warning is emitted.
        //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, §7.
        //
        // COUNTER-RESEARCH NOTE (CR-2, Xu, Lehalle & Alfonsi 2023):
        //   OFI is computed from best-level bid/ask only.  Multi-level
        //   OFI (top 5–10 levels) explains 10–30% more return variance.
        //   TODO: extend ingest_book_snapshot_for_ofi() to accept
        //   multiple book levels for a stronger directional signal.
        //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, §8.
        //
        // ISO/IEC 27001:2022: no secret data; all signals are market-derived.
        // ISO/IEC 5055: multipliers are clamped via their source methods.
        // ---------------------------------------------------------------
        double whale_mult = market_data_->get_whale_spread_multiplier(pair_name);
        double vpin       = market_data_->get_vpin(pair_name);
        double vpin_mult  = 1.0 + vpin * 0.5;   // linear scale [1.0, 1.5]
        double ofi        = std::abs(market_data_->get_normalized_ofi(pair_name));
        double ofi_mult   = 1.0 + ofi * 0.3;    // linear scale [1.0, 1.3]

        // [T5-CR1] Record VPIN activation when the signal is non-trivial.
        // A minimum threshold avoids counting negligible VPIN values that
        // widen the spread by effectively zero, which would dilute the
        // precision metric.  Deduplicate by block: only record one
        // activation per block_height across all pairs to prevent
        // multi-pair inflation (N pairs × 1 block = 1 activation, not N).
        // ISO/IEC 27001:2022: audit-quality signal tracking.
        // ISO/IEC 5055: bounded container via kMaxPendingActivations cap.
        static constexpr double kVpinActivationThreshold = 0.01;
        if (vpin > kVpinActivationThreshold &&
            (vpin_activation_blocks_.empty() ||
             vpin_activation_blocks_.back() != block_height) &&
            vpin_activation_blocks_.size() < kMaxPendingActivations) {
            ++vpin_activations_;
            vpin_activation_blocks_.push_back(block_height);
        }

        pcs.spread_result.total_spread_bps *= whale_mult * vpin_mult * ofi_mult;

        // Apply startup-analysis spread multiplier.
        // This adjusts initial quoting based on the pre-trading observation:
        //   Conservative → 1.5x (wider spreads)
        //   Normal       → 1.0x (no change)
        //   Aggressive   → 0.8x (tighter spreads)
        if (analysis_spread_mult_ != 1.0) {
            pcs.spread_result.total_spread_bps *= analysis_spread_mult_;
        }

        // [T3-06] Graduated staleness spread widening.
        // When data age exceeds 50% of stale_threshold, widen spreads
        // linearly up to 2x at the threshold.  Beyond the threshold,
        // Step 4 already pulls quotes entirely via is_stale().
        double staleness_frac = market_data_->get_staleness_fraction(pair_name);
        if (staleness_frac > 0.5) {
            // Linear ramp from 1.0 at 50% to 2.0 at 100%.
            double staleness_mult = 1.0 + std::min(staleness_frac - 0.5, 0.5) * 2.0;
            pcs.spread_result.total_spread_bps *= staleness_mult;
            spdlog::debug("[Engine] Step 5: {} staleness={:.0f}% spread widened by {:.2f}x",
                          pair_name, staleness_frac * 100.0, staleness_mult);
        }

        pcs.spread_result.half_spread =
            pcs.spread_result.total_spread_bps / 2.0;

        spdlog::debug("[Engine] Step 5: {} spread={:.1f}bps (adverse={:.1f} inv={:.1f} "
                      "cost={:.1f} comp={:.1f} mult={:.2f} "
                      "whale={:.2f} vpin={:.3f} ofi={:.3f} analysis={:.2f})",
                      pair_name,
                      pcs.spread_result.total_spread_bps,
                      pcs.spread_result.s_adverse,
                      pcs.spread_result.s_inventory,
                      pcs.spread_result.s_cost,
                      pcs.spread_result.s_competition,
                      pcs.spread_result.regime_multiplier,
                      whale_mult, vpin, ofi,
                      analysis_spread_mult_);

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

            book_state.fill_rate_24h = db_ ? db_->fill_rate_since_block(
                block_height > 4608 ? block_height - 4608 : 0, 0.30) : 0.30;
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

        // [T4-09] Inventory aging: for positions that have been underwater
        // for an extended period, gradually relax the no-loss floor so that
        // aged capital is not permanently locked.  The effective cost basis
        // is lowered by a discount that grows linearly with position age
        // beyond aging_start_blocks, capped at max_loss_relax_bps.
        //
        // Formula:
        //   age_past_start = max(0, position_age - aging_start_blocks)
        //   discount_bps   = min(max_loss_relax_bps,
        //                        age_past_start * relax_rate_bps_per_block)
        //   effective_basis = cost_basis * (1 - discount_bps / 10000)
        //
        // The no-loss constraint is then applied against the lowered basis,
        // effectively allowing a controlled loss on the ask side.
        Mojo effective_cost_basis = rec.weighted_avg_cost_basis;
        const auto& aging_cfg = config_.inventory_aging;

        if (aging_cfg.enabled && effective_cost_basis > 0) {
            int pos_age = inventory_->position_age_blocks(
                AssetId{pair_cfg->base_asset_id}, block_height);

            if (pos_age > 0 &&
                static_cast<uint32_t>(pos_age) > aging_cfg.aging_start_blocks) {
                // Position is old enough -- check if it is underwater.
                Mojo mid_mojos_age = static_cast<Mojo>(std::llround(
                    mid * static_cast<double>(kMojosPerXch)));

                if (inventory_->is_underwater(
                        AssetId{pair_cfg->base_asset_id}, mid_mojos_age)) {
                    uint32_t age_past_start =
                        static_cast<uint32_t>(pos_age) - aging_cfg.aging_start_blocks;
                    double discount_bps = std::min(
                        aging_cfg.max_loss_relax_bps,
                        static_cast<double>(age_past_start)
                            * aging_cfg.relax_rate_bps_per_block);

                    effective_cost_basis = static_cast<Mojo>(std::llround(
                        static_cast<double>(rec.weighted_avg_cost_basis)
                        * (1.0 - discount_bps / 10'000.0)));

                    if (discount_bps > 0.0) {
                        spdlog::info("[step6] {} Inventory aging: position "
                            "age={} blocks, discount={:.1f} bps, "
                            "basis {} -> {} mojos",
                            pair_name, pos_age, discount_bps,
                            rec.weighted_avg_cost_basis, effective_cost_basis);
                    }
                }
            }
        }

        quote = pre_trade_->enforce_no_loss(
            quote, effective_cost_basis, /*enable=*/true);

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
                mkt_params.fill_rate_per_block = db_
                    ? std::max(0.005, db_->fill_rate_since_block(
                          block_height > 4608 ? block_height - 4608 : 0, 0.03) / 4608.0)
                    : 0.03;
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

        // -- [T7-09] Circuit-breaker rebalance for aged inventory ----------
        // When the base loss_manager is disabled (default), this automatic
        // circuit breaker can still engage it for a single pair when ALL of:
        //   1. circuit_breaker_enabled == true
        //   2. inventory_ratio > circuit_breaker_hard_limit_ratio
        //   3. DriftAnalyzer recommends ManualRebalance or PullOverweight
        //   4. position_age > aging_start_blocks * circuit_breaker_age_multiplier
        // One rebalance per pair per heartbeat; capped at max_loss_bps.
        if (config_.risk.circuit_breaker_enabled
            && loss_manager_ && drift_analyzer_
            && !(loss_manager_->config().enabled))  // Skip if base loss_mgr already handled it
        {
            Mojo mid_mojos_cb = static_cast<Mojo>(std::llround(
                mid * static_cast<double>(kMojosPerXch)));
            double inv_ratio_cb = std::abs(inventory_->inventory_ratio(
                AssetId{pair_cfg->base_asset_id},
                AssetId{pair_cfg->quote_asset_id},
                mid_mojos_cb));

            if (inv_ratio_cb > config_.risk.circuit_breaker_hard_limit_ratio) {
                // Check position age against threshold.
                int pos_age_cb = inventory_->position_age_blocks(
                    AssetId{pair_cfg->base_asset_id}, block_height);
                const uint32_t age_threshold = static_cast<uint32_t>(
                    config_.inventory_aging.aging_start_blocks
                    * config_.risk.circuit_breaker_age_multiplier);

                if (pos_age_cb > 0
                    && static_cast<uint32_t>(pos_age_cb) > age_threshold)
                {
                    // Consult drift analyzer for rebalance recommendation.
                    double sigma_cb = 0.0;
                    {
                        auto vol_cb_it = vol_estimators_.find(pair_name);
                        if (vol_cb_it != vol_estimators_.end()
                            && vol_cb_it->second->is_ready()) {
                            sigma_cb = vol_cb_it->second->get_sigma_annual();
                        }
                    }

                    // Map regime to MarketCondition.
                    MarketCondition mc_cb = MarketCondition::RandomWalk;
                    {
                        auto strat_cb_it = strategies_.find(pair_name);
                        if (strat_cb_it != strategies_.end()
                            && strat_cb_it->second) {
                            auto reg = strat_cb_it->second->current_regime().regime;
                            if (reg == MarketRegime::Momentum)
                                mc_cb = MarketCondition::TrendingUp;
                            else if (reg == MarketRegime::MeanReverting)
                                mc_cb = MarketCondition::MeanReverting;
                        }
                    }

                    auto drift_report = drift_analyzer_->analyze_drift(
                        inv_ratio_cb, sigma_cb, mc_cb);

                    if (drift_report.recommended_action >= RecommendedAction::PullOverweight)
                    {
                        // Build a temporary LossManagerConfig with the CB cap.
                        LossManagerConfig cb_cfg;
                        cb_cfg.enabled = true;
                        cb_cfg.max_acceptable_loss_bps =
                            config_.risk.circuit_breaker_max_loss_bps;
                        cb_cfg.min_spread_recovery_blocks = 200.0;
                        cb_cfg.target_inventory_ratio = 0.50;

                        StrategicLossManager cb_loss_mgr(cb_cfg);

                        std::unordered_map<AssetId, Mojo> price_map_cb;
                        price_map_cb[pair_cfg->base_asset_id] = mid_mojos_cb;

                        MarketParams mkt_cb{};
                        {
                            auto vol_cb_it2 = vol_estimators_.find(pair_name);
                            mkt_cb.sigma = (vol_cb_it2 != vol_estimators_.end()
                                            && vol_cb_it2->second->is_ready())
                                ? vol_cb_it2->second->get_sigma_block()
                                : 0.0;
                        }
                        mkt_cb.fill_rate_per_block = db_
                            ? std::max(0.005, db_->fill_rate_since_block(
                                  block_height > 4608 ? block_height - 4608 : 0,
                                  0.03) / 4608.0)
                            : 0.03;
                        mkt_cb.spread_bps =
                            pcs.spread_result.total_spread_bps / 2.0;
                        mkt_cb.vpin = market_data_->get_vpin(pair_name);
                        {
                            auto s_cb_it = strategies_.find(pair_name);
                            mkt_cb.variance_ratio =
                                (s_cb_it != strategies_.end() && s_cb_it->second)
                                ? s_cb_it->second->current_regime().variance_ratio
                                : 1.0;
                        }
                        mkt_cb.current_block = block_height;

                        auto cb_decision = cb_loss_mgr.should_rebalance_at_loss(
                            AssetId{pair_cfg->base_asset_id},
                            mid_mojos_cb,
                            cb_cfg.target_inventory_ratio,
                            *inventory_,
                            price_map_cb,
                            mkt_cb);

                        if (cb_decision.should_take_loss
                            && cb_decision.loss_bps <=
                               config_.risk.circuit_breaker_max_loss_bps)
                        {
                            spdlog::warn("[step6] {} CIRCUIT-BREAKER rebalance: "
                                "inv_ratio={:.2f} age={} drift_action={} "
                                "loss={:.1f}bps breakeven={:.0f}blk "
                                "rationale='{}'",
                                pair_name, inv_ratio_cb, pos_age_cb,
                                to_string(drift_report.recommended_action),
                                cb_decision.loss_bps,
                                cb_decision.blocks_to_breakeven,
                                cb_decision.rationale);

                            quote = pre_trade_->enforce_no_loss(
                                quote, rec.weighted_avg_cost_basis,
                                /*enable=*/false);
                        } else {
                            spdlog::info("[step6] {} circuit-breaker: hold "
                                "(loss={:.1f}bps > cap={:.0f}bps or EV negative)",
                                pair_name, cb_decision.loss_bps,
                                config_.risk.circuit_breaker_max_loss_bps);
                        }
                    }
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
void Engine::step_generate_ladder([[maybe_unused]] BlockHeight block_height)
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

    // -- T4-03: Dynamic fee selection ----------------------------------------
    // Ask the FeeTracker for the recommended fee and push it into the
    // OfferManager so that every subsequent create/cancel uses the
    // optimal fee.  When fee tracking is disabled, the static
    // offer_fee_mojos from StrategyConfig is used unchanged.
    const std::uint64_t recommended_fee = fee_tracker_->get_recommended_fee(
        config_.strategy.offer_fee_mojos, block_height);

    if (recommended_fee == 0 && fee_tracker_->enabled()) {
        spdlog::warn("[Engine] Step 8: fee budget exhausted -- "
                     "skipping all offer posting this block");
        co_return;
    }

    offer_mgr_->set_dynamic_fee(recommended_fee);

    for (auto& [pair_name, pcs] : cycle_) {
        if (!pcs.quote_valid || pcs.ladder.empty()) continue;

        // [T3-24] Final gate: do not post offers if market data was invalid.
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
            // T4-03: Record cancel fees in the tracker.
            if (fee_tracker_->enabled()) {
                fee_tracker_->record_fee(
                    static_cast<std::uint64_t>(cancelled) * recommended_fee,
                    block_height);
            }
        }

        // Find the PairConfig for this pair (O(1) map lookup).
        const PairConfig* pair_cfg = find_pair_config(pair_name);
        if (!pair_cfg) continue;

        // -- T4-03: Fee-vs-gain gating per tier ------------------------------
        // Estimate expected gain for each tier and filter out tiers where
        // the fee exceeds the configured ratio of expected gain.
        // mid_price for the gain calculation.
        const Mojo mid = (pcs.risk_quote.bid_price + pcs.risk_quote.ask_price) / 2;

        std::vector<TierQuote> fee_filtered_tiers;
        if (fee_tracker_->enabled() && mid > 0) {
            fee_filtered_tiers.reserve(pcs.ladder.size());
            for (const auto& tier : pcs.ladder) {
                // Expected gain = |tier_price - mid| * size / mid
                // This gives gain in base-mojos (proportional, same scale
                // as fee when converted to XCH).
                const double price_diff = std::abs(
                    static_cast<double>(tier.price) - static_cast<double>(mid));
                const double gain_fraction = price_diff / static_cast<double>(mid);
                // Convert to XCH-equivalent mojos for comparison with fee.
                const double gain_mojos =
                    gain_fraction * static_cast<double>(tier.size)
                    * static_cast<double>(pair_cfg->base_mojos_per_unit)
                    / static_cast<double>(kMojosPerXch);

                const auto expected_gain = static_cast<std::uint64_t>(
                    std::max(0.0, gain_mojos));

                if (fee_tracker_->should_post_offer(
                        expected_gain, recommended_fee, block_height)) {
                    fee_filtered_tiers.push_back(tier);
                } else {
                    spdlog::info("[Engine] Step 8: {} {} tier {} skipped "
                                 "(fee {} > {:.0f}% of gain {})",
                                 pair_name,
                                 (tier.side == Side::Bid) ? "bid" : "ask",
                                 tier.tier_index,
                                 recommended_fee,
                                 config_.fees.fee_to_gain_max_ratio * 100.0,
                                 expected_gain);
                }
            }
        } else {
            // Fee tracking disabled or mid is 0 -- pass all tiers through.
            fee_filtered_tiers = pcs.ladder;
        }

        if (fee_filtered_tiers.empty()) {
            spdlog::info("[Engine] Step 8: {} all tiers filtered by "
                         "fee-vs-gain gating", pair_name);
            continue;
        }

        // [T1-03] co_await post_quotes directly instead of use_future.
        int posted = co_await offer_mgr_->post_quotes(
            *pair_cfg, fee_filtered_tiers, block_height);

        // T4-03: Record posting fees in the tracker.
        if (fee_tracker_->enabled() && posted > 0) {
            fee_tracker_->record_fee(
                static_cast<std::uint64_t>(posted) * recommended_fee,
                block_height);
        }
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

            for (const auto& etq : fee_filtered_tiers) {
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

        spdlog::info("[Engine] Step 8: posted {} offers for {} (cancelled {}, "
                     "fee {} mojos/offer)",
                     posted, pair_name, cancelled, recommended_fee);
    }

    // -- [T4-11] Periodic offer-state reconciliation -------------------------
    // Every reconciliation_interval_blocks, perform a full comparison of
    // the in-memory pending-offer map against the authoritative wallet RPC
    // state.  Corrects orphans, phantoms, and missed status transitions.
    const uint32_t recon_interval = config_.strategy.reconciliation_interval_blocks;
    if (recon_interval > 0 &&
        block_height >= last_reconciliation_block_ + recon_interval) {
        try {
            int corrections = co_await offer_mgr_->reconcile_offers();
            last_reconciliation_block_ = block_height;
            if (corrections > 0) {
                spdlog::info("[Engine] Step 8: offer reconciliation corrected "
                             "{} discrepancies", corrections);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[Engine] Step 8: offer reconciliation failed: {}",
                         e.what());
        }
    }

    co_return;
}

// Step 9: Check arbitrage opportunities.
void Engine::step_check_arbitrage([[maybe_unused]] BlockHeight block_height)
{
    // -- 9a: Legacy MarketDataFeed CEX-DEX signal check ----------------------
    // The MarketDataFeed emits ArbitrageSignals for CEX-DEX divergences
    // detected during step 1.  Log them for visibility.
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        auto signal = market_data_->get_latest_arb_signal(pair.name);
        if (signal) {
            spdlog::info("[Engine] Step 9: CEX-DEX signal for {}: "
                         "dex={:.4f} cex={:.4f} divergence={:.1f}bps dir={}",
                         pair.name,
                         signal->dex_price, signal->cex_price,
                         signal->divergence_bps,
                         to_string(signal->direction));
        }
    }

    // -- 9b: Full ArbitrageDetector scan -------------------------------------
    // Feed current mid-prices and depth from this block's market data into
    // the detector, then run all scans (CEX-DEX, cross-DEX, triangular,
    // cross-bridge).
    if (!config_.arbitrage.enabled || !arb_detector_) {
        return;
    }

    // Build pair-price map from the current cycle state.
    // Depth data is not yet available from MarketDataFeed (Phase 2), so
    // the depth map stays empty — scan_triangular will use its fallback
    // max_position_size cap.
    PairPriceMap pair_prices;
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        auto it = cycle_.find(pair.name);
        if (it == cycle_.end() || !it->second.market_data_valid) continue;

        // Get mid-price from the market data feed (double, quote-per-base).
        const double mid = market_data_->get_mid_price(pair.name);
        if (mid > 0.0) {
            pair_prices[pair.name] = mid;
        }
    }

    if (pair_prices.empty()) {
        return;  // no valid market data this block
    }

    // Feed data to detector.
    arb_detector_->set_pair_prices(pair_prices);
    // Depth data: Phase 2 will populate pair depths from dexie order book
    // snapshots for liquidity-aware sizing.  For now the detector falls
    // back to the max_position_size cap.

    // Run all scans.
    auto opportunities = arb_detector_->scan_all();

    if (opportunities.empty()) {
        spdlog::debug("[Engine] Step 9: no arbitrage opportunities detected "
                      "(block {})", block_height);
        return;
    }

    // Log all detected opportunities.
    spdlog::info("[Engine] Step 9: {} arbitrage opportunities detected "
                 "(block {})", opportunities.size(), block_height);

    for (const auto& opp : opportunities) {
        spdlog::info("[Engine] Step 9: {} | edge={:.1f}bps "
                     "profit={:.6f} conf={:.3f} urgency={}blk | {}",
                     to_string(opp.type), opp.edge_bps,
                     opp.estimated_profit, opp.confidence,
                     opp.urgency_blocks, opp.description);

        // Phase 2: execute the arbitrage trade.
        // TODO: ArbitrageExecutor that takes the top opportunity and
        //       constructs + submits the multi-leg spend bundle.
    }
}

// Step 10: Run hedging layer (compute skew, NHE).
// [T3-08] Now calls compute_nhe() with actual fill data from Step 2
// accumulators (nhe_net_inventory_change_, nhe_total_volume_) and
// alerts when NHE drops below the 0.70 target.
void Engine::step_run_hedging([[maybe_unused]] BlockHeight block_height)
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

    // [T5-CR1] VPIN signal quality reporting (Andersen & Bondarenko 2014).
    // Rolling precision = rolling_tp / rolling_resolved.  Using a rolling
    // window (instead of lifetime counters) allows precision to recover
    // after early false-positive bursts, matching ISO/IEC 25000 requirements
    // for actionable operational metrics.
    // ISO/IEC 27001:2022: continuous signal-quality monitoring.
    if (vpin_rolling_resolved_ > 0) {
        double vpin_precision = static_cast<double>(vpin_rolling_tp_)
                              / static_cast<double>(vpin_rolling_resolved_);
        spdlog::info("[Engine] Step 10: VPIN signal quality: precision={:.2f} "
                     "({}/{} resolved, {} pending, {} lifetime activations)",
                     vpin_precision, vpin_rolling_tp_, vpin_rolling_resolved_,
                     vpin_activation_blocks_.size(), vpin_activations_);

        // Warn when precision is below the minimum threshold after burn-in.
        // This suggests the VPIN multiplier is widening spreads without
        // corresponding adverse-fill prediction, costing competitiveness.
        // ISO/IEC 5055: guard against premature warning during burn-in.
        if (vpin_activations_ >= kVpinBurnIn &&
            vpin_precision < vpin_min_precision_) {
            spdlog::warn("[Engine] Step 10: VPIN precision {:.2f} < "
                         "min threshold {:.2f} after {} activations -- "
                         "consider reducing VPIN weight "
                         "(Andersen & Bondarenko 2014: VPIN may lack "
                         "incremental power beyond volume+volatility)",
                         vpin_precision, vpin_min_precision_,
                         vpin_activations_);
        }

        // Reset rolling counters when they exceed the window to keep the
        // metric responsive.  Halve both to preserve the current ratio.
        if (vpin_rolling_resolved_ >= kVpinRollingWindow) {
            vpin_rolling_tp_       /= 2;
            vpin_rolling_resolved_ /= 2;
        }
    }

    spdlog::debug("[Engine] Step 10: hedging layer complete; {} positions tracked, "
                  "NHE={:.3f}", positions.size(), nhe);
}

// Step 11: Update PnL attribution.
void Engine::step_update_pnl(BlockHeight block_height)
{
    // Mark-to-market all positions.
    pnl_->mark_to_market(
        // get_price callback: return mid-price in mojos for a pair/asset.
        [this](const std::string& pair, [[maybe_unused]] const std::string& asset) -> Mojo {
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

            alerts_->send_alert(AlertRule::CircuitBreaker,
                "Global max-drawdown circuit breaker triggered: drawdown " +
                std::to_string(drawdown_frac * 100.0) + "% exceeds threshold " +
                std::to_string(max_drawdown_pct_ * 100.0) +
                "% -- engine PAUSED.  Manual intervention required.");
        }
    }

    // [T3-36] Rolling time-window loss circuit breaker.
    //
    // Maintains a deque of (block_height, total_pnl) snapshots capped at
    // loss_window_blocks entries.  On each cycle:
    //   1. Append the current (block_height, total_pnl) to the back.
    //   2. Pop front entries older than loss_window_blocks blocks.
    //   3. Compute window_loss = front_pnl - current_pnl.
    //   4. If window_loss exceeds the configured threshold, pause.
    //
    // The loss threshold is expressed in mojos: it equals
    //   |peak_pnl_hwm_| * max_window_loss_bps / 10 000
    // anchored to the all-time peak so that the threshold scales with the
    // bot's trading volume.  When peak_pnl_hwm_ <= 0 we use the absolute
    // window loss in mojos compared to the bps threshold applied to a
    // nominal 1 XCH (1e12 mojos) to avoid a zero denominator.
    //
    // A configured max_window_loss_bps of 0 disables this check entirely.
    //
    // ISO/IEC 27001:2022: time-windowed monitoring catches slow loss spirals
    //   that individual HWM drawdown or flash-crash checks may miss.
    // ISO/IEC 5055: deque bounded by loss_window_blocks; no UB division.
    if (config_.risk.max_window_loss_bps > 0.0
            && state_->status() == BotStatus::Running) {

        // 1. Append current snapshot.
        pnl_window_.push_back({block_height, total.total_pnl});

        // 2. Trim entries that fall outside the rolling window.
        //    Entries are ordered by ascending block_height; pop from front.
        //    We keep only entries whose age is strictly less than
        //    loss_window_blocks (i.e., within the window).  An entry is
        //    considered stale when block_height - entry_block >= window_size.
        while (pnl_window_.size() > 1) {
            const BlockHeight oldest = pnl_window_.front().first;
            if (block_height - oldest >= config_.risk.loss_window_blocks) {
                pnl_window_.pop_front();
            } else {
                break;
            }
        }

        // 3. Compute window_loss (positive = PnL decreased over the window).
        if (pnl_window_.size() >= 2) {
            const Mojo window_start_pnl = pnl_window_.front().second;
            const Mojo window_loss      = window_start_pnl - total.total_pnl;

            // Compute the threshold in mojos.
            // Anchored to |peak_pnl_hwm_| when positive; fall back to 1 XCH.
            const Mojo anchor   = (peak_pnl_hwm_ > 0)
                                ? peak_pnl_hwm_
                                : kMojosPerXch;
            const Mojo threshold_mojos = static_cast<Mojo>(
                static_cast<double>(anchor)
                * config_.risk.max_window_loss_bps / 10'000.0);

            // 4. Fire if window_loss exceeds the threshold.
            if (window_loss > 0 && threshold_mojos > 0
                    && window_loss > threshold_mojos) {

                const BlockHeight window_actual =
                    block_height - pnl_window_.front().first;

                spdlog::error("[Engine] Step 13: ROLLING-WINDOW CIRCUIT BREAKER "
                              "-- loss={} mojos over {} blocks "
                              "> threshold={} mojos ({:.1f} bps) "
                              "-- transitioning to Paused state",
                              window_loss, window_actual,
                              threshold_mojos,
                              config_.risk.max_window_loss_bps);

                state_->set_status(BotStatus::Paused);

                alerts_->send_alert(AlertRule::CircuitBreaker,
                    "Rolling-window circuit breaker triggered: lost " +
                    std::to_string(window_loss) + " mojos in " +
                    std::to_string(window_actual) + " blocks (limit " +
                    std::to_string(static_cast<int>(config_.risk.max_window_loss_bps)) +
                    " bps = " + std::to_string(threshold_mojos) +
                    " mojos) -- engine PAUSED.  Manual intervention required.");
            }
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
    // Determine operating mode: full_node, wallet_only, or auto-detect.
    if (config_.chia.mode == ChiaMode::WalletOnly) {
        wallet_only_mode_ = true;
        spdlog::info("[Engine] Configured for wallet-only mode "
                     "(no full node required)");
    } else if (config_.chia.mode == ChiaMode::FullNode) {
        wallet_only_mode_ = false;
    } else {
        // Auto-detect: try full node first, fall back to wallet-only.
        wallet_only_mode_ = false;  // optimistic
    }

    // Open the Chia wallet RPC (always required).
    co_await wallet_->open();
    spdlog::info("[Engine] Connected to Chia wallet at {}:{}",
                 config_.chia.wallet_host, config_.chia.wallet_port);

    // Open the Chia full node RPC (skip in wallet-only mode).
    if (!wallet_only_mode_) {
        try {
            co_await full_node_->open();
            // Verify reachability with a quick height query.
            co_await full_node_->get_block_height();
            spdlog::info("[Engine] Connected to Chia full node at {}:{}",
                         config_.chia.full_node_host,
                         config_.chia.full_node_port);
        } catch (const std::exception& ex) {
            if (config_.chia.mode == ChiaMode::Auto) {
                spdlog::warn("[Engine] Full node unreachable ({}); "
                             "falling back to wallet-only mode", ex.what());
                wallet_only_mode_ = true;
                full_node_->close();
            } else {
                // FullNode mode: failure is fatal.
                throw;
            }
        }
    }

    if (wallet_only_mode_) {
        // Verify wallet sync status.
        try {
            auto sync = co_await wallet_->get_sync_status();
            bool synced = sync.contains("synced")
                          && sync["synced"].is_boolean()
                          && sync["synced"].get<bool>();
            if (!synced) {
                spdlog::warn("[Engine] Wallet is not fully synced yet; "
                             "block heights may be stale until sync completes");
            }
            auto height = co_await wallet_->get_height_info();
            spdlog::info("[Engine] Wallet-only mode active — wallet synced "
                         "height: {}", height);
        } catch (const std::exception& ex) {
            spdlog::warn("[Engine] Could not verify wallet sync status: {}",
                         ex.what());
        }
    }

    // Open the dexie REST client (synchronous HTTP -- no coroutine needed).
    dexie_->open();
    spdlog::info("[Engine] Connected to dexie API at {}",
                 config_.dexie.api_base);

    // Open the CoinGecko client if enabled.
    if (coingecko_) {
        coingecko_->open();
        spdlog::info("[Engine] CoinGecko price reference enabled "
                     "(polling every {}ms)", config_.coingecko.polling_interval_ms);
    }

    co_return;
}

void Engine::close_connections()
{
    if (!wallet_only_mode_) {
        full_node_->close();
    }
    wallet_->close();
    dexie_->close();
    if (coingecko_) {
        coingecko_->close();
    }
    spdlog::info("[Engine] All connections closed");
}

}  // namespace xop
