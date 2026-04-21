// pnl.cpp -- Implementation of xop::PnLTracker.
//
// PnL attribution splits total returns into three independent components:
//   1. Spread PnL   -- realised profit from completing a buy+sell round trip.
//   2. Inventory PnL -- unrealised mark-to-market on positions still held.
//   3. Fee PnL       -- net of blockchain costs and DBX/AMM incentive income.
//
// The SQLite trade log is the authoritative record.  Writes use prepared
// statements with bound parameters to prevent SQL injection (ISO/IEC 27001).
// The database is opened in WAL mode for concurrent read/write performance.
//
// All monetary arithmetic uses int64_t (mojos).  Doubles are used only for
// dimensionless ratios (Sharpe, drawdown, fill rate) that are never stored.
//
// Compliant with:
//   ISO/IEC 27001:2022  -- parameterised queries, append-only audit log
//   ISO/IEC 5055        -- no const_cast on mutable state (T2-07), RAII
//   ISO/IEC 25000       -- clear error handling, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- portable C++20, defined behaviour throughout

#include "xop/monitoring/pnl.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace xop {

// =========================================================================
// Timestamp conversion helpers
// =========================================================================

std::string PnLTracker::timestamp_to_iso(Timestamp ts)
{
    // Convert system_clock time_point to ISO-8601 UTC string.
    // Format: "YYYY-MM-DDTHH:MM:SS.mmmZ"
    const auto epoch   = ts.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch);
    const auto millis  = std::chrono::duration_cast<std::chrono::milliseconds>(epoch)
                         - std::chrono::duration_cast<std::chrono::milliseconds>(seconds);

    const std::time_t tt = std::chrono::system_clock::to_time_t(ts);
    std::tm utc_tm{};

#if defined(_MSC_VER) || defined(_WIN32)
    gmtime_s(&utc_tm, &tt);
#else
    gmtime_r(&tt, &utc_tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << millis.count()
        << 'Z';
    return oss.str();
}

Timestamp PnLTracker::iso_to_timestamp(const std::string& iso)
{
    // Parse "YYYY-MM-DDTHH:MM:SS" (milliseconds and trailing 'Z' optional).
    std::tm tm{};
    std::istringstream iss(iso);
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

    if (iss.fail()) {
        spdlog::warn("iso_to_timestamp: failed to parse '{}'", iso);
        return Timestamp{};
    }

    // Portable UTC conversion: set tm_isdst to 0 to avoid DST adjustments,
    // then use platform-specific UTC mktime.
    tm.tm_isdst = 0;

#if defined(_MSC_VER) || defined(_WIN32)
    const std::time_t tt = _mkgmtime(&tm);
#else
    const std::time_t tt = timegm(&tm);
#endif

    if (tt == static_cast<std::time_t>(-1)) {
        spdlog::warn("iso_to_timestamp: mkgmtime failed for '{}'", iso);
        return Timestamp{};
    }

    auto result = std::chrono::system_clock::from_time_t(tt);

    // Parse optional milliseconds after the decimal point.
    if (iss.peek() == '.') {
        iss.get();  // consume '.'
        int ms = 0;
        iss >> ms;
        result += std::chrono::milliseconds(ms);
    }

    return result;
}

std::string PnLTracker::timestamp_to_date(Timestamp ts)
{
    const std::time_t tt = std::chrono::system_clock::to_time_t(ts);
    std::tm utc_tm{};

#if defined(_MSC_VER) || defined(_WIN32)
    gmtime_s(&utc_tm, &tt);
#else
    gmtime_r(&tt, &utc_tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&utc_tm, "%Y-%m-%d");
    return oss.str();
}

// =========================================================================
// Construction / Destruction
// =========================================================================

PnLTracker::PnLTracker(const std::string& db_path)
    : db_path_{db_path}
{
    spdlog::info("PnLTracker: opening database at '{}'", db_path_);
}

PnLTracker::~PnLTracker()
{
    // Finalise prepared statements before closing the connection.
    // Order does not matter; each is independent.
    finalize_stmt(stmt_insert_);
    finalize_stmt(stmt_query_range_);
    finalize_stmt(stmt_query_pair_);

    if (db_) {
        const int rc = sqlite3_close(db_);
        if (rc != SQLITE_OK) {
            // Cannot throw from a destructor.  Log the error.
            spdlog::error("PnLTracker: sqlite3_close failed: {}",
                          sqlite3_errmsg(db_));
        } else {
            spdlog::info("PnLTracker: database closed");
        }
        db_ = nullptr;
    }
}

void PnLTracker::finalize_stmt(sqlite3_stmt*& stmt) noexcept
{
    if (stmt) {
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
}

// =========================================================================
// Database initialisation
// =========================================================================

void PnLTracker::init_database()
{
    // Open (or create) the database file with WAL journaling for concurrent
    // read performance and crash safety.
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        const std::string err = db_ ? sqlite3_errmsg(db_) : "out of memory";
        throw std::runtime_error("PnLTracker: sqlite3_open failed: " + err);
    }

    // Enable WAL mode for concurrent readers and durable writes.
    char* errmsg = nullptr;
    rc = sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        const std::string err = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        spdlog::warn("PnLTracker: failed to set WAL mode: {}", err);
    }

    // Enable foreign keys (good practice even if not currently used).
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

    // Create the trade_log table and indices.  IF NOT EXISTS makes this
    // idempotent -- safe to call on every startup.
    static constexpr const char* kCreateTable = R"SQL(
        CREATE TABLE IF NOT EXISTS trade_log (
            id                INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp         TEXT    NOT NULL,
            trade_id          TEXT    UNIQUE NOT NULL,
            pair_name         TEXT    NOT NULL,
            side              TEXT    NOT NULL,
            price_mojos       INTEGER NOT NULL,
            size_mojos        INTEGER NOT NULL,
            fee_mojos         INTEGER NOT NULL,
            cost_basis_mojos  INTEGER,
            realized_pnl_mojos INTEGER,
            block_height      INTEGER,
            offer_hash        TEXT,
            acquisition_ts    TEXT,
            created_at        TEXT    DEFAULT CURRENT_TIMESTAMP
        );
    )SQL";

    static constexpr const char* kCreateIdxPair = R"SQL(
        CREATE INDEX IF NOT EXISTS idx_trade_log_pair
            ON trade_log(pair_name);
    )SQL";

    static constexpr const char* kCreateIdxTs = R"SQL(
        CREATE INDEX IF NOT EXISTS idx_trade_log_timestamp
            ON trade_log(timestamp);
    )SQL";

    errmsg = nullptr;
    rc = sqlite3_exec(db_, kCreateTable, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        const std::string err = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        throw std::runtime_error("PnLTracker: CREATE TABLE failed: " + err);
    }

    // T2-06: Migrate existing databases -- add columns that may be
    // missing in older schemas.  ALTER TABLE ... ADD COLUMN returns an
    // error when a column already exists; this is intentionally ignored.
    static constexpr const char* kMigrateOfferHash = R"SQL(
        ALTER TABLE trade_log ADD COLUMN offer_hash TEXT;
    )SQL";

    sqlite3_exec(db_, kMigrateOfferHash, nullptr, nullptr, nullptr);

    static constexpr const char* kMigrateAcqTs = R"SQL(
        ALTER TABLE trade_log ADD COLUMN acquisition_ts TEXT;
    )SQL";
    sqlite3_exec(db_, kMigrateAcqTs, nullptr, nullptr, nullptr);

    errmsg = nullptr;
    rc = sqlite3_exec(db_, kCreateIdxPair, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        const std::string err = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        spdlog::warn("PnLTracker: CREATE INDEX pair failed: {}", err);
    }

    errmsg = nullptr;
    rc = sqlite3_exec(db_, kCreateIdxTs, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        const std::string err = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        spdlog::warn("PnLTracker: CREATE INDEX timestamp failed: {}", err);
    }

    // Prepare reusable statements.  Parameterised queries guard against
    // SQL injection (ISO/IEC 27001:2022 A.8.26 -- application security).

    static constexpr const char* kInsertSql = R"SQL(
        INSERT INTO trade_log
            (timestamp, trade_id, pair_name, side,
             price_mojos, size_mojos, fee_mojos,
             cost_basis_mojos, realized_pnl_mojos,
             block_height, offer_hash, acquisition_ts)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12);
    )SQL";

    static constexpr const char* kQueryRangeSql = R"SQL(
        SELECT timestamp, trade_id, pair_name, side,
               price_mojos, size_mojos, fee_mojos,
               cost_basis_mojos, realized_pnl_mojos,
               block_height, offer_hash, acquisition_ts
        FROM trade_log
        WHERE timestamp >= ?1 AND timestamp < ?2
        ORDER BY timestamp ASC;
    )SQL";

    static constexpr const char* kQueryPairSql = R"SQL(
        SELECT timestamp, trade_id, pair_name, side,
               price_mojos, size_mojos, fee_mojos,
               cost_basis_mojos, realized_pnl_mojos,
               block_height, offer_hash, acquisition_ts
        FROM trade_log
        WHERE pair_name = ?1 AND timestamp >= ?2 AND timestamp < ?3
        ORDER BY timestamp ASC;
    )SQL";

    rc = sqlite3_prepare_v2(db_, kInsertSql, -1, &stmt_insert_, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(
            "PnLTracker: prepare INSERT failed: "
            + std::string(sqlite3_errmsg(db_)));
    }

    rc = sqlite3_prepare_v2(db_, kQueryRangeSql, -1, &stmt_query_range_, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(
            "PnLTracker: prepare SELECT range failed: "
            + std::string(sqlite3_errmsg(db_)));
    }

    rc = sqlite3_prepare_v2(db_, kQueryPairSql, -1, &stmt_query_pair_, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(
            "PnLTracker: prepare SELECT pair failed: "
            + std::string(sqlite3_errmsg(db_)));
    }

    spdlog::info("PnLTracker: database initialised (WAL mode, tables verified)");
}

// =========================================================================
// Trade persistence
// =========================================================================

void PnLTracker::insert_trade(const TradeRecord& record)
{
    // ISO/IEC 5055 -- CWE-362: protect SQLite prepared-statement access from
    // concurrent callers.  Callers that already hold mtx_ (e.g. record_fill)
    // must use insert_trade_unlocked() to avoid deadlock.
    std::lock_guard<std::mutex> lock(mtx_);
    insert_trade_unlocked(record);
}

void PnLTracker::insert_trade_unlocked(const TradeRecord& record)
{
    if (!db_ || !stmt_insert_) {
        throw std::runtime_error(
            "PnLTracker::insert_trade: database not initialised");
    }

    // Reset the prepared statement for reuse.
    sqlite3_reset(stmt_insert_);
    sqlite3_clear_bindings(stmt_insert_);

    const std::string ts_str = timestamp_to_iso(record.timestamp);
    // Normalise side to lowercase to match DB CHECK(side IN ('bid','ask')).
    const char* side_str = (record.side == Side::Bid) ? "bid" : "ask";

    // Bind all parameters.  Text uses SQLITE_TRANSIENT so SQLite copies
    // the string -- safe because ts_str and side_str may go out of scope.
    sqlite3_bind_text(stmt_insert_,  1, ts_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_,  2, record.trade_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_,  3, record.pair_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_,  4, side_str, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_insert_, 5, record.price_mojos);
    sqlite3_bind_int64(stmt_insert_, 6, record.size_mojos);
    sqlite3_bind_int64(stmt_insert_, 7, record.fee_mojos);
    sqlite3_bind_int64(stmt_insert_, 8, record.cost_basis_mojos);
    sqlite3_bind_int64(stmt_insert_, 9, record.realized_pnl_mojos);
    // ISO/IEC 5055 -- CWE-681: use int64 to avoid truncation of BlockHeight
    // (uint32_t can exceed INT_MAX when high bit is set).
    sqlite3_bind_int64(stmt_insert_, 10, static_cast<int64_t>(record.block_height));
    sqlite3_bind_text(stmt_insert_, 11, record.offer_hash.c_str(), -1, SQLITE_TRANSIENT);

    // T2-06: Persist acquisition timestamp for IRS Form 8949 "Date Acquired".
    // Only sell records carry a meaningful acquisition timestamp; buys bind NULL.
    if (record.side == Side::Ask
        && record.acquisition_ts != Timestamp{}) {
        const std::string acq_str = timestamp_to_iso(record.acquisition_ts);
        sqlite3_bind_text(stmt_insert_, 12, acq_str.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt_insert_, 12);
    }

    const int rc = sqlite3_step(stmt_insert_);
    if (rc != SQLITE_DONE) {
        spdlog::error("PnLTracker::insert_trade: step failed (rc={}): {}",
                       rc, sqlite3_errmsg(db_));
        throw std::runtime_error(
            "PnLTracker::insert_trade: INSERT failed: "
            + std::string(sqlite3_errmsg(db_)));
    }

    spdlog::debug("PnLTracker::insert_trade: persisted trade_id={}",
                   record.trade_id);
}

// =========================================================================
// Trade querying
// =========================================================================

namespace {

/// Read a single TradeRecord from the current row of a stepped statement.
TradeRecord read_row(sqlite3_stmt* stmt)
{
    TradeRecord rec;

    // Column indices match the SELECT order in the prepared statements.
    const char* ts_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    rec.timestamp = PnLTracker::iso_to_timestamp(ts_text ? ts_text : "");

    const char* tid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    rec.trade_id = tid ? tid : "";

    const char* pair = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    rec.pair_name = pair ? pair : "";

    const char* side_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    if (side_text &&
        (std::strcmp(side_text, "ask") == 0 || std::strcmp(side_text, "Ask") == 0)) {
        rec.side = Side::Ask;
    } else {
        rec.side = Side::Bid;
    }

    rec.price_mojos        = sqlite3_column_int64(stmt, 4);
    rec.size_mojos         = sqlite3_column_int64(stmt, 5);
    rec.fee_mojos          = sqlite3_column_int64(stmt, 6);
    rec.cost_basis_mojos   = sqlite3_column_int64(stmt, 7);
    rec.realized_pnl_mojos = sqlite3_column_int64(stmt, 8);
    // ISO/IEC 5055 -- CWE-681: use int64 to avoid truncation of BlockHeight.
    rec.block_height       = static_cast<BlockHeight>(sqlite3_column_int64(stmt, 9));

    const char* hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
    rec.offer_hash = hash ? hash : "";

    // T2-06: Read acquisition timestamp (column 11).  NULL for buy records.
    const char* acq_ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    rec.acquisition_ts = (acq_ts && acq_ts[0] != '\0')
                             ? PnLTracker::iso_to_timestamp(acq_ts)
                             : Timestamp{};

    return rec;
}

}  // anonymous namespace

// T2-07: Removed const qualifier.  SQLite's bind/step/reset API mutates
// prepared-statement state; the prior const_cast was unsafe because
// concurrent const accessors could corrupt that state (ISO/IEC 5055,
// CWE-362).  Non-const communicates the mutation to the type system so
// the compiler enforces proper synchronisation by callers.
std::vector<TradeRecord> PnLTracker::get_trade_log(
    Timestamp start, Timestamp end)
{
    if (!db_ || !stmt_query_range_) {
        throw std::runtime_error(
            "PnLTracker::get_trade_log: database not initialised");
    }

    const std::string start_str = timestamp_to_iso(start);
    const std::string end_str   = timestamp_to_iso(end);

    // Reset the prepared statement for reuse -- safe now that the method
    // is non-const and callers cannot alias the statement concurrently.
    sqlite3_reset(stmt_query_range_);
    sqlite3_clear_bindings(stmt_query_range_);

    sqlite3_bind_text(stmt_query_range_, 1, start_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_query_range_, 2, end_str.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<TradeRecord> results;
    while (sqlite3_step(stmt_query_range_) == SQLITE_ROW) {
        results.push_back(read_row(stmt_query_range_));
    }

    return results;
}

// T2-07: Removed const qualifier for the same reason as get_trade_log.
std::vector<TradeRecord> PnLTracker::query_trades(
    const std::string& pair_name,
    const std::string& start,
    const std::string& end)
{
    if (!db_) {
        throw std::runtime_error(
            "PnLTracker::query_trades: database not initialised");
    }

    // If pair_name is empty, query all pairs via the range-only statement.
    if (pair_name.empty()) {
        const Timestamp ts_start = iso_to_timestamp(start);
        const Timestamp ts_end   = iso_to_timestamp(end);
        return get_trade_log(ts_start, ts_end);
    }

    if (!stmt_query_pair_) {
        throw std::runtime_error(
            "PnLTracker::query_trades: pair statement not prepared");
    }

    // Bind and step the pair-filtered statement directly, without const_cast.
    sqlite3_reset(stmt_query_pair_);
    sqlite3_clear_bindings(stmt_query_pair_);

    sqlite3_bind_text(stmt_query_pair_, 1, pair_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_query_pair_, 2, start.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_query_pair_, 3, end.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<TradeRecord> results;
    while (sqlite3_step(stmt_query_pair_) == SQLITE_ROW) {
        results.push_back(read_row(stmt_query_pair_));
    }

    return results;
}

// =========================================================================
// Fill recording and PnL attribution
// =========================================================================

void PnLTracker::record_fill(const Fill& fill, Mojo fee, Mojo cost_basis,
                             Mojo realized_pnl)
{
    // -- Step 1: Use the pre-computed realised PnL from the engine ---------
    //
    // [T9-FIX] Realised PnL is computed once in engine.cpp (single source
    // of truth) and passed here.  This eliminates the prior redundant
    // computation that was fragile if either copy was modified independently.
    //
    // Formula (computed by engine):  (price - cost_basis) * size / kMojosPerXch
    //   - For sells (Ask): the surplus proceeds above cost
    //   - For buys (Bid): always 0
    //
    // Fee PnL is recorded separately for both sides.
    // Inventory PnL is not updated here -- it is recalculated by
    // mark_to_market() which uses current market prices.

    // -- Step 2: Persist to SQLite (crash-safe before in-memory update) ---

    TradeRecord record;
    record.timestamp          = fill.timestamp;
    record.trade_id           = fill.offer_id;
    record.pair_name          = fill.pair_name;
    record.side               = fill.side;
    record.price_mojos        = fill.price;
    record.size_mojos         = fill.size;
    record.fee_mojos          = fee;
    record.cost_basis_mojos   = cost_basis;
    record.realized_pnl_mojos = realized_pnl;
    record.block_height       = fill.block_height;
    record.offer_hash         = fill.offer_id;  // Offer ID is the spend-bundle hash.

    // ISO/IEC 5055 -- CWE-362: acquire the lock once for the entire
    // insert + in-memory update sequence, ensuring atomicity and avoiding
    // deadlock from nested lock_guard acquisitions.
    std::lock_guard<std::mutex> lock(mtx_);

    // T2-06: For sell fills, attach the pair's current weighted-average
    // acquisition timestamp so it is persisted alongside the trade record.
    // This must be read under the lock before the insert, and before the
    // accumulator update changes the value.
    auto& ppnl = pair_pnl_[fill.pair_name];

    if (fill.side == Side::Ask) {
        record.acquisition_ts = ppnl.avg_acquisition_ts;
    }
    // Buy records leave acquisition_ts default-initialised (epoch); the
    // insert_trade_unlocked method binds NULL for those.

    insert_trade_unlocked(record);

    // -- Step 3: Update in-memory PnL accumulators -----------------------

    if (fill.side == Side::Bid) {
        // T2-06: Update the weighted-average acquisition timestamp.
        //
        // Weighted-average timestamp formula mirrors the cost-basis
        // weighted-average calculation:
        //   new_ts = (old_ts * old_qty + fill_ts * fill_qty) / (old_qty + fill_qty)
        //
        // Timestamps are converted to durations-since-epoch for the
        // arithmetic, then converted back.  This is exact for
        // system_clock::duration (typically nanoseconds or microseconds).
        const Mojo old_qty = ppnl.acquisition_qty;
        const Mojo new_qty = old_qty + fill.size;

        if (new_qty > 0) {
            const auto old_dur = ppnl.avg_acquisition_ts.time_since_epoch();
            const auto new_dur = fill.timestamp.time_since_epoch();

            // Use double intermediates to avoid overflow on the multiply
            // (duration count * mojos can exceed int64 if both are large).
            const double blended =
                (static_cast<double>(old_dur.count()) * static_cast<double>(old_qty)
               + static_cast<double>(new_dur.count()) * static_cast<double>(fill.size))
                / static_cast<double>(new_qty);

            using Dur = Timestamp::duration;
            ppnl.avg_acquisition_ts = Timestamp{
                Dur{static_cast<Dur::rep>(blended)}};
        }

        ppnl.acquisition_qty = new_qty;
    } else {
        // Sell side: attribute realised spread PnL.
        ppnl.spread_pnl += realized_pnl;
        total_pnl_.spread_pnl += realized_pnl;

        if (realized_pnl >= 0) {
            ppnl.gross_profit += realized_pnl;
            total_pnl_.gross_profit += realized_pnl;
        } else {
            // Absolute value for gross_loss tracking.
            ppnl.gross_loss += (-realized_pnl);
            total_pnl_.gross_loss += (-realized_pnl);
        }

        // T2-06: Reduce acquisition_qty proportionally on sell (matches
        // weighted-average cost drawdown).  The timestamp itself is not
        // changed -- it remains the weighted average of the remaining lots.
        if (ppnl.acquisition_qty >= fill.size) {
            ppnl.acquisition_qty -= fill.size;
        } else {
            // Sold more than tracked (should not happen in normal operation).
            ppnl.acquisition_qty = 0;
        }
    }

    // Fee attribution (always negative for blockchain costs).
    ppnl.fee_pnl    -= fee;
    total_pnl_.fee_pnl -= fee;

    // Update fill counters and timestamps.
    ppnl.fill_count += 1;
    total_pnl_.fill_count += 1;

    if (ppnl.fill_count == 1) {
        ppnl.first_fill_ts = fill.timestamp;
        if (total_pnl_.fill_count == 1) {
            total_pnl_.first_fill_ts = fill.timestamp;
        }
    }
    ppnl.last_fill_ts = fill.timestamp;
    total_pnl_.last_fill_ts = fill.timestamp;

    spdlog::info("PnLTracker::record_fill pair={} side={} price={} size={} "
                 "realized_pnl={} fee={} spread_pnl_total={}",
                 fill.pair_name, to_string(fill.side), fill.price, fill.size,
                 realized_pnl, fee, total_pnl_.spread_pnl);
}

void PnLTracker::record_fee(const std::string& pair_name, Mojo amount)
{
    std::lock_guard<std::mutex> lock(mtx_);

    total_pnl_.fee_pnl += amount;

    if (!pair_name.empty()) {
        pair_pnl_[pair_name].fee_pnl += amount;
    }

    spdlog::debug("PnLTracker::record_fee pair='{}' amount={} total_fee_pnl={}",
                   pair_name, amount, total_pnl_.fee_pnl);
}

// =========================================================================
// Mark-to-market
// =========================================================================

void PnLTracker::mark_to_market(
    const std::function<Mojo(const std::string&, const std::string&)>& get_price,
    const std::function<Mojo(const std::string&)>& get_balance,
    const std::function<Mojo(const std::string&)>& get_cost_basis,
    double xch_usd_price,
    const std::function<double(const std::string&)>& get_pair_unit_factor)
{
    std::lock_guard<std::mutex> lock(mtx_);

    xch_usd_rate_ = xch_usd_price;

    // Reset aggregate inventory PnL before recalculating.
    total_pnl_.inventory_pnl = 0;

    for (auto& [pair_name, ppnl] : pair_pnl_) {
        // Extract the base asset from the pair name.
        // Convention: pair_name is "BASE/QUOTE", e.g. "XCH/wUSDC".
        // The base asset is the part before the '/'.
        const auto slash_pos = pair_name.find('/');
        if (slash_pos == std::string::npos) {
            spdlog::warn("PnLTracker::mark_to_market: "
                         "invalid pair format '{}'", pair_name);
            continue;
        }

        const std::string base_asset = pair_name.substr(0, slash_pos);

        const Mojo current_price = get_price(pair_name, base_asset);

        // [T8-21] Smooth the mid-price with an EMA to reduce unrealized
        // PnL noise from volatile spot price ticks.
        constexpr double kEmaAlpha = 0.3;
        auto ema_it = price_ema_.find(pair_name);
        Mojo smoothed_price = current_price;
        if (current_price > 0) {
            if (ema_it != price_ema_.end()) {
                double ema = kEmaAlpha * static_cast<double>(current_price)
                           + (1.0 - kEmaAlpha) * ema_it->second;
                ema_it->second = ema;
                smoothed_price = static_cast<Mojo>(ema);
            } else {
                price_ema_[pair_name] = static_cast<double>(current_price);
            }
        }
        const Mojo balance       = get_balance(base_asset);
        const Mojo basis         = get_cost_basis(base_asset);

        if (basis > 0 && balance > 0) {
            // [PNL-UNIT-FIX] Inventory PnL in quote-asset mojos.
            // Uses the canonical xop::quote_mojos_for helper from types.hpp
            // so this stays in lock-step with offer_manager.cpp and engine.cpp.
            // The unit_factor below is (quote_denom / base_denom) supplied
            // by the caller via get_pair_unit_factor; when absent we fall
            // back to 1.0 for legacy callers / tests.
            double unit_factor = 1.0;
            if (get_pair_unit_factor) {
                const double f = get_pair_unit_factor(pair_name);
                if (f > 0.0) {
                    unit_factor = f;
                }
            }
            // Equivalent to quote_mojos_for(balance, smoothed_price - basis,
            //                               base_denom, quote_denom)
            // with quote_denom/base_denom collapsed into unit_factor.
            ppnl.inventory_pnl = static_cast<Mojo>(std::llround(
                static_cast<double>(smoothed_price - basis)
                * static_cast<double>(balance)
                * unit_factor
                / static_cast<double>(kMojosPerXch)));
        } else {
            ppnl.inventory_pnl = 0;
        }

        total_pnl_.inventory_pnl += ppnl.inventory_pnl;
    }

    // Record a PnL snapshot for Sharpe/drawdown analytics.
    const Mojo total = total_pnl_.spread_pnl
                     + total_pnl_.inventory_pnl
                     + total_pnl_.fee_pnl;

    PnLSnapshot snap;
    snap.timestamp = std::chrono::system_clock::now();
    snap.total_pnl = total;

    pnl_history_.push_back(snap);

    // Cap the history buffer to prevent unbounded memory growth.
    // T3-21: pop_front() on std::deque is O(1), replacing the prior
    // std::vector::erase(begin()) which was O(n) per trim.
    if (pnl_history_.size() > kMaxSnapshots) {
        pnl_history_.pop_front();
    }

    spdlog::debug("PnLTracker::mark_to_market spread={} inventory={} fee={} "
                  "total={} xch_usd={:.4f}",
                  total_pnl_.spread_pnl, total_pnl_.inventory_pnl,
                  total_pnl_.fee_pnl, total, xch_usd_rate_);
}

// =========================================================================
// PnL queries
// =========================================================================

double PnLTracker::compute_sharpe() const
{
    // Annualised Sharpe ratio from the PnL snapshot history.
    //
    // Sharpe = mean(returns) / stdev(returns) * sqrt(periods_per_year)
    //
    // Each snapshot is taken ~52 seconds apart (one Chia block).
    // Periods per year = 365.25 * 24 * 3600 / 52 = ~606,646.
    //
    // We compute returns as the difference in total_pnl between
    // consecutive snapshots.  This is in mojos, but the ratio is
    // dimensionless so the unit cancels.

    if (pnl_history_.size() < 2) {
        return 0.0;
    }

    const std::size_t n = pnl_history_.size() - 1;
    std::vector<double> returns(n);

    for (std::size_t i = 0; i < n; ++i) {
        returns[i] = static_cast<double>(
            pnl_history_[i + 1].total_pnl - pnl_history_[i].total_pnl);
    }

    const double mean = std::accumulate(returns.begin(), returns.end(), 0.0)
                       / static_cast<double>(n);

    double sum_sq = 0.0;
    for (double r : returns) {
        const double diff = r - mean;
        sum_sq += diff * diff;
    }
    const double stdev = std::sqrt(sum_sq / static_cast<double>(n));

    if (stdev < 1e-12) {
        // Avoid division by near-zero.  If returns are flat, Sharpe is
        // infinite (or undefined).  Return 0 as a safe default.
        return 0.0;
    }

    // Annualisation factor.  We estimate the average interval between
    // snapshots from the actual timestamps rather than assuming 52s.
    const auto span = pnl_history_.back().timestamp - pnl_history_.front().timestamp;
    const double span_seconds = static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(span).count());

    if (span_seconds < 1.0) {
        return 0.0;
    }

    const double avg_interval = span_seconds / static_cast<double>(n);
    constexpr double seconds_per_year = 365.25 * 24.0 * 3600.0;
    const double periods_per_year = seconds_per_year / avg_interval;

    return (mean / stdev) * std::sqrt(periods_per_year);
}

double PnLTracker::compute_max_drawdown() const
{
    // Maximum drawdown = largest peak-to-trough decline as a fraction
    // of the peak value.  Expressed as a non-negative number in [0, 1].

    if (pnl_history_.size() < 2) {
        return 0.0;
    }

    Mojo peak = pnl_history_.front().total_pnl;
    double max_dd = 0.0;

    for (const auto& snap : pnl_history_) {
        if (snap.total_pnl > peak) {
            peak = snap.total_pnl;
        }

        // [T9-FIX] Track drawdown even when PnL is negative.
        // Use absolute peak value to avoid division by zero.
        if (peak != 0) {
            const double dd = static_cast<double>(peak - snap.total_pnl)
                            / std::abs(static_cast<double>(peak));
            if (dd > max_dd) {
                max_dd = dd;
            }
        }
    }

    return max_dd;
}

PnLSummary PnLTracker::build_summary(const PairPnL& ppnl,
                                       double xch_usd) const
{
    PnLSummary s{};
    s.spread_pnl    = ppnl.spread_pnl;
    s.inventory_pnl = ppnl.inventory_pnl;
    s.fee_pnl       = ppnl.fee_pnl;
    s.total_pnl     = ppnl.spread_pnl + ppnl.inventory_pnl + ppnl.fee_pnl;

    // Convert mojos to USD: total_pnl is in mojos-of-XCH (for XCH-quoted
    // pairs).  1 XCH = 10^12 mojos.
    s.total_pnl_usd = (static_cast<double>(s.total_pnl)
                       / static_cast<double>(kMojosPerXch)) * xch_usd;

    s.sharpe_ratio = compute_sharpe();
    s.max_drawdown = compute_max_drawdown();

    // Profit factor = gross_profit / gross_loss.  Undefined when
    // gross_loss is zero; return a large sentinel to signal "no losses".
    if (ppnl.gross_loss > 0) {
        s.profit_factor = static_cast<double>(ppnl.gross_profit)
                        / static_cast<double>(ppnl.gross_loss);
    } else if (ppnl.gross_profit > 0) {
        s.profit_factor = 1e9;  // Effectively infinite.
    } else {
        s.profit_factor = 0.0;  // No trades.
    }

    s.fill_count = ppnl.fill_count;

    // Fill rate per hour.
    if (ppnl.fill_count > 0 && ppnl.first_fill_ts != Timestamp{}) {
        const auto span = ppnl.last_fill_ts - ppnl.first_fill_ts;
        const double hours = static_cast<double>(
            std::chrono::duration_cast<std::chrono::seconds>(span).count())
            / 3600.0;

        s.fill_rate_per_hour = (hours > 0.0)
            ? static_cast<double>(ppnl.fill_count) / hours
            : static_cast<double>(ppnl.fill_count);
    } else {
        s.fill_rate_per_hour = 0.0;
    }

    // Adverse selection rate = adverse_fills / total_fills.
    if (ppnl.fill_count > 0) {
        s.adverse_selection_rate = static_cast<double>(ppnl.adverse_fills)
                                 / static_cast<double>(ppnl.fill_count);
    } else {
        s.adverse_selection_rate = 0.0;
    }

    return s;
}

PnLSummary PnLTracker::get_total_pnl() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return build_summary(total_pnl_, xch_usd_rate_);
}

PnLSummary PnLTracker::get_pair_pnl(const std::string& pair_name) const
{
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = pair_pnl_.find(pair_name);
    if (it == pair_pnl_.end()) {
        return PnLSummary{};
    }
    return build_summary(it->second, xch_usd_rate_);
}

DailySummary PnLTracker::get_daily_summary() const
{
    std::lock_guard<std::mutex> lock(mtx_);

    DailySummary ds{};
    ds.date = timestamp_to_date(std::chrono::system_clock::now());

    // NOTE: Returns cumulative lifetime totals, not a single day's figures.
    // [T9-FIX] Documented as lifetime; rename deferred to avoid
    // breaking any future callers.  A true daily implementation would
    // need to query trade_log with date-range filters.
    ds.spread_pnl    = total_pnl_.spread_pnl;
    ds.inventory_pnl = total_pnl_.inventory_pnl;
    ds.fee_pnl       = total_pnl_.fee_pnl;
    ds.total_pnl     = total_pnl_.spread_pnl
                     + total_pnl_.inventory_pnl
                     + total_pnl_.fee_pnl;

    ds.total_pnl_usd = (static_cast<double>(ds.total_pnl)
                        / static_cast<double>(kMojosPerXch)) * xch_usd_rate_;

    ds.fill_count    = total_pnl_.fill_count;
    ds.gross_profit  = total_pnl_.gross_profit;
    ds.gross_loss    = total_pnl_.gross_loss;

    // Compute fill rate for the day.
    if (total_pnl_.fill_count > 0 && total_pnl_.first_fill_ts != Timestamp{}) {
        const auto span = total_pnl_.last_fill_ts - total_pnl_.first_fill_ts;
        const double hours = static_cast<double>(
            std::chrono::duration_cast<std::chrono::seconds>(span).count())
            / 3600.0;
        ds.fill_rate_per_hour = (hours > 0.0)
            ? static_cast<double>(total_pnl_.fill_count) / hours
            : static_cast<double>(total_pnl_.fill_count);
    } else {
        ds.fill_rate_per_hour = 0.0;
    }

    return ds;
}

// =========================================================================
// Tax reporting
// =========================================================================

// T2-06: Uses stored acquisition timestamps for "Date Acquired" column.
// T2-07: Non-const -- transitive from query_trades (ISO/IEC 5055).
void PnLTracker::export_trades_csv(const std::string& start_date,
                                    const std::string& end_date,
                                    const std::string& csv_path)
{
    // IRS Form 8949 compatible CSV.
    // Columns: Date Acquired, Date Sold, Description, Proceeds (mojos),
    //          Cost Basis (mojos), Gain or Loss (mojos), Term

    const auto records = query_trades("", start_date, end_date);

    std::ofstream out(csv_path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        throw std::runtime_error(
            "PnLTracker::export_trades_csv: cannot open '" + csv_path + "'");
    }

    // Header row.
    out << "Date Acquired,Date Sold,Description,Proceeds (mojos),"
           "Cost Basis (mojos),Gain or Loss (mojos),Term\n";

    for (const auto& rec : records) {
        // Only sell-side fills generate taxable events.
        if (rec.side != Side::Ask) {
            continue;
        }

        // T2-06: Use the stored acquisition timestamp for "Date Acquired".
        // Fall back to the sell date if no acquisition timestamp was recorded
        // (e.g. legacy records persisted before this fix).
        const std::string date_sold     = timestamp_to_date(rec.timestamp);
        const std::string date_acquired =
            (rec.acquisition_ts != Timestamp{})
                ? timestamp_to_date(rec.acquisition_ts)
                : date_sold;

        // [T9-FIX] Use double intermediates to avoid int64 overflow
        // (price_mojos * size_mojos can exceed 2^63 for realistic trades).
        const auto proceeds = static_cast<Mojo>(std::llround(
            static_cast<double>(rec.price_mojos)
            * static_cast<double>(rec.size_mojos)
            / static_cast<double>(kMojosPerXch)));
        const auto cost = static_cast<Mojo>(std::llround(
            static_cast<double>(rec.cost_basis_mojos)
            * static_cast<double>(rec.size_mojos)
            / static_cast<double>(kMojosPerXch)));
        const Mojo gain     = proceeds - cost;

        // T2-06: Determine holding period from actual acquisition date.
        // Positions held >= 1 year qualify for long-term capital gains.
        const char* term = "Short-term";
        if (rec.acquisition_ts != Timestamp{}) {
            const auto holding = rec.timestamp - rec.acquisition_ts;
            constexpr auto one_year =
                std::chrono::hours(365 * 24);  // Conservative: 365 days.
            if (holding >= one_year) {
                term = "Long-term";
            }
        }

        // Description follows IRS guidelines: "qty units of asset via pair".
        out << date_acquired << ","     // Date Acquired
            << date_sold << ","         // Date Sold
            << rec.size_mojos << " mojos " << rec.pair_name << ","
            << proceeds << ","
            << cost << ","
            << gain << ","
            << term << "\n";
    }

    out.flush();
    if (out.fail()) {
        throw std::runtime_error(
            "PnLTracker::export_trades_csv: write error on '" + csv_path + "'");
    }

    spdlog::info("PnLTracker::export_trades_csv: wrote {} to '{}'",
                  records.size(), csv_path);
}

// T2-06: Uses stored acquisition timestamps for proper short/long term split.
// T2-07: Non-const -- transitive from query_trades (ISO/IEC 5055).
RealizedGains PnLTracker::compute_realized_gains(int year)
{
    // Query all fills for the calendar year.
    const std::string start = std::to_string(year) + "-01-01T00:00:00.000Z";
    const std::string end   = std::to_string(year + 1) + "-01-01T00:00:00.000Z";

    const auto records = query_trades("", start, end);

    RealizedGains gains{};
    gains.tax_year = year;

    for (const auto& rec : records) {
        // Only sells produce realised gains.
        if (rec.side != Side::Ask) {
            continue;
        }

        // T2-06: Classify by actual holding period when an acquisition
        // timestamp is available.  Fall back to short-term for legacy
        // records that lack acquisition_ts (conservative / higher tax).
        if (rec.acquisition_ts != Timestamp{}) {
            const auto holding = rec.timestamp - rec.acquisition_ts;
            constexpr auto one_year = std::chrono::hours(365 * 24);
            if (holding >= one_year) {
                gains.long_term += rec.realized_pnl_mojos;
            } else {
                gains.short_term += rec.realized_pnl_mojos;
            }
        } else {
            gains.short_term += rec.realized_pnl_mojos;
        }
    }

    gains.total = gains.short_term + gains.long_term;

    spdlog::info("PnLTracker::compute_realized_gains year={} short={} long={} "
                 "total={}",
                 year, gains.short_term, gains.long_term, gains.total);

    return gains;
}

}  // namespace xop
