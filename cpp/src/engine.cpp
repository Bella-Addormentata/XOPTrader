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
#include <cstdlib>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace xop {

namespace asio = boost::asio;

namespace {

int score_offer_competitiveness(Side side,
                                Mojo price,
                                Mojo best_bid,
                                Mojo best_ask)
{
    if (price <= 0) return 0;

    if (best_bid > 0 && best_ask > best_bid) {
        if (side == Side::Bid && price >= best_ask) return 1;
        if (side == Side::Ask && price <= best_bid) return 1;

        const double spread = static_cast<double>(best_ask - best_bid);
        if (side == Side::Bid) {
            if (price >= best_bid) return 10;
            const double widths_below_best =
                static_cast<double>(best_bid - price) / spread;
            return std::clamp(
                10 - static_cast<int>(std::llround(widths_below_best * 4.0)),
                1, 10);
        }

        if (price <= best_ask) return 10;
        const double widths_above_best =
            static_cast<double>(price - best_ask) / spread;
        return std::clamp(
            10 - static_cast<int>(std::llround(widths_above_best * 4.0)),
            1, 10);
    }

    const Mojo same_side_best = (side == Side::Bid) ? best_bid : best_ask;
    if (same_side_best <= 0) return 0;
    if ((side == Side::Bid && price >= same_side_best)
        || (side == Side::Ask && price <= same_side_best)) {
        return 9;
    }

    const double bps_off_best =
        std::abs(static_cast<double>(price - same_side_best))
        / static_cast<double>(same_side_best) * 10000.0;
    if (bps_off_best <= 5.0) return 8;
    if (bps_off_best <= 10.0) return 7;
    if (bps_off_best <= 25.0) return 6;
    if (bps_off_best <= 50.0) return 4;
    return 2;
}

Mojo compute_queue_ahead_mojos(Side side,
                               Mojo price,
                               const std::vector<CompetingOffer>& competing_offers)
{
    Mojo queue_ahead_mojos = 0;
    for (const auto& offer : competing_offers) {
        if (offer.side != side) continue;

        const bool ahead_of_us =
            (side == Side::Bid) ? (offer.price >= price) : (offer.price <= price);
        if (ahead_of_us) {
            queue_ahead_mojos += offer.size;
        }
    }
    return queue_ahead_mojos;
}

int score_queue_position(Mojo queue_ahead_mojos, Mojo our_size_mojos)
{
    if (our_size_mojos <= 0) return 0;
    if (queue_ahead_mojos <= 0) return 10;

    const double queue_ratio = static_cast<double>(queue_ahead_mojos)
        / static_cast<double>(our_size_mojos);
    if (queue_ratio <= 0.25) return 9;
    if (queue_ratio <= 0.50) return 8;
    if (queue_ratio <= 1.00) return 7;
    if (queue_ratio <= 2.00) return 6;
    if (queue_ratio <= 4.00) return 5;
    if (queue_ratio <= 8.00) return 4;
    if (queue_ratio <= 12.00) return 3;
    if (queue_ratio <= 20.00) return 2;
    return 1;
}

int score_execution_quality(int competitiveness_score, int queue_ahead_score)
{
    if (competitiveness_score <= 0) return queue_ahead_score;
    if (queue_ahead_score <= 0) return competitiveness_score;

    const double weighted_score = 0.7 * static_cast<double>(competitiveness_score)
        + 0.3 * static_cast<double>(queue_ahead_score);
    return std::clamp(static_cast<int>(std::lround(weighted_score)), 1, 10);
}

}  // namespace

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
    , drawdown_grace_remaining_(config.risk.drawdown_grace_blocks)
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
        fn_cfg.verify_ssl       = config_.chia.verify_ssl;
        full_node_ = std::make_shared<rpc::ChiaFullNodeRPC>(ioc_, fn_cfg);
    }

    // Build wallet RPC config from AppConfig.
    rpc::ChiaRPCConfig wal_cfg;
    wal_cfg.host = config_.chia.wallet_host;
    wal_cfg.port = config_.chia.wallet_port;
    wal_cfg.tls.cert_path    = config_.chia.wallet_cert_path;
    wal_cfg.tls.key_path     = config_.chia.wallet_key_path;
    wal_cfg.tls.ca_cert_path = config_.chia.ca_cert_path;
    wal_cfg.verify_ssl       = config_.chia.verify_ssl;
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
    md_cfg.amm_blend_weight             = config_.strategy.amm_blend_weight;
    md_cfg.amm_freshness_threshold_sec  = 300.0;  // 5 min default
    md_cfg.orderbook_mid_enabled        = config_.market_data.orderbook_mid_enabled;
    md_cfg.orderbook_mid_depth          = config_.market_data.orderbook_mid_depth;
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
    sp_cfg.high_vol_multiplier = config_.strategy.high_vol_multiplier;
    spread_opt_ = std::make_unique<SpreadOptimizer>(sp_cfg);

    // Per-pair liquidity engines ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â use per-pair tier overrides when present.
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;
        LiquidityConfig liq_cfg;
        liq_cfg.tier_spacing_bps =
            pair.tier_spacing_bps_override.value_or(
                config_.strategy.tier_spacing_bps);
        liq_cfg.tier_size_pct    =
            pair.tier_size_pct_override.value_or(
                config_.strategy.tier_size_pct);
        // When per-pair overrides provide fewer tiers than the global
        // num_tiers, derive the count from the override vectors to
        // prevent out-of-bounds access in build_raw_ladder.
        liq_cfg.num_tiers = static_cast<uint32_t>(std::min(
            liq_cfg.tier_spacing_bps.size(),
            liq_cfg.tier_size_pct.size()));
        if (liq_cfg.num_tiers == 0) {
            liq_cfg.num_tiers = config_.strategy.num_tiers;
        }
        liq_cfg.offer_ttl_blocks = config_.strategy.offer_ttl_blocks;

        // Gap-aware dynamic tier spacing.
        liq_cfg.gap_aware_spacing  = config_.strategy.gap_aware_spacing;
        liq_cfg.min_gap_bps        = config_.strategy.min_gap_bps;
        liq_cfg.max_gap_scan_bps   = config_.strategy.max_gap_scan_bps;
        liq_cfg.gap_blend_factor   = config_.strategy.gap_blend_factor;

        // Competitive anchor pricing.
        liq_cfg.competitive_anchor_enabled           = config_.strategy.competitive_anchor_enabled;
        liq_cfg.competitive_anchor_max_distance_bps  = config_.strategy.competitive_anchor_max_distance_bps;
        liq_cfg.competitive_anchor_stride_bps        = config_.strategy.competitive_anchor_stride_bps;

        // Adverse-selection-aware tier sizing.
        liq_cfg.adverse_selection_sizing           = config_.strategy.adverse_selection_sizing;
        liq_cfg.adverse_selection_decay            = config_.strategy.adverse_selection_decay;
        liq_cfg.adverse_selection_sigma_threshold  = config_.strategy.adverse_selection_sigma_threshold;

        // Fill-rate-weighted adaptive tier sizing.
        liq_cfg.fill_rate_sizing          = config_.strategy.fill_rate_sizing;
        liq_cfg.fill_rate_blend           = config_.strategy.fill_rate_blend;
        liq_cfg.fill_rate_lookback_hours  = config_.strategy.fill_rate_lookback_hours;
        liq_cfg.fill_rate_min_pct         = config_.strategy.fill_rate_min_pct;

        // Inventory skew strength -- must be wired from config.
        liq_cfg.phi = config_.strategy.phi;

        // Stablecoin overrides: skip adverse-selection and gap-aware
        // adjustments that widen spreads counterproductively.
        if (pair.stablecoin_skip_gap_aware) {
            liq_cfg.gap_aware_spacing = false;
        }
        if (pair.stablecoin_flat_sizing) {
            liq_cfg.adverse_selection_sizing = false;
        }

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
        arb_cfg.crossed_book_enabled      = config_.arbitrage.crossed_book_enabled;
        arb_cfg.crossed_book_min_edge_bps = config_.arbitrage.crossed_book_min_edge_bps;
        arb_cfg.crossed_book_max_take_xch = config_.arbitrage.crossed_book_max_take_xch;
        arb_cfg.cross_stable_arb_enabled  = config_.arbitrage.cross_stable_arb_enabled;
        arb_cfg.cross_stable_min_edge_bps = config_.arbitrage.cross_stable_min_edge_bps;
        arb_cfg.cross_stable_max_take_xch = config_.arbitrage.cross_stable_max_take_xch;
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

    // -- On-chain reconciler (full-node ground truth verification) -----------
    if (full_node_) {
        on_chain_reconciler_ = std::make_unique<OnChainReconciler>(
            full_node_, wallet_, state_);
        spdlog::info("[Engine] On-chain reconciler initialized");
    }

    // -- New strategy modules -----------------------------------------------
    order_book_tactician_ = std::make_unique<OrderBookTactician>(
        OrderBookTacticsConfig{});  // Default config for now

    strategy_portfolio_ = std::make_unique<StrategyPortfolio>(
        PortfolioConfig{});  // Default config with beta=2.0

    // ChiaEdgeOptimizer implements StrategyBase ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â construct with default config
    chia_edge_ = std::make_unique<ChiaEdgeOptimizer>(
        ChiaEdgeConfig{});

    coin_age_quoting_ = std::make_unique<CoinAgeWeightedQuoting>(
        CoinAgeConfig{});

    block_cadence_ = std::make_unique<BlockCadenceAdaptiveSpread>(
        BlockCadenceConfig{});

    mempool_sentinel_ = std::make_unique<MempoolSentinelStrategy>(
        MempoolSentinelConfig{});

    // -- Dynamic market allocator -------------------------------------------
    market_allocator_ = std::make_unique<MarketAllocator>(
        config_.market_allocator, market_data_.get(), db_.get());
    if (config_.market_allocator.enabled) {
        spdlog::info("[Engine] Market allocator enabled: eval every {} blocks, "
                     "alloc range [{:.0f}%, {:.0f}%]",
                     config_.market_allocator.eval_interval_blocks,
                     config_.market_allocator.min_alloc_pct * 100.0,
                     config_.market_allocator.max_alloc_pct * 100.0);
    }

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

    // -- Pause flag file path ------------------------------------------------
    // Resolve once: place pause.flag next to the database file so the GUI
    // (which knows the config directory) can create/remove it.
    {
        namespace fs = std::filesystem;
        fs::path db_dir = fs::path(config_.database.path).parent_path();
        if (db_dir.empty()) db_dir = fs::current_path();
        pause_flag_path_ = db_dir / "pause.flag";
        spdlog::info("[Engine] Pause flag path: {}", pause_flag_path_.string());
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
                auto shutdown_cancelled = co_await offer_mgr_->cancel_all();
                spdlog::info("[Engine] All outstanding offers cancelled");
                // [T5-02] Persist cancellation status to database with retry.
                // Shutdown is our last chance to update the audit trail; stale
                // "pending" records cause ghost offers on next startup.  Retry
                // up to 3 times with short delays before giving up.
                for (const auto& oid : shutdown_cancelled) {
                    bool persisted = false;
                    for (int attempt = 0; attempt < 3 && !persisted; ++attempt) {
                        try {
                            db_->update_offer_status(oid, "cancelled", 0,
                                                    "shutdown");
                            persisted = true;
                        } catch (const std::exception& e) {
                            spdlog::warn("[Engine] shutdown update_offer_status "
                                         "attempt {}/3 failed for {}: {}",
                                         attempt + 1, oid.substr(0, 12),
                                         e.what());
                        }
                    }
                    if (!persisted) {
                        spdlog::error("[Engine] CRITICAL: offer {} stuck as "
                                      "'pending' in DB after 3 retries -- "
                                      "manual cleanup required",
                                      oid.substr(0, 12));
                    }
                }
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

    // -- Startup offer reconciliation ----------------------------------------
    // Scan the wallet for any PENDING_ACCEPT offers left over from a
    // previous run.  Cross-reference against the DB's pending records:
    //   - Known offers (in DB) are restored into State for tracking.
    //   - Unknown offers (orphans) are evaluated using cost-aware analysis
    //     (GuÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â©ant-Lehalle 2013, Gao-Wang 2020): well-priced orphans are
    //     ADOPTED to preserve market presence; mispriced ones are cancelled.
    // This runs once before any trading begins.
    if (offer_mgr_ && db_) {
        try {
            // Fetch current block height for orphan age evaluation.
            BlockHeight startup_block = 0;
            try {
                if (full_node_) {
                    startup_block = static_cast<BlockHeight>(
                        co_await full_node_->get_block_height());
                } else if (wallet_) {
                    startup_block = static_cast<BlockHeight>(
                        co_await wallet_->get_height_info());
                }
            } catch (const std::exception& e) {
                spdlog::debug("[Engine] Could not fetch block height for "
                              "orphan evaluation: {}", e.what());
            }

            // Load what the DB remembers as pending.
            auto db_pending = db_->query_pending_offers();
            std::unordered_set<std::string> known_ids;
            known_ids.reserve(db_pending.size());
            for (const auto& rec : db_pending) {
                known_ids.insert(rec.offer_id);
            }
            spdlog::info("[Engine] Startup reconcile: {} pending offers in DB",
                         known_ids.size());

            auto orphans = co_await offer_mgr_->startup_reconcile(
                known_ids, startup_block);

            // Mark cancelled orphans in the DB.  Adopted orphans were
            // already upserted into State by startup_reconcile; persist
            // them to the DB as well so they survive the next restart.
            for (const auto& oid : orphans) {
                try {
                    db_->update_offer_status(oid, "cancelled", 0,
                                            "startup_orphan");
                } catch (const std::exception& e) {
                    spdlog::debug("[Engine] startup_reconcile update_offer_status "
                                 "failed for {}: {}",
                                 oid.substr(0, 12), e.what());
                }
            }

            // Persist adopted orphans so they show up as DB-pending on
            // the next restart (preventing re-evaluation churn).
            {
                auto adopted_offers = state_->get_all_offers();
                for (const auto& po : adopted_offers) {
                    // Skip offers already in the DB (they were restored above).
                    if (known_ids.count(po.offer_id)) continue;
                    try {
                        DbOfferRecord rec;
                        rec.offer_id      = po.offer_id;
                        rec.pair_name     = po.pair_name;
                        rec.side          = (po.side == Side::Bid) ? "bid" : "ask";
                        rec.price_mojos   = po.price;
                        rec.size_mojos    = po.size;
                        rec.tier          = static_cast<int>(po.tier);
                        rec.status        = "pending";
                        rec.created_block = po.created_at_block;
                        rec.fee_mojos     = po.fee_mojos;
                        db_->insert_offer(rec);
                    } catch (const std::exception& e) {
                        spdlog::debug("[Engine] Failed to persist adopted "
                                      "orphan {}: {}",
                                      po.offer_id.substr(0, 12), e.what());
                    }
                }
            }

            // Restore known offers into State so the engine can track them.
            for (const auto& rec : db_pending) {
                if (!known_ids.count(rec.offer_id)) {
                    continue;  // Was cancelled as orphan somehow.
                }
                PendingOffer po;
                po.offer_id        = rec.offer_id;
                po.pair_name       = rec.pair_name;
                po.side            = (rec.side == "bid") ? Side::Bid : Side::Ask;
                po.price           = rec.price_mojos;
                po.size            = rec.size_mojos;
                po.tier            = static_cast<std::uint8_t>(rec.tier);
                po.created_at_block = rec.created_block;
                po.fee_mojos       = rec.fee_mojos;
                state_->upsert_offer(po);
            }

            if (!db_pending.empty()) {
                spdlog::info("[Engine] Restored {} pending offers from DB into State",
                             db_pending.size());
            }

            // -- Prune stuck transactions ------------------------------------
            // After offer reconciliation, scan wallet transaction lists for
            // transactions that were created but never broadcast (no spend
            // bundle).  These occur when the wallet daemon selects an
            // already-spent coin for a new offer, creating a local record
            // that can never confirm.  Clear them to free up pending balance.
            {
                std::vector<std::int64_t> wallet_ids;
                for (const auto& pair : config_.pairs) {
                    if (!pair.enabled) continue;
                    auto bwid = offer_mgr_->resolve_wallet_id(pair.base_asset_id);
                    auto qwid = offer_mgr_->resolve_wallet_id(pair.quote_asset_id);
                    if (bwid > 0) wallet_ids.push_back(bwid);
                    if (qwid > 0) wallet_ids.push_back(qwid);
                }
                // Deduplicate wallet IDs.
                std::sort(wallet_ids.begin(), wallet_ids.end());
                wallet_ids.erase(
                    std::unique(wallet_ids.begin(), wallet_ids.end()),
                    wallet_ids.end());

                if (!wallet_ids.empty()) {
                    auto pruned = co_await offer_mgr_->prune_stuck_transactions(
                        wallet_ids, 600);
                    if (pruned > 0) {
                        spdlog::info("[Engine] Startup: pruned stuck transactions "
                                     "from {} wallet(s)", pruned);
                    }
                }
            }
        } catch (const std::exception& ex) {
            spdlog::warn("[Engine] Startup offer reconciliation failed: {}; "
                         "continuing without recovery", ex.what());
        }
    }

    // -- Coin pool maintenance at startup ------------------------------------
    if (!wallet_circuit_open_ && (config_.strategy.coin_pool_target_count > 0
            || config_.strategy.cat_coin_pool_target_count > 0)) {
        try {
            co_await step_maintain_coin_pool(0);
        } catch (const std::exception& ex) {
            spdlog::warn("[Engine] Startup coin pool maintenance failed: {}; "
                         "continuing without splitting", ex.what());
        }
    }
    // -- Wait for wallet sync before seeding inventory -----------------------
    // Wallet balance queries return unreliable data when the wallet is
    // still syncing.  Poll sync status until fully synced, with a
    // timeout to avoid blocking forever on a stuck wallet.
    if (wallet_) {
        constexpr int kMaxSyncWaitBlocks = 30;  // ~26 min at 52s/block
        for (int attempt = 0; attempt < kMaxSyncWaitBlocks; ++attempt) {
            try {
                auto ss = co_await wallet_->get_sync_status();
                bool synced  = ss.value("synced", false);
                bool syncing = ss.value("syncing", true);
                if (synced && !syncing) {
                    spdlog::info("[Engine] Wallet fully synced -- "
                                 "proceeding with inventory seed");
                    break;
                }
                spdlog::info("[Engine] Waiting for wallet sync "
                             "(synced={}, syncing={}, attempt {}/{})",
                             synced, syncing, attempt + 1,
                             kMaxSyncWaitBlocks);
            } catch (const std::exception& e) {
                spdlog::warn("[Engine] Wallet sync check failed: {}",
                             e.what());
            }
            // Wait ~10 seconds between probes.
            co_await asio::steady_timer(
                co_await asio::this_coro::executor,
                std::chrono::seconds(10)).async_wait(asio::use_awaitable);
            if (stop_requested_.load(std::memory_order_relaxed)) break;
        }
    }

    // -- Register pair asset-ID keys with State for mark-to-xch lookup ---
    // Each pair's base/quote asset IDs are mapped to the pair's human-
    // readable name so that mark_to_xch() can resolve market snapshots.
    if (state_) {
        for (const auto& pair : config_.pairs) {
            if (!pair.enabled) continue;
            state_->register_pair_asset_keys(
                pair.base_asset_id, pair.quote_asset_id, pair.name);
        }
    }

    // -- Seed inventory from wallet balances ---------------------------------
    // Query on-chain balances for each configured pair's base and quote
    // assets and seed the InventoryTracker so that inventory_ratio()
    // reflects actual holdings from the first tick.  Without this, the
    // tracker starts at zero for both sides and reports 0.5 (perfectly
    // balanced), causing the Avellaneda-Stoikov model to ignore real
    // portfolio skew and place symmetric quotes regardless of actual
    // wallet composition.
    if (inventory_ && offer_mgr_ && wallet_) {
        try {
            // Ensure the wallet-ID cache is populated so that
            // resolve_wallet_id() returns real IDs for CAT assets.
            co_await offer_mgr_->ensure_wallet_ids();

            // Collect unique asset IDs across all enabled pairs.
            std::unordered_set<std::string> seed_asset_ids;
            for (const auto& pair : config_.pairs) {
                if (!pair.enabled) continue;
                seed_asset_ids.insert(pair.base_asset_id);
                seed_asset_ids.insert(pair.quote_asset_id);
            }

            Mojo total_seeded = 0;
            for (const auto& aid : seed_asset_ids) {
                auto wid = offer_mgr_->resolve_wallet_id(aid);
                if (wid <= 0) continue;

                try {
                    auto bal_json = co_await wallet_->get_wallet_balance(wid);
                    Mojo spendable = bal_json.value("spendable_balance",
                                                    static_cast<Mojo>(0));
                    Mojo confirmed = bal_json.value("confirmed_wallet_balance",
                                                    static_cast<Mojo>(0));
                    const Mojo seed_qty = (confirmed > 0) ? confirmed : spendable;
                    if (seed_qty <= 0) continue;

                    // Use 1 as synthetic cost basis ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the actual value
                    // doesn't affect inventory_ratio because that method
                    // uses the live mid-price to convert baseÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢quote.
                    // What matters is that total_quantity reflects the
                    // real mojos held.
                    inventory_->seed_position(AssetId{aid}, seed_qty,
                                              Mojo{1});
                    // Also seed State positions so that apply_limits()
                    // has accurate balances from the start (not just
                    // from detected fills).
                    state_->record_buy(AssetId{aid}, seed_qty, Mojo{1});
                    total_seeded += seed_qty;

                    spdlog::info("[Engine] Seeded inventory for asset {} "
                                 "(wallet {}): {} mojos",
                                 aid.substr(0, 12), wid, seed_qty);
                } catch (const std::exception& e) {
                    spdlog::debug("[Engine] Could not query balance for "
                                  "wallet {}: {}", wid, e.what());
                }
            }

            if (total_seeded > 0) {
                spdlog::info("[Engine] Inventory seeded from wallet: "
                             "total {} mojos across {} assets",
                             total_seeded, seed_asset_ids.size());
            }
        } catch (const std::exception& ex) {
            spdlog::warn("[Engine] Startup inventory seeding failed: {}; "
                         "continuing with zero inventory", ex.what());
        }
    }

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
                        // Success ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â wallet is back.
                        wallet_circuit_open_       = false;
                        wallet_consecutive_failures_ = 0;
                        spdlog::info("[Engine] Wallet circuit breaker CLOSED "
                                     "-- wallet is reachable again");
                        // [T5-10] Invalidate the wallet-ID cache so that
                        // newly added CAT wallets are discovered.  The wallet
                        // may have been restarted with additional tokens.
                        if (offer_mgr_) {
                            offer_mgr_->invalidate_wallet_ids();
                        }
                        // Reconcile offers to catch any orphans from the
                        // outage period.
                        if (offer_mgr_) {
                            try {
                                auto fixed = co_await offer_mgr_->reconcile_offers(0);
                                if (!fixed.empty()) {
                                    spdlog::info("[Engine] Post-reconnect offer "
                                                 "reconciliation corrected {} "
                                                 "discrepancies", fixed.size());
                                    for (const auto& oid : fixed) {
                                        try {
                                            db_->update_offer_status(oid, "cancelled", 0,
                                                                    "reconnect_reconcile");
                                        } catch (const std::exception& e) {
                                            spdlog::debug("[Engine] reconnect update_offer_status "
                                                         "failed for {}: {}",
                                                         oid.substr(0, 12), e.what());
                                        }
                                    }
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
            // Non-shutdown timer error ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â log and fall through to
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

        // Publish system health during analysis so the GUI shows
        // wallet/node connectivity immediately, not only after
        // analysis completes and the main trading loop starts.
        if (metrics_->is_running()) {
            SystemHealthSnapshot health;
            health.block_height     = current_block;
            health.node_synced      = true;   // We just got a block ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ node OK.
            health.wallet_connected = wallet_->is_open();
            metrics_->update_system_health(health);
        }

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
    // Conservative ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ 1.5  (50% wider spreads: protect against adverse selection)
    // Normal       ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ 1.0  (no change from configured defaults)
    // Aggressive   ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ 0.8  (20% tighter: capture spread in stable markets)
    //
    // Compute overall_recommendation() once and derive the multiplier from
    // it, avoiding a second full traversal of all pair summaries.
    const auto overall = market_analyzer_->overall_recommendation();
    switch (overall) {
        case AnalysisAggressiveness::Conservative: analysis_spread_mult_ = 1.5; break;
        case AnalysisAggressiveness::Aggressive:   analysis_spread_mult_ = 0.8; break;
        default:                                   analysis_spread_mult_ = 1.0; break;
    }

    spdlog::info("[Engine] Analysis recommendation: {} ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ spread multiplier {:.2f}x",
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

    // Check GUI-initiated pause flag each heartbeat.
    check_pause_flag();

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

    // -- Periodic coin pool maintenance (XCH + CAT) -------------------------
    if (!wallet_circuit_open_
        && (config_.strategy.coin_pool_target_count > 0
            || config_.strategy.cat_coin_pool_target_count > 0)
        && config_.strategy.coin_pool_interval_blocks > 0
        && block_height >= coin_pool_last_block_
                           + config_.strategy.coin_pool_interval_blocks) {
        try {
            co_await step_maintain_coin_pool(block_height);
        } catch (const std::exception& e) {
            spdlog::warn("[Engine] Coin pool maintenance failed: {}", e.what());
        }
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

    // -- XCH Recovery Mode: check balance and enter/exit recovery. ---------
    // Runs before Steps 7-8 so that recovery can gate offer posting.
    // When active, cancels offers and takes cheap XCH asks instead.
    if (!wallet_circuit_open_) {
        try {
            co_await step_xch_recovery(block_height);
        } catch (const std::exception& e) {
            spdlog::error("[Engine] XCH recovery failed: {}", e.what());
        }
    }

    // -- Dynamic market allocator: re-score pairs periodically. ------------
    // Must run before Step 7 (ladder generation) so that allocation fractions
    // are up-to-date when capital is distributed across pairs.
    if (market_allocator_ && market_allocator_->should_evaluate(block_height)) {
        try {
            std::vector<std::string> enabled_pairs;
            for (const auto& [pn, pcs] : cycle_) {
                if (pcs.market_data_valid) enabled_pairs.push_back(pn);
            }
            market_allocator_->evaluate(enabled_pairs, block_height);
        }
        catch (const std::exception& e) {
            spdlog::error("[Engine] Market allocator failed: {}", e.what());
        }
    }

    // Gate Steps 7-8 when in XCH recovery mode (no market-making until
    // XCH balance is restored).
    if (xch_recovery_mode_) {
        spdlog::info("[Engine] Steps 7-8 SKIPPED: XCH recovery mode active");
    } else {

    // [v0.7.38] Query XCH confirmed balance once before Step 7 so the
    // ladder generator can hard-cap avail_inventory against reality.
    // step_generate_ladder is synchronous (non-coroutine) so it cannot
    // co_await the wallet RPC itself.
    if (!wallet_circuit_open_) {
        try {
            auto xch_bal = co_await wallet_->get_wallet_balance(1);
            if (xch_bal.contains("confirmed_wallet_balance"))
                xch_confirmed_balance_ =
                    xch_bal["confirmed_wallet_balance"].get<Mojo>();
        } catch (const std::exception& e) {
            spdlog::debug("[Engine] XCH balance query for Step 7 cap failed: {}",
                          e.what());
            // Keep previous cached value; do not zero out.
        }
    }

    try { step_generate_ladder(block_height); }
    catch (const std::exception& e) {
        spdlog::error("[Engine] Step 7 (ladder) failed: {}", e.what());
    }

    // Give active drift correction first use of spendable balances before
    // passive market-making offers lock those coins in new pending offers.
    if (!wallet_circuit_open_) {
        try { co_await step_run_drift_corrector(block_height); }
        catch (const std::exception& e) {
            spdlog::error("[Engine] Step 9f (drift corrector) failed: {}", e.what());
        }
    }

    // [T1-03] Step 8 is a coroutine (co_awaits cancel_stale + post_quotes).
    // [T3-10] Gate Step 8 during Crash/Recovery states.
    // Also gated by the wallet circuit breaker and GUI pause flag.
    if (wallet_circuit_open_) {
        spdlog::debug("[Engine] Step 8 SKIPPED: wallet circuit breaker open");
    } else if (gui_pause_active_) {
        spdlog::debug("[Engine] Step 8 SKIPPED: trading paused by GUI");
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

    } // end of !xch_recovery_mode_ block

    try { co_await step_check_arbitrage(block_height); }
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

        // Per-pair isolation: a failure in one pair must not abort the
        // market-data update for the remaining pairs.
        try {

        auto ticker = co_await dexie_->get_ticker(
            pair.base_asset_id,
            pair.quote_asset_id);
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
            // Fetch both sides directly.  Broad pair_id queries return the
            // first page of a denomination-wide book and can miss the target
            // pair's opposite side on thin CAT markets such as XCH/DBX.
            auto asks = co_await dexie_->get_offers(
                /*pair_id=*/   {},
                /*offered=*/   pair.base_asset_id,
                /*requested=*/ pair.quote_asset_id,
                /*page=*/      1,
                /*page_size=*/ 50,
                /*sort=*/      "price_asc",
                /*compact=*/   true,
                /*status=*/    0);
            auto bids = co_await dexie_->get_offers(
                /*pair_id=*/   {},
                /*offered=*/   pair.quote_asset_id,
                /*requested=*/ pair.base_asset_id,
                /*page=*/      1,
                /*page_size=*/ 50,
                /*sort=*/      "price_asc",
                /*compact=*/   true,
                /*status=*/    0);

            std::vector<rpc::OfferRecord> all_dexie_offers;
            all_dexie_offers.reserve(
                asks.offers.size() + bids.offers.size());
            all_dexie_offers.insert(all_dexie_offers.end(),
                std::make_move_iterator(asks.offers.begin()),
                std::make_move_iterator(asks.offers.end()));
            all_dexie_offers.insert(all_dexie_offers.end(),
                std::make_move_iterator(bids.offers.begin()),
                std::make_move_iterator(bids.offers.end()));

            // Build CompetingOffer vector from dexie OfferRecord data.
            std::vector<CompetingOffer> comp_offers;
            comp_offers.reserve(all_dexie_offers.size());
            for (const auto& orec : all_dexie_offers) {
                if (orec.offered.empty() || orec.requested.empty()) {
                    continue;
                }

                const std::string& offered_id = orec.offered[0].id;
                const std::string& requested_id = orec.requested[0].id;
                const bool matches_pair =
                    (offered_id == pair.base_asset_id && requested_id == pair.quote_asset_id)
                    || (offered_id == pair.quote_asset_id && requested_id == pair.base_asset_id);
                if (!matches_pair) {
                    continue;
                }

                CompetingOffer co;
                co.offer_id         = orec.id;
                co.pair_name        = pair.name;
                // Determine side from the offered/requested arrays:
                // if offered contains the pair's base asset, this is an ask.
                co.side             = (!orec.offered.empty() &&
                                       orec.offered[0].id == pair.base_asset_id)
                                      ? Side::Ask : Side::Bid;
                // Dexie price = requested_amount / offered_amount.
                // For ASK (offered=base, requested=quote): price = quote/base
                //   = market convention (correct).
                // For BID (offered=quote, requested=base): price = base/quote
                //   = reciprocal.  Invert to market convention so downstream
                //   code (competitive cap, peg-crossing, spread analysis) can
                //   compare prices consistently.
                const double market_price =
                    (co.side == Side::Bid && orec.price > 0.0)
                    ? (1.0 / orec.price) : orec.price;
                co.price            = static_cast<Mojo>(std::llround(
                    market_price * static_cast<double>(kMojosPerXch)));
                co.size             = 0;
                if (!orec.offered.empty()) {
                    // ISO/IEC 5055: round instead of truncate for size conversion.
                    // Use the correct denomination for the offered asset (CAT = 10^3, XCH = 10^12).
                    const std::int64_t offered_denom =
                        (orec.offered[0].id == pair.base_asset_id)
                        ? pair.base_mojos_per_unit
                        : pair.quote_mojos_per_unit;
                    co.size = static_cast<Mojo>(std::llround(
                        orec.offered[0].amount *
                        static_cast<double>(offered_denom)));
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
                pair.name, comp_offers, own_ids,
                pair.base_mojos_per_unit, pair.quote_mojos_per_unit);
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

        // -- TibetSwap AMM implied mid-price ---------------------------------
        // Derive the AMM implied mid-price from cached TibetSwap pool
        // reserves using the constant-product formula.  When available, the
        // market data feed blends this into its composite mid-price to
        // anchor our quotes and prevent the AMM from arbitraging us.
        if (arb_detector_) {
            const auto& reserves = arb_detector_->get_tibetswap_reserves();
            for (const auto& pool : reserves) {
                if (pool.pair_name == pair.name
                    && pool.xch_reserve > 0.0
                    && pool.token_reserve > 0.0)
                {
                    const double amm_implied =
                        tibet::get_implied_price(pool.xch_reserve,
                                                 pool.token_reserve);
                    if (amm_implied > 0.0) {
                        market_data_->ingest_amm_mid(pair.name, amm_implied);
                        spdlog::debug("[Engine] Step 1: {} Tibet AMM "
                                      "implied_mid={:.6f}",
                                      pair.name, amm_implied);
                    }
                    break;
                }
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

        } catch (const std::exception& ex) {
            spdlog::error("[Engine] Step 1: {} data fetch failed: {} "
                          "-- pair isolated, continuing with remaining pairs",
                          pair.name, ex.what());
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
            auto est = co_await full_node_->get_fee_estimate(config_.fees.fee_estimate_target_seconds);
            if (est > 0) {
                fee_tracker_->update_mempool_estimate(est);
            }
        } catch (const std::exception& e) {
            spdlog::debug("[Engine] Step 1: get_fee_estimate failed: {} "
                          "-- using static/historic fee", e.what());
        }
    }

    // -- Compute XCH conversion rates for risk mark-to-market ---------------
    // For each non-XCH asset that has an XCH pair, compute the XCH-mojo
    // value of one asset mojo.  Used by PreTradeCheck::mark_to_xch() in the
    // risk system so that inventory concentration, CAT cap, and pair-capital
    // checks reflect real economic values instead of raw mojo counts.
    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;
        double mid = market_data_->get_mid_price(pair.name);
        if (mid <= 0.0) continue;

        if (pair.base_asset_id == "xch" && pair.quote_asset_id != "xch") {
            // Pair is XCH/<CAT>: mid = display_price (get_mid_price returns display)
            // 1 CAT mojo = kMojosPerXch / (mid * quote_mojos_per_unit) XCH mojos
            double rate = static_cast<double>(kMojosPerXch)
                        / (mid * static_cast<double>(pair.quote_mojos_per_unit));
            state_->set_asset_xch_rate(AssetId{pair.quote_asset_id}, rate);
            spdlog::debug("[Engine] XCH rate: {} -> {:.2f} XCH mojos/asset mojo (mid={:.6f})",
                          pair.quote_asset_id.substr(0, 12), rate, mid);
        } else if (pair.quote_asset_id == "xch" && pair.base_asset_id != "xch") {
            // Pair is <CAT>/XCH: mid = display_price
            // 1 base mojo = mid * kMojosPerXch / base_mojos_per_unit XCH mojos
            double rate = mid * static_cast<double>(kMojosPerXch)
                        / static_cast<double>(pair.base_mojos_per_unit);
            state_->set_asset_xch_rate(AssetId{pair.base_asset_id}, rate);
            spdlog::debug("[Engine] XCH rate: {} -> {:.2f} XCH mojos/asset mojo (mid={:.6f})",
                          pair.base_asset_id.substr(0, 12), rate, mid);
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
        // [T9-FIX] Retrieve the offer-creation fee from State.
        // The Chia offer protocol does not surface settlement fees, so the
        // creation fee (paid to get the offer on-chain) is the best available
        // approximation.  Returns 0 if the offer was already removed.
        {
            auto po = state_->get_offer(fill.offer_id);
            tr.fee_mojos = po.fee_mojos;
        }
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
        // [C2/PNL-UNIT-FIX] PnL unit normalization in QUOTE-asset mojos.
        //
        // Engine pseudo-units convention (see offer_manager.cpp build_offer_dict):
        //   fill.price = quote_units_per_base_unit * kMojosPerXch
        //   fill.size  = base_mojos = base_units * base_mojos_per_unit
        //
        // Realized PnL on a sell, in quote-asset mojos, is:
        //   pnl_quote_mojos = (sell_price_real - cost_basis_real)
        //                   * size_base_units * quote_mojos_per_unit
        //                   = (fill.price - cost_basis) * fill.size
        //                     * quote_denom
        //                     / (base_denom * kMojosPerXch)
        //
        // The previous formula omitted the (quote_denom / base_denom)
        // factor, which inflated XCH/wUSDC.b PnL by 1e9 ("billions of
        // dollars" in the GUI). Doubles avoid int64 overflow.
        //
        // Guard: a cost basis of 0/1 is the startup-seed sentinel in some
        // inventory bootstraps, not a real acquisition price. Counting sells
        // against that sentinel massively inflates realized PnL. In this case
        // treat realized PnL as unknown and record 0 for this fill.
        const bool basis_unknown = (tr.cost_basis_mojos <= 1);
        if (fill.side == Side::Ask && basis_unknown) {
            tr.realized_pnl_mojos = 0;
            spdlog::warn(
                "[Engine] Fill {} {} uses unknown cost basis {} -- "
                "realized_pnl forced to 0",
                fill.offer_id, fill.pair_name, tr.cost_basis_mojos);
        } else if (fill.side == Side::Ask) {
            const double quote_denom = static_cast<double>(
                fill_pair_cfg->quote_mojos_per_unit);
            const double base_denom  = static_cast<double>(
                fill_pair_cfg->base_mojos_per_unit);
            // Use the canonical helper to keep this site in lock-step
            // with offer_manager and pnl. See xop::quote_mojos_for in
            // types.hpp and tests/test_pnl_units.cpp.
            const double pnl_quote_mojos = quote_mojos_for(
                static_cast<double>(fill.size),
                static_cast<double>(fill.price - tr.cost_basis_mojos),
                base_denom,
                quote_denom);
            if (base_denom > 0.0 && quote_denom > 0.0) {
                tr.realized_pnl_mojos = static_cast<Mojo>(
                    std::llround(pnl_quote_mojos));
            } else {
                tr.realized_pnl_mojos = 0;
                spdlog::error(
                    "[Engine] Fill {} {} invalid denom (base={}, quote={})"
                    " -- realized_pnl forced to 0",
                    fill.offer_id, fill.pair_name,
                    fill_pair_cfg->base_mojos_per_unit,
                    fill_pair_cfg->quote_mojos_per_unit);
            }
        } else {
            tr.realized_pnl_mojos = 0;
        }

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

        // [T7-FIX] Removed duplicate db_->insert_trade(tr) here.
        // PnLTracker::record_fill() (below) is the single writer for
        // trade_log.  The prior dual-INSERT caused UNIQUE constraint
        // violations on trade_id, which threw an exception in the
        // PnLTracker and prevented in-memory PnL accumulators from
        // being updated.  PnLTracker's INSERT includes all 12 columns
        // (offer_hash, acquisition_ts) vs Database's 10-column INSERT.

        // Record the offer as filled in the offer log.
        db_->update_offer_status(fill.offer_id, "filled", fill.block_height, "");

        // Update the inventory tracker using the pair's actual base asset.
        auto now = std::chrono::system_clock::now();
        // [H3] fill_pair_cfg is guaranteed non-null (guarded above).
        const std::string& fill_base = fill_pair_cfg->base_asset_id;
        if (fill.side == Side::Bid) {
            inventory_->record_buy(fill_base, fill.size, fill.price,
                                   fill.block_height, now);
        } else {
            // Confirmed fills must always reduce tracked inventory. The
            // never-sell-at-loss rule is a pre-trade control, so bypass it
            // here and only fail on missing or insufficient tracked quantity.
            // ISO/IEC 5055: checked return value on every code path.
            bool sell_ok = inventory_->record_sell(
                fill_base, fill.size, fill.price,
                fill.block_height, now, /*enforce_no_loss=*/false);
            if (!sell_ok) {
                spdlog::error("[Engine] Step 2: record_sell() REJECTED fill "
                              "for {} {} @ {} mojos (block {}) -- "
                              "tracked inventory missing or insufficient.  "
                              "Fill was confirmed on-chain but inventory "
                              "tracker refused it.",
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
        // [T9-FIX] Pass the pre-computed realized_pnl so PnLTracker uses
        // a single source of truth instead of recomputing independently.
        pnl_->record_fill(fill, tr.fee_mojos, tr.cost_basis_mojos,
                          tr.realized_pnl_mojos);

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
        // ISO/IEC 5055: convert mojos to base-asset display units for VPIN
        // using the pair's actual base_mojos_per_unit (CAT=10^3, XCH=10^12).
        const double fill_volume =
            static_cast<double>(fill.size)
            / static_cast<double>(fill_pair_cfg->base_mojos_per_unit);
        market_data_->ingest_trade_for_vpin(
            fill.pair_name, fill.side, fill_volume, kIsOwnFill);

        spdlog::info("[Engine] Fill: {} {} {} @ {} (block {})",
                     fill.pair_name,
                     (fill.side == Side::Bid) ? "BUY" : "SELL",
                     fill.size, fill.price, fill.block_height);

        // PID adaptive spread: count fills per pair for this heartbeat.
        if (config_.strategy.pid_spread_enabled) {
            spread_pid_state_[fill.pair_name].fills_this_cycle++;
        }
        // [v0.7.48] PID adaptive competitiveness-threshold: also count
        // this fill so observe_block() can fold it into the EMA.
        if (config_.strategy.comp_pid_enabled) {
            comp_pid_fills_this_block_[fill.pair_name]++;
        }

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

        // [T8-06] On regime transition, partially reset the Thompson Sampler
        // posteriors so accumulated evidence from the old regime does not
        // anchor spread selection in the new regime.
        if (vol_est.get_regime_duration_blocks() == 1 && spread_opt_) {
            spread_opt_->reset_thompson_posteriors(0.5);
            spdlog::info("[Engine] Step 3: {} regime transition detected -- "
                         "Thompson posteriors decayed by 50%", pair.name);
        }

        // -- Stablecoin depeg monitoring ------------------------------------
        // Feed the current mid-price to the depeg detector for any pair
        // flagged as a stablecoin.  The detector tracks sustained deviations
        // and transitions through Normal ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ Warning ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ Bailed states.
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

        // [v0.7.37] Sigma floor: prevent degenerate GLFT when Yang-Zhang
        // returns zero (flat market).  The floor keeps the volatility-driven
        // position-sizing term alive and avoids extreme raw half-spreads.
        if (config_.strategy.sigma_floor > 0.0) {
            sigma = std::max(sigma, config_.strategy.sigma_floor);
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
            // SpreadOptimizer::compute_spread() expects daily sigma, but
            // the estimator provides annualised sigma.  Convert:
            //   sigma_daily = sigma_annual / sqrt(365)
            sigma = vol_it->second->get_sigma_annual() / std::sqrt(365.0);
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

        // ---------------------------------------------------------------
        // [Wall-aware retail niche premium]
        //
        // On Chia DEX offers are atomic ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â a taker must match the full
        // amount.  Small traders cannot take wall-sized offers (e.g. 100+
        // XCH) and are a captive market for our smaller, accessible
        // offers.  When wall offers dominate the competing order book, we
        // widen our spread by a configurable niche premium to capture
        // this retail segment rather than futilely undercutting walls
        // we can't compete with on size.
        //
        // This stacks multiplicatively with subsequent whale/VPIN/OFI
        // multipliers, which is correct: during high-risk periods we
        // want even wider spreads on top of the niche premium.
        // ---------------------------------------------------------------
        {
            auto comp_offers_s5 =
                market_data_->get_competing_offers(pair_name);
            if (!comp_offers_s5.empty() &&
                config_.strategy.wall_niche_premium_pct > 0.0) {
                const Mojo wall_thresh = static_cast<Mojo>(std::llround(
                    config_.strategy.wall_size_threshold_xch
                    * static_cast<double>(kMojosPerXch)));
                bool has_wall = false;
                for (const auto& co : comp_offers_s5) {
                    if (co.size > wall_thresh) {
                        has_wall = true;
                        break;
                    }
                }
                if (has_wall) {
                    const double niche_mult =
                        1.0 + config_.strategy.wall_niche_premium_pct;
                    pcs.spread_result.total_spread_bps *= niche_mult;
                    pcs.spread_result.half_spread =
                        pcs.spread_result.total_spread_bps / 2.0;
                    spdlog::info("[Engine] Step 5: {} wall detected ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â "
                                "retail niche premium {:.0f}% "
                                "(spread now {:.1f}bps)",
                                pair_name,
                                config_.strategy.wall_niche_premium_pct * 100.0,
                                pcs.spread_result.total_spread_bps);
                }
            }
        }

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
                              "ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â spread widened by {:.1f}x",
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
        //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â§7.
        //
        // COUNTER-RESEARCH NOTE (CR-2, Xu, Lehalle & Alfonsi 2023):
        //   OFI is computed from best-level bid/ask only.  Multi-level
        //   OFI (top 5ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã¢â‚¬Å“10 levels) explains 10ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã¢â‚¬Å“30% more return variance.
        //   TODO: extend ingest_book_snapshot_for_ofi() to accept
        //   multiple book levels for a stronger directional signal.
        //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â§8.
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
        // multi-pair inflation (N pairs ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â 1 block = 1 activation, not N).
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
        //   Conservative ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ 1.5x (wider spreads)
        //   Normal       ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ 1.0x (no change)
        //   Aggressive   ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ 0.8x (tighter spreads)
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

        // [T8-05] DEX/CEX divergence spread widening.
        // When the DEX mid price diverges significantly from the CEX
        // reference, widen spreads proportionally to avoid adverse
        // selection from cross-venue arbitrageurs.
        //
        // Linear ramp: mult = 1.0 + min(divergence_bps / 1000, 0.5)
        // At 200 bps: 1.2x.  At 500+ bps: 1.5x (max).
        if (auto cex_ref = market_data_->get_cex_reference(pair_name)) {
            double dex_mid = market_data_->get_mid_price(pair_name);
            if (dex_mid > 0.0 && *cex_ref > 0.0) {
                double divergence_bps =
                    std::abs(dex_mid - *cex_ref) / dex_mid * 10000.0;
                constexpr double kDivergenceScaleBps = 1000.0;
                constexpr double kMaxDivergenceMult  = 0.5;
                const double divergence_mult =
                    1.0 + std::min(divergence_bps / kDivergenceScaleBps,
                                   kMaxDivergenceMult);
                if (divergence_mult > 1.01) {
                    pcs.spread_result.total_spread_bps *= divergence_mult;
                    spdlog::warn("[Engine] Step 5: {} DEX/CEX divergence={:.0f}bps "
                                 "-- spread widened by {:.2f}x",
                                 pair_name, divergence_bps, divergence_mult);
                }
            }
        }

        // -- PID adaptive spread controller --------------------------------
        // Adjusts spread based on fill-rate feedback: tightens when offers
        // don't fill, widens when fills become frequent.
        if (config_.strategy.pid_spread_enabled) {
            auto& pid = spread_pid_state_[pair_name];

            // Increment blocks-active counter whenever we reach Step 5.
            ++pid.blocks_active;

            // Binary fill signal: 1 if any fill this block, 0 otherwise.
            const double fill_signal = (pid.fills_this_cycle > 0) ? 1.0 : 0.0;

            // Update EMA fill rate.
            const double alpha = config_.strategy.pid_ema_alpha;
            pid.ema_fill_rate = alpha * fill_signal
                              + (1.0 - alpha) * pid.ema_fill_rate;

            // Compute PID only after warm-up period.
            if (pid.blocks_active > config_.strategy.pid_warmup_blocks) {
                const double error =
                    config_.strategy.pid_target_fill_rate - pid.ema_fill_rate;

                const double p_term = config_.strategy.pid_kp * error;
                const double i_term = config_.strategy.pid_ki * pid.integral_error;
                const double d_term = config_.strategy.pid_kd
                                    * (error - pid.prev_error);

                const double output = p_term + i_term + d_term;

                // Convert to multiplier: positive output -> tighten (< 1.0)
                pid.current_mult = std::clamp(
                    1.0 - output,
                    config_.strategy.pid_min_mult,
                    config_.strategy.pid_max_mult);

                // Update integral with anti-windup clamp.
                pid.integral_error = std::clamp(
                    pid.integral_error + error,
                    -config_.strategy.pid_integral_max,
                    config_.strategy.pid_integral_max);

                pid.prev_error = error;
            }

            // Reset per-cycle fill counter (consumed).
            pid.fills_this_cycle = 0;

            // Apply the PID multiplier.
            pcs.spread_result.total_spread_bps *= pid.current_mult;

            spdlog::debug("[Engine] Step 5: {} PID mult={:.3f} "
                          "(ema_fill={:.4f} target={:.4f} blocks={})",
                          pair_name, pid.current_mult,
                          pid.ema_fill_rate,
                          config_.strategy.pid_target_fill_rate,
                          pid.blocks_active);
        }

        // -- [v0.7.48] PID adaptive competitiveness-threshold controller --
        // Tick the per-pair competitiveness PID exactly once per heartbeat
        // here in Step 5.  The Step 8 gate below reads current_offset()
        // synchronously after this update.  fills counter is consumed.
        if (config_.strategy.comp_pid_enabled) {
            // Lazily create per-pair PID with current strategy config.
            auto pid_it = comp_pid_state_.find(pair_name);
            if (pid_it == comp_pid_state_.end()) {
                xop::strategy::CompetitivenessPidConfig cpc;
                cpc.enabled           = config_.strategy.comp_pid_enabled;
                cpc.target_fill_rate  = config_.strategy.comp_pid_target_fill_rate;
                cpc.kp                = config_.strategy.comp_pid_kp;
                cpc.ki                = config_.strategy.comp_pid_ki;
                cpc.kd                = config_.strategy.comp_pid_kd;
                cpc.ema_alpha         = config_.strategy.comp_pid_ema_alpha;
                cpc.integral_max      = config_.strategy.comp_pid_integral_max;
                cpc.warmup_blocks     = config_.strategy.comp_pid_warmup_blocks;
                cpc.min_offset        = config_.strategy.comp_pid_min_offset;
                cpc.max_offset        = config_.strategy.comp_pid_max_offset;
                pid_it = comp_pid_state_.emplace(
                    pair_name,
                    xop::strategy::CompetitivenessPid{cpc}).first;
            }
            const int fills = comp_pid_fills_this_block_[pair_name];
            pid_it->second.observe_block(fills > 0);
            comp_pid_fills_this_block_[pair_name] = 0;

            spdlog::debug("[Engine] Step 5: {} comp-PID offset={} "
                          "(ema_fill={:.4f} target={:.4f} blocks={})",
                          pair_name,
                          pid_it->second.current_offset(),
                          pid_it->second.ema_fill_rate(),
                          config_.strategy.comp_pid_target_fill_rate,
                          pid_it->second.blocks_active());
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
        const double max_hs = [&]() -> double {
            const PairConfig* spc = find_pair_config(pair_name);
            if (spc && spc->max_half_spread_bps_override.has_value())
                return spc->max_half_spread_bps_override.value();
            return config_.strategy.max_half_spread_bps;
        }();
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
        //
        // SAFETY: The A-S half-spread formula (1/kappa)*ln(1+kappa/gamma)
        // produces an ABSOLUTE price spread that does not scale with the
        // mid-price level.  With gamma=0.01, kappa=1.5, the raw half-
        // spread is ~3.35 price units -- wider than the mid for any pair
        // under ~$6.70.  When the bid gets clamped to zero (mid < hs),
        // the naive average (0 + ask)/2 pushes the reservation mid FAR
        // above the market (e.g. 2.17 for a 0.99 pair), causing all
        // tiers to be priced at 2x+ market and the asks to pass the
        // order-book guard.  Clamp the reservation mid to within
        // kMaxReservationDeviationPct of the market mid so the
        // inventory skew is preserved but the center stays realistic.
        // ISO/IEC 5055: bounded output prevents pathological mispricing.
        double mid = market_data_->get_mid_price(pair_name);
        double reservation_mid = (pcs.raw_quote.bid_price
                                + pcs.raw_quote.ask_price) / 2.0;

        // Clamp reservation mid: preserve inventory skew but prevent the
        // A-S absolute-spread pathology from shifting the quote center.
        constexpr double kMaxReservationDeviationPct = 0.01;  // 1%
        const double max_dev = mid * kMaxReservationDeviationPct;
        if (std::abs(reservation_mid - mid) > max_dev) {
            const double clamped = std::clamp(
                reservation_mid, mid - max_dev, mid + max_dev);
            spdlog::info("[Engine] Step 6: {} reservation_mid clamped "
                         "{:.6f} -> {:.6f} (market mid={:.6f}, max dev={:.1f}%)",
                         pair_name, reservation_mid, clamped,
                         mid, kMaxReservationDeviationPct * 100.0);
            reservation_mid = clamped;
        }

        double half_spread = pcs.spread_result.half_spread / 10000.0 * mid;

        // Apply per-side asymmetric widening from whale/OFI analysis.
        auto asym = market_data_->get_asymmetric_spread_multipliers(pair_name);
        double bid_half = half_spread * asym.bid_multiplier;
        double ask_half = half_spread * asym.ask_multiplier;

        // ISO/IEC 5055: round instead of truncate for price/size conversions.
        // Prices use kMojosPerXch as a uniform fixed-point scaling factor
        // (consistent with build_offer_dict which undoes this scaling).
        // Sizes use the pair's actual base_mojos_per_unit (XCH=10^12,
        // CAT=10^3) since they represent real on-chain mojo amounts.
        Quote quote;
        quote.bid_price  = static_cast<Mojo>(std::llround(
            (reservation_mid - bid_half) * static_cast<double>(kMojosPerXch)));
        quote.ask_price  = static_cast<Mojo>(std::llround(
            (reservation_mid + ask_half) * static_cast<double>(kMojosPerXch)));
        quote.bid_size   = static_cast<Mojo>(std::llround(
            pcs.raw_quote.bid_size
            * static_cast<double>(pair_cfg->base_mojos_per_unit)));
        quote.ask_size   = static_cast<Mojo>(std::llround(
            pcs.raw_quote.ask_size
            * static_cast<double>(pair_cfg->base_mojos_per_unit)));
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
            const auto limits = pre_trade_->get_limit_status(
                AssetId{pair_cfg->base_asset_id},
                AssetId{pair_cfg->quote_asset_id},
                *state_);

            spdlog::warn(
                "[Engine] Step 6: {} -- both sides blocked by risk limits "
                "(base_conc={:.3f} soft_breach={} hard_breach={} "
                "cat_pct={:.3f} cat_breach={} pair_pct={:.3f} "
                "pair_breach={} cfg_soft={:.3f} cfg_hard={:.3f} "
                "cfg_cat={:.3f} cfg_pair={:.3f})",
                pair_name,
                limits.base_concentration,
                limits.soft_limit_breached,
                limits.hard_limit_breached,
                limits.cat_portfolio_pct,
                limits.cat_cap_breached,
                limits.pair_capital_pct,
                limits.pair_cap_breached,
                config_.risk.soft_limit_pct,
                config_.risk.hard_limit_pct,
                config_.risk.single_cat_cap_pct,
                config_.risk.max_capital_per_pair_pct);
        }
    }
}

// Step 7: Generate multi-tier offer ladder.
void Engine::step_generate_ladder([[maybe_unused]] BlockHeight block_height)
{
    // -- Per-asset portfolio percentages (for asset-level drift guard) ----
    // Computed once per cycle: XCH-equivalent value of each asset divided
    // by total XCH-equivalent portfolio value.  Keys are upper-cased
    // asset symbols to match config keys ("XCH", "BYC", "WUSDC.B", ...).
    std::unordered_map<std::string, double> portfolio_pct_by_asset;
    if (config_.strategy.asset_drift_guard_enabled
        && !config_.strategy.asset_target_allocations.empty())
    {
        auto to_lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        auto resolve_symbol = [&](const std::string& asset_id) -> std::string {
            std::string lid = to_lower(asset_id);
            if (lid == "xch") return "XCH";
            for (const auto& pair : config_.pairs) {
                if (to_lower(pair.base_asset_id) == lid) {
                    auto pos = pair.name.find('/');
                    if (pos != std::string::npos) {
                        auto sym = pair.name.substr(0, pos);
                        for (char& c : sym) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        return sym;
                    }
                }
                if (to_lower(pair.quote_asset_id) == lid) {
                    auto pos = pair.name.find('/');
                    if (pos != std::string::npos) {
                        auto sym = pair.name.substr(pos + 1);
                        for (char& c : sym) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        return sym;
                    }
                }
            }
            auto uid = asset_id;
            for (char& c : uid) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return uid;
        };

        const auto positions = state_->get_all_positions();
        double total_xch = 0.0;
        std::unordered_map<std::string, double> asset_xch;
        for (const auto& p : positions) {
            const double v = static_cast<double>(PreTradeCheck::mark_to_xch(p, *state_));
            if (v <= 0.0) continue;
            asset_xch[resolve_symbol(p.asset_id)] += v;
            total_xch += v;
        }
        if (total_xch > 0.0) {
            for (const auto& [k, v] : asset_xch) {
                portfolio_pct_by_asset[k] = v / total_xch;
            }
        }
    }

    for (auto& [pair_name, pcs] : cycle_) {
        if (!pcs.quote_valid) continue;

        auto liq_it = liquidity_engines_.find(pair_name);
        if (liq_it == liquidity_engines_.end()) continue;

        auto& liq = *liq_it->second;

        // Mid-price in mojos: use the market mid-price rather than the
        // risk_quote average, which inherits the A-S reservation price
        // skew.  The LiquidityEngine builds tiers symmetrically around
        // this center; the reservation skew was already applied to the
        // risk_quote sizes (bid_size / ask_size) in Step 6.
        double market_mid = market_data_->get_mid_price(pair_name);

        // -- Stablecoin peg-anchored mid ------------------------------------
        // For stable-stable pairs the thin order book produces noisy mid
        // estimates.  When the market mid is CLOSE to the peg (< 1%
        // deviation), lightly anchor towards the peg (50/50 blend) to
        // filter thin-book noise.  When the market deviates further, trust
        // the market ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â a genuine discount/premium likely reflects real
        // conditions (liquidity depth, bridge risk, etc.) and the depeg
        // detector handles any bail-out.
        {
            const PairConfig* pc = find_pair_config(pair_name);
            if (pc && pc->is_stablecoin && pc->peg_target > 0.0) {
                const double dev = std::abs(market_mid - pc->peg_target)
                                 / pc->peg_target;
                const double peg_threshold = pc->peg_anchor_threshold_pct / 100.0;
                const double peg_weight    = pc->peg_anchor_weight;
                if (dev <= peg_threshold) {
                    const double blended = peg_weight * pc->peg_target
                                         + (1.0 - peg_weight) * market_mid;
                    spdlog::debug("[Engine] Step 7: {} peg-anchor mid: "
                                  "market={:.6f} peg={:.4f} blended={:.6f} "
                                  "(dev={:.2f}% thr={:.1f}% w={:.0f}%)",
                                  pair_name, market_mid, pc->peg_target,
                                  blended, dev * 100.0,
                                  peg_threshold * 100.0,
                                  peg_weight * 100.0);
                    market_mid = blended;
                } else {
                    spdlog::debug("[Engine] Step 7: {} peg-anchor skipped: "
                                  "market={:.6f} peg={:.4f} dev={:.2f}%"
                                  " > {:.1f}% -- trusting market",
                                  pair_name, market_mid, pc->peg_target,
                                  dev * 100.0, peg_threshold * 100.0);
                }
            }
        }

        Mojo mid_mojos = static_cast<Mojo>(std::llround(
            market_mid * static_cast<double>(kMojosPerXch)));

        // Volatility.
        double sigma = 0.0;
        auto vol_it = vol_estimators_.find(pair_name);
        if (vol_it != vol_estimators_.end() && vol_it->second->is_ready()) {
            sigma = vol_it->second->get_sigma_annual();
        }

        // [v0.7.37] Sigma floor (same as Step 4).
        if (config_.strategy.sigma_floor > 0.0) {
            sigma = std::max(sigma, config_.strategy.sigma_floor);
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

        // -- Cross-pair correlated inventory skewing (Gueant 2019) --
        // When enabled, adjusts inv_ratio based on inventory pressure
        // from OTHER pairs sharing the same base or quote asset.
        // Example: if XCH/BYC is short BYC, BYC/wUSDC.b should skew
        // its bids UP (lower inv_ratio) to acquire more BYC.
        if (config_.strategy.cross_pair_skew_enabled && pair_cfg) {
            const double cphi = config_.strategy.cross_pair_skew_phi;
            double cross_adj = 0.0;
            double weight_sum = 0.0;

            for (const auto& [other_name, other_pcs] : cycle_) {
                if (other_name == pair_name) continue;
                const PairConfig* other_cfg = find_pair_config(other_name);
                if (!other_cfg) continue;

                // Check if other pair shares our base or quote asset.
                const bool shares_base =
                    (other_cfg->base_asset_id == pair_cfg->base_asset_id) ||
                    (other_cfg->quote_asset_id == pair_cfg->base_asset_id);
                const bool shares_quote =
                    (other_cfg->base_asset_id == pair_cfg->quote_asset_id) ||
                    (other_cfg->quote_asset_id == pair_cfg->quote_asset_id);

                if (!shares_base && !shares_quote) continue;

                // Get the other pair's inventory ratio.
                double other_mid = market_data_->get_mid_price(other_name);
                if (other_mid <= 0.0) continue;
                Mojo other_mid_mojos = static_cast<Mojo>(std::llround(
                    other_mid * static_cast<double>(kMojosPerXch)));
                double other_ratio = inventory_->inventory_ratio(
                    AssetId{other_cfg->base_asset_id},
                    AssetId{other_cfg->quote_asset_id},
                    other_mid_mojos);

                // Deviation of the other pair from its TARGET ratio (per
                // ratio_target_by_pair, falling back to the global
                // ratio_target).  Using the per-pair target rather than
                // a hardcoded 0.5 prevents the skew from interpreting an
                // intentionally-skewed allocation (e.g. BYC=5% / wUSDC.b=30%
                // -> BYC/wUSDC.b target ratio ~0.14) as inventory pressure
                // to acquire more of the overweight asset.
                double other_target = config_.strategy.ratio_target;
                if (auto it_t = config_.strategy.ratio_target_by_pair.find(other_name);
                    it_t != config_.strategy.ratio_target_by_pair.end()) {
                    other_target = it_t->second;
                }
                const double other_denom = std::max(other_target, 1.0 - other_target);
                double other_dev = (other_ratio - other_target)
                                 / std::max(other_denom, 1e-6);

                // Weight: market allocator fraction, or uniform.
                double w = 1.0 / static_cast<double>(cycle_.size());
                if (market_allocator_ && config_.market_allocator.enabled) {
                    w = market_allocator_->get_allocation(other_name);
                }

                // Determine the sign of the adjustment.
                // If the shared asset is the BASE of our pair:
                //   Other pair is long base (other_dev > 0) -> they
                //   want to sell -> we should also try to sell base
                //   -> push our inv_ratio UP (makes us appear longer).
                // If the shared asset is the QUOTE of our pair:
                //   Other pair is long its base (other_dev > 0) ->
                //   they need quote -> we should accumulate quote
                //   -> push our inv_ratio DOWN (sell base).
                double sign = 0.0;
                if (shares_base) sign += other_dev;
                if (shares_quote) sign -= other_dev;

                cross_adj += w * sign;
                weight_sum += w;
            }

            if (weight_sum > 0.0) {
                cross_adj = cphi * cross_adj / weight_sum;
                const double orig_ratio = inv_ratio;
                inv_ratio = std::clamp(inv_ratio + cross_adj, 0.0, 1.0);
                if (std::abs(cross_adj) > 0.01) {
                    spdlog::info("[Engine] Step 7: {} cross-pair skew: "
                                 "ratio {:.3f} -> {:.3f} (adj={:+.3f})",
                                 pair_name, orig_ratio, inv_ratio,
                                 cross_adj);
                }
            }
        }


        // Available capital for bids (quote asset) and asks (base asset).
        Mojo avail_capital   = pcs.risk_quote.bid_size;
        Mojo avail_inventory = pcs.risk_quote.ask_size;

        // -- XCH fee reserve: hold back fee_reserve_xch from the pool so
        // that enough XCH always remains spendable for on-chain fees
        // (offer creation / cancellation).  Only applies when XCH is the
        // base or quote asset of this pair.
        if (pair_cfg && config_.strategy.fee_reserve_xch > 0.0) {
            const auto reserve_mojos = static_cast<Mojo>(std::llround(
                config_.strategy.fee_reserve_xch
                * static_cast<double>(kMojosPerXch)));
            if (pair_cfg->base_asset_id == "xch" && avail_inventory > 0) {
                const auto prev = avail_inventory;
                avail_inventory = std::max(Mojo{0},
                                           avail_inventory - reserve_mojos);
                if (avail_inventory < prev) {
                    spdlog::info("[Engine] Step 7: {} XCH fee reserve: "
                                 "ask pool {:.6f} -> {:.6f} XCH "
                                 "(reserved {:.3f} XCH for fees)",
                                 pair_name,
                                 static_cast<double>(prev) / kMojosPerXch,
                                 static_cast<double>(avail_inventory) / kMojosPerXch,
                                 config_.strategy.fee_reserve_xch);
                }
            }
            if (pair_cfg->quote_asset_id == "xch" && avail_capital > 0) {
                const auto prev = avail_capital;
                avail_capital = std::max(Mojo{0},
                                         avail_capital - reserve_mojos);
                if (avail_capital < prev) {
                    spdlog::info("[Engine] Step 7: {} XCH fee reserve: "
                                 "bid pool {:.6f} -> {:.6f} XCH "
                                 "(reserved {:.3f} XCH for fees)",
                                 pair_name,
                                 static_cast<double>(prev) / kMojosPerXch,
                                 static_cast<double>(avail_capital) / kMojosPerXch,
                                 config_.strategy.fee_reserve_xch);
                }
            }
        }

        // -- [v0.7.38] Hard wallet-balance cap: prevent the Avellaneda model
        // from sizing offers beyond what the wallet actually holds.
        // q_max drives ask_size = q_max*(1+q/q_max) which can vastly exceed
        // the confirmed XCH balance.  Without this cap, oversized sell offers
        // drain XCH far faster than buy offers can replenish it.
        if (pair_cfg && xch_confirmed_balance_ > 0) {
            if (pair_cfg->base_asset_id == "xch"
                && avail_inventory > xch_confirmed_balance_)
            {
                spdlog::warn("[Engine] Step 7: {} ask pool {:.6f} XCH > "
                             "confirmed {:.6f} XCH -- CAPPED",
                             pair_name,
                             static_cast<double>(avail_inventory) / kMojosPerXch,
                             static_cast<double>(xch_confirmed_balance_) / kMojosPerXch);
                avail_inventory = xch_confirmed_balance_;
            }
            if (pair_cfg->quote_asset_id == "xch"
                && avail_capital > xch_confirmed_balance_)
            {
                spdlog::warn("[Engine] Step 7: {} bid pool {:.6f} XCH > "
                             "confirmed {:.6f} XCH -- CAPPED",
                             pair_name,
                             static_cast<double>(avail_capital) / kMojosPerXch,
                             static_cast<double>(xch_confirmed_balance_) / kMojosPerXch);
                avail_capital = xch_confirmed_balance_;
            }
        }

        // -- Symmetric CAT wallet-balance caps (mirrors the XCH cap above
        // for non-XCH base/quote assets).  The ask pool consumes the BASE
        // asset; the bid pool consumes the QUOTE asset.  Without these
        // caps, an Avellaneda+risk-sized pool can exceed what the CAT
        // wallet actually holds, producing offers the wallet can't back.
        if (pair_cfg && config_.strategy.wallet_balance_caps_enabled) {
            auto cat_confirmed = [&](const std::string& asset_id) -> Mojo {
                auto it = cached_wallet_balances_.find(asset_id);
                return (it != cached_wallet_balances_.end())
                           ? it->second.confirmed
                           : Mojo{0};
            };

            // Ask side: convert base wallet (in base mojos) directly.
            if (pair_cfg->base_asset_id != "xch") {
                const Mojo base_bal = cat_confirmed(pair_cfg->base_asset_id);
                if (base_bal > 0 && avail_inventory > base_bal) {
                    spdlog::warn("[Engine] Step 7: {} ask pool {:.4f} {} > "
                                 "confirmed {:.4f} {} -- CAPPED (CAT wallet)",
                                 pair_name,
                                 static_cast<double>(avail_inventory)
                                     / static_cast<double>(pair_cfg->base_mojos_per_unit),
                                 pair_cfg->base_asset_id,
                                 static_cast<double>(base_bal)
                                     / static_cast<double>(pair_cfg->base_mojos_per_unit),
                                 pair_cfg->base_asset_id);
                    avail_inventory = base_bal;
                }
            }

            // Bid side: convert quote wallet (in quote mojos) into the
            // maximum base mojos we could buy at the current mid price.
            //   max_base_units  = quote_units / mid
            //   max_base_mojos  = max_base_units * base_mojos_per_unit
            if (pair_cfg->quote_asset_id != "xch"
                && market_mid > 0.0
                && pair_cfg->quote_mojos_per_unit > 0
                && pair_cfg->base_mojos_per_unit > 0)
            {
                const Mojo quote_bal = cat_confirmed(pair_cfg->quote_asset_id);
                if (quote_bal > 0) {
                    const double quote_units =
                        static_cast<double>(quote_bal)
                        / static_cast<double>(pair_cfg->quote_mojos_per_unit);
                    const Mojo bid_cap_base = static_cast<Mojo>(std::llround(
                        (quote_units / market_mid)
                        * static_cast<double>(pair_cfg->base_mojos_per_unit)));
                    if (bid_cap_base > 0 && avail_capital > bid_cap_base) {
                        spdlog::warn("[Engine] Step 7: {} bid pool {:.4f} {} "
                                     "(={:.4f} {} @ {:.6f}) > wallet {:.4f} {} "
                                     "-- CAPPED (CAT wallet)",
                                     pair_name,
                                     static_cast<double>(avail_capital)
                                         / static_cast<double>(pair_cfg->base_mojos_per_unit),
                                     pair_cfg->base_asset_id,
                                     static_cast<double>(avail_capital)
                                         / static_cast<double>(pair_cfg->base_mojos_per_unit)
                                         * market_mid,
                                     pair_cfg->quote_asset_id,
                                     market_mid,
                                     quote_units,
                                     pair_cfg->quote_asset_id);
                        avail_capital = bid_cap_base;
                    }
                }
            }
        }

        // -- Dynamic market allocation: scale capital by scoring fraction.
        // When enabled, the MarketAllocator assigns a [0, 1] fraction per
        // pair (summing to 1.0 across all enabled pairs).  Multiply the
        // available capital/inventory by (alloc_frac * num_pairs) so that
        // the average pair gets 1.0x and the allocator redistributes.
        if (market_allocator_ && config_.market_allocator.enabled) {
            double alloc_frac = market_allocator_->get_allocation(pair_name);
            double num_pairs = static_cast<double>(cycle_.size());
            // Scale factor: alloc_frac * N so that equal allocation = 1.0x.
            // Capped to avoid over-allocating beyond actual balance.
            double scale = std::min(alloc_frac * num_pairs, 1.5);
            avail_capital   = static_cast<Mojo>(
                static_cast<double>(avail_capital) * scale);
            avail_inventory = static_cast<Mojo>(
                static_cast<double>(avail_inventory) * scale);
            spdlog::debug("[Engine] Step 7: {} market alloc={:.1f}% "
                          "scale={:.2f}x",
                          pair_name, alloc_frac * 100.0, scale);
        }

        // -- Ratio rebalance sizing (hysteresis-aware) ---------------------
        // Scale bid/ask pools based on target-ratio mode so the underweight
        // side receives more capital while the overweight side is tapered.
        if (pair_cfg && config_.strategy.ratio_rebalance_enabled) {
            auto it_mode = ratio_rebalance_modes_.find(pair_name);
            if (it_mode == ratio_rebalance_modes_.end()) {
                it_mode = ratio_rebalance_modes_.emplace(
                    pair_name, RatioRebalanceMode::Neutral).first;
            }

            const RatioRebalanceMode prev_mode = it_mode->second;
            double target = config_.strategy.ratio_target;
            if (auto it_target = config_.strategy.ratio_target_by_pair.find(pair_name);
                it_target != config_.strategy.ratio_target_by_pair.end()) {
                target = it_target->second;
            }
            double enter_band = config_.strategy.ratio_band_enter;
            double exit_band = config_.strategy.ratio_band_exit;
            if (auto it_band = config_.strategy.ratio_band_enter_by_pair.find(pair_name);
                it_band != config_.strategy.ratio_band_enter_by_pair.end()) {
                enter_band = it_band->second;
                // Keep exit_band <= enter_band so hysteresis stays consistent.
                exit_band = std::min(exit_band, enter_band * 0.5);
                if (exit_band <= 0.0) {
                    exit_band = enter_band * 0.5;
                }
            }
            const double upper_enter = target + enter_band;
            const double lower_enter = target - enter_band;
            const double upper_exit = target + exit_band;
            const double lower_exit = target - exit_band;

            RatioRebalanceMode next_mode = prev_mode;
            if (prev_mode == RatioRebalanceMode::Neutral) {
                if (inv_ratio >= upper_enter) {
                    next_mode = RatioRebalanceMode::AcquireQuote;
                } else if (inv_ratio <= lower_enter) {
                    next_mode = RatioRebalanceMode::AcquireBase;
                }
            } else if (inv_ratio >= lower_exit && inv_ratio <= upper_exit) {
                next_mode = RatioRebalanceMode::Neutral;
            }

            if (next_mode != prev_mode) {
                it_mode->second = next_mode;
                auto mode_name = [](RatioRebalanceMode m) {
                    switch (m) {
                        case RatioRebalanceMode::Neutral: return "Neutral";
                        case RatioRebalanceMode::AcquireBase: return "AcquireBase";
                        case RatioRebalanceMode::AcquireQuote: return "AcquireQuote";
                    }
                    return "Unknown";
                };
                spdlog::info("[Engine] Step 7: {} ratio mode {} -> {} "
                             "(ratio={:.3f} target={:.3f})",
                             pair_name,
                             mode_name(prev_mode),
                             mode_name(next_mode),
                             inv_ratio,
                             target);
            }

            const RatioRebalanceMode active_mode = it_mode->second;
            if (active_mode != RatioRebalanceMode::Neutral) {
                const double ratio_delta = std::abs(inv_ratio - target);
                const double norm = std::clamp(
                    ratio_delta / std::max(enter_band, 1e-6), 0.0, 1.0);
                const double min_scale = config_.strategy.ratio_tier_size_scale_min;
                const double max_scale = config_.strategy.ratio_tier_size_scale_max;
                const double overweight_scale =
                    std::clamp(1.0 - (1.0 - min_scale) * norm, min_scale, 1.0);
                const double underweight_scale =
                    std::clamp(1.0 + (max_scale - 1.0) * norm, 1.0, max_scale);

                const Mojo old_bid_pool = avail_capital;
                const Mojo old_ask_pool = avail_inventory;

                if (active_mode == RatioRebalanceMode::AcquireBase) {
                    // Boost bids (buy base), taper asks.
                    avail_capital = static_cast<Mojo>(std::llround(
                        static_cast<double>(avail_capital) * underweight_scale));
                    avail_inventory = static_cast<Mojo>(std::llround(
                        static_cast<double>(avail_inventory) * overweight_scale));
                } else {
                    // Boost asks (acquire quote), taper bids.
                    avail_capital = static_cast<Mojo>(std::llround(
                        static_cast<double>(avail_capital) * overweight_scale));
                    avail_inventory = static_cast<Mojo>(std::llround(
                        static_cast<double>(avail_inventory) * underweight_scale));
                }

                spdlog::info("[Engine] Step 7: {} ratio size scaling mode={} "
                             "bid_pool {} -> {} ask_pool {} -> {} "
                             "(ratio={:.3f}, overweight_scale={:.2f}, "
                             "underweight_scale={:.2f})",
                             pair_name,
                             (active_mode == RatioRebalanceMode::AcquireBase)
                                 ? "AcquireBase" : "AcquireQuote",
                             old_bid_pool,
                             avail_capital,
                             old_ask_pool,
                             avail_inventory,
                             inv_ratio,
                             overweight_scale,
                             underweight_scale);
            }
        }

        // -- Asset-level soft drift guard ---------------------------------
        // Acts as a "soft wall" on top of pair-level ratio rebalancing:
        // when an asset's actual portfolio fraction is more than `tol`
        // outside its target, taper the side of every pair that would
        // accumulate more of that asset.  The taper is linear from 1.0
        // at the edge of the band down to 0.0 at
        //   target +/- (asset_drift_guard_max_factor) * tol
        // so by the time an asset has drifted "twice the tolerance" past
        // its band, no further acquisition pressure remains.  Stops the
        // cross-pair skew / triangular-arb path from accumulating an asset
        // far past its target while one pair's ratio controller is already
        // pushing it back the other way.
        if (pair_cfg
            && config_.strategy.asset_drift_guard_enabled
            && !portfolio_pct_by_asset.empty()
            && !config_.strategy.asset_target_allocations.empty())
        {
            auto to_lower2 = [](std::string s) {
                for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            auto resolve_symbol2 = [&](const std::string& asset_id) -> std::string {
                std::string lid = to_lower2(asset_id);
                if (lid == "xch") return "XCH";
                for (const auto& pair : config_.pairs) {
                    if (to_lower2(pair.base_asset_id) == lid) {
                        auto pos = pair.name.find('/');
                        if (pos != std::string::npos) {
                            auto sym = pair.name.substr(0, pos);
                            for (char& c : sym) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                            return sym;
                        }
                    }
                    if (to_lower2(pair.quote_asset_id) == lid) {
                        auto pos = pair.name.find('/');
                        if (pos != std::string::npos) {
                            auto sym = pair.name.substr(pos + 1);
                            for (char& c : sym) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                            return sym;
                        }
                    }
                }
                auto uid = asset_id;
                for (char& c : uid) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                return uid;
            };

            const double max_factor = std::max(
                config_.strategy.asset_drift_guard_max_factor, 1.0001);

            // Returns a scale in [0, 1] for the side that *acquires* `asset_key`.
            // 1.0 when the asset is within or below its band; tapers to 0
            // as the asset rises above `target + max_factor * tol`.
            auto acquire_scale = [&](const std::string& asset_key) -> double {
                auto it_tgt = config_.strategy.asset_target_allocations.find(asset_key);
                if (it_tgt == config_.strategy.asset_target_allocations.end()) return 1.0;
                auto it_pct = portfolio_pct_by_asset.find(asset_key);
                if (it_pct == portfolio_pct_by_asset.end()) return 1.0;
                const double target = it_tgt->second;
                double tol = 0.0;
                if (auto it_tol = config_.strategy.asset_target_tolerances.find(asset_key);
                    it_tol != config_.strategy.asset_target_tolerances.end()) {
                    tol = it_tol->second;
                }
                // Without a tolerance, treat 25% of target (min 1pp) as the
                // implicit deadband so a zero-tolerance entry still produces
                // a soft, not infinite, taper window.
                if (tol <= 0.0) tol = std::max(0.01, target * 0.25);
                const double excess = it_pct->second - (target + tol);
                if (excess <= 0.0) return 1.0;
                const double taper_window = (max_factor - 1.0) * tol;
                if (taper_window <= 0.0) return 0.0;
                return std::clamp(1.0 - excess / taper_window, 0.0, 1.0);
            };

            const std::string base_key  = resolve_symbol2(pair_cfg->base_asset_id);
            const std::string quote_key = resolve_symbol2(pair_cfg->quote_asset_id);

            // Bid acquires base, spends quote.  Ask acquires quote, spends base.
            const double bid_scale = acquire_scale(base_key);
            const double ask_scale = acquire_scale(quote_key);

            if (bid_scale < 0.999 || ask_scale < 0.999) {
                const Mojo old_bid = avail_capital;
                const Mojo old_ask = avail_inventory;
                avail_capital = static_cast<Mojo>(std::llround(
                    static_cast<double>(avail_capital) * bid_scale));
                avail_inventory = static_cast<Mojo>(std::llround(
                    static_cast<double>(avail_inventory) * ask_scale));
                auto pct_or = [&](const std::string& k) {
                    auto it = portfolio_pct_by_asset.find(k);
                    return it != portfolio_pct_by_asset.end() ? it->second : 0.0;
                };
                spdlog::info("[Engine] Step 7: {} drift-guard scale "
                             "bid x{:.2f} ask x{:.2f} "
                             "(base {}={:.1f}% ask-acquires {}={:.1f}%) "
                             "bid_pool {} -> {} ask_pool {} -> {}",
                             pair_name,
                             bid_scale, ask_scale,
                             base_key, pct_or(base_key) * 100.0,
                             quote_key, pct_or(quote_key) * 100.0,
                             old_bid, avail_capital,
                             old_ask, avail_inventory);
            }
        }

        // -- Deploy-idle-inventory floor: when risk/allocator scaling has
        // collapsed a side's pool below `min_offer_size_units` despite the
        // wallet holding ample balance to back at least one full tier,
        // raise that pool up to the minimum so the ladder is not silently
        // suppressed downstream by the dust filter.  Respects ratio
        // rebalance mode: only the "acquire" side is floored in non-neutral
        // modes (matches the intent of one-sided ratio rebalancing).
        if (pair_cfg
            && config_.strategy.deploy_idle_inventory_enabled
            && pair_cfg->base_mojos_per_unit > 0
            && config_.strategy.min_offer_size_units > 0.0)
        {
            const Mojo min_pool = static_cast<Mojo>(std::llround(
                config_.strategy.min_offer_size_units
                * static_cast<double>(pair_cfg->base_mojos_per_unit)));

            // Determine ratio mode (Neutral by default if rebalancing off).
            RatioRebalanceMode mode = RatioRebalanceMode::Neutral;
            if (config_.strategy.ratio_rebalance_enabled) {
                auto it_m = ratio_rebalance_modes_.find(pair_name);
                if (it_m != ratio_rebalance_modes_.end()) {
                    mode = it_m->second;
                }
            }
            const bool floor_bid = (mode != RatioRebalanceMode::AcquireQuote);
            const bool floor_ask = (mode != RatioRebalanceMode::AcquireBase);

            auto cat_confirmed = [&](const std::string& asset_id) -> Mojo {
                if (asset_id == "xch") return xch_confirmed_balance_;
                auto it = cached_wallet_balances_.find(asset_id);
                return (it != cached_wallet_balances_.end())
                           ? it->second.confirmed
                           : Mojo{0};
            };

            // Bid floor: requires quote-asset wallet to cover min_pool in base
            // units at the current mid.
            if (floor_bid && min_pool > 0 && avail_capital < min_pool && market_mid > 0.0
                && pair_cfg->quote_mojos_per_unit > 0)
            {
                const Mojo quote_bal = cat_confirmed(pair_cfg->quote_asset_id);
                const double quote_units =
                    static_cast<double>(quote_bal)
                    / static_cast<double>(pair_cfg->quote_mojos_per_unit);
                const double required_quote_units =
                    config_.strategy.min_offer_size_units * market_mid;
                if (quote_units >= required_quote_units) {
                    spdlog::info("[Engine] Step 7: {} bid pool floored "
                                 "{} -> {} base mojos (wallet has {:.4f} {}, "
                                 "needs {:.4f} for min {} units)",
                                 pair_name,
                                 avail_capital,
                                 min_pool,
                                 quote_units,
                                 pair_cfg->quote_asset_id,
                                 required_quote_units,
                                 config_.strategy.min_offer_size_units);
                    avail_capital = min_pool;
                }
            }

            // Ask floor: requires base-asset wallet to cover min_pool directly.
            if (floor_ask && min_pool > 0 && avail_inventory < min_pool) {
                const Mojo base_bal = cat_confirmed(pair_cfg->base_asset_id);
                if (base_bal >= min_pool) {
                    spdlog::info("[Engine] Step 7: {} ask pool floored "
                                 "{} -> {} base mojos (wallet has {:.4f} {} "
                                 ">= min {} units)",
                                 pair_name,
                                 avail_inventory,
                                 min_pool,
                                 static_cast<double>(base_bal)
                                     / static_cast<double>(pair_cfg->base_mojos_per_unit),
                                 pair_cfg->base_asset_id,
                                 config_.strategy.min_offer_size_units);
                    avail_inventory = min_pool;
                }
            }
        }

        // Fetch competing offers for gap-aware dynamic tier spacing.
        auto comp_offers = market_data_->get_competing_offers(pair_name);

        // Query per-tier fill rates from the offer log for adaptive sizing.
        LiquidityConfig ladder_cfg = liq.config();
        if (ladder_cfg.fill_rate_sizing && ladder_cfg.num_tiers > 0) {
            auto cutoff = std::chrono::system_clock::now()
                - std::chrono::hours(ladder_cfg.fill_rate_lookback_hours);
            std::string cutoff_ts = PnLTracker::timestamp_to_iso(cutoff);
            ladder_cfg.tier_fill_rates = db_->query_tier_fill_rates(
                pair_name, cutoff_ts, ladder_cfg.num_tiers);
        }

        // Generate the tier ladder.
        // When competing offers are available, uses the gap-aware overload
        // that analyses the order book for gaps and applies adverse-
        // selection-aware sizing and fill-rate-weighted sizing.
        if (!comp_offers.empty()) {
            pcs.ladder = liq.compute_ladder(
                mid_mojos, sigma, inv_ratio,
                avail_capital, avail_inventory,
                comp_offers, ladder_cfg);
        } else {
            pcs.ladder = liq.compute_ladder(
                mid_mojos, sigma, inv_ratio,
                avail_capital, avail_inventory, ladder_cfg);
        }

        // -----------------------------------------------------------------
        // Order-book competitive cap: ensure every tier is priced at least
        // as aggressively as the Nth competing offer on its side.
        //
        // Problem: outer tiers (Tier 2ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã¢â‚¬Å“5) can end up far from mid due to
        // large tier_spacing_bps, putting them *behind* existing competing
        // offers.  Those tiers are dead capital ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â nobody takes an offer at
        // 2.5% from mid when a competing offer sits at 1%.
        //
        // Fix: sort competing bids (descending) and asks (ascending).
        // For tier i, find the competing offer at rank (i + 1).  If our
        // tier price is worse, improve it to match that offer ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â± 1 tick.
        //
        // Safety floor: never tighten a tier closer than min_margin from
        // mid.  This prevents TibetSwap (0.7% fee) or other AMMs from
        // profitably arbitraging our offers.
        // -----------------------------------------------------------------
        if (!comp_offers.empty() && mid_mojos > 0) {
            // Separate competing offers by side, retaining size for
            // wall detection, and sort by quality.
            struct PricedOffer { Mojo price; Mojo size; };
            std::vector<PricedOffer> comp_bids;
            std::vector<PricedOffer> comp_asks;
            for (const auto& co : comp_offers) {
                if (co.side == Side::Bid) comp_bids.push_back({co.price, co.size});
                if (co.side == Side::Ask) comp_asks.push_back({co.price, co.size});
            }
            // Bids: best (highest) first.
            std::sort(comp_bids.begin(), comp_bids.end(),
                [](const auto& a, const auto& b) { return a.price > b.price; });
            // Asks: best (lowest) first.
            std::sort(comp_asks.begin(), comp_asks.end(),
                [](const auto& a, const auto& b) { return a.price < b.price; });

            // Wall detection threshold (mojos).  Competing offers above
            // this size are "walls" ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â we serve a different (retail) market
            // segment and should NOT undercut them.  On Chia DEX, offers
            // are atomic: small traders cannot take wall-sized offers.
            const Mojo wall_threshold_mojos = static_cast<Mojo>(std::llround(
                config_.strategy.wall_size_threshold_xch
                * static_cast<double>(kMojosPerXch)));

            // Minimum allowed spread: max(min_margin_bps, tibetswap_fee_bps).
            // The TibetSwap fee creates a natural arbitrage boundary ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â
            // any offer tighter than ~70 bps can be profitably arbed.
            const double min_floor_bps = std::max(
                pair_cfg ? pair_cfg->min_profit_margin_bps_override.value_or(
                    config_.strategy.min_profit_margin_bps) : config_.strategy.min_profit_margin_bps,
                config_.arbitrage.tibetswap_fee_bps);
            const Mojo max_bid_floor = static_cast<Mojo>(std::llround(
                static_cast<double>(mid_mojos) * (1.0 - min_floor_bps / 10000.0)));
            const Mojo min_ask_ceil  = static_cast<Mojo>(std::llround(
                static_cast<double>(mid_mojos) * (1.0 + min_floor_bps / 10000.0)));

            const Mojo tick = std::max(
                static_cast<Mojo>(1),
                static_cast<Mojo>(std::llround(
                    static_cast<double>(mid_mojos) / 10000.0)));

            int capped_bids = 0;
            int capped_asks = 0;
            int wall_skips  = 0;

            for (auto& tq : pcs.ladder) {
                const auto rank = static_cast<std::size_t>(tq.tier_index + 1);

                if (tq.side == Side::Bid && rank < comp_bids.size()) {
                    // Wall check: don't undercut massive competing offers.
                    // Our small accessible offers target retail traders who
                    // can't take the wall's atomic fill requirement.
                    if (comp_bids[rank].size > wall_threshold_mojos) {
                        spdlog::debug("[Engine] Step 7: {} BID tier {} "
                                     "wall at rank {} (size={:.3f} XCH) "
                                     "ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â skipping competitive cap",
                                     pair_name, tq.tier_index, rank,
                                     static_cast<double>(comp_bids[rank].size)
                                         / static_cast<double>(kMojosPerXch));
                        ++wall_skips;
                        continue;
                    }
                    // The competing offer at rank (tier+1) would be filled
                    // before ours.  If our price is worse (lower), improve
                    // it to match + 1 tick.
                    const Mojo target = comp_bids[rank].price + tick;
                    if (tq.price < target) {
                        // Cap: never bid above the safety floor.
                        const Mojo capped = std::min(target, max_bid_floor);
                        if (capped > tq.price) {
                            spdlog::debug("[Engine] Step 7: {} BID tier {} "
                                         "competitive cap: {} -> {} "
                                         "(rank-{} comp={})",
                                         pair_name, tq.tier_index,
                                         tq.price, capped, rank,
                                         comp_bids[rank].price);
                            tq.price = capped;
                            ++capped_bids;
                        }
                    }
                }

                if (tq.side == Side::Ask && rank < comp_asks.size()) {
                    // Wall check: skip undercutting wall-sized offers.
                    if (comp_asks[rank].size > wall_threshold_mojos) {
                        spdlog::debug("[Engine] Step 7: {} ASK tier {} "
                                     "wall at rank {} (size={:.3f} XCH) "
                                     "ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â skipping competitive cap",
                                     pair_name, tq.tier_index, rank,
                                     static_cast<double>(comp_asks[rank].size)
                                         / static_cast<double>(kMojosPerXch));
                        ++wall_skips;
                        continue;
                    }
                    // If our ask is worse (higher), improve it to match
                    // the competing ask - 1 tick.
                    const Mojo target = comp_asks[rank].price - tick;
                    if (tq.price > target) {
                        // Floor: never ask below the safety floor.
                        const Mojo capped = std::max(target, min_ask_ceil);
                        if (capped < tq.price) {
                            spdlog::debug("[Engine] Step 7: {} ASK tier {} "
                                         "competitive cap: {} -> {} "
                                         "(rank-{} comp={})",
                                         pair_name, tq.tier_index,
                                         tq.price, capped, rank,
                                         comp_asks[rank].price);
                            tq.price = capped;
                            ++capped_asks;
                        }
                    }
                }
            }

            if (capped_bids > 0 || capped_asks > 0 || wall_skips > 0) {
                spdlog::info("[Engine] Step 7: {} competitive cap adjusted "
                             "{} bids, {} asks, {} wall-skips (floor={:.0f}bps)",
                             pair_name, capped_bids, capped_asks,
                             wall_skips, min_floor_bps);
            }
        }

        // -----------------------------------------------------------------
        // Stablecoin order-book undercutting: for the innermost tier (Tier 0)
        // on each side, improve our price to beat the best *competing* offer
        // by 1 bps.  This "penny ahead" strategy captures top-of-book
        // priority on both bid and ask, while the fee-aware floor prevents
        // us from quoting unprofitably.
        //
        //   Tier 0 BID: max(our_bid, best_competing_bid + 1 bps)
        //               capped at peg * (1 - min_margin)
        //   Tier 0 ASK: min(our_ask, best_competing_ask - 1 bps)
        //               floored at peg * (1 + min_margin)
        //
        // Outer tiers are left untouched -- they provide depth and catch
        // larger moves.  comp_offers excludes our own orders, so we never
        // undercut ourselves.
        // -----------------------------------------------------------------
        if (pair_cfg && pair_cfg->is_stablecoin && pair_cfg->peg_target > 0.0
            && !comp_offers.empty())
        {
            // Find best competing bid/ask from the order book.
            Mojo best_comp_bid = 0;
            Mojo best_comp_ask = 0;
            for (const auto& co : comp_offers) {
                if (co.side == Side::Bid && co.price > best_comp_bid) {
                    best_comp_bid = co.price;
                }
                if (co.side == Side::Ask &&
                    (best_comp_ask == 0 || co.price < best_comp_ask)) {
                    best_comp_ask = co.price;
                }
            }

            // 1 bps tick step relative to the mid.
            const Mojo tick = std::max(
                static_cast<Mojo>(1),
                static_cast<Mojo>(std::llround(
                    static_cast<double>(mid_mojos) / 10000.0)));

            // Safety bounds: never push our bid above mid or our ask
            // below mid -- that would cross our own spread.
            // For stablecoin pairs, also enforce peg-based bounds
            // so we never sell below peg or buy above peg.
            Mojo max_bid_ceil  = mid_mojos - tick;
            Mojo min_ask_floor = mid_mojos + tick;

            if (pair_cfg && pair_cfg->peg_target > 0.0) {
                const double uc_mbps =
                    pair_cfg->min_profit_margin_bps_override
                        .value_or(config_.strategy.min_profit_margin_bps);
                const auto uc_peg = static_cast<Mojo>(std::llround(
                    pair_cfg->peg_target
                    * static_cast<double>(kMojosPerXch)));
                const auto peg_ask_fl = static_cast<Mojo>(std::llround(
                    static_cast<double>(uc_peg)
                    * (1.0 + uc_mbps / 10000.0)));
                const auto peg_bid_cl = static_cast<Mojo>(std::llround(
                    static_cast<double>(uc_peg)
                    * (1.0 - uc_mbps / 10000.0)));
                min_ask_floor = std::max(min_ask_floor, peg_ask_fl);
                max_bid_ceil  = std::min(max_bid_ceil, peg_bid_cl);
            }

            for (auto& tq : pcs.ladder) {
                // Undercut all tiers when configured, else only tier 0.
                if (!pair_cfg->stablecoin_undercut_all_tiers
                    && tq.tier_index != 0)
                    continue;

                if (tq.side == Side::Bid && best_comp_bid > 0) {
                    const Mojo improved = best_comp_bid + tick;
                    if (improved > tq.price && improved <= max_bid_ceil) {
                        spdlog::info("[Engine] Step 7: {} stablecoin BID "
                                     "undercut: {} -> {} (comp_best_bid={})",
                                     pair_name, tq.price, improved,
                                     best_comp_bid);
                        tq.price = improved;
                    }
                }

                if (tq.side == Side::Ask && best_comp_ask > 0) {
                    const Mojo improved = best_comp_ask - tick;
                    if (improved < tq.price && improved >= min_ask_floor) {
                        spdlog::info("[Engine] Step 7: {} stablecoin ASK "
                                     "undercut: {} -> {} (comp_best_ask={})",
                                     pair_name, tq.price, improved,
                                     best_comp_ask);
                        tq.price = improved;
                    }
                }
            }
        }

        // -----------------------------------------------------------------
        // Order-book price guard: clamp our offers so that we never post
        // a BID above the book's best ASK or an ASK below the book's best
        // BID.  This guarantees our most aggressive quote stays inside the
        // existing spread rather than crossing it (which would be a gift
        // to arbitrageurs).
        //
        // Guard rule:
        //   BID price ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â°ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¤ dex_best_ask   (never overpay beyond the cheapest seller)
        //   ASK price ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â°ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¥ dex_best_bid   (never undersell below the richest buyer)
        //
        // Tiers that violate the constraint are clamped; if a clamped tier
        // would produce a zero or negative size it is dropped entirely.
        // -----------------------------------------------------------------
        const MarketSnapshot snap = state_->get_market(pair_name);

        // SAFETY: When we have NO order-book reference at all (both sides
        // zero), we cannot validate any prices.  Refuse to post offers
        // rather than risk mispricing.
        if (snap.best_bid <= 0 && snap.best_ask <= 0) {
            spdlog::warn("[Engine] Step 7: {} no order-book reference "
                         "(bid={} ask={}) -- clearing ladder to prevent "
                         "unguarded offers", pair_name,
                         snap.best_bid, snap.best_ask);
            pcs.ladder.clear();
        }

        if (snap.best_bid > 0 || snap.best_ask > 0) {
            int clamped_bids = 0;
            int clamped_asks = 0;

            for (auto& tq : pcs.ladder) {
                if (tq.side == Side::Bid && snap.best_ask > 0) {
                    if (tq.price > snap.best_ask) {
                        spdlog::info("[Engine] Step 7: {} BID tier {} clamped "
                                     "{} -> {} (dex best ask)",
                                     pair_name, tq.tier_index,
                                     tq.price, snap.best_ask);
                        tq.price = snap.best_ask;
                        ++clamped_bids;
                    }
                }
                if (tq.side == Side::Ask && snap.best_bid > 0) {
                    if (tq.price < snap.best_bid) {
                        spdlog::info("[Engine] Step 7: {} ASK tier {} clamped "
                                     "{} -> {} (dex best bid)",
                                     pair_name, tq.tier_index,
                                     tq.price, snap.best_bid);
                        tq.price = snap.best_bid;
                        ++clamped_asks;
                    }
                }
            }

            if (clamped_bids > 0 || clamped_asks > 0) {
                spdlog::warn("[Engine] Step 7: {} order-book guard clamped "
                             "{} bids, {} asks (book bid={} ask={})",
                             pair_name, clamped_bids, clamped_asks,
                             snap.best_bid, snap.best_ask);
            }
        }

        // -----------------------------------------------------------------
        // Smooth inventory throttle (all assets, both sides): when the asset
        // consumed by a side is running low, progressively make that side
        // less competitive (wider price, smaller size) instead of waiting for
        // hard suppression in Step 8.
        //
        // Side spending map:
        //   ASK spends base asset
        //   BID spends quote asset
        //
        // Threshold policy:
        //   - XCH: use explicit xch_ask_throttle_* config levels.
        //   - Other assets: derive levels from min_trading_units /
        //     min_reserve_units to keep behavior consistent across wallets.
        // -----------------------------------------------------------------
        if (pair_cfg
            && config_.strategy.xch_ask_throttle_enabled
            && !pcs.ladder.empty())
        {
            const double aggressiveness = std::clamp(
                config_.strategy.xch_ask_throttle_aggressiveness,
                0.1, 3.0);

            auto get_confirmed_balance = [&](const std::string& asset) -> Mojo {
                if (asset == "xch") {
                    return xch_confirmed_balance_;
                }
                auto it = cached_wallet_balances_.find(asset);
                if (it != cached_wallet_balances_.end()) {
                    return it->second.confirmed;
                }
                return 0;
            };

            auto apply_side_throttle = [&](Side side,
                                           const std::string& spend_asset,
                                           Mojo spend_asset_confirmed,
                                           Mojo spend_mojos_per_unit) {
                if (spend_asset_confirmed <= 0 || spend_mojos_per_unit <= 0) {
                    return;
                }

                double caution_threshold = 0.0;
                double low_threshold = 0.0;
                double critical_threshold = 0.0;

                if (spend_asset == "xch") {
                    caution_threshold = std::max(
                        config_.strategy.xch_ask_throttle_caution_xch, 0.01);
                    low_threshold = std::max(
                        config_.strategy.xch_ask_throttle_low_xch, 0.01);
                    critical_threshold = std::max(
                        config_.strategy.xch_ask_throttle_critical_xch, 0.0);
                } else {
                    critical_threshold = std::max(
                        config_.strategy.min_reserve_units, 0.0);
                    low_threshold = std::max(
                        config_.strategy.min_trading_units,
                        critical_threshold + 0.01);
                    caution_threshold = std::max(
                        config_.strategy.min_trading_units * 2.0,
                        low_threshold + 0.01);
                }

                if (!(caution_threshold > low_threshold
                      && low_threshold > critical_threshold)) {
                    return;
                }

                const double balance_units =
                    static_cast<double>(spend_asset_confirmed)
                    / static_cast<double>(spend_mojos_per_unit);

                double size_scale = 1.0;
                double extra_bps = 0.0;
                const char* throttle_stage = "healthy";
                bool critical_mode = false;

                if (balance_units >= caution_threshold) {
                    return;
                }

                if (balance_units >= low_threshold) {
                    const double denom = std::max(
                        caution_threshold - low_threshold, 1e-9);
                    const double t = std::clamp(
                        (caution_threshold - balance_units) / denom,
                        0.0, 1.0);
                    size_scale = std::clamp(
                        1.0 - (0.10 + 0.15 * t) * aggressiveness,
                        0.25, 1.0);
                    extra_bps = (15.0 + 35.0 * t) * aggressiveness;
                    throttle_stage = "caution";
                } else if (balance_units >= critical_threshold) {
                    const double denom = std::max(
                        low_threshold - critical_threshold, 1e-9);
                    const double t = std::clamp(
                        (low_threshold - balance_units) / denom,
                        0.0, 1.0);
                    size_scale = std::clamp(
                        1.0 - (0.30 + 0.25 * t) * aggressiveness,
                        0.10, 1.0);
                    extra_bps = (60.0 + 80.0 * t) * aggressiveness;
                    throttle_stage = "low";
                } else {
                    const double denom = std::max(critical_threshold, 1e-9);
                    const double t = 1.0 - std::clamp(
                        balance_units / denom, 0.0, 1.0);
                    size_scale = std::clamp(
                        1.0 - (0.65 + 0.20 * t) * aggressiveness,
                        0.05, 1.0);
                    extra_bps = (160.0 + 140.0 * t) * aggressiveness;
                    throttle_stage = "critical";
                    critical_mode = true;
                }

                std::uint8_t max_side_tier = 0;
                for (const auto& tq : pcs.ladder) {
                    if (tq.side == side) {
                        max_side_tier = std::max(max_side_tier, tq.tier_index);
                    }
                }

                int repriced = 0;
                int resized = 0;
                int dropped = 0;
                auto it = std::remove_if(
                    pcs.ladder.begin(), pcs.ladder.end(),
                    [&](TierQuote& tq) {
                        if (tq.side != side) return false;

                        if (critical_mode && tq.tier_index < max_side_tier) {
                            ++dropped;
                            return true;
                        }

                        const Mojo old_size = tq.size;

                        if (side == Side::Ask) {
                            const Mojo widened_price = static_cast<Mojo>(
                                std::llround(
                                    static_cast<double>(tq.price)
                                    * (1.0 + extra_bps / 10000.0)));
                            if (widened_price > tq.price) {
                                tq.price = widened_price;
                                ++repriced;
                            }
                        } else {
                            const Mojo reduced_price = static_cast<Mojo>(
                                std::llround(
                                    static_cast<double>(tq.price)
                                    * (1.0 - extra_bps / 10000.0)));
                            const Mojo clamped_price = std::max(
                                static_cast<Mojo>(1), reduced_price);
                            if (clamped_price < tq.price) {
                                tq.price = clamped_price;
                                ++repriced;
                            }
                        }

                        tq.size = std::max(
                            static_cast<Mojo>(1),
                            static_cast<Mojo>(std::llround(
                                static_cast<double>(tq.size) * size_scale)));
                        if (tq.size != old_size) {
                            ++resized;
                        }

                        if (mid_mojos > 0) {
                            tq.spread_bps =
                                (static_cast<double>(tq.price)
                                 - static_cast<double>(mid_mojos))
                                / static_cast<double>(mid_mojos) * 10000.0;
                        }

                        return false;
                    });
                pcs.ladder.erase(it, pcs.ladder.end());

                if (repriced > 0 || resized > 0 || dropped > 0) {
                    spdlog::info("[Engine] Step 7: {} {}-side throttle asset={} "
                                 "stage={} balance={:.3f} caution={:.3f} low={:.3f} critical={:.3f} "
                                 "bps={:.0f} size_scale={:.2f} repriced={} resized={} dropped={}",
                                 pair_name,
                                 (side == Side::Ask) ? "ask" : "bid",
                                 spend_asset,
                                 throttle_stage,
                                 balance_units,
                                 caution_threshold, low_threshold, critical_threshold,
                                 extra_bps, size_scale,
                                 repriced, resized, dropped);
                }
            };

            apply_side_throttle(
                Side::Ask,
                pair_cfg->base_asset_id,
                get_confirmed_balance(pair_cfg->base_asset_id),
                pair_cfg->base_mojos_per_unit);

            apply_side_throttle(
                Side::Bid,
                pair_cfg->quote_asset_id,
                get_confirmed_balance(pair_cfg->quote_asset_id),
                pair_cfg->quote_mojos_per_unit);
        }

        // -----------------------------------------------------------------
        // Post-clamp no-loss re-check: The price guard may have pushed an
        // ASK price below cost basis.  Re-apply the no-loss floor on each
        // ASK tier and drop any that cannot meet the minimum.
        // -----------------------------------------------------------------
        if (pair_cfg && !pcs.ladder.empty()) {
            auto rec = inventory_->get_record(AssetId{pair_cfg->base_asset_id});
            const Mojo cost_basis = rec.weighted_avg_cost_basis;

            if (cost_basis > 0) {
                const double margin = config_.strategy.min_profit_margin_bps
                                    / 10'000.0;
                const auto min_ask = static_cast<Mojo>(std::llround(
                    static_cast<double>(cost_basis) * (1.0 + margin)));

                auto it = std::remove_if(pcs.ladder.begin(), pcs.ladder.end(),
                    [&](const TierQuote& tq) {
                        if (tq.side == Side::Ask && tq.price < min_ask) {
                            spdlog::warn("[Engine] Step 7: {} ASK tier {} "
                                         "dropped: price {} < no-loss floor "
                                         "{} (basis={} margin={:.1f}bps)",
                                         pair_name, tq.tier_index,
                                         tq.price, min_ask, cost_basis,
                                         config_.strategy.min_profit_margin_bps);
                            return true;
                        }
                        return false;
                    });
                pcs.ladder.erase(it, pcs.ladder.end());
            }
        }

        // -----------------------------------------------------------------
        // Stablecoin peg guard: hard cap BID prices below the peg and
        // floor ASK prices above the peg.  This prevents the engine from
        // ever bidding >= $1.00 (or whatever peg_target is) on a
        // stablecoin pair -- even when a crossed or noisy order-book
        // produces a mid above peg.
        // -----------------------------------------------------------------
        if (pair_cfg && pair_cfg->is_stablecoin && pair_cfg->peg_target > 0.0
            && !pcs.ladder.empty())
        {
            const double margin_bps = pair_cfg->min_profit_margin_bps_override.value_or(
                config_.strategy.min_profit_margin_bps);
            const auto peg_mojos = static_cast<Mojo>(std::llround(
                pair_cfg->peg_target * static_cast<double>(kMojosPerXch)));
            const auto min_peg_ask = static_cast<Mojo>(std::llround(
                static_cast<double>(peg_mojos) * (1.0 + margin_bps / 10'000.0)));

            int dropped_bids = 0;
            int floored_asks = 0;

            auto it = std::remove_if(pcs.ladder.begin(), pcs.ladder.end(),
                [&](TierQuote& tq) {
                    // Hard rule: never bid at or above peg on a stablecoin.
                    if (tq.side == Side::Bid && tq.price >= peg_mojos) {
                        spdlog::warn("[Engine] Step 7: {} BID tier {} peg-guard "
                                     "dropped: price {} >= peg {} "
                                     "(never bid at-or-above peg on stablecoin)",
                                     pair_name, tq.tier_index,
                                     tq.price, peg_mojos);
                        ++dropped_bids;
                        return true;  // remove
                    }
                    // Floor ASK at peg + margin.
                    if (tq.side == Side::Ask && tq.price < min_peg_ask) {
                        spdlog::info("[Engine] Step 7: {} ASK tier {} peg-guard "
                                     "clamped {} -> {} (peg={:.4f} +{:.1f}bps)",
                                     pair_name, tq.tier_index,
                                     tq.price, min_peg_ask,
                                     pair_cfg->peg_target, margin_bps);
                        tq.price = min_peg_ask;
                        ++floored_asks;
                        return false; // keep, with clamped price
                    }
                    return false;
                });
            pcs.ladder.erase(it, pcs.ladder.end());

            if (dropped_bids > 0 || floored_asks > 0) {
                spdlog::warn("[Engine] Step 7: {} peg-guard: dropped {} bids "
                             ">= peg {}, floored {} asks to {} "
                             "(margin={:.1f}bps)",
                             pair_name, dropped_bids, peg_mojos,
                             floored_asks, min_peg_ask, margin_bps);
            }
        }

        // -----------------------------------------------------------------
        // Final sanity: drop any tier with a non-positive price.
        // -----------------------------------------------------------------
        {
            auto it = std::remove_if(pcs.ladder.begin(), pcs.ladder.end(),
                [&](const TierQuote& tq) {
                    if (tq.price <= 0) {
                        spdlog::warn("[Engine] Step 7: {} {} tier {} dropped: "
                                     "non-positive price {}",
                                     pair_name,
                                     (tq.side == Side::Bid) ? "BID" : "ASK",
                                     tq.tier_index, tq.price);
                        return true;
                    }
                    return false;
                });
            pcs.ladder.erase(it, pcs.ladder.end());
        }

        // -----------------------------------------------------------------
        // Minimum offer size: drop tiers below min_offer_size_units of
        // the base asset.  Sub-unit / dust offers are economically
        // insignificant and waste XCH fee coins + wallet UTXOs.
        // Per-pair override takes precedence over global default (1.0).
        //
        // [v0.7.49+] Emergency top-tier preservation: keep tier-0 alive
        // regardless of size so the book can still show a matched top-of-
        // ladder pair when the risk model sizes below the dust threshold.
        // Step 8 still side-gates asks or bids when a wallet side is not
        // safe to post.
        // -----------------------------------------------------------------
        if (pair_cfg) {
            const bool xch_base_pair =
                (pair_cfg->base_asset_id == "xch"
                 || pair_cfg->base_asset_id == "XCH");

            // Hard band for XCH-base pairs: each tier must be in [min_size, 10] XCH.
            // This prevents emergency posting of sub-min dust tiers and
            // caps oversized tiers that over-concentrate inventory.
            // Per-pair override takes precedence over default.
            const double eff_min_units = pair_cfg->min_offer_size_units_override.value_or(
                xch_base_pair ? 1.0 : config_.strategy.min_offer_size_units);
            const Mojo min_base_mojos = static_cast<Mojo>(std::llround(
                eff_min_units * static_cast<double>(pair_cfg->base_mojos_per_unit)));
            const double eff_max_units = xch_base_pair ? 10.0 : 0.0;
            const Mojo max_base_mojos = xch_base_pair
                ? static_cast<Mojo>(std::llround(
                      eff_max_units
                      * static_cast<double>(pair_cfg->base_mojos_per_unit)))
                : 0;

            if (xch_base_pair && max_base_mojos > 0) {
                int capped_tiers = 0;
                for (auto& tq : pcs.ladder) {
                    if (tq.size > max_base_mojos) {
                        spdlog::info("[Engine] Step 7: {} {} tier {} capped: "
                                     "size {} -> {} mojos ({:.1f} XCH max)",
                                     pair_name,
                                     (tq.side == Side::Bid) ? "BID" : "ASK",
                                     tq.tier_index,
                                     tq.size,
                                     max_base_mojos,
                                     eff_max_units);
                        tq.size = max_base_mojos;
                        ++capped_tiers;
                    }
                }
                if (capped_tiers > 0) {
                    spdlog::info("[Engine] Step 7: {} capped {} tiers to "
                                 "the XCH max size ({:.1f} XCH)",
                                 pair_name, capped_tiers, eff_max_units);
                }
            }

            // Up-scale smaller tiers to min_base_mojos to support minimum offer size
            // without reducing the number of active offers (never dropping them).
            int bumped_tiers = 0;
            for (auto& tq : pcs.ladder) {
                if (tq.size < min_base_mojos) {
                    const Mojo old_size = tq.size;
                    tq.size = min_base_mojos;
                    spdlog::info("[Engine] Step 7: {} {} tier {} up-scaled to minimum size: "
                                 "{} -> {} mojos ({:.2f} units)",
                                 pair_name, (tq.side == Side::Bid) ? "BID" : "ASK",
                                 tq.tier_index, old_size, min_base_mojos, eff_min_units);
                    ++bumped_tiers;
                }
            }

            // Diagnostic: show what's in ladder before dust filtering
            uint32_t pre_bids = 0, pre_asks = 0;
            for (const auto& tq : pcs.ladder) {
                if (tq.side == Side::Bid) {
                    pre_bids++;
                } else {
                    pre_asks++;
                }
            }
            spdlog::info("[Engine] Step 7: {} PRE-DUST ladder has {} bids, {} asks (total {})",
                         pair_name, pre_bids, pre_asks, pcs.ladder.size());

            const auto pre_count = pcs.ladder.size();
            auto it = std::remove_if(pcs.ladder.begin(), pcs.ladder.end(),
                [&](const TierQuote& tq) {
                    if (tq.size < min_base_mojos) {
                        // Emergency: never drop tier-0. Preserve the top
                        // quote on both sides so matched ladders survive the
                        // dust filter; Step 8 applies the final side gate.
                        // We also preserve tier-0 for XCH base pairs to avoid
                        // deadlock when capital/inventory drops below the floor.
                        if (tq.tier_index == 0) {
                            const char* side =
                                (tq.side == Side::Bid) ? "BID" : "ASK";
                            const char* reason =
                                (tq.side == Side::Bid)
                                    ? "keeping to prevent buy deadlock"
                                    : "keeping to preserve sell presence";
                            spdlog::warn("[Engine] Step 7: {} {} tier 0 "
                                         "PRESERVED (emergency): size {} << min {} -- "
                                         "{}",
                                         pair_name, side, tq.size,
                                         min_base_mojos, reason);
                            return false;
                        }

                        spdlog::debug("[Engine] Step 7: {} {} tier {} dropped: "
                                      "size {} < min {} mojos ({:.1f} units)",
                                      pair_name,
                                      (tq.side == Side::Bid) ? "BID" : "ASK",
                                      tq.tier_index, tq.size, min_base_mojos,
                                      eff_min_units);
                        return true;
                    }
                    return false;
                });
            pcs.ladder.erase(it, pcs.ladder.end());
            if (pcs.ladder.size() < pre_count) {
                if (xch_base_pair) {
                    spdlog::info("[Engine] Step 7: {} dropped {} tiers below "
                                 "XCH min size ({:.1f} XCH = {} mojos)",
                                 pair_name, pre_count - pcs.ladder.size(),
                                 eff_min_units, min_base_mojos);
                } else {
                    spdlog::info("[Engine] Step 7: {} dropped {} dust tiers "
                                 "(min {:.1f} units = {} mojos)",
                                 pair_name, pre_count - pcs.ladder.size(),
                                 eff_min_units, min_base_mojos);
                }
            }
        }

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
        metrics_->update_stuck_offers(0);
        co_return;
    }

    int total_stuck_offers = 0;

    // -- Wallet sync gate with auto-recovery ----------------------------------
    // The Chia wallet returns unreliable data when not fully synced:
    // get_all_offers may return incomplete lists (causing
    // verify_pending_offer_coins to falsely mark live offers as NOT FOUND),
    // and cancel_offer will fail outright.  Block ALL offer management
    // until the wallet reports synced=true.
    //
    // Auto-recovery: if the wallet stays unsynced for kWalletRestartThreshold
    // consecutive blocks (~3 min), restart the wallet service.  This breaks
    // the deadlock where pending_change prevents sync and the sync gate
    // prevents the force-delete escalation from ever firing.
    try {
        auto sync_status = co_await wallet_->get_sync_status();
        bool synced = false;
        if (sync_status.contains("synced"))
            synced = sync_status["synced"].get<bool>();
        bool syncing = false;
        if (sync_status.contains("syncing"))
            syncing = sync_status["syncing"].get<bool>();

        if (!synced || syncing) {
            ++consecutive_unsynced_blocks_;
            spdlog::warn("[Engine] Step 8: wallet not fully synced "
                         "(synced={}, syncing={}, unsynced_blocks={}/{}) "
                         "-- skipping all offer management",
                         synced, syncing,
                         consecutive_unsynced_blocks_,
                         kWalletRestartThreshold);

            // Escalation: restart wallet service after prolonged unsync.
            if (consecutive_unsynced_blocks_ >= kWalletRestartThreshold) {
                spdlog::warn("[Engine] Wallet unsynced for {} consecutive "
                             "blocks (~{} sec) -- restarting wallet service "
                             "to force clean resync",
                             consecutive_unsynced_blocks_,
                             consecutive_unsynced_blocks_ * 9);
#ifdef _WIN32
                int rc = std::system("chia stop wallet & chia start wallet");
#else
                int rc = std::system("chia stop wallet && chia start wallet");
#endif
                if (rc == 0) {
                    spdlog::info("[Engine] Wallet service restart initiated");
                } else {
                    spdlog::error("[Engine] Wallet service restart failed "
                                  "(rc={})", rc);
                }
                consecutive_unsynced_blocks_ = 0;
            }
            co_return;
        }

        // Wallet is synced -- reset the unsync counter.
        if (consecutive_unsynced_blocks_ > 0) {
            spdlog::info("[Engine] Wallet re-synced after {} blocks",
                         consecutive_unsynced_blocks_);
            consecutive_unsynced_blocks_ = 0;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[Engine] Step 8: wallet sync check failed: {} "
                     "-- skipping offer management cautiously", e.what());
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

    // -- UTXO Liberation ------------------------------------------------
    // The Chia wallet locks *entire* UTXOs when creating offers.  A small
    // fee (0.005 XCH) can lock a 16 XCH UTXO, draining spendable to
    // zero even though confirmed balance is healthy.  When spendable
    // falls below the fee reserve and we have pending offers, cancel the
    // oldest ones to free locked UTXOs before processing any pair.
    //
    // Even when liberation can't restore the reserve, we still allow the
    // pair loop to run in "XCH-buy-only" mode: only offers that would
    // acquire XCH (bid on XCH-base pairs, ask on XCH-quote pairs) are
    // posted.  This prevents capital starvation from becoming permanent.
    bool xch_buy_only_mode = false;
    Mojo xch_spendable_pre = 0;
    if (config_.strategy.fee_reserve_xch > 0.0) {
        bool liberation_needed = false;
        const auto reserve_mojos = static_cast<Mojo>(std::llround(
            config_.strategy.fee_reserve_xch
            * static_cast<double>(kMojosPerXch)));
        try {
            auto xch_bal = co_await wallet_->get_wallet_balance(1);
            if (xch_bal.contains("spendable_balance"))
                xch_spendable_pre = xch_bal["spendable_balance"].get<Mojo>();
            // Liberate whenever spendable is below reserve.  The old
            // guard (confirmed >= reserve*2) created a dead zone where
            // filled offers could drain confirmed below the threshold,
            // making liberation impossible even though tracked offers
            // still locked the remaining XCH.
            liberation_needed = xch_spendable_pre < reserve_mojos;
        } catch (const std::exception& e) {
            spdlog::debug("[Engine] UTXO liberation balance check failed: {}",
                          e.what());
        }

        if (liberation_needed) {
            auto all_offers = state_->get_all_offers();
            if (all_offers.empty()) {
                // Spendable below reserve but no tracked offers to cancel.
                // Allow pair loop in XCH-buy-only mode so we can post
                // offers that acquire XCH and recover from starvation.
                spdlog::info("[Engine] UTXO liberation: spendable {:.6f} XCH "
                             "< reserve {:.4f} XCH with 0 pending offers "
                             "-- entering XCH-buy-only mode",
                             static_cast<double>(xch_spendable_pre) / kMojosPerXch,
                             config_.strategy.fee_reserve_xch);
                xch_buy_only_mode = true;
            } else {
            // Sort oldest first so we cancel stale offers first.
            std::sort(all_offers.begin(), all_offers.end(),
                [](const PendingOffer& a, const PendingOffer& b) {
                    return a.created_at_block < b.created_at_block;
                });

            // Minimum age before an offer is eligible for liberation
            // cancellation.  Without this, the engine creates offers on
            // one heartbeat and liberation cancels them on the very next
            // heartbeat because they locked UTXOs and dropped spendable
            // below the fee reserve.  5 blocks ~2-3 minutes on Chia.
            constexpr BlockHeight kMinOfferAgeBlocks = 5;

            // Filter to only stale offers.
            std::vector<PendingOffer> stale_offers;
            for (const auto& po : all_offers) {
                if (block_height >= po.created_at_block + kMinOfferAgeBlocks) {
                    stale_offers.push_back(po);
                }
            }

            if (stale_offers.empty()) {
                // All offers are fresh -- don't cancel, just enter
                // buy-only mode until they age or get filled.
                spdlog::info("[Engine] UTXO liberation: spendable {:.6f} XCH "
                             "< reserve {:.4f} XCH but all {} offers are "
                             "younger than {} blocks -- XCH-buy-only mode",
                             static_cast<double>(xch_spendable_pre) / kMojosPerXch,
                             config_.strategy.fee_reserve_xch,
                             all_offers.size(),
                             kMinOfferAgeBlocks);
                xch_buy_only_mode = true;
            } else {
            spdlog::info("[Engine] UTXO liberation: spendable {:.6f} XCH "
                         "< reserve {:.4f} XCH with {} stale offers "
                         "(of {} total) -- cancelling oldest to free "
                         "locked UTXOs",
                         static_cast<double>(xch_spendable_pre) / kMojosPerXch,
                         config_.strategy.fee_reserve_xch,
                         stale_offers.size(),
                         all_offers.size());
            constexpr int kMaxLiberate = 3;
            int liberated = 0;
            for (const auto& po : stale_offers) {
                if (liberated >= kMaxLiberate) break;
                bool ok = co_await offer_mgr_->emergency_cancel(
                    po.offer_id, "utxo_liberation",
                    /*prefer_zero_fee=*/true);
                if (ok) {
                    state_->mark_cancel_pending(po.offer_id);
                    try {
                        db_->update_offer_status(
                            po.offer_id, "cancelled",
                            block_height, "utxo_liberation");
                    } catch (const std::exception& e) {
                        spdlog::debug("[Engine] UTXO liberation "
                                      "update_offer_status failed for "
                                      "{}: {}",
                                      po.offer_id.substr(0, 12),
                                      e.what());
                    }
                    ++liberated;
                    // Re-check spendable after each cancel.
                    try {
                        auto xch2 = co_await
                            wallet_->get_wallet_balance(1);
                        if (xch2.contains("spendable_balance")) {
                            xch_spendable_pre =
                                xch2["spendable_balance"]
                                    .get<Mojo>();
                            if (xch_spendable_pre >= reserve_mojos) {
                                spdlog::info(
                                    "[Engine] UTXO liberation "
                                    "complete: spendable {:.6f} XCH "
                                    "after cancelling {} offer(s)",
                                    static_cast<double>(
                                        xch_spendable_pre)
                                        / kMojosPerXch,
                                    liberated);
                                break;
                            }
                        }
                    } catch (...) {}
                }
            }
            if (liberated > 0 && xch_spendable_pre < reserve_mojos) {
                spdlog::warn("[Engine] UTXO liberation: cancelled {} "
                             "offer(s) but spendable still {:.6f} XCH "
                             "< reserve {:.4f} XCH -- entering "
                             "XCH-buy-only mode",
                             liberated,
                             static_cast<double>(xch_spendable_pre)
                                 / kMojosPerXch,
                             config_.strategy.fee_reserve_xch);
                // Still enter pair loop in XCH-buy-only mode so we can
                // post offers that acquire XCH.  Set cooldown to avoid
                // churn on non-XCH offers.
                liberation_cooldown_ = 5;
                xch_buy_only_mode = true;
            }
            } // end else (stale offers exist)
            } // end else (non-empty offers)
        }

        // Post-liberation cooldown: after liberation cancelled offers (or
        // found none to cancel), suppress the pair loop for several
        // heartbeats to let cancel transactions confirm on-chain.
        // Without this, spendable briefly recovers above reserve when a
        // cancel confirms, the engine posts a few offers, and the next
        // heartbeat cancels them again (residual churn).
        if (liberation_cooldown_ > 0) {
            if (xch_spendable_pre >= reserve_mojos * 2) {
                spdlog::info("[Engine] UTXO liberation cooldown reset: "
                             "spendable {:.6f} XCH >= {:.4f} XCH threshold",
                             static_cast<double>(xch_spendable_pre)
                                 / kMojosPerXch,
                             config_.strategy.fee_reserve_xch * 2.0);
                liberation_cooldown_ = 0;
            } else {
                --liberation_cooldown_;
                spdlog::info("[Engine] UTXO liberation cooldown: {} "
                             "heartbeats remaining, spendable {:.6f} "
                             "XCH -- XCH-buy-only mode",
                             liberation_cooldown_,
                             static_cast<double>(xch_spendable_pre)
                                 / kMojosPerXch);
                xch_buy_only_mode = true;
            }
        }

        // UTXO-lock danger zone: spendable is above the 1x reserve (so
        // liberation doesn't trigger) but below 2x reserve.  Creating
        // ANY offer can lock the entire remaining UTXO and drain
        // spendable to zero.  Enter buy-only mode preemptively so the
        // pair loop only allows buy-XCH offers (and the offer_manager
        // enforces its own 2x floor to actually block creation).
        if (!liberation_needed && liberation_cooldown_ == 0
            && xch_spendable_pre > 0
            && xch_spendable_pre < reserve_mojos * 2) {
            spdlog::info("[Engine] UTXO-lock danger zone: spendable "
                         "{:.6f} XCH is between reserve {:.4f} and "
                         "2x reserve {:.4f} -- entering XCH-buy-only mode",
                         static_cast<double>(xch_spendable_pre) / kMojosPerXch,
                         config_.strategy.fee_reserve_xch,
                         config_.strategy.fee_reserve_xch * 2.0);
            xch_buy_only_mode = true;
        }
    }

    std::set<std::int64_t> pending_wallets_this_block;
    for (auto& [pair_name, pcs] : cycle_) {
        if (!pcs.quote_valid || pcs.ladder.empty()) continue;

        // [T3-24] Final gate: do not post offers if market data was invalid.
        if (!pcs.market_data_valid) {
            spdlog::warn("[Engine] Step 8: {} market data invalid -- "
                         "skipping offer posting", pair_name);
            continue;
        }

        // [T5-01] Selective refresh: classify existing tiers before deciding
        // whether to do a full cancel+repost or a surgical selective refresh.
        //
        // Per Gao & Wang (2020), the zero-offer gap during a full cancelÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢
        // repost cycle is the primary source of adverse selection for latent
        // market makers.  By classifying each pending tier's price deviation
        // from the current optimal, we can cancel only the mispriced tiers
        // while keeping well-priced tiers live on the order book.
        //
        // Decision matrix:
        //   - All tiers Fresh          ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ skip cancel+repost entirely.
        //   - Some tiers Stale/Expired ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ selective_cancel stale IDs, then
        //                                post_quotes for replacement tiers.
        //   - All tiers Stale/Expired  ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ fall back to full cancel_stale
        //                                (same as before).
        //   - No pending tiers at all  ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ post new ladder from scratch.
        auto tier_classes = offer_mgr_->classify_tier_staleness(
            pair_name, pcs.ladder, block_height,
            config_.strategy.offer_ttl_blocks,
            static_cast<Mojo>(std::llround(
                market_data_->get_mid_price(pair_name)
                * static_cast<double>(kMojosPerXch))),
            config_.strategy.competitive_anchor_enabled);

        bool has_pending = !tier_classes.empty();
        int fresh_count = 0, stale_count = 0, expired_count = 0;
        for (const auto& tc : tier_classes) {
            switch (tc.staleness) {
                case execution::TierStaleness::Fresh:   ++fresh_count;   break;
                case execution::TierStaleness::Stale:   ++stale_count;   break;
                case execution::TierStaleness::Expired: ++expired_count; break;
            }
        }

        std::vector<std::string> cancelled_ids;

        if (has_pending && fresh_count > 0 &&
            (stale_count + expired_count) > 0 &&
            (stale_count + expired_count) < static_cast<int>(tier_classes.size())) {
            // Selective refresh: only cancel stale/expired tiers.
            std::vector<std::string> stale_ids;
            stale_ids.reserve(stale_count + expired_count);
            for (const auto& tc : tier_classes) {
                if (tc.staleness != execution::TierStaleness::Fresh) {
                    stale_ids.push_back(tc.offer_id);
                }
            }
            cancelled_ids = co_await offer_mgr_->selective_cancel(stale_ids);
            if (!cancelled_ids.empty()) {
                spdlog::info("[Engine] Step 8: selective refresh for {} -- "
                             "cancelled {}/{} stale tiers, {} fresh tiers "
                             "remain live",
                             pair_name, cancelled_ids.size(),
                             stale_count + expired_count, fresh_count);
            }
        } else if (has_pending && fresh_count == 0) {
            // All tiers stale/expired: cancel all via selective_cancel
            // (cancel_stale only handles TTL-based expiration and would
            // miss price-deviation-stale offers within TTL).
            std::vector<std::string> all_ids;
            all_ids.reserve(tier_classes.size());
            for (const auto& tc : tier_classes) {
                all_ids.push_back(tc.offer_id);
            }
            cancelled_ids = co_await offer_mgr_->selective_cancel(all_ids);
            if (!cancelled_ids.empty()) {
                spdlog::info("[Engine] Step 8: full cancel for {} -- "
                             "all {} tiers were stale/expired",
                             pair_name, cancelled_ids.size());
            }
        } else if (has_pending && stale_count == 0 && expired_count == 0) {
            // All tiers are Fresh ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â nothing to cancel or repost.
            // Still proceed to balance gates so suppressed pairs can free
            // the capital locked by these fresh offers.
            spdlog::debug("[Engine] Step 8: {} all {} tiers fresh -- "
                          "skipping cancel+repost", pair_name, fresh_count);
        }
        // else: no pending tiers ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ post from scratch (cancelled_ids empty).

        // Build cancel-reason map from tier classifications.
        std::unordered_map<std::string, std::string> cancel_reasons;
        for (const auto& tc : tier_classes) {
            if (tc.staleness == execution::TierStaleness::Expired) {
                cancel_reasons[tc.offer_id] = "ttl_expired";
            } else if (tc.staleness == execution::TierStaleness::Stale) {
                if (tc.crossed) {
                    cancel_reasons[tc.offer_id] =
                        "crossed_mid(" + std::to_string(tc.price_deviation * 100.0)
                        .substr(0, 5) + "%)";
                } else {
                    cancel_reasons[tc.offer_id] =
                        "price_adverse(" + std::to_string(tc.price_deviation * 100.0)
                        .substr(0, 5) + "%)";
                }
            }
        }

        if (!cancelled_ids.empty()) {
            // Persist cancellation status to database.
            for (const auto& oid : cancelled_ids) {
                try {
                    auto it = cancel_reasons.find(oid);
                    const std::string reason = (it != cancel_reasons.end())
                        ? it->second : "stale";
                    db_->update_offer_status(oid, "cancelled", block_height,
                                            reason);
                } catch (const std::exception& e) {
                    spdlog::debug("[Engine] update_offer_status failed for {}: {}",
                                 oid.substr(0, 12), e.what());
                }
            }
            // T4-03: Record cancel fees in the tracker.
            if (fee_tracker_->enabled()) {
                fee_tracker_->record_fee(
                    static_cast<std::uint64_t>(cancelled_ids.size()) * recommended_fee,
                    block_height);
            }
        }

        // -- Stuck offer detection -------------------------------------------
        // Offers older than hard TTL + stuck_offer_age_blocks are considered
        // stuck (e.g. RPC cancel failed). Log them with fee info and
        // force a second cancel pass with an extended threshold.
        // Hard TTL = soft TTL ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â kHardTtlMultiplier; offers past hard TTL
        // are already classified as Expired, so "stuck" means the cancel
        // RPC itself failed on a previous attempt.
        {
            const auto all_offers = state_->get_all_offers();
            const uint32_t hard_ttl =
                config_.strategy.offer_ttl_blocks
                * execution::OfferManager::kHardTtlMultiplier;
            const uint32_t stuck_threshold =
                hard_ttl + config_.strategy.stuck_offer_age_blocks;
            int stuck_count = 0;
            for (const auto& po : all_offers) {
                if (po.pair_name != pair_name) continue;
                if (block_height > po.created_at_block &&
                    (block_height - po.created_at_block) > stuck_threshold) {
                    ++stuck_count;
                    spdlog::warn("[Engine] Stuck offer {} pair={} side={} tier={} "
                                 "age={} blocks fee={} mojos",
                                 po.offer_id.substr(0, 12), po.pair_name,
                                 to_string(po.side), po.tier,
                                 block_height - po.created_at_block,
                                 po.fee_mojos);
                }
            }
            if (stuck_count > 0) {
                spdlog::warn("[Engine] Step 8: {} stuck offers for {} -- "
                             "attempting forced cancel", stuck_count, pair_name);
                auto stuck_cancelled = co_await offer_mgr_->cancel_stale(
                    pair_name, block_height, stuck_threshold);
                for (const auto& oid : stuck_cancelled) {
                    try {
                        db_->update_offer_status(oid, "cancelled", block_height,
                                                "stuck");
                    } catch (const std::exception& e) {
                        spdlog::debug("[Engine] update_offer_status failed for {}: {}",
                                     oid.substr(0, 12), e.what());
                    }
                }
            }
            total_stuck_offers += stuck_count;
        }

        // -- Spendable reserve & pending-change gating ----------------------
        // Query wallet balances for each wallet involved in this pair.
        // - pending_change > 0: suppress the side backed by that wallet.
        //   If both sides are pending, posting is skipped for this pair.
        // - Side-aware minimum balance: suppress ASK when base < reserve,
        //   suppress BID when quote < reserve.  When auto_rebalance is on
        //   and an asset is below min_trading_units, only the acquisition
        //   side is posted to restore the balance.
        bool can_bid = true;   // need quote to bid (buy base)
        bool can_ask = true;   // need base to ask (sell base)
        Mojo pair_base_spendable = 0;
        Mojo pair_quote_spendable = 0;
        bool pair_base_balance_known = false;
        bool pair_quote_balance_known = false;
        Mojo pair_base_pending_spend = 0;
        Mojo pair_quote_pending_spend = 0;
        Mojo pair_base_reserve_mojos = 0;
        Mojo pair_quote_reserve_mojos = 0;

        // XCH-buy-only mode: when UTXO liberation couldn't restore the
        // fee reserve, only allow pairs that can acquire XCH, and only
        // on the side that buys XCH.  Skip non-XCH pairs entirely.
        // Also skip if spendable is near-zero: creating any offer
        // (even buy-XCH) requires locking an XCH UTXO for the fee.
        if (xch_buy_only_mode) {
            // Hard floor: if spendable can't cover even one fee coin
            // (fee_min_spendable_xch), skip entirely.  This prevents
            // creating offers that lock the last dust UTXO.
            if (xch_spendable_pre <
                static_cast<Mojo>(std::llround(
                    config_.strategy.fee_min_spendable_xch
                    * static_cast<double>(kMojosPerXch)))) {
                spdlog::info("[Engine] Step 8: {} skipped -- XCH-buy-only "
                             "mode but spendable {:.6f} XCH < min fee "
                             "{:.4f} XCH",
                             pair_name,
                             static_cast<double>(xch_spendable_pre) / kMojosPerXch,
                             config_.strategy.fee_min_spendable_xch);
                continue;
            }
            const PairConfig* buypc = find_pair_config(pair_name);
            const bool base_is_xch  = buypc && buypc->base_asset_id  == "xch";
            const bool quote_is_xch = buypc && buypc->quote_asset_id == "xch";
            if (base_is_xch) {
                can_ask = false;  // only bid (buy XCH)
            } else if (quote_is_xch) {
                can_bid = false;  // only ask (receive XCH)
            } else {
                // Non-XCH pair: skip unless stablecoin_exempt_buyonly.
                if (!buypc || !buypc->stablecoin_exempt_buyonly) {
                    spdlog::debug("[Engine] Step 8: {} skipped -- XCH-buy-only "
                                  "mode (no XCH side)", pair_name);
                    continue;
                }
                // Stablecoin exempt: allow normal bid+ask posting.
                spdlog::info("[Engine] Step 8: {} exempt from XCH-buy-only "
                             "(stablecoin_exempt_buyonly=true)", pair_name);
            }
        }

        {
            bool saw_pending_change = false;
            const PairConfig* gate_pc = find_pair_config(pair_name);
            if (gate_pc) {
                auto bwid = offer_mgr_->resolve_wallet_id(gate_pc->base_asset_id);
                auto qwid = offer_mgr_->resolve_wallet_id(gate_pc->quote_asset_id);

                struct SideBalance {
                    std::string label;
                    std::int64_t wid;
                    std::int64_t mojos_per_unit;
                    bool is_base;
                };
                std::vector<SideBalance> sides;
                if (bwid > 0)
                    sides.push_back({gate_pc->base_asset_id, bwid,
                                     gate_pc->base_mojos_per_unit, true});
                if (qwid > 0)
                    sides.push_back({gate_pc->quote_asset_id, qwid,
                                     gate_pc->quote_mojos_per_unit, false});

                for (const auto& sb : sides) {
                    try {
                        auto bal_json = co_await wallet_->get_wallet_balance(sb.wid);
                        Mojo spendable = 0, confirmed = 0, pending = 0;
                        if (bal_json.contains("spendable_balance"))
                            spendable = bal_json["spendable_balance"].get<Mojo>();
                        if (bal_json.contains("confirmed_wallet_balance"))
                            confirmed = bal_json["confirmed_wallet_balance"].get<Mojo>();
                        if (bal_json.contains("pending_change"))
                            pending = bal_json["pending_change"].get<Mojo>();

                        if (sb.is_base) {
                            pair_base_spendable = spendable;
                            pair_base_balance_known = true;
                        } else {
                            pair_quote_spendable = spendable;
                            pair_quote_balance_known = true;
                        }

                        // Update the cache for metrics.
                        cached_wallet_balances_[sb.label] = {spendable, confirmed, pending};
                        const AssetId tracked_asset{sb.label};
                        if (confirmed > 0 && inventory_
                            && inventory_->net_inventory(tracked_asset) == 0)
                        {
                            inventory_->seed_position(tracked_asset, confirmed,
                                                      Mojo{1});
                            spdlog::warn("[Engine] Step 8: recovered inventory "
                                         "seed for asset {} from wallet "
                                         "confirmed balance {} mojos",
                                         sb.label, confirmed);
                        }
                        if (confirmed > 0 && state_
                            && state_->get_position(tracked_asset).balance == 0)
                        {
                            state_->record_buy(tracked_asset, confirmed, Mojo{1});
                            spdlog::warn("[Engine] Step 8: recovered state "
                                         "position for asset {} from wallet "
                                         "confirmed balance {} mojos",
                                         sb.label, confirmed);
                        }
                        metrics_->update_spendable_reserve(
                            sb.label,
                            (confirmed > 0)
                                ? static_cast<double>(spendable) / static_cast<double>(confirmed)
                                : 1.0);

                        // Gate 1: pending_change > 0 means coins are in-flight.
                        // Suppress only the side that spends this wallet's
                        // asset, allowing the opposite side to keep quoting.
                        if (pending > 0) {
                            spdlog::info("[Engine] Step 8: {} wallet {} has "
                                         "pending_change={} -- suppressing {} "
                                         "side until confirmed",
                                         pair_name, sb.label, pending,
                                         sb.is_base ? "ask" : "bid");

                            // Track consecutive blocks with pending_change.
                            pending_wallets_this_block.insert(sb.wid);
                            if (!saw_pending_change) {
                                ++consecutive_pending_blocks_;
                                saw_pending_change = true;
                            }

                            // Periodic stuck-tx pruning.
                            if (block_height >= last_stuck_tx_prune_block_
                                                + kStuckTxPruneInterval) {
                                last_stuck_tx_prune_block_ = block_height;
                                try {
                                    auto pruned = co_await
                                        offer_mgr_->prune_stuck_transactions(
                                            {sb.wid}, /*max_age_seconds=*/600);
                                    if (pruned > 0) {
                                        spdlog::info("[Engine] Step 8: pruned "
                                                     "stuck transactions from "
                                                     "wallet {} ({})",
                                                     sb.label, sb.wid);
                                    }
                                } catch (const std::exception& e) {
                                    spdlog::warn("[Engine] Step 8: stuck tx "
                                                 "prune failed for {}: {}",
                                                 sb.label, e.what());
                                }
                            }

                            // Escalation: force-delete if stuck too long.
                            // Iterate ALL wallets with pending_change this
                            // block, not just the current one.  A single
                            // global counter can fire while examining any
                            // wallet; clearing only that wallet leaves the
                            // others stuck indefinitely.
                            if (consecutive_pending_blocks_ >=
                                kForceDeletePendingBlocks) {
                                spdlog::warn("[Engine] Step 8: pending_change "
                                             "persisted for {} consecutive "
                                             "blocks -- force-deleting ALL "
                                             "unconfirmed transactions for "
                                             "{} pending wallets",
                                             consecutive_pending_blocks_,
                                             pending_wallets_this_block.size());
                                for (auto pw : pending_wallets_this_block) {
                                    try {
                                        co_await wallet_->
                                            delete_unconfirmed_transactions(pw);
                                        spdlog::info("[Engine] Step 8: "
                                                     "force-deleted unconfirmed "
                                                     "transactions for wallet "
                                                     "id {}", pw);
                                    } catch (const std::exception& e) {
                                        spdlog::warn("[Engine] Step 8: "
                                                     "force-delete failed for "
                                                     "wallet id {}: {}",
                                                     pw, e.what());
                                    }
                                }
                                consecutive_pending_blocks_ = 0;
                            }

                            if (sb.is_base) can_ask = false;
                            else            can_bid = false;
                            continue;
                        }

                        // Gate 2: spendable reserve too low (fractional).
                        // [v0.7.38] Now applies to ALL wallets including XCH.
                        // The old exemption (sb.wid != 1) allowed the engine
                        // to keep posting sell-side offers until XCH was nearly
                        // depleted.  The Step 7 wallet-balance cap prevents
                        // oversized offers, and this gate provides the ratio-
                        // based suppression for all assets uniformly.
                        if (confirmed > 0) {
                            double ratio = static_cast<double>(spendable)
                                         / static_cast<double>(confirmed);
                            if (ratio < config_.strategy.min_spendable_reserve_pct) {
                                spdlog::info("[Engine] Step 8: {} spendable reserve "
                                             "{:.1f}% < {:.1f}% threshold -- "
                                             "suppressing {} side",
                                             sb.label, ratio * 100.0,
                                             config_.strategy.min_spendable_reserve_pct * 100.0,
                                             sb.is_base ? "ask" : "bid");
                                if (sb.is_base) can_ask = false;
                                else            can_bid = false;
                            }
                        }

                        // Gate 3: minimum balance management (unit-based).
                        const auto reserve_mojos = static_cast<Mojo>(std::llround(
                            config_.strategy.min_reserve_units
                            * static_cast<double>(sb.mojos_per_unit)));
                        const auto trading_mojos = static_cast<Mojo>(std::llround(
                            config_.strategy.min_trading_units
                            * static_cast<double>(sb.mojos_per_unit)));

                        if (spendable < reserve_mojos) {
                            // Below absolute reserve: suppress the sell side
                            // for this asset to prevent full depletion.
                            spdlog::info("[Engine] Step 8: {} balance {:.3f} < "
                                         "reserve {:.3f} -- suppressing {} side",
                                         sb.label,
                                         static_cast<double>(spendable) / sb.mojos_per_unit,
                                         config_.strategy.min_reserve_units,
                                         sb.is_base ? "ask" : "bid");
                            if (sb.is_base) can_ask = false;
                            else            can_bid = false;
                        } else if (spendable < trading_mojos
                                   && config_.strategy.auto_rebalance_enabled) {
                            // Below trading minimum with auto-rebalance:
                            // still suppress sell side, keep buy side to
                            // acquire more of this asset.
                            spdlog::info("[Engine] Step 8: {} balance {:.3f} < "
                                         "trading min {:.3f} -- auto-rebalance: "
                                         "suppressing {} side to acquire more",
                                         sb.label,
                                         static_cast<double>(spendable) / sb.mojos_per_unit,
                                         config_.strategy.min_trading_units,
                                         sb.is_base ? "ask" : "bid");
                            if (sb.is_base) can_ask = false;
                            else            can_bid = false;
                        }

                        if (sb.is_base) {
                            pair_base_reserve_mojos = reserve_mojos;
                        } else {
                            pair_quote_reserve_mojos = reserve_mojos;
                        }
                    } catch (const std::exception& e) {
                        spdlog::warn("[Engine] Step 8: balance query failed for "
                                     "wallet {} ({}): {}", sb.label, sb.wid, e.what());
                        // On failure, suppress both sides conservatively.
                        can_bid = false;
                        can_ask = false;
                        break;
                    }
                }

                // Pending exposure guard (existing offers): if all currently
                // pending same-side offers were filled, do we dip below reserve?
                // If yes, proactively cancel the least competitive offers first.
                if (config_.strategy.auto_rebalance_enabled) {
                    struct ExposureCandidate {
                        std::string offer_id;
                        Mojo spend_cost{0};
                        std::uint8_t tier{0};
                        int staleness_rank{2};
                        Mojo price{0};
                    };
                    auto ceil_to_mojo = [](long double value) -> Mojo {
                        if (!std::isfinite(static_cast<double>(value)) || value <= 0.0L) {
                            return 0;
                        }
                        const long double cap = static_cast<long double>(
                            std::numeric_limits<Mojo>::max());
                        if (value >= cap) {
                            return std::numeric_limits<Mojo>::max();
                        }
                        return static_cast<Mojo>(std::ceil(value));
                    };
                    auto quote_cost_for_base_size = [&](Mojo base_size, Mojo price) -> Mojo {
                        const long double base_mpu = static_cast<long double>(
                            gate_pc->base_mojos_per_unit > 0 ? gate_pc->base_mojos_per_unit : 1);
                        const long double quote_mpu = static_cast<long double>(
                            gate_pc->quote_mojos_per_unit > 0 ? gate_pc->quote_mojos_per_unit : 1);
                        const long double price_scale = static_cast<long double>(kMojosPerXch);
                        return ceil_to_mojo(
                            static_cast<long double>(base_size)
                            * static_cast<long double>(price)
                            * quote_mpu / (base_mpu * price_scale));
                    };

                    std::unordered_map<std::string, execution::TierClassification> tc_by_id;
                    tc_by_id.reserve(tier_classes.size());
                    for (const auto& tc : tier_classes) {
                        tc_by_id.emplace(tc.offer_id, tc);
                    }

                    std::vector<ExposureCandidate> ask_candidates;
                    std::vector<ExposureCandidate> bid_candidates;
                    const auto all_pending = state_->get_all_offers();
                    for (const auto& po : all_pending) {
                        if (po.pair_name != pair_name || po.cancel_pending) {
                            continue;
                        }

                        int staleness_rank = 2;
                        Mojo class_price = po.price;
                        auto tc_it = tc_by_id.find(po.offer_id);
                        if (tc_it != tc_by_id.end()) {
                            switch (tc_it->second.staleness) {
                                case execution::TierStaleness::Expired:
                                    staleness_rank = 0;
                                    break;
                                case execution::TierStaleness::Stale:
                                    staleness_rank = 1;
                                    break;
                                case execution::TierStaleness::Fresh:
                                default:
                                    staleness_rank = 2;
                                    break;
                            }
                        }

                        if (po.side == Side::Ask) {
                            const Mojo spend = po.size;
                            if (spend > 0) {
                                pair_base_pending_spend += spend;
                                ask_candidates.push_back(
                                    {po.offer_id, spend, po.tier, staleness_rank, class_price});
                            }
                        } else {
                            const Mojo spend = quote_cost_for_base_size(po.size, po.price);
                            if (spend > 0) {
                                pair_quote_pending_spend += spend;
                                bid_candidates.push_back(
                                    {po.offer_id, spend, po.tier, staleness_rank, class_price});
                            }
                        }
                    }

                    const Mojo ex_mid = static_cast<Mojo>(std::llround(
                        market_data_->get_mid_price(pair_name)
                        * static_cast<double>(kMojosPerXch)));
                    auto farther_from_mid = [&](Side side, Mojo price_a, Mojo price_b) {
                        if (ex_mid == 0) {
                            if (side == Side::Ask) {
                                return price_a > price_b;
                            }
                            return price_a < price_b;
                        }
                        const Mojo da = (price_a > ex_mid) ? (price_a - ex_mid) : (ex_mid - price_a);
                        const Mojo db = (price_b > ex_mid) ? (price_b - ex_mid) : (ex_mid - price_b);
                        return da > db;
                    };
                    auto sort_candidates = [&](std::vector<ExposureCandidate>& cands, Side side) {
                        std::sort(cands.begin(), cands.end(),
                                  [&](const ExposureCandidate& a, const ExposureCandidate& b) {
                            if (a.staleness_rank != b.staleness_rank)
                                return a.staleness_rank < b.staleness_rank;
                            if (a.tier != b.tier)
                                return a.tier > b.tier;
                            if (a.price != b.price)
                                return farther_from_mid(side, a.price, b.price);
                            return a.offer_id < b.offer_id;
                        });
                    };

                    if (pair_base_balance_known && pair_base_reserve_mojos > 0
                        && pair_base_spendable > 0 && pair_base_pending_spend > 0) {
                        const Mojo projected_after_fill =
                            (pair_base_spendable > pair_base_pending_spend)
                                ? (pair_base_spendable - pair_base_pending_spend)
                                : Mojo{0};
                        if (projected_after_fill < pair_base_reserve_mojos) {
                            Mojo need_to_free = pair_base_reserve_mojos - projected_after_fill;
                            sort_candidates(ask_candidates, Side::Ask);
                            std::vector<std::string> cancel_ids;
                            Mojo freed = 0;
                            for (const auto& cand : ask_candidates) {
                                cancel_ids.push_back(cand.offer_id);
                                freed += cand.spend_cost;
                                if (freed >= need_to_free) {
                                    break;
                                }
                            }
                            if (!cancel_ids.empty()) {
                                auto cancelled = co_await offer_mgr_->selective_cancel(cancel_ids);
                                if (!cancelled.empty()) {
                                    pair_base_pending_spend = (pair_base_pending_spend > freed)
                                        ? (pair_base_pending_spend - freed)
                                        : Mojo{0};
                                    can_ask = false;
                                    spdlog::warn("[Engine] Step 8: {} pending exposure on {} "
                                                 "breached reserve (spendable={} pending={} "
                                                 "reserve={}) -- cancelled {} ask offers "
                                                 "to rebalance exposure",
                                                 pair_name,
                                                 gate_pc->base_asset_id,
                                                 pair_base_spendable,
                                                 pair_base_pending_spend,
                                                 pair_base_reserve_mojos,
                                                 cancelled.size());
                                    for (const auto& oid : cancelled) {
                                        try {
                                            db_->update_offer_status(
                                                oid, "cancelled", block_height,
                                                "exposure_floor_rebalance");
                                        } catch (...) {}
                                    }
                                }
                            }
                        }
                    }

                    if (pair_quote_balance_known && pair_quote_reserve_mojos > 0
                        && pair_quote_spendable > 0 && pair_quote_pending_spend > 0) {
                        const Mojo projected_after_fill =
                            (pair_quote_spendable > pair_quote_pending_spend)
                                ? (pair_quote_spendable - pair_quote_pending_spend)
                                : Mojo{0};
                        if (projected_after_fill < pair_quote_reserve_mojos) {
                            Mojo need_to_free = pair_quote_reserve_mojos - projected_after_fill;
                            sort_candidates(bid_candidates, Side::Bid);
                            std::vector<std::string> cancel_ids;
                            Mojo freed = 0;
                            for (const auto& cand : bid_candidates) {
                                cancel_ids.push_back(cand.offer_id);
                                freed += cand.spend_cost;
                                if (freed >= need_to_free) {
                                    break;
                                }
                            }
                            if (!cancel_ids.empty()) {
                                auto cancelled = co_await offer_mgr_->selective_cancel(cancel_ids);
                                if (!cancelled.empty()) {
                                    pair_quote_pending_spend = (pair_quote_pending_spend > freed)
                                        ? (pair_quote_pending_spend - freed)
                                        : Mojo{0};
                                    can_bid = false;
                                    spdlog::warn("[Engine] Step 8: {} pending exposure on {} "
                                                 "breached reserve (spendable={} pending={} "
                                                 "reserve={}) -- cancelled {} bid offers "
                                                 "to rebalance exposure",
                                                 pair_name,
                                                 gate_pc->quote_asset_id,
                                                 pair_quote_spendable,
                                                 pair_quote_pending_spend,
                                                 pair_quote_reserve_mojos,
                                                 cancelled.size());
                                    for (const auto& oid : cancelled) {
                                        try {
                                            db_->update_offer_status(
                                                oid, "cancelled", block_height,
                                                "exposure_floor_rebalance");
                                        } catch (...) {}
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // (counter reset moved to after the pair loop)
            if (!can_bid && !can_ask) {
                // Both sides suppressed -- cancel STALE/EXPIRED offers
                // for this pair to free locked UTXOs / capital.
                // IMPORTANT: do NOT cancel Fresh offers.  Creating an
                // offer locks XCH UTXOs, which can drop the spendable
                // reserve ratio below the threshold on the very next
                // heartbeat.  Cancelling them would waste the creation
                // fee and start a create->suppress->cancel->create churn
                // cycle.  Fresh offers will naturally expire via TTL.
                if (has_pending) {
                    std::vector<std::string> stale_ids;
                    for (const auto& tc : tier_classes) {
                        if (tc.staleness != execution::TierStaleness::Fresh)
                            stale_ids.push_back(tc.offer_id);
                    }
                    if (!stale_ids.empty()) {
                        auto freed = co_await offer_mgr_->selective_cancel(stale_ids);
                        if (!freed.empty()) {
                            spdlog::info("[Engine] Step 8: {} both sides suppressed "
                                         "-- cancelled {} stale offers to free "
                                         "locked capital ({} fresh kept live)",
                                         pair_name, freed.size(), fresh_count);
                            for (const auto& oid : freed) {
                                try {
                                    db_->update_offer_status(oid, "cancelled",
                                                            block_height,
                                                            "suppressed_capital_free");
                                } catch (...) {}
                            }
                        }
                    } else {
                        spdlog::info("[Engine] Step 8: {} both sides suppressed "
                                     "but all {} offers are fresh -- keeping "
                                     "live to prevent churn", pair_name,
                                     fresh_count);
                    }
                }
                spdlog::info("[Engine] Step 8: {} both sides suppressed -- "
                             "skipping offer posting", pair_name);
                continue;
            }
        }

        // All tiers fresh and nothing was cancelled ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ no repost needed.
        // (The early-continue was removed so balance gates can free capital
        //  when both sides are suppressed, but when at least one side is
        //  active, existing fresh offers are kept as-is.)
        if (has_pending && cancelled_ids.empty()
            && stale_count == 0 && expired_count == 0) {
            continue;
        }

        // -- XCH fee reserve gate (pre-creation, UTXO-aware) ---------------
        // The Chia wallet locks entire UTXOs when creating offers -- a small
        // offer can lock a large coin, and even non-XCH offers lock XCH for
        // fee coins.  Check actual XCH spendable before posting any offers
        // for this pair.  Uses fee_reserve_xch (the full reserve) to prevent
        // UTXO locking from draining the reserve across multiple pairs.
        // Previously used fee_min_spendable_xch (0.01), but that was too
        // permissive: pair 2+ could drain the reserve left by pair 1.
        //
        // Exception: offers that BUY XCH are always allowed through, since
        // executing them will increase the XCH balance and help recover
        // from capital starvation.  For XCH-base pairs, bid buys XCH;
        // for XCH-quote pairs, ask buys XCH.
        if (config_.strategy.fee_reserve_xch > 0.0) {
            try {
                auto xch_bal = co_await wallet_->get_wallet_balance(1);
                Mojo xch_spendable = 0;
                if (xch_bal.contains("spendable_balance"))
                    xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
                const auto xch_min = static_cast<Mojo>(std::llround(
                    config_.strategy.fee_reserve_xch
                    * static_cast<double>(kMojosPerXch)));
                if (xch_spendable < xch_min) {
                    // Check if this pair can acquire XCH on one side.
                    const PairConfig* fee_pc = find_pair_config(pair_name);
                    const bool xch_is_base  = fee_pc && fee_pc->base_asset_id  == "xch";
                    const bool xch_is_quote = fee_pc && fee_pc->quote_asset_id == "xch";

                    if (xch_is_base) {
                        // Bid buys base (XCH) ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â allow bids, suppress asks.
                        can_ask = false;
                        spdlog::info("[Engine] Step 8: {} XCH fee reserve gate: "
                                     "spendable {:.6f} XCH < reserve {:.4f} XCH "
                                     "-- allowing buy-XCH (bid) side only",
                                     pair_name,
                                     static_cast<double>(xch_spendable) / kMojosPerXch,
                                     config_.strategy.fee_reserve_xch);
                    } else if (xch_is_quote) {
                        // Ask sells base, receives quote (XCH) ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â allow asks, suppress bids.
                        can_bid = false;
                        spdlog::info("[Engine] Step 8: {} XCH fee reserve gate: "
                                     "spendable {:.6f} XCH < reserve {:.4f} XCH "
                                     "-- allowing buy-XCH (ask) side only",
                                     pair_name,
                                     static_cast<double>(xch_spendable) / kMojosPerXch,
                                     config_.strategy.fee_reserve_xch);
                    } else {
                        // Neither side acquires XCH ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â skip entirely.
                        spdlog::info("[Engine] Step 8: {} XCH fee reserve gate: "
                                     "spendable {:.6f} XCH < reserve {:.4f} XCH "
                                     "-- skipping offer posting",
                                     pair_name,
                                     static_cast<double>(xch_spendable) / kMojosPerXch,
                                     config_.strategy.fee_reserve_xch);
                        continue;
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("[Engine] Step 8: {} XCH fee reserve gate: "
                             "balance check failed: {} -- skipping cautiously",
                             pair_name, e.what());
                continue;
            }
        }

        // Find the PairConfig for this pair (O(1) map lookup).
        const PairConfig* pair_cfg = find_pair_config(pair_name);
        if (!pair_cfg) continue;

        // -- Ratio rebalance mode (hysteresis) -----------------------------
        // Target-ratio controller around strategy.ratio_target.
        // - AcquireBase  => only post bids (buy base)
        // - AcquireQuote => only post asks (sell base / receive quote)
        if (config_.strategy.ratio_rebalance_enabled) {
            const double rb_mid = market_data_->get_mid_price(pair_name);
            if (rb_mid > 0.0) {
                const Mojo rb_mid_mojos = static_cast<Mojo>(std::llround(
                    rb_mid * static_cast<double>(kMojosPerXch)));
                const double inv_ratio_rb = inventory_->inventory_ratio(
                    AssetId{pair_cfg->base_asset_id},
                    AssetId{pair_cfg->quote_asset_id},
                    rb_mid_mojos);

                auto it_mode = ratio_rebalance_modes_.find(pair_name);
                if (it_mode == ratio_rebalance_modes_.end()) {
                    it_mode = ratio_rebalance_modes_.emplace(
                        pair_name, RatioRebalanceMode::Neutral).first;
                }
                const RatioRebalanceMode prev_mode = it_mode->second;

                double target = config_.strategy.ratio_target;
                if (auto it_target = config_.strategy.ratio_target_by_pair.find(pair_name);
                    it_target != config_.strategy.ratio_target_by_pair.end()) {
                    target = it_target->second;
                }
                double enter_band = config_.strategy.ratio_band_enter;
                double exit_band = config_.strategy.ratio_band_exit;
                if (auto it_band = config_.strategy.ratio_band_enter_by_pair.find(pair_name);
                    it_band != config_.strategy.ratio_band_enter_by_pair.end()) {
                    enter_band = it_band->second;
                    exit_band = std::min(exit_band, enter_band * 0.5);
                    if (exit_band <= 0.0) {
                        exit_band = enter_band * 0.5;
                    }
                }

                const double upper_enter = target + enter_band;
                const double lower_enter = target - enter_band;
                const double upper_exit = target + exit_band;
                const double lower_exit = target - exit_band;

                RatioRebalanceMode next_mode = prev_mode;
                if (prev_mode == RatioRebalanceMode::Neutral) {
                    if (inv_ratio_rb >= upper_enter) {
                        next_mode = RatioRebalanceMode::AcquireQuote;
                    } else if (inv_ratio_rb <= lower_enter) {
                        next_mode = RatioRebalanceMode::AcquireBase;
                    }
                } else if (inv_ratio_rb >= lower_exit && inv_ratio_rb <= upper_exit) {
                    next_mode = RatioRebalanceMode::Neutral;
                }

                if (next_mode != prev_mode) {
                    it_mode->second = next_mode;
                    auto mode_name = [](RatioRebalanceMode m) {
                        switch (m) {
                            case RatioRebalanceMode::Neutral:
                                return "Neutral";
                            case RatioRebalanceMode::AcquireBase:
                                return "AcquireBase";
                            case RatioRebalanceMode::AcquireQuote:
                                return "AcquireQuote";
                        }
                        return "Unknown";
                    };
                    spdlog::info("[Engine] Step 8: {} ratio mode {} -> {} "
                                 "(ratio={:.3f} target={:.3f} enter=+/-{:.3f} "
                                 "exit=+/-{:.3f})",
                                 pair_name,
                                 mode_name(prev_mode),
                                 mode_name(next_mode),
                                 inv_ratio_rb,
                                 target,
                                 enter_band,
                                 exit_band);
                }

                if (config_.strategy.ratio_force_one_sided) {
                    const RatioRebalanceMode active_mode = it_mode->second;
                    auto mode_name = [](RatioRebalanceMode m) {
                        switch (m) {
                            case RatioRebalanceMode::Neutral:
                                return "Neutral";
                            case RatioRebalanceMode::AcquireBase:
                                return "AcquireBase";
                            case RatioRebalanceMode::AcquireQuote:
                                return "AcquireQuote";
                        }
                        return "Unknown";
                    };
                    if (active_mode == RatioRebalanceMode::AcquireBase) {
                        can_ask = false;
                    } else if (active_mode == RatioRebalanceMode::AcquireQuote) {
                        can_bid = false;
                    }
                    if (active_mode != RatioRebalanceMode::Neutral) {
                        spdlog::info("[Engine] Step 8: {} ratio mode {} "
                                     "enforced (can_bid={}, can_ask={})",
                                     pair_name,
                                     mode_name(active_mode),
                                     can_bid,
                                     can_ask);
                    }
                }
            }
        }

        // -- T4-03: Fee-vs-gain gating per tier ------------------------------
        // Estimate expected gain for each tier and filter out tiers where
        // the fee exceeds the configured ratio of expected gain.
        // Use market mid for the gain calculation (avoids A-S skew bias).
        const Mojo mid = static_cast<Mojo>(std::llround(
            market_data_->get_mid_price(pair_name)
            * static_cast<double>(kMojosPerXch)));

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
                // tier.size is in base-asset mojos; scale up by
                // kMojosPerXch / base_mojos_per_unit so that CAT mojos
                // (1e3/unit) are comparable to XCH-mojo fees (1e12/unit).
                const double gain_mojos =
                    gain_fraction * static_cast<double>(tier.size)
                    * static_cast<double>(kMojosPerXch)
                    / static_cast<double>(pair_cfg->base_mojos_per_unit);

                const auto expected_gain = static_cast<std::uint64_t>(
                    std::max(0.0, gain_mojos));

                if (fee_tracker_->should_post_offer(
                        expected_gain, recommended_fee, block_height)) {
                    fee_filtered_tiers.push_back(tier);
                } else {
                    spdlog::info("[Engine] Step 8: {} {} tier {} skipped "
                                 "(round-trip fee {}x{:.1f} > {:.0f}% of gain {})",
                                 pair_name,
                                 (tier.side == Side::Bid) ? "bid" : "ask",
                                 tier.tier_index,
                                 recommended_fee,
                                 config_.fees.cancel_cost_multiplier,
                                 config_.fees.fee_to_gain_max_ratio * 100.0,
                                 expected_gain);
                }
            }
        } else {
            // Fee tracking disabled or mid is 0 -- pass all tiers through.
            fee_filtered_tiers = pcs.ladder;
        }

        // In one-sided posting mode (e.g. pending_change on one wallet side),
        // the fee gate can occasionally filter out every remaining tier,
        // creating a no-quote gap on the only allowed side. Keep the tightest
        // eligible tier as a continuity fallback instead of withdrawing fully.
        if (fee_tracker_->enabled() && mid > 0
            && fee_filtered_tiers.empty()
            && (can_bid != can_ask)) {
            auto fallback_it = pcs.ladder.end();
            for (auto it = pcs.ladder.begin(); it != pcs.ladder.end(); ++it) {
                const bool side_allowed =
                    (it->side == Side::Bid && can_bid)
                    || (it->side == Side::Ask && can_ask);
                if (!side_allowed) {
                    continue;
                }
                if (fallback_it == pcs.ladder.end()
                    || it->tier_index < fallback_it->tier_index) {
                    fallback_it = it;
                }
            }
            if (fallback_it != pcs.ladder.end()) {
                fee_filtered_tiers.push_back(*fallback_it);
                spdlog::warn("[Engine] Step 8: {} fee-vs-gain fallback kept {} "
                             "tier {} in one-sided mode (can_bid={}, can_ask={})",
                             pair_name,
                             (fallback_it->side == Side::Bid) ? "bid" : "ask",
                             fallback_it->tier_index,
                             can_bid,
                             can_ask);
            }
        }

        // -----------------------------------------------------------------
        // Dynamic tier limiting (Gueant-Lehalle-Fernandez-Tapia 2013,
        // Cartea-Jaimungal-Penalva 2015 Ch. 10).
        //
        // When capital is scarce, posting N configured tiers may lock
        // more XCH UTXOs than the spendable balance can support, which
        // triggers UTXO liberation and creates a churn cycle.
        //
        // Each create_offer locks ~0.25 XCH in a fee-coin UTXO,
        // regardless of the offer's asset type.  With fee_reserve=1.0
        // and only 4.8 XCH spendable, 6 offers (3 bid + 3 ask) lock
        // 1.5 XCH in fee UTXOs alone, plus the base-asset value of asks.
        //
        // We compute the maximum number of tiers whose cumulative UTXO
        // cost (offer size + fee overhead) stays within the XCH budget:
        //   xch_budget = xch_spendable - fee_reserve - safety_margin
        //
        // Tiers are dropped from the outside in (highest tier_index
        // first), preserving tight tiers that have the best fill
        // probability.  Remaining tier sizes are NOT renormalised --
        // the ladder was already built with full allocations and
        // renormalising would change the effective position size.
        //
        // Academic rationale: thin outer tiers are worse than no tiers.
        // They increase adverse selection exposure (Gueant et al. 2013)
        // and cancel/replace costs scale linearly with tier count, not
        // fill probability.
        // -----------------------------------------------------------------
        // [v0.7.14] Skip XCH budget limiter in recovery mode.
        // When xch_buy_only_mode is active, the remaining tiers are
        // already restricted to XCH-buying sides.  Trimming them by
        // the tiny xch_budget causes a deadlock: no bids can be
        // posted, so XCH can never recover.  The offer_manager's
        // per-tier UTXO-lock recovery zone check handles safety.
        const bool skip_xch_budget_limiter = xch_buy_only_mode
            && pair_cfg
            && (pair_cfg->base_asset_id == "xch"
                || pair_cfg->quote_asset_id == "xch");

        if (pair_cfg && !fee_filtered_tiers.empty()
            && config_.strategy.fee_reserve_xch > 0.0
            && !skip_xch_budget_limiter) {

            // Estimated UTXO overhead per offer (~0.25 XCH).
            constexpr Mojo kUtxoOverheadMojos =
                static_cast<Mojo>(250000000000LL);  // 0.25 * 1e12

            // Safety margin: 20% headroom above fee reserve.
            constexpr double kSafetyMarginPct = 0.20;

            const auto reserve_mojos = static_cast<Mojo>(std::llround(
                config_.strategy.fee_reserve_xch
                * static_cast<double>(kMojosPerXch)));
            const auto safety_mojos = static_cast<Mojo>(std::llround(
                static_cast<double>(reserve_mojos) * kSafetyMarginPct));

            const Mojo xch_budget =
                (xch_spendable_pre > reserve_mojos + safety_mojos)
                    ? xch_spendable_pre - reserve_mojos - safety_mojos
                    : Mojo{0};

            if (xch_budget > 0) {
                // Find the maximum tier_index across the ladder.
                uint8_t max_tier = 0;
                for (const auto& tq : fee_filtered_tiers)
                    max_tier = std::max(max_tier, tq.tier_index);

                // Count offers per tier and estimate XCH cost per tier.
                // For XCH-base pairs, ask offers directly lock XCH equal
                // to their size.  For all offers, add UTXO overhead.
                const bool base_is_xch =
                    pair_cfg->base_asset_id == "xch";

                // Accumulate cost from tier 0 upward.  Stop when adding
                // the next tier would exceed the budget.
                Mojo cumulative_cost = 0;
                uint8_t max_affordable_tier = max_tier;  // inclusive

                for (uint8_t ti = 0; ti <= max_tier; ++ti) {
                    Mojo tier_cost = 0;
                    int tier_offer_count = 0;
                    for (const auto& tq : fee_filtered_tiers) {
                        if (tq.tier_index != ti) continue;
                        ++tier_offer_count;
                        tier_cost += kUtxoOverheadMojos;  // fee UTXO
                        // If base is XCH and this is an ask, the offer
                        // itself locks XCH equal to the offer size.
                        if (base_is_xch && tq.side == Side::Ask) {
                            tier_cost += tq.size;
                        }
                    }
                    if (tier_offer_count == 0) continue;

                    if (cumulative_cost + tier_cost > xch_budget) {
                        max_affordable_tier = (ti > 0) ? ti - 1 : 0;
                        // If even tier 0 doesn't fit, keep it anyway
                        // (minimum 1 tier).
                        break;
                    }
                    cumulative_cost += tier_cost;
                    max_affordable_tier = ti;
                }

                // Remove tiers beyond max_affordable_tier.
                if (max_affordable_tier < max_tier) {
                    const auto orig_size = fee_filtered_tiers.size();
                    std::vector<TierQuote> trimmed;
                    trimmed.reserve(orig_size);
                    for (const auto& tq : fee_filtered_tiers) {
                        if (tq.tier_index <= max_affordable_tier) {
                            trimmed.push_back(tq);
                        }
                    }

                    spdlog::info("[Engine] Dynamic tier limit: {} -> {} "
                                 "tiers for {} (xch_budget={:.3f} "
                                 "xch_spendable={:.3f} reserve={:.3f})",
                                 orig_size, trimmed.size(),
                                 pair_name,
                                 static_cast<double>(xch_budget) / kMojosPerXch,
                                 static_cast<double>(xch_spendable_pre) / kMojosPerXch,
                                 config_.strategy.fee_reserve_xch);
                    fee_filtered_tiers = std::move(trimmed);
                }
            } else {
                // Zero budget -- only keep tier 0 (minimum presence).
                const auto orig_size = fee_filtered_tiers.size();
                if (orig_size > 0) {
                    // Find minimum tier_index present.
                    uint8_t min_ti = 255;
                    for (const auto& tq : fee_filtered_tiers)
                        min_ti = std::min(min_ti, tq.tier_index);

                    std::vector<TierQuote> trimmed;
                    for (const auto& tq : fee_filtered_tiers) {
                        if (tq.tier_index == min_ti)
                            trimmed.push_back(tq);
                    }
                    if (trimmed.size() < orig_size) {
                        spdlog::info("[Engine] Dynamic tier limit: {} -> {} "
                                     "tiers for {} (zero XCH budget, "
                                     "xch_spendable={:.3f})",
                                     orig_size, trimmed.size(), pair_name,
                                     static_cast<double>(xch_spendable_pre)
                                         / kMojosPerXch);
                        fee_filtered_tiers = std::move(trimmed);
                    }
                }
            }
        }

                // -- Crossed-mid pre-post guard -------------------------------------
        // Defense-in-depth: filter out any tier that would cross the
        // current model mid-price BEFORE posting on-chain.
        //
        // The competitive anchor in liquidity.cpp already clamps to
        // min(bbo_ref, mid), but the model mid can change between Step 7
        // (ladder generation) and Step 8 (posting).  Without this guard,
        // crossed offers are posted on-chain (wasting 5000 mojo fee each)
        // and then cancelled the next cycle by classify_tier_staleness.
        //
        // 267 crossed_mid cancellations in the current session wasted
        // ~10M mojos and exposed us to adverse selection for one block
        // per offer before cancellation.
        if (mid > 0) {
            std::vector<TierQuote> mid_safe;
            mid_safe.reserve(fee_filtered_tiers.size());
            std::size_t suppressed_count = 0;
            for (const auto& tier : fee_filtered_tiers) {
                if (tier.side == Side::Bid && tier.price > mid) {
                    spdlog::info("[Engine] Step 8: {} bid tier {} suppressed "
                                 "-- price {} > mid {} (crossed)",
                                 pair_name, tier.tier_index,
                                 tier.price, mid);
                    ++suppressed_count;
                    continue;
                }
                if (tier.side == Side::Ask && tier.price < mid) {
                    spdlog::info("[Engine] Step 8: {} ask tier {} suppressed "
                                 "-- price {} < mid {} (crossed)",
                                 pair_name, tier.tier_index,
                                 tier.price, mid);
                    ++suppressed_count;
                    continue;
                }
                mid_safe.push_back(tier);
            }
            if (suppressed_count > 0) {
                spdlog::warn("[Engine] Step 8: {} crossed-mid guard removed "
                             "{}/{} tiers",
                             pair_name, suppressed_count,
                             fee_filtered_tiers.size());
            }
            fee_filtered_tiers = std::move(mid_safe);
        }

        // -- BBO proximity sanity check -----------------------------------
        // Defense-in-depth: reject any tier whose price is unreasonably
        // far from the current Dexie BBO.  This catches cases where the
        // model mid diverges from actual market prices (e.g. asymmetric
        // depth pulling the microprice 40% below BBO).
        {
            const auto book_snap = state_->get_market(pair_name);
            const Mojo best_bid = book_snap.best_bid;
            const Mojo best_ask = book_snap.best_ask;
            const bool has_bbo  = (best_bid > 0 && best_ask > 0);

            if (has_bbo && !fee_filtered_tiers.empty()) {
                const Mojo bbo_mid_m = (best_bid + best_ask) / 2;
                constexpr double kMaxBboDeviation = 0.10;  // 10% -- matches microprice clamp

                // Check 1: Model mid vs BBO sanity
                if (mid > 0) {
                    const double mid_dev = std::abs(
                        static_cast<double>(mid) - static_cast<double>(bbo_mid_m))
                        / static_cast<double>(bbo_mid_m);
                    if (mid_dev > kMaxBboDeviation) {
                        spdlog::warn("[Engine] Step 8: {} model mid {} deviates "
                                     "{:.1f}% from BBO mid {} -- suppressing ALL "
                                     "offers this block",
                                     pair_name, mid, mid_dev * 100.0, bbo_mid_m);
                        fee_filtered_tiers.clear();
                    }
                }

                // Check 2: Per-tier BBO proximity
                if (!fee_filtered_tiers.empty()) {
                    std::vector<TierQuote> bbo_safe;
                    bbo_safe.reserve(fee_filtered_tiers.size());
                    std::size_t bbo_suppressed = 0;

                    for (const auto& tier : fee_filtered_tiers) {
                        const Mojo ref = (tier.side == Side::Bid)
                            ? best_bid : best_ask;
                        const double dev = std::abs(
                            static_cast<double>(tier.price)
                            - static_cast<double>(ref))
                            / static_cast<double>(ref);

                        if (dev > kMaxBboDeviation) {
                            spdlog::warn("[Engine] Step 8: {} {} tier {} price {} "
                                         "deviates {:.1f}% from BBO {} -- "
                                         "suppressed",
                                         pair_name,
                                         (tier.side == Side::Bid) ? "bid" : "ask",
                                         tier.tier_index, tier.price,
                                         dev * 100.0, ref);
                            ++bbo_suppressed;
                            continue;
                        }
                        bbo_safe.push_back(tier);
                    }
                    if (bbo_suppressed > 0) {
                        spdlog::warn("[Engine] Step 8: {} BBO sanity check "
                                     "removed {}/{} tiers",
                                     pair_name, bbo_suppressed,
                                     fee_filtered_tiers.size());
                    }
                    fee_filtered_tiers = std::move(bbo_safe);
                }
            }
        }

        // -- Side-aware balance filter: suppress tiers on depleted sides.
        if (!can_bid || !can_ask) {
            std::vector<TierQuote> side_filtered;
            side_filtered.reserve(fee_filtered_tiers.size());
            for (const auto& tier : fee_filtered_tiers) {
                if (tier.side == Side::Bid && !can_bid) continue;
                if (tier.side == Side::Ask && !can_ask) continue;
                side_filtered.push_back(tier);
            }
            const auto suppressed = fee_filtered_tiers.size() - side_filtered.size();
            if (suppressed > 0) {
                spdlog::info("[Engine] Step 8: {} suppressed {} tiers "
                             "(can_bid={}, can_ask={})",
                             pair_name, suppressed, can_bid, can_ask);
            }
            fee_filtered_tiers = std::move(side_filtered);
        }

        // -- Quote-asset recovery pricing ----------------------------------
        // When the quote asset is depleted (can_bid=false) and the
        // inventory ratio is extreme, reprice the single tightest ask
        // to just below the current DEX best ask.  This makes us the
        // cheapest seller on the DEX so buyers prefer our offer over the
        // wall, accelerating inventory rebalancing.
        if (!can_bid && can_ask
            && config_.strategy.auto_rebalance_enabled
            && config_.strategy.quote_recovery_enabled
            && !fee_filtered_tiers.empty()) {
            const PairConfig* pair_cfg_qr = find_pair_config(pair_name);
            const double qr_mid = market_data_->get_mid_price(pair_name);
            const Mojo qr_mid_mojos = static_cast<Mojo>(std::llround(
                qr_mid * static_cast<double>(kMojosPerXch)));
            const double inv_ratio_qr = (pair_cfg_qr && qr_mid_mojos > 0)
                ? std::abs(inventory_->inventory_ratio(
                      AssetId{pair_cfg_qr->base_asset_id},
                      AssetId{pair_cfg_qr->quote_asset_id},
                      qr_mid_mojos))
                : 0.0;
            if (inv_ratio_qr >= config_.strategy.quote_recovery_ratio_threshold) {
                const auto mkt_qr = state_->get_market(pair_name);
                if (mkt_qr.best_ask > 0) {
                    // Find the tightest (lowest tier_index) ask tier.
                    int min_tier_idx = -1;
                    for (const auto& tq : fee_filtered_tiers) {
                        if (tq.side == Side::Ask) {
                            if (min_tier_idx < 0 || tq.tier_index < min_tier_idx)
                                min_tier_idx = tq.tier_index;
                        }
                    }
                    if (min_tier_idx >= 0) {
                        const double undercut_frac =
                            config_.strategy.quote_recovery_undercut_bps / 10000.0;
                        const Mojo recovery_price = static_cast<Mojo>(std::llround(
                            static_cast<double>(mkt_qr.best_ask)
                            * (1.0 - undercut_frac)));
                        for (auto& tq : fee_filtered_tiers) {
                            if (tq.side == Side::Ask
                                && tq.tier_index == min_tier_idx
                                && tq.price > recovery_price) {
                                spdlog::info(
                                    "[Engine] Step 8: {} quote-recovery mode "
                                    "(inv_ratio={:.3f} >= threshold={:.3f}) -- "
                                    "repricing ask tier {} from {:.6f} to {:.6f} "
                                    "({:.1f} bps below best-ask)",
                                    pair_name,
                                    inv_ratio_qr,
                                    config_.strategy.quote_recovery_ratio_threshold,
                                    min_tier_idx,
                                    static_cast<double>(tq.price)
                                        / static_cast<double>(kMojosPerXch),
                                    static_cast<double>(recovery_price)
                                        / static_cast<double>(kMojosPerXch),
                                    config_.strategy.quote_recovery_undercut_bps);
                                tq.price = recovery_price;
                            }
                        }
                    }
                }
            }
        }

        // -- Competitiveness guard -----------------------------------------
        {
            // [v0.7.46 #5] Stablecoin pairs (BYC/wUSDC.b etc.) trade
            // inside a sub-1% range and rarely score 3+ on the 0-10
            // competitiveness scale, which historically caused 100% of
            // BYC tiers to be suppressed (151 sanity rejections in
            // 0.7.45 audit).  Use a lower threshold for stablecoin pairs.
            const PairConfig* pair_cfg_comp = find_pair_config(pair_name);
            const int kBaseCompetitivenessScore =
                (pair_cfg_comp && pair_cfg_comp->is_stablecoin) ? 1 : 3;

            // [v0.7.48] Apply PID offset to the base threshold.  Negative
            // offset (underfilling) lowers the gate so more tiers post;
            // positive offset (overfilling) raises it.  Final value is
            // clamped to the legal 0-10 range.
            int kMinCompetitivenessScore = kBaseCompetitivenessScore;
            if (config_.strategy.comp_pid_enabled) {
                auto pid_it = comp_pid_state_.find(pair_name);
                if (pid_it != comp_pid_state_.end()) {
                    const int offset = pid_it->second.current_offset();
                    kMinCompetitivenessScore = std::clamp(
                        kBaseCompetitivenessScore + offset, 0, 10);
                    if (offset != 0) {
                        spdlog::debug("[Engine] Step 8: {} comp gate base={} "
                                      "PID-offset={} -> effective={}",
                                      pair_name,
                                      kBaseCompetitivenessScore,
                                      offset,
                                      kMinCompetitivenessScore);
                    }
                }
            }
            const auto book_snap = state_->get_market(pair_name);
            const auto competing_offers = market_data_->get_competing_offers(pair_name);
            std::vector<TierQuote> competitive_tiers;
            competitive_tiers.reserve(fee_filtered_tiers.size());

            for (const auto& tier : fee_filtered_tiers) {
                const int score = score_offer_competitiveness(
                    tier.side,
                    tier.price,
                    book_snap.best_bid,
                    book_snap.best_ask);
                const Mojo queue_ahead_mojos = compute_queue_ahead_mojos(
                    tier.side,
                    tier.price,
                    competing_offers);
                const int queue_ahead_score = score_queue_position(
                    queue_ahead_mojos,
                    tier.size);
                if (score >= kMinCompetitivenessScore) {
                    competitive_tiers.push_back(tier);
                    continue;
                }

                spdlog::info("[Engine] Step 8: {} {} tier {} suppressed -- "
                             "competitiveness {}/10 queue {}/10 ahead={} (price={} bbo={}/{})",
                             pair_name,
                             (tier.side == Side::Bid ? "bid" : "ask"),
                             tier.tier_index,
                             score,
                             queue_ahead_score,
                             queue_ahead_mojos,
                             tier.price,
                             book_snap.best_bid,
                             book_snap.best_ask);
                try {
                    xop::DbSanityFailure failure;
                    failure.block_height = block_height;
                    failure.pair_name = pair_name;
                    failure.side = (tier.side == Side::Bid) ? "bid" : "ask";
                    failure.tier = static_cast<int>(tier.tier_index);
                    failure.proposed_price_mojos = static_cast<Mojo>(tier.price);
                    failure.reference_price_mojos =
                        (tier.side == Side::Bid) ? book_snap.best_bid : book_snap.best_ask;
                    failure.deviation_pct = 0.0;
                    failure.failure_reason = "competitiveness_too_low";
                    failure.details = fmt::format(
                        "{{score:{}, queue_ahead_mojos:{}, queue_ahead_score:{}, best_bid:{}, best_ask:{}}}",
                        score,
                        queue_ahead_mojos,
                        queue_ahead_score,
                        book_snap.best_bid,
                        book_snap.best_ask);
                    db_->insert_sanity_failure(failure);
                } catch (const std::exception& e) {
                    spdlog::debug("[Engine] Failed to log competitiveness failure: {}",
                                  e.what());
                }
            }

            if (competitive_tiers.size() != fee_filtered_tiers.size()) {
                spdlog::warn("[Engine] Step 8: {} competitiveness guard suppressed {}/{} tiers",
                             pair_name,
                             fee_filtered_tiers.size() - competitive_tiers.size(),
                             fee_filtered_tiers.size());
            }
            fee_filtered_tiers = std::move(competitive_tiers);
        }

        // Keep one safe bid alive when ask-side is intentionally suppressed
        // and competitiveness filtering removed every remaining bid tier.
        // Candidate tiers have already passed price/BBO/mid guards.
        if (fee_filtered_tiers.empty() && can_bid && !can_ask) {
            const TierQuote* fallback_bid = nullptr;
            for (const auto& tier : pcs.ladder) {
                if (tier.side != Side::Bid) {
                    continue;
                }
                if (fallback_bid == nullptr
                    || tier.tier_index < fallback_bid->tier_index
                    || (tier.tier_index == fallback_bid->tier_index
                        && tier.price > fallback_bid->price)) {
                    fallback_bid = &tier;
                }
            }

            if (fallback_bid != nullptr) {
                fee_filtered_tiers.push_back(*fallback_bid);
                spdlog::warn("[Engine] Step 8: {} competitiveness/fee fallback "
                             "kept BID tier {} (price={}, size={}) while "
                             "ask-side suppressed",
                             pair_name,
                             fallback_bid->tier_index,
                             fallback_bid->price,
                             fallback_bid->size);
            }
        }

        {
            int requested_bids = 0;
            int requested_asks = 0;
            int emitted_bids = 0;
            int emitted_asks = 0;
            for (const auto& tq : pcs.ladder) {
                if (tq.side == Side::Bid) ++requested_bids;
                else                      ++requested_asks;
            }
            for (const auto& tq : fee_filtered_tiers) {
                if (tq.side == Side::Bid) ++emitted_bids;
                else                      ++emitted_asks;
            }
            spdlog::info("[Engine] Step 8: {} tier_summary requested={} "
                         "(bids={}, asks={}) emitted={} (bids={}, asks={}) "
                         "can_bid={} can_ask={}",
                         pair_name,
                         pcs.ladder.size(), requested_bids, requested_asks,
                         fee_filtered_tiers.size(), emitted_bids, emitted_asks,
                         can_bid, can_ask);
        }

        if (fee_filtered_tiers.empty()) {
            spdlog::info("[Engine] Step 8: {} all tiers filtered by "
                         "fee-vs-gain gating", pair_name);
            continue;
        }

        // Pending exposure guard (pre-post projection): include currently
        // pending offers plus candidate new tiers and suppress any side that
        // would breach reserve if all accepted.
        {
            auto ceil_to_mojo = [](long double value) -> Mojo {
                if (!std::isfinite(static_cast<double>(value)) || value <= 0.0L) {
                    return 0;
                }
                const long double cap = static_cast<long double>(
                    std::numeric_limits<Mojo>::max());
                if (value >= cap) {
                    return std::numeric_limits<Mojo>::max();
                }
                return static_cast<Mojo>(std::ceil(value));
            };
            auto quote_cost_for_base_size = [&](Mojo base_size, Mojo price) -> Mojo {
                const long double base_mpu = static_cast<long double>(
                    pair_cfg->base_mojos_per_unit > 0 ? pair_cfg->base_mojos_per_unit : 1);
                const long double quote_mpu = static_cast<long double>(
                    pair_cfg->quote_mojos_per_unit > 0 ? pair_cfg->quote_mojos_per_unit : 1);
                const long double price_scale = static_cast<long double>(kMojosPerXch);
                return ceil_to_mojo(
                    static_cast<long double>(base_size)
                    * static_cast<long double>(price)
                    * quote_mpu / (base_mpu * price_scale));
            };

            Mojo pending_plus_new_ask = pair_base_pending_spend;
            Mojo pending_plus_new_bid = pair_quote_pending_spend;
            for (const auto& tq : fee_filtered_tiers) {
                if (tq.side == Side::Ask) {
                    pending_plus_new_ask += tq.size;
                } else {
                    pending_plus_new_bid += quote_cost_for_base_size(tq.size, tq.price);
                }
            }

            bool suppress_ask_projection = false;
            bool suppress_bid_projection = false;
            if (can_ask && pair_base_balance_known && pair_base_reserve_mojos > 0) {
                const Mojo projected_after_fill =
                    (pair_base_spendable > pending_plus_new_ask)
                        ? (pair_base_spendable - pending_plus_new_ask)
                        : Mojo{0};
                if (projected_after_fill < pair_base_reserve_mojos) {
                    suppress_ask_projection = true;
                    can_ask = false;
                    spdlog::info("[Engine] Step 8: {} projected ask exposure "
                                 "would breach reserve on {} (spendable={} "
                                 "pending+new={} reserve={}) -- suppressing ask",
                                 pair_name,
                                 pair_cfg->base_asset_id,
                                 pair_base_spendable,
                                 pending_plus_new_ask,
                                 pair_base_reserve_mojos);
                }
            }
            if (can_bid && pair_quote_balance_known && pair_quote_reserve_mojos > 0) {
                const Mojo projected_after_fill =
                    (pair_quote_spendable > pending_plus_new_bid)
                        ? (pair_quote_spendable - pending_plus_new_bid)
                        : Mojo{0};
                if (projected_after_fill < pair_quote_reserve_mojos) {
                    suppress_bid_projection = true;
                    can_bid = false;
                    spdlog::info("[Engine] Step 8: {} projected bid exposure "
                                 "would breach reserve on {} (spendable={} "
                                 "pending+new={} reserve={}) -- suppressing bid",
                                 pair_name,
                                 pair_cfg->quote_asset_id,
                                 pair_quote_spendable,
                                 pending_plus_new_bid,
                                 pair_quote_reserve_mojos);
                }
            }

            if (suppress_ask_projection || suppress_bid_projection) {
                std::vector<TierQuote> projected_safe_tiers;
                projected_safe_tiers.reserve(fee_filtered_tiers.size());
                for (const auto& tq : fee_filtered_tiers) {
                    if ((tq.side == Side::Bid && can_bid)
                        || (tq.side == Side::Ask && can_ask)) {
                        projected_safe_tiers.push_back(tq);
                    }
                }
                fee_filtered_tiers = std::move(projected_safe_tiers);
            }
        }

        if (fee_filtered_tiers.empty()) {
            spdlog::info("[Engine] Step 8: {} all tiers filtered by pending "
                         "exposure projection", pair_name);
            continue;
        }

        // [T5-01] Selective refresh filter: when we did a selective cancel
        // (some tiers were Fresh and left live), only post replacements for
        // the tiers that were actually cancelled.  Posting duplicates of
        // Fresh tiers would create double-exposure at the same price level.
        if (has_pending && fresh_count > 0 && !cancelled_ids.empty()) {
            // Build a set of (side, tier_index) keys for cancelled tiers.
            std::unordered_set<std::string> cancelled_keys;
            for (const auto& tc : tier_classes) {
                if (tc.staleness != execution::TierStaleness::Fresh) {
                    cancelled_keys.insert(
                        std::to_string(static_cast<int>(tc.side))
                        + "_" + std::to_string(tc.tier_index));
                }
            }
            // Also include tiers that have no pending offer at all
            // (brand new tiers that weren't in the previous ladder).
            std::unordered_set<std::string> pending_keys;
            for (const auto& tc : tier_classes) {
                pending_keys.insert(
                    std::to_string(static_cast<int>(tc.side))
                    + "_" + std::to_string(tc.tier_index));
            }

            std::vector<TierQuote> selective_tiers;
            for (const auto& tq : fee_filtered_tiers) {
                std::string key = std::to_string(static_cast<int>(tq.side))
                                + "_" + std::to_string(tq.tier_index);
                if (cancelled_keys.count(key) > 0 ||
                    pending_keys.count(key) == 0) {
                    selective_tiers.push_back(tq);
                }
            }

            if (selective_tiers.empty()) {
                spdlog::debug("[Engine] Step 8: {} selective refresh has no "
                              "tiers to repost (all fresh)", pair_name);
                continue;
            }

            spdlog::info("[Engine] Step 8: {} selective refresh -- posting "
                         "{}/{} replacement tiers",
                         pair_name, selective_tiers.size(),
                         fee_filtered_tiers.size());
            fee_filtered_tiers = std::move(selective_tiers);
        }

        // [T1-03] co_await post_quotes directly instead of use_future.
        // In xch_buy_only_mode, pass the full fee_reserve_xch so
        // offer_manager enforces the same UTXO-lock safety margin.
        // Previously used fee_min_spendable_xch (0.01) which was far
        // too permissive: a single offer could lock the entire remaining
        // XCH UTXO and drain spendable to zero.
        const double fee_override = xch_buy_only_mode
            ? config_.strategy.fee_reserve_xch
            : 0.0;
        int posted = co_await offer_mgr_->post_quotes(
            *pair_cfg, fee_filtered_tiers, block_height, fee_override);

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
            int competitiveness_sum = 0;
            int competitiveness_count = 0;
            int queue_ahead_sum = 0;
            int execution_quality_sum = 0;
            const auto competing_offers = market_data_->get_competing_offers(pair_name);
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
                orec.fee_mojos     = recommended_fee;

                // Capture order-book context for outcome analysis.
                {
                    const auto book_snap = state_->get_market(pair_name);
                    orec.book_best_bid = book_snap.best_bid;
                    orec.book_best_ask = book_snap.best_ask;
                    orec.competitiveness_score = score_offer_competitiveness(
                        etq.side,
                        etq.price,
                        orec.book_best_bid,
                        orec.book_best_ask);
                    orec.queue_ahead_mojos = compute_queue_ahead_mojos(
                        etq.side,
                        etq.price,
                        competing_offers);
                    orec.queue_ahead_score = score_queue_position(
                        orec.queue_ahead_mojos,
                        etq.size);
                    orec.execution_quality_score = score_execution_quality(
                        orec.competitiveness_score,
                        orec.queue_ahead_score);
                }

                // Look up the actual offer_id from State.
                std::string key = std::to_string(static_cast<int>(etq.side))
                    + "_" + std::to_string(etq.tier_index);
                auto id_it = tier_to_id.find(key);
                if (id_it != tier_to_id.end()) {
                    // [T2-09] Use the actual wallet-assigned offer ID.
                    orec.offer_id = id_it->second;
                } else {
                    // post_quotes may have skipped this tier due to a wallet
                    // RPC error (for example insufficient funds).  Do not
                    // persist a fake placeholder ID into the audit trail.
                    spdlog::warn("[Engine] Step 8: no wallet offer_id for {} "
                                 "{} tier {} -- skipping database insert",
                                 pair_name,
                                 (etq.side == Side::Bid) ? "bid" : "ask",
                                 etq.tier_index);
                    continue;
                }
                db_->insert_offer(orec);
                competitiveness_sum += orec.competitiveness_score;
                queue_ahead_sum += orec.queue_ahead_score;
                execution_quality_sum += orec.execution_quality_score;
                ++competitiveness_count;
            }

            if (competitiveness_count > 0) {
                spdlog::info("[Engine] Step 8: {} price avg={:.1f}/10 queue avg={:.1f}/10 execution avg={:.1f}/10 over {} posted offers",
                             pair_name,
                             static_cast<double>(competitiveness_sum)
                                 / static_cast<double>(competitiveness_count),
                             static_cast<double>(queue_ahead_sum)
                                 / static_cast<double>(competitiveness_count),
                             static_cast<double>(execution_quality_sum)
                                 / static_cast<double>(competitiveness_count),
                             competitiveness_count);
            }
        }

        // Record rebalance baseline so the LiquidityEngine can evaluate
        // future rebalance triggers.
        auto& liq = *liquidity_engines_[pair_name];
        Mojo mid_mojos = static_cast<Mojo>(std::llround(
            market_data_->get_mid_price(pair_name)
            * static_cast<double>(kMojosPerXch)));
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
                     posted, pair_name, cancelled_ids.size(), recommended_fee);
    }

    // Reset consecutive pending counter only when NO pair hit Gate 1
    // (pending_change > 0) on this entire block.  Resetting per-pair
    // would incorrectly clear the counter whenever a clean pair processes.
    if (pending_wallets_this_block.empty()) {
        consecutive_pending_blocks_ = 0;
    }

    // Report the aggregate stuck-offer count across all pairs for this cycle.
    metrics_->update_stuck_offers(total_stuck_offers);

    // -- [T4-11] Periodic offer-state reconciliation -------------------------
    // Every reconciliation_interval_blocks, perform a full comparison of
    // the in-memory pending-offer map against the authoritative wallet RPC
    // state.  Corrects orphans, phantoms, and missed status transitions.
    const uint32_t recon_interval = config_.strategy.reconciliation_interval_blocks;
    if (recon_interval > 0 &&
        block_height >= last_reconciliation_block_ + recon_interval) {
        try {
            auto reconciled_ids = co_await offer_mgr_->reconcile_offers(block_height);
            last_reconciliation_block_ = block_height;
            if (!reconciled_ids.empty()) {
                spdlog::info("[Engine] Step 8: offer reconciliation corrected "
                             "{} discrepancies", reconciled_ids.size());
                // Persist reconciled cancellations to database.
                for (const auto& oid : reconciled_ids) {
                    try {
                        db_->update_offer_status(oid, "cancelled", block_height,
                                                "periodic_reconcile");
                    } catch (const std::exception& e) {
                        spdlog::debug("[Engine] reconcile update_offer_status "
                                     "failed for {}: {}",
                                     oid.substr(0, 12), e.what());
                    }
                }
            }

            // Persist any newly adopted wallet offers to the DB so they
            // survive the next restart.  reconcile_offers() upserts them
            // into State; we mirror to the DB here.
            {
                auto current_offers = state_->get_all_offers();
                for (const auto& po : current_offers) {
                    try {
                        // insert_offer is idempotent if the offer_id already
                        // exists (or use upsert if available).
                        DbOfferRecord rec;
                        rec.offer_id      = po.offer_id;
                        rec.pair_name     = po.pair_name;
                        rec.side          = (po.side == Side::Bid) ? "bid" : "ask";
                        rec.price_mojos   = po.price;
                        rec.size_mojos    = po.size;
                        rec.tier          = static_cast<int>(po.tier);
                        rec.status        = "pending";
                        rec.created_block = po.created_at_block;
                        rec.fee_mojos     = po.fee_mojos;
                        db_->insert_offer(rec);
                    } catch (...) {
                        // Already exists or DB constraint - fine.
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("[Engine] Step 8: offer reconciliation failed: {}",
                         e.what());
        }

        // -- On-chain reconciliation (full node ground truth) ----------------
        // Runs alongside the wallet reconciliation every interval.  Verifies
        // balance consistency and detects stale offers via blockchain data.
        if (on_chain_reconciler_) {
            try {
                // Build wallet ID map from enabled pairs.
                std::unordered_map<std::string, std::int64_t> wallet_ids;
                std::unordered_set<std::string> our_puzzle_hashes;

                for (const auto& pair : config_.pairs) {
                    if (!pair.enabled) continue;
                    // Resolve wallet IDs via OfferManager (same as balance gating).
                    auto bwid = offer_mgr_->resolve_wallet_id(pair.base_asset_id);
                    auto qwid = offer_mgr_->resolve_wallet_id(pair.quote_asset_id);
                    if (bwid > 0) wallet_ids[pair.name + "/base"] = bwid;
                    if (qwid > 0) wallet_ids[pair.name + "/quote"] = qwid;
                }

                // Collect puzzle hashes from spendable coins for fee tracking.
                for (const auto& [label, wid] : wallet_ids) {
                    try {
                        auto coins = co_await wallet_->get_spendable_coins(wid);
                        for (const auto& cr : coins) {
                            const auto& coin = cr.contains("coin")
                                ? cr["coin"] : cr;
                            if (coin.contains("puzzle_hash")) {
                                std::string ph =
                                    coin["puzzle_hash"].get<std::string>();
                                if (ph.size() > 2 && ph.substr(0, 2) == "0x") {
                                    ph = ph.substr(2);
                                }
                                our_puzzle_hashes.insert(ph);
                            }
                        }
                    } catch (...) { /* best-effort puzzle hash collection */ }
                }

                auto [stale_ids, balance_discreps] =
                    co_await on_chain_reconciler_->run_full_reconciliation(
                        wallet_ids, block_height, our_puzzle_hashes);

                if (!stale_ids.empty()) {
                    spdlog::warn("[Engine] Step 8: on-chain reconciliation "
                                 "found {} stale offers", stale_ids.size());
                    for (const auto& oid : stale_ids) {
                        try {
                            db_->update_offer_status(oid, "cancelled",
                                                    block_height,
                                                    "on_chain_reconcile");
                        } catch (const std::exception& e) {
                            spdlog::debug("[Engine] on-chain reconcile "
                                         "update_offer_status failed for "
                                         "{}: {}",
                                         oid.substr(0, 12), e.what());
                        }
                    }
                }

                if (!balance_discreps.empty()) {
                    spdlog::warn("[Engine] Step 8: on-chain reconciliation "
                                 "found {} balance discrepancies",
                                 balance_discreps.size());
                    for (const auto& d : balance_discreps) {
                        spdlog::warn("  {} (wid={}): wallet={} on_chain={} "
                                     "diff={}",
                                     d.wallet_label, d.wallet_id,
                                     d.wallet_confirmed, d.on_chain_total,
                                     d.difference);
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("[Engine] Step 8: on-chain reconciliation "
                             "failed: {}", e.what());
            }
        }
    }

    co_return;
}

// Step 9: Check arbitrage opportunities + execute crossed-book takes.
// [T9-01] Now a coroutine: co_awaits dexie + wallet RPCs for crossed-book arb.
asio::awaitable<void> Engine::step_check_arbitrage(
    [[maybe_unused]] BlockHeight block_height)
{
    // -- 9a: Legacy MarketDataFeed CEX-DEX signal check ----------------------
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
    if (config_.arbitrage.enabled && arb_detector_) {
        PairPriceMap pair_prices;
        for (const auto& pair : config_.pairs) {
            if (!pair.enabled) continue;
            auto it = cycle_.find(pair.name);
            if (it == cycle_.end() || !it->second.market_data_valid) continue;
            const double mid = market_data_->get_mid_price(pair.name);
            if (mid > 0.0) {
                pair_prices[pair.name] = mid;
            }
        }

        if (!pair_prices.empty()) {
            arb_detector_->set_pair_prices(pair_prices);
            auto opportunities = arb_detector_->scan_all();

            if (!opportunities.empty()) {
                spdlog::info("[Engine] Step 9: {} arbitrage opportunities "
                             "detected (block {})",
                             opportunities.size(), block_height);
                for (const auto& opp : opportunities) {
                    spdlog::info("[Engine] Step 9: {} | edge={:.1f}bps "
                                 "profit={:.6f} conf={:.3f} urgency={}blk | {}",
                                 to_string(opp.type), opp.edge_bps,
                                 opp.estimated_profit, opp.confidence,
                                 opp.urgency_blocks, opp.description);
                }
            } else {
                spdlog::debug("[Engine] Step 9: no arbitrage opportunities "
                              "detected (block {})", block_height);
            }
        }
    }

    // -- 9c: Crossed-book arbitrage (intra-DEX offer taking) -----------------
    // Dexie has no matching engine, so crossed books (bid >= ask) are normal.
    // When we detect a cross, we can take the cheap ask offer for an instant
    // profit equal to (bid - ask) minus blockchain fee.
    //
    // Flow:
    //   1. For each pair, check if competing offers show bid >= ask
    //   2. Identify the cheapest ask offers inside the cross
    //   3. Fetch full offer text (bech32m) from Dexie
    //   4. Take the offer via wallet RPC (atomic swap)
    //
    // Guards: dry-run mode, fee budget, max take size, wallet circuit breaker.

    if (!config_.arbitrage.enabled ||
        !config_.arbitrage.crossed_book_enabled ||
        !dexie_ || !wallet_) {
        co_return;
    }

    if (dry_run_) {
        // In dry-run mode we still detect and log crossed-book opportunities.
    }

    if (wallet_circuit_open_) {
        spdlog::debug("[Engine] Step 9c: crossed-book SKIPPED -- wallet "
                      "circuit breaker open");
        co_return;
    }

    const double min_edge_bps = config_.arbitrage.crossed_book_min_edge_bps;
    const double max_take_xch = config_.arbitrage.crossed_book_max_take_xch;

    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        // Get the competing offers (already fetched in Step 1).
        auto comp_offers = market_data_->get_competing_offers(pair.name);
        if (comp_offers.empty()) continue;

        // Find best (highest) bid and best (lowest) ask from competing offers.
        Mojo best_bid_price = 0;
        Mojo best_ask_price = std::numeric_limits<Mojo>::max();
        std::string best_ask_offer_id;
        Mojo best_ask_size = 0;

        for (const auto& co : comp_offers) {
            if (co.side == Side::Bid && co.price > best_bid_price) {
                best_bid_price = co.price;
            }
            if (co.side == Side::Ask && co.price < best_ask_price) {
                best_ask_price = co.price;
                best_ask_offer_id = co.offer_id;
                best_ask_size = co.size;
            }
        }

        // No two-sided market ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ nothing to cross.
        if (best_bid_price == 0 ||
            best_ask_price == std::numeric_limits<Mojo>::max()) {
            continue;
        }

        // Not crossed ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ no opportunity.
        if (best_bid_price < best_ask_price) continue;

        // Compute edge in basis points.
        const double ask_d = static_cast<double>(best_ask_price);
        const double bid_d = static_cast<double>(best_bid_price);
        const double edge_bps = (bid_d - ask_d) / ask_d * 10000.0;

        if (edge_bps < min_edge_bps) {
            spdlog::debug("[Engine] Step 9c: {} crossed book edge={:.1f}bps "
                          "< min={:.1f}bps -- skipping",
                          pair.name, edge_bps, min_edge_bps);
            continue;
        }

        // Cap the take size (use pair denomination, not kMojosPerXch).
        const Mojo max_take_mojos = static_cast<Mojo>(
            max_take_xch * static_cast<double>(pair.base_mojos_per_unit));
        const Mojo take_size = std::min(best_ask_size, max_take_mojos);

        spdlog::info("[Engine] Step 9c: {} CROSSED BOOK detected -- "
                     "bid={} ask={} edge={:.1f}bps offer_id={} size={}",
                     pair.name, best_bid_price, best_ask_price,
                     edge_bps, best_ask_offer_id.substr(0, 12), take_size);

        if (dry_run_) {
            spdlog::info("[Engine] Step 9c: {} DRY RUN -- would take offer "
                         "{} for {} mojos",
                         pair.name, best_ask_offer_id.substr(0, 12),
                         take_size);
            continue;
        }

        // Fetch the full offer text (bech32m) from Dexie.
        // The competing-offers fetch uses compact=true which omits the text.
        try {
            auto offer_status =
                co_await dexie_->get_offer_status(best_ask_offer_id);

            if (!offer_status.success ||
                offer_status.offer.offer_bech32.empty()) {
                spdlog::warn("[Engine] Step 9c: {} failed to fetch offer "
                             "text for {} -- skipping",
                             pair.name, best_ask_offer_id.substr(0, 12));
                continue;
            }

            // Verify offer is still active (status 0 = active on Dexie).
            if (offer_status.offer.status != 0) {
                spdlog::info("[Engine] Step 9c: {} offer {} no longer active "
                             "(status={}) -- skipping",
                             pair.name, best_ask_offer_id.substr(0, 12),
                             offer_status.offer.status);
                continue;
            }

            // Determine fee from the FeeTracker.
            const std::uint64_t fee = fee_tracker_
                ? fee_tracker_->get_recommended_fee(
                      config_.fees.min_fee_mojos, block_height)
                : config_.fees.min_fee_mojos;

            spdlog::info("[Engine] Step 9c: {} TAKING crossed-book offer "
                         "{} (edge={:.1f}bps, size={}, fee={})",
                         pair.name, best_ask_offer_id.substr(0, 12),
                         edge_bps, take_size, fee);

            // Take the offer via wallet RPC (atomic swap).
            auto result = co_await wallet_->take_offer(
                offer_status.offer.offer_bech32, fee);

            if (result.contains("success") &&
                result["success"].get<bool>()) {
                const std::string trade_id =
                    result.contains("trade_record") &&
                    result["trade_record"].contains("trade_id")
                    ? result["trade_record"]["trade_id"].get<std::string>()
                    : "unknown";

                spdlog::info("[Engine] Step 9c: {} TOOK crossed-book offer "
                             "-- trade_id={} edge={:.1f}bps size={}",
                             pair.name, trade_id.substr(0, 12),
                             edge_bps, take_size);

                // Record the fee spend.
                if (fee_tracker_) {
                    fee_tracker_->record_fee(fee, block_height);
                }

                // Alert for visibility.
                if (alerts_) {
                    alerts_->send_alert(
                        AlertRule::ArbitrageDetected,
                        pair.name + " crossed-book arb TAKEN: edge=" +
                        std::to_string(edge_bps) + "bps size=" +
                        std::to_string(take_size) + " mojos");
                }
            } else {
                const std::string err = result.contains("error")
                    ? result["error"].get<std::string>()
                    : "unknown error";
                spdlog::warn("[Engine] Step 9c: {} take_offer failed: {}",
                             pair.name, err);
            }

        } catch (const std::exception& e) {
            spdlog::error("[Engine] Step 9c: {} crossed-book take failed: {}",
                          pair.name, e.what());
        }
    }

    // -- 9d: Cross-stablecoin arbitrage (Shleifer & Vishny 1997) -------------
    // BYC and wUSDC.b are both USD-pegged stablecoins.  The XCH/BYC and
    // XCH/wUSDC.b order books should price XCH equivalently after adjusting
    // for the BYC/wUSDC.b cross-rate.  When one market's ask is cheaper than
    // the other market's bid (net of fees), we take the cheap ask to capture
    // the cross-market spread.
    //
    // Scholarly basis:
    //   - Shleifer & Vishny (1997): structural frictions persist mispricings.
    //   - Makarov & Schoar (2020): 100-300 bps persistent cross-venue spreads
    //     in crypto, especially on fragmented DEXes without matching engines.
    //   - Kozhan & Tham (2012): execution risk in triangular FX arbitrage
    //     implies minimum edge thresholds proportional to settlement latency.
    //
    // Implementation:
    //   For each directed pair (A, B) of XCH-base stablecoin pairs:
    //     1. Find cheapest ask on pair A (someone selling XCH for stable A).
    //     2. Find highest bid on pair B (someone buying XCH with stable B).
    //     3. Normalise via BYC/wUSDC.b mid-rate to a common USD basis.
    //     4. If normalised_bid > normalised_ask + fees => take the ask on A.

    if (config_.arbitrage.enabled &&
        config_.arbitrage.cross_stable_arb_enabled &&
        dexie_ && wallet_ && !dry_run_ && !wallet_circuit_open_)
    {
        // Identify XCH-base stablecoin pairs.
        struct XchStablePair {
            std::string pair_name;
            std::string quote_asset;
            std::string quote_asset_id;
        };
        std::vector<XchStablePair> xch_stable_pairs;

        // Collect asset IDs that appear in a stablecoin pair.
        std::set<std::string> stablecoin_asset_ids;
        for (const auto& pc : config_.pairs) {
            if (pc.is_stablecoin) {
                stablecoin_asset_ids.insert(pc.base_asset_id);
                stablecoin_asset_ids.insert(pc.quote_asset_id);
            }
        }

        for (const auto& pc : config_.pairs) {
            if (!pc.enabled) continue;
            if (pc.base_asset_id != "xch") continue;
            if (stablecoin_asset_ids.count(pc.quote_asset_id)) {
                const auto slash = pc.name.find('/');
                const std::string qname = (slash != std::string::npos)
                    ? pc.name.substr(slash + 1) : pc.quote_asset_id;
                xch_stable_pairs.push_back({pc.name, qname,
                                            pc.quote_asset_id});
            }
        }

        if (xch_stable_pairs.size() >= 2) {
            // Get the BYC/wUSDC.b cross-rate for normalisation.
            // If no stablecoin pair exists, assume 1.0 (at peg).
            double stable_cross_rate = 1.0;
            std::string stable_pair_base_id;
            for (const auto& pc : config_.pairs) {
                if (pc.is_stablecoin && pc.enabled) {
                    const double mid = market_data_->get_mid_price(pc.name);
                    if (mid > 0.0) stable_cross_rate = mid;
                    stable_pair_base_id = pc.base_asset_id;
                    break;
                }
            }

            const double cs_min_edge_bps =
                config_.arbitrage.cross_stable_min_edge_bps;
            const double cs_max_take_xch =
                config_.arbitrage.cross_stable_max_take_xch;

            for (std::size_t i = 0; i < xch_stable_pairs.size(); ++i) {
                const auto& pair_a = xch_stable_pairs[i];
                auto offers_a = market_data_->get_competing_offers(
                    pair_a.pair_name);

                // Find cheapest competing ask on pair A.
                Mojo best_ask_a = std::numeric_limits<Mojo>::max();
                std::string best_ask_a_id;
                Mojo best_ask_a_size = 0;
                for (const auto& co : offers_a) {
                    if (co.side == Side::Ask && co.price < best_ask_a) {
                        best_ask_a = co.price;
                        best_ask_a_id = co.offer_id;
                        best_ask_a_size = co.size;
                    }
                }
                if (best_ask_a == std::numeric_limits<Mojo>::max())
                    continue;

                for (std::size_t j = 0; j < xch_stable_pairs.size(); ++j) {
                    if (j == i) continue;
                    const auto& pair_b = xch_stable_pairs[j];
                    auto offers_b = market_data_->get_competing_offers(
                        pair_b.pair_name);

                    // Find highest competing bid on pair B.
                    Mojo best_bid_b = 0;
                    for (const auto& co : offers_b) {
                        if (co.side == Side::Bid && co.price > best_bid_b) {
                            best_bid_b = co.price;
                        }
                    }
                    if (best_bid_b == 0) continue;

                    // Normalise bid_b to pair_a's quote currency.
                    // stable_cross_rate = mid of stable pair (base/quote).
                    // e.g. BYC/wUSDC.b mid ~= 1.0.
                    double normalised_bid_b = 0.0;
                    if (pair_a.quote_asset_id == pair_b.quote_asset_id) {
                        normalised_bid_b = static_cast<double>(best_bid_b);
                    } else if (pair_a.quote_asset_id == stable_pair_base_id) {
                        // A quotes in stable-base (BYC), B quotes in
                        // stable-quote (wUSDC.b).  bid_b is wUSDC.b ->
                        // multiply by cross_rate to get BYC.
                        normalised_bid_b = static_cast<double>(best_bid_b)
                                         * stable_cross_rate;
                    } else {
                        // A quotes in stable-quote (wUSDC.b), B quotes in
                        // stable-base (BYC).  bid_b is BYC -> divide by
                        // cross_rate to get wUSDC.b.
                        normalised_bid_b = (stable_cross_rate > 0.0)
                            ? static_cast<double>(best_bid_b)
                              / stable_cross_rate
                            : 0.0;
                    }

                    const double ask_a_d = static_cast<double>(best_ask_a);
                    if (ask_a_d <= 0.0 || normalised_bid_b <= 0.0) continue;

                    const double edge_bps =
                        (normalised_bid_b - ask_a_d) / ask_a_d * 10000.0;

                    if (edge_bps < cs_min_edge_bps) {
                        spdlog::debug("[Engine] Step 9d: cross-stable "
                            "{} ask vs {} bid -- edge={:.1f}bps "
                            "< min={:.1f}bps",
                            pair_a.pair_name, pair_b.pair_name,
                            edge_bps, cs_min_edge_bps);
                        continue;
                    }

                    spdlog::info("[Engine] Step 9d: CROSS-STABLE ARB "
                        "detected -- buy XCH on {} (ask={}) sell on {} "
                        "(bid={}, normalised={:.0f}) edge={:.1f}bps",
                        pair_a.pair_name, best_ask_a,
                        pair_b.pair_name, best_bid_b,
                        normalised_bid_b, edge_bps);

                    // Take the cheap ask on pair A.
                    try {
                        auto offer_status =
                            co_await dexie_->get_offer_status(
                                best_ask_a_id);

                        if (!offer_status.success ||
                            offer_status.offer.offer_bech32.empty()) {
                            spdlog::warn("[Engine] Step 9d: failed to "
                                "fetch offer {} -- skipping",
                                best_ask_a_id.substr(0, 12));
                            continue;
                        }

                        if (offer_status.offer.status != 0) {
                            spdlog::info("[Engine] Step 9d: offer {} "
                                "no longer active (status={}) -- skip",
                                best_ask_a_id.substr(0, 12),
                                offer_status.offer.status);
                            continue;
                        }

                        const Mojo max_take_mojos = static_cast<Mojo>(
                            cs_max_take_xch
                            * static_cast<double>(kMojosPerXch));
                        const Mojo take_size = std::min(
                            best_ask_a_size, max_take_mojos);

                        const std::uint64_t fee = fee_tracker_
                            ? fee_tracker_->get_recommended_fee(
                                  config_.fees.min_fee_mojos,
                                  block_height)
                            : config_.fees.min_fee_mojos;

                        spdlog::info("[Engine] Step 9d: TAKING "
                            "cross-stable arb on {} -- offer={} "
                            "edge={:.1f}bps size={} fee={}",
                            pair_a.pair_name,
                            best_ask_a_id.substr(0, 12),
                            edge_bps, take_size, fee);

                        auto result = co_await wallet_->take_offer(
                            offer_status.offer.offer_bech32, fee);

                        if (result.contains("success") &&
                            result["success"].get<bool>()) {
                            const std::string trade_id =
                                result.contains("trade_record") &&
                                result["trade_record"].contains(
                                    "trade_id")
                                ? result["trade_record"]["trade_id"]
                                      .get<std::string>()
                                : "unknown";

                            spdlog::info("[Engine] Step 9d: TOOK "
                                "cross-stable arb -- buy {} sell {} "
                                "trade_id={} edge={:.1f}bps size={}",
                                pair_a.pair_name, pair_b.pair_name,
                                trade_id.substr(0, 12),
                                edge_bps, take_size);

                            if (fee_tracker_) {
                                fee_tracker_->record_fee(
                                    fee, block_height);
                            }

                            if (alerts_) {
                                alerts_->send_alert(
                                    AlertRule::ArbitrageDetected,
                                    "Cross-stable arb TAKEN: buy " +
                                    pair_a.pair_name + " sell " +
                                    pair_b.pair_name + " edge=" +
                                    std::to_string(edge_bps) + "bps");
                            }
                        } else {
                            const std::string err =
                                result.contains("error")
                                ? result["error"].get<std::string>()
                                : "unknown error";
                            spdlog::warn("[Engine] Step 9d: take_offer "
                                "failed on {}: {}",
                                pair_a.pair_name, err);
                        }
                    } catch (const std::exception& e) {
                        spdlog::error("[Engine] Step 9d: cross-stable "
                            "take failed on {}: {}",
                            pair_a.pair_name, e.what());
                    }
                }
            }
        }
    }

    // -- 9e: Peg-crossing offer taker ----------------------------------
    // On stablecoin pairs (BYC/wUSDC.b), competing offers that cross
    // the $1 peg are mispriced when the peg is trusted (Normal depeg
    // status).  BIDs above peg = free premium to sell into; ASKs below
    // peg = discounted buys.  Only fires when depeg detector says Normal
    // (no peg-loss alert), preventing us from buying into a real depeg.

    if (config_.arbitrage.enabled &&
        config_.arbitrage.peg_arb_enabled &&
        dexie_ && wallet_ && depeg_detector_ && !wallet_circuit_open_) {

        const double peg_min_edge = config_.arbitrage.peg_arb_min_edge_bps;
        const double peg_max_units = config_.arbitrage.peg_arb_max_take_units;

        for (const auto& pair : config_.pairs) {
            if (!pair.enabled || !pair.is_stablecoin) continue;

            // Only take when peg is trusted (Normal state).
            const auto* ds = depeg_detector_->get_state(pair.name);
            if (!ds || ds->status != DepegStatus::Normal) {
                spdlog::debug("[Engine] Step 9e: {} depeg status={} "
                              "-- skipping peg-arb",
                              pair.name,
                              ds ? static_cast<int>(ds->status) : -1);
                continue;
            }

            const Mojo peg_mojos = static_cast<Mojo>(
                pair.peg_target * static_cast<double>(kMojosPerXch));

            auto comp = market_data_->get_competing_offers(pair.name);
            if (comp.empty()) continue;

            // Inventory ratio guard: suppress buy-side takes that would
            // push the base holding beyond peg_arb_max_inventory_ratio,
            // and suppress sell-side takes that would push quote beyond
            // the same limit (i.e. base below 1 - limit).
            const double peg_max_ratio =
                config_.arbitrage.peg_arb_max_inventory_ratio;
            double inv_ratio_9e = 0.5;
            if (inventory_) {
                Mojo mid9e = static_cast<Mojo>(std::llround(
                    market_data_->get_mid_price(pair.name)
                    * static_cast<double>(kMojosPerXch)));
                inv_ratio_9e = inventory_->inventory_ratio(
                    AssetId{pair.base_asset_id},
                    AssetId{pair.quote_asset_id}, mid9e);
            }
            const bool suppress_buy  = (inv_ratio_9e >= peg_max_ratio);
            const bool suppress_sell = (inv_ratio_9e <= (1.0 - peg_max_ratio));

            // Best crossing offer per side.
            struct PegCandidate {
                std::string id;
                Mojo price{0};
                Mojo size{0};
                Side side{Side::Bid};
                double edge_bps{0.0};
            };
            std::optional<PegCandidate> best_bid;  // BID > peg
            std::optional<PegCandidate> best_ask;  // ASK < peg

            for (const auto& co : comp) {
                if (co.side == Side::Bid && co.price > peg_mojos) {
                    const double e = (static_cast<double>(co.price) -
                                      static_cast<double>(peg_mojos))
                                     / static_cast<double>(peg_mojos) * 10000.0;
                    if (e >= peg_min_edge &&
                        (!best_bid || co.price > best_bid->price)) {
                        best_bid = PegCandidate{co.offer_id, co.price,
                                               co.size, co.side, e};
                    }
                }
                if (co.side == Side::Ask && co.price < peg_mojos) {
                    const double e = (static_cast<double>(peg_mojos) -
                                      static_cast<double>(co.price))
                                     / static_cast<double>(peg_mojos) * 10000.0;
                    if (e >= peg_min_edge &&
                        (!best_ask || co.price < best_ask->price)) {
                        best_ask = PegCandidate{co.offer_id, co.price,
                                               co.size, co.side, e};
                    }
                }
            }

            // Lambda to take a single peg-crossing offer.
            auto take_peg = [&](const PegCandidate& c)
                -> asio::awaitable<void> {
                const Mojo max_mojos = static_cast<Mojo>(
                    peg_max_units
                    * static_cast<double>(pair.base_mojos_per_unit));
                const Mojo take_sz = std::min(c.size, max_mojos);

                spdlog::info("[Engine] Step 9e: {} PEG-CROSS {} "
                             "price={} peg={} edge={:.1f}bps "
                             "offer={} size={}",
                             pair.name, to_string(c.side),
                             c.price, peg_mojos, c.edge_bps,
                             c.id.substr(0, 12), take_sz);

                if (dry_run_) {
                    spdlog::info("[Engine] Step 9e: {} DRY RUN -- "
                                 "would take peg-cross {}",
                                 pair.name, c.id.substr(0, 12));
                    co_return;
                }

                auto os = co_await dexie_->get_offer_status(c.id);
                if (!os.success || os.offer.offer_bech32.empty()) {
                    spdlog::warn("[Engine] Step 9e: {} fetch failed "
                                 "for {} -- skip",
                                 pair.name, c.id.substr(0, 12));
                    co_return;
                }
                if (os.offer.status != 0) {
                    spdlog::info("[Engine] Step 9e: {} offer {} "
                                 "no longer active (status={})",
                                 pair.name, c.id.substr(0, 12),
                                 os.offer.status);
                    co_return;
                }

                const std::uint64_t fee = fee_tracker_
                    ? fee_tracker_->get_recommended_fee(
                          config_.fees.min_fee_mojos, block_height)
                    : config_.fees.min_fee_mojos;

                // Pre-balance check: verify we have enough spendable
                // balance before sending a doomed take_offer RPC.
                // For ASK takes (buying base) we need quote balance;
                // for BID takes (selling base) we need base balance.
                if (offer_mgr_) {
                    const std::string& spend_asset =
                        (c.side == Side::Ask)
                            ? pair.quote_asset_id
                            : pair.base_asset_id;
                    auto spend_wid =
                        offer_mgr_->resolve_wallet_id(spend_asset);
                    if (spend_wid > 0) {
                        try {
                            auto bal = co_await
                                wallet_->get_wallet_balance(spend_wid);
                            Mojo spendable = bal.value(
                                "spendable_balance",
                                static_cast<Mojo>(0));

                            // Estimate cost: for ASK take we pay
                            // take_sz * price / kMojosPerXch in quote
                            // mojos; for BID take we deliver take_sz
                            // base mojos.
                            const Mojo cost =
                                (c.side == Side::Ask)
                                    ? static_cast<Mojo>(
                                          static_cast<double>(take_sz)
                                          * static_cast<double>(c.price)
                                          / static_cast<double>(
                                                kMojosPerXch))
                                    : take_sz;

                            if (spendable < cost) {
                                spdlog::warn(
                                    "[Engine] Step 9e: {} SKIP {} "
                                    "take -- insufficient balance: "
                                    "need {} spendable {} (wallet {})",
                                    pair.name, to_string(c.side),
                                    cost, spendable, spend_wid);
                                co_return;
                            }
                        } catch (const std::exception& be) {
                            spdlog::debug(
                                "[Engine] Step 9e: {} balance check "
                                "failed: {} -- proceeding cautiously",
                                pair.name, be.what());
                        }
                    }
                }

                spdlog::info("[Engine] Step 9e: {} TAKING peg-cross "
                             "{} offer {} (edge={:.1f}bps size={} "
                             "fee={})",
                             pair.name, to_string(c.side),
                             c.id.substr(0, 12), c.edge_bps,
                             take_sz, fee);

                auto result = co_await wallet_->take_offer(
                    os.offer.offer_bech32, fee);

                if (result.contains("success") &&
                    result["success"].get<bool>()) {
                    const std::string tid =
                        result.contains("trade_record") &&
                        result["trade_record"].contains("trade_id")
                        ? result["trade_record"]["trade_id"]
                              .get<std::string>()
                        : "unknown";

                    spdlog::info("[Engine] Step 9e: {} TOOK peg-cross "
                                 "{} -- trade={} edge={:.1f}bps "
                                 "size={}",
                                 pair.name, to_string(c.side),
                                 tid.substr(0, 12), c.edge_bps,
                                 take_sz);

                    if (fee_tracker_)
                        fee_tracker_->record_fee(fee, block_height);
                    if (alerts_) {
                        alerts_->send_alert(
                            AlertRule::ArbitrageDetected,
                            pair.name + " peg-cross " +
                            to_string(c.side) + " TAKEN: edge=" +
                            std::to_string(c.edge_bps) + "bps");
                    }
                } else {
                    const std::string err = result.contains("error")
                        ? result["error"].get<std::string>()
                        : "unknown error";
                    spdlog::warn("[Engine] Step 9e: {} take failed: {}",
                                 pair.name, err);
                }
            };

            try {
                if (best_bid && !suppress_sell) {
                    co_await take_peg(*best_bid);
                } else if (best_bid && suppress_sell) {
                    spdlog::info("[Engine] Step 9e: {} SKIP BID take -- "
                                 "inv_ratio={:.3f} <= {:.2f} (too much quote)",
                                 pair.name, inv_ratio_9e, 1.0 - peg_max_ratio);
                }
                if (best_ask && !suppress_buy) {
                    co_await take_peg(*best_ask);
                } else if (best_ask && suppress_buy) {
                    spdlog::info("[Engine] Step 9e: {} SKIP ASK take -- "
                                 "inv_ratio={:.3f} >= {:.2f} (too much base)",
                                 pair.name, inv_ratio_9e, peg_max_ratio);
                }
            } catch (const std::exception& e) {
                spdlog::error("[Engine] Step 9e: {} peg-arb failed: {}",
                              pair.name, e.what());
            }
        }
    }

    co_return;
}

// ---------------------------------------------------------------------------
// Step 9f: Drift corrector -- active asset rebalancer.
//
// Periodically inspect the portfolio's per-asset XCH-equivalent share.
// For any asset whose share is outside target +/- trigger_factor*tolerance,
// scan Dexie for competitively-priced offers that move us back toward
// target.  Hysteresis: stop taking once the share is back within
// target +/- exit_factor*tolerance.
//
//   * Overweight base: look for BIDs (sell base for quote).
//   * Underweight base: look for ASKs (buy base with quote).
//
// Safety:
//   * Disabled by default; requires arbitrage.drift_corrector_enabled=true
//     AND strategy.asset_drift_guard_enabled=true.
//   * Per-block cooldown (drift_corrector_cooldown_blocks).
//   * Rolling 24h trade quota (drift_corrector_max_trades_per_day).
//   * Respects wallet circuit breaker, dry_run_, and the unified pre-take
//     balance check.
//   * Price must be within drift_corrector_max_premium_bps of mid.
// ---------------------------------------------------------------------------
asio::awaitable<void> Engine::step_run_drift_corrector(BlockHeight block_height)
{
    const auto& acfg = config_.arbitrage;
    const auto& scfg = config_.strategy;

    if (!acfg.enabled || !acfg.drift_corrector_enabled) co_return;
    if (!scfg.asset_drift_guard_enabled) co_return;
    if (scfg.asset_target_allocations.empty()) co_return;
    if (!dexie_ || !wallet_ || !market_data_) co_return;
    if (wallet_circuit_open_) co_return;

    // -- Cooldown ----------------------------------------------------------
    if (last_drift_correction_block_ != 0 &&
        block_height < last_drift_correction_block_ +
                       acfg.drift_corrector_cooldown_blocks) {
        co_return;
    }

    // -- Daily quota: trim history older than 24h --------------------------
    const auto now = std::chrono::system_clock::now();
    const auto day_ago = now - std::chrono::hours(24);
    while (!drift_correction_history_.empty() &&
           drift_correction_history_.front() < day_ago) {
        drift_correction_history_.pop_front();
    }
    if (drift_correction_history_.size() >=
        acfg.drift_corrector_max_trades_per_day) {
        spdlog::info("[Engine] Step 9f: daily quota reached ({}/{})",
                     drift_correction_history_.size(),
                     acfg.drift_corrector_max_trades_per_day);
        co_return;
    }

    // -- Compute per-asset portfolio percentages (XCH-equivalent) ----------
    auto upper = [](std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };
    auto to_lower = [](std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto resolve_symbol = [&](const std::string& asset_id) -> std::string {
        std::string lid = to_lower(asset_id);
        if (lid == "xch") return "XCH";
        for (const auto& pair : config_.pairs) {
            if (to_lower(pair.base_asset_id) == lid) {
                auto pos = pair.name.find('/');
                if (pos != std::string::npos) {
                    auto sym = pair.name.substr(0, pos);
                    for (char& c : sym) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    return sym;
                }
            }
            if (to_lower(pair.quote_asset_id) == lid) {
                auto pos = pair.name.find('/');
                if (pos != std::string::npos) {
                    auto sym = pair.name.substr(pos + 1);
                    for (char& c : sym) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    return sym;
                }
            }
        }
        auto uid = asset_id;
        for (char& c : uid) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return uid;
    };

    std::unordered_map<std::string, double> portfolio_pct_by_asset;
    {
        const auto positions = state_->get_all_positions();
        double total_xch = 0.0;
        std::unordered_map<std::string, double> asset_xch;
        for (const auto& p : positions) {
            const double v = static_cast<double>(
                PreTradeCheck::mark_to_xch(p, *state_));
            if (v <= 0.0) continue;
            asset_xch[resolve_symbol(p.asset_id)] += v;
            total_xch += v;
        }
        if (total_xch <= 0.0) co_return;
        for (const auto& [k, v] : asset_xch) {
            portfolio_pct_by_asset[k] = v / total_xch;
        }
    }

    // -- Classify each configured asset as Overweight / Underweight / OK --
    enum class DriftState { Ok, Overweight, Underweight };
    auto drift_state_name = [](DriftState s) -> const char* {
        switch (s) {
            case DriftState::Overweight:  return "OW";
            case DriftState::Underweight: return "UW";
            case DriftState::Ok:          return "ok";
        }
        return "ok";
    };
    auto classify = [&](const std::string& asset_upper) -> DriftState {
        auto it_t = scfg.asset_target_allocations.find(asset_upper);
        if (it_t == scfg.asset_target_allocations.end())
            return DriftState::Ok;
        auto it_tol = scfg.asset_target_tolerances.find(asset_upper);
        const double target = it_t->second;
        const double tol = (it_tol != scfg.asset_target_tolerances.end())
                           ? it_tol->second : 0.0;
        if (tol <= 0.0) return DriftState::Ok;

        auto it_p = portfolio_pct_by_asset.find(asset_upper);
        const double pct = (it_p != portfolio_pct_by_asset.end())
                           ? it_p->second : 0.0;

        const double trigger = tol * acfg.drift_corrector_trigger_factor;
        if (pct > target + trigger) return DriftState::Overweight;
        if (pct < target - trigger) return DriftState::Underweight;
        return DriftState::Ok;
    };

    // Exit-state check (hysteresis): once we're inside the exit band the
    // caller is expected to stop taking.  Used after a candidate take to
    // avoid overshoot when multiple offers are queued in one block.
    auto in_exit_band = [&](const std::string& asset_upper) -> bool {
        auto it_t = scfg.asset_target_allocations.find(asset_upper);
        if (it_t == scfg.asset_target_allocations.end()) return true;
        auto it_tol = scfg.asset_target_tolerances.find(asset_upper);
        const double target = it_t->second;
        const double tol = (it_tol != scfg.asset_target_tolerances.end())
                           ? it_tol->second : 0.0;
        const double exit_band = tol * acfg.drift_corrector_exit_factor;
        auto it_p = portfolio_pct_by_asset.find(asset_upper);
        const double pct = (it_p != portfolio_pct_by_asset.end())
                           ? it_p->second : 0.0;
        return std::abs(pct - target) <= exit_band;
    };

    // -- Walk pairs; for each, see if taking helps a breached asset -------
    const double max_prem_bps = acfg.drift_corrector_max_premium_bps;
    const double max_units = acfg.drift_corrector_max_take_units;
    bool took_any = false;

    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;
        if (took_any) break;  // one take per block

        const std::string base_u = resolve_symbol(pair.base_asset_id);
        const std::string quote_u = resolve_symbol(pair.quote_asset_id);
        const DriftState base_state = classify(base_u);
        const DriftState quote_state = classify(quote_u);

        // Determine helpful side(s) for this pair.
        //   ASK take -> base UP, quote DOWN
        //   BID take -> base DOWN, quote UP
        const bool want_ask = (base_state == DriftState::Underweight) ||
                              (quote_state == DriftState::Overweight);
        const bool want_bid = (base_state == DriftState::Overweight) ||
                              (quote_state == DriftState::Underweight);
        if (!want_ask && !want_bid) continue;

        // Don't actively swap an already-OK asset away from target.
        // Require at least one side of the pair to be breached.
        if (base_state == DriftState::Ok && quote_state == DriftState::Ok) {
            continue;
        }

        double recent_ask_share = 0.5;
        std::size_t recent_fill_count = 0;
        double recent_fill_bias_bps = 0.0;
        if (db_) {
            try {
                const auto recent_since = now - std::chrono::hours(24);
                const auto recent_trades = db_->query_trades(
                    pair.name,
                    PnLTracker::timestamp_to_iso(recent_since),
                    PnLTracker::timestamp_to_iso(now));
                std::size_t ask_fills = 0;
                std::size_t bid_fills = 0;
                for (const auto& tr : recent_trades) {
                    if (tr.side == "ask") {
                        ++ask_fills;
                    } else if (tr.side == "bid") {
                        ++bid_fills;
                    }
                }
                recent_fill_count = ask_fills + bid_fills;
                if (recent_fill_count > 0) {
                    recent_ask_share = static_cast<double>(ask_fills)
                                     / static_cast<double>(recent_fill_count);
                    recent_fill_bias_bps = std::clamp(
                        (recent_ask_share - 0.5) * 80.0,
                        -30.0,
                        30.0);
                }
            } catch (const std::exception& e) {
                spdlog::debug("[Engine] Step 9f: {} recent fill skew lookup "
                              "failed: {}",
                              pair.name, e.what());
            }
        }

        auto comp = market_data_->get_competing_offers(pair.name);
        if (comp.empty()) {
            spdlog::info("[Engine] Step 9f: {} wants drift correction "
                         "(base={} {} quote={} {}, want_ask={} want_bid={}) "
                         "but has no competing offers",
                         pair.name, base_u, drift_state_name(base_state),
                         quote_u, drift_state_name(quote_state),
                         want_ask, want_bid);
            continue;
        }

        Mojo mid = 0;
        const auto snap = state_->get_market(pair.name);
        if (snap.best_bid > 0 && snap.best_ask > 0) {
            mid = (snap.best_bid + snap.best_ask) / 2;
        }
        if (mid == 0) {
            mid = static_cast<Mojo>(std::llround(
                market_data_->get_mid_price(pair.name)
                * static_cast<double>(kMojosPerXch)));
        }
        if (mid == 0) {
            spdlog::info("[Engine] Step 9f: {} wants drift correction "
                         "but has no usable mid price",
                         pair.name);
            continue;
        }

        // Pick best ASK (lowest price) and best BID (highest price) that
        // fall within max_premium_bps of mid.
        const Mojo max_mojos = static_cast<Mojo>(
            max_units * static_cast<double>(pair.base_mojos_per_unit));
        auto ceil_to_mojo = [](long double value) -> Mojo {
            if (!std::isfinite(static_cast<double>(value)) || value <= 0.0L) {
                return 0;
            }
            const long double cap = static_cast<long double>(
                std::numeric_limits<Mojo>::max());
            if (value >= cap) {
                return std::numeric_limits<Mojo>::max();
            }
            return static_cast<Mojo>(std::ceil(value));
        };
        const long double base_mpu = static_cast<long double>(
            pair.base_mojos_per_unit > 0 ? pair.base_mojos_per_unit : 1);
        const long double quote_mpu = static_cast<long double>(
            pair.quote_mojos_per_unit > 0 ? pair.quote_mojos_per_unit : 1);
        const long double price_scale = static_cast<long double>(kMojosPerXch);
        auto quote_cost_for_ask = [&](Mojo base_size, Mojo price) -> Mojo {
            return ceil_to_mojo(
                static_cast<long double>(base_size)
                * static_cast<long double>(price)
                * quote_mpu / (base_mpu * price_scale));
        };
        auto base_cost_for_bid = [&](Mojo quote_size, Mojo price) -> Mojo {
            if (price <= 0) return static_cast<Mojo>(0);
            return ceil_to_mojo(
                static_cast<long double>(quote_size)
                * price_scale * base_mpu
                / (static_cast<long double>(price) * quote_mpu));
        };

        struct Cand {
            std::string id;
            Mojo price{0};
            Mojo size{0};
            Mojo base_size{0};
            Mojo spend_cost{0};
            Side side{Side::Bid};
            double premium_bps{0.0};
        };
        std::optional<Cand> best_ask;
        std::optional<Cand> best_bid;

        for (const auto& co : comp) {
            if (co.size == 0 || co.price == 0) continue;
            const double mid_d = static_cast<double>(mid);
            const double price_d = static_cast<double>(co.price);
            if (co.side == Side::Ask && want_ask) {
                const Mojo base_size = co.size;
                const Mojo spend_cost = quote_cost_for_ask(co.size, co.price);
                if (base_size == 0 || spend_cost == 0 || base_size > max_mojos) {
                    continue;
                }
                // premium = how much above mid we'd pay
                const double prem = (price_d - mid_d) / mid_d * 10000.0;
                if (prem <= max_prem_bps &&
                    (!best_ask || co.price < best_ask->price)) {
                    best_ask = Cand{co.offer_id, co.price, co.size,
                                    base_size, spend_cost, co.side, prem};
                }
            } else if (co.side == Side::Bid && want_bid) {
                const Mojo base_size = base_cost_for_bid(co.size, co.price);
                if (base_size == 0 || base_size > max_mojos) {
                    continue;
                }
                // premium = how much below mid we'd accept
                const double prem = (mid_d - price_d) / mid_d * 10000.0;
                if (prem <= max_prem_bps &&
                    (!best_bid || co.price > best_bid->price)) {
                    best_bid = Cand{co.offer_id, co.price, co.size,
                                    base_size, base_size, co.side, prem};
                }
            }
        }

        // Prefer the side that addresses the more-breached asset.  If both
        // are candidates, take whichever has the better (lower) premium.
        std::optional<Cand> chosen;
        auto adjusted_premium = [&](const Cand& cand) -> double {
            if (recent_fill_count < 10) {
                return cand.premium_bps;
            }
            const double bias = (cand.side == Side::Ask)
                ? recent_fill_bias_bps
                : -recent_fill_bias_bps;
            return cand.premium_bps + bias;
        };
        if (best_ask && best_bid) {
            chosen = (adjusted_premium(*best_ask) <= adjusted_premium(*best_bid))
                     ? best_ask : best_bid;
        } else if (best_ask) {
            chosen = best_ask;
        } else if (best_bid) {
            chosen = best_bid;
        }
        if (!chosen) {
            spdlog::info("[Engine] Step 9f: {} wants drift correction "
                         "(base={} {} quote={} {}, want_ask={} want_bid={}) "
                         "but no offer passed max_premium={}bps among {} "
                         "competing offers (recent_ask_share={:.2f}, "
                         "bias={:+.1f}bps)",
                         pair.name, base_u, drift_state_name(base_state),
                         quote_u, drift_state_name(quote_state),
                         want_ask, want_bid, max_prem_bps, comp.size(),
                         recent_ask_share, recent_fill_bias_bps);
            continue;
        }

        const Mojo take_sz = chosen->base_size;
        if (take_sz == 0) continue;

        spdlog::info("[Engine] Step 9f: {} DRIFT-CORRECT {} "
                     "base={}({}) quote={}({}) price={} mid={} "
                     "prem={:.1f}bps offer={} size={} recent_ask_share={:.2f} "
                     "bias={:+.1f}bps",
                     pair.name, to_string(chosen->side),
                     base_u,
                     base_state == DriftState::Overweight ? "OW" :
                     base_state == DriftState::Underweight ? "UW" : "ok",
                     quote_u,
                     quote_state == DriftState::Overweight ? "OW" :
                     quote_state == DriftState::Underweight ? "UW" : "ok",
                     chosen->price, mid, chosen->premium_bps,
                     chosen->id.substr(0, 12), take_sz,
                     recent_ask_share, recent_fill_bias_bps);

        if (dry_run_) {
            spdlog::info("[Engine] Step 9f: {} DRY RUN -- would take {}",
                         pair.name, chosen->id.substr(0, 12));
            continue;
        }

        // Fetch live offer bech32 from Dexie.
        auto os = co_await dexie_->get_offer_status(chosen->id);
        if (!os.success || os.offer.offer_bech32.empty()) {
            spdlog::warn("[Engine] Step 9f: {} fetch failed for {} -- skip",
                         pair.name, chosen->id.substr(0, 12));
            continue;
        }
        if (os.offer.status != 0) {
            spdlog::info("[Engine] Step 9f: {} offer {} no longer active "
                         "(status={})",
                         pair.name, chosen->id.substr(0, 12),
                         os.offer.status);
            continue;
        }

        // Pre-balance check (mirrors Step 9e).
        if (offer_mgr_) {
            const std::string& spend_asset =
                (chosen->side == Side::Ask)
                    ? pair.quote_asset_id : pair.base_asset_id;
            auto spend_wid = offer_mgr_->resolve_wallet_id(spend_asset);
            if (spend_wid > 0) {
                try {
                    auto bal = co_await
                        wallet_->get_wallet_balance(spend_wid);
                    Mojo spendable = bal.value(
                        "spendable_balance", static_cast<Mojo>(0));
                      const Mojo cost = chosen->spend_cost;
                    if (spendable < cost) {
                        spdlog::warn("[Engine] Step 9f: {} SKIP -- "
                                     "insufficient balance: need {} "
                                     "spendable {} (wallet {})",
                                     pair.name, cost, spendable,
                                     spend_wid);
                        continue;
                    }
                } catch (const std::exception& be) {
                    spdlog::debug("[Engine] Step 9f: {} balance check "
                                  "failed: {}", pair.name, be.what());
                }
            }
        }

        const std::uint64_t fee = fee_tracker_
            ? fee_tracker_->get_recommended_fee(
                  config_.fees.min_fee_mojos, block_height)
            : config_.fees.min_fee_mojos;

        spdlog::info("[Engine] Step 9f: {} TAKING drift-corr {} offer {} "
                     "(prem={:.1f}bps size={} fee={})",
                     pair.name, to_string(chosen->side),
                     chosen->id.substr(0, 12), chosen->premium_bps,
                     take_sz, fee);

        auto result = co_await wallet_->take_offer(
            os.offer.offer_bech32, fee);

        if (result.contains("success") && result["success"].get<bool>()) {
            const std::string tid =
                result.contains("trade_record") &&
                result["trade_record"].contains("trade_id")
                ? result["trade_record"]["trade_id"].get<std::string>()
                : "unknown";
            spdlog::info("[Engine] Step 9f: {} TOOK drift-corr {} -- "
                         "trade={} prem={:.1f}bps size={}",
                         pair.name, to_string(chosen->side),
                         tid.substr(0, 12), chosen->premium_bps, take_sz);
            if (fee_tracker_)
                fee_tracker_->record_fee(fee, block_height);
            if (alerts_) {
                alerts_->send_alert(
                    AlertRule::ArbitrageDetected,
                    pair.name + " drift-correct " +
                    to_string(chosen->side) + " TAKEN: prem=" +
                    std::to_string(chosen->premium_bps) + "bps");
            }
            last_drift_correction_block_ = block_height;
            drift_correction_history_.push_back(now);
            took_any = true;
            (void)in_exit_band;  // hysteresis tracked across blocks
        } else {
            const std::string err = result.contains("error")
                ? result["error"].get<std::string>() : "unknown error";
            spdlog::warn("[Engine] Step 9f: {} take failed: {}",
                         pair.name, err);
        }
    }

    co_return;
}

// ---------------------------------------------------------------------------
// XCH Recovery Mode
//
// When XCH spendable drops below recovery.xch_low_threshold:
//   1. Enter recovery mode (cancel all offers to free locked coins).
//   2. Scan Dexie order books for XCH-selling asks on XCH-base pairs.
//   3. Take asks priced within max_premium_bps of CoinGecko CEX reference.
//   4. Exit recovery when XCH spendable > xch_recovery_target.
//
// This ensures the engine can recover from an XCH-depleted state by
// spending counter-assets (wUSDC, BYC) to buy cheap XCH back.
// ---------------------------------------------------------------------------
asio::awaitable<void> Engine::step_xch_recovery(BlockHeight block_height)
{
    const auto& rcfg = config_.recovery;
    if (!rcfg.enabled) co_return;

    // -- 1. Check XCH spendable balance ------------------------------------
    Mojo xch_spendable = 0;
    Mojo xch_confirmed = 0;
    try {
        auto xch_bal = co_await wallet_->get_wallet_balance(1);
        if (xch_bal.contains("spendable_balance"))
            xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
        if (xch_bal.contains("confirmed_wallet_balance"))
            xch_confirmed = xch_bal["confirmed_wallet_balance"].get<Mojo>();
    } catch (const std::exception& e) {
        spdlog::warn("[Recovery] Failed to get XCH balance: {} -- skipping",
                     e.what());
        co_return;
    }

    const double xch_spendable_d =
        static_cast<double>(xch_spendable) / kMojosPerXch;
    const double xch_confirmed_d =
        static_cast<double>(xch_confirmed) / kMojosPerXch;

    // -- 2. Recovery mode transitions --------------------------------------
    if (!xch_recovery_mode_) {
        // Check if we need to enter recovery mode.
        if (xch_spendable_d >= rcfg.xch_low_threshold) {
            co_return;  // Balance is fine.
        }

        // If spendable is low but confirmed is healthy, the XCH is just
        // locked by our own offers (UTXO locking), not truly depleted.
        // Don't enter recovery ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â the offers will either fill (returning
        // XCH) or be cancelled (freeing UTXOs).
        if (xch_confirmed_d >= rcfg.xch_low_threshold) {
            spdlog::debug("[Recovery] XCH spendable {:.6f} < {:.4f} but "
                          "confirmed {:.6f} is healthy -- UTXO locking "
                          "from own offers, not entering recovery",
                          xch_spendable_d, rcfg.xch_low_threshold,
                          xch_confirmed_d);
            co_return;
        }

        // Enter recovery mode.
        xch_recovery_mode_ = true;
        xch_recovery_cancelled_ = false;
        spdlog::warn("[Recovery] ENTERING recovery mode: XCH spendable "
                     "{:.6f} confirmed {:.6f} < threshold {:.4f} XCH "
                     "-- will cancel offers and seek cheap XCH asks",
                     xch_spendable_d, xch_confirmed_d,
                     rcfg.xch_low_threshold);

        if (alerts_) {
            alerts_->send_alert(AlertRule::ArbitrageDetected,
                "XCH Recovery Mode ENTERED: spendable " +
                std::to_string(xch_spendable_d) + " XCH < " +
                std::to_string(rcfg.xch_low_threshold) + " threshold");
        }
    } else {
        // Check if we can exit recovery mode.
        if (xch_spendable_d >= rcfg.xch_recovery_target) {
            xch_recovery_mode_ = false;
            xch_recovery_cancelled_ = false;
            spdlog::info("[Recovery] EXITING recovery mode: XCH spendable "
                         "{:.6f} >= target {:.4f} XCH -- resuming normal "
                         "trading", xch_spendable_d, rcfg.xch_recovery_target);

            if (alerts_) {
                alerts_->send_alert(AlertRule::ArbitrageDetected,
                    "XCH Recovery Mode EXITED: spendable " +
                    std::to_string(xch_spendable_d) + " XCH -- normal "
                    "trading resumed");
            }
            co_return;
        }

        spdlog::info("[Recovery] Active: XCH spendable {:.6f} "
                     "(target: {:.4f} XCH)", xch_spendable_d,
                     rcfg.xch_recovery_target);
    }

    // -- 3. Cancel all offers on first entry (free locked coins) -----------
    if (rcfg.cancel_on_enter && !xch_recovery_cancelled_) {
        spdlog::info("[Recovery] Cancelling all offers to free locked coins");

        // Use wallet RPC directly (not offer_mgr_->cancel_all()) because
        // the engine state may not track offers from previous instances.
        // Use fee=0 and secure=false so cancellation works even with 0 XCH
        // spendable.  Non-secure cancel invalidates offers locally and
        // releases the locked UTXOs immediately; offers may still appear
        // on Dexie until they expire or are taken.
        bool cancel_ok = false;
        try {
            co_await wallet_->cancel_offers(/*fee=*/0, /*secure=*/false);
            spdlog::info("[Recovery] Wallet-level cancel_offers(fee=0, "
                         "secure=false) succeeded -- all pending offers "
                         "invalidated locally");
            cancel_ok = true;

            // Also mark all tracked offers as cancel_pending.
            auto tracked = state_->get_all_offers();
            for (const auto& po : tracked) {
                state_->mark_cancel_pending(po.offer_id);
            }
            spdlog::info("[Recovery] Marked {} offers as cancel_pending",
                         tracked.size());
        } catch (const std::exception& e) {
            spdlog::error("[Recovery] wallet cancel_offers failed: {} "
                          "-- will retry next block", e.what());
        }
        xch_recovery_cancelled_ = cancel_ok;

        // After cancellation, coins need time to settle (pending_change).
        // Skip acquisition this block; next block they'll be spendable.
        if (cancel_ok) {
            spdlog::info("[Recovery] Waiting for coins to settle after "
                         "cancellation -- will scan for XCH asks next block");
            co_return;
        }
    }

    // -- 4. Derive CEX reference price for XCH/wUSDC.b --------------------
    //    Used to evaluate whether an ask is "reasonably priced."
    double cex_xch_usdc = 0.0;
    {
        auto xch_it  = coingecko_prices_.find("chia");
        auto usdc_it = coingecko_prices_.find("usd-coin");
        if (xch_it != coingecko_prices_.end() &&
            usdc_it != coingecko_prices_.end() &&
            usdc_it->second > 0.0) {
            cex_xch_usdc = xch_it->second / usdc_it->second;
        }
    }

    if (cex_xch_usdc <= 0.0) {
        spdlog::warn("[Recovery] No CoinGecko CEX reference for XCH/wUSDC "
                     "-- cannot evaluate ask prices, skipping");
        co_return;
    }

    spdlog::info("[Recovery] CEX reference XCH/wUSDC = {:.6f}", cex_xch_usdc);

    // -- 5. Scan XCH-base pairs for cheap asks to take ---------------------
    //    An "ask" on an XCH-base pair means someone is selling XCH.
    //    Taking it gives us XCH in exchange for the quote asset.
    const double max_take_mojos_total =
        rcfg.max_take_per_block_xch * static_cast<double>(kMojosPerXch);
    double taken_mojos_this_block = 0.0;

    for (const auto& pair : config_.pairs) {
        if (!pair.enabled) continue;

        // Only process XCH-base pairs (we want to BUY XCH).
        // XCH-base means base_asset_id is the XCH asset ID.
        // On Dexie, XCH has no explicit asset ID (it's the native coin).
        // We identify XCH-base pairs by checking if the pair name starts
        // with "XCH/".
        if (pair.name.substr(0, 4) != "XCH/") continue;

        // Get competing offers for this pair.
        auto comp_offers = market_data_->get_competing_offers(pair.name);
        if (comp_offers.empty()) continue;

        // Derive CEX reference for this specific pair.
        // For XCH/wUSDC.b we already have cex_xch_usdc.
        // For XCH/BYC we don't have a CEX price (BYC has no CoinGecko).
        // Skip pairs without CEX reference to avoid overpaying.
        double cex_ref = 0.0;
        if (pair.name == "XCH/wUSDC.b") {
            cex_ref = cex_xch_usdc;
        } else {
            // No CEX reference for this pair -- skip.
            spdlog::debug("[Recovery] {} -- no CEX reference, skipping",
                          pair.name);
            continue;
        }

        // Max acceptable price = CEX + premium.
        const double max_price_d = cex_ref * (1.0 + rcfg.max_premium_bps / 10000.0);
        const Mojo max_price_mojos = static_cast<Mojo>(std::llround(
            max_price_d * static_cast<double>(kMojosPerXch)));

        // Filter own offers.
        std::unordered_set<std::string> own_ids;
        auto pending = state_->get_all_offers();
        for (const auto& po : pending) {
            own_ids.insert(po.offer_id);
        }

        // Find the cheapest asks (someone selling XCH) within our budget.
        struct CandidateAsk {
            std::string offer_id;
            Mojo price;
            Mojo size;
        };
        std::vector<CandidateAsk> candidates;

        for (const auto& co : comp_offers) {
            if (co.side != Side::Ask) continue;
            if (own_ids.count(co.offer_id)) continue;
            if (co.price > max_price_mojos) continue;
            if (co.price == 0) continue;

            candidates.push_back({co.offer_id, co.price, co.size});
        }

        // Sort by price ascending (cheapest first).
        std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) { return a.price < b.price; });

        for (const auto& cand : candidates) {
            if (taken_mojos_this_block >= max_take_mojos_total) break;

            const double price_d =
                static_cast<double>(cand.price) / kMojosPerXch;
            const double premium_bps =
                (price_d - cex_ref) / cex_ref * 10000.0;

            spdlog::info("[Recovery] {} candidate ask: id={} price={:.6f} "
                         "(CEX={:.6f}, premium={:.1f}bps)",
                         pair.name, cand.offer_id.substr(0, 12),
                         price_d, cex_ref, premium_bps);

            // Fetch full offer text (bech32m) from Dexie.
            try {
                auto offer_status =
                    co_await dexie_->get_offer_status(cand.offer_id);

                if (!offer_status.success ||
                    offer_status.offer.offer_bech32.empty()) {
                    spdlog::warn("[Recovery] {} failed to fetch offer text "
                                 "for {} -- skipping",
                                 pair.name, cand.offer_id.substr(0, 12));
                    continue;
                }

                // Verify offer is still active (status 0 on Dexie).
                if (offer_status.offer.status != 0) {
                    spdlog::info("[Recovery] {} offer {} no longer active "
                                 "(status={}) -- skipping",
                                 pair.name, cand.offer_id.substr(0, 12),
                                 offer_status.offer.status);
                    continue;
                }

                // Use fee=0 when XCH is near-zero so the take doesn't
                // fail due to insufficient funds.  Zero-fee transactions
                // are valid on Chia (lower mempool priority but still
                // processed).  The taker only needs quote-asset coins
                // (e.g. wUSDC) plus the fee -- with fee=0, no XCH needed.
                const std::uint64_t fee =
                    (xch_spendable_d < 0.001) ? 0 : config_.fees.min_fee_mojos;

                spdlog::info("[Recovery] {} TAKING ask {} "
                             "(price={:.6f}, premium={:.1f}bps, fee={})",
                             pair.name, cand.offer_id.substr(0, 12),
                             price_d, premium_bps, fee);

                auto result = co_await wallet_->take_offer(
                    offer_status.offer.offer_bech32, fee);

                if (result.contains("success") &&
                    result["success"].get<bool>()) {
                    const std::string trade_id =
                        result.contains("trade_record") &&
                        result["trade_record"].contains("trade_id")
                        ? result["trade_record"]["trade_id"]
                              .get<std::string>()
                        : "unknown";

                    spdlog::info("[Recovery] {} TOOK ask -- "
                                 "trade_id={} price={:.6f} "
                                 "premium={:.1f}bps",
                                 pair.name, trade_id.substr(0, 12),
                                 price_d, premium_bps);

                    taken_mojos_this_block +=
                        static_cast<double>(cand.size);

                    if (fee_tracker_) {
                        fee_tracker_->record_fee(fee, block_height);
                    }

                    if (alerts_) {
                        alerts_->send_alert(
                            AlertRule::ArbitrageDetected,
                            "Recovery: TOOK " + pair.name + " ask at " +
                            std::to_string(price_d) + " (premium " +
                            std::to_string(premium_bps) + "bps)");
                    }

                    // Only take one offer per block to let coins settle.
                    break;

                } else {
                    const std::string err = result.contains("error")
                        ? result["error"].get<std::string>()
                        : "unknown error";
                    spdlog::warn("[Recovery] {} take_offer failed: {}",
                                 pair.name, err);
                }
            } catch (const std::exception& e) {
                spdlog::error("[Recovery] {} take failed: {}",
                              pair.name, e.what());
            }
        }
    }

    if (taken_mojos_this_block == 0.0) {
        spdlog::info("[Recovery] No acceptable XCH asks found this block "
                     "(max premium: {:.0f}bps over CEX {:.6f})",
                     rcfg.max_premium_bps, cex_xch_usdc);
    }

    co_return;
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
        kFallbackXchUsdRate,
        // [PNL-UNIT-FIX] Per-pair unit factor (quote_denom/base_denom).
        // Without this the inventory PnL is overstated by 1e9 for
        // CAT-quoted pairs like XCH/wUSDC.b (kMojosPerXch / 1e3).
        [this](const std::string& pair) -> double {
            const PairConfig* pc = find_pair_config(pair);
            if (!pc) return 1.0;
            const double q = static_cast<double>(pc->quote_mojos_per_unit);
            const double b = static_cast<double>(pc->base_mojos_per_unit);
            return (b > 0.0) ? (q / b) : 1.0;
        });

    // Persist a snapshot for each enabled pair.
    std::vector<DbSnapshot> batch;
    std::vector<DbStrategyQuote> quote_batch;
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

        // Phase 2: strategy decision parameters for post-hoc analysis.
        auto cycle_it = cycle_.find(pair.name);
        if (cycle_it != cycle_.end() && cycle_it->second.quote_valid) {
            const auto& pcs = cycle_it->second;

            // Reservation price = midpoint of raw bid/ask quote (in mojos).
            snap.reservation_price_mojos = static_cast<Mojo>(
                (pcs.risk_quote.bid_price + pcs.risk_quote.ask_price) / 2);

            // Spread decomposition from the spread optimizer.
            snap.half_spread_bps = pcs.spread_result.half_spread;
            snap.s_adverse_bps   = pcs.spread_result.s_adverse;
            snap.s_inventory_bps = pcs.spread_result.s_inventory;
            snap.s_cost_bps      = pcs.spread_result.s_cost;

            // Per-tier quotes for fill probability and tier optimization.
            for (const auto& tq : pcs.ladder) {
                DbStrategyQuote sq;
                sq.block_height = block_height;
                sq.pair_name    = pair.name;
                sq.tier         = tq.tier_index;
                sq.side         = (tq.side == Side::Bid) ? "bid" : "ask";
                sq.price_mojos  = tq.price;
                sq.size_mojos   = tq.size;
                quote_batch.push_back(std::move(sq));
            }
        }

        // Kappa from the fill-intensity calibrator.
        if (kappa_calibrator_) {
            snap.kappa = kappa_calibrator_->current_kappa();
        }

        // Variance ratio from the regime detector.
        snap.variance_ratio = regime.variance_ratio;

        // Adverse selection rate from the PIN estimator.
        auto pin_it = pin_estimators_.find(pair.name);
        if (pin_it != pin_estimators_.end()) {
            snap.adverse_rate = pin_it->second->get_adverse_rate();
        }

        batch.push_back(std::move(snap));
    }

    if (!batch.empty()) {
        db_->insert_snapshots_batch(batch);
    }

    if (!quote_batch.empty()) {
        db_->insert_strategy_quotes_batch(quote_batch);
    }

    spdlog::debug("[Engine] Step 11: PnL updated and {} snapshots, {} quotes persisted",
                  batch.size(), quote_batch.size());
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

    // Paused state gauge
    metrics_->update_bot_paused(gui_pause_active_);

    // Dashboard 8: Rolling 24-hour blockchain fees
    if (fee_tracker_ && fee_tracker_->enabled()) {
        metrics_->update_fees_paid_24h(
            fee_tracker_->get_rolling_total(block_height));
    }

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
    // [T8-03] Decrement the grace period counter each cycle.  During the
    // grace window the HWM drawdown check is skipped so that a small initial
    // loss from the zero-peak baseline does not immediately pause the engine.
    if (drawdown_grace_remaining_ > 0) {
        --drawdown_grace_remaining_;
    }

    if ((peak_pnl_hwm_ > 0 || total.total_pnl < 0)
            && drawdown_grace_remaining_ == 0) {
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
            spdlog::info("[Engine] Wallet-only mode active ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â wallet synced "
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

// ---------------------------------------------------------------------------
// check_pause_flag -- GUI-initiated pause via signal file.
// ---------------------------------------------------------------------------
void Engine::check_pause_flag()
{
    namespace fs = std::filesystem;
    const bool flag_exists = fs::exists(pause_flag_path_);

    if (flag_exists && !gui_pause_active_) {
        // Transition Running ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ Paused.
        gui_pause_active_ = true;
        if (state_->status() == BotStatus::Running) {
            state_->set_status(BotStatus::Paused);
            spdlog::info("[Engine] Pause flag detected -- entering Paused state "
                         "(Steps 1-6, 9-13 continue; Step 8 skipped)");
        }
    } else if (!flag_exists && gui_pause_active_) {
        // Transition Paused ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ Running.
        gui_pause_active_ = false;
        if (state_->status() == BotStatus::Paused) {
            state_->set_status(BotStatus::Running);
            spdlog::info("[Engine] Pause flag removed -- resuming trading");
        }
    }
}


// ---------------------------------------------------------------------------
// step_maintain_coin_pool -- ensure enough pre-split coins for trading
// ---------------------------------------------------------------------------
// Maintains the UTXO pool for ALL wallets used by enabled pairs (XCH + CATs).
//
// For XCH (wallet 1):
//   target count  = config_.strategy.coin_pool_target_count
//   denomination  = config_.strategy.coin_pool_target_xch (in XCH units)
//
// For each CAT wallet (BYC, wUSDC.b, etc.):
//   target count  = config_.strategy.cat_coin_pool_target_count
//   denomination  = config_.strategy.cat_coin_pool_target_units (in display units)
//   mojos         = target_units * mojos_per_unit for that asset
//
// The function collects unique (wallet_id, mojos_per_unit) pairs across all
// enabled pair configs, then runs ensure_split() for each one.
// ---------------------------------------------------------------------------

asio::awaitable<void> Engine::step_maintain_coin_pool(BlockHeight block_height)
{
    // -----------------------------------------------------------------------
    // Phase 1: XCH coin pool (wallet_id 1)
    // -----------------------------------------------------------------------
    const int xch_target_count = config_.strategy.coin_pool_target_count;
    if (xch_target_count > 0) {
        const double target_xch = config_.strategy.coin_pool_target_xch;
        const auto target_mojos = static_cast<Mojo>(
            std::llround(target_xch * static_cast<double>(kMojosPerXch)));

        // Skip if prior XCH splits are still confirming.
        bool xch_pending = false;
        try {
            auto bal = co_await wallet_->get_wallet_balance(1);
            Mojo pending_change = 0;
            if (bal.contains("pending_change"))
                pending_change = bal["pending_change"].get<Mojo>();
            if (pending_change > 0) {
                spdlog::debug("[Engine] Coin pool: XCH pending_change={} mojos, "
                              "waiting for prior split to confirm",
                              pending_change);
                xch_pending = true;
            }
        } catch (const std::exception& e) {
            spdlog::warn("[Engine] Coin pool: XCH balance check failed: {}",
                         e.what());
            xch_pending = true;
        }

        if (!xch_pending) {
            int free_count = 0;
            try {
                free_count = co_await coin_mgr_->count_free_coins(1);
            } catch (const std::exception& e) {
                spdlog::warn("[Engine] Coin pool: XCH count_free_coins failed: {}",
                             e.what());
            }

            if (free_count >= xch_target_count) {
                spdlog::debug("[Engine] Coin pool: XCH {} free coins >= target {} -- OK",
                              free_count, xch_target_count);
            } else {
                spdlog::info("[Engine] Coin pool: XCH {} free coins < target {} -- "
                             "splitting to create {} more coins of {:.2f} XCH each",
                             free_count, xch_target_count,
                             xch_target_count - free_count, target_xch);

                std::string address;
                try {
                    address = co_await wallet_->get_next_address(1, false);
                } catch (const std::exception& e) {
                    spdlog::error("[Engine] Coin pool: XCH get_next_address failed: {}",
                                  e.what());
                }

                if (!address.empty()) {
                    constexpr Mojo split_fee = 0;
                    try {
                        auto result = co_await coin_mgr_->ensure_split(
                            1, xch_target_count, target_mojos, address, split_fee);

                        if (result.success && result.coins_created > 0) {
                            spdlog::info("[Engine] Coin pool: XCH created {} new coins",
                                         result.coins_created);
                        } else if (!result.success) {
                            spdlog::warn("[Engine] Coin pool: XCH split failed "
                                         "(insufficient balance or RPC error)");
                        }
                    } catch (const std::exception& e) {
                        spdlog::error("[Engine] Coin pool: XCH ensure_split failed: {}",
                                      e.what());
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Phase 2: CAT coin pools (all unique non-XCH wallets from enabled pairs)
    // -----------------------------------------------------------------------
    const int cat_target_count = config_.strategy.cat_coin_pool_target_count;
    if (cat_target_count > 0 && offer_mgr_) {
        const double cat_target_units = config_.strategy.cat_coin_pool_target_units;

        // Collect unique (asset_id -> mojos_per_unit) from all enabled pairs.
        // Each asset may appear as both base and quote across different pairs.
        struct AssetInfo {
            std::int64_t wallet_id;
            std::int64_t mojos_per_unit;
            std::string  display_name;   // For logging only.
        };
        std::unordered_map<std::string, AssetInfo> cat_assets;

        for (const auto& pair : config_.pairs) {
            if (!pair.enabled) continue;

            // Base asset (skip XCH).
            if (pair.base_asset_id != "xch" && pair.base_asset_id != "XCH") {
                auto wid = offer_mgr_->resolve_wallet_id(pair.base_asset_id);
                if (wid > 0 && cat_assets.find(pair.base_asset_id) == cat_assets.end()) {
                    // Extract display name from pair name (e.g. "BYC" from "BYC/wUSDC.b").
                    std::string dname = pair.name;
                    auto slash = dname.find('/');
                    if (slash != std::string::npos) dname = dname.substr(0, slash);

                    cat_assets[pair.base_asset_id] = AssetInfo{
                        wid, pair.base_mojos_per_unit, dname};
                }
            }

            // Quote asset (skip XCH).
            if (pair.quote_asset_id != "xch" && pair.quote_asset_id != "XCH") {
                auto wid = offer_mgr_->resolve_wallet_id(pair.quote_asset_id);
                if (wid > 0 && cat_assets.find(pair.quote_asset_id) == cat_assets.end()) {
                    std::string dname = pair.name;
                    auto slash = dname.find('/');
                    if (slash != std::string::npos) dname = dname.substr(slash + 1);

                    cat_assets[pair.quote_asset_id] = AssetInfo{
                        wid, pair.quote_mojos_per_unit, dname};
                }
            }
        }

        // Process each unique CAT asset.
        for (const auto& [asset_id, info] : cat_assets) {
            // Compute target denomination in mojos.
            const auto target_mojos = static_cast<Mojo>(
                std::llround(cat_target_units * static_cast<double>(info.mojos_per_unit)));

            if (target_mojos <= 0) {
                spdlog::warn("[Engine] Coin pool: {} target_mojos=0 "
                             "(units={}, mpu={}), skipping",
                             info.display_name, cat_target_units,
                             info.mojos_per_unit);
                continue;
            }

            // Check for pending transactions on this wallet.
            bool cat_pending = false;
            try {
                auto bal = co_await wallet_->get_wallet_balance(info.wallet_id);
                Mojo pending_change = 0;
                if (bal.contains("pending_change"))
                    pending_change = bal["pending_change"].get<Mojo>();
                if (pending_change > 0) {
                    spdlog::debug("[Engine] Coin pool: {} (wid={}) "
                                  "pending_change={} mojos, skipping",
                                  info.display_name, info.wallet_id,
                                  pending_change);
                    cat_pending = true;
                }
            } catch (const std::exception& e) {
                spdlog::warn("[Engine] Coin pool: {} balance check failed: {}",
                             info.display_name, e.what());
                cat_pending = true;
            }

            if (cat_pending) continue;

            // Count free coins for this wallet.
            int free_count = 0;
            try {
                free_count = co_await coin_mgr_->count_free_coins(info.wallet_id);
            } catch (const std::exception& e) {
                spdlog::warn("[Engine] Coin pool: {} count_free_coins failed: {}",
                             info.display_name, e.what());
                continue;
            }

            if (free_count >= cat_target_count) {
                spdlog::debug("[Engine] Coin pool: {} {} free coins >= target {} -- OK",
                              info.display_name, free_count, cat_target_count);
                continue;
            }

            spdlog::info("[Engine] Coin pool: {} {} free coins < target {} -- "
                         "splitting to create {} more coins of {:.1f} units "
                         "({} mojos) each",
                         info.display_name, free_count, cat_target_count,
                         cat_target_count - free_count,
                         cat_target_units, target_mojos);

            // Get a receive address for this CAT wallet.
            std::string address;
            try {
                address = co_await wallet_->get_next_address(info.wallet_id, false);
            } catch (const std::exception& e) {
                spdlog::error("[Engine] Coin pool: {} get_next_address failed: {}",
                              info.display_name, e.what());
                continue;
            }

            // Execute the split.
            constexpr Mojo split_fee = 0;
            try {
                auto result = co_await coin_mgr_->ensure_split(
                    info.wallet_id, cat_target_count, target_mojos,
                    address, split_fee);

                if (result.success && result.coins_created > 0) {
                    spdlog::info("[Engine] Coin pool: {} created {} new coins "
                                 "of {} mojos each (wid={})",
                                 info.display_name, result.coins_created,
                                 target_mojos, info.wallet_id);
                } else if (!result.success) {
                    spdlog::warn("[Engine] Coin pool: {} split failed "
                                 "(insufficient balance or RPC error)",
                                 info.display_name);
                }
            } catch (const std::exception& e) {
                spdlog::error("[Engine] Coin pool: {} ensure_split failed: {}",
                              info.display_name, e.what());
            }
        }
    }

    coin_pool_last_block_ = block_height;
    co_return;
}
}  // namespace xop
