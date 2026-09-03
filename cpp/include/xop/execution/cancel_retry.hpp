#ifndef XOP_EXECUTION_CANCEL_RETRY_HPP
#define XOP_EXECUTION_CANCEL_RETRY_HPP
// ---------------------------------------------------------------------------
// cancel_retry.hpp -- may we attempt the shutdown cancel AGAIN, and when?
//
// [S46 2026-09-02] WHAT WAS OBSERVED
// ----------------------------------
// A planned graceful stop ran the whole shutdown path correctly and then:
//
//   13:10:17 [error]    Failed to cancel offer 0x9a72dfe913: Wallet needs to
//                       be fully synced before making transactions.
//   13:10:17 [info]     cancel_all: 0/7 offers cancelled successfully
//   13:10:17 [critical] [S31] graceful cancellation got 0/7 -- invoking the
//                       independent fallback
//   13:10:17 [critical] [S31] cancel FAILED: Wallet needs to be fully synced
//   13:10:17 [error]    [ALERT:CRITICAL] DEAD MAN'S SWITCH COULD NOT CANCEL
//
// Seven offers were left live and unmanaged and the process exited cleanly.
// Every one of those five lines is in the SAME SECOND: the engine tried
// exactly once, and the S31 "independent fallback" goes through the same
// wallet RPC, so it failed for the same reason in the same millisecond. A net
// that fails identically to the thing it catches is not a second chance.
//
// This header is the policy that decides whether to try again. It is a pure
// function so a gtest can hold it: nothing in cpp/tests constructs an Engine
// (S36), so a retry verified only by reading engine.cpp is not verified.
//
// WHY WALL CLOCK AND NOT BLOCKS
// -----------------------------
// Everything in take_retry.hpp is block-keyed because Step 9c runs on the
// block cadence and its holds are minutes-to-hours. A shutdown is neither: it
// is racing several INDEPENDENT stoppers with wall-clock budgets, and the
// engine does not get another cycle to make up for a slow one.
//
//   GUI _stop_engine_process ... 30 s, then terminate()   (engine_bridge.py)
//   console Ctrl-C ............. unbounded; 2nd signal -> std::_Exit()
//   hand-written shutdown.flag . ~600 s (the watchdog's own stall threshold)
//   SIGTERM / taskkill /F ...... 0 s -- no handler runs at all
//   console window close (X) ... ~5 s -- CTRL_CLOSE_EVENT is not mapped
//   a new engine starting ...... 0 s -- kill_old_instances() TerminateProcess
//
// The incident was the third row (600 s), NOT the first. Sizing this to 30 s
// would size it for the one waiter that hard-kills us regardless.
//
// THE BUDGET, AND WHY 90 s
// ------------------------
// Two independent measurements of the desync flap, both from this repo:
//
//   this session, 7.18 h of Step-8 sync-gate samples, 1159 cycles:
//       8 episodes; 7 of 8 lasted a single cycle; longest span 80.9 s
//   take_retry.hpp:50-57, 35.2 h (its raw logs have since rotated away):
//       92 episodes, 87 single-cycle (95%), median 0 s, longest 155 s
//
// Combined ~42 h: ~100 episodes, ~94% clear inside one block, worst ever
// observed 155 s. Emphatically a flap, not a condition.
//
// 90 s clears the 80.9 s episode and the whole single-cycle mass. It is
// BELOW the 155 s worst-ever and this header does not pretend otherwise --
// see kBudgetDoesNotCoverWorstObservedEpisode in the test, which pins that
// admission so nobody later reads the number as a guarantee.
//
// [review] That paragraph was FALSE as first shipped. max_attempts was 5, so
// the ladder stopped on its own attempt ceiling at t=75 s -- 5.9 s before the
// 80.9 s episode would have cleared -- and budget_ms never bound at all. The
// number quoted in the design was not the number the code delivered. Both are
// now proved by cancel_last_attempt_start_ms() at compile time rather than
// asserted here, because a comment cannot notice when max_attempts is edited.
//
// The extra 60 s over a 30 s cap buys about 2%. It is taken anyway because
// the cost of a retry is a sleep, not an RPC: "Wallet needs to be fully
// synced" is an application-level rejection, rpc_post retries only transport
// and 5xx (chia_rpc.cpp:563-577), and all seven incident failures logged in
// the same second. The loop is bounded on WALL CLOCK rather than attempt
// count precisely because the other shape -- a wallet that HANGS instead of
// refusing -- costs request_timeout{30s} x max_retries{3} per call, and an
// attempt-counted loop would then run for many minutes.
//
// WHAT THIS CANNOT SAVE, STATED PLAINLY
// -------------------------------------
// SIGTERM, taskkill /F, console-window close and kill_old_instances() run no
// shutdown path at all, so no retry length whatsoever helps them. Under a GUI
// stop the effective cap is 30 s no matter what this header says. That is why
// the retry is only half the fix: the durable half is the write-ahead cancel
// intent the engine persists BEFORE the first attempt, which the next engine
// reads and finishes. This header covers the flap; the intent file covers the
// kill.
//
// ON THE ZERO ENUMERATOR -- A DELIBERATE DEPARTURE
// ------------------------------------------------
// This repo's rule is "the zero enumerator declines" (take_retry.hpp,
// coin_pool_verdict.hpp), because there the dangerous direction is acting on
// a state we do not understand. Here BOTH directions are dangerous and they
// are not symmetric in kind:
//
//   * a default-constructed plan that RETRIES spins the shutdown forever --
//     an engine that will not exit is its own outage, and it destroys the
//     operator's escape hatch;
//   * a default-constructed plan that STOPS is the documented fail-open shape
//     "we tried once, call it done".
//
// So CancelRetryVerdict::Stop is the zero enumerator -- an inert default
// cannot hang anything -- and the fail-open is closed STRUCTURALLY instead of
// by the default: every Stop carries a CancelStopReason whose OWN zero
// enumerator is `Unknown`, the caller must name the still-live offer ids in
// its alert, and the intent file plus startup recovery make "gave up" a state
// the next process acts on rather than a silence. Stopping is allowed to be
// the default; stopping SILENTLY is not reachable.
//
// FUNDING IS NOT ON THIS SCHEDULE, AND THAT IS NOT A GIVE-UP
// ----------------------------------------------------------
// classify_take_failure() is reused verbatim -- writing a second classifier
// that can disagree with the first about the same RPC string is how two paths
// in one file end up believing different things. But the three classes do not
// share a schedule:
//
//   Unsynced -> the dominant case and self-clearing. Full ladder.
//   Other    -> unmodelled; it burned RPCs. Short leash, then stop.
//   Funding  -> waiting does not refill a wallet. Stop IMMEDIATELY with
//               NeedsEmergencyLadder so the caller escalates to the zero-fee
//               secure ladder NOW instead of burning 75 s first. That is an
//               escalation, not an abandonment, and the enumerator says so.
//
// Pure header: plain integers, no engine types, no asio, no RPC, no spdlog,
// no clock. Elapsed time is an INPUT. Driven by cpp/tests/test_cancel_retry.cpp.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xop/execution/take_retry.hpp"

namespace xop::execution {

// ---------------------------------------------------------------------------
// The verdict. Stop is zero -- see the header note on why this file departs
// from the usual "zero declines" reading of the rule.
// ---------------------------------------------------------------------------
enum class CancelRetryVerdict : int {
    Stop  = 0,  ///< do not attempt again. NEVER silently -- see stop_reason.
    Retry = 1,  ///< sleep delay_ms, then re-attempt the REMAINING ids.
};

// ---------------------------------------------------------------------------
// Why we stopped. `Unknown` is zero so a half-filled plan cannot claim a
// clean finish: the caller alerts on anything that is not Done.
// ---------------------------------------------------------------------------
enum class CancelStopReason : int {
    Unknown              = 0,  ///< nobody set this. Treat as a failure.
    Done                 = 1,  ///< nothing left outstanding. The only clean one.
    AttemptsExhausted    = 2,  ///< ladder ran out with offers still live.
    BudgetExhausted      = 3,  ///< wall-clock budget ran out.
    NeedsEmergencyLadder = 4,  ///< funding refusal: escalate, do not wait.
};

[[nodiscard]] constexpr const char* to_string(CancelStopReason r) noexcept
{
    switch (r) {
        case CancelStopReason::Done:                 return "done";
        case CancelStopReason::AttemptsExhausted:    return "attempts-exhausted";
        case CancelStopReason::BudgetExhausted:      return "budget-exhausted";
        case CancelStopReason::NeedsEmergencyLadder: return "needs-emergency-ladder";
        case CancelStopReason::Unknown:              break;
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* to_string(CancelRetryVerdict v) noexcept
{
    return v == CancelRetryVerdict::Retry ? "retry" : "stop";
}

// ---------------------------------------------------------------------------
// Tuning. Milliseconds, not blocks. See the header note.
// ---------------------------------------------------------------------------
struct CancelRetryConfig {
    /// Hard wall-clock cap on the whole retry sequence, measured from the
    /// START of the first attempt. This is the number that keeps the promise
    /// "the shutdown does not hang".
    std::uint32_t budget_ms{90'000};
    /// Total attempts including the first. 6 attempts => 5 retries at
    /// 5/10/20/40/40 s = 115 s of scheduled sleeping, which does NOT fit in
    /// budget_ms and is not meant to: `budget_ms` is what binds, and the last
    /// retry is clamped into whatever is left of it.
    ///
    /// [review] This was 5, and at 5 the ladder self-terminated on the
    /// attempt ceiling at t=75 s -- BEFORE the 90 s budget could ever bind,
    /// which made budget_ms decorative and made this header's own claim to
    /// "clear the 80.9 s episode measured this session" false: the last
    /// attempt went out at 75 s, 5.9 s before that episode ended. Six
    /// attempts put the final attempt at exactly budget_ms, which is proved
    /// below by static_assert rather than asserted in prose.
    std::uint32_t max_attempts{6};
    /// First retry delay; doubles per retry to max_delay_ms.
    std::uint32_t base_delay_ms{5'000};
    std::uint32_t max_delay_ms{40'000};
    /// A retry whose sleep would be clamped below this is not worth taking:
    /// it re-asks a wallet that has had no time to change its mind, and it
    /// turns the tail of the budget into a spin. Stop instead.
    std::uint32_t min_useful_delay_ms{1'000};
    /// TakeFailureClass::Other is not modelled. It gets a shorter leash than
    /// the sync flap: two retries, not four.
    std::uint32_t other_max_attempts{3};
};

// ---------------------------------------------------------------------------
// cancel_backoff_ms -- delay before retry N (1-based), doubling, saturating.
//
// Same shape as doubling_backoff_blocks() in take_retry.hpp, kept separate
// only because the unit differs; the shape must not drift.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr std::uint32_t
cancel_backoff_ms(std::uint32_t retry_index,
                  const CancelRetryConfig& cfg) noexcept
{
    if (retry_index == 0 || cfg.base_delay_ms == 0) return 0;
    std::uint64_t d = cfg.base_delay_ms;
    for (std::uint32_t i = 1; i < retry_index && d < cfg.max_delay_ms; ++i) {
        d *= 2;
    }
    const std::uint64_t capped =
        (d < cfg.max_delay_ms) ? d : static_cast<std::uint64_t>(cfg.max_delay_ms);
    return static_cast<std::uint32_t>(capped);
}

/// Total sleeping the full ladder would do, if nothing else stopped it.
/// Exists so a static_assert can prove the schedule fits inside the budget --
/// the two constants are edited independently and a ladder that overruns its
/// own cap would make the "does not hang" claim false at compile time.
[[nodiscard]] constexpr std::uint32_t
cancel_schedule_total_ms(const CancelRetryConfig& cfg) noexcept
{
    std::uint64_t total = 0;
    for (std::uint32_t i = 1; i < cfg.max_attempts; ++i) {
        total += cancel_backoff_ms(i, cfg);
    }
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(total, 0xFFFF'FFFFull));
}

static_assert(cancel_backoff_ms(1, CancelRetryConfig{}) == 5'000);
static_assert(cancel_backoff_ms(2, CancelRetryConfig{}) == 10'000);
static_assert(cancel_backoff_ms(3, CancelRetryConfig{}) == 20'000);
static_assert(cancel_backoff_ms(4, CancelRetryConfig{}) == 40'000);
static_assert(cancel_backoff_ms(9, CancelRetryConfig{}) == 40'000,
              "the ladder must saturate, not overflow");
static_assert(cancel_schedule_total_ms(CancelRetryConfig{}) == 115'000);
static_assert(cancel_schedule_total_ms(CancelRetryConfig{})
                  > CancelRetryConfig{}.budget_ms,
              "budget_ms must be the binding constraint. When the schedule "
              "fits inside the budget the ladder stops on its attempt "
              "ceiling and the wall-clock cap never fires -- which is how "
              "the 90 s number came to be quoted for a ladder that actually "
              "stopped at 75 s");

/// The longest measured desync episode in this repo's Step-8 sync-gate
/// sampling (7.18 h, 1159 cycles, 8 episodes). The budget is sized to reach
/// PAST this, and the static_assert below is what makes that a fact rather
/// than a sentence in a comment.
inline constexpr std::uint32_t kLongestMeasuredDesyncMs = 80'900;

/// The worst desync episode ever observed here (take_retry.hpp, 35.2 h).
/// The budget does NOT cover this and this header does not pretend it does.
inline constexpr std::uint32_t kWorstObservedDesyncMs = 155'000;

// ---------------------------------------------------------------------------
// What the caller knows when it asks. Every field defaults to the value that
// produces a Stop, so a half-populated state cannot manufacture a retry.
// ---------------------------------------------------------------------------
struct CancelAttemptState {
    /// Attempts COMPLETED so far, including the first. 0 is a programming
    /// error (nothing has been tried, so there is nothing to classify) and is
    /// treated as such below.
    std::uint32_t    attempts_made{0};
    /// Offers still believed live after `attempts_made` attempts. Zero means
    /// we are finished, and that is the only clean stop.
    std::uint32_t    remaining_offers{0};
    /// Wall clock since the FIRST attempt began, milliseconds.
    std::uint32_t    elapsed_ms{0};
    /// classify_take_failure() applied to the last failure text. Defaults to
    /// Other, the short-leash class -- an unclassified failure must not
    /// inherit the sync flap's generous ladder.
    TakeFailureClass last_class{TakeFailureClass::Other};
};

struct CancelRetryPlan {
    CancelRetryVerdict verdict{CancelRetryVerdict::Stop};
    CancelStopReason   stop_reason{CancelStopReason::Unknown};
    /// Milliseconds to sleep before the next attempt. 0 when stopping.
    std::uint32_t      delay_ms{0};
    /// 1-based index of the attempt this plan authorises. 0 when stopping.
    std::uint32_t      next_attempt{0};
    /// Budget left AFTER the sleep this plan schedules. Purely for the log
    /// line -- but a real one: the operator watching a stop wants to know how
    /// much longer it intends to sit there.
    std::uint32_t      budget_left_ms{0};
};

// ---------------------------------------------------------------------------
// plan_cancel_retry -- the whole policy, as one total function.
//
// Order is load-bearing:
//   1. Nothing outstanding -> Done. Checked FIRST so a finished cancel can
//      never be reported through a failure enumerator.
//   2. Funding -> NeedsEmergencyLadder immediately. Waiting cannot refill a
//      wallet, and the caller has a zero-fee secure ladder for exactly this.
//   3. Attempt ceiling (per class).
//   4. Wall-clock budget, INCLUDING the sleep we are about to schedule. A
//      retry that would finish after the cap is not taken; it would be the
//      hang this file exists to prevent.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr CancelRetryPlan
plan_cancel_retry(CancelAttemptState state,
                  const CancelRetryConfig& cfg) noexcept
{
    CancelRetryPlan plan{};

    // (1) The clean exit, and the only one.
    if (state.remaining_offers == 0) {
        plan.stop_reason = CancelStopReason::Done;
        return plan;
    }

    // A caller that has not attempted anything has nothing to retry FROM, and
    // no failure text to classify. Refusing here rather than silently
    // authorising attempt 1 keeps "attempts_made" meaning what it says.
    if (state.attempts_made == 0) {
        plan.stop_reason = CancelStopReason::Unknown;
        return plan;
    }

    // (2) Funding: escalate, do not wait. Named distinctly so the caller can
    // tell "we ran out of road" from "this needs the other mechanism".
    if (state.last_class == TakeFailureClass::Funding) {
        plan.stop_reason = CancelStopReason::NeedsEmergencyLadder;
        return plan;
    }

    // (3) Per-class attempt ceiling. Other gets the short leash.
    const std::uint32_t ceiling =
        (state.last_class == TakeFailureClass::Other)
            ? std::min(cfg.other_max_attempts, cfg.max_attempts)
            : cfg.max_attempts;
    if (state.attempts_made >= ceiling) {
        plan.stop_reason = CancelStopReason::AttemptsExhausted;
        return plan;
    }

    // (4) Wall clock. Guarded subtraction -- an elapsed past the budget is a
    // slow wallet, not a negative remainder.
    if (state.elapsed_ms >= cfg.budget_ms) {
        plan.stop_reason = CancelStopReason::BudgetExhausted;
        return plan;
    }
    const std::uint32_t budget_left = cfg.budget_ms - state.elapsed_ms;

    std::uint32_t delay = cancel_backoff_ms(state.attempts_made, cfg);
    if (delay > budget_left) {
        // Clamp into what is left rather than refusing outright: the last
        // partial window is still a real chance at a flap that is about to
        // clear. But a clamp below min_useful_delay_ms re-asks a wallet that
        // has had no time to change its mind, so that becomes a stop.
        delay = budget_left;
    }
    if (delay < cfg.min_useful_delay_ms) {
        plan.stop_reason = CancelStopReason::BudgetExhausted;
        return plan;
    }

    plan.verdict        = CancelRetryVerdict::Retry;
    plan.stop_reason    = CancelStopReason::Unknown;  // not a stop
    plan.delay_ms       = delay;
    plan.next_attempt   = state.attempts_made + 1;
    plan.budget_left_ms = budget_left - delay;
    return plan;
}

// ---------------------------------------------------------------------------
// cancel_last_attempt_start_ms -- when does the FINAL attempt go out?
//
// Walks the real policy (not a re-derivation of it) for a cancel that never
// succeeds, and returns the elapsed time at which the last attempt starts,
// assuming the RPCs themselves are instantaneous. That assumption is what
// makes it a schedule proof rather than a runtime promise: real RPCs only
// push the last attempt EARLIER in attempt-count terms, never later in wall
// clock, because elapsed_ms is an input to the same policy.
//
// It exists so the header's coverage claim is checked by the compiler. A
// prose claim about which desync episode the ladder clears is exactly the
// kind of thing that silently stops being true when max_attempts is edited.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr std::uint32_t
cancel_last_attempt_start_ms(const CancelRetryConfig& cfg,
                             TakeFailureClass cls) noexcept
{
    std::uint32_t elapsed  = 0;
    std::uint32_t attempts = 0;
    // Bounded independently of the policy so a future edit that makes the
    // policy non-terminating cannot hang a constant evaluation.
    for (std::uint32_t guard = 0; guard < 1'000; ++guard) {
        ++attempts;
        CancelAttemptState st{};
        st.attempts_made    = attempts;
        st.remaining_offers = 1;  // never finishes: the worst case
        st.elapsed_ms       = elapsed;
        st.last_class       = cls;
        const auto plan = plan_cancel_retry(st, cfg);
        if (plan.verdict != CancelRetryVerdict::Retry) return elapsed;
        elapsed += plan.delay_ms;
    }
    return elapsed;
}

static_assert(cancel_last_attempt_start_ms(CancelRetryConfig{},
                                           TakeFailureClass::Unsynced)
                  == 90'000,
              "the last Unsynced attempt must go out at the budget");
static_assert(cancel_last_attempt_start_ms(CancelRetryConfig{},
                                           TakeFailureClass::Unsynced)
                  >= kLongestMeasuredDesyncMs,
              "the ladder must still be trying when the longest desync "
              "episode this repo has actually measured would have cleared. "
              "This is the claim the 90 s budget was chosen for");
static_assert(cancel_last_attempt_start_ms(CancelRetryConfig{},
                                           TakeFailureClass::Unsynced)
                  < kWorstObservedDesyncMs,
              "and it must NOT claim to cover the worst episode ever seen. "
              "If this ever becomes false the header prose is lying in the "
              "other direction");

// ---------------------------------------------------------------------------
// more_retryable -- fold N failure classes into the one that decides.
//
// [review] CancelOutcome used to carry only the LAST failure string, and the
// caller classified that one arbitrary sample. With a heterogeneous batch --
// six offers refused for "not synced" and a seventh short of XCH -- the
// seventh's Funding text won by being last, plan_cancel_retry returned
// NeedsEmergencyLadder, and the ladder was abandoned after ONE attempt for
// six offers whose only problem was the self-clearing flap.
//
// take_retry.hpp already states the precedence for a single message: "the
// sync condition is the one that will clear on its own, and misfiling it as
// Funding is the expensive direction." This is that same precedence applied
// ACROSS messages, which is where it was being lost.
//
//   Unsynced (self-clearing, full ladder)
//     > Other (unmodelled, short leash)
//       > Funding (waiting cannot refill a wallet, escalate now)
//
// Note this deliberately does NOT mean "ignore the funding failure": the
// funding offer's own emergency ladder already ran inside cancel_ids. What
// it means is that one offer's funding problem must not cancel six other
// offers' retries.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr int cancel_class_rank(TakeFailureClass c) noexcept
{
    switch (c) {
        case TakeFailureClass::Unsynced: return 2;
        case TakeFailureClass::Other:    return 1;
        case TakeFailureClass::Funding:  return 0;
    }
    return 1;  // unreachable; Other's rank, the middle leash
}

[[nodiscard]] constexpr TakeFailureClass
more_retryable(TakeFailureClass a, TakeFailureClass b) noexcept
{
    return (cancel_class_rank(a) >= cancel_class_rank(b)) ? a : b;
}

static_assert(more_retryable(TakeFailureClass::Funding,
                             TakeFailureClass::Unsynced)
                  == TakeFailureClass::Unsynced,
              "one funding refusal must not strip the sync flap's ladder "
              "from the rest of the batch");
static_assert(more_retryable(TakeFailureClass::Funding,
                             TakeFailureClass::Other)
                  == TakeFailureClass::Other);
static_assert(more_retryable(TakeFailureClass::Unsynced,
                             TakeFailureClass::Other)
                  == TakeFailureClass::Unsynced);
static_assert(more_retryable(TakeFailureClass::Funding,
                             TakeFailureClass::Funding)
                  == TakeFailureClass::Funding,
              "an all-funding batch must still escalate immediately");

// ---------------------------------------------------------------------------
// CancelAttemptOutcome -- what one attempt against the wallet produced.
//
// Deliberately expressed in plain strings so this header stays pure and the
// gtest can construct one without a wallet, a State, or an Engine.
// ---------------------------------------------------------------------------
struct CancelAttemptOutcome {
    /// Ids the wallet accepted a cancel for. SUBMITTED, not confirmed.
    std::vector<std::string> cancelled;
    /// Ids we still believe are LIVE. These are what the next attempt retries.
    std::vector<std::string> failed;
    /// Ids skipped because a cancel spend is ALREADY in flight for them.
    /// Neither cancelled by this attempt nor retryable -- charging them again
    /// pays a second fee for one spend.
    std::vector<std::string> already_pending;
    /// Verbatim last failure text, for the operator-facing log only.
    std::string              last_error;
    /// The most-retryable class seen across ALL failures in this attempt.
    TakeFailureClass         worst_class{TakeFailureClass::Other};
    /// Set when `cancelled` came from the wallet-wide bulk endpoint.
    bool                     bulk_submitted{false};
};

// ---------------------------------------------------------------------------
// What the driver should do next.
// ---------------------------------------------------------------------------
enum class CancelLadderStep : int {
    /// ZERO. A ladder nobody has stepped is finished, never "go attempt
    /// something": a default-constructed action must not authorise an RPC.
    Finish  = 0,
    Attempt = 1,
    Sleep   = 2,
};

struct CancelLadderAction {
    CancelLadderStep step{CancelLadderStep::Finish};
    /// 1-based index of the attempt this authorises (Attempt only).
    std::uint32_t    attempt_index{0};
    /// Milliseconds to sleep (Sleep only).
    std::uint32_t    delay_ms{0};
    /// Budget remaining after that sleep, for the log line (Sleep only).
    std::uint32_t    budget_left_ms{0};
};

// ---------------------------------------------------------------------------
// CancelLadder -- the shutdown retry loop's STATE, out of engine.cpp.
//
// [review] This class exists because of a measured hole, not a style
// preference. Reinstating the original S46 fail-open at the engine call site
// -- replacing `outstanding = oc.failed;` with `outstanding.clear();` --
// left the entire 1246-test suite green. The policy header was tested; the
// four lines of plumbing that DECIDE WHAT IS STILL LIVE were not, and those
// four lines are the whole bug that was being fixed.
//
// S36 says nothing in cpp/tests constructs an Engine, and that stays true.
// The answer is to move the state, not the I/O: everything that decides is
// here and driven by a gtest, and what remains in engine.cpp is a switch
// that co_awaits and hands the result back. A mutation of the surviving
// engine code can only fail to call record() (which cannot terminate) or
// skip the timer (which changes timing, not correctness).
//
// Not a coroutine and not templated on one: the driver co_awaits, then calls
// record(). Keeping the awaiting in the caller is what lets this be a pure,
// synchronously testable object.
// ---------------------------------------------------------------------------
class CancelLadder {
public:
    CancelLadder(std::vector<std::string> ids, CancelRetryConfig cfg) noexcept
        : cfg_(cfg), outstanding_(std::move(ids))
    {
        if (outstanding_.empty()) stop_reason_ = CancelStopReason::Done;
    }

    /// The decision. `elapsed_ms` is wall clock since the FIRST attempt began
    /// and is an INPUT -- this class owns no clock.
    [[nodiscard]] CancelLadderAction next(std::uint32_t elapsed_ms) noexcept
    {
        CancelLadderAction act{};

        if (outstanding_.empty()) {
            stop_reason_ = CancelStopReason::Done;
            return act;  // Finish
        }

        // The first attempt is not a retry and is never planned: there is no
        // failure to classify yet, and plan_cancel_retry correctly refuses to
        // authorise an attempt from attempts_made == 0.
        if (attempts_ == 0) {
            authorised_attempt_ = 1;
            act.step            = CancelLadderStep::Attempt;
            act.attempt_index   = 1;
            return act;
        }

        // A sleep already granted for the next attempt has been served.
        if (authorised_attempt_ > attempts_) {
            act.step          = CancelLadderStep::Attempt;
            act.attempt_index = authorised_attempt_;
            return act;
        }

        CancelAttemptState st{};
        st.attempts_made    = attempts_;
        st.remaining_offers =
            static_cast<std::uint32_t>(outstanding_.size());
        st.elapsed_ms       = elapsed_ms;
        st.last_class       = worst_class_;

        const auto plan = plan_cancel_retry(st, cfg_);
        if (plan.verdict != CancelRetryVerdict::Retry) {
            stop_reason_ = plan.stop_reason;
            return act;  // Finish
        }

        authorised_attempt_  = plan.next_attempt;
        act.step             = CancelLadderStep::Sleep;
        act.delay_ms         = plan.delay_ms;
        act.budget_left_ms   = plan.budget_left_ms;
        act.attempt_index    = plan.next_attempt;
        return act;
    }

    /// Fold one attempt's result in. THE line that matters is
    /// `outstanding_ = oc.failed` -- "what is still live is what the wallet
    /// refused", not "we tried, so we are done".
    void record(CancelAttemptOutcome oc)
    {
        ++attempts_;

        for (auto& id : oc.cancelled) submitted_.push_back(std::move(id));
        for (auto& id : oc.already_pending) {
            // Already spending. Not a success of THIS attempt and not
            // retryable: it is reported so the caller can say so, and it is
            // never re-charged.
            already_pending_.push_back(std::move(id));
        }

        outstanding_ = std::move(oc.failed);

        if (!oc.last_error.empty()) last_error_ = std::move(oc.last_error);
        bulk_submitted_ = bulk_submitted_ || oc.bulk_submitted;

        // Only a failing attempt carries class information. A clean attempt
        // must not reset the leash the previous failures earned.
        if (!outstanding_.empty()) worst_class_ = oc.worst_class;
        else stop_reason_ = CancelStopReason::Done;
    }

    [[nodiscard]] const std::vector<std::string>& outstanding() const noexcept
    { return outstanding_; }
    /// Ids whose cancel this ladder SUBMITTED. Not proof they are gone.
    [[nodiscard]] const std::vector<std::string>& submitted() const noexcept
    { return submitted_; }
    /// Ids skipped because a cancel was already in flight for them.
    [[nodiscard]] const std::vector<std::string>&
    already_pending() const noexcept { return already_pending_; }
    [[nodiscard]] std::uint32_t attempts() const noexcept { return attempts_; }
    [[nodiscard]] CancelStopReason stop_reason() const noexcept
    { return stop_reason_; }
    [[nodiscard]] const std::string& last_error() const noexcept
    { return last_error_; }
    [[nodiscard]] bool bulk_submitted() const noexcept
    { return bulk_submitted_; }
    [[nodiscard]] TakeFailureClass worst_class() const noexcept
    { return worst_class_; }
    /// True when this ladder finished with nothing believed live. The ONLY
    /// clean outcome, and the caller alerts on anything else.
    [[nodiscard]] bool clean() const noexcept
    {
        return outstanding_.empty()
               && stop_reason_ == CancelStopReason::Done;
    }

private:
    CancelRetryConfig        cfg_{};
    std::vector<std::string> outstanding_{};
    std::vector<std::string> submitted_{};
    std::vector<std::string> already_pending_{};
    std::string              last_error_{};
    std::uint32_t            attempts_{0};
    std::uint32_t            authorised_attempt_{0};
    CancelStopReason         stop_reason_{CancelStopReason::Unknown};
    TakeFailureClass         worst_class_{TakeFailureClass::Other};
    bool                     bulk_submitted_{false};
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_CANCEL_RETRY_HPP
