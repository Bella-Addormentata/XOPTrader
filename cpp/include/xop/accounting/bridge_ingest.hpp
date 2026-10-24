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
// figure kept out of trading P&L.  Drawdown-peak policy (engine.cpp,
// owner decision after review rounds 24-41): the peak is NOT adjusted
// in place for flows -- fills do not all reach the inventory tracker
// (taker fills book no legs, TODO S48; maker fills booked only their base
// leg until [FILL-LEGS 2026-09-13]), so no wallet movement can be
// attributed to a specific flow, and every in-place factor scheme was
// refuted.  Instead, booking an IN-PROCESS flow
// resets the peak to its unseeded state and the next equity valuation
// re-anchors it, exactly the accepted restart semantics: drawdown
// measurement restarts at the capital event.  Flows completed before
// the process started are already inside the current anchor and leave
// it untouched.
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
// KNOWN LIMITATION (review round 43, deferred).  Inbound flows book at
// COMPLETED, but the warp service holds a claimed mint in CLAIMING for
// the chia_confirmation_min_height wait AFTER the CAT coin is already
// on-chain -- during that window the wallet balance contains the mint
// with no bridge leg, so the invariant absorbs it as an adjust and the
// booking at COMPLETED then converges through the documented
// three-entry catch-up (adjust + bridge leg + counter-adjust net to
// zero; one expected LedgerDivergence alert).  Booking mid-CLAIMING was
// rejected in round 30: CLAIMING precedes the wallet effect and the
// handler can regress to signature collection, so an early booking
// could book a mint that never landed.  The clean fix is the same
// cross-component work as the round-30 item below (the GUI persists
// the wallet-effect event; TODO.md S21).
//
// KNOWN LIMITATION (review round 30, deferred).  The chronology lower
// bound (MIN ts of CLAIMING / BURN_SENT) is a STATUS time, not the
// wallet-effect time: the warp service enters CLAIMING before a later
// handler invocation actually builds and pushes the claim (and can
// bounce back to signature collection first).  A ledger genesis
// captured after the first CLAIMING event but before the mint lands
// produces an opening WITHOUT the mint while flow_lb <= opening_time
// skips the booking permanently -- the flow is then absorbed as an
// adjustment (the pre-S19 behaviour: equity and the invariant stay
// correct, only the Net Deposits attribution misses it).  Closing this
// needs the GUI's warp service to persist the wallet-affecting claim /
// burn chain height per job so the engine can compare effect time, not
// status time, against the opening (TODO.md S21).
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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

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
    /// ISO-8601 UTC of the job's first broadcast event (CLAIMING /
    /// BURN_SENT) -- a LOWER BOUND on when the wallet could have
    /// changed.  warp_events.ts records GUI-persist time, which lags
    /// the on-chain effect by up to minutes; the opening filter must
    /// compare against the earliest possible wallet change or a genesis
    /// falling inside that lag window double-books the flow (review
    /// round 17).  Empty when no broadcast event exists (fall back to
    /// flow_at).
    std::string  flow_lb;

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
    std::string event_id;           ///< "bridge:job:<id>:<created_at>".
    /// The job's asset fingerprint from its state payload, verbatim
    /// ("v1:<erc20>:<erc20_dec>:<cat_dec>:<chia_asset_id>"; empty when the
    /// job predates fingerprints).  The caller MUST check it against the
    /// configured bridge asset via fingerprint_matches_asset before
    /// booking -- the warp GUI can bridge more than one asset, and a
    /// milliETH job valued at the $1 numeraire would be badly wrong
    /// (review round 1).
    std::string asset_fingerprint;
};

/// Classify one booking-eligible warp job into a ledger flow: inbound
/// rows at COMPLETED, outbound rows from the first burn-confirmed status
/// onward (the caller selects by EXISTENCE of the historical
/// booking-point event, so a post-burn job later moved to FAILED still
/// classifies).  Returns valid=false (book nothing) when the row cannot
/// be classified: bad id, unparseable state JSON (skip rather than guess
/// a direction), unknown direction value, or a non-positive quantity for
/// the indicated direction.  Jobs that never reached a booking point
/// (pre-burn FAILED / CANCELLED, DRY_RUN_OK) have no such event and must
/// be excluded by the caller's selection before this runs.
/// The ledger identity of a job's flow.  Shared between classification
/// and the engine's pre-fetch booked-candidate check (round 31) so the
/// two can never drift apart.
[[nodiscard]] inline std::string bridge_event_id(
    std::int64_t id, const std::string& created_at)
{
    return "bridge:job:" + std::to_string(id) + ":" + created_at;
}

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
    f.event_id = bridge_event_id(row.id, row.created_at);
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
    // Strict shape validation (review round 31): the live format is
    // v1:<erc20>:<erc20-decimals>:<cat-decimals>:<asset-id>, and the
    // ingester hard-codes mojos_per_unit = 1e3, so a fingerprint whose
    // CAT-decimals field is not 3 (or whose version is unknown) must
    // fail closed -- a tail-only match would book such a row at the
    // wrong unit value by orders of magnitude.
    std::size_t field_starts[5];
    std::size_t nfields = 0, start = 0;
    for (std::size_t i = 0; i <= fingerprint.size(); ++i) {
        if (i == fingerprint.size() || fingerprint[i] == ':') {
            if (nfields == 5) return false;      // too many fields
            field_starts[nfields++] = start;
            start = i + 1;
        }
    }
    if (nfields != 5) return false;              // too few fields
    const auto field = [&](std::size_t n) {
        const std::size_t b = field_starts[n];
        const std::size_t e = (n + 1 < 5) ? field_starts[n + 1] - 1
                                          : fingerprint.size();
        return fingerprint.substr(b, e - b);
    };
    if (field(0) != "v1") return false;          // unknown version
    if (field(3) != "3")  return false;          // wrong CAT precision
    const std::string tail = field(4);
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
    // Reject what the live accumulator and the restart parser will
    // reject (review round 32): add_net_deposit_usd ignores
    // |usd| >= 1e12 and parse_bridge_flow_usd rehydrates it as zero,
    // so journaling such a row would leave the ledger/GUI and the PnL
    // accumulator permanently inconsistent.  An int64 mojo quantity
    // can reach ~9.2e15 USD at the $1 numeraire, so the bound is
    // reachable, not theoretical.
    if (!std::isfinite(v.flow_usd)
        || !(std::fabs(v.flow_usd) < 1e12)) {
        return BridgeValuation{};
    }
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
    // Same magnitude bound as PnLTracker::add_net_deposit_usd (review
    // round 15): a row the live accumulator rejects must not reappear on
    // rehydration, or restart invariance breaks.
    if (!std::isfinite(v) || !(std::fabs(v) < 1e12)) return 0.0;
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
    // Semantic field ranges (review round 13): "2026-99-99T99:99:99"
    // passes the shape check yet sorts after any real timestamp, and
    // this predicate is the fail-closed guard for double-booking and
    // peak shifts.  Both writers are machine-generated, so a violation
    // means corruption -- reject it.
    const auto two = [&s](std::size_t i) {
        return (s[i] - '0') * 10 + (s[i + 1] - '0');
    };
    const int month = two(5), day = two(8);
    const int hour = two(11), minute = two(14), second = two(17);
    if (month < 1 || month > 12 || day < 1
        || hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    // Calendar-exact day bound (review round 22): "2026-02-31" passed a
    // flat <=31 check yet sorts after real February timestamps.
    const int year = two(0) * 100 + two(2);
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};
    const int max_day = (month == 2 && leap) ? 29 : kDays[month - 1];
    return day <= max_day;
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
// Secondary bridge assets ([WARP-MILLIETH 2026-10-24]).
//
// accounting.bridge_secondary_asset_ids (wmilliETH.b by default) are bridge
// assets that are NOT the numeraire and that fills keep trading, so two
// things differ from the bridge asset above:
//
//   * VALUE.  A flow is valued at the engine's own USD mark for the asset --
//     the live pseudo-price when there is one, else the price equity is
//     carrying it at -- so Net Deposits moves by what equity moves.  With
//     neither, the flow WAITS (never terminal): unpriced now is not
//     unpriceable forever.
//
//   * INVENTORY.  The bridge asset's tail reconcile (tracked := wallet every
//     heartbeat) would fight the fill path on a traded asset, so a secondary
//     flow moves inventory RELATIVELY, by its own delta.  That is exact
//     unless an ABSOLUTE writer -- the startup seed, Step 8's recovery seed,
//     Step 11's one-shot reconcile -- set the quantity from the wallet at or
//     after the flow's broadcast lower bound: that write may already contain
//     the flow, and adding the delta again would count it twice.  Those
//     flows (and a delta that would drive the tracker negative -- it was
//     already short) re-arm Step 11's one-shot wallet reconcile instead, so
//     wallet truth arrives under that reconcile's own rules.
// ---------------------------------------------------------------------------

/// Whether `asset_id` is one of the configured secondary bridge assets.
[[nodiscard]] inline bool is_secondary_bridge_asset(
    const std::vector<std::string>& secondary_ids,
    const std::string& asset_id)
{
    return std::find(secondary_ids.begin(), secondary_ids.end(), asset_id)
        != secondary_ids.end();
}

/// Whether a job's fingerprint resolves to one of the secondary assets
/// (the bridge-asset scan leaves such jobs to the secondary scan rather
/// than warning that they are foreign).
[[nodiscard]] inline bool is_secondary_bridge_asset_job(
    const std::vector<std::string>& secondary_ids,
    const std::string& fingerprint)
{
    return std::any_of(secondary_ids.begin(), secondary_ids.end(),
                       [&](const std::string& id) {
                           return fingerprint_matches_asset(fingerprint, id);
                       });
}

/// USD per display unit to book a secondary-asset flow at: the live
/// pseudo-price (USD x 1e12) when positive, else a finite positive carry,
/// else nullopt (the flow waits for a price).
[[nodiscard]] inline std::optional<double> secondary_flow_usd_per_unit(
    Mojo live_pseudo_price, std::optional<double> carry_usd_per_unit) noexcept
{
    if (live_pseudo_price > 0) {
        return static_cast<double>(live_pseudo_price) / 1e12;
    }
    if (carry_usd_per_unit && std::isfinite(*carry_usd_per_unit)
        && *carry_usd_per_unit > 0.0) {
        return *carry_usd_per_unit;
    }
    return std::nullopt;
}

enum class SecondaryInventoryAction { Relative, WalletTruth };

struct SecondaryInventoryPlan {
    SecondaryInventoryAction action{SecondaryInventoryAction::WalletTruth};
    Mojo target{0};   ///< Relative only: the tracked quantity to set.
};

/// How a booked secondary flow reaches the inventory tracker (see the
/// banner above).  `last_absolute_write_iso` is the last absolute write's
/// stamp; the engine passes its process start when none happened in this
/// process, since the persisted quantity it restored may itself come from
/// an earlier process's absolute write.  Empty means "none, ever" (pure
/// callers and tests).  Fails toward wallet
/// truth: a malformed flow floor with any absolute write on record, an
/// overflowing sum, or a negative result all read the wallet instead.
[[nodiscard]] inline SecondaryInventoryPlan plan_secondary_inventory(
    Mojo tracked, Mojo delta_mojos,
    const std::string& last_absolute_write_iso,
    const std::string& flow_floor_iso) noexcept
{
    SecondaryInventoryPlan p{};
    if (tracked < 0) return p;
    if (!last_absolute_write_iso.empty()
        && !iso_strictly_after(flow_floor_iso, last_absolute_write_iso)) {
        return p;
    }
    if (delta_mojos > 0
        && tracked > std::numeric_limits<Mojo>::max() - delta_mojos) {
        return p;
    }
    const Mojo target = tracked + delta_mojos;
    if (target < 0) return p;
    p.action = SecondaryInventoryAction::Relative;
    p.target = target;
    return p;
}

/// Step 13's rolling-loss window rebase after a booked secondary flow
/// (engine.cpp): the window restarts every cycle while this says Wait.
enum class WindowRebaseVerdict { Wait, CaughtUp, Expired };

/// Whether a booked flow's rebase can retire.  CaughtUp needs State to hold
/// the wallet's balance from a validated snapshot taken at or after the
/// booking, AND a heartbeat strictly after the booking one: Step 11 marks
/// P&L before the scan books, so a flow booked this heartbeat (a deposit's
/// new cost basis, a relative inventory move) reaches the marks only next
/// heartbeat, whatever State shows now.  Expired after one loss window, so
/// an asset whose State never catches up cannot blind the breaker.
[[nodiscard]] inline WindowRebaseVerdict window_rebase_verdict(
    std::uint64_t booked_block, std::uint64_t now_block,
    bool snapshot_validated, std::uint64_t snapshot_block,
    Mojo state_balance, Mojo wallet_confirmed,
    std::uint64_t window_blocks) noexcept
{
    if (now_block > booked_block && snapshot_validated
        && snapshot_block >= booked_block
        && state_balance == wallet_confirmed) {
        return WindowRebaseVerdict::CaughtUp;
    }
    if (now_block >= booked_block && now_block - booked_block >= window_blocks) {
        return WindowRebaseVerdict::Expired;
    }
    return WindowRebaseVerdict::Wait;
}

}  // namespace xop::accounting

#endif  // XOP_ACCOUNTING_BRIDGE_INGEST_HPP
