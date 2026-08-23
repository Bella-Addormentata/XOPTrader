// bridge_ingest.hpp -- Classification and valuation of completed warp.green
// bridge flows, booked as EXTERNAL CAPITAL ([S19 2026-08-23]).
//
// WHY THIS EXISTS.  The first live bridge (job 2, +4.985 wUSDC.b minted at
// block 9189949) exposed a gap: the ledger had no deposit/withdrawal event
// type, so a completed bridge inflow was absorbed by the invariant control
// as an "adjust" entry ("unexplained divergence reconciled to wallet").
// The books tied, but attribution was blind -- the equity jump could mask
// real trading losses in the breaker windows, and nothing separated capital
// movements from performance.
//
// ACCOUNTING TREATMENT (owner-approved, GIPS/TWR-style): external capital
// flows are NOT performance.  A completed bridge job books as a first-class
// ledger event (bridge_deposit inbound / bridge_withdrawal outbound) BEFORE
// the divergence control runs, so the movement is explained flow rather
// than a blind adjustment.  The USD amount accumulates in a NET DEPOSITS
// figure kept out of trading P&L, and the drawdown anchor shifts WITH the
// flow so a deposit cannot mask losses (nor a withdrawal fake them).
//
// DATA SOURCE.  The GUI owns data/warp_jobs.db (WAL); the engine reads it
// READ-ONLY during the ledger tie.  A job row's direction rides in its JSON
// state payload ("direction": "out" for unwraps; absent for inbound), the
// minted quantity is post_tip_mojos (inbound, after the warp.green tip) and
// the burned quantity is amount_mojos (outbound).  Idempotency comes from
// the ledger's UNIQUE(event_id, leg, asset_id) via event_id
// "bridge:job:<id>" -- re-scans and restarts are no-ops.
//
// VALUATION.  wUSDC.b is the $1.00 numeraire: the peg is MONITORED, not
// priced in (AccountingConfig peg-monitor doctrine), so usd_per_unit is
// exactly 1.0 and the flow's USD is embedded in the ledger note for
// restart-invariant rehydration -- the same writer/parser-side-by-side
// pattern as reward_ingest.hpp.
//
// Compliant with:
//   ISO/IEC 5055  -- pure functions, NaN-guarded, no UB
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
//                    (tests/test_bridge_ingest.cpp)

#ifndef XOP_ACCOUNTING_BRIDGE_INGEST_HPP
#define XOP_ACCOUNTING_BRIDGE_INGEST_HPP

#include "xop/types.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

namespace xop::accounting {

// ---------------------------------------------------------------------------
// One row of the GUI's warp_jobs table, as read by the engine.
// ---------------------------------------------------------------------------

struct BridgeJobRow {
    std::int64_t id{0};             ///< warp_jobs.id (the idempotency key).
    Mojo         amount_mojos{0};   ///< Outbound: CAT mojos burned.
    Mojo         post_tip_mojos{0}; ///< Inbound: CAT mojos minted after tip.
    std::string  updated_at;        ///< ISO-8601 UTC of the last transition.
    std::string  state_json;        ///< Evolving payload; carries direction.
};

// ---------------------------------------------------------------------------
// Classification -- which ledger event a completed job becomes.
// ---------------------------------------------------------------------------

struct BridgeFlow {
    bool        valid{false};
    bool        inbound{false};
    Mojo        delta_mojos{0};     ///< Signed: + mint, - burn.
    std::string event_type;         ///< bridge_deposit | bridge_withdrawal.
    std::string event_id;           ///< "bridge:job:<id>".
};

/// Classify one COMPLETED warp job into a ledger flow.  Returns
/// valid=false (book nothing) when the row cannot be classified: bad id,
/// unparseable state JSON (skip rather than guess a direction), or a
/// non-positive quantity for the indicated direction.  DRY_RUN_OK /
/// FAILED / CANCELLED jobs move nothing cross-chain and must be filtered
/// out by the caller's status predicate before this runs.
[[nodiscard]] inline BridgeFlow classify_bridge_job(const BridgeJobRow& row)
{
    BridgeFlow f{};
    if (row.id <= 0) return f;

    bool outbound = false;
    try {
        const auto st = nlohmann::json::parse(row.state_json);
        outbound = st.value("direction", std::string{}) == "out";
    } catch (const nlohmann::json::exception&) {
        return f;   // unreadable state: refuse to guess a direction
    }

    if (outbound) {
        if (row.amount_mojos <= 0) return f;
        f.inbound     = false;
        f.delta_mojos = -row.amount_mojos;
        f.event_type  = "bridge_withdrawal";
    } else {
        if (row.post_tip_mojos <= 0) return f;
        f.inbound     = true;
        f.delta_mojos = row.post_tip_mojos;
        f.event_type  = "bridge_deposit";
    }
    f.event_id = "bridge:job:" + std::to_string(row.id);
    f.valid    = true;
    return f;
}

// ---------------------------------------------------------------------------
// Valuation -- same pseudo-price convention as reward_ingest.hpp.
// ---------------------------------------------------------------------------

struct BridgeValuation {
    /// Cost-basis price in the InventoryTracker's USD-pseudo convention:
    /// USD per display unit in kMojosPerXch (1e12) fixed point.
    Mojo   fmv_pseudo_price{0};

    /// The external flow in USD, SIGNED: positive for a deposit, negative
    /// for a withdrawal.  (delta_mojos / mojos_per_unit) * usd_per_unit.
    double flow_usd{0.0};
};

/// Value a bridge flow.  Zeroed valuation when the quantity is zero or any
/// rate is non-positive / non-finite.
[[nodiscard]] inline BridgeValuation value_bridge_flow(
    Mojo delta_mojos, double mojos_per_unit, double usd_per_unit) noexcept
{
    BridgeValuation v{};
    if (delta_mojos == 0) return v;
    if (!(mojos_per_unit > 0.0) || !(usd_per_unit > 0.0)) return v;  // NaN-safe
    if (!(usd_per_unit < 1e12)) return v;                            // Inf-guard

    v.fmv_pseudo_price = static_cast<Mojo>(usd_per_unit * 1e12 + 0.5);
    v.flow_usd = (static_cast<double>(delta_mojos) / mojos_per_unit)
               * usd_per_unit;
    return v;
}

/// Ledger-note format for a bridge entry.  The signed USD flow is embedded
/// so the net-deposits total is rebuilt from the ledger alone on restart
/// (PnLTracker::rehydrate_from_db) -- writer and parser live side by side
/// so the format cannot drift.
[[nodiscard]] inline std::string bridge_note(double flow_usd,
                                             double usd_per_unit,
                                             std::int64_t job_id)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "warp bridge %s; flow_usd=%.10f; "
                  "px_usd_per_unit=%.10f; job=%lld",
                  flow_usd >= 0.0 ? "deposit" : "withdrawal",
                  flow_usd, usd_per_unit,
                  static_cast<long long>(job_id));
    return std::string(buf);
}

/// Parse the SIGNED flow_usd field back out of a bridge note.  Returns 0.0
/// for anything that does not carry the field (foreign notes, hand edits)
/// or carries a non-finite / absurd value.  Unlike parse_reward_fmv_usd,
/// negative values are VALID here -- they are withdrawals.
[[nodiscard]] inline double parse_bridge_flow_usd(
    const std::string& note) noexcept
{
    static constexpr char kKey[] = "flow_usd=";
    const auto pos = note.find(kKey);
    if (pos == std::string::npos) return 0.0;
    const double v = std::strtod(note.c_str() + pos + sizeof(kKey) - 1,
                                 nullptr);
    if (!std::isfinite(v) || !(std::fabs(v) < 1e15)) return 0.0;
    return v;
}

// ---------------------------------------------------------------------------
// Drawdown-anchor guard.
// ---------------------------------------------------------------------------

/// Whether a job completed while THIS engine process was alive.  The
/// drawdown peak only shifts for such flows: after a restart the startup
/// grace re-anchors the peak to an equity that already contains the flow,
/// and shifting again would double-count it.  Both timestamps are ISO-8601
/// UTC, so comparing the first 19 chars ("YYYY-MM-DDTHH:MM:SS") is a
/// correct chronological compare regardless of the suffix ("Z" vs
/// "+00:00").  Malformed or missing timestamps fail closed (no shift).
[[nodiscard]] inline bool completed_during_process(
    const std::string& updated_at_iso,
    const std::string& process_start_iso) noexcept
{
    constexpr std::size_t kPrefix = 19;
    if (updated_at_iso.size() < kPrefix
        || process_start_iso.size() < kPrefix) {
        return false;
    }
    return updated_at_iso.compare(0, kPrefix,
                                  process_start_iso, 0, kPrefix) >= 0;
}

}  // namespace xop::accounting

#endif  // XOP_ACCOUNTING_BRIDGE_INGEST_HPP
