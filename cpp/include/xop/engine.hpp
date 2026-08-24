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
#include "xop/rpc/tibetswap_client.hpp"

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
#include "xop/risk/drawdown_breaker.hpp"
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
#include "xop/strategy/competitiveness_pid.hpp"

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
#include <set>
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

    /// [AS-WARM] Warm-start every pair's VolatilityEstimator from the
    /// persisted snapshots table at startup (mirrors
    /// PnLTracker::rehydrate_from_db for P&L), so the estimator is ready on
    /// the FIRST tick after a restart instead of after ~32 h of
    /// uninterrupted uptime.  Pairs with sparse history stay cold and keep
    /// the existing sigma-floor behaviour.
    void warm_start_volatility_estimators();

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

    /// Derive an INDEPENDENT, TRIANGULATED fair value for every enabled pair
    /// and push it into MarketDataFeed.  Called once per heartbeat at the end
    /// of Step 1, after the dexie and AMM ingests, so the solve sees this
    /// heartbeat's observations.
    ///
    /// Assets are nodes and pairs are edges; the external USD feed anchors
    /// whichever assets it lists.  Each pair is then priced by a weighted
    /// least-squares solve (xop::fv::solve_pair) run with THAT PAIR'S OWN BOOK
    /// EDGE DELETED, which is what makes the result independent of the book it
    /// validates.  Edge weights are 1/sigma^2 from observable quality only --
    /// book width, heartbeats since the mid last moved, resting depth -- so a
    /// frozen or very wide book contributes almost nothing.  No pair or asset
    /// is special-cased anywhere in the implementation.
    ///
    /// Publishes, per pair: the price, the solve's own 1-sigma, the
    /// CONSISTENCY RESIDUAL between that pair's book and the rest of the
    /// graph, and a confidence tier.  When no estimate survives the weights
    /// the tier is Unavailable and Step 7 widens instead of clamping; it never
    /// falls back to the book being validated.
    void update_fair_values();

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

    /// Step 9d helper: opportunistically recycle near-mid asks into maker
    /// inventory when they are discounted enough to cover round-trip costs.
    boost::asio::awaitable<void> step_run_midpoint_recycling(
        BlockHeight block_height);

    /// XCH Recovery Mode: check balance, manage mode transitions, and
    /// scan for cheap XCH asks to take when in recovery.
    boost::asio::awaitable<void> step_xch_recovery(BlockHeight block_height);

    /// Step 9e: Buyer -- dedicated opportunistic offer-taker flow.
    /// Scans Dexie order books for offers meeting profitability criteria
    /// defined in a separate buyer.yaml config.  Aware of maker settings
    /// (inventory limits, recovery mode, wallet health, fee budgets).
    boost::asio::awaitable<void> step_run_buyer(BlockHeight block_height);

    /// Step 9f: Drift corrector -- active asset rebalancer.  When an asset's
    /// portfolio share is outside target +/- trigger_factor*tolerance, scan
    /// Dexie for competitively-priced offers that, if taken, push the
    /// portfolio back toward target allocations.  Hysteresis: stop taking
    /// once back inside target +/- exit_factor*tolerance.
    boost::asio::awaitable<void> step_run_drift_corrector(BlockHeight block_height);

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

    // -- USD normalization helpers (PNL-BASIS-USD 2026-07-30) ----------------
    //
    // The InventoryTracker keeps ONE cost-basis record per asset, but the
    // engine's pseudo-prices are denominated per-pair in that pair's QUOTE
    // currency.  XCH trades against wUSDC.b (~1.4e12), BYC (~1.2e12) and DBX
    // (~1.4e14) simultaneously, so feeding raw pair prices into one shared
    // record blended incompatible currencies.  All basis values are now
    // stored in USD-normalized pseudo-units (USD-per-base-unit * 1e12):
    // fills convert pair price -> USD on the way in (to_usd_pseudo) and
    // basis -> pair price on the way out (from_usd_pseudo).  Mirrors the
    // GUI's per-quote conversion in database_service.py::pnl_usdc_expr.

    /// Live USD value of 1 XCH, derived from an enabled XCH/<usd-stable>
    /// pair's mid price.  Returns 0 ("unknown") when no such market snapshot
    /// is available yet -- deliberately, not a fallback rate: see the
    /// [PNL-BASIS-USD] note in usd_per_xch for why a fixed rate is unsafe
    /// now that cost basis is persisted.
    [[nodiscard]] double usd_per_xch() const;

    /// USD value of one QUOTE display unit for the pair.  1.0 for
    /// USD-pegged stables (wUSDC/wUSDC.b/USDS) and BYC; cross-derived for
    /// DBX (usd_per_xch / dbx_per_xch); usd_per_xch for XCH-quoted pairs.
    /// Returns 0.0 when unknown (pair excluded from USD accounting).
    [[nodiscard]] double quote_usd_factor(const PairConfig& pc) const;

    /// [S20 2026-08-24] True when quote_usd_factor() answers this pair
    /// from a par constant (fiat-collateralised wrapper, or BYC falling
    /// back to $1) rather than from a live mid.  Callers that must not
    /// value inventory on an ungraded market observation use this to tell
    /// the two cases apart -- a par constant needs no provenance.
    [[nodiscard]] bool quote_usd_factor_is_par(const PairConfig& pc) const;

    /// [S20] Name of the pair whose published snapshot quote_usd_factor()
    /// reads to answer `pc` -- usually `pc` itself, but a BYC quote is
    /// answered from the separate BYC/<stable> cross.  Empty when the
    /// factor is a par constant or no source pair is enabled.  Callers
    /// checking valuation grade must test THIS snapshot, not `pc`'s.
    [[nodiscard]] std::string quote_usd_factor_source_pair(
        const PairConfig& pc) const;

    /// Convert a pair-quote pseudo-price to a USD-normalized pseudo-price.
    /// Returns 0 when the quote's USD value is unknown.
    [[nodiscard]] Mojo to_usd_pseudo(Mojo pair_price,
                                     const PairConfig& pc) const;

    /// Convert a USD-normalized pseudo-price back into the pair's quote
    /// pseudo-price.  Returns 0 when the quote's USD value is unknown.
    [[nodiscard]] Mojo from_usd_pseudo(Mojo usd_price,
                                       const PairConfig& pc) const;

    /// USD-normalized pseudo-price for one display unit of an asset,
    /// resolved from any enabled pair that trades it (base: mid * factor;
    /// quote: factor * kMojosPerXch).  0 when no market data yet.
    /// [S20] The base-of-pair branch only accepts valuation-grade mids.
    [[nodiscard]] Mojo asset_usd_pseudo_price(const AssetId& asset_id) const;

    /// [S20 2026-08-24] Median implied price of `pc` triangulated through
    /// every healthy pair of enabled sibling books (see the definition for
    /// leg-health rules).  0 when no healthy triangle exists.  Feeds
    /// MarketDataFeed::ingest_reference_anchor each heartbeat.
    [[nodiscard]] double compute_implied_cross_anchor(const PairConfig& pc) const;

    /// [DRAWDOWN-EQUITY 2026-08-04] Total portfolio equity in USD: the sum
    /// over all tracked inventory records of holdings x USD price, using
    /// the same valuation machinery as the accounting paths
    /// (asset_usd_pseudo_price / quote_usd_factor / usd_per_xch).  When an
    /// asset has no live conversion this cycle it is carried at its LAST
    /// KNOWN price (last_asset_usd_price_) instead of being dropped, so a
    /// data gap cannot masquerade as a crash.  Non-const: refreshes that
    /// cache.  This is the denominator of the max-drawdown breaker and the
    /// anchor of the rolling-window loss threshold.
    /// [S20] Pass the heartbeat's block height to arm the carry-TTL
    /// degradation verdict (valuation_degraded_); 0 skips it, for callers
    /// outside the heartbeat that only want the number.
    [[nodiscard]] double compute_portfolio_equity_usd(BlockHeight current_block = 0);

    /// Snapshot all InventoryTracker records into the inventory_state table
    /// (PNL-BASIS-PERSIST).  Called after every mutation; never throws.
    void persist_inventory_state() noexcept;

    // -- Double-entry accounting (LEDGER 2026-07-30) ------------------------

    /// Establish opening balances once, from the wallet's confirmed balances.
    /// The ledger deliberately does NOT replay trade_log: that table was shown
    /// to disagree with the wallet by ~665 XCH, so replaying it would import
    /// the very corruption the ledger exists to detect.
    /// @param at_block  Chain height when the balances were observed.  Fills
    ///                  that settled at or below it are ALREADY inside the
    ///                  opening balance (offers restored from a prior run can
    ///                  settle during downtime and are detected afterwards),
    ///                  so their legs must be suppressed or they double-count.
    void post_ledger_genesis(
        const std::unordered_map<AssetId, Mojo>& balances,
        BlockHeight at_block,
        const std::unordered_map<AssetId, std::string>& observed_at);

    /// Post the balanced legs of a settled fill (base, quote and fee).
    /// Idempotent on the fill's trade id.
    void post_ledger_fill(const Fill& fill, const PairConfig& pc,
                          Mojo quote_mojos);

    /// Record a TAKER trade -- one where we crossed the spread and lifted
    /// someone else's offer.  Writes both the ledger legs (so the books
    /// balance) and a taker_fills row (so the trade is attributable to the
    /// strategy that placed it).
    ///
    /// Every take_offer() success site must call this.  Before 2026-07-30
    /// none of them did: 70 taker trades across the retained logs wrote
    /// nothing to trade_log, offer_log or the ledger, which is why the
    /// strategies placing them had no measurable P&L.
    ///
    /// @param we_bought_base true when we acquired the base asset and paid
    ///                       quote; false for the reverse.
    /// @param base_mojos     Absolute size of the base leg.
    /// @param price_mojos    Execution price in engine pseudo-units.
    void record_taker_fill(const std::string& strategy,
                           const std::string& trade_id,
                           const std::string& counterparty_offer_id,
                           const PairConfig& pc,
                           bool we_bought_base,
                           Mojo base_mojos,
                           Mojo price_mojos,
                           std::uint64_t fee_mojos,
                           BlockHeight block_height) noexcept;

    /// Notional of asset @p asset_id currently committed to live offers.
    /// This bounds how much the wallet can legitimately move before the
    /// ledger sees it, and collapses to 0 when the book is empty -- which is
    /// what lets the control tighten to near-exact between quoting cycles.
    [[nodiscard]] Mojo live_offer_exposure(const AssetId& asset_id) const;

    /// [REWARD-INCOME 2026-08-01] Detect dexie DBX liquidity-reward inflows
    /// on the reward asset's wallet (daily bursts of micro incoming
    /// transactions -- detection evidence in accounting/reward_ingest.hpp)
    /// and book each one: 'reward' ledger entry at CoinGecko fair value,
    /// quantity folded into the cost basis at that FMV, USD accumulated as
    /// reward income separate from trading P&L.  MUST run before
    /// step_check_ledger_invariant in the same heartbeat so a rewarded
    /// inflow is explained flow by the time the books are tied to the
    /// wallet, not "unexplained divergence" for the adjusting entries to
    /// absorb.  Idempotent per wallet transaction (ledger event_id
    /// uniqueness), so re-scans and restarts never double-book.
    asio::awaitable<void> step_ingest_reward_inflows(
        BlockHeight block_height);

    /// [S19 2026-08-23] Book completed warp bridge flows as first-class
    /// ledger events (bridge_deposit / bridge_withdrawal) so external
    /// capital is explained flow by the time the books are tied to the
    /// wallet, not "unexplained divergence" for the adjusting entries to
    /// absorb.  Reads the GUI-owned warp_jobs.db READ-ONLY; idempotent
    /// per job (ledger event_id uniqueness), so re-scans and restarts
    /// never double-book.  GIPS: the USD accumulates as net deposits
    /// outside trading P&L; booking an in-process flow re-anchors the
    /// drawdown peak from the next equity valuation (restart
    /// semantics -- in-place peak adjustment was retired after review
    /// rounds 24-41; see bridge_ingest.hpp).
    asio::awaitable<void> step_ingest_bridge_flows(
        BlockHeight block_height);

    /// Tie the ledger's implied balances to the wallet's confirmed balances
    /// and escalate on sustained, unexplained divergence.  Alert-only unless
    /// accounting.pause_enabled is set.
    void step_check_ledger_invariant(BlockHeight block_height);

    /// Alert when a quote stablecoin leaves its peg.  Accounting keeps
    /// valuing wUSDC.b/wUSDC/USDS at $1.00 either way -- this makes the
    /// exposure visible rather than silently priced in.
    void step_check_stablecoin_peg(BlockHeight block_height);

    /// Emit a trade decision-tree metric when the Prometheus exporter exists.
    void record_trade_decision_metric(const char* strategy,
                                      const char* scenario_id,
                                      const char* result);

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

    /// Wall-clock instant of that same successful fetch, passed to
    /// MarketDataFeed::ingest_cex_reference so the CEX freshness gates
    /// measure the age of the DATA rather than the age of the re-ingest.
    /// Steady-clock cannot serve: Timestamp is a system_clock time_point.
    /// Default-constructed until the first success.  Note that passing that
    /// value would NOT read as "never observed": ingest_cex_reference maps
    /// Timestamp{} to now(), matching ingest_amm_mid.  It cannot be passed,
    /// because the only call site sits behind `if (!coingecko_prices_.empty())`
    /// and that cache is populated only by a successful fetch -- so the first
    /// ingest always carries a real observation time.
    std::chrono::system_clock::time_point coingecko_last_success_at_{};

    /// TibetSwap AMM reserve client -- the producer for
    /// ArbitrageDetector::set_tibetswap_reserves().
    std::shared_ptr<rpc::TibetSwapClient> tibetswap_;

    /// Timestamp of the last TibetSwap fetch attempt (successful or not).
    /// Gates the poll so a hard-down API is retried on the configured cadence
    /// rather than on every block.
    std::chrono::steady_clock::time_point tibetswap_last_fetch_;

    /// True once at least one TibetSwap poll has been attempted, so the very
    /// first heartbeat fetches immediately instead of waiting a full interval.
    bool tibetswap_fetch_attempted_{false};

    /// Wall-clock time of the last SUCCESSFUL TibetSwap reserve fetch, i.e.
    /// when the cached reserves were actually read from the chain.  This is
    /// what MarketDataFeed::ingest_amm_mid() is given as the observation time,
    /// so amm_age_seconds measures real staleness.  Default-constructed means
    /// "never fetched", and no AMM sample is published in that state.
    Timestamp tibetswap_reserves_at_{};

    /// True when tibetswap_reserves_at_ advanced on THIS heartbeat and the new
    /// reserves have not been published to the market data feed yet.  Step 1
    /// publishes once per successful fetch and then clears this; without it the
    /// cache was re-ingested every heartbeat, re-stamping a stale sample as
    /// fresh and making every AMM freshness gate unreachable.
    bool tibetswap_reserves_pending_{false};

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

    /// Set by every RISK-BREAKER pause (max-drawdown, rolling-window loss,
    /// ledger divergence) and never cleared at runtime: manual intervention
    /// means a restart, matching the in-memory peak re-anchor semantics.
    /// A separate flag because BotStatus is NOT a safe carrier for breaker
    /// state -- check_pause_flag() flips any Paused status back to Running
    /// when the GUI flag is removed, without knowing who owns the pause, so
    /// a GUI toggle after a breaker trip would silently resume posting.
    bool breaker_pause_active_{false};

    /// The Step 8 skip warning fires once per breaker trip, then drops to
    /// debug -- re-warning every block for an indefinite pause is the same
    /// log spam Step 13 rate-limits.
    bool breaker_skip_warned_{false};

    /// [S19 review round 11] Whether the bridge scan can currently act
    /// as the bridge asset's inventory maintainer.  The Step 8 recovery
    /// seed and Step 11 one-shot reconcile exclude the asset ONLY while
    /// this holds -- if the scan stands down (ledger off, genesis not
    /// done, incomplete ledger, empty path, asset not opened), the
    /// recovery paths resume maintaining it instead of both sides
    /// standing down forever.
    [[nodiscard]] bool bridge_accounting_operational() const;


    /// [S19 review round 18] Ledger event ids already booked (or found
    /// already-booked) by the bridge scan.  Historical jobs stay in the
    /// scan forever, and without this every one of them executed a
    /// write transaction (BEGIN IMMEDIATE / INSERT OR IGNORE / COMMIT)
    /// per heartbeat for the lifetime of the database.  One no-op pass
    /// per restart warms the cache.
    std::set<std::string> bridge_booked_event_ids_;
    /// [S19 round 33] Event ids whose booking decision is TERMINAL for
    /// this process: unclassifiable rows, foreign-asset fingerprints,
    /// pre-opening chronology, and rejected valuations are immutable
    /// functions of the row content, the config, and the ledger
    /// opening, so such a job can never become bookable later.  Without
    /// this cache every permanently-skipped job (the expected
    /// pre-opening job 2 included) would keep the round-31
    /// unbooked-candidate check true forever and force an extra wallet
    /// RPC every heartbeat.  In-memory: restarts re-derive it.
    std::set<std::string> bridge_terminal_skip_ids_;
    /// [S19 rounds 35+42] Wall-clock ISO stamp captured on the
    /// scan's first invocation.  A flow whose completion (flow_at)
    /// predates it is already inside the current equity anchor, so
    /// booking it must NOT trigger the flow re-anchor -- erasing live
    /// drawdown state for capital that moved before this process
    /// existed.  Ties and unparseable stamps count as pre-process.
    std::string bridge_process_start_iso_;
    /// [S19 review round 10] Job ids whose skip condition
    /// (unclassifiable / foreign fingerprint / missing transition event)
    /// has already been warned about -- such jobs stay in the scan
    /// forever, and one legitimate milliETH job must not warn on every
    /// heartbeat for the lifetime of the database.
    std::set<std::int64_t> bridge_warned_jobs_;

    /// [S18 2026-08-23] Consecutive lifted evaluations of the max-drawdown
    /// condition; the re-alert gate only re-arms once this reaches the
    /// debounce streak, so one transient false read (a flaky wallet RPC
    /// corrupting an equity computation) cannot re-arm it mid-episode.
    int breaker_lift_streak_{0};

    /// [S17 2026-08-23] Last depeg status logged per pair, so Step 3 logs
    /// transitions at full severity and ongoing states at debug.
    std::unordered_map<std::string, DepegStatus> depeg_logged_status_;

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

        // [2026-08-01 adversarial review, finding 1] Step 7's uncertainty
        // width floor, threaded to Step 8 so quote-recovery repricing can
        // respect it without recomputing: the blended ladder centre in
        // mojos, and the per-pair minimum half-spread in bps
        // (max(min_profit_margin, tibetswap_fee, k_sigma * combined_sigma)
        // -- the same value the Step 7 width-floor pass enforces).  Both
        // stay 0 until Step 7 reaches ladder generation for the pair this
        // cycle; Step 8 treats 0 as "no floor available" and skips the
        // recovery repricing rather than running it unfloored.
        Mojo          quote_mid_mojos{0};
        double        quote_min_half_spread_bps{0.0};
    };

    /// Per-pair cycle state for the current block.
    std::unordered_map<std::string, PairCycleState> cycle_;

    // -- Ratio-based inventory rebalance mode (per pair) -------------------
    enum class RatioRebalanceMode : std::uint8_t {
        Neutral      = 0,
        AcquireBase  = 1,
        AcquireQuote = 2
    };
    std::unordered_map<std::string, RatioRebalanceMode> ratio_rebalance_modes_;

    // [H6] Portfolio-equity high-water mark for drawdown detection in
    // step 13 alerts.  Monotonically non-decreasing; updated each cycle in
    // step_check_alerts.
    // ISO/IEC 5055: prevents false drawdown resets on equity oscillation.
    //
    // [DRAWDOWN-EQUITY 2026-08-04] USD PORTFOLIO EQUITY (sum of holdings x
    // USD price), replacing the P&L-total peak: measuring drawdown against
    // the ~$25 P&L peak turned a ~5% overnight XCH retrace (~$8 of marks on
    // a ~$158 book) into a "60% drawdown" false trip at 04:14.  In-memory
    // only, re-seeded from the first cycle after every restart -- A RESTART
    // RE-ANCHORS THE PEAK to current equity, exactly as the old P&L peak
    // behaved; the breaker then protects against drawdown from that new
    // anchor.  No persisted state carries the old semantics across the
    // change.
    double peak_equity_hwm_usd_{0.0};

    // [MEDIUM-7] note: the old P&L peak needed an explicit first-cycle
    // seed flag because P&L can be negative and max(0, pnl) would have
    // hidden a losing start.  Equity is non-negative by construction, so
    // the monotonic max IS the seed and the flag is retired; the startup
    // grace window covers the pre-valuation cycles.

    // [DRAWDOWN-EQUITY 2026-08-04] Last successfully observed USD price
    // per asset unit, keyed by asset id.  When an asset has no live USD
    // conversion this cycle (empty book, cold feed), its equity
    // contribution is carried at this last-known price instead of being
    // dropped: a vanished conversion would otherwise delete the asset's
    // entire value from equity and read as an instantaneous crash --
    // firing the breaker on a DATA GAP rather than a market move.
    std::unordered_map<AssetId, double> last_asset_usd_price_;

    // [S20 2026-08-24] Block at which each asset last had a LIVE
    // valuation-grade price (as opposed to a carried one).  When a held
    // asset's carry outlives risk.valuation_carry_ttl_blocks, the equity
    // figure is declared degraded for the cycle: the value itself keeps
    // being used (a data gap must not read as a crash) but it loses the
    // authority to move the drawdown PEAK.  Breakers stay armed against
    // the frozen peak -- disarming them would fail open.
    std::unordered_map<AssetId, BlockHeight> last_asset_live_block_;

    /// [S20] Set by compute_portfolio_equity_usd when any held asset's
    /// carry has outlived its TTL this cycle.  Consumed by Step 13, where
    /// it removes PEAK-UPDATE authority ONLY: a suspect number must not
    /// ratchet the high-water mark, but both breakers stay armed and keep
    /// comparing against the frozen peak.  Disarming them would make a
    /// risk control fail open for a condition that can persist hours.
    bool valuation_degraded_{false};

    /// [S20] Debounce streak for re-arming the peak after degradation.
    static constexpr int kValuationRearmCleanCycles = 10;

    /// [S20] Consecutive non-degraded equity computations.  After a
    /// degraded episode the peak stays frozen until this reaches
    /// kValuationRearmCleanCycles -- the S18 lift-streak idiom, so
    /// alternating junk/honest cycles cannot flap the peak upward.
    /// Starts AT the threshold: a process that has never degraded seeds
    /// its peak from the first valued cycle, as before.
    int valuation_clean_streak_{kValuationRearmCleanCycles};

    /// [S20] Warn-once flag for the degraded episode (reset on recovery).
    bool valuation_degraded_warned_{false};

    // [DRAWDOWN-EQUITY 2026-08-04] Re-alert suppression for the breakers:
    // while the engine stays Paused on a persisting condition, the
    // CRITICAL alert is re-raised at most every
    // risk.breaker_realert_minutes (default 30) and the condition is
    // otherwise logged at info level.  Cleared when the condition lifts.
    risk::BreakerRealertGate breaker_realert_gate_;

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

    // -- Buyer (Step 9e) state -----------------------------------------------
    // Per-pair last take block for cooldown enforcement.
    std::unordered_map<std::string, BlockHeight> buyer_last_take_block_;

    // Per-pair accumulated take volume in current epoch for daily cap.
    std::unordered_map<std::string, double> buyer_epoch_taken_;

    // Block at which the current epoch started (for daily cap reset).
    BlockHeight buyer_epoch_start_{0};

    // Count of successful takes this block (for per-block cap).
    uint32_t buyer_takes_this_block_{0};

    // -- Midpoint recycling (Step 9d) state ---------------------------------
    std::unordered_map<std::string, BlockHeight> midpoint_last_take_block_;
    std::unordered_map<std::string, double> midpoint_epoch_taken_xch_;
    BlockHeight midpoint_epoch_start_{0};
    uint32_t midpoint_takes_this_block_{0};

    // -- Drift corrector (Step 9f) state ------------------------------------
    // Last block at which the corrector successfully took an offer (for
    // cooldown enforcement) and a rolling 24h history of take timestamps
    // (for the daily-trade quota).
    BlockHeight last_drift_correction_block_{0};
    std::deque<std::chrono::system_clock::time_point> drift_correction_history_;

    // True when Step 9c successfully took at least one offer on this block.
    bool crossed_book_take_this_block_{false};

    // [T3-09] Max-drawdown global circuit breaker threshold.
    // Drawdown fraction = risk::equity_drawdown_frac(peak_equity_hwm_usd_,
    // equity_usd) -- portfolio equity on both sides ([DRAWDOWN-EQUITY
    // 2026-08-04]).  When exceeded, engine transitions to
    // BotStatus::Paused and alerts.  Configurable via
    // risk.max_drawdown_frac in config.yaml; default 10% of equity.
    // ISO/IEC 5055: named constant with documented default.
    double max_drawdown_frac_;

    // [T3-36] Rolling time-window PnL loss circuit breaker.
    //
    // Records (block_height, total_pnl_USD) pairs each heartbeat cycle
    // ([DRAWDOWN-USD 2026-08-02]: USD double, previously the raw
    // quote-mojo sum).  The deque is trimmed to retain only entries within
    // the most recent loss_window_blocks blocks; stale entries (age >=
    // window) are discarded from the front.  The oldest surviving entry
    // provides the baseline PnL for the window loss calculation.
    //
    // Loss in window = oldest_pnl_usd - current_pnl_usd (positive = losing).
    // Threshold      = risk::window_loss_threshold_usd(peak_pnl_hwm_usd_,
    //                  anchor, max_window_loss_bps); the anchor falls back
    //                  to the live 1-XCH USD value (or the conservative
    //                  fixed nominal) when the bot has never been
    //                  profitable.
    //
    // When loss_in_window > threshold AND threshold > 0, the engine
    // transitions to BotStatus::Paused the same way the HWM circuit breaker
    // does.  The two circuit breakers are independent; either can fire first.
    //
    // ISO/IEC 27001:2022: continuous monitoring within a bounded time window.
    // ISO/IEC 5055: deque prevents unbounded memory growth.
    std::deque<std::pair<BlockHeight, double>> pnl_window_usd_;

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

    // -- PID adaptive spread controller state (per-pair) ------------------
    // Tracks fill-rate EMA and PID accumulators for each trading pair.
    // Updated in Step 2 (fill counting) and Step 5 (PID update + apply).
    struct SpreadPidState {
        double   ema_fill_rate{0.0};     ///< Exponential moving average of fill signal.
        double   integral_error{0.0};    ///< Accumulated integral error.
        double   prev_error{0.0};        ///< Previous error for derivative term.
        double   current_mult{1.0};      ///< Current output spread multiplier.
        uint32_t blocks_active{0};       ///< Blocks since first offer was posted.
        int      fills_this_cycle{0};    ///< Fills counted this heartbeat.
    };
    std::unordered_map<std::string, SpreadPidState> spread_pid_state_;

    // -- PID adaptive competitiveness-threshold controller (per-pair) -----
    // Companion to spread_pid_state_.  Adjusts the integer score required
    // by the Step 8 competitiveness gate based on observed fill rate.
    // See cpp/include/xop/strategy/competitiveness_pid.hpp.
    //
    // Per-pair entries are lazily created on first observe in Step 5.
    // The pid_fills_this_block_ counter is incremented on each confirmed
    // fill (Step 2) and reset by observe_block() at end of heartbeat.
    std::unordered_map<std::string, xop::strategy::CompetitivenessPid>
        comp_pid_state_;
    std::unordered_map<std::string, int> comp_pid_fills_this_block_;

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
        // Block at which this snapshot was taken.  Step 8 (the only writer)
        // is skipped during GUI pause, flash-crash, XCH-recovery and
        // wallet-circuit-open, while Step 2 keeps processing fills -- so a
        // consumer must check freshness or it may reconcile against a
        // pre-fill balance and undo the fill (PNL-BASIS-PERSIST 2026-07-30).
        BlockHeight as_of_block{0};
        // [S19 review round 28] True only when the RPC response actually
        // carried confirmed_wallet_balance AND pending_change.  Writers
        // default missing fields to zero for the spendable-gating
        // consumers, but a defaulted zero must never read as settled
        // wallet truth: the bridge scan requires this bit before
        // reconciling inventory/State, or a malformed response would
        // fold a real balance to nothing and let the invariant adjust
        // the recorded ledger balance away.
        bool fields_validated{false};
    };
    std::unordered_map<std::string, WalletBalanceEntry> cached_wallet_balances_;

    // -- [PNL-BASIS-PERSIST 2026-07-30] One-shot wallet reconcile ---------
    // After restart the restored inventory quantities can drift from the
    // wallet (deposits/withdrawals while the engine was down; fills settled
    // during downtime are replayed via restored pending offers instead).
    // Once cached_wallet_balances_ and market mids are warm, tracked
    // quantities are reconciled to wallet truth exactly once per process:
    // decreases draw cost down proportionally, increases are added at the
    // current mid (mark-at-receipt).  Gated on an empty confirmation buffer
    // plus a grace period so downtime fills replay through the normal path
    // first (otherwise the wallet delta would be double-applied).
    //
    // Tracked PER ASSET, not as a single flag: an asset can be skipped on a
    // given heartbeat (coins in flight, stale balance, no market mark), and
    // a single flag would burn the one-shot for every other asset too.  A
    // market maker reposting a ladder each block leaves XCH with
    // pending_change != 0 most of the time, so the single-flag version
    // almost never reconciled XCH -- the asset that needed it most.
    std::unordered_set<std::string> inventory_reconciled_assets_;

    /// First block height observed by step_update_pnl (0 = none yet).
    /// Anchors the wallet-reconcile grace period above.
    BlockHeight pnl_first_block_{0};

    // -- Ledger invariant control state (LEDGER 2026-07-30) ---------------
    // Consecutive same-sign breaches per asset.  A divergence caused by
    // detection latency self-heals on the next observation; a real one does
    // not, so escalation requires persistence rather than a single sample.
    // Recent observations per asset, newest last.  Breaches are scored over
    // this window rather than requiring strict consecutiveness: the
    // tolerance includes live offer exposure, which swings ~100x between
    // heartbeats as the book is re-quoted, so a genuine constant divergence
    // would otherwise keep having a consecutive counter reset by whichever
    // heartbeats happen to carry a large book.
    struct LedgerObservation {
        int  sign{0};             // +1 ledger above wallet, -1 below, 0 clean.
        Mojo divergence{0};
    };
    std::unordered_map<std::string, std::deque<LedgerObservation>>
        ledger_observations_;

    /// True once opening balances have been established this process.
    bool ledger_genesis_done_{false};

    /// Consecutive breaching observations per peg signal, keyed by signal
    /// name.  DEX-vs-CEX basis spikes are transient; a real depeg persists.
    std::unordered_map<std::string, int> peg_breach_;

    // -- Arbitrage leg-pair state machine (ARB-ARMING 2026-07-30) ----------
    //
    // Spreads on this venue swing by an order of magnitude between
    // heartbeats, so a single reading cannot separate a real opportunity
    // from a stale quote -- and stale is the common case (a chosen offer
    // came back status=3, already gone, on 2026-07-30).  Execution is
    // therefore gated on an edge that PERSISTS, with a lower disarm
    // threshold so the state cannot flap at the boundary.
    struct ArbLegState {
        int         consecutive_above{0};  ///< Observations above arm threshold.
        bool        armed{false};
        double      last_net_edge_bps{0.0};
        BlockHeight armed_since_block{0};
    };
    /// Keyed by direction, e.g. "buy@XCH/BYC -> sell@XCH/wUSDC.b".
    std::unordered_map<std::string, ArbLegState> arb_leg_state_;

    /// Update the arm/disarm machine for one leg pair and persist the
    /// observation.  Returns true when execution is permitted right now
    /// (armed AND the execute switch is on).
    bool update_arb_leg_state(const std::string& direction,
                              BlockHeight block_height,
                              Mojo ask_a, Mojo bid_b, double cross_rate,
                              double gross_edge_bps, double net_edge_bps,
                              Mojo ask_size, Mojo bid_size);

    /// Chain height observed during startup reconcile; the anchor for
    /// genesis so downtime fills are not counted twice.
    BlockHeight startup_block_{0};

    /// Per-asset genesis block, cached from the ledger's `opening` legs.
    /// A fill at or below an asset's genesis block is already inside its
    /// opening balance and must not post a leg.
    std::unordered_map<std::string, BlockHeight> ledger_genesis_block_;

    /// Assets that actually have an `opening` leg.  Legs are posted ONLY for
    /// these.  A per-asset wallet RPC timeout at startup (observed on this
    /// deployment 2026-07-26 for the XCH wallet) leaves an asset unopened;
    /// posting its fills anyway would put legs in the ledger with no opening
    /// balance, and the NEXT restart would then open at a wallet balance that
    /// already reflects them -- baking in a permanent divergence equal to the
    /// whole session's flow, carrying the same signature as the phantom-fill
    /// bug this control exists to measure.
    std::unordered_set<std::string> ledger_opened_assets_;

    /// Set when a ledger write FAILED (not merely duplicated).  The ledger
    /// is then known-incomplete, so the invariant control stands down rather
    /// than reporting a divergence it caused itself.
    bool ledger_incomplete_{false};

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
