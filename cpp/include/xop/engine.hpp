// engine.hpp -- Top-level orchestrator for XOPTrader CHIA DEX market-maker.
//
// The Engine class owns every subsystem and drives the per-block heartbeat
// loop described in Section 13 of CHIA_MARKET_MAKER_STRATEGY.md.
//
// Architecture:
//   A boost::asio::io_context drives a native C++20 coroutine loop that
//   polls the Chia full node for the current block height every 5 seconds.
//   When a new block is detected, the engine executes the 13-step main
//   cycle as a co_await chain (single-threaded coroutine model, no
//   deadlock-prone co_spawn/use_future/.get() patterns).
//
//   All subsystems are constructed in the Engine constructor and wired
//   together through shared pointers to State and the Database.  This
//   guarantees deterministic initialization order and enables the
//   constructor to validate the entire configuration before entering the
//   main loop.
//
// Lifecycle:
//   Engine(AppConfig, dry_run)  -- construct all subsystems, validate config
//   run()                       -- open connections, enter main loop, block
//   shutdown()                  -- signal stop, cancel all offers, close
//
// Thread safety:
//   The engine runs on a single io_context thread.  Subsystems that need
//   concurrent access (State, InventoryTracker) provide their own internal
//   locking.  The engine never creates additional threads.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- secrets are never logged; audit trail via Database
//   ISO/IEC 5055       -- no raw owning pointers; RAII via unique_ptr
//   ISO/IEC 25000      -- documented lifecycle, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard C++20 coroutines via Boost.Asio

#ifndef XOP_ENGINE_HPP
#define XOP_ENGINE_HPP

#include "xop/config.hpp"
#include "xop/database.hpp"
#include "xop/state.hpp"
#include "xop/types.hpp"

// RPC / API clients
#include "xop/rpc/chia_rpc.hpp"
#include "xop/rpc/dexie_client.hpp"
#include "xop/rpc/coingecko_client.hpp"

// Execution layer
#include "xop/execution/coin_manager.hpp"
#include "xop/execution/market_data.hpp"
#include "xop/execution/offer_manager.hpp"

// Data / analytics
#include "xop/data/volatility.hpp"
#include "xop/data/adverse_selection.hpp"

// Strategy layer
#include "xop/strategy/base.hpp"
#include "xop/strategy/spread.hpp"
#include "xop/strategy/liquidity.hpp"
#include "xop/strategy/arbitrage.hpp"
#include "xop/strategy/depeg_detector.hpp"

// Risk layer
#include "xop/risk/inventory.hpp"
#include "xop/risk/limits.hpp"
#include "xop/risk/hedging.hpp"

// Monitoring layer
#include "xop/monitoring/pnl.hpp"
#include "xop/monitoring/metrics.hpp"
#include "xop/monitoring/alerts.hpp"
#include "xop/monitoring/on_chain_reconciler.hpp"

// New strategy modules
#include "xop/strategy/order_book_tactics.hpp"
#include "xop/strategy/strategy_portfolio.hpp"
#include "xop/strategy/chia_edge.hpp"
#include "xop/strategy/new_strategies.hpp"
#include "xop/strategy/fee_tracker.hpp"
#include "xop/strategy/kappa_calibrator.hpp"
#include "xop/strategy/market_allocator.hpp"

// New risk modules
#include "xop/risk/loss_manager.hpp"
#include "xop/risk/drift_analyzer.hpp"

// Startup market analysis
#include "xop/data/market_analyzer.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// FlashCrashState -- three-state machine for flash-crash circuit breaker.
//
// Normal    : market is operating within expected parameters.
// Crash     : a flash crash has been detected (> threshold drop).
//             New offer posting (Step 8) is gated during this state.
// Recovery  : stability band met, awaiting required_stable_blocks.
//             New offer posting (Step 8) is still gated.
//
// Transitions:
//   Normal   -> Crash    : check_flash_crash() returns true.
//   Crash    -> Recovery : is_stable_after_crash() with partial window.
//   Recovery -> Normal   : is_stable_after_crash() with full window.
//   Recovery -> Crash    : another drop detected during recovery.
//
// ISO/IEC 5055: exhaustive enum with no implicit int conversion.
// ---------------------------------------------------------------------------
enum class FlashCrashState : std::uint8_t {
    Normal   = 0,
    Crash    = 1,
    Recovery = 2
};

/// Human-readable label for logging.
inline const char* to_string(FlashCrashState s) noexcept {
    switch (s) {
        case FlashCrashState::Normal:   return "Normal";
        case FlashCrashState::Crash:    return "Crash";
        case FlashCrashState::Recovery: return "Recovery";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// PostedOfferInfo -- associates a posted tier with the actual wallet-assigned
// offer ID (trade_id).  Used by step_manage_offers to persist accurate
// offer IDs to the database after post_quotes returns.
//
// ISO/IEC 5055: structured return prevents orphaned placeholder IDs.
// ---------------------------------------------------------------------------
struct PostedOfferInfo {
    std::string offer_id;       ///< Wallet-assigned trade_id.
    std::string pair_name;      ///< Trading pair name.
    Side        side;           ///< Bid or ask.
    Mojo        price;          ///< Price in mojos.
    Mojo        size;           ///< Size in mojos.
    int         tier_index;     ///< Tier index in the ladder.
};

// ---------------------------------------------------------------------------
// Engine -- the top-level orchestrator.
//
// Owns all subsystems and drives the per-block heartbeat loop.
// ---------------------------------------------------------------------------

class Engine {
public:
    // -- Construction --------------------------------------------------------

    /// Construct the engine, creating all subsystems from the given config.
    ///
    /// @param config   Fully validated application configuration.
    /// @param dry_run  If true, the engine simulates all wallet operations
    ///                 without broadcasting transactions on-chain.  Useful
    ///                 for integration testing against a live full node.
    ///
    /// @throws std::runtime_error if any subsystem fails to initialise.
    Engine(const AppConfig& config, bool dry_run);

    /// Destructor.  Calls shutdown() if the engine is still running.
    ~Engine();

    // Non-copyable, non-movable -- owns io_context and subsystem lifetimes.
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&)                 = delete;
    Engine& operator=(Engine&&)      = delete;

    // -- Lifecycle -----------------------------------------------------------

    /// Open all connections (full node, wallet, dexie), initialise the
    /// Prometheus exporter, and enter the main polling loop.
    ///
    /// This method blocks the calling thread until shutdown() is invoked
    /// (typically from a signal handler or a separate control thread).
    ///
    /// @throws std::runtime_error if connection setup fails.
    void run();

    /// Signal the engine to stop gracefully.
    ///
    /// 1. Sets the bot status to ShuttingDown.
    /// 2. Cancels the polling timer.
    /// 3. Cancels all outstanding offers (on-chain, secure).
    /// 4. Closes all RPC connections.
    /// 5. Shuts down the Prometheus exporter.
    /// 6. Sets the bot status to Stopped.
    ///
    /// Safe to call from any thread (posts work to the io_context).
    /// Idempotent: calling shutdown() more than once is a safe no-op.
    void shutdown();

    /// True while the engine is in the Running state.
    [[nodiscard]] bool is_running() const noexcept;

    // -- Accessors (for testing / diagnostics) --------------------------------

    /// Read-only reference to the shared global state.
    [[nodiscard]] const State& state() const noexcept;

    /// Read-only reference to the database.
    [[nodiscard]] const Database& database() const noexcept;

    /// The most recent block height processed by the engine.
    [[nodiscard]] BlockHeight last_processed_block() const noexcept;

    /// True if the engine was constructed in dry-run mode.
    [[nodiscard]] bool is_dry_run() const noexcept;

private:
    // -- Main loop -----------------------------------------------------------

    /// [T1-03] Launch the native coroutine polling loop via co_spawn.
    /// Replaces the old start_polling() + poll_block_height() pair that
    /// used co_spawn(use_future).get() -- a deadlock-prone pattern when
    /// ioc_.run() is on the same thread as .get().
    ///
    /// The coroutine sleeps for kPollInterval between polls and co_awaits
    /// all async operations directly, keeping the io_context free.
    void start_polling();

    /// [T1-03] Native coroutine polling loop.  Runs indefinitely on the
    /// io_context, yielding between polls via a steady_timer co_await.
    /// All RPC calls are co_awaited directly -- no use_future/.get().
    boost::asio::awaitable<void> poll_loop_coro();

    /// [T1-03] Execute the 13-step per-block heartbeat cycle as a
    /// coroutine.  Steps 2 and 8 contain async RPC calls that are
    /// co_awaited rather than blocking via use_future.
    ///
    /// The 13 steps (from Section 13 of the strategy document):
    ///
    ///  1. Update market state (prices from DEX + CEX)
    ///  2. Process any fills from this block
    ///  3. Update volatility, PIN, regime estimates
    ///  4. Compute optimal quotes (A-S / GLFT)
    ///  5. Apply spread optimizer adjustments
    ///  6. Apply risk limits (inventory, Kelly, no-loss)
    ///  7. Generate multi-tier offer ladder
    ///  8. Cancel stale offers, post new ones
    ///  9. Check arbitrage opportunities
    /// 10. Run hedging layer (compute skew, NHE)
    /// 11. Update PnL attribution
    /// 12. Export metrics to Prometheus
    /// 13. Check alert rules
    ///
    /// @param block_height  The new block height to process.
    boost::asio::awaitable<void> on_new_block_coro(BlockHeight block_height);

    // -- Per-step helpers (map 1:1 to the 13 steps) ---------------------------

    /// Step 1: Fetch latest prices from dexie and CEX feeds; update the
    /// MarketDataFeed and write MarketSnapshots into State.
    /// [T1-02] Coroutine: co_awaits dexie async methods (thread-pool dispatch).
    /// [T3-24] Sets market_data_valid per pair for dependency gating.
    boost::asio::awaitable<void> step_update_market_state(BlockHeight block_height);

    /// Step 2: Poll the wallet for settled offers, detect fills, record
    /// them in the Database and update positions in State.
    /// [T1-03] Coroutine: co_awaits offer_mgr_->detect_fills().
    boost::asio::awaitable<void> step_process_fills(BlockHeight block_height);

    /// Step 3: Feed the latest mid-price into the VolatilityEstimator and
    /// AdverseSelectionEstimator; update the regime classification.
    void step_update_analytics(BlockHeight block_height);

    /// Step 4: Invoke the active strategy (A-S or GLFT) to compute optimal
    /// bid/ask quotes for each enabled pair.
    void step_compute_quotes(BlockHeight block_height);

    /// Step 5: Pass the raw strategy quotes through the SpreadOptimizer to
    /// apply the four-component spread model and dynamic adjustments.
    void step_apply_spread_optimizer(BlockHeight block_height);

    /// Step 6: Apply pre-trade risk checks (never-sell-at-loss, inventory
    /// limits, Kelly sizing, CAT concentration cap).
    void step_apply_risk_limits(BlockHeight block_height);

    /// Step 7: Expand the risk-filtered quotes into a multi-tier offer
    /// ladder via the LiquidityEngine.
    void step_generate_ladder(BlockHeight block_height);

    /// Step 8: Cancel offers that have exceeded their TTL and post the new
    /// offer ladder via the OfferManager.
    /// [T1-03] Coroutine: co_awaits cancel_stale() and post_quotes().
    /// [T2-09] Captures actual wallet offer IDs and persists to DB.
    boost::asio::awaitable<void> step_manage_offers(BlockHeight block_height);

    /// Step 9: Scan for CEX-DEX, cross-DEX, triangular, cross-bridge, and
    /// crossed-book arbitrage opportunities.  Takes crossed-book offers
    /// when profitable (Dexie has no matching engine).
    /// [T9-01] Coroutine: co_awaits wallet take_offer for crossed books.
    boost::asio::awaitable<void> step_check_arbitrage(BlockHeight block_height);

    /// XCH Recovery Mode: check balance, manage mode transitions, and
    /// scan for cheap XCH asks to take when in recovery.
    boost::asio::awaitable<void> step_xch_recovery(BlockHeight block_height);

    /// Step 10: Compute inventory skew adjustments, NHE, portfolio-level
    /// netting, and statistical pairs hedging suggestions.
    void step_run_hedging(BlockHeight block_height);

    /// Step 11: Mark-to-market inventory, attribute spread/inventory/fee
    /// PnL, and persist a snapshot to the Database.
    void step_update_pnl(BlockHeight block_height);

    /// Step 12: Push all metric families to the Prometheus exporter.
    void step_export_metrics(BlockHeight block_height);

    /// Step 13: Evaluate the 14 alert rules and dispatch any triggered
    /// alerts to Telegram.
    void step_check_alerts(BlockHeight block_height);

    /// Coin pool maintenance: ensure enough pre-split XCH coins exist
    /// for concurrent multi-tier offer creation.  Called at startup and
    /// periodically (every coin_pool_interval_blocks).  Uses CoinManager
    /// ensure_split() to self-send if the free coin count is too low.
    boost::asio::awaitable<void> step_maintain_coin_pool(BlockHeight block_height);

    // -- Connection management -----------------------------------------------

    /// [CRITICAL-1] Open connections to the Chia full node, wallet, and dexie
    /// API.  Converted from void to awaitable<void> so it can be co_awaited
    /// from poll_loop_coro() while ioc_ is already running -- eliminates the
    /// deadlock caused by co_spawn(use_future).get() before ioc_.run().
    /// ISO/IEC 27001:2022: connection lifecycle is audit-logged.
    /// ISO/IEC 5055: no blocking .get() on the event loop thread.
    boost::asio::awaitable<void> open_connections();

    /// Close all RPC/API connections.
    void close_connections();

    // -- Configuration -------------------------------------------------------

    /// Immutable copy of the application configuration.
    AppConfig config_;

    /// Dry-run mode flag.
    bool dry_run_;

    /// True when running without the full node (wallet-only mode).
    /// Set during open_connections() based on config_.chia.mode and
    /// full-node reachability (auto-detect).
    bool wallet_only_mode_{false};

    // -- Pair config lookup ---------------------------------------------------
    // [M11] Declared after config_ so that C++ member initialization order
    // is correct (pair_config_map_ depends on config_.pairs pointers).
    // ISO/IEC JTC 1/SC 22: member init order matches declaration order.

    /// O(1) lookup map from pair name to the corresponding PairConfig entry
    /// in config_.pairs.  Built once during construction; eliminates the
    /// repeated O(N) linear scans that previously appeared in steps 2, 5,
    /// 6, 7, and 8.
    ///
    /// [T8-23] Pointer lifetime invariant: the raw pointers stored here
    /// point into `config_.pairs`, which is const after Engine construction
    /// and never reallocated.  Do NOT modify `config_.pairs` after the
    /// Engine constructor completes.
    ///
    /// ISO/IEC 5055: deterministic lookup, no raw pointer ownership.
    std::unordered_map<std::string, const PairConfig*> pair_config_map_;

    /// Return a pointer to the PairConfig for @p pair_name, or nullptr if
    /// no matching pair is configured.
    [[nodiscard]] const PairConfig* find_pair_config(
        const std::string& pair_name) const
    {
        auto it = pair_config_map_.find(pair_name);
        return (it != pair_config_map_.end()) ? it->second : nullptr;
    }

    // -- Boost.Asio event loop -----------------------------------------------

    /// The single io_context that drives the polling timer and all async
    /// operations.  Owned by the Engine so that its lifetime encompasses
    /// all subsystem lifetimes.
    boost::asio::io_context ioc_;

    /// 5-second steady timer for block-height polling.
    boost::asio::steady_timer poll_timer_;

    /// Polling interval (configurable, default 5 seconds).
    static constexpr std::chrono::seconds kPollInterval{5};

    // -- Shared state --------------------------------------------------------

    /// Global mutable state shared across subsystems.
    std::shared_ptr<State> state_;

    /// SQLite persistence layer.
    std::unique_ptr<Database> db_;

    // -- RPC / API clients ---------------------------------------------------

    /// Chia full node RPC (port 8555).
    std::shared_ptr<rpc::ChiaFullNodeRPC> full_node_;

    /// Chia wallet RPC (port 9256).
    std::shared_ptr<rpc::ChiaWalletRPC> wallet_;

    /// Dexie aggregator REST client (shared with OfferManager for submission).
    std::shared_ptr<rpc::DexieClient> dexie_;

    /// CoinGecko external price reference client.
    std::shared_ptr<rpc::CoinGeckoClient> coingecko_;

    /// Cached CoinGecko prices (coin_id -> USD price).
    /// Updated once per polling_interval_ms in step_update_market_state.
    std::map<std::string, double> coingecko_prices_;

    /// Timestamp of the last successful CoinGecko fetch.
    std::chrono::steady_clock::time_point coingecko_last_fetch_;

    // -- Execution layer -----------------------------------------------------

    /// Coin-set (UTXO) manager for pre-splitting and locking.
    std::unique_ptr<execution::CoinManager> coin_mgr_;

    /// Last block at which coin pool maintenance ran.
    BlockHeight coin_pool_last_block_{0};

    /// True while a split transaction is pending confirmation.
    /// Prevents overlapping splits that would fail or waste fees.
    bool coin_pool_split_pending_{false};

    /// Offer lifecycle manager (create, monitor, cancel).
    std::unique_ptr<execution::OfferManager> offer_mgr_;

    /// Multi-source market data aggregation.
    std::unique_ptr<MarketDataFeed> market_data_;

    // -- Data / analytics ----------------------------------------------------

    /// Yang-Zhang hybrid volatility estimator per pair.
    std::unordered_map<std::string, std::unique_ptr<VolatilityEstimator>> vol_estimators_;

    /// Bayesian PIN estimator (adverse selection) per pair.
    std::unordered_map<std::string, std::unique_ptr<AdverseSelectionEstimator>> pin_estimators_;

    // -- Strategy layer ------------------------------------------------------

    /// [T1-11] Per-pair strategy instances to prevent state bleed.
    /// Each enabled pair gets its own StrategyBase (A-S or GLFT) so that
    /// price histories, regime states, and internal estimators are isolated.
    /// Follows the same pattern as vol_estimators_ and pin_estimators_.
    /// ISO/IEC 5055: no shared mutable state across independent pairs.
    std::unordered_map<std::string, std::unique_ptr<StrategyBase>> strategies_;

    /// Four-component spread optimizer.
    std::unique_ptr<SpreadOptimizer> spread_opt_;

    /// Per-pair multi-tier liquidity engine.
    std::unordered_map<std::string, std::unique_ptr<LiquidityEngine>> liquidity_engines_;

    /// Stablecoin depeg detector -- monitors pegged pairs for failure.
    std::unique_ptr<DepegDetector> depeg_detector_;

    /// Arbitrage detector -- scans for CEX-DEX, cross-DEX, triangular, and
    /// cross-bridge opportunities each block.
    std::unique_ptr<ArbitrageDetector> arb_detector_;

    /// Fee budget tracker -- dynamic fee selection, fee-vs-gain gating,
    /// daily budget enforcement.
    std::unique_ptr<FeeTracker> fee_tracker_;

    /// [T4-16] Online κ calibrator -- rolling fill-rate estimator that
    /// fits λ(δ) = A·exp(−κ·δ) from observed fill data.
    std::unique_ptr<KappaCalibrator> kappa_calibrator_;

    // -- Risk layer ----------------------------------------------------------

    /// Inventory tracking, cost basis, Kelly sizing, capital allocation.
    std::unique_ptr<InventoryTracker> inventory_;

    /// Pre-trade risk checks (no-loss, limits, flash crash).
    std::unique_ptr<PreTradeCheck> pre_trade_;

    /// Hedging framework (layers 1-4).
    std::unique_ptr<HedgingManager> hedging_;

    // -- Monitoring layer ----------------------------------------------------

    /// PnL attribution engine and trade logging.
    std::unique_ptr<PnLTracker> pnl_;

    /// Prometheus HTTP metrics exporter.
    std::unique_ptr<MetricsExporter> metrics_;

    /// Telegram alert manager.
    std::unique_ptr<AlertManager> alerts_;

    /// On-chain reconciler -- verifies internal state against full node.
    std::unique_ptr<OnChainReconciler> on_chain_reconciler_;

    // -- New strategy modules ------------------------------------------------

    /// Order book interaction tactician — gap-filling, join/improve/step-back.
    std::unique_ptr<OrderBookTactician> order_book_tactician_;

    /// Strategy portfolio — Brock-Hommes dynamic blending of strategy components.
    std::unique_ptr<StrategyPortfolio> strategy_portfolio_;

    /// CHIA structural edge optimizer — 5-factor composite multiplier.
    std::unique_ptr<ChiaEdgeOptimizer> chia_edge_;

    /// Coin age weighted quoting — age-based spread adjustment.
    std::unique_ptr<CoinAgeWeightedQuoting> coin_age_quoting_;

    /// Block cadence adaptive spread — block arrival timing.
    std::unique_ptr<BlockCadenceAdaptiveSpread> block_cadence_;

    /// Mempool sentinel — mempool-aware spread/skew.
    std::unique_ptr<MempoolSentinelStrategy> mempool_sentinel_;

    /// Dynamic market allocator — scores pairs and shifts capital.
    std::unique_ptr<MarketAllocator> market_allocator_;

    // -- New risk modules ----------------------------------------------------

    /// Strategic loss manager — 5-scenario EV analysis for rebalancing.
    std::unique_ptr<StrategicLossManager> loss_manager_;

    /// Inventory drift analyzer — random walk, trending, Monte Carlo.
    std::unique_ptr<InventoryDriftAnalyzer> drift_analyzer_;

    // -- Startup market analysis ---------------------------------------------

    /// Startup market analyzer.  Collects market observations for
    /// config_.strategy.startup_analysis_blocks blocks before entering
    /// active trading.  nullptr when startup_analysis_blocks == 0.
    std::unique_ptr<MarketAnalyzer> market_analyzer_;

    /// Spread multiplier derived from startup analysis.  Applied in
    /// step_apply_spread_optimizer() to widen/tighten initial spreads
    /// based on the analysis recommendation.
    ///   Conservative → 1.5  (50% wider)
    ///   Normal       → 1.0  (no change)
    ///   Aggressive   → 0.8  (20% tighter)
    /// Default 1.0 (no adjustment) if analysis is skipped.
    double analysis_spread_mult_{1.0};

    /// [T0] Coroutine: run the startup analysis phase.  Collects
    /// startup_analysis_blocks blocks of market data, exports per-block
    /// metrics, and logs the completed analysis summary.  Called from
    /// poll_loop_coro() before the main trading loop begins.
    boost::asio::awaitable<void> run_startup_analysis();

    // -- Runtime state -------------------------------------------------------

    /// The last block height successfully processed by on_new_block().
    std::atomic<BlockHeight> last_block_{0};

    /// Stop flag checked by the polling loop.
    std::atomic<bool> stop_requested_{false};

    /// [HIGH-2] Shutdown completion flag.  Set to true after cancel_all()
    /// completes (or times out) during shutdown().  Replaces the deadlock-
    /// prone promise/future pattern with a non-blocking atomic + timer poll.
    /// ISO/IEC 5055: no blocking .get()/.wait() on the io_context thread.
    std::atomic<bool> shutdown_cancel_done_{false};

    // -- Wallet circuit breaker ----------------------------------------------
    // After consecutive wallet RPC failures, skip wallet-dependent heartbeat
    // steps (2 and 8) and poll for wallet recovery instead.  This prevents
    // timeout cascades from stalling the entire heartbeat loop when the
    // wallet daemon is unreachable.

    /// Number of consecutive wallet RPC failures.
    std::uint32_t wallet_consecutive_failures_{0};

    /// Threshold: after this many consecutive failures, wallet-dependent
    /// heartbeat steps are skipped until the wallet recovers.
    static constexpr std::uint32_t kWalletCircuitBreakerThreshold{3};

    /// True when the circuit breaker has tripped (wallet assumed unreachable).
    bool wallet_circuit_open_{false};

    /// Timestamp of the last wallet recovery probe (to throttle retries).
    std::chrono::steady_clock::time_point wallet_last_probe_{};

    /// Minimum interval between wallet recovery probes when circuit is open.
    static constexpr std::chrono::seconds kWalletProbeInterval{30};

    /// Per-pair working storage for the current cycle's quotes.
    /// Populated by step_compute_quotes, consumed through steps 5-8.
    // [M10] Value-initialize all aggregate members to prevent
    // undefined reads on first access within a cycle.
    // ISO/IEC 5055: deterministic initial state for all fields.
    struct PairCycleState {
        std::string   pair_name;            ///< Pair being processed.
        QuoteResult   raw_quote{};          ///< Output of strategy.
        SpreadResult  spread_result{};      ///< Output of spread optimizer.
        Quote         risk_quote{};         ///< After risk filter.
        bool          quote_valid{false};   ///< False if risk killed both sides.
        std::vector<TierQuote> ladder;      ///< Multi-tier expansion.

        // [T3-24] Dependency-aware gating: set to true only when Step 1
        // successfully fetches fresh market data for this pair.  Steps 4-8
        // are gated on this flag so that stale/missing data cannot propagate
        // into quoting and offer management.
        // ISO/IEC 5055: prevents acting on invalid upstream data.
        bool          market_data_valid{false};
    };

    /// Per-pair cycle state for the current block.
    std::unordered_map<std::string, PairCycleState> cycle_;

    // [H6] PnL high-water mark for drawdown detection in step 13 alerts.
    // Monotonically non-decreasing; updated each cycle in step_check_alerts.
    // ISO/IEC 5055: prevents false drawdown resets on PnL oscillation.
    Mojo peak_pnl_hwm_{0};

    // [MEDIUM-7] Tracks whether peak_pnl_hwm_ has been seeded with the
    // first-cycle total_pnl.  Without this, the drawdown circuit breaker
    // is bypassed entirely until the first profitable cycle -- leaving the
    // engine unprotected against losses from startup.
    // ISO/IEC 27001:2022: ensures continuous risk monitoring from first tick.
    bool hwm_initialized_{false};

    // [T8-03] Drawdown grace period: skip the HWM drawdown circuit breaker
    // for the first N blocks after engine start so that a small initial
    // loss from the zero-peak case does not immediately pause the engine.
    uint32_t drawdown_grace_remaining_{0};

    // [T3-10] Flash-crash circuit breaker state machine.
    // Transitions: Normal -> Crash -> Recovery -> Normal.
    // During Crash and Recovery states, Step 8 (offer posting) is gated.
    // ISO/IEC 5055: deterministic initial state; exhaustive enum.
    FlashCrashState flash_crash_state_{FlashCrashState::Normal};

    // XCH Recovery Mode -- entered when XCH spendable drops below threshold.
    // While active: offers cancelled, Steps 7-8 skipped, engine scans for
    // cheap XCH asks to take.  Exits when XCH > recovery_target.
    bool xch_recovery_mode_{false};

    /// True if offers were already cancelled on recovery mode entry.
    bool xch_recovery_cancelled_{false};

    // [v0.7.38] Cached XCH confirmed balance (mojos), queried once per
    // heartbeat before Step 7.  Used by step_generate_ladder to hard-cap
    // avail_inventory against actual wallet balance.
    Mojo xch_confirmed_balance_{0};

    // UTXO liberation cooldown -- after liberation cancels offers or detects
    // no offers to cancel, suppress the pair loop for this many heartbeats
    // to let cancel transactions confirm on-chain.  Prevents the engine from
    // posting offers that would immediately get liberated next heartbeat.
    // Resets early if spendable recovers above 2× reserve.
    int liberation_cooldown_{0};

    // [T3-09] Max-drawdown global circuit breaker threshold.
    // Drawdown fraction = (peak_pnl_hwm_ - total_pnl) / abs(peak_pnl_hwm_).
    // When exceeded, engine transitions to BotStatus::Paused and alerts.
    // Configurable via risk.max_drawdown_pct in config.yaml; default 10%.
    // ISO/IEC 5055: named constant with documented default.
    double max_drawdown_pct_;

    // [T3-36] Rolling time-window PnL loss circuit breaker.
    //
    // Records (block_height, total_pnl_mojos) pairs each heartbeat cycle.
    // The deque is trimmed to retain only entries within the most recent
    // loss_window_blocks blocks; stale entries (age >= window) are discarded
    // from the front.  The oldest surviving entry provides the baseline PnL
    // for the window loss calculation.
    //
    // Loss in window = oldest_pnl - current_pnl  (positive when losing).
    // Threshold      = peak_pnl_hwm_ * max_window_loss_bps / 10000.
    //
    // When loss_in_window > threshold AND threshold > 0, the engine
    // transitions to BotStatus::Paused the same way the HWM circuit breaker
    // does.  The two circuit breakers are independent; either can fire first.
    //
    // ISO/IEC 27001:2022: continuous monitoring within a bounded time window.
    // ISO/IEC 5055: deque prevents unbounded memory growth.
    std::deque<std::pair<BlockHeight, Mojo>> pnl_window_;

    // [T3-08] NHE (Natural Hedge Efficiency) accumulators for step 10.
    // These running totals track net inventory change and total traded
    // volume across all pairs for the NHE computation.
    // Reset each cycle; fed from step 2 fill data.
    // ISO/IEC 5055: deterministic zero-initialization.
    double nhe_net_inventory_change_{0.0};
    double nhe_total_volume_{0.0};

    // [T5-CR1] VPIN validation gate (Andersen & Bondarenko 2014).
    // Runtime tracker that measures whether VPIN activations (vpin_mult > 1.0)
    // actually predict adverse fills within a sliding block window.  If the
    // precision drops below vpin_min_precision_ after a burn-in period, the
    // engine warns the operator that VPIN may lack incremental predictive
    // power beyond raw volume and volatility.
    // ISO/IEC 27001:2022: operational monitoring of signal quality.
    // ISO/IEC 5055: deterministic zero-initialization; named constants.

    /// Rolling-window counters for VPIN signal quality.  The window covers
    /// the last kVpinRollingWindow activations so that precision reflects
    /// recent signal quality and can recover from early false-positive bursts.
    /// ISO/IEC 25000: operational quality metric with bounded memory.
    static constexpr uint32_t kVpinRollingWindow = 200;

    /// Total activations (lifetime, for burn-in gating only).
    uint32_t vpin_activations_{0};

    /// Rolling true-positive count (reset when window rolls over).
    uint32_t vpin_rolling_tp_{0};

    /// Rolling total resolved within the window (TP + FP).
    uint32_t vpin_rolling_resolved_{0};

    /// Activations whose validation window expired with no adverse fill.
    uint32_t vpin_false_positives_{0};

    /// How many blocks after a VPIN activation to wait for an adverse fill
    /// before classifying the activation as a false positive.
    static constexpr uint32_t kVpinValidationWindow = 10;

    /// Minimum acceptable precision (TP / resolved) before warning.
    /// Default 0.3 (30%).  Below this after kVpinBurnIn activations, a
    /// warning is emitted suggesting VPIN weight reduction.
    static constexpr double kDefaultVpinMinPrecision = 0.30;
    double vpin_min_precision_{kDefaultVpinMinPrecision};

    /// Burn-in count: precision warnings are suppressed until this many
    /// activations have been recorded (avoids noisy early signals).
    static constexpr uint32_t kVpinBurnIn = 100;

    /// Maximum pending activations awaiting validation.  Prevents unbounded
    /// growth if block_height stalls (node sync failure).
    /// ISO/IEC 5055: bounded container under all reachable states.
    static constexpr size_t kMaxPendingActivations = 512;

    /// Ring buffer of block heights at which VPIN activated.
    /// Entries are removed once validated (TP) or expired (FP).
    /// ISO/IEC 5055: bounded by kMaxPendingActivations.
    std::vector<BlockHeight> vpin_activation_blocks_;

    // -- [T4-02] Reorg protection: pending-fill confirmation buffer --------
    // Fills detected by detect_fills() are first inserted here keyed by
    // their confirmed block height.  Each heartbeat cycle, fills whose
    // age (current_block - fill_block) >= confirmation_depth_blocks are
    // promoted to actual fill processing (inventory, PnL, DB, etc.).
    // Fills are evicted after processing or if the wallet reports them as
    // no longer confirmed (reorg rollback).
    // ISO/IEC 27001:2022: prevents accepting fills that a chain reorg may
    // reverse, protecting cost-basis integrity.
    std::vector<Fill> pending_unconfirmed_fills_;

    // -- [T4-11] Offer reconciliation tracking -----------------------------
    // Block height of the last full reconciliation run.  Compared against
    // current_block to decide when to trigger the next reconciliation.
    BlockHeight last_reconciliation_block_{0};

    // -- Stuck transaction pruning cooldown ----------------------------------
    // Block height of the last stuck-transaction prune attempt.  Limits
    // pruning to once every kStuckTxPruneInterval blocks to avoid
    // hammering the wallet RPC every heartbeat.
    BlockHeight last_stuck_tx_prune_block_{0};
    static constexpr uint32_t kStuckTxPruneInterval{20};  // ~17 min

    // Number of consecutive heartbeat blocks where pending_change > 0 was
    // seen for any wallet.  After kForceDeletePendingBlocks consecutive
    // blocks, unconditionally call delete_unconfirmed_transactions rather
    // than relying on the transaction-scanning heuristic.
    uint32_t consecutive_pending_blocks_{0};
    static constexpr uint32_t kForceDeletePendingBlocks{12};  // ~10 min

    // -- Wallet unsync auto-recovery -------------------------------------
    // When the Chia wallet stays unsynced for kWalletRestartThreshold
    // consecutive blocks, the engine restarts the wallet service to
    // force a clean resync from the trusted full node.  This breaks the
    // deadlock where pending_change prevents sync and the sync gate
    // prevents the force-delete from ever firing.
    uint32_t consecutive_unsynced_blocks_{0};
    static constexpr uint32_t kWalletRestartThreshold{20};  // ~3 min

    // -- [T4-04] Cached wallet balances for spendable-reserve gating ------
    // Populated from wallet RPC each heartbeat; keyed by wallet label.
    struct WalletBalanceEntry {
        Mojo spendable{0};
        Mojo confirmed{0};
        Mojo pending_change{0};
    };
    std::unordered_map<std::string, WalletBalanceEntry> cached_wallet_balances_;

    // -- [T4-05] GUI-requested pause via signal file ----------------------
    // The GUI creates / removes a "pause.flag" file next to the database.
    // The engine checks once per heartbeat and transitions between
    // BotStatus::Running and BotStatus::Paused accordingly.
    // Steps 1-6 & 9-13 continue; only Step 8 (offer posting) is skipped.
    std::filesystem::path pause_flag_path_;    ///< Resolved once in constructor.
    bool gui_pause_active_{false};             ///< Cached last-seen state.

    /// Check for the pause flag file and update BotStatus accordingly.
    void check_pause_flag();
};

}  // namespace xop

#endif  // XOP_ENGINE_HPP
