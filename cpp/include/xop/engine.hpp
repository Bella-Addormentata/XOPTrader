// engine.hpp -- Top-level orchestrator for XOPTrader CHIA DEX market-maker.
//
// The Engine class owns every subsystem and drives the per-block heartbeat
// loop described in Section 13 of CHIA_MARKET_MAKER_STRATEGY.md.
//
// Architecture:
//   A boost::asio::io_context drives a 5-second polling timer that queries
//   the Chia full node for the current block height.  When a new block is
//   detected, the engine executes the 13-step main cycle synchronously
//   (single-threaded coroutine model).
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

// Risk layer
#include "xop/risk/inventory.hpp"
#include "xop/risk/limits.hpp"
#include "xop/risk/hedging.hpp"

// Monitoring layer
#include "xop/monitoring/pnl.hpp"
#include "xop/monitoring/metrics.hpp"
#include "xop/monitoring/alerts.hpp"

// New strategy modules
#include "xop/strategy/order_book_tactics.hpp"
#include "xop/strategy/strategy_portfolio.hpp"
#include "xop/strategy/chia_edge.hpp"
#include "xop/strategy/new_strategies.hpp"

// New risk modules
#include "xop/risk/loss_manager.hpp"
#include "xop/risk/drift_analyzer.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace xop {

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

    /// Start the 5-second polling timer.  Each tick queries the full node
    /// for the current block height and, if a new block is detected,
    /// invokes on_new_block().
    void start_polling();

    /// Handler invoked by the polling timer.  Queries the full node,
    /// compares against last_block_, and dispatches on_new_block() if a
    /// new block has appeared.
    void poll_block_height();

    /// Execute the 13-step per-block heartbeat cycle.
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
    void on_new_block(BlockHeight block_height);

    // -- Per-step helpers (map 1:1 to the 13 steps) ---------------------------

    /// Step 1: Fetch latest prices from dexie and CEX feeds; update the
    /// MarketDataFeed and write MarketSnapshots into State.
    void step_update_market_state(BlockHeight block_height);

    /// Step 2: Poll the wallet for settled offers, detect fills, record
    /// them in the Database and update positions in State.
    void step_process_fills(BlockHeight block_height);

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
    void step_manage_offers(BlockHeight block_height);

    /// Step 9: Scan for CEX-DEX, cross-DEX, triangular, and cross-bridge
    /// arbitrage opportunities.
    void step_check_arbitrage(BlockHeight block_height);

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

    // -- Connection management -----------------------------------------------

    /// Open connections to the Chia full node, wallet, and dexie API.
    void open_connections();

    /// Close all RPC/API connections.
    void close_connections();

    // -- Configuration -------------------------------------------------------

    /// Immutable copy of the application configuration.
    AppConfig config_;

    /// Dry-run mode flag.
    bool dry_run_;

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

    /// Dexie aggregator REST client.
    std::unique_ptr<rpc::DexieClient> dexie_;

    // -- Execution layer -----------------------------------------------------

    /// Coin-set (UTXO) manager for pre-splitting and locking.
    std::unique_ptr<execution::CoinManager> coin_mgr_;

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

    /// Active market-making strategy (Avellaneda-Stoikov or GLFT).
    /// Pluggable via StrategyBase interface.
    std::unique_ptr<StrategyBase> strategy_;

    /// Four-component spread optimizer.
    std::unique_ptr<SpreadOptimizer> spread_opt_;

    /// Per-pair multi-tier liquidity engine.
    std::unordered_map<std::string, std::unique_ptr<LiquidityEngine>> liquidity_engines_;

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

    // -- New risk modules ----------------------------------------------------

    /// Strategic loss manager — 5-scenario EV analysis for rebalancing.
    std::unique_ptr<StrategicLossManager> loss_manager_;

    /// Inventory drift analyzer — random walk, trending, Monte Carlo.
    std::unique_ptr<InventoryDriftAnalyzer> drift_analyzer_;

    // -- Runtime state -------------------------------------------------------

    /// The last block height successfully processed by on_new_block().
    std::atomic<BlockHeight> last_block_{0};

    /// Stop flag checked by the polling loop.
    std::atomic<bool> stop_requested_{false};

    /// Per-pair working storage for the current cycle's quotes.
    /// Populated by step_compute_quotes, consumed through steps 5-8.
    struct PairCycleState {
        std::string   pair_name;       ///< Pair being processed.
        QuoteResult   raw_quote;       ///< Output of strategy.
        SpreadResult  spread_result;   ///< Output of spread optimizer.
        Quote         risk_quote;      ///< After risk filter.
        bool          quote_valid{false}; ///< False if risk killed both sides.
        std::vector<TierQuote> ladder; ///< Multi-tier expansion.
    };

    /// Per-pair cycle state for the current block.
    std::unordered_map<std::string, PairCycleState> cycle_;
};

}  // namespace xop

#endif  // XOP_ENGINE_HPP
