// database.cpp -- SQLite persistence layer implementation for XOPTrader.
//
// Schema design rationale:
//   - trade_log is append-only with a UNIQUE constraint on trade_id to prevent
//     duplicate fills from being recorded (idempotent fill processing).
//   - offer_log tracks the full lifecycle: pending -> filled/cancelled/expired.
//   - snapshots are indexed by (pair_name, block_height) for efficient
//     time-series queries by the Grafana dashboards.
//
// Performance:
//   - WAL mode avoids writer-blocking-reader contention.
//   - Prepared statements are compiled once and reused, avoiding repeated
//     SQL parsing on hot paths.
//   - Batch snapshot inserts use explicit BEGIN/COMMIT to amortise fsync.
//   - Synchronous mode is set to NORMAL (not FULL) for a 10x write speed
//     improvement with acceptable durability (WAL protects against corruption;
//     at most one transaction can be lost on a power failure).
//
// Compliant with:
//   ISO/IEC 27001:2022 -- parameterised queries prevent injection
//   ISO/IEC 5055       -- RAII cleanup, checked return codes
//   ISO/IEC 25000      -- single-responsibility, documented schema

#include "xop/database.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <stdexcept>
#include <string>

namespace xop {

// ===========================================================================
// SQL constants -- schema DDL and DML statements
// ===========================================================================

namespace {

// -- Schema DDL (idempotent via IF NOT EXISTS) --------------------------------

constexpr const char* kCreateTradeLog = R"SQL(
CREATE TABLE IF NOT EXISTS trade_log (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp         TEXT    NOT NULL,
    trade_id          TEXT    UNIQUE NOT NULL,
    pair_name         TEXT    NOT NULL,
    side              TEXT    NOT NULL CHECK(side IN ('bid','ask')),
    price_mojos       INTEGER NOT NULL,
    size_mojos        INTEGER NOT NULL,
    fee_mojos         INTEGER NOT NULL DEFAULT 0,
    cost_basis_mojos  INTEGER,
    realized_pnl_mojos INTEGER,
    block_height      INTEGER NOT NULL,
    created_at        TEXT    DEFAULT CURRENT_TIMESTAMP
);
)SQL";

constexpr const char* kCreateOfferLog = R"SQL(
CREATE TABLE IF NOT EXISTS offer_log (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    offer_id        TEXT    UNIQUE NOT NULL,
    pair_name       TEXT    NOT NULL,
    side            TEXT    NOT NULL,
    price_mojos     INTEGER NOT NULL,
    size_mojos      INTEGER NOT NULL,
    tier            INTEGER DEFAULT 0,
    status          TEXT    NOT NULL DEFAULT 'pending',
    created_block   INTEGER NOT NULL,
    resolved_block  INTEGER,
    created_at      TEXT    DEFAULT CURRENT_TIMESTAMP,
    resolved_at     TEXT
);
)SQL";

constexpr const char* kCreateSnapshots = R"SQL(
CREATE TABLE IF NOT EXISTS snapshots (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    block_height     INTEGER NOT NULL,
    pair_name        TEXT    NOT NULL,
    mid_price_mojos  INTEGER,
    spread_bps       REAL,
    inventory_ratio  REAL,
    sigma_block      REAL,
    regime           TEXT,
    pnl_total_mojos  INTEGER,
    created_at       TEXT    DEFAULT CURRENT_TIMESTAMP
);
)SQL";

// -- Index DDL ----------------------------------------------------------------

constexpr const char* kIndexTradeTimestamp = R"SQL(
CREATE INDEX IF NOT EXISTS idx_trade_log_timestamp
    ON trade_log(timestamp);
)SQL";

constexpr const char* kIndexTradePair = R"SQL(
CREATE INDEX IF NOT EXISTS idx_trade_log_pair
    ON trade_log(pair_name, timestamp);
)SQL";

constexpr const char* kIndexOfferStatus = R"SQL(
CREATE INDEX IF NOT EXISTS idx_offer_log_status
    ON offer_log(status);
)SQL";

constexpr const char* kIndexOfferPair = R"SQL(
CREATE INDEX IF NOT EXISTS idx_offer_log_pair
    ON offer_log(pair_name);
)SQL";

constexpr const char* kIndexSnapshotPairBlock = R"SQL(
CREATE INDEX IF NOT EXISTS idx_snapshots_pair_block
    ON snapshots(pair_name, block_height DESC);
)SQL";

// -- DML (INSERT / UPDATE / SELECT) -------------------------------------------

constexpr const char* kInsertTrade = R"SQL(
INSERT INTO trade_log
    (timestamp, trade_id, pair_name, side, price_mojos, size_mojos,
     fee_mojos, cost_basis_mojos, realized_pnl_mojos, block_height)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
)SQL";

constexpr const char* kQueryTradesByPair = R"SQL(
SELECT timestamp, trade_id, pair_name, side, price_mojos, size_mojos,
       fee_mojos, cost_basis_mojos, realized_pnl_mojos, block_height
FROM trade_log
WHERE pair_name = ? AND timestamp >= ? AND timestamp < ?
ORDER BY timestamp ASC;
)SQL";

constexpr const char* kQueryTradesAll = R"SQL(
SELECT timestamp, trade_id, pair_name, side, price_mojos, size_mojos,
       fee_mojos, cost_basis_mojos, realized_pnl_mojos, block_height
FROM trade_log
WHERE timestamp >= ? AND timestamp < ?
ORDER BY timestamp ASC;
)SQL";

constexpr const char* kInsertOffer = R"SQL(
INSERT INTO offer_log
    (offer_id, pair_name, side, price_mojos, size_mojos, tier,
     status, created_block)
VALUES (?, ?, ?, ?, ?, ?, ?, ?);
)SQL";

constexpr const char* kUpdateOfferStatus = R"SQL(
UPDATE offer_log
SET status = ?, resolved_block = ?, resolved_at = CURRENT_TIMESTAMP
WHERE offer_id = ?;
)SQL";

constexpr const char* kInsertSnapshot = R"SQL(
INSERT INTO snapshots
    (block_height, pair_name, mid_price_mojos, spread_bps,
     inventory_ratio, sigma_block, regime, pnl_total_mojos)
VALUES (?, ?, ?, ?, ?, ?, ?, ?);
)SQL";

constexpr const char* kLastSnapshot = R"SQL(
SELECT block_height, pair_name, mid_price_mojos, spread_bps,
       inventory_ratio, sigma_block, regime, pnl_total_mojos
FROM snapshots
WHERE pair_name = ?
ORDER BY block_height DESC
LIMIT 1;
)SQL";

constexpr const char* kTradeCount  = "SELECT COUNT(*) FROM trade_log;";
constexpr const char* kOfferCount  = "SELECT COUNT(*) FROM offer_log;";
constexpr const char* kSnapshotCount = "SELECT COUNT(*) FROM snapshots;";

} // anonymous namespace

// ===========================================================================
// Construction / destruction
// ===========================================================================

Database::Database(const std::string& db_path)
    : db_path_(db_path)
{
    // Open the database file, creating it if it does not exist.
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string msg = "Failed to open database '" + db_path + "': ";
        if (db_) {
            msg += sqlite3_errmsg(db_);
            sqlite3_close(db_);
            db_ = nullptr;
        } else {
            msg += "out of memory";
        }
        throw std::runtime_error(msg);
    }

    spdlog::info("[Database] Opened SQLite database at '{}'", db_path);

    // Configure pragmas (WAL mode, synchronous, cache size).
    configure_pragmas();

    // Create tables and indices if they do not exist.
    run_migrations();

    // Compile all prepared statements once for reuse.
    stmt_insert_trade_       = prepare(kInsertTrade);
    stmt_query_trades_pair_  = prepare(kQueryTradesByPair);
    stmt_query_trades_all_   = prepare(kQueryTradesAll);
    stmt_insert_offer_       = prepare(kInsertOffer);
    stmt_update_offer_       = prepare(kUpdateOfferStatus);
    stmt_insert_snapshot_    = prepare(kInsertSnapshot);
    stmt_last_snapshot_      = prepare(kLastSnapshot);
    stmt_trade_count_        = prepare(kTradeCount);
    stmt_offer_count_        = prepare(kOfferCount);
    stmt_snapshot_count_     = prepare(kSnapshotCount);

    spdlog::info("[Database] Schema migration complete; prepared statements compiled");
}

Database::~Database()
{
    // Finalize all prepared statements before closing the connection.
    finalize(stmt_insert_trade_);
    finalize(stmt_query_trades_pair_);
    finalize(stmt_query_trades_all_);
    finalize(stmt_insert_offer_);
    finalize(stmt_update_offer_);
    finalize(stmt_insert_snapshot_);
    finalize(stmt_last_snapshot_);
    finalize(stmt_trade_count_);
    finalize(stmt_offer_count_);
    finalize(stmt_snapshot_count_);

    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
        spdlog::info("[Database] Closed SQLite database '{}'", db_path_);
    }
}

// ===========================================================================
// Trade log
// ===========================================================================

void Database::insert_trade(const DbTradeRecord& r)
{
    // Bind all 10 parameters to the prepared INSERT statement.
    bind_text  (stmt_insert_trade_, 1, r.timestamp);
    bind_text  (stmt_insert_trade_, 2, r.trade_id);
    bind_text  (stmt_insert_trade_, 3, r.pair_name);
    bind_text  (stmt_insert_trade_, 4, r.side);
    bind_int64 (stmt_insert_trade_, 5, r.price_mojos);
    bind_int64 (stmt_insert_trade_, 6, r.size_mojos);
    bind_int64 (stmt_insert_trade_, 7, r.fee_mojos);
    bind_int64 (stmt_insert_trade_, 8, r.cost_basis_mojos);
    bind_int64 (stmt_insert_trade_, 9, r.realized_pnl_mojos);
    bind_int64 (stmt_insert_trade_, 10, static_cast<std::int64_t>(r.block_height));

    step_and_reset(stmt_insert_trade_);

    spdlog::debug("[Database] Inserted trade '{}' pair={} side={} price={} size={}",
                  r.trade_id, r.pair_name, r.side, r.price_mojos, r.size_mojos);
}

std::vector<DbTradeRecord> Database::query_trades(
    const std::string& pair_name,
    const std::string& start_ts,
    const std::string& end_ts) const
{
    std::vector<DbTradeRecord> results;

    // Choose the appropriate prepared statement based on whether a pair
    // filter was supplied.
    sqlite3_stmt* stmt = pair_name.empty()
        ? stmt_query_trades_all_
        : stmt_query_trades_pair_;

    // Bind parameters.  The pair-filtered statement has 3 params; the
    // unfiltered one has 2.
    if (!pair_name.empty()) {
        bind_text(stmt, 1, pair_name);
        bind_text(stmt, 2, start_ts);
        bind_text(stmt, 3, end_ts);
    } else {
        bind_text(stmt, 1, start_ts);
        bind_text(stmt, 2, end_ts);
    }

    // Step through result rows.
    // ISO/IEC 5055 -- CWE-476: null-guard all sqlite3_column_text results
    // to prevent undefined behaviour when columns contain SQL NULL.
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DbTradeRecord rec;
        // Column indices match the SELECT column order.
        const char* p0 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        rec.timestamp          = p0 ? p0 : "";
        const char* p1 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.trade_id           = p1 ? p1 : "";
        const char* p2 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        rec.pair_name          = p2 ? p2 : "";
        const char* p3 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        rec.side               = p3 ? p3 : "";
        rec.price_mojos        = sqlite3_column_int64(stmt, 4);
        rec.size_mojos         = sqlite3_column_int64(stmt, 5);
        rec.fee_mojos          = sqlite3_column_int64(stmt, 6);
        rec.cost_basis_mojos   = sqlite3_column_int64(stmt, 7);
        rec.realized_pnl_mojos = sqlite3_column_int64(stmt, 8);
        rec.block_height       = static_cast<BlockHeight>(sqlite3_column_int64(stmt, 9));
        results.push_back(std::move(rec));
    }

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    return results;
}

// ===========================================================================
// Offer log
// ===========================================================================

void Database::insert_offer(const DbOfferRecord& r)
{
    bind_text  (stmt_insert_offer_, 1, r.offer_id);
    bind_text  (stmt_insert_offer_, 2, r.pair_name);
    bind_text  (stmt_insert_offer_, 3, r.side);
    bind_int64 (stmt_insert_offer_, 4, r.price_mojos);
    bind_int64 (stmt_insert_offer_, 5, r.size_mojos);
    bind_int64 (stmt_insert_offer_, 6, static_cast<std::int64_t>(r.tier));
    bind_text  (stmt_insert_offer_, 7, r.status);
    bind_int64 (stmt_insert_offer_, 8, static_cast<std::int64_t>(r.created_block));

    step_and_reset(stmt_insert_offer_);

    spdlog::debug("[Database] Inserted offer '{}' pair={} side={} tier={}",
                  r.offer_id, r.pair_name, r.side, r.tier);
}

void Database::update_offer_status(const std::string& offer_id,
                                   const std::string& new_status,
                                   BlockHeight        resolved_block)
{
    bind_text  (stmt_update_offer_, 1, new_status);
    bind_int64 (stmt_update_offer_, 2, static_cast<std::int64_t>(resolved_block));
    bind_text  (stmt_update_offer_, 3, offer_id);

    step_and_reset(stmt_update_offer_);

    // Verify that at least one row was modified.
    int changes = sqlite3_changes(db_);
    if (changes == 0) {
        throw std::runtime_error(
            "[Database] update_offer_status: no offer found with id '" + offer_id + "'");
    }

    spdlog::debug("[Database] Updated offer '{}' -> status='{}' resolved_block={}",
                  offer_id, new_status, resolved_block);
}

// ===========================================================================
// Snapshots
// ===========================================================================

void Database::insert_snapshot(const DbSnapshot& s)
{
    bind_int64 (stmt_insert_snapshot_, 1, static_cast<std::int64_t>(s.block_height));
    bind_text  (stmt_insert_snapshot_, 2, s.pair_name);
    bind_int64 (stmt_insert_snapshot_, 3, s.mid_price_mojos);
    bind_double(stmt_insert_snapshot_, 4, s.spread_bps);
    bind_double(stmt_insert_snapshot_, 5, s.inventory_ratio);
    bind_double(stmt_insert_snapshot_, 6, s.sigma_block);
    bind_text  (stmt_insert_snapshot_, 7, s.regime);
    bind_int64 (stmt_insert_snapshot_, 8, s.pnl_total_mojos);

    step_and_reset(stmt_insert_snapshot_);
}

void Database::insert_snapshots_batch(const std::vector<DbSnapshot>& batch)
{
    if (batch.empty()) {
        return;
    }

    // Wrap the batch in an explicit transaction to amortise fsync.
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string msg = "[Database] BEGIN failed: ";
        if (err_msg) { msg += err_msg; sqlite3_free(err_msg); }
        throw std::runtime_error(msg);
    }

    try {
        for (const auto& s : batch) {
            insert_snapshot(s);
        }
    } catch (...) {
        // Roll back on any failure to maintain atomicity.
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }

    rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string msg = "[Database] COMMIT failed: ";
        if (err_msg) { msg += err_msg; sqlite3_free(err_msg); }
        // Attempt rollback after failed commit.
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw std::runtime_error(msg);
    }

    spdlog::debug("[Database] Batch-inserted {} snapshots", batch.size());
}

std::optional<DbSnapshot> Database::get_last_snapshot(
    const std::string& pair_name) const
{
    bind_text(stmt_last_snapshot_, 1, pair_name);

    int rc = sqlite3_step(stmt_last_snapshot_);
    std::optional<DbSnapshot> result;

    if (rc == SQLITE_ROW) {
        DbSnapshot s;
        s.block_height    = static_cast<BlockHeight>(sqlite3_column_int64(stmt_last_snapshot_, 0));
        // ISO/IEC 5055 -- CWE-476: null-guard sqlite3_column_text result.
        const char* pn = reinterpret_cast<const char*>(sqlite3_column_text(stmt_last_snapshot_, 1));
        s.pair_name       = pn ? pn : "";
        s.mid_price_mojos = sqlite3_column_int64(stmt_last_snapshot_, 2);
        s.spread_bps      = sqlite3_column_double(stmt_last_snapshot_, 3);
        s.inventory_ratio = sqlite3_column_double(stmt_last_snapshot_, 4);
        s.sigma_block     = sqlite3_column_double(stmt_last_snapshot_, 5);

        const auto* regime_text = sqlite3_column_text(stmt_last_snapshot_, 6);
        if (regime_text) {
            s.regime = reinterpret_cast<const char*>(regime_text);
        }

        s.pnl_total_mojos = sqlite3_column_int64(stmt_last_snapshot_, 7);
        result = std::move(s);
    }

    sqlite3_reset(stmt_last_snapshot_);
    sqlite3_clear_bindings(stmt_last_snapshot_);

    return result;
}

// ===========================================================================
// Diagnostics
// ===========================================================================

std::int64_t Database::trade_count() const
{
    sqlite3_step(stmt_trade_count_);
    std::int64_t count = sqlite3_column_int64(stmt_trade_count_, 0);
    sqlite3_reset(stmt_trade_count_);
    return count;
}

std::int64_t Database::offer_count() const
{
    sqlite3_step(stmt_offer_count_);
    std::int64_t count = sqlite3_column_int64(stmt_offer_count_, 0);
    sqlite3_reset(stmt_offer_count_);
    return count;
}

std::int64_t Database::snapshot_count() const
{
    sqlite3_step(stmt_snapshot_count_);
    std::int64_t count = sqlite3_column_int64(stmt_snapshot_count_, 0);
    sqlite3_reset(stmt_snapshot_count_);
    return count;
}

bool Database::is_open() const noexcept
{
    return db_ != nullptr;
}

// ===========================================================================
// Private -- schema and pragmas
// ===========================================================================

void Database::configure_pragmas()
{
    char* err_msg = nullptr;

    // WAL mode: allows concurrent readers while a single writer operates.
    int rc = sqlite3_exec(db_, "PRAGMA journal_mode=WAL;",
                          nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string msg = "[Database] PRAGMA journal_mode=WAL failed: ";
        if (err_msg) { msg += err_msg; sqlite3_free(err_msg); }
        spdlog::warn("{}", msg);
        // Non-fatal: WAL is a performance optimisation, not required.
    }

    // NORMAL synchronous: fsync only at critical moments (checkpoint).
    // Acceptable trade-off: at most one transaction lost on power failure,
    // but no database corruption.
    rc = sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;",
                      nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK && err_msg) {
        spdlog::warn("[Database] PRAGMA synchronous=NORMAL failed: {}", err_msg);
        sqlite3_free(err_msg);
    }

    // Increase default page cache to 10 MB (-10000 pages * 1024 bytes).
    rc = sqlite3_exec(db_, "PRAGMA cache_size=-10000;",
                      nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK && err_msg) {
        spdlog::warn("[Database] PRAGMA cache_size failed: {}", err_msg);
        sqlite3_free(err_msg);
    }

    // Enable foreign keys (not strictly needed here but good practice).
    rc = sqlite3_exec(db_, "PRAGMA foreign_keys=ON;",
                      nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK && err_msg) {
        spdlog::warn("[Database] PRAGMA foreign_keys=ON failed: {}", err_msg);
        sqlite3_free(err_msg);
    }

    spdlog::debug("[Database] Pragmas configured (WAL, NORMAL sync, 10MB cache)");
}

void Database::run_migrations()
{
    char* err_msg = nullptr;

    // Execute each DDL statement.  Order matters: tables before indices.
    const char* ddl_statements[] = {
        kCreateTradeLog,
        kCreateOfferLog,
        kCreateSnapshots,
        kIndexTradeTimestamp,
        kIndexTradePair,
        kIndexOfferStatus,
        kIndexOfferPair,
        kIndexSnapshotPairBlock
    };

    for (const char* ddl : ddl_statements) {
        int rc = sqlite3_exec(db_, ddl, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string msg = "[Database] Migration failed: ";
            if (err_msg) { msg += err_msg; sqlite3_free(err_msg); }
            throw std::runtime_error(msg);
        }
    }
}

// ===========================================================================
// Private -- prepared statement helpers
// ===========================================================================

sqlite3_stmt* Database::prepare(const std::string& sql) const
{
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(),
                                static_cast<int>(sql.size()),
                                &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        std::string msg = "[Database] Failed to prepare: ";
        msg += sqlite3_errmsg(db_);
        msg += " | SQL: " + sql;
        throw std::runtime_error(msg);
    }
    return stmt;
}

void Database::finalize(sqlite3_stmt*& stmt) noexcept
{
    if (stmt) {
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
}

void Database::bind_text(sqlite3_stmt* stmt, int index, const std::string& val)
{
    int rc = sqlite3_bind_text(stmt, index, val.c_str(),
                               static_cast<int>(val.size()),
                               SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("[Database] bind_text failed at index " +
                                 std::to_string(index));
    }
}

void Database::bind_int64(sqlite3_stmt* stmt, int index, std::int64_t val)
{
    int rc = sqlite3_bind_int64(stmt, index, val);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("[Database] bind_int64 failed at index " +
                                 std::to_string(index));
    }
}

void Database::bind_double(sqlite3_stmt* stmt, int index, double val)
{
    int rc = sqlite3_bind_double(stmt, index, val);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("[Database] bind_double failed at index " +
                                 std::to_string(index));
    }
}

void Database::step_and_reset(sqlite3_stmt* stmt) const
{
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string msg = "[Database] step failed: ";
        msg += sqlite3_errmsg(db_);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        throw std::runtime_error(msg);
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

}  // namespace xop
