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

#include "xop/accounting/bridge_ingest.hpp"
#include "xop/accounting/reward_ingest.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

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

    // Busy timeout: retry for up to 5 s instead of throwing SQLITE_BUSY when
    // another connection (Database class, GUI, maintenance scripts) briefly
    // holds the write lock.  A thrown insert during fill processing would
    // permanently lose that fill's audit-trail row (PNL-DURABILITY).
    sqlite3_exec(db_, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);

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

    // [PNL-HISTORY 2026-07-30] Prepare the durable trade-history directory
    // next to the database file.  Every recorded fill is appended to
    // trade_history/trades_live.csv as a plain-text, restart-proof mirror of
    // trade_log (the SQLite file remains the authoritative store).
    try {
        namespace fs = std::filesystem;
        const fs::path dir =
            fs::path(db_path_).parent_path() / "trade_history";
        fs::create_directories(dir);
        history_csv_path_ = (dir / "trades_live.csv").string();
    } catch (const std::exception& e) {
        spdlog::warn("PnLTracker: could not create trade_history dir: {} -- "
                     "CSV mirroring disabled", e.what());
        history_csv_path_.clear();
    }

    spdlog::info("PnLTracker: database initialised (WAL mode, tables verified)");

    // [PNL-REHYDRATE 2026-07-30] Rebuild in-memory accumulators from the
    // persisted trade log so cumulative P&L survives restarts.  Must run
    // before any live fill is recorded.
    rehydrate_from_db();
}

void PnLTracker::rehydrate_from_db()
{
    if (!db_) {
        return;
    }

    // Aggregate per pair.  realized_pnl_mojos is stored in QUOTE-asset mojos
    // (0 for buys and for legacy sentinel-basis sells); fee_mojos is XCH
    // mojos.  These are exactly the units the live accumulators use, so the
    // replayed totals are unit-identical to what record_fill would have
    // accumulated in a single uninterrupted run.
    static constexpr const char* kRehydrateSql = R"SQL(
        SELECT pair_name,
               COUNT(*),
               COALESCE(SUM(realized_pnl_mojos), 0),
               COALESCE(SUM(CASE WHEN realized_pnl_mojos > 0
                                 THEN realized_pnl_mojos ELSE 0 END), 0),
               COALESCE(SUM(CASE WHEN realized_pnl_mojos < 0
                                 THEN -realized_pnl_mojos ELSE 0 END), 0),
               COALESCE(SUM(fee_mojos), 0),
               MIN(timestamp),
               MAX(timestamp)
        FROM trade_log
        GROUP BY pair_name;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, kRehydrateSql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("PnLTracker::rehydrate_from_db prepare failed: {}",
                      sqlite3_errmsg(db_));
        return;
    }

    std::lock_guard<std::mutex> lock(mtx_);

    std::size_t pairs = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* pair_text = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 0));
        if (!pair_text || pair_text[0] == '\0') {
            continue;
        }

        auto& ppnl = pair_pnl_[pair_text];
        ppnl.fill_count   = static_cast<std::uint64_t>(
            sqlite3_column_int64(stmt, 1));
        ppnl.spread_pnl   = sqlite3_column_int64(stmt, 2);
        ppnl.gross_profit = sqlite3_column_int64(stmt, 3);
        ppnl.gross_loss   = sqlite3_column_int64(stmt, 4);
        // record_fill accumulates fee_pnl -= fee per fill.
        ppnl.fee_pnl      = -sqlite3_column_int64(stmt, 5);

        const char* first_ts = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 6));
        const char* last_ts  = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 7));
        if (first_ts) ppnl.first_fill_ts = iso_to_timestamp(first_ts);
        if (last_ts)  ppnl.last_fill_ts  = iso_to_timestamp(last_ts);

        // Acquisition-timestamp weighting cannot be reconstructed from the
        // aggregate; it re-establishes itself from the next buy fill.

        total_pnl_.fill_count   += ppnl.fill_count;
        total_pnl_.spread_pnl   += ppnl.spread_pnl;
        total_pnl_.gross_profit += ppnl.gross_profit;
        total_pnl_.gross_loss   += ppnl.gross_loss;
        total_pnl_.fee_pnl      += ppnl.fee_pnl;
        if (total_pnl_.first_fill_ts == Timestamp{}
            || (ppnl.first_fill_ts != Timestamp{}
                && ppnl.first_fill_ts < total_pnl_.first_fill_ts)) {
            total_pnl_.first_fill_ts = ppnl.first_fill_ts;
        }
        if (ppnl.last_fill_ts > total_pnl_.last_fill_ts) {
            total_pnl_.last_fill_ts = ppnl.last_fill_ts;
        }
        ++pairs;
    }

    sqlite3_finalize(stmt);

    // [PNL-USD-TOTALS 2026-08-01] The aggregate spread figure is labelled
    // raw: it sums quote mojos across different quote assets and is not a
    // money amount.  (USD normalization happens at query time via
    // realized_usd_totals_locked once the engine registers the per-pair
    // conversions; they are not registered yet during init_database.)
    spdlog::info("PnLTracker: rehydrated {} pairs from trade_log "
                 "(fills={} spread_pnl_raw_quote_mojos={} fee_pnl={})",
                 pairs, total_pnl_.fill_count, total_pnl_.spread_pnl,
                 total_pnl_.fee_pnl);

    // -- [REWARD-INCOME 2026-08-01] Rebuild reward income from the ledger --
    // Reward receipts are journaled as 'reward' rows in ledger_entries
    // (same SQLite file: the engine constructs both Database and PnLTracker
    // on config.database.path), with the USD fair value at receipt embedded
    // in the note by accounting::reward_note.  Summing the parsed notes here
    // makes the accumulator restart-invariant like the USD totals.  The
    // table may not exist when the tracker runs standalone (tests, tools);
    // that is a clean "no rewards yet", not an error.
    static constexpr const char* kRewardSql = R"SQL(
        SELECT COALESCE(note, ''), delta_mojos
        FROM ledger_entries
        WHERE event_type = 'reward';
    )SQL";

    sqlite3_stmt* rstmt = nullptr;
    rc = sqlite3_prepare_v2(db_, kRewardSql, -1, &rstmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::debug("PnLTracker: no ledger_entries table for reward "
                      "rehydration ({}) -- reward income starts at 0",
                      sqlite3_errmsg(db_));
        return;
    }

    // NOTE: mtx_ is still held by the lock_guard taken for the trade_log
    // replay above (its scope is the rest of this function), so the
    // accumulator write below is already protected -- re-locking here would
    // self-deadlock on the non-recursive mutex.
    double      reward_usd  = 0.0;
    std::size_t reward_rows = 0;
    Mojo        reward_mojos = 0;
    while (sqlite3_step(rstmt) == SQLITE_ROW) {
        const char* note_text = reinterpret_cast<const char*>(
            sqlite3_column_text(rstmt, 0));
        reward_usd += accounting::parse_reward_fmv_usd(
            note_text ? note_text : "");
        reward_mojos += sqlite3_column_int64(rstmt, 1);
        ++reward_rows;
    }
    reward_income_usd_ = reward_usd;
    sqlite3_finalize(rstmt);

    if (reward_rows > 0) {
        spdlog::info("PnLTracker: rehydrated {} reward receipts from the "
                     "ledger ({} mojos, ${:.6f} income at receipt FMV)",
                     reward_rows, reward_mojos, reward_usd);
    }

    // -- [S19 2026-08-23] Rebuild net bridge deposits from the ledger ------
    // Bridge flows are journaled as bridge_deposit / bridge_withdrawal
    // rows with the SIGNED USD embedded in the note by
    // accounting::bridge_note.  Same restart-invariance treatment as the
    // reward block above (and the same table, so the prepare cannot fail
    // here after the reward prepare succeeded -- but check anyway).
    static constexpr const char* kBridgeSql = R"SQL(
        SELECT COALESCE(note, ''), delta_mojos
        FROM ledger_entries
        WHERE event_type IN ('bridge_deposit', 'bridge_withdrawal');
    )SQL";

    sqlite3_stmt* bstmt = nullptr;
    rc = sqlite3_prepare_v2(db_, kBridgeSql, -1, &bstmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::debug("PnLTracker: bridge rehydration prepare failed ({}) "
                      "-- net deposits start at 0", sqlite3_errmsg(db_));
        return;
    }

    double      bridge_usd  = 0.0;
    std::size_t bridge_rows = 0;
    Mojo        bridge_mojos = 0;
    while (sqlite3_step(bstmt) == SQLITE_ROW) {
        const char* note_text = reinterpret_cast<const char*>(
            sqlite3_column_text(bstmt, 0));
        bridge_usd += accounting::parse_bridge_flow_usd(
            note_text ? note_text : "");
        bridge_mojos += sqlite3_column_int64(bstmt, 1);
        ++bridge_rows;
    }
    net_deposits_usd_ = bridge_usd;
    sqlite3_finalize(bstmt);

    if (bridge_rows > 0) {
        spdlog::info("PnLTracker: rehydrated {} bridge flow(s) from the "
                     "ledger ({} mojos, ${:+.6f} net deposits)",
                     bridge_rows, bridge_mojos, bridge_usd);
    }
}

// =========================================================================
// Reward income ([REWARD-INCOME 2026-08-01])
// =========================================================================

void PnLTracker::add_reward_income_usd(double usd)
{
    // NaN/negative-safe: income is recognized at receipt and only grows.
    if (!(usd > 0.0) || !(usd < 1e12)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    reward_income_usd_ += usd;
}

void PnLTracker::add_net_deposit_usd(double usd)
{
    // Signed by design (withdrawals negative); NaN/huge-safe.
    if (!std::isfinite(usd) || !(std::fabs(usd) < 1e12) || usd == 0.0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    net_deposits_usd_ += usd;
}

double PnLTracker::get_net_deposits_usd() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return net_deposits_usd_;
}

double PnLTracker::get_reward_income_usd() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return reward_income_usd_;
}

void PnLTracker::set_pair_conversion(const std::string& pair_name,
                                     const std::string& base_asset_id,
                                     double base_mojos_per_unit,
                                     double quote_mojos_per_unit,
                                     double usd_per_quote_unit)
{
    if (pair_name.empty() || base_mojos_per_unit <= 0.0
        || quote_mojos_per_unit <= 0.0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    auto& conv = pair_conv_[pair_name];
    conv.base_asset_id       = base_asset_id;
    conv.base_mojos_per_unit = base_mojos_per_unit;
    conv.quote_mojos_per_unit = quote_mojos_per_unit;
    conv.usd_per_quote_unit  = usd_per_quote_unit;
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
    // A duplicate trade_id is not an error for this direct-insert entry
    // point: the row is already in the audit trail, which is the caller's
    // intent.  (record_fill needs the distinction and uses the bool.)
    static_cast<void>(insert_trade_unlocked(record));
}

bool PnLTracker::insert_trade_unlocked(const TradeRecord& record)
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
        // Idempotent replay: a UNIQUE violation on trade_id means this fill
        // was already journalled by a previous run (crash between the insert
        // and the engine's side effects, or a re-detection race).  Signal
        // "already recorded" instead of throwing so the caller can complete
        // any missing side effects without double-counting P&L.
        if (rc == SQLITE_CONSTRAINT) {
            spdlog::warn("PnLTracker::insert_trade: trade_id={} already "
                         "recorded -- skipping duplicate", record.trade_id);
            return false;
        }
        spdlog::error("PnLTracker::insert_trade: step failed (rc={}): {}",
                       rc, sqlite3_errmsg(db_));
        throw std::runtime_error(
            "PnLTracker::insert_trade: INSERT failed: "
            + std::string(sqlite3_errmsg(db_)));
    }

    spdlog::debug("PnLTracker::insert_trade: persisted trade_id={}",
                   record.trade_id);
    return true;
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

bool PnLTracker::record_fill(const Fill& fill, Mojo fee, Mojo cost_basis,
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

    if (!insert_trade_unlocked(record)) {
        // Already journalled by a prior run: the accumulators either counted
        // it live before the crash or picked it up via rehydrate_from_db().
        // Do not double-count; the caller completes its own side effects.
        return false;
    }

    // Mirror the fill to the durable trade-history CSV (best-effort).
    append_history_csv(record, realized_pnl);

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

    // [PNL-USD-TOTALS 2026-08-01] The cross-pair total is logged in USD.
    // The old raw total_pnl_.spread_pnl summed quote-asset mojos across
    // different quote currencies (one DBX fill worth ~$0.04 once jumped the
    // "total" from 2587 to 7173 because DBX mojos are ~70x cheaper than
    // wUSDC.b mojos).  Per-pair spread_pnl stays in that pair's quote mojos,
    // which is well-defined.
    spdlog::info("PnLTracker::record_fill pair={} side={} price={} size={} "
                 "realized_pnl={} fee={} pair_spread_pnl={} "
                 "spread_pnl_total_usd={:.4f}",
                 fill.pair_name, to_string(fill.side), fill.price, fill.size,
                 realized_pnl, fee, ppnl.spread_pnl,
                 realized_usd_totals_locked().spread_pnl);
    return true;
}

void PnLTracker::append_history_csv(const TradeRecord& record,
                                    Mojo realized_pnl)
{
    if (history_csv_path_.empty()) {
        return;
    }

    try {
        namespace fs = std::filesystem;
        const bool need_header = !fs::exists(history_csv_path_)
                              || fs::file_size(history_csv_path_) == 0;

        std::ofstream out(history_csv_path_,
                          std::ios::out | std::ios::app);
        if (!out.is_open()) {
            spdlog::warn("PnLTracker: cannot open trade-history CSV '{}'",
                         history_csv_path_);
            return;
        }

        if (need_header) {
            out << "timestamp_utc,trade_id,pair,side,"
                   "price_pseudo_mojos,size_base_mojos,"
                   "price_quote_per_base,size_base_units,quote_amount,"
                   "fee_xch_mojos,cost_basis_pseudo_mojos,"
                   "realized_pnl_quote_mojos,realized_pnl_usd,"
                   "block_height\n";
        }

        // Display-unit conversions need the pair's registered denominations;
        // when the pair is unknown the raw mojo columns still make the row
        // fully reconstructible.
        std::string price_disp, size_disp, quote_amt_disp, pnl_usd_disp;
        auto conv_it = pair_conv_.find(record.pair_name);
        if (conv_it != pair_conv_.end()) {
            const auto& conv = conv_it->second;
            const double price_units =
                static_cast<double>(record.price_mojos)
                / static_cast<double>(kMojosPerXch);
            const double size_units =
                static_cast<double>(record.size_mojos)
                / conv.base_mojos_per_unit;

            std::ostringstream tmp;
            tmp << std::setprecision(12) << price_units;
            price_disp = tmp.str();

            tmp.str(""); tmp << std::setprecision(12) << size_units;
            size_disp = tmp.str();

            tmp.str(""); tmp << std::setprecision(12)
                             << (price_units * size_units);
            quote_amt_disp = tmp.str();

            if (conv.usd_per_quote_unit > 0.0) {
                tmp.str("");
                tmp << std::setprecision(12)
                    << (static_cast<double>(realized_pnl)
                        / conv.quote_mojos_per_unit
                        * conv.usd_per_quote_unit);
                pnl_usd_disp = tmp.str();
            }
        }

        out << timestamp_to_iso(record.timestamp) << ','
            << record.trade_id << ','
            << record.pair_name << ','
            << ((record.side == Side::Bid) ? "bid" : "ask") << ','
            << record.price_mojos << ','
            << record.size_mojos << ','
            << price_disp << ','
            << size_disp << ','
            << quote_amt_disp << ','
            << record.fee_mojos << ','
            << record.cost_basis_mojos << ','
            << realized_pnl << ','
            << pnl_usd_disp << ','
            << record.block_height << '\n';

        out.flush();
        if (out.fail()) {
            spdlog::warn("PnLTracker: write error on trade-history CSV '{}'",
                         history_csv_path_);
        }
    } catch (const std::exception& e) {
        spdlog::warn("PnLTracker: trade-history CSV append failed: {}",
                     e.what());
    }
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

    // [PNL-MTM-DEDUP 2026-07-30] A base asset must be marked at most ONCE
    // per pass.  Balances are per ASSET but this loop is per PAIR, and XCH
    // is the base of three enabled pairs (XCH/wUSDC.b, XCH/BYC, XCH/DBX) --
    // marking each pair would value the entire XCH holding three times and
    // sum all three into the total.  This was latent while the old
    // display-symbol keying made every balance lookup miss (unrealized P&L
    // was structurally 0); fixing the keying made the triple-count live.
    // The first pair to produce a usable mark owns the asset's unrealized
    // P&L; the others report 0 so per-pair figures still add up to the total.
    std::unordered_map<std::string, bool> asset_marked;

    // [S20 2026-08-24] Pairs that hold a position but had no usable price
    // this cycle, deferred to a second pass.
    //
    // They carry the last trusted PRICE, not the last computed amount.
    // Balances and cost basis keep moving while a mid is ungraded -- the
    // engine goes on quoting -- so freezing the amount would retain the
    // unrealized P&L of a position that no longer exists: after a partial
    // fill the realized leg is booked while the stale unrealized leg
    // survives beside it, which is a false step in exactly the
    // rolling-window series this carry exists to keep smooth.  Recomputing
    // from the carried price and the CURRENT balance/basis keeps the two
    // legs consistent.
    //
    // Deferred rather than resolved inline so a carried price never
    // pre-empts a pair that has a live one: the prior mark belongs to one
    // specific pair (dedup zeroes the others) and pair_pnl_ is unordered.
    struct DeferredMark {
        std::string pair_name;
        std::string base_asset;
        Mojo        balance{0};
        Mojo        basis{0};
        double      unit_factor{1.0};
    };
    std::vector<DeferredMark> deferred;

    for (auto& [pair_name, ppnl] : pair_pnl_) {
        // [PNL-KEYING-FIX 2026-07-30] Resolve the CANONICAL base asset id
        // ("xch" / 64-hex CAT id) from the registered pair conversion.  The
        // previous code passed the display-symbol prefix of the pair name
        // ("XCH", "BYC") into callbacks whose stores are keyed by canonical
        // ids, so every balance/basis lookup missed and unrealized P&L was
        // structurally zero.  The substring fallback remains only for
        // unregistered pairs (legacy tests).
        std::string base_asset;
        const PairConversion* conv = nullptr;
        auto conv_it = pair_conv_.find(pair_name);
        if (conv_it != pair_conv_.end()) {
            conv = &conv_it->second;
            base_asset = conv->base_asset_id;
        } else {
            const auto slash_pos = pair_name.find('/');
            if (slash_pos == std::string::npos) {
                spdlog::warn("PnLTracker::mark_to_market: "
                             "invalid pair format '{}'", pair_name);
                continue;
            }
            base_asset = pair_name.substr(0, slash_pos);
        }

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
        Mojo       basis         = get_cost_basis(base_asset);

        // [PNL-BASIS-USD 2026-07-30] The engine stores cost basis in
        // USD-normalized pseudo-units so that one asset traded against
        // several quote currencies keeps a single coherent basis.  Convert
        // to THIS pair's quote pseudo-units before subtracting from the
        // (quote-denominated) smoothed mid.  A pair whose quote has no
        // known USD value cannot be marked -- treat basis as unknown.
        if (conv) {
            if (conv->usd_per_quote_unit > 0.0) {
                basis = static_cast<Mojo>(std::llround(
                    static_cast<double>(basis) / conv->usd_per_quote_unit));
            } else {
                basis = 0;
            }
        }

        // [PNL-MTM-DEDUP] Skip if another pair already marked this asset.
        const bool already_marked = asset_marked[base_asset];

        // [S20 2026-08-24] CARRY, don't zero, when the only thing missing
        // is a usable price.
        //
        // The `smoothed_price > 0` guard below was written so a missing
        // snapshot could not mark a position as a total loss -- but the
        // else-branch assigned 0, which does exactly that to the
        // UNREALIZED component: the position's mark vanishes for the
        // cycle and reappears the next, injecting a spurious step into
        // total_pnl_usd and therefore into the engine's rolling-window
        // loss series.  That series is a trading-loss detector; a data gap
        // must not look like a loss and then a gain.
        //
        // So a price-only failure now carries the previous mark, and the
        // caller can safely withhold a price it does not trust (an
        // ungraded mid) instead of being forced to pass one.  Position
        // reasons -- no basis, no balance, another pair already marked
        // this asset -- still zero, because those genuinely mean "no
        // unrealized P&L here".
        //
        // The carry is DEFERRED to a second pass rather than applied here.
        // pair_pnl_ is an unordered_map and one base asset spans several
        // pairs (XCH is the base of three), so claiming the asset on the
        // first unpriced pair visited would lock out a later pair that has
        // a perfectly good price -- freezing the mark on iteration order.
        // A carry is the LAST resort, valid only once every pair for the
        // asset has failed to supply one.
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

        const bool has_position = (basis > 0 && balance > 0);

        if (!already_marked && has_position && smoothed_price <= 0) {
            // [S20] Defer: this pair may still be markable from its
            // carried price, but only if no pair can price the asset live.
            deferred.push_back({pair_name, base_asset, balance, basis,
                                unit_factor});
            continue;
        }

        if (!already_marked && has_position && smoothed_price > 0) {
            // Equivalent to quote_mojos_for(balance, smoothed_price - basis,
            //                               base_denom, quote_denom)
            // with quote_denom/base_denom collapsed into unit_factor.
            ppnl.inventory_pnl = static_cast<Mojo>(std::llround(
                static_cast<double>(smoothed_price - basis)
                * static_cast<double>(balance)
                * unit_factor
                / static_cast<double>(kMojosPerXch)));
            asset_marked[base_asset] = true;
            mark_owner_[base_asset] = pair_name;   // [S20] carry source
        } else {
            ppnl.inventory_pnl = 0;
        }
    }

    // [S20 2026-08-24] Second pass: mark from the CARRIED PRICE, and only
    // for assets no pair could price live.
    //
    // price_ema_ already holds the last trusted smoothed price per pair,
    // so the mark is recomputed from it against this cycle's balance and
    // basis.  That keeps the unrealized leg consistent with any fills
    // booked into the realized leg while the mid was ungraded, which
    // carrying the previous AMOUNT could not do.
    // Which deferred pair may carry an asset: the one that last marked it
    // live.  price_ema_ existence is NOT that test -- every pair with a
    // positive price updates its own EMA before dedup runs, so an asset's
    // pairs hold EMAs of differing ages, and the first one an unordered
    // map yields could be the stalest.  It would also be denominated in a
    // different quote asset, since each pair's EMA and basis conversion
    // live in that pair's own quote units, so borrowing another pair's EMA
    // is a unit error as well as a staleness one.  With no recorded owner
    // (nothing has ever marked this asset) any deferred pair may carry its
    // own EMA -- they are all equally unproven.
    std::unordered_map<std::string, bool> asset_has_owner;
    for (const auto& d : deferred) {
        auto own_it = mark_owner_.find(d.base_asset);
        asset_has_owner[d.base_asset] =
            (own_it != mark_owner_.end() && !own_it->second.empty());
    }

    for (const auto& d : deferred) {
        auto it = pair_pnl_.find(d.pair_name);
        if (it == pair_pnl_.end()) continue;

        if (asset_marked[d.base_asset]) {
            // Priced live by another pair, or already carried by one.
            it->second.inventory_pnl = 0;
            continue;
        }

        if (asset_has_owner[d.base_asset]) {
            auto own_it = mark_owner_.find(d.base_asset);
            if (own_it == mark_owner_.end() || own_it->second != d.pair_name) {
                // Some other pair owns this asset's mark; it will carry.
                it->second.inventory_pnl = 0;
                continue;
            }
        }

        auto ema_it = price_ema_.find(d.pair_name);
        if (ema_it == price_ema_.end() || ema_it->second <= 0.0) {
            // Never priced -- nothing to carry.
            it->second.inventory_pnl = 0;
            continue;
        }

        const Mojo carried = static_cast<Mojo>(ema_it->second);
        it->second.inventory_pnl = static_cast<Mojo>(std::llround(
            static_cast<double>(carried - d.basis)
            * static_cast<double>(d.balance)
            * d.unit_factor
            / static_cast<double>(kMojosPerXch)));
        asset_marked[d.base_asset] = true;

        spdlog::debug("PnLTracker::mark_to_market: no live price for {} -- "
                      "marked {} from carried price {} against current "
                      "balance {} (result {})",
                      d.base_asset, d.pair_name, carried, d.balance,
                      it->second.inventory_pnl);
    }

    // Total is summed from the per-pair figures AFTER both passes, so the
    // two can never disagree (a deferred pair's contribution is not known
    // until the second pass decides whether it marks or zeroes).
    total_pnl_.inventory_pnl = 0;
    for (const auto& [name, ppnl] : pair_pnl_) {
        total_pnl_.inventory_pnl += ppnl.inventory_pnl;
    }

    // Record a PnL snapshot for Sharpe/drawdown analytics.
    // [PNL-USD-TOTALS 2026-08-01] Snapshot the USD-normalized total.  The
    // old raw mojo sum (spread + inventory + fee) mixed quote currencies
    // across pairs with an XCH-mojo fee leg, so the Sharpe/drawdown series
    // was dominated by whichever pair had the cheapest quote mojos (DBX).
    // fill_usd_components_locked is the same per-pair conversion path the
    // displayed USD figures use; mtx_ is held throughout this method.
    PnLSummary usd_tmp{};
    fill_usd_components_locked(usd_tmp, /*pair_name=*/"");

    PnLSnapshot snap;
    snap.timestamp     = std::chrono::system_clock::now();
    snap.total_pnl_usd = usd_tmp.total_pnl_usd;

    pnl_history_.push_back(snap);

    // Cap the history buffer to prevent unbounded memory growth.
    // T3-21: pop_front() on std::deque is O(1), replacing the prior
    // std::vector::erase(begin()) which was O(n) per trim.
    if (pnl_history_.size() > kMaxSnapshots) {
        pnl_history_.pop_front();
    }

    spdlog::debug("PnLTracker::mark_to_market realized_usd={:.4f} "
                  "unrealized_usd={:.4f} fee_usd={:.4f} total_usd={:.4f} "
                  "xch_usd={:.4f}",
                  usd_tmp.realized_pnl_usd, usd_tmp.unrealized_pnl_usd,
                  usd_tmp.fee_pnl_usd, usd_tmp.total_pnl_usd, xch_usd_rate_);
}

// =========================================================================
// PnL queries
// =========================================================================

double PnLTracker::annualized_sharpe(double mean_return,
                                     double stdev_return,
                                     double avg_interval_seconds)
{
    // Sharpe = mean(returns) / stdev(returns) * sqrt(periods_per_year)
    //
    // [SHARPE-CADENCE 2026-08-01] periods_per_year MUST come from the
    // MEASURED snapshot cadence, never a constant.  Snapshots are taken
    // once per engine heartbeat, and the heartbeat is ~19 minutes (median
    // inter-snapshot spacing 1,165 s measured from the snapshots table),
    // not the 52-second chain block time an earlier comment here claimed.
    // Annualizing 19-minute returns with the 52-second constant would
    // inflate Sharpe by sqrt(1165 / 52) ~ 4.7x.  This helper is public and
    // pinned by a test so the constant cannot quietly come back.
    if (stdev_return < 1e-12 || avg_interval_seconds <= 0.0) {
        return 0.0;
    }
    constexpr double seconds_per_year = 365.25 * 24.0 * 3600.0;
    const double periods_per_year = seconds_per_year / avg_interval_seconds;
    return (mean_return / stdev_return) * std::sqrt(periods_per_year);
}

double PnLTracker::compute_sharpe() const
{
    // Annualised Sharpe ratio from the PnL snapshot history.
    //
    // Returns are differences of the USD-normalized total between
    // CONSECUTIVE snapshots (one per engine heartbeat, ~19 min); the
    // annualization uses the cadence measured from the snapshots' own
    // timestamps -- see annualized_sharpe.  The USD normalization matters
    // because raw quote mojos across pairs are not commensurable
    // (PNL-USD-TOTALS 2026-08-01).

    if (pnl_history_.size() < 2) {
        return 0.0;
    }

    const std::size_t n = pnl_history_.size() - 1;
    std::vector<double> returns(n);

    for (std::size_t i = 0; i < n; ++i) {
        returns[i] = pnl_history_[i + 1].total_pnl_usd
                   - pnl_history_[i].total_pnl_usd;
    }

    const double mean = std::accumulate(returns.begin(), returns.end(), 0.0)
                       / static_cast<double>(n);

    double sum_sq = 0.0;
    for (double r : returns) {
        const double diff = r - mean;
        sum_sq += diff * diff;
    }
    const double stdev = std::sqrt(sum_sq / static_cast<double>(n));

    // Annualisation: estimate the average interval between snapshots from
    // the actual timestamps (measured cadence), never a constant -- see
    // annualized_sharpe for the units note and the 4.7x failure mode.
    const auto span = pnl_history_.back().timestamp - pnl_history_.front().timestamp;
    const double span_seconds = static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(span).count());

    if (span_seconds < 1.0) {
        return 0.0;
    }

    const double avg_interval = span_seconds / static_cast<double>(n);
    return annualized_sharpe(mean, stdev, avg_interval);
}

double PnLTracker::compute_max_drawdown() const
{
    // Maximum drawdown = largest peak-to-trough decline as a fraction
    // of the peak value.  Expressed as a non-negative number in [0, 1].

    if (pnl_history_.size() < 2) {
        return 0.0;
    }

    double peak = pnl_history_.front().total_pnl_usd;
    double max_dd = 0.0;

    for (const auto& snap : pnl_history_) {
        if (snap.total_pnl_usd > peak) {
            peak = snap.total_pnl_usd;
        }

        // [T9-FIX] Track drawdown even when PnL is negative.
        // Use absolute peak value to avoid division by (near-)zero.
        if (std::abs(peak) > 1e-9) {
            const double dd = (peak - snap.total_pnl_usd) / std::abs(peak);
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

double PnLTracker::quote_mojos_to_usd_locked(const std::string& pair_name,
                                             Mojo quote_mojos_int) const
{
    const double quote_mojos = static_cast<double>(quote_mojos_int);

    auto it = pair_conv_.find(pair_name);
    if (it != pair_conv_.end()
        && it->second.usd_per_quote_unit > 0.0
        && it->second.quote_mojos_per_unit > 0.0) {
        return quote_mojos / it->second.quote_mojos_per_unit
                           * it->second.usd_per_quote_unit;
    }

    // Retired-pair fallback: rehydration resurrects pairs that are no longer
    // in config (e.g. "XCH/wUSDC" before the wUSDC.b migration), so the
    // engine never registers a conversion for them.  Without this, their
    // historical P&L silently vanishes from the engine's USD total while the
    // GUI's SQL still counts it (pnl_usdc_expr matches LIKE '%/wUSDC%'), and
    // the two displays disagree.  Mirror that heuristic exactly: USD-pegged
    // CAT quotes at 1e3 mojos per $1 unit.  Anything else stays excluded --
    // an unpegged quote (DBX) has no defensible USD value here.
    const auto slash = pair_name.find('/');
    if (slash == std::string::npos) {
        return 0.0;
    }
    const std::string quote = pair_name.substr(slash + 1);
    if (quote == "wUSDC" || quote == "wUSDC.b" || quote == "USDS"
        || quote == "BYC") {
        constexpr double kCatMojosPerUnit = 1'000.0;
        return quote_mojos / kCatMojosPerUnit;
    }

    return 0.0;
}

PnLTracker::UsdRealizedTotals PnLTracker::realized_usd_totals_locked() const
{
    // [PNL-USD-TOTALS 2026-08-01] Derive the cross-pair realized totals
    // from the per-pair quote-mojo accumulators at query time, through the
    // same quote_mojos_to_usd_locked conversion that produces the displayed
    // usd_realized figure.  Deriving (rather than accumulating a separate
    // USD counter) guarantees the total always equals the sum of the
    // per-pair USD figures AND that a restart reproduces the same number:
    // rehydrate_from_db rebuilds exactly these per-pair accumulators, so
    // live and rehydrated totals are identical by construction.
    UsdRealizedTotals t;
    for (const auto& [name, ppnl] : pair_pnl_) {
        t.spread_pnl   += quote_mojos_to_usd_locked(name, ppnl.spread_pnl);
        t.gross_profit += quote_mojos_to_usd_locked(name, ppnl.gross_profit);
        t.gross_loss   += quote_mojos_to_usd_locked(name, ppnl.gross_loss);
    }
    return t;
}

void PnLTracker::fill_usd_components_locked(PnLSummary& s,
                                            const std::string& pair_name) const
{
    // [PNL-UNITS 2026-07-30] The mojo totals mix per-pair quote currencies
    // (wUSDC.b / BYC / DBX mojos) with an XCH-mojo fee leg, so the old
    // single "/kMojosPerXch * xch_usd" conversion was wrong by ~1e9.
    // Convert realized and unrealized per pair with that pair's own
    // quote-USD factor; fees are XCH-denominated and use the XCH rate.
    double realized_usd   = 0.0;
    double unrealized_usd = 0.0;
    Mojo   fee_mojos      = 0;

    if (pair_name.empty()) {
        for (const auto& [name, ppnl] : pair_pnl_) {
            realized_usd   += quote_mojos_to_usd_locked(name, ppnl.spread_pnl);
            unrealized_usd += quote_mojos_to_usd_locked(name,
                                                        ppnl.inventory_pnl);
        }
        fee_mojos = total_pnl_.fee_pnl;
    } else {
        auto it = pair_pnl_.find(pair_name);
        if (it != pair_pnl_.end()) {
            realized_usd   = quote_mojos_to_usd_locked(pair_name,
                                                       it->second.spread_pnl);
            unrealized_usd = quote_mojos_to_usd_locked(
                pair_name, it->second.inventory_pnl);
            fee_mojos = it->second.fee_pnl;
        }
    }

    const double fee_usd = static_cast<double>(fee_mojos)
                         / static_cast<double>(kMojosPerXch) * xch_usd_rate_;

    s.realized_pnl_usd   = realized_usd;
    s.unrealized_pnl_usd = unrealized_usd;
    s.fee_pnl_usd        = fee_usd;
    s.total_pnl_usd      = realized_usd + unrealized_usd + fee_usd;
}

PnLSummary PnLTracker::get_total_pnl() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    PnLSummary s = build_summary(total_pnl_, xch_usd_rate_);
    fill_usd_components_locked(s, /*pair_name=*/"");

    // [REWARD-INCOME 2026-08-01] Surfaced, NOT summed: reward income is
    // other income (recognized at receipt FMV), deliberately kept out of
    // total_pnl_usd so the trading P&L stays a measure of trading.
    s.reward_income_usd = reward_income_usd_;
    s.net_deposits_usd  = net_deposits_usd_;

    // [PNL-USD-TOTALS 2026-08-01] Rebuild the cross-pair profit factor from
    // USD-normalized grosses.  build_summary's ratio over total_pnl_'s raw
    // sums mixed quote currencies (a $0.04 DBX loss weighed like a $3 one
    // against wUSDC.b profits), which distorted the ratio arbitrarily.
    // Per-pair summaries keep the raw ratio -- within one quote asset it is
    // unit-invariant and identical to the USD ratio.
    const auto usd = realized_usd_totals_locked();
    if (usd.gross_loss > 1e-12) {
        s.profit_factor = usd.gross_profit / usd.gross_loss;
    } else if (usd.gross_profit > 1e-12) {
        s.profit_factor = 1e9;  // Effectively infinite (no losses).
    } else {
        s.profit_factor = 0.0;  // No convertible realized PnL.
    }
    return s;
}

PnLSummary PnLTracker::get_pair_pnl(const std::string& pair_name) const
{
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = pair_pnl_.find(pair_name);
    if (it == pair_pnl_.end()) {
        return PnLSummary{};
    }
    PnLSummary s = build_summary(it->second, xch_usd_rate_);
    fill_usd_components_locked(s, pair_name);
    return s;
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
    // [PNL-USD-TOTALS 2026-08-01] The Mojo fields below are RAW cross-pair
    // quote-mojo sums (mixed quote currencies plus an XCH-mojo fee leg) --
    // total_pnl_usd is the only money figure here.  Currently unused by
    // the engine; kept raw for API compatibility.
    ds.spread_pnl    = total_pnl_.spread_pnl;
    ds.inventory_pnl = total_pnl_.inventory_pnl;
    ds.fee_pnl       = total_pnl_.fee_pnl;
    ds.total_pnl     = total_pnl_.spread_pnl
                     + total_pnl_.inventory_pnl
                     + total_pnl_.fee_pnl;

    // [PNL-UNITS 2026-07-30] Per-pair USD conversion; see get_total_pnl.
    {
        PnLSummary tmp{};
        fill_usd_components_locked(tmp, /*pair_name=*/"");
        ds.total_pnl_usd = tmp.total_pnl_usd;
    }

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

        // [PNL-UNITS 2026-07-30] Route through the canonical quote_mojos_for
        // helper.  This site was missed by the v0.7.46 centralization and
        // still used the pre-v0.7.45 formula (1e9-inflated for XCH/CAT
        // pairs).  Denominations come from the registered pair conversion;
        // unregistered pairs fall back to the raw (legacy) formula so the
        // export degrades rather than throws.
        double base_denom  = static_cast<double>(kMojosPerXch);
        double quote_denom = static_cast<double>(kMojosPerXch);
        {
            std::lock_guard<std::mutex> conv_lock(mtx_);
            auto conv_it = pair_conv_.find(rec.pair_name);
            if (conv_it != pair_conv_.end()) {
                base_denom  = conv_it->second.base_mojos_per_unit;
                quote_denom = conv_it->second.quote_mojos_per_unit;
            }
        }
        const auto proceeds = static_cast<Mojo>(std::llround(quote_mojos_for(
            static_cast<double>(rec.size_mojos),
            static_cast<double>(rec.price_mojos),
            base_denom, quote_denom)));
        const auto cost = static_cast<Mojo>(std::llround(quote_mojos_for(
            static_cast<double>(rec.size_mojos),
            static_cast<double>(rec.cost_basis_mojos),
            base_denom, quote_denom)));
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
