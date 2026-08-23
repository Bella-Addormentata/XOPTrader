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
// "bridge:job:<id>:<created_at>" (the immutable creation stamp guards
// against a recreated jobs DB reusing AUTOINCREMENT ids) -- re-scans
// and restarts are no-ops.
//
// KNOWN LIMITATION (review round 1, deferred).  A job is not bound to
// the ENGINE's wallet: warp.chia_receiver_address is operator-configured,
// and a wrap delivered to a foreign address would still book here as
// engine capital (the divergence control would then post a compensating
// adjust).  Today the receiver is the engine wallet by construction; if
// that ever changes, the booking must compare the job's receiver_ph
// against the engine wallet's puzzle hashes before booking.
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
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace xop::accounting {

// ---------------------------------------------------------------------------
// One row of the GUI's warp_jobs table, as read by the engine.
// ---------------------------------------------------------------------------

struct BridgeJobRow {
    std::int64_t id{0};             ///< warp_jobs.id (AUTOINCREMENT -- only
                                    ///< unique within one DB file).
    Mojo         amount_mojos{0};   ///< Outbound: CAT mojos burned.
    Mojo         post_tip_mojos{0}; ///< Inbound: CAT mojos minted after tip.
    /// ISO-8601 UTC of the job's FIRST entry into a booking-eligible
    /// status, taken from warp_events (immutable).  NOT
    /// warp_jobs.updated_at, which rewrites on every poll and would let
    /// a pre-opening burn drift past the opening filter (review round 3).
    std::string  flow_at;
    std::string  state_json;        ///< Evolving payload; carries direction.
    /// ISO-8601 UTC creation stamp (immutable, never rewritten).  Part of
    /// the ledger identity: a recreated jobs DB restarts AUTOINCREMENT,
    /// and "bridge:job:1" alone would collide with a genuinely new flow,
    /// which INSERT OR IGNORE would then silently drop (review round 3).
    std::string  created_at;
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
    /// The job's asset fingerprint from its state payload, verbatim
    /// ("v1:<erc20>:<erc20_dec>:<cat_dec>:<chia_asset_id>"; empty when the
    /// job predates fingerprints).  The caller MUST check it against the
    /// configured bridge asset via fingerprint_matches_asset before
    /// booking -- the warp GUI can bridge more than one asset, and a
    /// milliETH job valued at the $1 numeraire would be badly wrong
    /// (review round 1).
    std::string asset_fingerprint;
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
        // STRICT direction vocabulary (review round 1): absent or empty
        // means inbound (the historical marker), "out" means outbound,
        // and any other present value is unclassifiable -- a typo or a
        // future enum value must never default to a signed booking.
        const auto direction = st.value("direction", std::string{});
        if (direction == "out") {
            outbound = true;
        } else if (!direction.empty()) {
            return f;   // unknown direction: refuse to guess a sign
        }
        f.asset_fingerprint = st.value("asset_fingerprint", std::string{});
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
    f.event_id = "bridge:job:" + std::to_string(row.id) + ":"
               + row.created_at;
    f.valid    = true;
    return f;
}

/// Whether a job's asset fingerprint resolves to `asset_id`.  The
/// fingerprint's LAST ':'-separated segment is the expected Chia asset id
/// (gui/services/warp/service.py::_asset_fingerprint); the comparison is
/// case-insensitive because the GUI lowercases and config might not.
/// STRICT: an empty or malformed fingerprint does NOT match -- with no
/// stated identity the engine refuses to book rather than assume the
/// configured asset (every live job row carries a fingerprint).
[[nodiscard]] inline bool fingerprint_matches_asset(
    const std::string& fingerprint, const std::string& asset_id) noexcept
{
    if (fingerprint.empty() || asset_id.empty()) return false;
    const auto pos = fingerprint.rfind(':');
    if (pos == std::string::npos || pos + 1 >= fingerprint.size()) {
        return false;
    }
    const std::string tail = fingerprint.substr(pos + 1);
    if (tail.size() != asset_id.size()) return false;
    for (std::size_t i = 0; i < tail.size(); ++i) {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z')
                 ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (lower(tail[i]) != lower(asset_id[i])) return false;
    }
    return true;
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
    // Reject non-finite rates outright (review round 7: +Inf
    // mojos_per_unit passed the positivity check and produced a nonzero
    // pseudo-price with a zero USD flow, violating the contract).
    if (!std::isfinite(mojos_per_unit) || !std::isfinite(usd_per_unit)) {
        return v;
    }
    if (!(mojos_per_unit > 0.0) || !(usd_per_unit > 0.0)) return v;
    // The 1e12 pseudo-price scale overflows Mojo above ~$9.2M/unit, and
    // converting an out-of-range double to int64 is UB -- bound the SCALED
    // value against Mojo max before the cast (review round 1).  Also
    // rejects Inf.
    const double scaled = usd_per_unit * 1e12 + 0.5;
    if (!(scaled < static_cast<double>(
              std::numeric_limits<Mojo>::max()))) {
        return v;
    }

    v.fmv_pseudo_price = static_cast<Mojo>(scaled);
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

/// Shape-check the 19-char "YYYY-MM-DDTHH:MM:SS" prefix: digits in the
/// digit positions, the exact separators between them.  Length alone is
/// not fail-closed -- 19 chars of garbage that happens to sort high would
/// otherwise pass a lexicographic compare and shift the drawdown peak
/// (review round 2).
[[nodiscard]] inline bool looks_like_iso_prefix(
    const std::string& s) noexcept
{
    constexpr std::size_t kPrefix = 19;
    if (s.size() < kPrefix) return false;
    for (std::size_t i = 0; i < kPrefix; ++i) {
        const char c = s[i];
        switch (i) {
            case 4: case 7:
                if (c != '-') return false;
                break;
            case 10:
                if (c != 'T') return false;
                break;
            case 13: case 16:
                if (c != ':') return false;
                break;
            default:
                if (c < '0' || c > '9') return false;
                break;
        }
    }
    return true;
}

/// Whether ISO-8601 UTC timestamp `a` is STRICTLY after `b`, comparing the
/// 19-char "YYYY-MM-DDTHH:MM:SS" prefix (suffix-style-indifferent, same as
/// completed_during_process below).  Malformed or missing timestamps fail
/// closed (false).  Used by the opening filter: a job completed at or
/// before the asset's ledger opening is already INSIDE the opening balance
/// (the opening records the live wallet, mint included), and booking it
/// again would double-count -- the fresh/reset-ledger scenario (review
/// round 1).  Ties skip on purpose: an under-booked flow degrades to the
/// pre-S19 divergence-adjust behaviour, while a double-booked one
/// permanently overstates net deposits.
[[nodiscard]] inline bool iso_strictly_after(
    const std::string& a, const std::string& b) noexcept
{
    constexpr std::size_t kPrefix = 19;
    if (!looks_like_iso_prefix(a) || !looks_like_iso_prefix(b)) {
        return false;
    }
    return a.compare(0, kPrefix, b, 0, kPrefix) > 0;
}

// ---------------------------------------------------------------------------
// Drawdown-anchor guard.
// ---------------------------------------------------------------------------

/// Whether a job's flow happened while THIS engine process was alive.
/// The drawdown peak only rescales for such flows: after a restart the
/// startup grace re-anchors the peak to an equity that already contains
/// the flow, and rescaling again would double-count it.  STRICTLY after
/// (review round 3): the GUI stamps whole seconds while the engine start
/// carries sub-second precision the prefix compare discards, so an
/// equal-second flow is ambiguous and must fail closed -- the same
/// doctrine as the opening filter, and literally the same ordering.
[[nodiscard]] inline bool completed_during_process(
    const std::string& flow_at_iso,
    const std::string& process_start_iso) noexcept
{
    return iso_strictly_after(flow_at_iso, process_start_iso);
}

}  // namespace xop::accounting

#endif  // XOP_ACCOUNTING_BRIDGE_INGEST_HPP
