// SPDX-License-Identifier: MIT
//
// fee_controller.hpp -- closed-loop control of the blockchain fee RATE.
//
// [S67 2026-09-20] Why it exists.  Chia blocks ran ~97% full for a week.  With
// the mempool at capacity the node admits a spend only at >= 5 mojos per unit
// of CLVM cost (chia 2.7.4 mempool_manager.py, nonzero_fee_minimum_fpc) and
// refuses the rest with INVALID_FEE_TOO_CLOSE_TO_ZERO.  The engine's fee was
// an OPEN loop: clamp(node estimate for a plain XCH send, min, max).  Nothing
// looked at whether our own spends got in, so cancels and takes sat
// unconfirmed, pending_change persisted, and Step 8 force-deleted every
// unconfirmed wallet transaction 10-12 times a day.  Raising
// fees.min_fee_mojos by hand did not stop it.
//
// What it controls.  ONE state, a fee rate in mojos per CLVM cost, kept as a
// log2 LEVEL over an anchor:
//
//     rate  = max(anchor x 2^level, feed-forward floor)
//     fee_c = clamp(ceil(rate x cost_c), min_fee, max_fee)      per class c
//
// Required fees scale with bundle cost, so a rate plus a small per-class cost
// model (ClassCosts) yields a fee for every action from one state.
//
// THE PLANT IS A THRESHOLD, AND THAT SHAPES THE LAW.  Above the admission
// floor the confirmation delay is flat (measured: median 2-5 peak heights,
// p90 <= 13, at every fee rate from 0 to 2.5 mojos/cost); below it the spend
// does not confirm at all.  Two consequences:
//
//   * a too-low fee may NEVER confirm, so waiting for a confirmation would
//     deadlock the loop.  A spend still pending after the target delay is a
//     CENSORED observation and is evidence of "too low" NOW;
//   * delay carries no information about OVERPAYMENT -- the error is zero
//     everywhere above the floor.  A PID on delay error alone can therefore
//     find the floor from below and never from above.  The way down is an
//     explicit PROBE schedule with memory (last known-good level) and
//     exponential backoff.
//
// The law, per observation k, with e_k the lateness in units of the target
// delay (0 when on target, capped at max_error):
//
//     up_k    = Kp*max(0, e_k - e_{k-1}) + Ki*e_k
//             + Kd*max(0, e_k - 2e_{k-1} + e_{k-2})
//     level_k = clamp(level_{k-1} + min(up_k, max_step_up), lo, hi)
//
// This is the VELOCITY (incremental) form of u = Kp*e + Ki*sum(e) + Kd*de with
// the negative increments dropped.  The positional form was rejected for one
// reason: its P and D terms vanish when the error returns to zero, which
// lowers the fee below a level that was just verified good -- an unscheduled
// probe with no memory and no backoff.  Here the PID only ever raises (fast
// up), and the probe schedule is the only way down (slow down).
//
// ANTI-WINDUP.  The level IS the integrator, clamped to [lo, hi], the band in
// which the fee of at least one class is strictly between min_fee and
// max_fee.  Evidence is also gated so a stuck spend cannot wind the level up:
//
//   * ANSWERED rule: too-low evidence from a spend submitted at level s
//     counts only while level < s + min_raise.  Once the level has been raised
//     that far the spend has been answered; whether the NEW level works is
//     not known until a spend submitted at it is late too.  This is also what
//     bounds windup behind a saturated actuator (a fee clamped by max_fee or
//     by the budget was submitted BELOW the level, so it stops counting as
//     soon as the gap reaches min_raise);
//   * DEAD TIME: wallet-level signals that cannot be attributed to one spend
//     (pending_change persisting, force-delete, a sent_to fee error) are
//     ignored for TWO target delays after a raise.  One is not enough: Step 8
//     force-deletes a median of 176 s (9 peak heights) into a stuck episode,
//     one height past an 8-height target, so with a dead time of one target
//     the same stuck spends would be heard twice -- once as tickets, once as
//     the wipe.  (Measured against the simulated floor the wipes add nothing
//     to the overshoot at two targets; see
//     ForceDeleteSignalsDoNotDoubleCountTheSameStuckEpisode.);
//   * CORROBORATION: at most kMaxUncorroboratedRaises such signals in a row
//     may raise the level.  pending_change can persist for reasons no fee
//     cures (a wallet that is not syncing, 45-second blocks on 2026-09-14);
//     unbounded, a force-delete every three minutes would walk the fee to
//     max_fee in half an hour.  Any attributed observation re-arms them.
//
// FEED-FORWARD IS A FLOOR, NOT A MULTIPLIER.  The node's estimate was measured
// 13x below the full-mempool admission floor and reads 0 when the mempool has
// room, so an anchor that followed it would flap, and a correction learned in
// one regime would overshoot in the other.  As a floor it can only raise the
// fee, the level stays an absolute quantity (last known-good keeps its
// meaning), and an unreachable node simply means no floor: the learned level
// carries on (the node RPC was unreachable for hours on 2026-09-14).
//
// EVERY DELAY IS IN PEAK HEIGHTS: 4,608 per day, 18.75 s each -- what
// get_block_height() counts -- never the ~52 s transaction-block figure.
//
// Pure header: no engine, config, RPC, JSON or logging types, no clock, no
// randomness.  cpp/tests/test_fee_controller.cpp drives every rule directly
// and runs the controller against a simulated hidden floor.
// ---------------------------------------------------------------------------

#ifndef XOP_STRATEGY_FEE_CONTROLLER_HPP
#define XOP_STRATEGY_FEE_CONTROLLER_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace xop::strategy::fee {

// ---------------------------------------------------------------------------
// Action classes and their cost model
// ---------------------------------------------------------------------------

/// What a fee is about to pay for.  ZERO IS THE LEGACY VALUE: a call site that
/// names no class gets the fee attached to a posted offer, which is what
/// every call site received before classes existed.
enum class ActionClass : std::uint8_t {
    OfferAttached = 0,  ///< fee inside a posted offer; paid only if it fills
    CancelXch,          ///< secure cancel of an offer that offered XCH
    CancelCat,          ///< secure cancel of an offer that offered a CAT
    Take,               ///< take_offer on a pair with a CAT leg
};

inline constexpr std::size_t kActionClassCount = 4;

[[nodiscard]] constexpr const char* to_string(ActionClass c) noexcept
{
    switch (c) {
        case ActionClass::OfferAttached: return "offer_attached";
        case ActionClass::CancelXch:     return "cancel_xch";
        case ActionClass::CancelCat:     return "cancel_cat";
        case ActionClass::Take:          return "take";
    }
    return "unknown";
}

/// Cancels and takes are spends of OURS that must get in; an attached fee is
/// paid by a fill, if one ever comes.  The budget keeps a reserve for the
/// former (apply_budget).
[[nodiscard]] constexpr bool is_priority(ActionClass c) noexcept
{
    return c != ActionClass::OfferAttached;
}

/// CLVM cost per class.  MEASURED 2026-09-20 on this wallet's own spend
/// bundles (321 bundles, three days, costed with chia_rs
/// get_conditions_from_spendbundle under the 2.7.4 mainnet constants):
///
///   cancel_xch       8,331,578 -  8,379,584   (133 bundles, one XCH spend;
///                    the fee comes out of the cancelled coin)
///   cancel_cat      42,054,545 - 42,258,669   (177 bundles: one CAT spend
///                    plus one XCH fee spend)
///   take            92,618,934 - 212,112,758  (11 bundles; 92.6M with one
///                    maker coin, about +30.5M per extra maker coin; 125M
///                    covers 7 of the 11)
///   offer_attached  no bundle of ours exists until a taker builds one; 21M
///                    is the mean of the XCH (8.4M) and CAT (33.8M) maker
///                    spend a fill adds to the taker's bundle.
///
/// For scale: the node's own stopgap table (full_node_rpc_api.py
/// _get_spendbundle_type_cost) says send_xch_transaction 9,401,710,
/// cancel_offer 212,443,993 and take_offer 721,393,265 -- the first is what
/// the engine used to ask about, the other two are 5x above what this wallet
/// produces.
struct ClassCosts {
    std::uint64_t offer_attached{21'000'000ULL};
    std::uint64_t cancel_xch{8'400'000ULL};
    std::uint64_t cancel_cat{42'300'000ULL};
    std::uint64_t take{125'000'000ULL};
};

[[nodiscard]] constexpr std::uint64_t cost_of(const ClassCosts& costs, ActionClass c) noexcept
{
    switch (c) {
        case ActionClass::OfferAttached: return costs.offer_attached;
        case ActionClass::CancelXch:     return costs.cancel_xch;
        case ActionClass::CancelCat:     return costs.cancel_cat;
        case ActionClass::Take:          return costs.take;
    }
    return costs.cancel_cat;
}

[[nodiscard]] constexpr std::uint64_t min_cost(const ClassCosts& c) noexcept
{
    return std::min(std::min(c.offer_attached, c.cancel_xch), std::min(c.cancel_cat, c.take));
}

[[nodiscard]] constexpr std::uint64_t max_cost(const ClassCosts& c) noexcept
{
    return std::max(std::max(c.offer_attached, c.cancel_xch), std::max(c.cancel_cat, c.take));
}

/// The class of a cancel, from the asset the offer OFFERED -- the coins a
/// secure cancel spends.  A bid offers the quote asset, an ask the base.
[[nodiscard]] constexpr ActionClass cancel_class(bool offered_asset_is_xch) noexcept
{
    return offered_asset_is_xch ? ActionClass::CancelXch : ActionClass::CancelCat;
}

/// The fee ONE cancel pays.  `override_active` false is the pre-controller
/// behaviour, byte for byte: every cancel pays `legacy_fee`.  With it true the
/// fee follows the offered asset, and an offer nobody can classify pays the
/// CAT fee -- the larger, so an unknown offer errs toward getting in.
[[nodiscard]] constexpr std::uint64_t cancel_fee_for(bool          override_active,
                                                     std::uint64_t legacy_fee,
                                                     std::uint64_t xch_fee,
                                                     std::uint64_t cat_fee,
                                                     bool          offer_known,
                                                     bool          offered_is_xch) noexcept
{
    if (!override_active) {
        return legacy_fee;
    }
    if (!offer_known) {
        return std::max(xch_fee, cat_fee);
    }
    return offered_is_xch ? xch_fee : cat_fee;
}

static_assert(cancel_fee_for(false, 7U, 1U, 2U, true, true) == 7U);
static_assert(cancel_fee_for(true, 7U, 1U, 2U, true, true) == 1U);
static_assert(cancel_fee_for(true, 7U, 1U, 2U, true, false) == 2U);
static_assert(cancel_fee_for(true, 7U, 1U, 2U, false, true) == 2U);

/// chia 2.7.4 mempool_manager.py: with the mempool at capacity a spend below
/// this many mojos per cost is refused (INVALID_FEE_TOO_CLOSE_TO_ZERO).
inline constexpr double kFullMempoolMinRate = 5.0;

/// 2^63, exactly representable.  The largest fee this header will emit; a
/// constant "just below UINT64_MAX" rounds UP to 2^64 as a double and the
/// cast back is undefined (memory: msvc-gcc-divergence).
inline constexpr double        kFeeCeilingDouble = 9223372036854775808.0;
inline constexpr std::uint64_t kFeeCeiling       = 9223372036854775808ULL;

/// One part in 10^12: how far above a whole number a product may sit and still
/// be that number.  (min_fee / cost) x cost is min_fee in exact arithmetic and
/// min_fee x (1 + 2e-16) in doubles, and a bare ceil() would charge min_fee + 1
/// for it -- "level 0 pays EXACTLY min_fee" would be off by a mojo on some
/// inputs and not others, by platform.  Double rounding noise is ~1e-15; this
/// is a thousand times that and still one mojo per trillion.
inline constexpr double kCeilTolerance = 1e-12;

/// ceil(rate x cost) as mojos -- never BELOW the computed requirement, up to
/// kCeilTolerance.  0 for a rate or cost that is not finite and > 0; saturates
/// at 2^63.
[[nodiscard]] inline std::uint64_t fee_from_rate(double rate, std::uint64_t cost) noexcept
{
    if (!std::isfinite(rate) || !(rate > 0.0) || cost == 0U) {
        return 0U;
    }
    const double product = rate * static_cast<double>(cost);
    const double raw     = std::ceil(product - product * kCeilTolerance);
    if (!(raw < kFeeCeilingDouble)) {
        return kFeeCeiling;
    }
    return static_cast<std::uint64_t>(raw);
}

// ---------------------------------------------------------------------------
// Budget interaction
// ---------------------------------------------------------------------------

struct BudgetedFee {
    std::uint64_t fee{0};
    bool          bound{false};   ///< the budget lowered the fee
};

/// The rolling budget, made explicit.  Without the controller an exhausted
/// budget returns 0, which Step 8 reads as "skip everything" -- a loop that
/// raises fees would then silently stop the bot from quoting AND from
/// cancelling stale quotes.  With it:
///
///   * a PRIORITY action (cancel, take) may use the whole headroom;
///   * an offer-attached fee may use only the headroom above `reserve`, the
///     budget held back so the resting book can still be cancelled -- and
///     only a 1/`batch` share of it.  [review #163] Step 8 asks for ONE
///     attached fee per heartbeat and then attaches it to every tier it posts,
///     recording posted x fee afterwards, so a fee shaped for one offer let a
///     ladder of ten spend ten times the room above the reserve.  `batch` is
///     the number of offers the fee may be attached to before the budget is
///     next consulted (the caller's upper bound; 0 counts as 1);
///   * a PRIORITY action is NOT shaped by a batch, on purpose: a cancel that
///     must happen pays, so for cancels the budget is a soft limit that one
///     heartbeat can overshoot and the next degrades to min_fee;
///   * nothing is ever lowered below min_fee, the operator's own floor, and
///     nothing is ever 0: an exhausted budget degrades every fee to min_fee
///     and says so (`bound`), it does not stop the bot.
///
/// `desired` is already clamped to [min_fee, max_fee] by the caller.
[[nodiscard]] constexpr BudgetedFee apply_budget(std::uint64_t desired,
                                                 std::uint64_t headroom,
                                                 std::uint64_t reserve,
                                                 std::uint64_t min_fee,
                                                 bool          priority,
                                                 std::uint64_t batch = 1U) noexcept
{
    const std::uint64_t above_reserve = headroom > reserve ? headroom - reserve : 0U;
    const std::uint64_t available =
        priority ? headroom : above_reserve / (batch == 0U ? 1U : batch);
    std::uint64_t fee = std::min(desired, available);
    if (fee < min_fee) {
        fee = std::min(desired, min_fee);
    }
    return BudgetedFee{fee, fee < desired};
}

static_assert(apply_budget(200U, 1'000U, 900U, 10U, true).fee == 200U);
static_assert(apply_budget(200U, 1'000U, 900U, 10U, false).fee == 100U);
static_assert(apply_budget(200U, 1'000U, 900U, 10U, false).bound);
static_assert(apply_budget(200U, 0U, 0U, 10U, true).fee == 10U);
static_assert(!apply_budget(200U, 200U, 0U, 10U, true).bound);
// Ten offers share the 100 above the reserve: 10 each, not 100 each.
static_assert(apply_budget(200U, 1'000U, 900U, 10U, false, 10U).fee == 10U);
static_assert(apply_budget(200U, 1'000U, 900U, 1U, false, 4U).fee == 25U);
static_assert(apply_budget(200U, 1'000U, 900U, 1U, false, 0U).fee == 100U);
// A priority action is not shaped by the batch.
static_assert(apply_budget(200U, 1'000U, 900U, 10U, true, 10U).fee == 200U);

// ---------------------------------------------------------------------------
// The wallet's own word: sent_to
// ---------------------------------------------------------------------------

/// chia 2.7.4 wallet_transaction_store.py increment_sent appends
/// (peer, MempoolInclusionStatus, Err name) to TransactionRecord.sent_to, and
/// transaction_record.py is_valid treats exactly these two names as
/// temporary.  They are the node saying "this fee is too low", verbatim.
[[nodiscard]] constexpr bool is_fee_rejection(std::string_view err_name) noexcept
{
    return err_name == "INVALID_FEE_TOO_CLOSE_TO_ZERO" || err_name == "INVALID_FEE_LOW_FEE";
}

static_assert(is_fee_rejection("INVALID_FEE_LOW_FEE"));
static_assert(!is_fee_rejection("MEMPOOL_CONFLICT"));

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/// Values come from xop::FeeConfig (`fees.controller_*`).  Every member has
/// a default member initializer (GCC -Wmissing-field-initializers).
struct ControllerConfig {
    bool enabled{false};

    /// Setpoint: a spend of ours should confirm within this many PEAK
    /// heights (18.75 s each; 8 = 150 s).  Measured p90 of confirmed spends
    /// is 5-13 heights; the Step 8 force-delete fires a median of 176 s after
    /// pending_change first shows.
    std::uint32_t target_delay_blocks{8};

    /// Gains, in log2 fee units per unit of lateness.  One unit of lateness
    /// is one whole target delay late.
    double kp{1.0};
    double ki{0.5};
    double kd{0.5};

    /// Cap on the lateness one observation can report.  Hard signals
    /// (force-delete, sent_to fee error) report exactly this.
    double max_error{2.0};

    /// Cap on one observation's raise, in log2 units (1.0 = at most x2).
    double max_step_up{1.0};

    /// The ANSWERED rule's margin, in log2 units (1.0 = x2): a stuck spend
    /// keeps raising the level until it is this far above what that spend
    /// paid.  It is also the size of one raise, because a spend submitted at
    /// the new level cannot be late for another whole target delay.  Against
    /// the simulated floor 0.5 needed 21 actions to cross a 14x gap and 1.0
    /// needs 12.  The price is overshoot: spends submitted part-way up a ramp
    /// are stuck too and each asks for this much above what IT paid, so the
    /// level can end 2 x min_raise above the last level that failed (x4 in
    /// theory; x2.37 and x1.46 measured), which the probes walk back 15% at a
    /// time.  A stuck cancel is a quote left takeable and costs a
    /// force-delete; an overpaid one costs thousandths of a cent.
    double min_raise{1.0};

    /// Observations consumed before the level may move.  They still update
    /// the error history.
    std::uint32_t warmup_observations{3};

    /// PROBE-DOWN.  After `probe_after_confirmations` consecutive on-target
    /// confirmations the fee is stepped down by `probe_fraction`.  The probe
    /// succeeds after `probe_confirmations` on-target confirmations at the
    /// lower level.  If it fails the level returns to last-good times
    /// (1 + probe_fail_bump), the failed level is remembered as KNOWN-BAD,
    /// and the RETEST interval doubles, up to `probe_backoff_cap`.
    ///
    /// A probe whose target is above known-bad explores the bracket between
    /// known-bad and known-good and runs at the base interval.  One whose
    /// target is at or below known-bad is a RETEST of a level that already
    /// failed, and waits for the doubled interval.  Only a successful retest
    /// -- proof the floor has fallen -- forgets known-bad and resets the
    /// interval.  Without that memory a fixed floor produced a cycle of
    /// succeed / fail / succeed that kept the interval at its base forever:
    /// 9.3% of actions below the floor and a x1.81 swing, measured.
    ///
    /// The cap is in on-target confirmations that TESTED the level (an XCH
    /// cancel clamped up to min_fee does not).  Each failed retest leaves the
    /// two or three spends submitted at the probe level stuck, so the cap
    /// trades following a fallen floor against disturbing a fixed one:
    /// measured against the simulated floor, 64 put 4.0% of actions below the
    /// floor and 256 puts 1.2%.  256 is about four days of this wallet's CAT
    /// cancels; overpaying a fallen floor for that long costs about 0.05 XCH,
    /// and the node's feed-forward floor, when there is one, drops at once.
    double        probe_fraction{0.15};
    std::uint32_t probe_after_confirmations{8};
    std::uint32_t probe_confirmations{3};
    double        probe_fail_bump{0.10};
    std::uint32_t probe_backoff_cap{256};

    /// Multiplier on the node's estimate and on its admission floor.
    double ff_margin{1.10};

    /// A feed-forward reading older than this many peak heights is dropped
    /// (32 = 10 min).  The learned level then stands alone.
    std::uint32_t ff_max_age_blocks{32};

    ClassCosts costs{};
};

// ---------------------------------------------------------------------------
// Observations and results
// ---------------------------------------------------------------------------

enum class Signal : std::uint8_t {
    Confirmed = 0,       ///< a tracked spend confirmed; blocks = its delay
    Pending,             ///< a tracked spend is still pending; blocks = its age
    MempoolRejected,     ///< sent_to carries a fee error
    PendingChangeStuck,  ///< Step 8's pending_change counter is half way
    ForceDelete,         ///< Step 8 force-deleted unconfirmed transactions
};

struct Observation {
    Signal        signal{Signal::Confirmed};
    /// Delay (Confirmed) or age (Pending) in peak heights.  Ignored otherwise.
    double        blocks{0.0};
    /// True when the evidence belongs to ONE spend whose submitted rate is
    /// known; `submit_level` is then level_of(fee paid, class).
    bool          attributed{false};
    double        submit_level{0.0};
    /// Current peak height, for the dead-time rule.
    std::uint32_t now{0};
};

/// ZERO IS "NOTHING HAPPENED" (book_side_quality.hpp's rule).
enum class ChangeReason : std::uint8_t {
    None = 0,
    Disabled,
    Invalid,            ///< non-finite or negative input; ignored
    WarmUp,
    OnTarget,
    Answered,           ///< too-low evidence the level has already answered
    DeadTime,           ///< unattributed evidence inside the dead time
    Uncorroborated,     ///< unattributed evidence past its streak limit
    CensoredDelay,      ///< raise: a spend is pending past the target
    LateConfirmation,   ///< raise: a spend confirmed late
    SentToError,        ///< raise: the node refused a fee
    PendingChange,      ///< raise: pending_change is persisting
    ForceDelete,        ///< raise: Step 8 force-deleted
    ProbeDown,          ///< scheduled step down
    ProbeSucceeded,
    ProbeFailed,        ///< back to last-good plus the bump
    FeedForward,        ///< the node's floor moved the effective rate
};

[[nodiscard]] constexpr const char* to_string(ChangeReason r) noexcept
{
    switch (r) {
        case ChangeReason::None:             return "none";
        case ChangeReason::Disabled:         return "disabled";
        case ChangeReason::Invalid:          return "invalid";
        case ChangeReason::WarmUp:           return "warm-up";
        case ChangeReason::OnTarget:         return "on-target";
        case ChangeReason::Answered:         return "answered";
        case ChangeReason::DeadTime:         return "dead-time";
        case ChangeReason::Uncorroborated:   return "uncorroborated";
        case ChangeReason::CensoredDelay:    return "censored delay";
        case ChangeReason::LateConfirmation: return "late confirmation";
        case ChangeReason::SentToError:      return "sent_to error";
        case ChangeReason::PendingChange:    return "pending_change";
        case ChangeReason::ForceDelete:      return "force-delete";
        case ChangeReason::ProbeDown:        return "probe-down";
        case ChangeReason::ProbeSucceeded:   return "probe-succeeded";
        case ChangeReason::ProbeFailed:      return "probe-failed";
        case ChangeReason::FeedForward:      return "feed-forward";
    }
    return "unknown";
}

struct Change {
    bool         moved{false};      ///< the effective rate changed
    double       old_rate{0.0};     ///< mojos per cost, feed-forward included
    double       new_rate{0.0};
    ChangeReason reason{ChangeReason::None};
};

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

class Controller {
public:
    /// Tolerance when comparing a submitted level with the current one: a fee
    /// is ceil()ed to whole mojos, so a spend submitted AT the level reads a
    /// hair above it.
    static constexpr double kLevelEps = 1e-6;

    /// Levels are clamped here before any pow(), whatever the fee bounds say.
    static constexpr double kLevelAbsMax = 64.0;

    /// Wallet-level signals that may raise the level back to back with no
    /// attributed observation between them: three is x8 at the default step.
    static constexpr std::uint32_t kMaxUncorroboratedRaises = 3;


    explicit Controller(ControllerConfig cfg = {},
                        std::uint64_t min_fee_mojos = 0,
                        std::uint64_t max_fee_mojos = 0)
        : cfg_{sanitised(cfg)}
        , min_fee_{std::min(min_fee_mojos, max_fee_mojos)}
        , max_fee_{std::min(std::max(min_fee_mojos, max_fee_mojos), kFeeCeiling)}
    {
        // level 0 <=> a CAT cancel, the commonest spend, pays EXACTLY min_fee,
        // for every min_fee config.cpp accepts (any value >= 1 with the
        // controller on).  [review #163] An earlier revision floored the
        // anchor at 1,000,000 mojos, so min_fee_mojos: 5000 -- the value in
        // config.example.yaml -- started at 200x the operator's floor.  The
        // one-mojo guard below exists only for direct construction with 0,
        // which the parser refuses; a multiple of 0 would be 0 for ever.
        const double ref_cost = static_cast<double>(std::max<std::uint64_t>(cfg_.costs.cancel_cat, 1U));
        anchor_rate_ = static_cast<double>(std::max<std::uint64_t>(min_fee_, 1U)) / ref_cost;

        // [lo, hi]: outside it EVERY class is pinned at min_fee (below) or at
        // max_fee (above), so the level has no authority and further error
        // would accumulate into nothing.  This clamp is the anti-windup.
        const double lo_rate = static_cast<double>(std::max<std::uint64_t>(min_fee_, 1U))
                             / static_cast<double>(std::max<std::uint64_t>(max_cost(cfg_.costs), 1U));
        const double hi_rate = static_cast<double>(std::max<std::uint64_t>(max_fee_, 1U))
                             / static_cast<double>(std::max<std::uint64_t>(min_cost(cfg_.costs), 1U));
        level_lo_ = std::clamp(std::log2(lo_rate / anchor_rate_), -kLevelAbsMax, 0.0);
        level_hi_ = std::clamp(std::log2(hi_rate / anchor_rate_), 0.0, kLevelAbsMax);
        probe_interval_ = cfg_.probe_after_confirmations;
    }

    [[nodiscard]] bool enabled() const noexcept { return cfg_.enabled; }
    [[nodiscard]] const ControllerConfig& config() const noexcept { return cfg_; }

    // -- outputs ------------------------------------------------------------

    /// The learned level, log2 over the anchor.  Feed-forward NOT included.
    [[nodiscard]] double level() const noexcept { return level_; }
    [[nodiscard]] double level_lo() const noexcept { return level_lo_; }
    [[nodiscard]] double level_hi() const noexcept { return level_hi_; }
    [[nodiscard]] double anchor_rate() const noexcept { return anchor_rate_; }

    /// anchor x 2^level: what the loop has learned, in mojos per cost.
    [[nodiscard]] double learned_rate() const noexcept
    {
        return anchor_rate_ * std::exp2(std::clamp(level_, -kLevelAbsMax, kLevelAbsMax));
    }

    /// The feed-forward floor in force at `now`, mojos per cost; 0 when there
    /// is none or the reading is older than ff_max_age_blocks.
    [[nodiscard]] double feed_forward_rate(std::uint32_t now) const noexcept
    {
        if (!ff_known_) {
            return 0.0;
        }
        const std::uint32_t age = now >= ff_block_ ? now - ff_block_ : 0U;
        if (age > cfg_.ff_max_age_blocks) {
            return 0.0;
        }
        return ff_rate_;
    }

    /// The rate fees are computed from: the larger of learned and floor.
    [[nodiscard]] double effective_rate(std::uint32_t now) const noexcept
    {
        return std::max(learned_rate(), feed_forward_rate(now));
    }

    /// The fee for one action, clamped to [min_fee, max_fee].  Disabled:
    /// `passthrough` unchanged.
    [[nodiscard]] std::uint64_t fee_for(ActionClass c, std::uint32_t now,
                                        std::uint64_t passthrough = 0) const noexcept
    {
        if (!cfg_.enabled) {
            return passthrough;
        }
        const std::uint64_t raw = fee_from_rate(effective_rate(now), cost_of(cfg_.costs, c));
        return std::clamp(raw, min_fee_, max_fee_);
    }

    /// The level a fee of `fee_mojos` on class `c` was really submitted at.
    /// A fee clamped UP by min_fee reads above the level, one clamped DOWN by
    /// max_fee or the budget reads below it -- which is what lets the
    /// ANSWERED rule bound windup behind a saturated actuator.
    [[nodiscard]] double level_of(std::uint64_t fee_mojos, ActionClass c) const noexcept
    {
        const std::uint64_t cost = std::max<std::uint64_t>(cost_of(cfg_.costs, c), 1U);
        if (fee_mojos == 0U) {
            return -kLevelAbsMax;
        }
        const double rate = static_cast<double>(fee_mojos) / static_cast<double>(cost);
        return std::clamp(std::log2(rate / anchor_rate_), -kLevelAbsMax, kLevelAbsMax);
    }

    // -- feed-forward -------------------------------------------------------

    /// One node reading.  `estimate_rate` is get_fee_estimate's answer per
    /// unit of cost; `admission_floor_rate` is the node's own floor when its
    /// mempool is at capacity (max(5, mempool_min_fees)), else 0.  A
    /// non-finite or negative value counts as 0: the node did not say.
    Change set_feed_forward(double estimate_rate, double admission_floor_rate,
                            std::uint32_t now) noexcept
    {
        Change out{};
        if (!cfg_.enabled) {
            out.reason = ChangeReason::Disabled;
            return out;
        }
        const double before = effective_rate(now);
        const double est    = (std::isfinite(estimate_rate) && estimate_rate > 0.0) ? estimate_rate : 0.0;
        const double floor_ = (std::isfinite(admission_floor_rate) && admission_floor_rate > 0.0)
                                  ? admission_floor_rate : 0.0;
        ff_rate_  = std::max(est, floor_) * cfg_.ff_margin;
        ff_block_ = now;
        ff_known_ = true;
        return finish(out, before, now, ChangeReason::FeedForward);
    }

    /// The node did not answer.  The last reading ages out on its own.
    void clear_feed_forward() noexcept { ff_known_ = false; ff_rate_ = 0.0; }

    // -- feedback -----------------------------------------------------------

    Change observe(const Observation& o) noexcept
    {
        Change out{};
        if (!cfg_.enabled) {
            out.reason = ChangeReason::Disabled;
            return out;
        }
        const bool timed = o.signal == Signal::Confirmed || o.signal == Signal::Pending;
        if ((timed && !(std::isfinite(o.blocks) && o.blocks >= 0.0))
            || (o.attributed && !std::isfinite(o.submit_level))) {
            out.reason = ChangeReason::Invalid;
            return out;
        }
        const double before = effective_rate(o.now);
        const double target = static_cast<double>(cfg_.target_delay_blocks);

        // 1. The error this observation reports.
        double       err    = 0.0;
        ChangeReason reason = ChangeReason::OnTarget;
        switch (o.signal) {
            case Signal::Confirmed:
                if (o.blocks > target) {
                    err    = std::min((o.blocks - target) / target, cfg_.max_error);
                    reason = ChangeReason::LateConfirmation;
                }
                break;
            case Signal::Pending:
                if (!(o.blocks > target)) {
                    // Not late yet: no information either way.
                    out.reason = ChangeReason::None;
                    return out;
                }
                err    = std::min((o.blocks - target) / target, cfg_.max_error);
                reason = ChangeReason::CensoredDelay;
                break;
            case Signal::MempoolRejected:
                err    = cfg_.max_error;
                reason = ChangeReason::SentToError;
                break;
            case Signal::PendingChangeStuck:
                err    = std::min(1.0, cfg_.max_error);
                reason = ChangeReason::PendingChange;
                break;
            case Signal::ForceDelete:
                err    = cfg_.max_error;
                reason = ChangeReason::ForceDelete;
                break;
        }

        // 2. Warm-up: history moves, the level does not.
        if (observations_ < cfg_.warmup_observations) {
            ++observations_;
            push_error(err);
            out.reason = ChangeReason::WarmUp;
            return out;
        }
        if (observations_ != std::numeric_limits<std::uint32_t>::max()) {
            ++observations_;
        }

        // 3. On target.
        if (!(err > 0.0)) {
            push_error(0.0);
            // A confirmation says the rate it was SUBMITTED at works.  Paid
            // above the level (clamped up by min_fee, or submitted before a
            // probe-down), it says nothing about the level.
            if (o.attributed && o.submit_level > level_ + kLevelEps) {
                out.reason = ChangeReason::OnTarget;
                return out;
            }
            uncorroborated_raises_ = 0;
            return on_target(out, before, o.now);
        }

        // 4. Too low -- if the evidence is about the level we are at.
        if (o.attributed) {
            if (level_ >= o.submit_level + cfg_.min_raise - kLevelEps) {
                out.reason = ChangeReason::Answered;
                return out;
            }
            // A failed probe leaves two or three spends stuck at the probe
            // level.  The first one to speak fails the probe; the level is
            // already back above known-bad when the rest do, and "known-bad
            // is bad" is not news.  Without this they drove the restored
            // level up to min_raise above the PROBE level -- measured against
            // the simulated floor as a x2.9 swing at a floor that never moved.
            if (bad_known_ && o.submit_level <= bad_level_ + kLevelEps
                && level_ > bad_level_ + kLevelEps) {
                out.reason = ChangeReason::Answered;
                return out;
            }
            // While a probe is in flight, only a spend that paid AT LEAST the
            // probe level can fail it: "a lower level is too low" says nothing
            // about this one.
            if (probing_ && o.submit_level < level_ - kLevelEps) {
                out.reason = ChangeReason::Answered;
                return out;
            }
        } else {
            if (raised_once_ && !probing_) {
                const std::uint32_t since =
                    o.now >= last_raise_block_ ? o.now - last_raise_block_ : 0U;
                if (since / 2U < cfg_.target_delay_blocks) {
                    out.reason = ChangeReason::DeadTime;
                    return out;
                }
            }
            if (uncorroborated_raises_ >= kMaxUncorroboratedRaises) {
                out.reason = ChangeReason::Uncorroborated;
                return out;
            }
        }
        if (o.attributed) {
            uncorroborated_raises_ = 0;
        } else {
            ++uncorroborated_raises_;
        }
        on_target_run_ = 0;

        // 5. A failed probe is undone, not integrated: last-good plus a bump.
        //    The level it failed at becomes KNOWN-BAD and the retest interval
        //    doubles, so a fixed floor is retested ever more rarely.
        if (probing_) {
            probing_        = false;
            probe_confirms_ = 0;
            bad_known_      = true;
            bad_level_      = level_;
            // good_level_ is the LOWEST level verified good since the bracket
            // opened, so a second failure restores to the same place as the
            // first: the bump never compounds.
            level_ = std::clamp(good_level_ + std::log2(1.0 + cfg_.probe_fail_bump), level_lo_, level_hi_);
            probe_interval_ = (probe_interval_ > cfg_.probe_backoff_cap / 2U)
                                  ? cfg_.probe_backoff_cap
                                  : probe_interval_ * 2U;
            probe_interval_ = std::max(probe_interval_, cfg_.probe_after_confirmations);
            push_error(err);
            note_raise(o.now);
            return finish(out, before, o.now, ChangeReason::ProbeFailed);
        }
        // Too low WITHOUT a probe in flight: the floor itself has risen past
        // the level.  The bracket below it is moot and so is its backoff: this
        // is a new regime, explored from the base interval.
        bad_known_      = false;
        good_valid_     = false;
        probe_interval_ = cfg_.probe_after_confirmations;

        // 6. The PID, velocity form, raises only.
        const double d1 = err - err_1_;
        const double d2 = err - 2.0 * err_1_ + err_2_;
        double up = cfg_.kp * std::max(0.0, d1) + cfg_.ki * err + cfg_.kd * std::max(0.0, d2);
        if (!std::isfinite(up) || up < 0.0) {
            up = 0.0;
        }
        up     = std::min(up, cfg_.max_step_up);
        level_ = std::clamp(level_ + up, level_lo_, level_hi_);
        push_error(err);
        note_raise(o.now);
        return finish(out, before, o.now, reason);
    }

    // -- telemetry and tests ------------------------------------------------

    [[nodiscard]] bool          is_warm() const noexcept { return cfg_.enabled && observations_ >= cfg_.warmup_observations; }
    [[nodiscard]] bool          probing() const noexcept { return probing_; }
    [[nodiscard]] double        good_level() const noexcept { return good_level_; }
    /// The level of the last failed probe, while it is still remembered.
    [[nodiscard]] bool          bad_level_known() const noexcept { return bad_known_; }
    [[nodiscard]] double        bad_level() const noexcept { return bad_level_; }
    [[nodiscard]] std::uint32_t probe_interval() const noexcept { return probe_interval_; }
    [[nodiscard]] std::uint32_t on_target_run() const noexcept { return on_target_run_; }
    [[nodiscard]] std::uint64_t min_fee() const noexcept { return min_fee_; }
    [[nodiscard]] std::uint64_t max_fee() const noexcept { return max_fee_; }

    /// True when `c` would be clamped by max_fee at `now`: the actuator is
    /// saturated for that class.
    [[nodiscard]] bool saturated(ActionClass c, std::uint32_t now) const noexcept
    {
        return fee_from_rate(effective_rate(now), cost_of(cfg_.costs, c)) > max_fee_;
    }

private:
    /// Out-of-range tuning is replaced by the default, so a Controller built
    /// from unvalidated values is still well defined.  config.cpp REFUSES the
    /// same ranges; this is the second line, not the first.
    [[nodiscard]] static ControllerConfig sanitised(ControllerConfig c) noexcept
    {
        const ControllerConfig d{};
        const auto gain_ok = [](double g) { return std::isfinite(g) && g >= 0.0 && g <= 16.0; };
        if (c.target_delay_blocks == 0U) { c.target_delay_blocks = d.target_delay_blocks; }
        if (!gain_ok(c.kp)) { c.kp = d.kp; }
        if (!gain_ok(c.ki)) { c.ki = d.ki; }
        if (!gain_ok(c.kd)) { c.kd = d.kd; }
        if (!(std::isfinite(c.max_error) && c.max_error > 0.0 && c.max_error <= 16.0)) { c.max_error = d.max_error; }
        if (!(std::isfinite(c.max_step_up) && c.max_step_up > 0.0 && c.max_step_up <= 8.0)) { c.max_step_up = d.max_step_up; }
        if (!(std::isfinite(c.min_raise) && c.min_raise > 0.0 && c.min_raise <= 8.0)) { c.min_raise = d.min_raise; }
        if (!(std::isfinite(c.probe_fraction) && c.probe_fraction > 0.0 && c.probe_fraction <= 0.9)) { c.probe_fraction = d.probe_fraction; }
        if (c.probe_after_confirmations == 0U) { c.probe_after_confirmations = d.probe_after_confirmations; }
        if (c.probe_confirmations == 0U) { c.probe_confirmations = d.probe_confirmations; }
        if (!(std::isfinite(c.probe_fail_bump) && c.probe_fail_bump >= 0.0 && c.probe_fail_bump <= 1.0)) { c.probe_fail_bump = d.probe_fail_bump; }
        if (c.probe_backoff_cap < c.probe_after_confirmations) { c.probe_backoff_cap = c.probe_after_confirmations; }
        if (!(std::isfinite(c.ff_margin) && c.ff_margin >= 1.0 && c.ff_margin <= 4.0)) { c.ff_margin = d.ff_margin; }
        if (c.costs.offer_attached == 0U) { c.costs.offer_attached = d.costs.offer_attached; }
        if (c.costs.cancel_xch == 0U) { c.costs.cancel_xch = d.costs.cancel_xch; }
        if (c.costs.cancel_cat == 0U) { c.costs.cancel_cat = d.costs.cancel_cat; }
        if (c.costs.take == 0U) { c.costs.take = d.costs.take; }
        return c;
    }

    void push_error(double err) noexcept
    {
        err_2_ = err_1_;
        err_1_ = err;
    }

    void note_raise(std::uint32_t now) noexcept
    {
        raised_once_      = true;
        last_raise_block_ = now;
    }

    Change on_target(Change out, double before, std::uint32_t now) noexcept
    {
        if (on_target_run_ != std::numeric_limits<std::uint32_t>::max()) {
            ++on_target_run_;
        }
        if (probing_) {
            ++probe_confirms_;
            if (probe_confirms_ >= cfg_.probe_confirmations) {
                probing_        = false;
                probe_confirms_ = 0;
                good_level_     = level_;
                good_valid_     = true;
                if (probe_is_retest_) {
                    // A level that failed before now works: the floor has
                    // fallen.  Forget it and probe at the base interval again.
                    bad_known_      = false;
                    probe_interval_ = cfg_.probe_after_confirmations;
                }
                return finish(out, before, now, ChangeReason::ProbeSucceeded);
            }
            out.reason = ChangeReason::OnTarget;
            return out;
        }
        if (level_ > level_lo_ + kLevelEps) {
            double target =
                std::clamp(level_ + std::log2(1.0 - cfg_.probe_fraction), level_lo_, level_hi_);
            // Sitting above a level already verified good (the bump after a
            // failed probe): give the bump back first.  That step is inside the
            // bracket, so it runs at the base interval and risks nothing the
            // loop has not already seen work.
            if (good_valid_ && good_level_ < level_ - kLevelEps && target < good_level_) {
                target = good_level_;
            }
            const bool retest = bad_known_ && target <= bad_level_ + kLevelEps;
            const std::uint32_t needed = retest ? probe_interval_ : cfg_.probe_after_confirmations;
            if (on_target_run_ >= needed) {
                if (!good_valid_ || level_ < good_level_) {
                    good_level_ = level_;      // this run just verified it
                    good_valid_ = true;
                }
                probing_         = true;
                probe_is_retest_ = retest;
                probe_confirms_  = 0;
                on_target_run_   = 0;
                level_           = target;
                return finish(out, before, now, ChangeReason::ProbeDown);
            }
        }
        out.reason = ChangeReason::OnTarget;
        return out;
    }

    [[nodiscard]] Change finish(Change out, double before, std::uint32_t now,
                                ChangeReason reason) const noexcept
    {
        out.old_rate = before;
        out.new_rate = effective_rate(now);
        out.reason   = reason;
        out.moved    = out.new_rate != before;
        return out;
    }

    ControllerConfig cfg_;
    std::uint64_t    min_fee_{0};
    std::uint64_t    max_fee_{0};
    double           anchor_rate_{1.0};
    double           level_lo_{0.0};
    double           level_hi_{0.0};

    double           level_{0.0};
    double           err_1_{0.0};
    double           err_2_{0.0};
    std::uint32_t    observations_{0};
    bool             raised_once_{false};
    std::uint32_t    last_raise_block_{0};
    std::uint32_t    uncorroborated_raises_{0};

    bool             probing_{false};
    bool             probe_is_retest_{false};
    double           good_level_{0.0};
    bool             good_valid_{false};
    bool             bad_known_{false};
    double           bad_level_{0.0};
    std::uint32_t    probe_confirms_{0};
    std::uint32_t    on_target_run_{0};
    std::uint32_t    probe_interval_{8};

    bool             ff_known_{false};
    double           ff_rate_{0.0};
    std::uint32_t    ff_block_{0};
};

// ---------------------------------------------------------------------------
// Reachability advisory (pid_reachability.hpp's DERIVE-don't-validate shape)
// ---------------------------------------------------------------------------

/// What the configured bounds and gains can actually do.  Reported, never
/// thrown: every finding resolves deterministically, so it advises.
struct Reachability {
    /// Observations at max_error needed to walk the level from lo to hi.
    /// 0 when the gains cannot raise at all.
    std::uint32_t raises_to_span{0};
    /// kp = ki = kd = 0: the loop can never raise.  Probes still lower it.
    bool          cannot_raise{false};
    /// Per class: max_fee / cost < 5 mojos per cost, so NO level can get that
    /// class into a full mempool.  This is the live configuration's state for
    /// cancel_cat and take (100M / 42.3M = 2.4).
    bool          max_fee_below_full_mempool[kActionClassCount]{};
    /// The smallest max_fee_mojos that clears a full mempool for every
    /// class, ff_margin included.
    std::uint64_t max_fee_for_full_mempool{0};
};

[[nodiscard]] inline Reachability reachability(const ControllerConfig& cfg,
                                               std::uint64_t min_fee_mojos,
                                               std::uint64_t max_fee_mojos) noexcept
{
    Reachability r{};
    ControllerConfig switched_on = cfg;
    switched_on.enabled = true;
    const Controller probe{switched_on, min_fee_mojos, max_fee_mojos};
    const ControllerConfig& c = probe.config();
    // A sustained hard signal: e = max_error every time, so d1 = d2 = 0 after
    // the first and the raise per observation is ki * max_error (capped).
    const double per_obs = std::min(c.ki * c.max_error, c.max_step_up);
    const double first   = std::min((c.kp + c.ki + c.kd) * c.max_error, c.max_step_up);
    r.cannot_raise = !(first > 0.0);
    const double span = probe.level_hi() - probe.level_lo();
    if (!r.cannot_raise && span > 0.0) {
        const double rest = std::max(0.0, span - first);
        const double n = per_obs > 0.0 ? 1.0 + std::ceil(rest / per_obs)
                                       : (rest > 0.0 ? 0.0 : 1.0);
        r.raises_to_span = n >= 4'000'000'000.0 ? std::numeric_limits<std::uint32_t>::max()
                                                : static_cast<std::uint32_t>(n);
        if (!(per_obs > 0.0) && rest > 0.0) {
            r.cannot_raise = true;   // ki = 0: one kick, then nothing
        }
    }
    const ActionClass all[kActionClassCount] = {ActionClass::OfferAttached, ActionClass::CancelXch,
                                                ActionClass::CancelCat, ActionClass::Take};
    const double need_rate = kFullMempoolMinRate * c.ff_margin;
    for (std::size_t i = 0; i < kActionClassCount; ++i) {
        const std::uint64_t need = fee_from_rate(need_rate, cost_of(c.costs, all[i]));
        r.max_fee_below_full_mempool[i] = probe.max_fee() < need;
        r.max_fee_for_full_mempool = std::max(r.max_fee_for_full_mempool, need);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Rate-limited change log
// ---------------------------------------------------------------------------

/// One log line per level change is still too many when a stuck spend raises
/// the level every heartbeat.  This folds a burst into one line: it opens on
/// the first move, and is due when `min_gap_blocks` have passed since the last
/// line.  The line then reports first-old -> latest-new and how many moves it
/// covers.  Hard reasons are due immediately.
struct ChangeLogGate {
    std::uint32_t min_gap_blocks{4};

    bool          open{false};
    double        first_old_rate{0.0};
    double        latest_new_rate{0.0};
    ChangeReason  latest_reason{ChangeReason::None};
    std::uint32_t folded{0};
    std::uint32_t last_emit_block{0};
    bool          emitted_once{false};

    /// Feed one Change.  Returns true when a line is due NOW; the caller
    /// formats it from the members above and then calls emitted().
    [[nodiscard]] bool note(const Change& ch, std::uint32_t now) noexcept
    {
        if (!ch.moved) {
            return false;
        }
        if (!open) {
            open           = true;
            first_old_rate = ch.old_rate;
            folded         = 0;
        }
        latest_new_rate = ch.new_rate;
        latest_reason   = ch.reason;
        if (folded != std::numeric_limits<std::uint32_t>::max()) {
            ++folded;
        }
        const bool hard = ch.reason == ChangeReason::ForceDelete
                       || ch.reason == ChangeReason::SentToError
                       || ch.reason == ChangeReason::ProbeFailed
                       || ch.reason == ChangeReason::ProbeDown;
        const std::uint32_t since = now >= last_emit_block ? now - last_emit_block : 0U;
        return hard || !emitted_once || since >= min_gap_blocks;
    }

    void emitted(std::uint32_t now) noexcept
    {
        open            = false;
        folded          = 0;
        last_emit_block = now;
        emitted_once    = true;
    }
};

// ---------------------------------------------------------------------------
// Spend tracker -- turns "we submitted X" into observations
// ---------------------------------------------------------------------------

/// One fee-paying spend of ours, from submission to a verdict.
struct Ticket {
    ActionClass   cls{ActionClass::CancelCat};
    std::uint32_t submit_block{0};
    double        submit_level{0.0};
    /// Height of the last Pending observation emitted, so a spend speaks at
    /// most once per height.
    std::uint32_t last_pending_block{0};
    /// Set when the spend left the engine's pending set without a verdict
    /// yet (a cancel whose offer left State awaits Step 2's wallet verdict).
    bool          awaiting_verdict{false};
    std::uint32_t left_block{0};
};

/// How long a ticket that left the pending set waits for its verdict before
/// it is dropped WITHOUT an observation: 256 peak heights = 80 min.  It may
/// have been a fill, which says nothing about our fee.
inline constexpr std::uint32_t kVerdictTtlBlocks = 256;

/// Tickets kept at once.  The live book rests ~22 offers; 512 is a bound,
/// not a budget.
inline constexpr std::size_t kMaxTickets = 512;

/// Age of a ticket in peak heights; 0 on a height regression (no wrap).
[[nodiscard]] constexpr std::uint32_t ticket_age(const Ticket& t, std::uint32_t now) noexcept
{
    return now >= t.submit_block ? now - t.submit_block : 0U;
}

/// Is a Pending observation due for this ticket at `now`?  Only past the
/// target, only while it is still pending, and once per height.
[[nodiscard]] constexpr bool pending_observation_due(const Ticket& t, std::uint32_t now,
                                                     std::uint32_t target_delay_blocks) noexcept
{
    return !t.awaiting_verdict
        && ticket_age(t, now) > target_delay_blocks
        && t.last_pending_block != now;
}

/// Has a ticket awaiting its verdict outlived kVerdictTtlBlocks?
[[nodiscard]] constexpr bool verdict_expired(const Ticket& t, std::uint32_t now) noexcept
{
    return t.awaiting_verdict
        && (now >= t.left_block ? now - t.left_block : 0U) > kVerdictTtlBlocks;
}

/// The confirmation delay of a ticket whose spend landed at
/// `confirmed_block`, in peak heights; 0 when the verdict height is behind
/// the submission (the first sighting can trail the real submission by one
/// heartbeat, never lead it).
[[nodiscard]] constexpr std::uint32_t confirmation_delay(const Ticket& t,
                                                         std::uint32_t confirmed_block) noexcept
{
    return confirmed_block >= t.submit_block ? confirmed_block - t.submit_block : 0U;
}

/// What Step 2's terminal verdict on a ticketed cancel tells the controller.
///
/// [review #163] OfferManager::recheck_terminal answers StillTerminal for the
/// wallet statuses CANCELLED and FAILED alike.  Only CANCELLED says the cancel
/// spend confirmed.  FAILED says the offer died some other way, and fed as an
/// on-target confirmation it could validate a probe and LOWER the fee on no
/// evidence.  So: `has` is false unless the wallet said CANCELLED, and the
/// caller drops the ticket unheard.
struct VerdictObservation {
    bool        has{false};
    Observation observation{};
};

[[nodiscard]] constexpr VerdictObservation observation_for_cancel_verdict(
    const Ticket& t, bool wallet_says_cancelled, std::uint32_t observed_block,
    std::uint32_t now) noexcept
{
    VerdictObservation out{};
    if (!wallet_says_cancelled) {
        return out;
    }
    out.has                      = true;
    out.observation.signal       = Signal::Confirmed;
    out.observation.blocks       = static_cast<double>(confirmation_delay(t, observed_block));
    out.observation.attributed   = true;
    out.observation.submit_level = t.submit_level;
    out.observation.now          = now;
    return out;
}

static_assert(!observation_for_cancel_verdict(Ticket{}, false, 10U, 10U).has);
static_assert(observation_for_cancel_verdict(Ticket{}, true, 10U, 10U).has);
static_assert(confirmation_delay(Ticket{ActionClass::Take, 100U, 0.0, 0U, false, 0U}, 96U) == 0U);
static_assert(pending_observation_due(Ticket{ActionClass::Take, 100U, 0.0, 0U, false, 0U}, 109U, 8U));
static_assert(!pending_observation_due(Ticket{ActionClass::Take, 100U, 0.0, 0U, false, 0U}, 108U, 8U));

}  // namespace xop::strategy::fee

#endif  // XOP_STRATEGY_FEE_CONTROLLER_HPP
