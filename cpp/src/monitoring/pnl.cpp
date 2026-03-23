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
//   ISO/IEC 5055        -- no unchecked casts, RAII resource management
//   ISO/IEC 25000       -- clear error handling, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- portable C++17, defined behaviour throughout

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
             block_height, offer_hash)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);
    )SQL";

    static constexpr const char* kQueryRangeSql = R"SQL(
        SELECT timestamp, trade_id, pair_name, side,
               price_mojos, size_mojos, fee_mojos,
               cost_basis_mojos, realized_pnl_mojos,
               block_height, offer_hash
        FROM trade_log
        WHERE timestamp >= ?1 AND timestamp < ?2
        ORDER BY timestamp ASC;
    )SQL";

    static constexpr const char* kQueryPairSql = R"SQL(
        SELECT timestamp, trade_id, pair_name, side,
               price_mojos, size_mojos, fee_mojos,
               cost_basis_mojos, realized_pnl_mojos,
               block_height, offer_hash
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
    if (!db_ || !stmt_insert_) {
        throw std::runtime_error(
            "PnLTracker::insert_trade: database not initialised");
    }

    // Reset the prepared statement for reuse.
    sqlite3_reset(stmt_insert_);
    sqlite3_clear_bindings(stmt_insert_);

    const std::string ts_str = timestamp_to_iso(record.timestamp);
    const char* side_str = to_string(record.side);

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
    sqlite3_bind_int(stmt_insert_,  10, static_cast<int>(record.block_height));
    sqlite3_bind_text(stmt_insert_, 11, record.offer_hash.c_str(), -1, SQLITE_TRANSIENT);

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
    rec.side = (side_text && std::strcmp(side_text, "Ask") == 0)
                   ? Side::Ask : Side::Bid;

    rec.price_mojos        = sqlite3_column_int64(stmt, 4);
    rec.size_mojos         = sqlite3_column_int64(stmt, 5);
    rec.fee_mojos          = sqlite3_column_int64(stmt, 6);
    rec.cost_basis_mojos   = sqlite3_column_int64(stmt, 7);
    rec.realized_pnl_mojos = sqlite3_column_int64(stmt, 8);
    rec.block_height       = static_cast<BlockHeight>(sqlite3_column_int(stmt, 9));

    const char* hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
    rec.offer_hash = hash ? hash : "";

    return rec;
}

}  // anonymous namespace

std::vector<TradeRecord> PnLTracker::get_trade_log(
    Timestamp start, Timestamp end) const
{
    if (!db_ || !stmt_query_range_) {
        throw std::runtime_error(
            "PnLTracker::get_trade_log: database not initialised");
    }

    const std::string start_str = timestamp_to_iso(start);
    const std::string end_str   = timestamp_to_iso(end);

    // The statement is logically const (read-only query) but SQLite's
    // step/reset API requires mutable access.  The const_cast is safe
    // because we only read rows, never modify the database.
    auto* stmt = const_cast<sqlite3_stmt*>(stmt_query_range_);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    sqlite3_bind_text(stmt, 1, start_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, end_str.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<TradeRecord> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(read_row(stmt));
    }

    return results;
}

std::vector<TradeRecord> PnLTracker::query_trades(
    const std::string& pair_name,
    const std::string& start,
    const std::string& end) const
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

    auto* stmt = const_cast<sqlite3_stmt*>(stmt_query_pair_);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    sqlite3_bind_text(stmt, 1, pair_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, start.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, end.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<TradeRecord> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(read_row(stmt));
    }

    return results;
}

// =========================================================================
// Fill recording and PnL attribution
// =========================================================================

void PnLTracker::record_fill(const Fill& fill, Mojo fee, Mojo cost_basis)
{
    // -- Step 1: Compute realised PnL for this fill -----------------------
    //
    // Spread PnL is realised only on sells (Ask side).  When we sell, the
    // realised PnL per unit = sell_price - cost_basis.
    //   realised = (price - cost_basis) * size   [for sells]
    //   realised = 0                              [for buys]
    //
    // Fee PnL is recorded separately for both sides.
    //
    // Inventory PnL is not updated here -- it is recalculated by
    // mark_to_market() which uses current market prices.

    Mojo realized_pnl = 0;
    if (fill.side == Side::Ask && cost_basis > 0) {
        // Realised spread PnL on the sell leg.
        //
        // The realised PnL is the surplus proceeds above the cost of
        // the units sold.  For weighted-average costing:
        //   proceeds     = price * size         (in quote mojos)
        //   cost_of_sold = cost_basis * size    (in quote mojos)
        //   realised     = proceeds - cost_of_sold
        //
        // Both price and cost_basis are expressed as "mojos-of-quote per
        // mojo-of-base", and size is in mojos-of-base, so:
        //   realised = (price - cost_basis) * size
        //
        // Overflow analysis: this can overflow int64 if the price spread
        // and size are both near 10^12.  In practice per-fill sizes on
        // Chia DEX are well under 10^9 mojos (~0.001 XCH), and price
        // differences are much smaller than the price itself, so the
        // product fits comfortably.  We perform the subtraction first to
        // reduce magnitude.
        //
        // The never-sell-at-loss constraint should ensure realized_pnl >= 0,
        // but we record the actual number regardless for audit accuracy.
        realized_pnl = (fill.price - cost_basis) * fill.size;
    }

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

    insert_trade(record);

    // -- Step 3: Update in-memory PnL accumulators -----------------------

    std::lock_guard<std::mutex> lock(mtx_);

    auto& ppnl = pair_pnl_[fill.pair_name];

    if (fill.side == Side::Ask) {
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
    double xch_usd_price)
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
        const Mojo balance       = get_balance(base_asset);
        const Mojo basis         = get_cost_basis(base_asset);

        // Inventory PnL = (current_price - cost_basis) * balance.
        // Both prices are in mojos-of-quote per mojo-of-base, and balance
        // is in mojos-of-base.  The product is mojos-of-quote.
        //
        // When cost_basis is zero (no position), inventory_pnl is zero.
        if (basis > 0 && balance > 0) {
            ppnl.inventory_pnl = (current_price - basis) * balance;
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
    if (pnl_history_.size() > kMaxSnapshots) {
        pnl_history_.erase(pnl_history_.begin());
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

        if (peak > 0) {
            const double dd = static_cast<double>(peak - snap.total_pnl)
                            / static_cast<double>(peak);
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

    // Aggregate from all pairs for today.
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

void PnLTracker::export_trades_csv(const std::string& start_date,
                                    const std::string& end_date,
                                    const std::string& csv_path) const
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

        const std::string date_str = timestamp_to_date(rec.timestamp);
        const Mojo proceeds = rec.price_mojos * rec.size_mojos;
        const Mojo cost     = rec.cost_basis_mojos * rec.size_mojos;
        const Mojo gain     = proceeds - cost;

        // Holding period: without per-lot acquisition dates we
        // conservatively classify everything as short-term.  A full
        // FIFO lot-tracking system would determine the actual term.
        const char* term = "Short-term";

        // Description follows IRS guidelines: "qty units of asset via pair".
        out << date_str << ","          // Date Acquired (approx)
            << date_str << ","          // Date Sold
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

RealizedGains PnLTracker::compute_realized_gains(int year) const
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

        // Without per-lot acquisition timestamps, all gains are
        // classified as short-term.  This is the conservative (higher
        // tax) treatment.  A future enhancement can match FIFO lots to
        // determine actual holding period.
        //
        // realised_pnl_mojos is already computed at fill time and stored
        // in the database.
        gains.short_term += rec.realized_pnl_mojos;
    }

    gains.total = gains.short_term + gains.long_term;

    spdlog::info("PnLTracker::compute_realized_gains year={} short={} long={} "
                 "total={}",
                 year, gains.short_term, gains.long_term, gains.total);

    return gains;
}

}  // namespace xop
