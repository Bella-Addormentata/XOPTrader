// database.hpp -- SQLite persistence layer for XOPTrader CHIA DEX market-maker.
//
// Provides crash-safe, append-only storage for the three core data streams:
//
//   1. trade_log   -- every settled fill, forming the authoritative audit trail
//                     (ISO/IEC 27001:2022 Section 15 compliance).
//   2. offer_log   -- full lifecycle of every offer (create -> fill/cancel).
//   3. snapshots   -- periodic per-block market/risk state for analytics.
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

    // -- Strategy quotes (per-tier quote persistence) ------------------------

    /// Insert a batch of per-tier strategy quotes inside a single transaction.
    /// Called from step_update_pnl to persist all tier quotes for the block.
    ///
    /// @param batch  Vector of DbStrategyQuote records.
    void insert_strategy_quotes_batch(const std::vector<DbStrategyQuote>& batch);

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

    /// Run CREATE TABLE IF NOT EXISTS for all three tables and their indices.
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

    /// UPDATE offer_log SET status = ?, resolved_block = ?, resolved_at = ?
    sqlite3_stmt* stmt_update_offer_{nullptr};

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
