// database.cpp -- SQLite persistence layer implementation for XOPTrader.
//
// Schema design rationale:
//   - trade_log is append-only with a UNIQUE constraint on trade_id to prevent
//     duplicate fills from being recorded (idempotent fill processing).
//   - offer_log tracks the current lifecycle state of each offer.
//   - offer_closure_events preserves append-only resolution history so later
//     reconciliation does not overwrite the original cancel cause.
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

bool is_reconcile_reason(const std::string& reason)
{
    return reason == "periodic_reconcile"
        || reason == "on_chain_reconcile"
        || reason == "reconnect_reconcile";
}

bool is_terminal_status(const std::string& status)
{
    return status == "filled"
        || status == "cancelled"
        || status == "expired";
}

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
    offer_hash        TEXT,
    acquisition_ts    TEXT,
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
    competitiveness_score INTEGER DEFAULT 0,
    queue_ahead_mojos INTEGER DEFAULT 0,
    queue_ahead_score INTEGER DEFAULT 0,
    execution_quality_score INTEGER DEFAULT 0,
    status          TEXT    NOT NULL DEFAULT 'pending',
    created_block   INTEGER NOT NULL,
    resolved_block  INTEGER,
    fee_mojos       INTEGER DEFAULT 0,
    cancel_reason   TEXT,
    book_best_bid   INTEGER DEFAULT 0,
    book_best_ask   INTEGER DEFAULT 0,
    created_at      TEXT    DEFAULT CURRENT_TIMESTAMP,
    resolved_at     TEXT
);
)SQL";

constexpr const char* kCreateOfferClosureEvents = R"SQL(
CREATE TABLE IF NOT EXISTS offer_closure_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    offer_id        TEXT    NOT NULL,
    pair_name       TEXT    NOT NULL,
    event_type      TEXT    NOT NULL,
    previous_status TEXT,
    observed_status TEXT    NOT NULL,
    closure_reason  TEXT,
    resolved_block  INTEGER,
    created_at      TEXT    DEFAULT CURRENT_TIMESTAMP
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
    xch_usd_rate     REAL    DEFAULT 0,
    pnl_total_usd    REAL    DEFAULT 0,
    created_at       TEXT    DEFAULT CURRENT_TIMESTAMP
);
)SQL";

constexpr const char* kCreateStrategyQuotes = R"SQL(
CREATE TABLE IF NOT EXISTS strategy_quotes (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    block_height    INTEGER NOT NULL,
    pair_name       TEXT    NOT NULL,
    tier            INTEGER NOT NULL,
    side            TEXT    NOT NULL CHECK(side IN ('bid','ask')),
    price_mojos     INTEGER NOT NULL,
    size_mojos      INTEGER NOT NULL,
    created_at      TEXT    DEFAULT CURRENT_TIMESTAMP
);
)SQL";

constexpr const char* kCreateSanityFailures = R"SQL(
CREATE TABLE IF NOT EXISTS sanity_failures (
    id                     INTEGER PRIMARY KEY AUTOINCREMENT,
    block_height           INTEGER NOT NULL,
    pair_name              TEXT    NOT NULL,
    side                   TEXT    NOT NULL CHECK(side IN ('bid','ask')),
    tier                   INTEGER NOT NULL,
    proposed_price_mojos   INTEGER NOT NULL,
    reference_price_mojos  INTEGER NOT NULL,
    deviation_pct          REAL    NOT NULL,
    failure_reason         TEXT    NOT NULL,
    details                TEXT,
    created_at             TEXT    DEFAULT CURRENT_TIMESTAMP
);
)SQL";

constexpr const char* kCreateInventoryState = R"SQL(
CREATE TABLE IF NOT EXISTS inventory_state (
    asset_id                TEXT    PRIMARY KEY,
    total_quantity          INTEGER NOT NULL,
    total_cost              REAL    NOT NULL,
    basis_is_seed_sentinel  INTEGER NOT NULL DEFAULT 0,
    updated_at              TEXT    DEFAULT CURRENT_TIMESTAMP
);
)SQL";

constexpr const char* kCreateLedgerEntries = R"SQL(
CREATE TABLE IF NOT EXISTS ledger_entries (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    entry_time    TEXT    NOT NULL,
    event_type    TEXT    NOT NULL,
    event_id      TEXT    NOT NULL,
    leg           TEXT    NOT NULL,
    asset_id      TEXT    NOT NULL,
    delta_mojos   INTEGER NOT NULL,
    pair_name     TEXT,
    block_height  INTEGER,
    note          TEXT,
    created_at    TEXT    DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(event_id, leg, asset_id)
);
)SQL";

constexpr const char* kCreateArbEdgeLog = R"SQL(
CREATE TABLE IF NOT EXISTS arb_edge_log (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    observed_at    TEXT    NOT NULL,
    block_height   INTEGER NOT NULL,
    direction      TEXT    NOT NULL,
    ask_a_mojos    INTEGER NOT NULL,
    bid_b_mojos    INTEGER NOT NULL,
    cross_rate     REAL    NOT NULL,
    gross_edge_bps REAL    NOT NULL,
    net_edge_bps   REAL    NOT NULL,
    ask_size_mojos INTEGER NOT NULL,
    bid_size_mojos INTEGER NOT NULL,
    state          TEXT    NOT NULL,
    armed          INTEGER NOT NULL DEFAULT 0,
    executed       INTEGER NOT NULL DEFAULT 0,
    created_at     TEXT    DEFAULT CURRENT_TIMESTAMP
);
)SQL";

constexpr const char* kIndexArbEdgeDirTime = R"SQL(
CREATE INDEX IF NOT EXISTS idx_arb_edge_log_dir_time
    ON arb_edge_log(direction, observed_at);
)SQL";

constexpr const char* kIndexLedgerAsset = R"SQL(
CREATE INDEX IF NOT EXISTS idx_ledger_entries_asset
    ON ledger_entries(asset_id);
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

constexpr const char* kIndexOfferClosureEventsOffer = R"SQL(
CREATE INDEX IF NOT EXISTS idx_offer_closure_events_offer
    ON offer_closure_events(offer_id, id);
)SQL";

constexpr const char* kIndexOfferClosureEventsPair = R"SQL(
CREATE INDEX IF NOT EXISTS idx_offer_closure_events_pair
    ON offer_closure_events(pair_name, id DESC);
)SQL";

constexpr const char* kIndexSnapshotPairBlock = R"SQL(
CREATE INDEX IF NOT EXISTS idx_snapshots_pair_block
    ON snapshots(pair_name, block_height DESC);
)SQL";

constexpr const char* kIndexStrategyQuotesPairBlock = R"SQL(
CREATE INDEX IF NOT EXISTS idx_strategy_quotes_pair_block
    ON strategy_quotes(pair_name, block_height DESC);
)SQL";

constexpr const char* kIndexSanityFailuresPair = R"SQL(
CREATE INDEX IF NOT EXISTS idx_sanity_failures_pair
    ON sanity_failures(pair_name, block_height DESC);
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
    competitiveness_score, queue_ahead_mojos, queue_ahead_score,
    execution_quality_score, status, created_block, fee_mojos,
    book_best_bid, book_best_ask)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
)SQL";

constexpr const char* kQueryPendingOffers = R"SQL(
SELECT offer_id, pair_name, side, price_mojos, size_mojos, tier,
       status, created_block, fee_mojos
FROM offer_log
WHERE status = 'pending'
ORDER BY created_block ASC;
)SQL";

constexpr const char* kQueryOfferStatus = R"SQL(
SELECT pair_name,
       status,
       COALESCE(resolved_block, 0),
       COALESCE(cancel_reason, '')
FROM offer_log
WHERE offer_id = ?
LIMIT 1;
)SQL";

constexpr const char* kUpdateOfferStatus = R"SQL(
UPDATE offer_log
SET status = ?, resolved_block = ?, resolved_at = CURRENT_TIMESTAMP, cancel_reason = ?
WHERE offer_id = ?;
)SQL";

constexpr const char* kInsertOfferClosureEvent = R"SQL(
INSERT INTO offer_closure_events
    (offer_id, pair_name, event_type, previous_status, observed_status,
     closure_reason, resolved_block)
VALUES (?, ?, ?, ?, ?, ?, ?);
)SQL";

constexpr const char* kInsertSnapshot = R"SQL(
INSERT INTO snapshots
    (block_height, pair_name, mid_price_mojos, spread_bps,
    inventory_ratio, sigma_block, regime, pnl_total_mojos,
    xch_usd_rate, pnl_total_usd,
     reservation_price_mojos, half_spread_bps, kappa,
     variance_ratio, adverse_rate, s_adverse_bps,
     s_inventory_bps, s_cost_bps)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
)SQL";

constexpr const char* kInsertStrategyQuote = R"SQL(
INSERT INTO strategy_quotes
    (block_height, pair_name, tier, side, price_mojos, size_mojos)
VALUES (?, ?, ?, ?, ?, ?);
)SQL";

constexpr const char* kInsertSanityFailure = R"SQL(
INSERT INTO sanity_failures
    (block_height, pair_name, side, tier, proposed_price_mojos,
     reference_price_mojos, deviation_pct, failure_reason, details)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
)SQL";

constexpr const char* kLastSnapshot = R"SQL(
SELECT block_height, pair_name, mid_price_mojos, spread_bps,
    inventory_ratio, sigma_block, regime, pnl_total_mojos,
    xch_usd_rate, pnl_total_usd,
       reservation_price_mojos, half_spread_bps, kappa,
       variance_ratio, adverse_rate, s_adverse_bps,
       s_inventory_bps, s_cost_bps
FROM snapshots
WHERE pair_name = ?
ORDER BY block_height DESC
LIMIT 1;
)SQL";

constexpr const char* kTradeCount  = "SELECT COUNT(*) FROM trade_log;";
constexpr const char* kOfferCount  = "SELECT COUNT(*) FROM offer_log;";
constexpr const char* kSnapshotCount = "SELECT COUNT(*) FROM snapshots;";

constexpr const char* kFillRateSinceBlock = R"SQL(
SELECT
    COALESCE(SUM(CASE WHEN status = 'filled' THEN 1 ELSE 0 END), 0),
    COUNT(*)
FROM offer_log
WHERE created_block >= ? AND status IN ('filled', 'cancelled', 'expired');
)SQL";

constexpr const char* kTierFillRates = R"SQL(
SELECT tier,
       SUM(CASE WHEN status = 'filled' THEN 1 ELSE 0 END),
       COUNT(*)
FROM offer_log
WHERE pair_name = ?
  AND status IN ('filled', 'cancelled', 'expired')
  AND created_at >= ?
GROUP BY tier
ORDER BY tier ASC;
)SQL";

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
    stmt_query_pending_      = prepare(kQueryPendingOffers);
    stmt_query_offer_status_ = prepare(kQueryOfferStatus);
    stmt_update_offer_       = prepare(kUpdateOfferStatus);
    stmt_insert_offer_closure_event_ = prepare(kInsertOfferClosureEvent);
    stmt_insert_snapshot_    = prepare(kInsertSnapshot);
    stmt_last_snapshot_      = prepare(kLastSnapshot);
    stmt_trade_count_        = prepare(kTradeCount);
    stmt_offer_count_        = prepare(kOfferCount);
    stmt_snapshot_count_     = prepare(kSnapshotCount);
    stmt_fill_rate_          = prepare(kFillRateSinceBlock);
    stmt_tier_fill_rates_    = prepare(kTierFillRates);
    stmt_insert_strategy_quote_ = prepare(kInsertStrategyQuote);
    stmt_insert_sanity_failure_ = prepare(kInsertSanityFailure);

    // [T8-20] Transaction control prepared statements.
    stmt_begin_    = prepare("BEGIN TRANSACTION");
    stmt_commit_   = prepare("COMMIT");
    stmt_rollback_ = prepare("ROLLBACK");

    spdlog::info("[Database] Schema migration complete; prepared statements compiled");
}

Database::~Database()
{
    // Finalize all prepared statements before closing the connection.
    finalize(stmt_insert_trade_);
    finalize(stmt_query_trades_pair_);
    finalize(stmt_query_trades_all_);
    finalize(stmt_insert_offer_);
    finalize(stmt_query_pending_);
    finalize(stmt_query_offer_status_);
    finalize(stmt_update_offer_);
    finalize(stmt_insert_offer_closure_event_);
    finalize(stmt_insert_snapshot_);
    finalize(stmt_last_snapshot_);
    finalize(stmt_trade_count_);
    finalize(stmt_offer_count_);
    finalize(stmt_snapshot_count_);
    finalize(stmt_fill_rate_);
    finalize(stmt_tier_fill_rates_);
    finalize(stmt_insert_strategy_quote_);
    finalize(stmt_begin_);
    finalize(stmt_commit_);
    finalize(stmt_rollback_);

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
    std::lock_guard<std::mutex> lock(mtx_);
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
    std::lock_guard<std::mutex> lock(mtx_);
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
    std::lock_guard<std::mutex> lock(mtx_);
    bind_text  (stmt_insert_offer_, 1, r.offer_id);
    bind_text  (stmt_insert_offer_, 2, r.pair_name);
    bind_text  (stmt_insert_offer_, 3, r.side);
    bind_int64 (stmt_insert_offer_, 4, r.price_mojos);
    bind_int64 (stmt_insert_offer_, 5, r.size_mojos);
    bind_int64 (stmt_insert_offer_, 6, static_cast<std::int64_t>(r.tier));
    bind_int64 (stmt_insert_offer_, 7, static_cast<std::int64_t>(r.competitiveness_score));
    bind_int64 (stmt_insert_offer_, 8, static_cast<std::int64_t>(r.queue_ahead_mojos));
    bind_int64 (stmt_insert_offer_, 9, static_cast<std::int64_t>(r.queue_ahead_score));
    bind_int64 (stmt_insert_offer_, 10, static_cast<std::int64_t>(r.execution_quality_score));
    bind_text  (stmt_insert_offer_, 11, r.status);
    bind_int64 (stmt_insert_offer_, 12, static_cast<std::int64_t>(r.created_block));
    bind_int64 (stmt_insert_offer_, 13, static_cast<std::int64_t>(r.fee_mojos));
    bind_int64 (stmt_insert_offer_, 14, static_cast<std::int64_t>(r.book_best_bid));
    bind_int64 (stmt_insert_offer_, 15, static_cast<std::int64_t>(r.book_best_ask));

    step_and_reset(stmt_insert_offer_);

    spdlog::debug("[Database] Inserted offer '{}' pair={} side={} tier={} fee={} competitiveness={} queue_ahead={} queue_score={} execution_quality={}",
                  r.offer_id, r.pair_name, r.side, r.tier, r.fee_mojos,
                  r.competitiveness_score, r.queue_ahead_mojos,
                  r.queue_ahead_score, r.execution_quality_score);
}

std::vector<DbOfferRecord> Database::query_pending_offers() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<DbOfferRecord> results;

    while (sqlite3_step(stmt_query_pending_) == SQLITE_ROW) {
        DbOfferRecord rec;
        const char* p0 = reinterpret_cast<const char*>(sqlite3_column_text(stmt_query_pending_, 0));
        rec.offer_id      = p0 ? p0 : "";
        const char* p1 = reinterpret_cast<const char*>(sqlite3_column_text(stmt_query_pending_, 1));
        rec.pair_name     = p1 ? p1 : "";
        const char* p2 = reinterpret_cast<const char*>(sqlite3_column_text(stmt_query_pending_, 2));
        rec.side          = p2 ? p2 : "";
        rec.price_mojos   = sqlite3_column_int64(stmt_query_pending_, 3);
        rec.size_mojos    = sqlite3_column_int64(stmt_query_pending_, 4);
        rec.tier          = static_cast<int>(sqlite3_column_int64(stmt_query_pending_, 5));
        // Column 6 is `status` (always the literal 'pending' given the WHERE
        // clause), 7 is created_block, 8 is fee_mojos.
        //
        // [FEE-INDEX-FIX 2026-07-30] These two used to read columns 6 and 7,
        // i.e. created_block was parsed from the TEXT 'pending' (yielding 0)
        // and fee_mojos was actually the BLOCK HEIGHT (~9e6).  Harmless while
        // the restored fee was unused, but FEE-FIX now propagates a restored
        // offer's fee into trade_log.fee_mojos, which would have booked
        // block heights as fees for every fill that settled during downtime.
        rec.status        = "pending";
        rec.created_block = static_cast<BlockHeight>(sqlite3_column_int64(stmt_query_pending_, 7));
        rec.fee_mojos     = static_cast<std::uint64_t>(sqlite3_column_int64(stmt_query_pending_, 8));
        results.push_back(std::move(rec));
    }

    sqlite3_reset(stmt_query_pending_);
    sqlite3_clear_bindings(stmt_query_pending_);

    return results;
}

void Database::update_offer_status(const std::string& offer_id,
                                   const std::string& new_status,
                                   BlockHeight        resolved_block,
                                   const std::string& cancel_reason)
{
    std::lock_guard<std::mutex> lock(mtx_);
    bind_text(stmt_query_offer_status_, 1, offer_id);

    int rc = sqlite3_step(stmt_query_offer_status_);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(stmt_query_offer_status_);
        sqlite3_clear_bindings(stmt_query_offer_status_);
        throw std::runtime_error(
            "[Database] update_offer_status: no offer found with id '" + offer_id + "'");
    }

    const char* pair_name_text = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt_query_offer_status_, 0));
    const char* current_status_text = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt_query_offer_status_, 1));
    const auto current_resolved_block = static_cast<BlockHeight>(
        sqlite3_column_int64(stmt_query_offer_status_, 2));
    const char* current_reason_text = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt_query_offer_status_, 3));

    const std::string pair_name = pair_name_text ? pair_name_text : "";
    const std::string current_status = current_status_text ? current_status_text : "";
    const std::string current_reason = current_reason_text ? current_reason_text : "";

    sqlite3_reset(stmt_query_offer_status_);
    sqlite3_clear_bindings(stmt_query_offer_status_);

    std::string stored_status = current_status;
    BlockHeight stored_resolved_block = current_resolved_block;
    std::string stored_reason = current_reason;
    bool should_update_offer_row = false;

    std::string event_type = "status_observation";
    const bool new_reason_is_reconcile = is_reconcile_reason(cancel_reason);
    const bool current_reason_is_reconcile = is_reconcile_reason(current_reason);

    if (current_status == "filled") {
        event_type = new_reason_is_reconcile
            ? "reconcile_observation"
            : "status_observation";
    } else if (new_status == "filled") {
        stored_status = new_status;
        stored_resolved_block = resolved_block;
        stored_reason.clear();
        should_update_offer_row = true;
        event_type = "status_update";
    } else if (!is_terminal_status(current_status)) {
        stored_status = new_status;
        stored_resolved_block = resolved_block;
        stored_reason = cancel_reason;
        should_update_offer_row = true;
        event_type = "status_update";
    } else if (current_status == new_status
               && (new_status == "cancelled" || new_status == "expired")) {
        if (stored_reason.empty() && !cancel_reason.empty()) {
            stored_reason = cancel_reason;
            should_update_offer_row = true;
            event_type = "status_update";
        } else if (current_reason_is_reconcile
                   && !new_reason_is_reconcile
                   && !cancel_reason.empty()) {
            stored_reason = cancel_reason;
            should_update_offer_row = true;
            event_type = "status_update";
        } else if (stored_resolved_block == 0 && resolved_block != 0) {
            stored_resolved_block = resolved_block;
            should_update_offer_row = true;
            event_type = "status_update";
        } else {
            event_type = new_reason_is_reconcile
                ? "reconcile_observation"
                : "status_observation";
        }

        if (should_update_offer_row && current_resolved_block != 0) {
            stored_resolved_block = current_resolved_block;
        }
    } else {
        event_type = new_reason_is_reconcile
            ? "reconcile_observation"
            : "status_observation";
    }

    if (should_update_offer_row) {
        bind_text  (stmt_update_offer_, 1, stored_status);
        bind_int64 (stmt_update_offer_, 2, static_cast<std::int64_t>(stored_resolved_block));
        bind_text  (stmt_update_offer_, 3, stored_reason);
        bind_text  (stmt_update_offer_, 4, offer_id);

        step_and_reset(stmt_update_offer_);

        int changes = sqlite3_changes(db_);
        if (changes == 0) {
            throw std::runtime_error(
                "[Database] update_offer_status: no offer found with id '" + offer_id + "'");
        }
    }

    bind_text  (stmt_insert_offer_closure_event_, 1, offer_id);
    bind_text  (stmt_insert_offer_closure_event_, 2, pair_name);
    bind_text  (stmt_insert_offer_closure_event_, 3, event_type);
    bind_text  (stmt_insert_offer_closure_event_, 4, current_status);
    bind_text  (stmt_insert_offer_closure_event_, 5, new_status);
    bind_text  (stmt_insert_offer_closure_event_, 6, cancel_reason);
    bind_int64 (stmt_insert_offer_closure_event_, 7, static_cast<std::int64_t>(resolved_block));
    step_and_reset(stmt_insert_offer_closure_event_);

    spdlog::debug("[Database] Offer '{}' status='{}' -> '{}' event='{}' "
                  "stored_reason='{}' observed_reason='{}'",
                  offer_id, current_status, stored_status, event_type,
                  stored_reason, cancel_reason);
}

// ===========================================================================
// Snapshots
// ===========================================================================

void Database::insert_snapshot(const DbSnapshot& s)
{
    std::lock_guard<std::mutex> lock(mtx_);
    bind_int64 (stmt_insert_snapshot_, 1, static_cast<std::int64_t>(s.block_height));
    bind_text  (stmt_insert_snapshot_, 2, s.pair_name);
    bind_int64 (stmt_insert_snapshot_, 3, s.mid_price_mojos);
    bind_double(stmt_insert_snapshot_, 4, s.spread_bps);
    bind_double(stmt_insert_snapshot_, 5, s.inventory_ratio);
    bind_double(stmt_insert_snapshot_, 6, s.sigma_block);
    bind_text  (stmt_insert_snapshot_, 7, s.regime);
    bind_int64 (stmt_insert_snapshot_, 8, s.pnl_total_mojos);
    bind_double(stmt_insert_snapshot_, 9, s.xch_usd_rate);
    bind_double(stmt_insert_snapshot_, 10, s.pnl_total_usd);
    bind_int64 (stmt_insert_snapshot_, 11, s.reservation_price_mojos);
    bind_double(stmt_insert_snapshot_, 12, s.half_spread_bps);
    bind_double(stmt_insert_snapshot_, 13, s.kappa);
    bind_double(stmt_insert_snapshot_, 14, s.variance_ratio);
    bind_double(stmt_insert_snapshot_, 15, s.adverse_rate);
    bind_double(stmt_insert_snapshot_, 16, s.s_adverse_bps);
    bind_double(stmt_insert_snapshot_, 17, s.s_inventory_bps);
    bind_double(stmt_insert_snapshot_, 18, s.s_cost_bps);

    step_and_reset(stmt_insert_snapshot_);
}

void Database::insert_snapshots_batch(const std::vector<DbSnapshot>& batch)
{
    if (batch.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    // Wrap the batch in an explicit transaction to amortise fsync.
    // [T8-20] Use prepared statements instead of sqlite3_exec().
    step_and_reset(stmt_begin_);

    try {
        for (const auto& s : batch) {
            // Inline the snapshot insert to avoid recursive lock on mtx_.
            bind_int64 (stmt_insert_snapshot_, 1, static_cast<std::int64_t>(s.block_height));
            bind_text  (stmt_insert_snapshot_, 2, s.pair_name);
            bind_int64 (stmt_insert_snapshot_, 3, s.mid_price_mojos);
            bind_double(stmt_insert_snapshot_, 4, s.spread_bps);
            bind_double(stmt_insert_snapshot_, 5, s.inventory_ratio);
            bind_double(stmt_insert_snapshot_, 6, s.sigma_block);
            bind_text  (stmt_insert_snapshot_, 7, s.regime);
            bind_int64 (stmt_insert_snapshot_, 8, s.pnl_total_mojos);
            bind_double(stmt_insert_snapshot_, 9, s.xch_usd_rate);
            bind_double(stmt_insert_snapshot_, 10, s.pnl_total_usd);
            bind_int64 (stmt_insert_snapshot_, 11, s.reservation_price_mojos);
            bind_double(stmt_insert_snapshot_, 12, s.half_spread_bps);
            bind_double(stmt_insert_snapshot_, 13, s.kappa);
            bind_double(stmt_insert_snapshot_, 14, s.variance_ratio);
            bind_double(stmt_insert_snapshot_, 15, s.adverse_rate);
            bind_double(stmt_insert_snapshot_, 16, s.s_adverse_bps);
            bind_double(stmt_insert_snapshot_, 17, s.s_inventory_bps);
            bind_double(stmt_insert_snapshot_, 18, s.s_cost_bps);
            step_and_reset(stmt_insert_snapshot_);
        }
    } catch (...) {
        // Roll back on any failure to maintain atomicity.
        sqlite3_reset(stmt_rollback_);
        sqlite3_step(stmt_rollback_);
        sqlite3_reset(stmt_rollback_);
        throw;
    }

    step_and_reset(stmt_commit_);

    spdlog::debug("[Database] Batch-inserted {} snapshots", batch.size());
}

void Database::insert_strategy_quotes_batch(const std::vector<DbStrategyQuote>& batch)
{
    if (batch.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    step_and_reset(stmt_begin_);

    try {
        for (const auto& q : batch) {
            bind_int64(stmt_insert_strategy_quote_, 1, static_cast<std::int64_t>(q.block_height));
            bind_text (stmt_insert_strategy_quote_, 2, q.pair_name);
            bind_int64(stmt_insert_strategy_quote_, 3, static_cast<std::int64_t>(q.tier));
            bind_text (stmt_insert_strategy_quote_, 4, q.side);
            bind_int64(stmt_insert_strategy_quote_, 5, q.price_mojos);
            bind_int64(stmt_insert_strategy_quote_, 6, q.size_mojos);
            step_and_reset(stmt_insert_strategy_quote_);
        }
    } catch (...) {
        sqlite3_reset(stmt_rollback_);
        sqlite3_step(stmt_rollback_);
        sqlite3_reset(stmt_rollback_);
        throw;
    }

    step_and_reset(stmt_commit_);

    spdlog::debug("[Database] Batch-inserted {} strategy quotes", batch.size());
}

void Database::insert_sanity_failure(const DbSanityFailure& r)
{
    std::lock_guard<std::mutex> lock(mtx_);
    bind_int64 (stmt_insert_sanity_failure_, 1, static_cast<std::int64_t>(r.block_height));
    bind_text  (stmt_insert_sanity_failure_, 2, r.pair_name);
    bind_text  (stmt_insert_sanity_failure_, 3, r.side);
    bind_int64 (stmt_insert_sanity_failure_, 4, static_cast<std::int64_t>(r.tier));
    bind_int64 (stmt_insert_sanity_failure_, 5, r.proposed_price_mojos);
    bind_int64 (stmt_insert_sanity_failure_, 6, r.reference_price_mojos);
    bind_double(stmt_insert_sanity_failure_, 7, r.deviation_pct);
    bind_text  (stmt_insert_sanity_failure_, 8, r.failure_reason);
    bind_text  (stmt_insert_sanity_failure_, 9, r.details);

    step_and_reset(stmt_insert_sanity_failure_);

    spdlog::debug("[Database] Logged sanity failure: pair={} side={} tier={} reason={}",
                  r.pair_name, r.side, r.tier, r.failure_reason);
}

std::optional<DbSnapshot> Database::get_last_snapshot(
    const std::string& pair_name) const
{
    std::lock_guard<std::mutex> lock(mtx_);
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
        s.xch_usd_rate    = sqlite3_column_double(stmt_last_snapshot_, 8);
        s.pnl_total_usd   = sqlite3_column_double(stmt_last_snapshot_, 9);

        // Phase 2 strategy decision fields.
        s.reservation_price_mojos = sqlite3_column_int64(stmt_last_snapshot_, 10);
        s.half_spread_bps  = sqlite3_column_double(stmt_last_snapshot_, 11);
        s.kappa            = sqlite3_column_double(stmt_last_snapshot_, 12);
        s.variance_ratio   = sqlite3_column_double(stmt_last_snapshot_, 13);
        s.adverse_rate     = sqlite3_column_double(stmt_last_snapshot_, 14);
        s.s_adverse_bps    = sqlite3_column_double(stmt_last_snapshot_, 15);
        s.s_inventory_bps  = sqlite3_column_double(stmt_last_snapshot_, 16);
        s.s_cost_bps       = sqlite3_column_double(stmt_last_snapshot_, 17);

        result = std::move(s);
    }

    sqlite3_reset(stmt_last_snapshot_);
    sqlite3_clear_bindings(stmt_last_snapshot_);

    return result;
}

// ===========================================================================
// Inventory state (cost-basis persistence, PNL-BASIS-PERSIST 2026-07-30)
// ===========================================================================

void Database::save_inventory_state(
    const std::vector<DbInventoryState>& records) noexcept
{
    // Fills arrive a handful of times per day; a one-off prepared statement
    // per call is fine and keeps the hot-path statement cache untouched.
    static constexpr const char* kUpsert = R"SQL(
        INSERT INTO inventory_state
            (asset_id, total_quantity, total_cost,
             basis_is_seed_sentinel, updated_at)
        VALUES (?1, ?2, ?3, ?4, CURRENT_TIMESTAMP)
        ON CONFLICT(asset_id) DO UPDATE SET
            total_quantity         = excluded.total_quantity,
            total_cost             = excluded.total_cost,
            basis_is_seed_sentinel = excluded.basis_is_seed_sentinel,
            updated_at             = CURRENT_TIMESTAMP;
    )SQL";

    std::lock_guard<std::mutex> lock(mtx_);

    try {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, kUpsert, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            spdlog::error("[Database] save_inventory_state prepare failed: {}",
                          sqlite3_errmsg(db_));
            return;
        }

        // The snapshot must be all-or-nothing: restore_record() replays these
        // rows verbatim at startup, so a half-written set (asset A's post-sell
        // quantity beside asset B's stale one) restores an inconsistent cost
        // basis that then contaminates realized P&L and the no-loss floor.
        //
        // A failed BEGIN leaves SQLite in autocommit, which would silently
        // turn the loop into N independent commits and make the ROLLBACK
        // below a no-op.  Bail out instead: keeping the previous consistent
        // snapshot is strictly better than writing a torn one, and the next
        // fill (or the next seed/reconcile) retries.
        rc = sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) {
            spdlog::error("[Database] save_inventory_state: BEGIN IMMEDIATE "
                          "failed ({}): {} -- skipping this snapshot, "
                          "previous state retained",
                          rc, sqlite3_errmsg(db_));
            sqlite3_finalize(stmt);
            return;
        }

        bool ok = true;
        for (const auto& rec : records) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_text(stmt, 1, rec.asset_id.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, rec.total_quantity);
            sqlite3_bind_double(stmt, 3, rec.total_cost);
            sqlite3_bind_int(stmt, 4, rec.basis_is_seed_sentinel ? 1 : 0);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                spdlog::error("[Database] save_inventory_state step failed "
                              "for {}: {}",
                              rec.asset_id.substr(0, 12), sqlite3_errmsg(db_));
                ok = false;
                break;
            }
        }

        sqlite3_finalize(stmt);

        char* end_err = nullptr;
        rc = sqlite3_exec(db_, ok ? "COMMIT;" : "ROLLBACK;",
                          nullptr, nullptr, &end_err);
        if (rc != SQLITE_OK) {
            spdlog::error("[Database] save_inventory_state: {} failed ({}): {}",
                          ok ? "COMMIT" : "ROLLBACK", rc,
                          end_err ? end_err : "unknown");
            if (end_err) sqlite3_free(end_err);
            // Never leave a transaction open on this shared connection --
            // the next unrelated write would inherit it.
            if (!sqlite3_get_autocommit(db_)) {
                sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
        }
    } catch (...) {
        // noexcept contract: persistence must never disrupt trading.
        if (!sqlite3_get_autocommit(db_)) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
        spdlog::error("[Database] save_inventory_state: unexpected exception");
    }
}

std::vector<DbInventoryState> Database::load_inventory_state() const
{
    static constexpr const char* kSelect = R"SQL(
        SELECT asset_id, total_quantity, total_cost, basis_is_seed_sentinel
        FROM inventory_state;
    )SQL";

    std::lock_guard<std::mutex> lock(mtx_);

    std::vector<DbInventoryState> result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, kSelect, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("[Database] load_inventory_state prepare failed: {}",
                      sqlite3_errmsg(db_));
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DbInventoryState rec;
        const char* aid = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 0));
        rec.asset_id               = aid ? aid : "";
        rec.total_quantity         = sqlite3_column_int64(stmt, 1);
        rec.total_cost             = sqlite3_column_double(stmt, 2);
        rec.basis_is_seed_sentinel = (sqlite3_column_int(stmt, 3) != 0);
        if (!rec.asset_id.empty()) {
            result.push_back(std::move(rec));
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

// ===========================================================================
// Double-entry ledger (2026-07-30)
// ===========================================================================

std::optional<std::size_t> Database::append_ledger_entries(
    const std::vector<DbLedgerEntry>& legs) noexcept
{
    if (legs.empty()) {
        return std::size_t{0};
    }

    // INSERT OR IGNORE against UNIQUE(event_id, leg, asset_id): re-posting an
    // already-recorded event is a no-op, which is what makes fill
    // re-detection after a crash safe.
    static constexpr const char* kInsert = R"SQL(
        INSERT OR IGNORE INTO ledger_entries
            (entry_time, event_type, event_id, leg, asset_id,
             delta_mojos, pair_name, block_height, note)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);
    )SQL";

    std::lock_guard<std::mutex> lock(mtx_);
    std::size_t inserted = 0;

    try {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, kInsert, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            spdlog::error("[Database] append_ledger_entries prepare failed: {}",
                          sqlite3_errmsg(db_));
            return std::nullopt;
        }

        // All-or-nothing: a half-posted event would permanently unbalance the
        // ledger and make the reconciliation control fire forever.
        rc = sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) {
            spdlog::error("[Database] append_ledger_entries: BEGIN failed "
                          "({}): {}", rc, sqlite3_errmsg(db_));
            sqlite3_finalize(stmt);
            return std::nullopt;
        }

        bool ok = true;
        for (const auto& leg : legs) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_text(stmt, 1, leg.entry_time.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, leg.event_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, leg.event_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, leg.leg.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, leg.asset_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 6, leg.delta_mojos);
            sqlite3_bind_text(stmt, 7, leg.pair_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 8, static_cast<std::int64_t>(leg.block_height));
            sqlite3_bind_text(stmt, 9, leg.note.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                spdlog::error("[Database] append_ledger_entries step failed "
                              "for event {} leg {}: {}",
                              leg.event_id.substr(0, 16), leg.leg,
                              sqlite3_errmsg(db_));
                ok = false;
                break;
            }
            inserted += static_cast<std::size_t>(sqlite3_changes(db_));
        }

        sqlite3_finalize(stmt);

        char* err = nullptr;
        rc = sqlite3_exec(db_, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            spdlog::error("[Database] append_ledger_entries: {} failed: {}",
                          ok ? "COMMIT" : "ROLLBACK", err ? err : "unknown");
            if (err) sqlite3_free(err);
            if (!sqlite3_get_autocommit(db_)) {
                sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
            return std::nullopt;
        }
        if (!ok) {
            return std::nullopt;
        }
    } catch (...) {
        if (!sqlite3_get_autocommit(db_)) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
        spdlog::error("[Database] append_ledger_entries: unexpected exception");
        return std::nullopt;
    }

    return inserted;
}

BlockHeight Database::ledger_opening_block(const AssetId& asset_id) const
{
    static constexpr const char* kSelect = R"SQL(
        SELECT COALESCE(block_height, 0) FROM ledger_entries
        WHERE asset_id = ?1 AND leg = 'opening' LIMIT 1;
    )SQL";

    std::lock_guard<std::mutex> lock(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSelect, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, asset_id.c_str(), -1, SQLITE_TRANSIENT);
    BlockHeight h = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        h = static_cast<BlockHeight>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return h;
}

std::unordered_map<AssetId, Mojo> Database::ledger_balances() const
{
    static constexpr const char* kSelect = R"SQL(
        SELECT asset_id, SUM(delta_mojos)
        FROM ledger_entries GROUP BY asset_id;
    )SQL";

    std::lock_guard<std::mutex> lock(mtx_);
    std::unordered_map<AssetId, Mojo> out;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSelect, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("[Database] ledger_balances prepare failed: {}",
                      sqlite3_errmsg(db_));
        return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (a && a[0]) {
            out[a] = sqlite3_column_int64(stmt, 1);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

bool Database::has_ledger_opening(const AssetId& asset_id) const
{
    static constexpr const char* kSelect = R"SQL(
        SELECT 1 FROM ledger_entries
        WHERE asset_id = ?1 AND leg = 'opening' LIMIT 1;
    )SQL";

    std::lock_guard<std::mutex> lock(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSelect, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, asset_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

std::int64_t Database::ledger_entry_count() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM ledger_entries;", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    std::int64_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        n = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return n;
}

// ===========================================================================
// Arbitrage edge history (2026-07-30)
// ===========================================================================

void Database::insert_arb_edge(const DbArbEdgeObservation& obs) noexcept
{
    static constexpr const char* kInsert = R"SQL(
        INSERT INTO arb_edge_log
            (observed_at, block_height, direction, ask_a_mojos, bid_b_mojos,
             cross_rate, gross_edge_bps, net_edge_bps, ask_size_mojos,
             bid_size_mojos, state, armed, executed)
        VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);
    )SQL";

    std::lock_guard<std::mutex> lock(mtx_);
    try {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, kInsert, -1, &stmt, nullptr) != SQLITE_OK) {
            spdlog::warn("[Database] insert_arb_edge prepare failed: {}",
                         sqlite3_errmsg(db_));
            return;
        }
        sqlite3_bind_text  (stmt, 1, obs.observed_at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 2, static_cast<std::int64_t>(obs.block_height));
        sqlite3_bind_text  (stmt, 3, obs.direction.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 4, obs.ask_a_mojos);
        sqlite3_bind_int64 (stmt, 5, obs.bid_b_mojos);
        sqlite3_bind_double(stmt, 6, obs.cross_rate);
        sqlite3_bind_double(stmt, 7, obs.gross_edge_bps);
        sqlite3_bind_double(stmt, 8, obs.net_edge_bps);
        sqlite3_bind_int64 (stmt, 9, obs.ask_size_mojos);
        sqlite3_bind_int64 (stmt, 10, obs.bid_size_mojos);
        sqlite3_bind_text  (stmt, 11, obs.state.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 12, obs.armed ? 1 : 0);
        sqlite3_bind_int   (stmt, 13, obs.executed ? 1 : 0);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            spdlog::warn("[Database] insert_arb_edge step failed: {}",
                         sqlite3_errmsg(db_));
        }
        sqlite3_finalize(stmt);
    } catch (...) {
        spdlog::warn("[Database] insert_arb_edge: unexpected exception");
    }
}

// ===========================================================================
// Diagnostics
// ===========================================================================

std::int64_t Database::trade_count() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    int rc = sqlite3_step(stmt_trade_count_);
    std::int64_t count = 0;
    if (rc == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt_trade_count_, 0);
    } else if (rc != SQLITE_DONE) {
        spdlog::warn("Database::trade_count: sqlite3_step failed ({})", rc);
    }
    sqlite3_reset(stmt_trade_count_);
    return count;
}

std::int64_t Database::offer_count() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    int rc = sqlite3_step(stmt_offer_count_);
    std::int64_t count = 0;
    if (rc == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt_offer_count_, 0);
    } else if (rc != SQLITE_DONE) {
        spdlog::warn("Database::offer_count: sqlite3_step failed ({})", rc);
    }
    sqlite3_reset(stmt_offer_count_);
    return count;
}

std::int64_t Database::snapshot_count() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    int rc = sqlite3_step(stmt_snapshot_count_);
    std::int64_t count = 0;
    if (rc == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt_snapshot_count_, 0);
    } else if (rc != SQLITE_DONE) {
        spdlog::warn("Database::snapshot_count: sqlite3_step failed ({})", rc);
    }
    sqlite3_reset(stmt_snapshot_count_);
    return count;
}

double Database::fill_rate_since_block(BlockHeight since, double fallback) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    bind_int64(stmt_fill_rate_, 1, static_cast<std::int64_t>(since));
    int rc = sqlite3_step(stmt_fill_rate_);
    double rate = fallback;
    if (rc == SQLITE_ROW) {
        std::int64_t filled = sqlite3_column_int64(stmt_fill_rate_, 0);
        std::int64_t total  = sqlite3_column_int64(stmt_fill_rate_, 1);
        if (total > 0) {
            rate = static_cast<double>(filled) / static_cast<double>(total);
        }
    } else if (rc != SQLITE_DONE) {
        spdlog::warn("Database::fill_rate_since_block: sqlite3_step failed ({})", rc);
    }
    sqlite3_reset(stmt_fill_rate_);
    return rate;
}

std::vector<double> Database::query_tier_fill_rates(
    const std::string& pair_name,
    const std::string& cutoff_ts,
    std::uint32_t max_tiers) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<double> rates(max_tiers, 0.0);

    bind_text(stmt_tier_fill_rates_, 1, pair_name);
    bind_text(stmt_tier_fill_rates_, 2, cutoff_ts);

    while (sqlite3_step(stmt_tier_fill_rates_) == SQLITE_ROW) {
        auto tier  = static_cast<std::uint32_t>(sqlite3_column_int64(stmt_tier_fill_rates_, 0));
        auto filled = sqlite3_column_int64(stmt_tier_fill_rates_, 1);
        auto total  = sqlite3_column_int64(stmt_tier_fill_rates_, 2);

        if (tier < max_tiers && total > 0) {
            rates[tier] = static_cast<double>(filled) / static_cast<double>(total);
        }
    }

    sqlite3_reset(stmt_tier_fill_rates_);
    sqlite3_clear_bindings(stmt_tier_fill_rates_);

    return rates;
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
        // [T7-06] WAL is required for concurrent reader safety.  Throw
        // rather than silently falling back to journal mode, which would
        // cause "database locked" errors under concurrent GUI access.
        throw std::runtime_error(msg);
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

    // Busy timeout: retry for up to 5 s instead of failing immediately with
    // SQLITE_BUSY when another connection (PnLTracker, GUI, maintenance
    // scripts) holds the write lock.  Without this, a transient lock during
    // fill processing throws and the fill's audit-trail row is lost forever.
    rc = sqlite3_exec(db_, "PRAGMA busy_timeout=5000;",
                      nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK && err_msg) {
        spdlog::warn("[Database] PRAGMA busy_timeout failed: {}", err_msg);
        sqlite3_free(err_msg);
    }

    spdlog::debug("[Database] Pragmas configured (WAL, NORMAL sync, 10MB cache, "
                  "5s busy timeout)");
}

void Database::run_migrations()
{
    char* err_msg = nullptr;

    // Execute each DDL statement.  Order matters: tables before indices.
    const char* ddl_statements[] = {
        kCreateTradeLog,
        kCreateOfferLog,
        kCreateOfferClosureEvents,
        kCreateSnapshots,
        kCreateStrategyQuotes,
        kCreateSanityFailures,
        kCreateInventoryState,
        kCreateLedgerEntries,
        kIndexLedgerAsset,
        kCreateArbEdgeLog,
        kIndexArbEdgeDirTime,
        kIndexTradeTimestamp,
        kIndexTradePair,
        kIndexOfferStatus,
        kIndexOfferPair,
        kIndexOfferClosureEventsOffer,
        kIndexOfferClosureEventsPair,
        kIndexSnapshotPairBlock,
        kIndexStrategyQuotesPairBlock,
        kIndexSanityFailuresPair
    };

    for (const char* ddl : ddl_statements) {
        int rc = sqlite3_exec(db_, ddl, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string msg = "[Database] Migration failed: ";
            if (err_msg) { msg += err_msg; sqlite3_free(err_msg); }
            throw std::runtime_error(msg);
        }
    }

    // Forward-compatible column migrations for pre-existing databases.
    // We intentionally ignore "duplicate column name" errors.
    sqlite3_exec(db_, "ALTER TABLE trade_log ADD COLUMN offer_hash TEXT;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE trade_log ADD COLUMN acquisition_ts TEXT;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE offer_log ADD COLUMN fee_mojos INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE offer_log ADD COLUMN cancel_reason TEXT;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE offer_log ADD COLUMN competitiveness_score INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE offer_log ADD COLUMN queue_ahead_mojos INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE offer_log ADD COLUMN queue_ahead_score INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE offer_log ADD COLUMN execution_quality_score INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE offer_log ADD COLUMN book_best_bid INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE offer_log ADD COLUMN book_best_ask INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);

    // Phase 2: strategy decision parameters on snapshots table.
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN reservation_price_mojos INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN half_spread_bps REAL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN kappa REAL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN variance_ratio REAL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN adverse_rate REAL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN s_adverse_bps REAL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN s_inventory_bps REAL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN s_cost_bps REAL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN xch_usd_rate REAL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "ALTER TABLE snapshots ADD COLUMN pnl_total_usd REAL DEFAULT 0;",
                 nullptr, nullptr, nullptr);
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
