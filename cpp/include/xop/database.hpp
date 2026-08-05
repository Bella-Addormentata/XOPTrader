// database.hpp -- SQLite persistence layer for XOPTrader CHIA DEX market-maker.
//
// Provides crash-safe, append-only storage for the core data streams:
//
//   1. trade_log   -- every settled fill, forming the authoritative audit trail
//                     (ISO/IEC 27001:2022 Section 15 compliance).
//   2. offer_log   -- current lifecycle state of every offer.
//   3. offer_closure_events -- append-only resolution/observation history for
//                     offers, preserving the first close cause.
//   4. snapshots   -- periodic per-block market/risk state for analytics.
//
// Implementation notes:
//   - Uses the sqlite3 C API directly (no ORM).
//   - WAL journal mode enables concurrent readers without blocking the single
//     writer (the engine heartbeat loop).
//   - All queries use prepared statements with parameter binding to prevent
//     SQL injection (ISO/IEC 5055 -- no string-concatenated queries).
//   - Transaction batching for snapshot writes avoids per-row fsync overhead.
//   - The schema is created idempotently via IF NOT EXISTS so that the
//     constructor is safe to call against an existing database file.
//
// Thread safety:
//   All public methods are protected by an internal std::mutex (T7-01),
//   making the Database safe for concurrent use from the engine loop and
//   the GUI's DatabaseService QThread.  WAL mode permits concurrent
//   *readers* from other processes (e.g. Grafana).
//
// Compliant with:
//   ISO/IEC 27001:2022 -- append-only audit trail, parameterised queries
//   ISO/IEC 5055       -- RAII resource management, prepared statements
//   ISO/IEC 25000      -- documented interfaces, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard C++20, no undefined behaviour

#ifndef XOP_DATABASE_HPP
#define XOP_DATABASE_HPP

#include "xop/types.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declare the opaque sqlite3 handles to avoid pulling the full
// SQLite amalgamation header into every translation unit.
struct sqlite3;
struct sqlite3_stmt;

namespace xop {

// ---------------------------------------------------------------------------
// DbTradeRecord -- maps 1:1 to a row in the trade_log table.
//
// Named with a Db prefix to avoid collision with xop::TradeRecord in pnl.hpp,
// which uses a different field layout (Timestamp vs ISO-8601 string, etc.).
// All monetary values use int64_t mojos to prevent floating-point drift.
// The trade_id field is the globally unique identifier (Chia spend-bundle
// hash or a UUID generated at fill-detection time).
// ---------------------------------------------------------------------------

struct DbTradeRecord {
    std::string timestamp;          ///< ISO-8601 UTC string.
    std::string trade_id;           ///< Globally unique fill identifier.
    std::string pair_name;          ///< Trading pair, e.g. "XCH/wUSDC".
    std::string side;               ///< "bid" or "ask".
    Mojo        price_mojos{0};     ///< Execution price in mojos.
    Mojo        size_mojos{0};      ///< Filled quantity in mojos of base asset.
    Mojo        fee_mojos{0};       ///< Blockchain fee paid in mojos.
    Mojo        cost_basis_mojos{0};///< Weighted-average cost basis at time of fill.
    Mojo        realized_pnl_mojos{0}; ///< Realized PnL on this fill (mojos).
    BlockHeight block_height{0};    ///< Settlement block number.
};

// ---------------------------------------------------------------------------
// DbOfferRecord -- maps 1:1 to a row in the offer_log table.
//
// Named with a Db prefix to avoid collision with xop::rpc::OfferRecord in
// dexie_client.hpp, which represents dexie API response data.
// ---------------------------------------------------------------------------

struct DbOfferRecord {
    std::string offer_id;           ///< Unique offer identifier.
    std::string pair_name;          ///< Trading pair, e.g. "XCH/wUSDC".
    std::string side;               ///< "bid" or "ask".
    Mojo        price_mojos{0};     ///< Offer price in mojos.
    Mojo        size_mojos{0};      ///< Offered quantity in mojos.
    int         tier{0};            ///< Tier index (0 = tightest).
    int         competitiveness_score{0}; ///< 1-10 score vs current competing BBO.
    Mojo        queue_ahead_mojos{0}; ///< Same-side competing size priced ahead of this offer.
    int         queue_ahead_score{0}; ///< 1-10 score for queue position vs same-side depth.
    int         execution_quality_score{0}; ///< Weighted 70/30 blend of price competitiveness and queue position.
    std::string status{"pending"};  ///< "pending", "filled", "cancelled", "expired".
    BlockHeight created_block{0};   ///< Block at which the offer was broadcast.
    BlockHeight resolved_block{0};  ///< Block at which the offer was resolved (0 if pending).
    std::uint64_t fee_mojos{0};      ///< Fee attached to this offer (mojos).
    Mojo        book_best_bid{0};   ///< Best competing bid at offer creation.
    Mojo        book_best_ask{0};   ///< Best competing ask at offer creation.
};

// ---------------------------------------------------------------------------
// DbSnapshot -- maps 1:1 to a row in the snapshots table.
// ---------------------------------------------------------------------------

struct DbSnapshot {
    BlockHeight block_height{0};    ///< Block at which the snapshot was taken.
    std::string pair_name;          ///< Trading pair.
    Mojo        mid_price_mojos{0}; ///< Aggregated mid-price in mojos.
    double      spread_bps{0.0};    ///< Current spread in basis points.
    double      inventory_ratio{0.0}; ///< Inventory ratio [0, 1]; 0.5 = balanced.
    double      sigma_block{0.0};   ///< Per-block volatility estimate.
    std::string regime;             ///< Market regime label ("MeanReverting", "Random", "Momentum").
    Mojo        pnl_total_mojos{0}; ///< Cumulative total PnL in mojos.
    double      xch_usd_rate{0.0};  ///< Live XCH/USD mark used for conversion.
    double      pnl_total_usd{0.0}; ///< Cumulative total PnL converted to USD.

    // -- Strategy decision parameters (Phase 2 analytics) --------------------
    Mojo        reservation_price_mojos{0}; ///< A-S reservation price in mojos.
    double      half_spread_bps{0.0};       ///< Optimal half-spread from spread optimizer (bps).
    double      kappa{0.0};                 ///< Calibrated fill-intensity decay parameter.
    double      variance_ratio{0.0};        ///< Lo-MacKinlay variance ratio (1.0 = random walk).
    double      adverse_rate{0.0};          ///< Fraction of fills classified as adverse.
    double      s_adverse_bps{0.0};         ///< Adverse selection spread component (bps).
    double      s_inventory_bps{0.0};       ///< Inventory risk spread component (bps).
    double      s_cost_bps{0.0};            ///< Transaction cost spread component (bps).
};

// ---------------------------------------------------------------------------
// DbSnapshotMidTick -- one (block, mid, wall-clock) observation from the
// snapshots table, the minimal row needed to warm-start the volatility
// estimator ([AS-WARM]).  unix_seconds is created_at as a Unix timestamp.
// ---------------------------------------------------------------------------

struct DbSnapshotMidTick {
    BlockHeight  block_height{0};    ///< Block at which the snapshot was taken.
    Mojo         mid_price_mojos{0}; ///< Mid-price in kMojosPerXch fixed point.
    std::int64_t unix_seconds{0};    ///< created_at as Unix epoch seconds.
};

// ---------------------------------------------------------------------------
// DbSanityFailure -- maps 1:1 to a row in the sanity_failures table.
//
// Records every offer that failed the pre-posting sanity checks, for
// post-hoc analysis and debugging of pricing anomalies.
// ---------------------------------------------------------------------------

struct DbSanityFailure {
    BlockHeight block_height{0};       ///< Block height when failure was detected.
    std::string pair_name;             ///< Trading pair, e.g. "XCH/wUSDC.b".
    std::string side;                  ///< "bid" or "ask".
    int         tier{-1};              ///< Tier index (-1 for pair-level failures).
    Mojo        proposed_price_mojos{0}; ///< Price that was rejected in mojos.
    Mojo        reference_price_mojos{0}; ///< Reference price (BBO mid, best_bid, etc.) in mojos.
    double      deviation_pct{0.0};    ///< Deviation as percentage (e.g., 25.5 for 25.5%).
    std::string failure_reason;        ///< Reason for rejection (e.g., "bbo_deviation_pair_level").
    std::string details;               ///< Additional context (JSON-friendly format).
};

// ---------------------------------------------------------------------------
// DbStrategyQuote -- maps 1:1 to a row in the strategy_quotes table.
//
// Persists the per-tier bid/ask quotes computed each block.  This enables
// post-hoc analysis of which tier spacings captured the most spread PnL,
// fill probability modelling, and optimal tier configuration tuning.
// ---------------------------------------------------------------------------

struct DbStrategyQuote {
    BlockHeight block_height{0};    ///< Block at which quotes were computed.
    std::string pair_name;          ///< Trading pair.
    int         tier{0};            ///< Tier index (0 = tightest).
    std::string side;               ///< "bid" or "ask".
    Mojo        price_mojos{0};     ///< Quote price in mojos.
    Mojo        size_mojos{0};      ///< Quote size in mojos.
};

// ---------------------------------------------------------------------------
// DbInventoryState -- maps 1:1 to a row in the inventory_state table.
//
// Persists the InventoryTracker's per-asset cost-basis accounting so that
// realized-P&L attribution survives engine restarts (PNL-BASIS-PERSIST,
// TODO #9).  Before this table existed, every restart re-seeded all assets
// at a sentinel basis and realized P&L was recorded as 0 until the next
// buy fill in the same process lifetime.
//
// total_cost is a REAL: it holds sum(fill_price * qty) where fill_price is a
// USD-normalized pseudo-price (~1e12) and qty is in base mojos, so the value
// (~1e24+) exceeds int64 range by design.
// ---------------------------------------------------------------------------

struct DbInventoryState {
    AssetId asset_id;                    ///< "xch" or 64-hex CAT id.
    Mojo    total_quantity{0};           ///< Tracked holdings in mojos.
    double  total_cost{0.0};             ///< Cumulative cost of open holdings.
    bool    basis_is_seed_sentinel{false}; ///< True when basis is synthetic.
};

// ---------------------------------------------------------------------------
// DbLedgerEntry -- one leg of a double-entry accounting event.
//
// Every event that changes what the bot believes it holds posts a BALANCED
// set of legs (e.g. an ask fill posts base -size, quote +proceeds, xch -fee).
// Summing delta_mojos per asset gives the ledger's implied balance, which is
// then tied to the wallet's confirmed balance by the reconciliation control.
//
// The ledger records what the bot BELIEVES happened, from its own event
// stream.  That is deliberate: the gap between belief and the wallet is the
// diagnostic signal.  trade_log was shown on 2026-07-30 to claim a ~604 XCH
// outflow across a period when the wallet actually gained 61 XCH, and nothing
// in the system noticed for three months because no invariant tied the two
// together.
//
// Idempotency: (event_id, leg, asset_id) is UNIQUE.  A fill re-detected after
// a crash re-posts identical legs, which are ignored rather than doubled.
// ---------------------------------------------------------------------------

struct DbLedgerEntry {
    std::string entry_time;         ///< ISO-8601 UTC of the event.
    std::string event_type;         ///< opening | fill | fee | take | adjust.
    std::string event_id;           ///< trade_id, or a synthetic unique id.
    std::string leg;                ///< base | quote | fee | opening | adjust.
    AssetId     asset_id;           ///< Canonical id ("xch" or 64-hex).
    Mojo        delta_mojos{0};     ///< Signed: + inflow, - outflow.
    std::string pair_name;          ///< Context (may be empty).
    BlockHeight block_height{0};    ///< Settlement block, 0 if not applicable.
    std::string note;               ///< Free-form provenance.
};

// ---------------------------------------------------------------------------
// DbArbEdgeObservation -- one sample of an arbitrage leg-pair's economics.
//
// Written every scan whether or not a trade is taken, so the strategy's
// viability can be measured from live data instead of assumed.  Spreads on
// this venue are extremely volatile (XCH/BYC ranges 0-1247 bps), so a
// point-in-time edge cannot distinguish a real opportunity from a stale
// quote -- the history is what separates them.
// ---------------------------------------------------------------------------

struct DbArbEdgeObservation {
    std::string observed_at;        ///< ISO-8601 UTC.
    BlockHeight block_height{0};
    std::string direction;          ///< "buy@XCH/BYC -> sell@XCH/wUSDC.b".
    Mojo        ask_a_mojos{0};     ///< Price paid on the buy leg.
    Mojo        bid_b_mojos{0};     ///< Price received on the sell leg.
    double      cross_rate{0.0};    ///< Stable cross used to normalise.
    double      gross_edge_bps{0.0};
    double      net_edge_bps{0.0};  ///< After fees and costs.
    Mojo        ask_size_mojos{0};  ///< Counterparty size (takes are all-or-nothing).
    Mojo        bid_size_mojos{0};
    std::string state;              ///< dormant | watching | armed.
    bool        armed{false};
    bool        executed{false};
};

// ---------------------------------------------------------------------------
// DbTakerFill -- a trade where WE crossed the spread and took someone's offer.
//
// Distinct from trade_log, which records OUR offers being filled by others.
// Taker trades used to write nothing anywhere: 70 of them across the
// retained logs, invisible to trade_log, offer_log and P&L alike. The ledger
// catches the aggregate as an `adjust` leg, but that only says "N units
// unaccounted" -- it cannot say which trade, at what price, or whether the
// strategy that placed it makes money.
// ---------------------------------------------------------------------------

struct DbTakerFill {
    std::string taken_at;              ///< ISO-8601 UTC.
    BlockHeight block_height{0};
    std::string strategy;              ///< crossed_book | cross_stable | peg_arb | drift | xch_recovery.
    std::string trade_id;              ///< Wallet trade id returned by take_offer.
    std::string counterparty_offer_id; ///< The offer we lifted.
    std::string pair_name;
    bool        we_bought_base{false}; ///< True = we acquired base, paid quote.
    AssetId     base_asset;
    Mojo        base_delta_mojos{0};   ///< Signed, our perspective.
    AssetId     quote_asset;
    Mojo        quote_delta_mojos{0};  ///< Signed, our perspective.
    Mojo        price_mojos{0};
    Mojo        fee_mojos{0};
};

// ---------------------------------------------------------------------------
// Database -- SQLite wrapper providing structured persistence for the bot.
//
// Lifecycle:
//   1. Database(path) -- opens (or creates) the database file, enables WAL
//      mode, and runs the schema migration (CREATE TABLE IF NOT EXISTS).
//   2. Call insert_* and update_* methods from the engine heartbeat loop.
//   3. Call query_* methods from monitoring or analytics subsystems.
//   4. Destructor finalises all prepared statements and closes the connection.
//
// Error handling:
//   All methods throw std::runtime_error on SQLite failures.  The caller
//   (engine) decides whether to retry, log, or abort.
// ---------------------------------------------------------------------------

class Database {
public:
    // -- Lifecycle -----------------------------------------------------------

    /// Open or create the database at the given filesystem path.
    /// Enables WAL journal mode and runs schema migrations.
    ///
    /// @param db_path  Filesystem path for the SQLite file.
    /// @throws std::runtime_error if the database cannot be opened or
    ///         the schema migration fails.
    explicit Database(const std::string& db_path);

    /// Finalise all prepared statements and close the SQLite connection.
    ~Database();

    // Non-copyable, non-movable -- the SQLite connection and prepared
    // statements are not safely relocatable.
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&)                 = delete;
    Database& operator=(Database&&)      = delete;

    // -- Trade log (append-only audit trail) ---------------------------------

    /// Insert a single trade record into the trade_log table.
    /// Uses a prepared statement with parameter binding.
    ///
    /// @param record  Fully populated DbTradeRecord.
    /// @throws std::runtime_error on SQLite error or UNIQUE constraint
    ///         violation (duplicate trade_id).
    void insert_trade(const DbTradeRecord& record);

    /// Query trades for a specific pair within a time range.
    /// Returns records ordered by timestamp ascending.
    ///
    /// @param pair_name  Filter by pair (empty string matches all pairs).
    /// @param start_ts   Inclusive lower bound, ISO-8601 UTC string.
    /// @param end_ts     Exclusive upper bound, ISO-8601 UTC string.
    /// @return           Vector of matching DbTradeRecords.
    [[nodiscard]]
    std::vector<DbTradeRecord> query_trades(const std::string& pair_name,
                                            const std::string& start_ts,
                                            const std::string& end_ts) const;

    // -- Offer log -----------------------------------------------------------

    /// Insert a new offer into the offer_log table.
    ///
    /// @param record  Fully populated DbOfferRecord.
    /// @throws std::runtime_error on SQLite error.
    void insert_offer(const DbOfferRecord& record);

    /// Update the status of an existing offer (e.g. "pending" -> "filled").
    /// Also records the block at which the offer was resolved and a
    /// resolution timestamp.
    ///
    /// When a later reconciliation pass observes an already-resolved offer,
    /// the current offer_log row preserves the original close cause while the
    /// later observation is appended to offer_closure_events.
    ///
    /// @param offer_id       The offer's unique identifier.
    /// @param new_status     New status string ("filled", "cancelled", "expired").
    /// @param resolved_block Block height at which the status changed.
    /// @param cancel_reason  Human-readable reason for cancellation (empty for fills).
    /// @throws std::runtime_error if no row matches offer_id.
    void update_offer_status(const std::string& offer_id,
                             const std::string& new_status,
                             BlockHeight        resolved_block,
                             const std::string& cancel_reason = "");

    /// Return all offers with status='pending' from the offer_log table.
    /// Used on startup to recover offers that were pending when the engine
    /// last shut down, enabling orphan detection against the wallet.
    ///
    /// @return Vector of DbOfferRecord with status "pending".
    [[nodiscard]]
    std::vector<DbOfferRecord> query_pending_offers() const;

    // -- Snapshots -----------------------------------------------------------

    /// Insert a single analytics snapshot.
    ///
    /// @param snap  Fully populated DbSnapshot.
    void insert_snapshot(const DbSnapshot& snap);

    /// Insert multiple snapshots inside a single transaction for performance.
    /// If any individual insert fails, the entire batch is rolled back.
    ///
    /// @param batch  Vector of DbSnapshot records.
    void insert_snapshots_batch(const std::vector<DbSnapshot>& batch);

    // -- Sanity failures log -------------------------------------------------

    /// Insert a sanity failure record for post-hoc analysis.
    /// Used when offers fail pre-posting sanity checks (BBO proximity, etc.).
    ///
    /// @param record  Fully populated DbSanityFailure.
    void insert_sanity_failure(const DbSanityFailure& record);

    /// Retrieve the most recent snapshot for a given trading pair.
    /// Returns std::nullopt if no snapshot exists for the pair.
    ///
    /// @param pair_name  Trading pair to query.
    /// @return           The latest DbSnapshot, or std::nullopt.
    [[nodiscard]]
    std::optional<DbSnapshot> get_last_snapshot(const std::string& pair_name) const;

    /// [AS-WARM] Most recent snapshot mid-prices for a pair, in ASCENDING
    /// block order, for warm-starting the volatility estimator at startup
    /// (VolatilityEstimator::rehydrate_from_ticks).  Rows with a
    /// non-positive mid are excluded at the SQL level.  unix_seconds is the
    /// row's created_at converted via strftime('%s', ...) so callers can
    /// measure the true tick cadence (median inter-row spacing) instead of
    /// assuming one.  Returns an empty vector when the pair has no history
    /// or on error (startup-only path; never throws).
    ///
    /// @param pair_name  Trading pair to query.
    /// @param limit      Maximum rows (the newest N are returned).
    [[nodiscard]] std::vector<DbSnapshotMidTick> get_recent_snapshot_mids(
        const std::string& pair_name, std::uint32_t limit) const noexcept;

    // -- Strategy quotes (per-tier quote persistence) ------------------------

    /// Insert a batch of per-tier strategy quotes inside a single transaction.
    /// Called from step_update_pnl to persist all tier quotes for the block.
    ///
    /// @param batch  Vector of DbStrategyQuote records.
    void insert_strategy_quotes_batch(const std::vector<DbStrategyQuote>& batch);

    // -- Inventory state (cost-basis persistence) ----------------------------

    /// Upsert the full set of inventory records inside one transaction.
    /// Called by the engine after every fill / seed / reconcile so the
    /// cost basis survives restarts.  Never throws: persistence failures
    /// are logged and must not disrupt live trading.
    void save_inventory_state(const std::vector<DbInventoryState>& records) noexcept;

    /// Load all persisted inventory records.  Returns an empty vector when
    /// the table is empty (first run) or on error.
    [[nodiscard]] std::vector<DbInventoryState> load_inventory_state() const;

    // -- Double-entry ledger -------------------------------------------------

    /// Append ledger legs inside one transaction.  Uses INSERT OR IGNORE on
    /// the (event_id, leg, asset_id) uniqueness key, so re-posting the legs
    /// of an already-recorded event is a no-op rather than a double count.
    /// Never throws: accounting must not disrupt live trading.
    ///
    /// @return number of legs actually inserted (0 = all were duplicates),
    ///         or std::nullopt when the write FAILED.  Callers must
    ///         distinguish these: a dropped fill leg is unrecoverable (the
    ///         fill is never re-processed) and leaves the ledger permanently
    ///         short, which would put the invariant control into a permanent
    ///         false breach.
    [[nodiscard]] std::optional<std::size_t> append_ledger_entries(
        const std::vector<DbLedgerEntry>& legs) noexcept;

    /// Block height recorded on an asset's `opening` leg, or 0 if it has
    /// none.  Fills at or below this height are already reflected in the
    /// opening balance and must not be posted again.
    [[nodiscard]] BlockHeight ledger_opening_block(const AssetId& asset_id) const;

    /// Sum of delta_mojos per asset across the whole ledger, i.e. the
    /// ledger's implied current balance for each asset.
    [[nodiscard]] std::unordered_map<AssetId, Mojo> ledger_balances() const;

    /// True when an 'opening' leg already exists for this asset, so genesis
    /// balances are established exactly once in the ledger's lifetime.
    [[nodiscard]] bool has_ledger_opening(const AssetId& asset_id) const;

    /// Total number of legs (diagnostics / first-run detection).
    [[nodiscard]] std::int64_t ledger_entry_count() const;

    // -- Arbitrage edge history ----------------------------------------------

    /// Record one arbitrage leg-pair observation.  Never throws.
    void insert_arb_edge(const DbArbEdgeObservation& obs) noexcept;

    /// Record a taker trade (we crossed the spread).  Idempotent on
    /// trade_id.  Never throws: accounting must not disrupt trading.
    void insert_taker_fill(const DbTakerFill& fill) noexcept;

    // -- Diagnostics ---------------------------------------------------------

    /// Return the total number of rows in the trade_log table.
    [[nodiscard]] std::int64_t trade_count() const;

    /// Return the total number of rows in the offer_log table.
    [[nodiscard]] std::int64_t offer_count() const;

    /// Return the total number of rows in the snapshots table.
    [[nodiscard]] std::int64_t snapshot_count() const;

    /// Compute fill rate from resolved offers created at or after the given block.
    /// Returns the fraction of resolved offers that were filled (0.0-1.0).
    /// If no resolved offers exist since the block, returns the provided default.
    [[nodiscard]] double fill_rate_since_block(BlockHeight since,
                                               double fallback = 0.30) const;

    /// Query per-tier fill rates for a trading pair over a recent time window.
    /// Returns a vector of length @p max_tiers where element [i] is the fill
    /// rate (filled / total resolved) for tier i.  Tiers with no resolved
    /// offers in the window return 0.0.
    ///
    /// @param pair_name   Trading pair to query (e.g. "XCH/wUSDC.b").
    /// @param cutoff_ts   ISO-8601 UTC lower bound for offer created_at.
    /// @param max_tiers   Number of tier slots to return (typically num_tiers).
    /// @return            Vector of fill rates [0.0, 1.0] per tier index.
    [[nodiscard]]
    std::vector<double> query_tier_fill_rates(const std::string& pair_name,
                                              const std::string& cutoff_ts,
                                              std::uint32_t max_tiers) const;

    /// True if the database connection is open and usable.
    [[nodiscard]] bool is_open() const noexcept;

private:
    // -- Schema management ---------------------------------------------------

    /// Run CREATE TABLE IF NOT EXISTS for all tables and their indices.
    /// Called once from the constructor.
    void run_migrations();

    /// Enable WAL journal mode and configure pragmas for performance.
    void configure_pragmas();

    // -- Prepared statement management ---------------------------------------

    /// Compile a SQL string into a prepared statement.
    /// @param sql  The SQL text.
    /// @return     Owning pointer to the compiled statement.
    /// @throws std::runtime_error on compilation error.
    sqlite3_stmt* prepare(const std::string& sql) const;

    /// Finalise a prepared statement and set the pointer to nullptr.
    static void finalize(sqlite3_stmt*& stmt) noexcept;

    /// Bind a text value to a prepared statement parameter.
    static void bind_text(sqlite3_stmt* stmt, int index, const std::string& val);

    /// Bind a 64-bit integer value to a prepared statement parameter.
    static void bind_int64(sqlite3_stmt* stmt, int index, std::int64_t val);

    /// Bind a double value to a prepared statement parameter.
    static void bind_double(sqlite3_stmt* stmt, int index, double val);

    /// Execute a prepared statement that does not return rows (INSERT/UPDATE).
    /// Resets the statement after execution so it can be reused.
    void step_and_reset(sqlite3_stmt* stmt) const;

    // -- Data members --------------------------------------------------------

    /// Mutex protecting all statement execution.  [T7-01]
    /// While the engine loop is single-stranded, the GUI's DatabaseService
    /// may query from a separate QThread.  The mutex serialises access to
    /// the prepared statements whose sqlite3_step/sqlite3_reset calls
    /// mutate internal state even through const methods.
    mutable std::mutex mtx_;

    /// Filesystem path of the database file (for diagnostics).
    std::string db_path_;

    /// SQLite connection handle.  nullptr after close.
    sqlite3* db_{nullptr};

    // -- Pre-compiled prepared statements (created once, reused) -------------

    /// INSERT INTO trade_log
    sqlite3_stmt* stmt_insert_trade_{nullptr};

    /// SELECT FROM trade_log WHERE pair_name = ? AND timestamp BETWEEN ? AND ?
    sqlite3_stmt* stmt_query_trades_pair_{nullptr};

    /// SELECT FROM trade_log WHERE timestamp BETWEEN ? AND ?
    sqlite3_stmt* stmt_query_trades_all_{nullptr};

    /// INSERT INTO offer_log
    sqlite3_stmt* stmt_insert_offer_{nullptr};

    /// SELECT FROM offer_log WHERE status = 'pending'
    sqlite3_stmt* stmt_query_pending_{nullptr};

    /// SELECT pair_name, status, resolved_block, cancel_reason FROM offer_log
    sqlite3_stmt* stmt_query_offer_status_{nullptr};

    /// UPDATE offer_log SET status = ?, resolved_block = ?, resolved_at = ?
    sqlite3_stmt* stmt_update_offer_{nullptr};

    /// INSERT INTO offer_closure_events
    sqlite3_stmt* stmt_insert_offer_closure_event_{nullptr};

    /// INSERT INTO snapshots
    sqlite3_stmt* stmt_insert_snapshot_{nullptr};

    /// INSERT INTO sanity_failures
    sqlite3_stmt* stmt_insert_sanity_failure_{nullptr};

    /// SELECT FROM snapshots WHERE pair_name = ? ORDER BY block_height DESC LIMIT 1
    sqlite3_stmt* stmt_last_snapshot_{nullptr};

    /// SELECT COUNT(*) FROM trade_log
    sqlite3_stmt* stmt_trade_count_{nullptr};

    /// SELECT COUNT(*) FROM offer_log
    sqlite3_stmt* stmt_offer_count_{nullptr};

    /// SELECT COUNT(*) FROM snapshots
    sqlite3_stmt* stmt_snapshot_count_{nullptr};

    /// Fill rate query: filled / total resolved offers since a given block
    sqlite3_stmt* stmt_fill_rate_{nullptr};

    /// Per-tier fill rate query: filled / total resolved per tier for a pair
    sqlite3_stmt* stmt_tier_fill_rates_{nullptr};

    /// INSERT INTO strategy_quotes (per-tier quote)
    sqlite3_stmt* stmt_insert_strategy_quote_{nullptr};

    // [T8-20] Transaction control prepared statements.
    sqlite3_stmt* stmt_begin_{nullptr};
    sqlite3_stmt* stmt_commit_{nullptr};
    sqlite3_stmt* stmt_rollback_{nullptr};
};

}  // namespace xop

#endif  // XOP_DATABASE_HPP
