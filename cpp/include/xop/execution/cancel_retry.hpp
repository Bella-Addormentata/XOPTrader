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
//   a new GUI starting ......... 0 s, or up to 45 s while shutdown.flag or a
//                                closing GUI's stop marker names this engine --
//                                then TerminateProcess of the old GUI AND of us
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
// [BULKCANCEL-B 2026-09-13] The cancel calls this ladder makes are no longer
// re-sent after a timeout or a 5xx (rpc/rpc_retry_policy.hpp), so a hanging
// wallet now costs one request_timeout, ~30 s, per call rather than up to
// four.  The wall-clock bound still decides: a per-id attempt makes one call
// per offer, so an attempt is still not a unit of time.
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
#include "xop/execution/terminal_recheck.hpp"

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
    /// [S33 2026-09-12] A wallet-wide sweep was REFUSED. Nothing is
    /// outstanding only because the refused sweep names no ids -- the
    /// wallet's book is UNKNOWN, never proven empty. NOT a clean stop.
    SweepRefused         = 5,
    /// [review 2026-09-13, round 2] A wallet-wide sweep got NO ANSWER -- a
    /// timeout, an empty or broken reply, an HTTP 5xx -- and may have run,
    /// may still be running, or may never have arrived. Nothing is
    /// outstanding only because no TRACKED offer is still believed live: the
    /// wallet's untracked book is UNKNOWN. NOT a clean stop, and not a
    /// refusal either -- a refusal is an answer.
    SweepPossiblySubmitted = 6,
};

[[nodiscard]] constexpr const char* to_string(CancelStopReason r) noexcept
{
    switch (r) {
        case CancelStopReason::Done:                 return "done";
        case CancelStopReason::AttemptsExhausted:    return "attempts-exhausted";
        case CancelStopReason::BudgetExhausted:      return "budget-exhausted";
        case CancelStopReason::NeedsEmergencyLadder: return "needs-emergency-ladder";
        case CancelStopReason::SweepRefused:         return "wallet-wide-sweep-refused";
        case CancelStopReason::SweepPossiblySubmitted:
            return "wallet-wide-sweep-possibly-submitted";
        case CancelStopReason::Unknown:              break;
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* to_string(CancelRetryVerdict v) noexcept
{
    return v == CancelRetryVerdict::Retry ? "retry" : "stop";
}

// ---------------------------------------------------------------------------
// [review 2026-09-13, round 2] THE WAIT AFTER A SWEEP THAT GOT NO ANSWER.
//
// cancel_all's wallet-wide cancel_offers can fail AFTER the wallet received
// it -- a timeout, an empty or broken reply, an HTTP 5xx
// (rpc::cancel_possibly_submitted) -- and the wallet may still be running it.
// On 2026-09-12 the failure reached the engine 30.7 s after the send, while
// the first request was still writing cancel records at 53 s.  In chia 2.7.4
// cancel_offers and cancel_offer both take the wallet state lock, and
// cancel_pending_offers does not check trade status, so a per-id retry queued
// behind the running sweep builds a SECOND, conflicting spend of an offer the
// sweep already cancelled.
//
// So the retry after such an attempt waits at least one request timeout of
// the wallet client plus kPossiblySubmittedWaitMarginMs, and then re-checks
// every offer before it cancels any (CancelLadder::needs_recheck).  On the
// 2026-09-12 timings a 35 s wait re-checks at ~66 s, about 13 s after the
// last record the first request wrote.
// ---------------------------------------------------------------------------

/// Slack on top of the wallet client's request timeout, for a request the
/// wallet was still running when curl gave up on it.
inline constexpr std::uint32_t kPossiblySubmittedWaitMarginMs = 5'000;

/// rpc::ChiaRPCConfig::request_timeout's default (chia_rpc.hpp), repeated only
/// so that CancelRetryConfig{} is never built with a wait shorter than one
/// timeout.  The engine passes its wallet client's real timeout, and
/// test_cancel_retry.cpp pins this copy to the client's default.
inline constexpr std::uint32_t kDefaultWalletRequestTimeoutMs = 30'000;

/// The least sleep after an attempt whose sweep got no answer: one request
/// timeout plus the margin.  A non-positive timeout waits the margin alone,
/// and the result saturates instead of wrapping.
[[nodiscard]] constexpr std::uint32_t
wait_after_possibly_submitted_ms(std::int64_t request_timeout_ms) noexcept
{
    const std::uint64_t timeout = request_timeout_ms > 0
        ? static_cast<std::uint64_t>(request_timeout_ms)
        : std::uint64_t{0};
    // timeout < 2^63, so adding the margin cannot wrap.
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        timeout + kPossiblySubmittedWaitMarginMs, 0xFFFF'FFFFull));
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
    /// [review 2026-09-13, round 2] The least sleep before the attempt that
    /// follows a wallet-wide sweep that got NO ANSWER (see the note above).
    /// Clamped by the budget like any delay, except that a budget unable to
    /// fit it plus min_useful_delay_ms stops the ladder instead of retrying
    /// early.  The engine sets it from its wallet client.
    std::uint32_t possibly_submitted_wait_ms{
        wait_after_possibly_submitted_ms(kDefaultWalletRequestTimeoutMs)};
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
static_assert(CancelRetryConfig{}.budget_ms
                  >= kDefaultWalletRequestTimeoutMs
                         + CancelRetryConfig{}.possibly_submitted_wait_ms
                         + CancelRetryConfig{}.min_useful_delay_ms,
              "a sweep that fails only after one full request timeout must "
              "still leave the budget room for the wait and a re-check, or "
              "every unanswered sweep at shutdown stops after attempt 1");

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
    /// [review 2026-09-13, round 2] The last attempt's wallet-wide sweep got
    /// NO ANSWER and may still be running, so the next sleep is at least
    /// cfg.possibly_submitted_wait_ms.  False keeps today's schedule;
    /// CancelLadder sets it from the attempt it recorded.
    bool             possibly_submitted{false};
    /// [review 2026-09-13, round 4] The part of that wait still owed, when
    /// the attempt was seeded from a sweep whose wait had already partly run
    /// (seeded_unanswered_sweep_outcome).  0 means the whole
    /// cfg.possibly_submitted_wait_ms.
    std::uint32_t    possibly_submitted_wait_ms{0};
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
    // (4a) [review 2026-09-13, round 2] After a sweep that got NO ANSWER the
    // next attempt must not start while that sweep may still be running, so
    // it sleeps at least the configured wait.  A budget that cannot fit the
    // wait and a useful moment after it stops the ladder HERE rather than
    // clamping the wait into the tail: a clamped wait is exactly the early
    // per-id re-cancel this exists to prevent.  BudgetExhausted is literally
    // why, and every consumer already treats it as unclean and hands the
    // still-live ids, by name, to the S31 fallback.
    if (state.possibly_submitted) {
        // [review 2026-09-13, round 4] Only the rest of the wait, when the
        // ladder was seeded from a sweep whose wait had already partly run.
        // The sleep is still never shorter than today's backoff.
        const std::uint32_t wait = state.possibly_submitted_wait_ms != 0
            ? state.possibly_submitted_wait_ms
            : cfg.possibly_submitted_wait_ms;
        const std::uint64_t needed =
            std::uint64_t{wait} + cfg.min_useful_delay_ms;
        if (budget_left < needed) {
            plan.stop_reason = CancelStopReason::BudgetExhausted;
            return plan;
        }
        delay = std::max(delay, wait);
    }
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
    /// [S33 2026-09-12] Set when a WALLET-WIDE sweep was attempted and
    /// REFUSED, and no id in `failed` represents that refusal.
    ///
    /// THE SHAPE THIS CLOSES.  cancel_all sends cancel_all:true, which sweeps
    /// every pending offer IN THE WALLET -- including a book left resting by a
    /// previous instance that this process never tracked.  When that sweep is
    /// refused and the local book is empty there are no ids to put in
    /// `failed`, because the bulk endpoint takes no offer id at all.  An empty
    /// `failed` set outstanding_ empty and stop_reason Done, so clean()
    /// returned TRUE for a sweep the wallet REFUSED: a refusal reading as
    /// success, which is the exact fail-open this family keeps removing.
    ///
    /// A sentinel id in `failed` was the alternative, and it is worse: the
    /// retry leg is cancel_ids (which looks ids up in State) and the shutdown
    /// alert names still-live ids to an operator, so both would end up
    /// handling an offer id that does not exist.  A flag cannot be mistaken
    /// for an offer.
    bool                     sweep_refused{false};
    /// [review 2026-09-13, round 2] Set when the attempt's WALLET-WIDE sweep
    /// failed AFTER it may have reached the wallet (a timeout, an empty or
    /// broken reply, an HTTP 5xx) and nothing was sent after it: every
    /// tracked id is in `failed`, none of them cancelled again.  The ladder
    /// then waits at least cfg.possibly_submitted_wait_ms and re-checks every
    /// offer before any retry (needs_recheck).  Unlike sweep_refused this is
    /// NOT an answer -- sending again at once is the duplicate -- but it
    /// proves the untracked book empty no more than a refusal does.
    bool                     bulk_possibly_submitted{false};
    /// [review 2026-09-13, round 4] With bulk_possibly_submitted: the part of
    /// the wait still owed, when that wait had already partly run before this
    /// ladder began.  0 means the whole cfg.possibly_submitted_wait_ms.  Set
    /// only by seeded_unanswered_sweep_outcome.
    std::uint32_t            possibly_submitted_wait_ms{0};
};

// ---------------------------------------------------------------------------
// [review 2026-09-13, round 2] RecheckPartition -- what the wallet says about
// each offer before a retry that follows a sweep that got no answer.
//
// One bucket per answer, decided by ONE pure function that both callers of
// cancel_all use (the shutdown ladder and operator Cancel All), with the
// posture Engine::sweep_cancel_intent already has:
//
//   Revived, no cancel spend in flight  -> recancel         still live
//   Revived, cancel_pending in State    -> already_pending  the wallet is
//        already cancelling it (PENDING_CANCEL); a second secure cancel
//        builds a second spend and pays a second fee
//   StillTerminal                       -> dead             cancelled/failed
//   Confirmed                           -> filled           a taker won the
//        race: NEVER cancelled again and NEVER stamped cancelled
//   NoVerdict                           -> unknown          no evidence either
//        way: not cancelled on this attempt, still outstanding
//
// `cancel_pending` is read from State AFTER recheck_terminal, which marks it
// for a PENDING_CANCEL record -- that is how the two Revived rows are told
// apart without widening TerminalRecheck.
//
// [review 2026-09-13, round 4] `pending_before_sweep`: State already had a
// cancel in flight for this offer BEFORE the sweep -- a TTL or rebalance
// cancel the engine sent itself.  The offer still lands in already_pending or
// dead, and is still never cancelled again, but it is no evidence that the
// sweep reached the book, so it does not count in `sweep_evidence`.
// ---------------------------------------------------------------------------
struct RecheckPartition {
    std::vector<std::string> recancel{};
    std::vector<std::string> already_pending{};
    std::vector<std::string> dead{};
    std::vector<std::string> filled{};
    std::vector<std::string> unknown{};
    /// [review 2026-09-13, round 4] Offers in already_pending or dead that
    /// were NOT already being cancelled before the sweep: the only ones that
    /// show the sweep reached the book (recheck_saw_the_sweep).
    std::uint32_t            sweep_evidence{0};
};

inline void partition_rechecked_offer(RecheckPartition& into,
                                      std::string       id,
                                      TerminalRecheck   verdict,
                                      bool              cancel_pending,
                                      bool              pending_before_sweep = false)
{
    switch (verdict) {
        case TerminalRecheck::StillTerminal:
            into.dead.push_back(std::move(id));
            if (!pending_before_sweep) ++into.sweep_evidence;
            return;
        case TerminalRecheck::Confirmed:
            into.filled.push_back(std::move(id));
            return;
        case TerminalRecheck::Revived:
            if (cancel_pending) {
                into.already_pending.push_back(std::move(id));
                if (!pending_before_sweep) ++into.sweep_evidence;
            } else {
                into.recancel.push_back(std::move(id));
            }
            return;
        case TerminalRecheck::NoVerdict:
            break;
    }
    // NoVerdict -- and any value this build does not name: keep, do not act.
    into.unknown.push_back(std::move(id));
}

// ---------------------------------------------------------------------------
// [review 2026-09-13, round 3] IS THE SWEEP STILL RUNNING?
//
// The fixed wait (one request timeout plus 5 s) rests on ONE measured sweep:
// 22 offers, still writing records 53 s after the send.  A book near the
// 50-offer batch can take longer.  In chia 2.7.4 cancel_pending_offers sets
// PENDING_CANCEL trade by trade inside the batch (trade_manager.py) while
// get_offer takes no lock (wallet_rpc_api.py), so a re-check that lands
// mid-sweep reads every offer the sweep has not reached yet as still live --
// and a per-id cancel of those queues behind the sweep's lock and builds the
// duplicate spend.
//
// The re-check can see it.  When it finds an offer the sweep already reached
// -- cancelling (PENDING_CANCEL) or cancelled -- AND an offer still live with
// no cancel in flight, the sweep is proven to have run and is taken to be
// still working through the book.  The live ids are NOT re-cancelled then:
//   * the shutdown ladder holds them back like a NoVerdict id: they stay
//     outstanding, and another re-check follows only if the budget fits it
//     (CancelLadder::record_recheck);
//   * operator Cancel All pauses and re-checks again, and at its deadline
//     re-cancels only the ids still live (possibly_submitted_rewait_ms).
//
// THE EVIDENCE IS CUMULATIVE.  A later re-check covers only the ids still
// undecided, so the offers that proved the sweep ran are no longer in it.
// Without carrying that evidence forward, a re-check of the still-live ids
// alone would read as "the sweep never ran" and re-cancel them mid-sweep.
//
// CONSERVATIVE, BUT NOT ON THE ENGINE'S OWN CANCELS.  An offer found cancelled
// or cancelling counts even when something other than the sweep did it, so
// the rule can hold ids back when the sweep never ran.  That costs time, and
// it moves the cancel rather than removing it: at shutdown the held ids go to
// the S31 fallback by name, and on the operator path they are re-cancelled at
// its deadline.  [review 2026-09-13, round 4] What does NOT count is an offer
// State already had a cancel in flight for BEFORE the sweep
// (partition_rechecked_offer's pending_before_sweep): a TTL or rebalance
// cancel the engine sent itself would otherwise hold every live offer back
// after a sweep that never arrived.  A FILLED offer is not evidence either: a
// taker, not the sweep, resolved it.
//
// NOT SEEN, AND DISCLOSED: a sweep still queued behind the wallet lock has
// reached no tracked offer yet, so a re-check then finds no evidence and the
// live offers are cancelled while that sweep may still run.
// ---------------------------------------------------------------------------

/// Did this re-check find an offer the sweep had already reached?
[[nodiscard]] inline bool recheck_saw_the_sweep(
    const RecheckPartition& p) noexcept
{
    return p.sweep_evidence != 0;
}

/// Does this re-check show the sweep still working through the book?  True
/// when offers are still live with no cancel in flight (`recancel`) while the
/// sweep is known to have reached others: in this re-check, or in an earlier
/// re-check of the same book (`seen_before`).
[[nodiscard]] inline bool recheck_shows_sweep_running(
    const RecheckPartition& p, bool seen_before) noexcept
{
    return !p.recancel.empty() && (seen_before || recheck_saw_the_sweep(p));
}

// ---------------------------------------------------------------------------
// [review 2026-09-13, round 3] OPERATOR CANCEL ALL: A DEADLINE OF ITS OWN.
//
// The shutdown ladder is bounded by budget_ms.  Operator Cancel All has no
// ladder: after a sweep that got no answer it waits, re-checks and re-cancels
// while cancel_all_inflight_ holds the other cancel paths -- and a shutdown --
// behind it, and in round 2 nothing bounded that.  So it gets a deadline: the
// wait plus the same budget_ms (90 s) the shutdown ladder gives its whole
// retry sequence.  That leaves room for the re-checks a still-running sweep
// needs and for the final re-cancel.  Probes and re-cancels stop at the
// deadline; only an RPC already in flight can run past it.
// ---------------------------------------------------------------------------

/// Pause between re-checks while a re-check shows the sweep still running.
/// The 2026-09-12 sweep wrote its cancel records in bursts about 6 s apart
/// (40, 46 and 52 s after the send), so a 10 s pause spans at least one.
inline constexpr std::uint32_t kPossiblySubmittedRecheckIntervalMs = 10'000;

/// The operator path's deadline, in milliseconds after cancel_all returned:
/// the wait, then the retry budget.
[[nodiscard]] constexpr std::uint64_t possibly_submitted_deadline_ms(
    std::uint32_t wait_ms, const CancelRetryConfig& cfg) noexcept
{
    return std::uint64_t{wait_ms} + cfg.budget_ms;
}

/// After a re-check on the operator path: pause this long and re-check
/// again, or 0 to act now -- re-cancel the ids still live.  It pauses only
/// while the re-check shows the sweep still running, and only while one more
/// pause AND `recancel_reserve_ms` for the final re-cancel still fit before
/// the deadline.  `elapsed_ms` and `deadline_ms` count from when cancel_all
/// returned.
[[nodiscard]] constexpr std::uint32_t possibly_submitted_rewait_ms(
    bool          sweep_running,
    std::uint64_t elapsed_ms,
    std::uint64_t deadline_ms,
    std::uint64_t recancel_reserve_ms) noexcept
{
    if (!sweep_running) return 0;
    if (elapsed_ms >= deadline_ms) return 0;
    const std::uint64_t needed =
        std::uint64_t{kPossiblySubmittedRecheckIntervalMs} + recancel_reserve_ms;
    if (needed > deadline_ms - elapsed_ms) return 0;
    return kPossiblySubmittedRecheckIntervalMs;
}

// ---------------------------------------------------------------------------
// [review 2026-09-13, round 4] NO SECOND WALLET-WIDE SWEEP WITHIN ONE WAIT OF
// AN UNANSWERED ONE.
//
// A shutdown ends operator Cancel All's wait at once (engine.cpp), and the
// shutdown ladder's attempt 1 is a wallet-wide cancel_all.  Sent while the
// operator's sweep may still be running, it queues behind that sweep in the
// wallet and, in chia 2.7.4, cancels again every trade the first sweep marked
// PENDING_CANCEL: the duplicate this header exists to prevent.
//
// So a shutdown that starts while operator Cancel All's no-answer branch still
// runs records THAT sweep as its attempt 1 (seeded_unanswered_sweep_outcome)
// instead of sending another: every tracked id stays outstanding, the ladder
// sleeps only the rest of the wait, and it re-checks each offer before
// attempt 2 cancels any.  A budget that cannot fit even that stops the ladder
// BudgetExhausted, and the S31 fallback runs as it does today.
//
// [review 2026-09-13, round 5] "While the branch still runs" means until its
// deadline (possibly_submitted_deadline_ms), not one wait: past the wait the
// branch keeps re-checking and pausing while its evidence says the sweep is
// still running.  The engine clears its stamp when the branch ends any way
// other than a shutdown request, so a finished branch seeds nothing later.  And
// a seeded ladder with no tracked offer to re-check stops at once, so it owes
// the rest of the wait before the S31 fallback sends its own wallet-wide cancel
// (CancelLadder::wait_owed_before_fallback).
// ---------------------------------------------------------------------------

/// The part of the wait a shutdown still owes an operator sweep that got no
/// usable answer `since_ms` ago.  0 when there was no such sweep (`seen`
/// false) or its branch is past its deadline: attempt 1 then goes out as
/// usual.  Otherwise the rest of the wait, never less than
/// cfg.min_useful_delay_ms -- nor less than 1, so a seed is never read as 0.
[[nodiscard]] constexpr std::uint32_t unanswered_sweep_remaining_wait_ms(
    bool seen, std::uint64_t since_ms, const CancelRetryConfig& cfg) noexcept
{
    if (!seen) return 0;
    if (since_ms >= possibly_submitted_deadline_ms(cfg.possibly_submitted_wait_ms, cfg)) return 0;
    const std::uint32_t least =
        cfg.min_useful_delay_ms != 0 ? cfg.min_useful_delay_ms : std::uint32_t{1};
    if (since_ms >= cfg.possibly_submitted_wait_ms) return least;
    const auto rest = static_cast<std::uint32_t>(
        cfg.possibly_submitted_wait_ms - since_ms);
    return rest > least ? rest : least;
}

/// The attempt a seeded shutdown ladder records in place of attempt 1: the
/// operator's unanswered sweep, with every tracked id still outstanding and
/// `remaining_wait_ms` (unanswered_sweep_remaining_wait_ms) still owed.
[[nodiscard]] inline CancelAttemptOutcome seeded_unanswered_sweep_outcome(
    std::vector<std::string> outstanding, std::uint32_t remaining_wait_ms)
{
    CancelAttemptOutcome oc{};
    oc.failed                     = std::move(outstanding);
    oc.last_error                 =
        "operator Cancel All's wallet-wide sweep got no usable answer";
    oc.worst_class                = classify_take_failure(oc.last_error);
    oc.bulk_possibly_submitted    = true;
    oc.possibly_submitted_wait_ms = remaining_wait_ms;
    return oc;
}

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
    /// [S33 2026-09-12] `sweep_when_empty` authorises attempt 1 even when
    /// `ids` is EMPTY. Attempt 1 is the WALLET-WIDE sweep (the driver routes
    /// attempt_index == 1 to cancel_all and every retry to cancel_ids), and a
    /// book a previous instance left resting is in the wallet and in nobody's
    /// id list: LOCAL EMPTINESS IS NOT WALLET EMPTINESS. Defaulted OFF, so a
    /// ladder that can only ever act on ids keeps "no book is the one
    /// genuinely clean start".
    CancelLadder(std::vector<std::string> ids, CancelRetryConfig cfg,
                 bool sweep_when_empty = false) noexcept
        : cfg_(cfg), outstanding_(std::move(ids)),
          sweep_when_empty_(sweep_when_empty)
    {
        // An empty book is Done only when nothing is going to be asked of the
        // wallet. With the sweep pending, nothing is proven yet.
        if (outstanding_.empty() && !sweep_when_empty_) {
            stop_reason_ = CancelStopReason::Done;
        }
    }

    /// The decision. `elapsed_ms` is wall clock since the FIRST attempt began
    /// and is an INPUT -- this class owns no clock.
    [[nodiscard]] CancelLadderAction next(std::uint32_t elapsed_ms) noexcept
    {
        CancelLadderAction act{};

        // [S33 2026-09-12] An empty id list finishes the ladder EXCEPT on the
        // very first step of a sweeping ladder. That one attempt is the
        // wallet-wide cancel_all, the only thing that can reach offers the
        // wallet knows about and we do not; without this exception a stop with
        // an empty State issued ZERO cancel RPCs and still logged "All
        // outstanding offers cancelled (0 attempt(s), 0 ms)". Once it has been
        // recorded (attempts_ > 0) the gate closes again: there is nothing to
        // retry per-id, so there is no second sweep and no ladder.
        if (outstanding_.empty()
            && !(sweep_when_empty_ && attempts_ == 0)) {
            // [S33 2026-09-12] Empty is Done ONLY when nothing was refused
            // wallet-wide. Without this guard next() overwrote the
            // SweepRefused that record() had just set -- the driver calls
            // next() once more to learn it should Finish -- handing the
            // fail-open straight back one line after it was closed.
            // [review 2026-09-13, round 2] Nor after a sweep that got no
            // answer.
            if (!sweep_refused_ && !possibly_submitted_) {
                stop_reason_ = CancelStopReason::Done;
            }
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
        // [review 2026-09-13, round 2] The wait follows the attempt whose
        // sweep got no answer, not every later retry.
        st.possibly_submitted = last_attempt_possibly_submitted_;
        // [review 2026-09-13, round 4] Owed only after a seeded attempt.
        st.possibly_submitted_wait_ms = last_attempt_wait_ms_;

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

        const bool attempt_failed = !oc.failed.empty();
        outstanding_ = std::move(oc.failed);
        // [review 2026-09-13, round 2] Ids the re-check got no verdict for
        // were held back from this attempt's cancel. As far as this ladder
        // knows they are still live.
        for (auto& id : held_back_) outstanding_.push_back(std::move(id));
        held_back_.clear();

        if (!oc.last_error.empty()) last_error_ = std::move(oc.last_error);
        bulk_submitted_ = bulk_submitted_ || oc.bulk_submitted;
        // [S33 2026-09-12] STICKY for the life of the ladder. A refused
        // wallet-wide sweep cannot be undone by a later per-id attempt:
        // cancel_ids only ever names ids this process tracks, so it can never
        // reach the untracked book the sweep was refused over.
        sweep_refused_ = sweep_refused_ || oc.sweep_refused;
        // [review 2026-09-13, round 2] A sweep that got NO ANSWER. The wait
        // applies to the next sleep only; the other two flags are STICKY: no
        // later per-id success proves the untracked book swept, and no retry
        // on this ladder cancels an id without asking the wallet first.
        last_attempt_possibly_submitted_ = oc.bulk_possibly_submitted;
        // [review 2026-09-13, round 4] The rest of a wait already partly run,
        // for the next sleep only.
        last_attempt_wait_ms_ =
            oc.bulk_possibly_submitted ? oc.possibly_submitted_wait_ms : 0;
        // [review 2026-09-13, round 5] A seed recorded as attempt 1.
        if (attempts_ == 1 && oc.bulk_possibly_submitted) {
            seeded_wait_ms_ = oc.possibly_submitted_wait_ms;
        }
        possibly_submitted_ = possibly_submitted_ || oc.bulk_possibly_submitted;
        needs_recheck_      = needs_recheck_ || oc.bulk_possibly_submitted;

        // Only a failing attempt carries class information. A clean attempt
        // must not reset the leash the previous failures earned -- nor does
        // one whose only outstanding ids are the held-back ones.
        if (!outstanding_.empty()) {
            if (attempt_failed) worst_class_ = oc.worst_class;
        } else if (sweep_refused_) {
            // [S33 2026-09-12] An EMPTY `failed` is not automatically Done.
            // A refused wallet-wide sweep has nothing to put in `failed` and
            // has proved nothing dead either.
            stop_reason_ = CancelStopReason::SweepRefused;
        } else if (possibly_submitted_) {
            // [review 2026-09-13, round 2] Nor after a sweep that got no
            // answer: no tracked offer is believed live, and the untracked
            // book is unproven.
            stop_reason_ = CancelStopReason::SweepPossiblySubmitted;
        } else {
            stop_reason_ = CancelStopReason::Done;
        }
    }

    /// [review 2026-09-13, round 2] Fold in the re-check the driver ran over
    /// outstanding() for a retry that follows a sweep that got no answer
    /// (needs_recheck()) -- BEFORE it hands outstanding() to cancel_ids.
    ///
    /// Afterwards outstanding() names ONLY the offers the wallet reports live
    /// with no cancel spend in flight: the ids that attempt may cancel.
    ///   already_pending  a cancel spend is in flight: reported, never charged
    ///                    again;
    ///   dead, filled     resolved by the wallet: they leave the ladder, and
    ///                    the caller handles them as the intent sweep does --
    ///                    a filled offer is never stamped cancelled;
    ///   unknown          no verdict: NOT cancelled on this attempt, and put
    ///                    back into outstanding() by the next record().
    ///
    /// [review 2026-09-13, round 3] And when the re-check shows the sweep
    /// still working through the book (recheck_shows_sweep_running, with the
    /// evidence kept across every re-check of this ladder), the live ids are
    /// held back exactly like the unknown ones: outstanding() is left EMPTY,
    /// so cancel_ids sends nothing on this attempt.
    void record_recheck(RecheckPartition part)
    {
        // [review 2026-09-13, round 3] Decided on the wallet's answers as they
        // came back, before any bucket moves.
        const bool sweep_running =
            recheck_shows_sweep_running(part, sweep_seen_running_);
        sweep_seen_running_ = sweep_seen_running_ || recheck_saw_the_sweep(part);

        for (auto& id : part.already_pending) {
            already_pending_.push_back(std::move(id));
        }
        for (auto& id : part.dead) resolved_dead_.push_back(std::move(id));
        for (auto& id : part.filled) resolved_filled_.push_back(std::move(id));
        if (sweep_running) {
            // [review 2026-09-13, round 3] The next record() returns them to
            // outstanding(), and next() re-checks them only if the budget and
            // the attempt ceiling allow another attempt.
            for (auto& id : part.recancel) held_back_.push_back(std::move(id));
            outstanding_.clear();
        } else {
            outstanding_ = std::move(part.recancel);
        }
        for (auto& id : part.unknown) held_back_.push_back(std::move(id));
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
    /// [S33 2026-09-12] True when a wallet-wide sweep was REFUSED during this
    /// ladder. The wallet's book is UNKNOWN, never proven empty.
    [[nodiscard]] bool sweep_refused() const noexcept
    { return sweep_refused_; }
    /// [review 2026-09-13, round 2] True once a wallet-wide sweep got NO
    /// ANSWER during this ladder. Sticky: the untracked book is unproven.
    [[nodiscard]] bool possibly_submitted() const noexcept
    { return possibly_submitted_; }
    /// [review 2026-09-13, round 2] True when the attempt about to run must
    /// re-check every outstanding offer, and fold the answers in with
    /// record_recheck(), before it cancels any. Sticky once a sweep got no
    /// answer.
    [[nodiscard]] bool needs_recheck() const noexcept
    { return needs_recheck_; }
    /// [review 2026-09-13, round 2] Offers a re-check found CANCELLED or
    /// FAILED in the wallet.
    [[nodiscard]] const std::vector<std::string>&
    resolved_dead() const noexcept { return resolved_dead_; }
    /// [review 2026-09-13, round 2] Offers a re-check found CONFIRMED: they
    /// FILLED. Never stamp them cancelled.
    [[nodiscard]] const std::vector<std::string>&
    resolved_filled() const noexcept { return resolved_filled_; }
    /// [review 2026-09-13, round 3] True once a re-check has found an offer
    /// the sweep had already reached (cancelling or cancelled). Sticky.
    [[nodiscard]] bool sweep_seen_running() const noexcept
    { return sweep_seen_running_; }
    /// [review 2026-09-13, round 5] The rest of a seeded wait this ladder never
    /// slept, owed before the S31 fallback: non-zero only when the seed was its
    /// only attempt and it stopped at once for want of a tracked offer to
    /// re-check (SweepPossiblySubmitted).  Every other stop owes nothing.
    [[nodiscard]] std::uint32_t wait_owed_before_fallback() const noexcept
    {
        return attempts_ == 1 && stop_reason_ == CancelStopReason::SweepPossiblySubmitted
            ? seeded_wait_ms_
            : 0;
    }
    [[nodiscard]] TakeFailureClass worst_class() const noexcept
    { return worst_class_; }
    /// True when this ladder finished with nothing believed live. The ONLY
    /// clean outcome, and the caller alerts on anything else.
    [[nodiscard]] bool clean() const noexcept
    {
        return outstanding_.empty()
               // [S33 2026-09-12] Stated independently of stop_reason_ on
               // purpose: "the wallet refused to sweep its book" must fail
               // this predicate on its own evidence, not via a second field
               // that some later edit could set back to Done.
               && !sweep_refused_
               // [review 2026-09-13, round 2] Nor a sweep that got no
               // answer, on its own evidence too.
               && !possibly_submitted_
               && stop_reason_ == CancelStopReason::Done;
    }

private:
    CancelRetryConfig        cfg_{};
    std::vector<std::string> outstanding_{};
    std::vector<std::string> submitted_{};
    std::vector<std::string> already_pending_{};
    std::string              last_error_{};
    std::uint32_t            attempts_{0};
    /// [S33 2026-09-12] Sticky: a wallet-wide sweep was refused.
    bool                     sweep_refused_{false};
    std::uint32_t            authorised_attempt_{0};
    CancelStopReason         stop_reason_{CancelStopReason::Unknown};
    TakeFailureClass         worst_class_{TakeFailureClass::Other};
    bool                     bulk_submitted_{false};
    /// [S33 2026-09-12] Authorises attempt 1 -- the wallet-wide sweep -- from
    /// an EMPTY id list. Set only by the shutdown driver.
    bool                     sweep_when_empty_{false};
    /// [review 2026-09-13, round 2] Sticky: a wallet-wide sweep got no answer.
    bool                     possibly_submitted_{false};
    /// [review 2026-09-13, round 2] The LAST recorded attempt's sweep got no
    /// answer, so the next sleep is at least cfg_.possibly_submitted_wait_ms.
    bool                     last_attempt_possibly_submitted_{false};
    /// [review 2026-09-13, round 4] The part of that wait still owed, when the
    /// attempt was seeded; 0 means the whole configured wait.
    std::uint32_t            last_attempt_wait_ms_{0};
    /// [review 2026-09-13, round 5] The wait a seeded attempt 1 still owed.
    std::uint32_t            seeded_wait_ms_{0};
    /// [review 2026-09-13, round 2] Sticky: re-check before every retry.
    bool                     needs_recheck_{false};
    /// [review 2026-09-13, round 2] No-verdict ids held back from the current
    /// attempt's cancel; record() returns them to outstanding_.
    std::vector<std::string> held_back_{};
    /// [review 2026-09-13, round 2] Ids a re-check found resolved.
    std::vector<std::string> resolved_dead_{};
    std::vector<std::string> resolved_filled_{};
    /// [review 2026-09-13, round 3] Sticky: a re-check has seen the sweep.
    bool                     sweep_seen_running_{false};
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_CANCEL_RETRY_HPP
