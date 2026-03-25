// pnl.hpp -- PnL attribution engine and trade logging for XOPTrader.
//
// Three-component PnL attribution:
//   1. Spread PnL   -- revenue from bid-ask spread capture on matched fills.
//   2. Inventory PnL -- mark-to-market gains/losses on held inventory.
//   3. Fee PnL       -- net blockchain fees paid minus DBX/AMM income earned.
//
// Trade logging uses an append-only SQLite table that serves as the
// authoritative audit trail.  Every settled fill is persisted before any
// in-memory state is updated, guaranteeing crash-safe recoverability.
//
// Tax reporting helpers produce IRS Form 8949 compatible CSV exports and
// compute realized capital gains split by holding period.
//
// All monetary values are stored as int64_t mojos.  No floating-point
// representation is used for money at rest or in transit.  Doubles appear
// only in derived analytics (ratios, rates, Sharpe) that are dimensionless.
//
// Thread safety:
//   A single std::mutex protects all mutable in-memory state.  The SQLite
//   database is opened in WAL mode with SERIALIZED threading so that
//   concurrent readers do not block writers.  No method nests locks.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- append-only audit trail, parameterised queries
//   ISO/IEC 5055       -- no unchecked arithmetic on monetary paths
//   ISO/IEC 25000      -- single-responsibility, documented interfaces
//   ISO/IEC JTC 1/SC 22 -- well-defined integral types, no UB

#ifndef XOP_MONITORING_PNL_HPP
#define XOP_MONITORING_PNL_HPP

#include "xop/types.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declare the opaque sqlite3 handle to avoid pulling the full
// SQLite header into every translation unit that includes this header.
struct sqlite3;
struct sqlite3_stmt;

namespace xop {

// ---------------------------------------------------------------------------
// TradeRecord -- a single settled fill persisted to the audit trail.
//
// Maps 1:1 to a row in the trade_log SQLite table.  Every field that
// represents money is stored as int64_t mojos.
// ---------------------------------------------------------------------------

struct TradeRecord {
    Timestamp   timestamp;            ///< Wall-clock time the fill was detected.
    std::string trade_id;             ///< Globally unique fill identifier.
    std::string pair_name;            ///< Trading pair, e.g. "XCH/wUSDC".
    Side        side;                 ///< Bid (buy) or Ask (sell).
    Mojo        price_mojos;          ///< Execution price in mojos.
    Mojo        size_mojos;           ///< Filled quantity in mojos of base.
    Mojo        fee_mojos;            ///< Blockchain fee paid (negative cost).
    Mojo        cost_basis_mojos;     ///< Cost basis at time of fill (mojos).
    Mojo        realized_pnl_mojos;   ///< Realized PnL on this fill (mojos).
    BlockHeight block_height;         ///< Settlement block number.
    std::string offer_hash;           ///< Chia spend-bundle hash.
    Timestamp   acquisition_ts;       ///< Weighted-average acquisition timestamp
                                      ///< for the position sold (sells only).
                                      ///< Used by IRS Form 8949 CSV export.
};

// ---------------------------------------------------------------------------
// PnLSummary -- snapshot of PnL attribution and derived risk analytics.
//
// Returned by get_total_pnl() and get_pair_pnl().  All monetary fields
// are in mojos except total_pnl_usd which converts at the current rate.
// ---------------------------------------------------------------------------

struct PnLSummary {
    Mojo   spread_pnl;              ///< Cumulative spread capture (mojos).
    Mojo   inventory_pnl;           ///< Current mark-to-market on inventory.
    Mojo   fee_pnl;                 ///< Net fees: income minus costs (mojos).
    Mojo   total_pnl;               ///< spread + inventory + fee (mojos).
    double total_pnl_usd;           ///< total_pnl converted at XCH/USD rate.
    double sharpe_ratio;            ///< Annualised Sharpe ratio.
    double max_drawdown;            ///< Maximum peak-to-trough decline [0,1].
    double profit_factor;           ///< gross_profit / gross_loss (>1 good).
    std::uint64_t fill_count;       ///< Total number of settled fills.
    double fill_rate_per_hour;      ///< Fills per wall-clock hour.
    double adverse_selection_rate;  ///< Fraction of fills with adverse move.
};

// ---------------------------------------------------------------------------
// DailySummary -- end-of-day aggregation for monitoring dashboards.
// ---------------------------------------------------------------------------

struct DailySummary {
    std::string date;               ///< ISO-8601 date string "YYYY-MM-DD".
    Mojo   spread_pnl;              ///< Day's spread capture (mojos).
    Mojo   inventory_pnl;           ///< EOD mark-to-market (mojos).
    Mojo   fee_pnl;                 ///< Day's net fees (mojos).
    Mojo   total_pnl;               ///< Day's total PnL (mojos).
    double total_pnl_usd;           ///< Day's PnL in USD.
    std::uint64_t fill_count;       ///< Number of fills on this day.
    double fill_rate_per_hour;      ///< Average fill rate for the day.
    Mojo   gross_profit;            ///< Sum of positive realised PnL (mojos).
    Mojo   gross_loss;              ///< Sum of negative realised PnL (mojos).
};

// ---------------------------------------------------------------------------
// RealizedGains -- annual capital gains split by holding period for tax.
// ---------------------------------------------------------------------------

struct RealizedGains {
    int    tax_year;                ///< Calendar year, e.g. 2026.
    Mojo   short_term;              ///< Gains on positions held < 1 year.
    Mojo   long_term;               ///< Gains on positions held >= 1 year.
    Mojo   total;                   ///< short_term + long_term.
};

// ---------------------------------------------------------------------------
// PnLTracker -- the PnL attribution engine and trade logging database.
//
// Owns:
//   - An SQLite database connection for the append-only trade log.
//   - In-memory running PnL accumulators per pair and aggregate.
//   - A ring buffer of recent PnL snapshots for Sharpe / drawdown.
//
// Lifecycle:
//   1. Construct with a database file path.
//   2. Call init_database() to create/verify the schema.
//   3. Call record_fill() on every settled fill.
//   4. Call mark_to_market() periodically (e.g. every heartbeat).
//   5. Query PnL via get_total_pnl(), get_pair_pnl(), get_daily_summary().
//   6. Destructor closes the SQLite connection.
// ---------------------------------------------------------------------------

class PnLTracker {
public:
    /// Construct a PnLTracker.
    ///
    /// @param db_path  Filesystem path for the SQLite database file.
    ///                 Created if it does not exist.  Parent directory must
    ///                 exist.
    explicit PnLTracker(const std::string& db_path);

    /// Destructor.  Finalises prepared statements and closes the database.
    ~PnLTracker();

    // Non-copyable, non-movable: the SQLite connection and prepared
    // statements are not safely relocatable.
    PnLTracker(const PnLTracker&)            = delete;
    PnLTracker& operator=(const PnLTracker&) = delete;
    PnLTracker(PnLTracker&&)                 = delete;
    PnLTracker& operator=(PnLTracker&&)      = delete;

    // -- Database lifecycle -----------------------------------------------

    /// Create or verify the trade_log table and its indices.
    /// Safe to call repeatedly (IF NOT EXISTS).  Must be called before
    /// any other method that touches the database.
    void init_database();

    // -- Fill recording ---------------------------------------------------

    /// Record a settled fill: persist to SQLite, then update in-memory PnL.
    ///
    /// The write to SQLite happens first so that the audit trail is
    /// crash-consistent even if the process terminates before in-memory
    /// state is updated.
    ///
    /// @param fill  The Fill struct from the offer-monitoring subsystem.
    /// @param fee   Blockchain fee paid for this settlement (mojos, >= 0).
    /// @param cost_basis  Weighted-average cost basis at the time of fill.
    void record_fill(const Fill& fill, Mojo fee, Mojo cost_basis);

    /// Record a fee event that is not associated with a specific fill.
    /// Used for DBX incentive income (positive) and standalone blockchain
    /// fees (negative).
    ///
    /// @param pair_name  Trading pair this fee relates to ("" for global).
    /// @param amount     Fee amount in mojos (positive = income).
    void record_fee(const std::string& pair_name, Mojo amount);

    // -- Mark-to-market ---------------------------------------------------

    /// Recalculate inventory PnL for every tracked pair using current
    /// market prices.  Call this once per heartbeat (~52 seconds).
    ///
    /// @param get_price  Callback: given (pair_name, asset_id) returns the
    ///                   current mid-price in mojos.  Return 0 if unknown.
    /// @param get_balance Callback: given (asset_id) returns current
    ///                    balance in mojos.
    /// @param get_cost_basis Callback: given (asset_id) returns the
    ///                       weighted-average cost basis in mojos.
    /// @param xch_usd_price  Current XCH/USD rate expressed as a double
    ///                       (e.g. 2.70).  Used for USD conversion only.
    void mark_to_market(
        const std::function<Mojo(const std::string& /*pair*/,
                                  const std::string& /*asset*/)>& get_price,
        const std::function<Mojo(const std::string& /*asset*/)>& get_balance,
        const std::function<Mojo(const std::string& /*asset*/)>& get_cost_basis,
        double xch_usd_price);

    // -- PnL queries ------------------------------------------------------

    /// Aggregate PnL across all pairs.
    [[nodiscard]] PnLSummary get_total_pnl() const;

    /// PnL for a single pair.  Returns a zeroed summary if the pair has
    /// never been traded.
    [[nodiscard]] PnLSummary get_pair_pnl(const std::string& pair_name) const;

    /// Summary for the current calendar day (UTC).
    [[nodiscard]] DailySummary get_daily_summary() const;

    // -- Trade log queries ------------------------------------------------

    /// Retrieve trade records within a time range from the SQLite database.
    ///
    /// T2-07: Non-const because SQLite prepared-statement bind/step/reset
    /// mutates statement state.  Removing const avoids the prior const_cast
    /// which was unsafe under concurrent access (ISO/IEC 5055, CWE-362).
    ///
    /// @param start  Inclusive lower bound (UTC).
    /// @param end    Exclusive upper bound (UTC).
    /// @return Records ordered by timestamp ascending.
    [[nodiscard]] std::vector<TradeRecord> get_trade_log(
        Timestamp start, Timestamp end);

    // -- Trade persistence (direct SQL) -----------------------------------

    /// Insert a single trade record into the SQLite trade_log table.
    /// Uses a parameterised prepared statement to prevent SQL injection.
    /// Thread-safe: acquires mtx_ internally.
    void insert_trade(const TradeRecord& record);

    /// Query trades for a specific pair within a time range.
    ///
    /// T2-07: Non-const because SQLite prepared-statement bind/step/reset
    /// mutates statement state (ISO/IEC 5055, CWE-362).
    ///
    /// @param pair_name  Filter by pair (empty string = all pairs).
    /// @param start      Inclusive lower bound ISO-8601 string.
    /// @param end        Exclusive upper bound ISO-8601 string.
    [[nodiscard]] std::vector<TradeRecord> query_trades(
        const std::string& pair_name,
        const std::string& start,
        const std::string& end);

    // -- Tax reporting ----------------------------------------------------

    /// Export trades to an IRS Form 8949 compatible CSV file.
    ///
    /// Columns: Date Acquired, Date Sold, Description, Proceeds, Cost Basis,
    ///          Gain or Loss, Short/Long Term.
    ///
    /// @param start_date  ISO-8601 date "YYYY-MM-DD" inclusive.
    /// @param end_date    ISO-8601 date "YYYY-MM-DD" exclusive.
    /// @param csv_path    Output file path.  Overwritten if it exists.
    /// T2-07: Non-const -- transitive from query_trades (ISO/IEC 5055).
    void export_trades_csv(const std::string& start_date,
                           const std::string& end_date,
                           const std::string& csv_path);

    /// Compute realised capital gains for a tax year, split by holding
    /// period (short-term < 1 year, long-term >= 1 year).
    /// T2-07: Non-const -- transitive from query_trades (ISO/IEC 5055).
    [[nodiscard]] RealizedGains compute_realized_gains(int year);

    // -- Timestamp utilities (public for use by read helpers) -------------

    /// Format a Timestamp as an ISO-8601 UTC string for SQLite storage.
    [[nodiscard]] static std::string timestamp_to_iso(Timestamp ts);

    /// Parse an ISO-8601 UTC string back into a Timestamp.
    [[nodiscard]] static Timestamp iso_to_timestamp(const std::string& iso);

    /// Format a Timestamp as a date-only string "YYYY-MM-DD".
    [[nodiscard]] static std::string timestamp_to_date(Timestamp ts);

private:
    // -- Internal types ---------------------------------------------------

    /// Per-pair running PnL accumulators.  Updated on every fill and
    /// mark-to-market cycle.
    struct PairPnL {
        Mojo   spread_pnl       = 0;  ///< Cumulative spread capture.
        Mojo   inventory_pnl    = 0;  ///< Latest mark-to-market value.
        Mojo   fee_pnl          = 0;  ///< Net fees (income - costs).
        Mojo   gross_profit     = 0;  ///< Sum of positive realized PnL.
        Mojo   gross_loss       = 0;  ///< Sum of negative realized PnL (stored positive).
        std::uint64_t fill_count = 0; ///< Number of settled fills.
        std::uint64_t adverse_fills = 0; ///< Fills followed by adverse price move.
        Timestamp first_fill_ts = {}; ///< Timestamp of the first fill.
        Timestamp last_fill_ts  = {}; ///< Timestamp of the most recent fill.
        Timestamp avg_acquisition_ts = {}; ///< Weighted-average acquisition time
                                           ///< across buys for this pair.  Used to
                                           ///< populate "Date Acquired" in IRS
                                           ///< Form 8949 CSV exports (T2-06).
        Mojo      acquisition_qty    = 0;  ///< Running quantity for acquisition
                                           ///< timestamp weighting (mojos).
    };

    /// A single PnL snapshot for Sharpe ratio and drawdown calculation.
    struct PnLSnapshot {
        Timestamp timestamp;
        Mojo      total_pnl;
    };

    /// Compute annualised Sharpe ratio from the PnL snapshot history.
    [[nodiscard]] double compute_sharpe() const;

    /// Compute maximum drawdown from the PnL snapshot history.
    [[nodiscard]] double compute_max_drawdown() const;

    /// Build a PnLSummary from a PairPnL accumulator.
    [[nodiscard]] PnLSummary build_summary(const PairPnL& ppnl,
                                            double xch_usd) const;

    /// Lock-free insert helper; caller must already hold mtx_.
    /// ISO/IEC 5055 -- CWE-362: separated to avoid deadlock with record_fill.
    void insert_trade_unlocked(const TradeRecord& record);

    /// Finalise and null-out a prepared statement, ignoring errors.
    static void finalize_stmt(sqlite3_stmt*& stmt) noexcept;

    // -- Data members -----------------------------------------------------

    std::string   db_path_;                      ///< SQLite file path.
    sqlite3*      db_ = nullptr;                 ///< SQLite connection.

    // Prepared statements -- created once in init_database(), reused.
    sqlite3_stmt* stmt_insert_    = nullptr;     ///< INSERT INTO trade_log.
    sqlite3_stmt* stmt_query_range_ = nullptr;   ///< SELECT by timestamp range.
    sqlite3_stmt* stmt_query_pair_  = nullptr;   ///< SELECT by pair + range.

    mutable std::mutex mtx_;                     ///< Protects all mutable state below.

    /// Per-pair PnL accumulators keyed by pair_name.
    std::unordered_map<std::string, PairPnL> pair_pnl_;

    /// Global (aggregate) PnL accumulator.
    PairPnL total_pnl_;

    /// Rolling PnL snapshots for Sharpe and drawdown.
    /// Capped at kMaxSnapshots entries; oldest are discarded.
    /// T3-21: std::deque provides O(1) front removal for the rolling window
    /// trim, versus O(n) for std::vector::erase(begin()).
    std::deque<PnLSnapshot> pnl_history_;

    /// Current XCH/USD rate for USD conversion.  Updated by mark_to_market().
    double xch_usd_rate_ = 0.0;

    /// Maximum number of PnL snapshots retained for analytics.
    /// 8640 snapshots at 52-second intervals covers ~5.2 days -- enough
    /// for a robust annualised Sharpe estimate.
    static constexpr std::size_t kMaxSnapshots = 8640;
};

}  // namespace xop

#endif  // XOP_MONITORING_PNL_HPP
