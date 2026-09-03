// ---------------------------------------------------------------------------
// [S46 2026-09-02] The shutdown cancel tried ONCE, failed on a transient
// wallet refusal, and left seven offers live on a process that then exited
// cleanly.
//
// WHAT WAS OBSERVED, before any of this existed
// ---------------------------------------------
//   13:10:17 [error]    Failed to cancel offer 0x9a72dfe913: Wallet needs to
//                       be fully synced before making transactions.
//   13:10:17 [info]     cancel_all: 0/7 offers cancelled successfully
//   13:10:17 [critical] [S31] graceful cancellation got 0/7 -- invoking the
//                       independent fallback
//   13:10:17 [critical] [S31] cancel FAILED: Wallet needs to be fully synced
//                       ... Offers are STILL LIVE on a wedged engine
//   13:10:17 [error]    [ALERT:CRITICAL] DEAD MAN'S SWITCH COULD NOT CANCEL
//
// All five lines in one second. The "independent fallback" is not
// independent: it goes through the same wallet RPC and failed for the same
// reason before the clock ticked. The replacement engine logged "Wallet fully
// synced -- proceeding with inventory seed" about three minutes later.
//
// WHAT THESE TESTS COVER
// ----------------------
// Two layers.
//
// (1) The POLICY: given (attempts made, offers left, elapsed ms, failure
//     class), do we try again and when.
//
// (2) The LADDER STATE, execution::CancelLadder.
//
//     [review] Layer 2 exists because layer 1 alone was measurably not
//     enough. This file's first version said so in its own header -- "the
//     call site in Engine::shutdown() remains an unguarded line; this suite
//     stays green if that wiring is deleted" -- and that turned out to be
//     literally true and fatal. Reinstating the original S46 fail-open at
//     the call site, replacing `outstanding = oc.failed;` with
//     `outstanding.clear();`, left all 1246 tests green while the engine
//     logged "All outstanding offers cancelled" over a live book. The
//     tested policy was never consulted, because after one attempt there was
//     nothing left to retry.
//
//     S36 (nothing in cpp/tests constructs an Engine) is still respected.
//     The answer was to move the STATE out of engine.cpp rather than to
//     accept that the deciding lines were untestable: CancelLadder owns
//     what is still live, what was submitted, how many attempts have run
//     and why it stopped, and engine.cpp keeps only the co_awaits.
//
// Mutation-checked. Every test below was re-run with the fix reinstated as
// the bug; the results are recorded beside each case.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "xop/execution/cancel_retry.hpp"

using namespace xop::execution;

namespace {

/// The observed incident, as policy inputs: one attempt made, seven offers
/// still live, no time elapsed, the wallet's verbatim refusal.
CancelAttemptState incident_state()
{
    CancelAttemptState s{};
    s.attempts_made    = 1;
    s.remaining_offers = 7;
    s.elapsed_ms       = 0;
    s.last_class       = classify_take_failure(
        "Wallet needs to be fully synced before making transactions.");
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// The regression itself.
// ---------------------------------------------------------------------------

// MUTATION: return Stop unconditionally after the first attempt (the shipped
// behaviour) -> FAILS here. This is the test that would have caught S46.
TEST(CancelRetry, TheObservedIncidentIsRetriedNotAbandoned)
{
    const CancelRetryConfig cfg{};
    const CancelRetryPlan p = plan_cancel_retry(incident_state(), cfg);

    EXPECT_EQ(p.verdict, CancelRetryVerdict::Retry);
    EXPECT_EQ(p.next_attempt, 2u);
    EXPECT_EQ(p.delay_ms, 5'000u);
}

// The live string must land in Unsynced through the EXISTING classifier. If
// this ever fails, a second classifier has been introduced somewhere, or the
// wallet changed its wording -- either way the retry silently degrades to the
// short Other leash and nobody would notice from the logs.
//
// MUTATION: change the needle in classify_take_failure to "fully-synced"
// -> FAILS here.
TEST(CancelRetry, TheLiveRefusalTextClassifiesAsUnsyncedViaTakeRetry)
{
    EXPECT_EQ(classify_take_failure(
                  "Wallet needs to be fully synced before making transactions."),
              TakeFailureClass::Unsynced);
    // The per-offer failure line from the incident, verbatim.
    EXPECT_EQ(classify_take_failure(
                  "Failed to cancel offer 0x9a72dfe913: Wallet needs to be "
                  "fully synced before making transactions."),
              TakeFailureClass::Unsynced);
}

// ---------------------------------------------------------------------------
// The bound. An engine that will not exit is its own outage.
// ---------------------------------------------------------------------------

// Walk the whole ladder the way the engine does and prove it terminates, that
// the sleeping totals what the schedule says, and that it never exceeds the
// budget. A policy that can loop forever is a worse bug than the one it fixes.
//
// MUTATION: remove the attempts ceiling -> the loop runs past 6 and the
// EXPECT_EQ(attempts, 6u) below trips. MUTATION: remove the budget clamp ->
// total sleep exceeds budget_ms and the final EXPECT_LE trips.
// (These notes said "past 5" and named an EXPECT_LE(attempts, ...) that this
// test does not contain; max_attempts is 6 and the assertion is an EXPECT_EQ.)
TEST(CancelRetry, TheLadderTerminatesAndNeverOutrunsTheBudget)
{
    const CancelRetryConfig cfg{};
    CancelAttemptState s = incident_state();

    std::uint32_t total_sleep = 0;
    std::uint32_t attempts    = 1;
    CancelStopReason why      = CancelStopReason::Unknown;

    for (int guard = 0; guard < 1000; ++guard) {
        const CancelRetryPlan p = plan_cancel_retry(s, cfg);
        if (p.verdict != CancelRetryVerdict::Retry) { why = p.stop_reason; break; }
        total_sleep    += p.delay_ms;
        s.elapsed_ms   += p.delay_ms;   // the wallet answers instantly, as observed
        s.attempts_made = p.next_attempt;
        attempts        = p.next_attempt;
        ASSERT_LE(s.elapsed_ms, cfg.budget_ms) << "budget overrun mid-ladder";
    }

    EXPECT_EQ(attempts, 6u) << "six attempts: the original plus five retries";
    EXPECT_EQ(total_sleep, 85'000u)
        << "5s + 10s + 20s + 40s, then the fifth retry clamped from 40s into "
           "the 15s of budget that is left MINUS the 5s execution reserve";
    EXPECT_LE(total_sleep, cfg.budget_ms) << "the budget bounds the sleeping";
    EXPECT_EQ(why, CancelStopReason::AttemptsExhausted);

    // [F3 2026-09-03] THE ASSERTION THAT USED TO BE TAUTOLOGICAL LIVES BELOW,
    // in TheFinalAttemptIsAdmittedByTheDeadlineItRunsAgainst. What was here
    // read `const std::uint32_t last_attempt_at = total_sleep;` -- TOTAL
    // SLEEPING TIME read as LAST-ATTEMPT TIME -- and then compared the walked
    // ladder against cancel_last_attempt_start_ms(), which is the same
    // quantity computed twice. It could not have caught F3 and did not: the
    // ladder authorised its final attempt at exactly budget_ms, cancel_ids
    // rejected every id at `now >= deadline` without issuing an RPC, and the
    // wallet was in fact last asked at 75'000 while this test proved 90'000.
    //
    // Total sleeping time is kept here as what it is, and nothing about
    // coverage is claimed from it.
    EXPECT_EQ(total_sleep,
              cancel_last_attempt_start_ms(cfg, TakeFailureClass::Unsynced))
        << "the constexpr schedule proof and the walked ladder must agree "
           "about when the ladder stops sleeping";
}

// ---------------------------------------------------------------------------
// [F3 2026-09-03] The interaction the policy test could not see.
//
// The bug was never inside plan_cancel_retry and never inside cancel_ids. It
// was in the composition of three facts, only one of which is in this header:
//   1. the ladder authorises its final attempt at elapsed E;
//   2. the engine passes D = t0 + cfg.budget_ms into cancel_ids;
//   3. cancel_ids admits an id iff now < D.
// E == D satisfied every existing assertion and issued zero RPCs.
//
// This is the assertion that must hold: E < D STRICTLY, for every attempt the
// ladder authorises -- not just the last one.
//
// MUTATIONS THAT MUST TRIP THIS TEST:
//   * set execution_reserve_ms to 0     -> final attempt lands at 90'000, is
//                                          not admitted, coverage falls to
//                                          75'000, both EXPECTs below fail.
//   * relax cancel_ids_admits to `<=`   -> the 90'000 attempt is "admitted",
//                                          AdmitsAtTheDeadline fails.
//   * set max_attempts back to 5        -> coverage falls to 75'000 and the
//                                          kLongestMeasuredDesyncMs EXPECT
//                                          fails.
// ---------------------------------------------------------------------------
TEST(CancelRetry, TheFinalAttemptIsAdmittedByTheDeadlineItRunsAgainst)
{
    const CancelRetryConfig cfg{};
    CancelAttemptState s = incident_state();

    std::uint32_t elapsed         = 0;
    std::uint32_t last_admitted   = 0;   // attempt 1 goes out at t=0
    std::uint32_t authorised      = 1;
    std::uint32_t admitted_count  = 1;

    for (int guard = 0; guard < 1000; ++guard) {
        const CancelRetryPlan p = plan_cancel_retry(s, cfg);
        if (p.verdict != CancelRetryVerdict::Retry) break;
        elapsed        += p.delay_ms;
        s.elapsed_ms    = elapsed;
        s.attempts_made = p.next_attempt;
        authorised      = p.next_attempt;

        // EVERY attempt the ladder authorises must be one the gate will let
        // through. An authorised attempt that cannot issue an RPC is not a
        // retry, it is a log line.
        EXPECT_TRUE(cancel_ids_admits(elapsed, cfg.budget_ms))
            << "attempt " << authorised << " was authorised at " << elapsed
            << " ms against a " << cfg.budget_ms
            << " ms deadline -- cancel_ids would reject every id before "
               "issuing a single RPC";
        if (cancel_ids_admits(elapsed, cfg.budget_ms)) {
            last_admitted = elapsed;
            ++admitted_count;
        }
    }

    EXPECT_EQ(admitted_count, 6u)
        << "all six attempts must actually reach the wallet; with the clamp "
           "consuming the whole remaining budget the sixth reached nothing";
    EXPECT_EQ(last_admitted, 85'000u);
    EXPECT_LT(last_admitted, cfg.budget_ms)
        << "STRICTLY before. Landing ON the deadline is the F3 defect";

    // THE COVERAGE CLAIM, asserted on the last time the WALLET IS ASKED.
    EXPECT_GE(last_admitted, kLongestMeasuredDesyncMs)
        << "the ladder must still be ASKING THE WALLET when the longest "
           "desync episode this repo has measured would have cleared";

    // And the walked ladder must agree with the constexpr proof, which is a
    // different function from the one above precisely because the two
    // quantities are different.
    EXPECT_EQ(last_admitted,
              cancel_last_effective_attempt_start_ms(
                  cfg, TakeFailureClass::Unsynced));
}

// The gate itself. One comparison, and the direction of the inequality is the
// whole finding: an attempt starting exactly AT the deadline gets no
// execution window.
//
// MUTATION: change cancel_ids_admits to `now_ms <= deadline_ms` -> fails.
TEST(CancelRetry, TheDeadlineGateRefusesAnAttemptThatStartsOnIt)
{
    EXPECT_TRUE(cancel_ids_admits(0, 90'000));
    EXPECT_TRUE(cancel_ids_admits(85'000, 90'000));
    EXPECT_TRUE(cancel_ids_admits(89'999, 90'000));
    EXPECT_FALSE(cancel_ids_admits(90'000, 90'000))
        << "AT the deadline the engine's cancel_ids rejects every id without "
           "an RPC, so authorising an attempt there delivers nothing";
    EXPECT_FALSE(cancel_ids_admits(90'001, 90'000));

    // The steady_clock overload must be the SAME rule, not a restatement of
    // it. MUTATION: reimplement it as its own `now < deadline` and then
    // change one of the two -> these disagree.
    const auto now = std::chrono::steady_clock::now();
    EXPECT_TRUE(cancel_ids_admits(now, now + std::chrono::seconds(5)));
    EXPECT_FALSE(cancel_ids_admits(now, now));
    EXPECT_FALSE(cancel_ids_admits(now, now - std::chrono::seconds(1)));
}

// The reserve is what buys the strict inequality. Without it the clamp always
// schedules the final attempt to wake at exactly budget_ms, FOR ANY schedule
// and ANY elapsed -- the general result, not an accident of these constants.
//
// MUTATION: restore `delay = budget_left` -> the zero-reserve arm here starts
// passing the admits check and the EXPECT_FALSE fails.
TEST(CancelRetry, WithoutTheExecutionReserveEveryClampedRetryIsDead)
{
    CancelRetryConfig cfg{};
    cfg.execution_reserve_ms = 0;

    // Walk to the clamped attempt with the reserve disabled.
    CancelAttemptState s = incident_state();
    std::uint32_t elapsed = 0;
    std::uint32_t last    = 0;
    for (int guard = 0; guard < 1000; ++guard) {
        const CancelRetryPlan p = plan_cancel_retry(s, cfg);
        if (p.verdict != CancelRetryVerdict::Retry) break;
        elapsed        += p.delay_ms;
        s.elapsed_ms    = elapsed;
        s.attempts_made = p.next_attempt;
        last            = elapsed;
    }

    EXPECT_EQ(last, cfg.budget_ms)
        << "the clamp sets delay = budget_left, so the attempt always wakes "
           "at exactly the budget";
    EXPECT_FALSE(cancel_ids_admits(last, cfg.budget_ms))
        << "...and is therefore rejected without issuing an RPC. This is F3.";
    EXPECT_EQ(cancel_last_effective_attempt_start_ms(
                  cfg, TakeFailureClass::Unsynced), 75'000u)
        << "so real coverage collapses to 75 s -- 5.9 s short of the 80.9 s "
           "episode the budget was sized for, which is the exact shortfall "
           "the previous review believed raising max_attempts had fixed";
}

// A wallet that HANGS rather than refuses is the expensive shape: each
// attempt can burn request_timeout{30s} x max_retries{3}. The loop must be
// bounded on wall clock, not on the attempt counter, or a hung wallet turns a
// five-attempt ladder into many minutes of a shutdown that will not finish.
//
// MUTATION: drop the `elapsed_ms >= budget_ms` clause -> returns Retry here
// and FAILS.
TEST(CancelRetry, AHangingWalletIsBoundedByWallClockNotByAttemptCount)
{
    const CancelRetryConfig cfg{};
    CancelAttemptState s = incident_state();
    s.attempts_made = 2;        // 4 of the 6 attempts still unused
    s.elapsed_ms    = 89'700;   // two slow attempts have eaten the budget

    const CancelRetryPlan p = plan_cancel_retry(s, cfg);
    EXPECT_EQ(p.verdict, CancelRetryVerdict::Stop);
    EXPECT_EQ(p.stop_reason, CancelStopReason::BudgetExhausted)
        << "attempts are nowhere near exhausted -- the clock is";

    // And at the cap exactly, without relying on the min-useful clamp.
    s.elapsed_ms = cfg.budget_ms;
    const CancelRetryPlan q = plan_cancel_retry(s, cfg);
    EXPECT_EQ(q.verdict, CancelRetryVerdict::Stop);
    EXPECT_EQ(q.stop_reason, CancelStopReason::BudgetExhausted);

    // [calibration note] The first cut of this test used elapsed_ms = 89'000,
    // which leaves exactly min_useful_delay_ms and therefore legitimately
    // returns Retry with a clamped 1 s sleep -- still inside the budget. That
    // is the documented clamp (see ThePartialTailIsClamped...), not a bound
    // failure, so the INPUTS were wrong here rather than the policy. Recorded
    // because "the test disagreed, so I changed the code" is the wrong
    // resolution and the next reader should be able to check which way it went.
}

// The tail of the budget is clamped into, not spun on -- and the clamp now
// stops one execution reserve SHORT of the budget rather than at it.
//
// [F3 2026-09-03] This test previously asserted `elapsed=86'500 -> Retry with
// delay 3'500, budget_left_ms 0`, i.e. an attempt scheduled to wake at
// EXACTLY 90'000. That expectation was the defect written down as a
// requirement: the engine hands 90'000 to cancel_ids as its deadline, and
// cancel_ids admits an id only while `now < deadline`, so that attempt
// rejected every id without issuing one RPC. `budget_left_ms == 0` was the
// tell -- zero budget left after the sleep means zero time to do anything.
//
// The correct behaviour is below: a clamp is taken only when the attempt it
// schedules lands STRICTLY inside the deadline with room to issue.
//
// MUTATION: delete the min_useful_delay_ms guard -> the useless case returns
// Retry with a sub-second sleep and FAILS.
// MUTATION: set execution_reserve_ms to 0 -> the dead-tail case below starts
// returning Retry at 90'000 and FAILS.
TEST(CancelRetry, ThePartialTailIsClampedShortOfTheDeadline)
{
    const CancelRetryConfig cfg{};

    // 10 s of budget left, next ladder step would be 20 s. Clamp to 5 s so
    // the attempt lands at 85 s, inside the deadline, with the reserve left
    // to actually issue it.
    CancelAttemptState usable = incident_state();
    usable.attempts_made = 3;
    usable.elapsed_ms    = 80'000;
    const CancelRetryPlan a = plan_cancel_retry(usable, cfg);
    EXPECT_EQ(a.verdict, CancelRetryVerdict::Retry);
    EXPECT_EQ(a.delay_ms, 5'000u)
        << "clamped into what is left MINUS the execution reserve, not the "
           "full 20s and not the full remaining budget";
    EXPECT_EQ(a.budget_left_ms, cfg.execution_reserve_ms)
        << "the reserve is what survives the sleep -- it is the window the "
           "attempt gets to be admitted in";
    EXPECT_TRUE(cancel_ids_admits(usable.elapsed_ms + a.delay_ms,
                                  cfg.budget_ms))
        << "the whole point: the attempt this plan authorises must be one "
           "cancel_ids will actually let through";

    // THE OLD EXPECTATION, now correctly a stop. 3.5 s left is less than the
    // reserve, so any attempt scheduled from here would wake at or past the
    // deadline and issue nothing. Refusing is strictly better than
    // authorising a retry that cannot happen and then reporting it as one.
    CancelAttemptState dead_tail = usable;
    dead_tail.elapsed_ms = 86'500;
    const CancelRetryPlan b = plan_cancel_retry(dead_tail, cfg);
    EXPECT_EQ(b.verdict, CancelRetryVerdict::Stop);
    EXPECT_EQ(b.stop_reason, CancelStopReason::BudgetExhausted);

    CancelAttemptState useless = usable;
    useless.elapsed_ms = 89'600;          // 400 ms left
    const CancelRetryPlan c = plan_cancel_retry(useless, cfg);
    EXPECT_EQ(c.verdict, CancelRetryVerdict::Stop);
    EXPECT_EQ(c.stop_reason, CancelStopReason::BudgetExhausted);
}

// ---------------------------------------------------------------------------
// Honesty about what the budget does NOT cover.
// ---------------------------------------------------------------------------

// 90 s clears the 80.9 s episode measured this session and the ~94% of
// episodes that last a single cycle. It does NOT clear the 155 s worst-ever
// from take_retry.hpp:54. This test exists so that number stays an admission
// rather than drifting into a claim: if someone later says "the retry covers
// the desync", this is the line that contradicts them.
//
// MUTATION: raise budget_ms to 200'000 -> FAILS, correctly, and forces the
// header note to be rewritten with it.
TEST(CancelRetry, TheBudgetDoesNotCoverTheWorstObservedEpisode)
{
    const CancelRetryConfig cfg{};
    // The header now owns this constant so the compile-time proof and this
    // test cannot drift apart.
    static_assert(kWorstObservedDesyncMs == 155'000);
    EXPECT_LT(cfg.budget_ms, kWorstObservedDesyncMs)
        << "the budget is deliberately below the worst observed desync. A "
           "desync longer than the ladder is a case this PR does NOT close, "
           "and the alert naming the still-live ids is what covers it";

    CancelAttemptState s = incident_state();
    s.attempts_made = 4;
    s.elapsed_ms    = kWorstObservedDesyncMs - 1;
    EXPECT_EQ(plan_cancel_retry(s, cfg).verdict, CancelRetryVerdict::Stop);
}

// ---------------------------------------------------------------------------
// The classes do not share a schedule.
// ---------------------------------------------------------------------------

// A funding refusal on a CANCEL is not fixed by waiting -- the caller has a
// zero-fee secure ladder for it. Stopping here is an escalation and must be
// distinguishable from running out of road, or the alert says the wrong thing
// and the operator reaches for the wrong tool.
//
// MUTATION: treat Funding like Unsynced -> returns Retry and FAILS; the
// engine would then burn 75 s before escalating.
TEST(CancelRetry, AFundingRefusalEscalatesImmediatelyAndSaysSo)
{
    const CancelRetryConfig cfg{};
    CancelAttemptState s = incident_state();
    s.last_class = classify_take_failure("insufficient funds in wallet 8");
    ASSERT_EQ(s.last_class, TakeFailureClass::Funding);

    const CancelRetryPlan p = plan_cancel_retry(s, cfg);
    EXPECT_EQ(p.verdict, CancelRetryVerdict::Stop);
    EXPECT_EQ(p.stop_reason, CancelStopReason::NeedsEmergencyLadder);
    EXPECT_NE(p.stop_reason, CancelStopReason::AttemptsExhausted);
    EXPECT_EQ(p.delay_ms, 0u) << "escalate now, do not sleep first";
}

// An unmodelled failure still gets a retry -- it may be a transport blip --
// but on a shorter leash than the flap we have 42 hours of statistics for.
//
// MUTATION: use max_attempts for Other as well -> the third call returns
// Retry and FAILS.
TEST(CancelRetry, AnUnmodelledFailureGetsAShorterLeashThanTheSyncFlap)
{
    const CancelRetryConfig cfg{};
    CancelAttemptState s = incident_state();
    s.last_class = classify_take_failure("connection reset by peer");
    ASSERT_EQ(s.last_class, TakeFailureClass::Other);

    s.attempts_made = 1;
    EXPECT_EQ(plan_cancel_retry(s, cfg).verdict, CancelRetryVerdict::Retry);
    s.attempts_made = 2;
    EXPECT_EQ(plan_cancel_retry(s, cfg).verdict, CancelRetryVerdict::Retry);
    s.attempts_made = 3;
    const CancelRetryPlan p = plan_cancel_retry(s, cfg);
    EXPECT_EQ(p.verdict, CancelRetryVerdict::Stop);
    EXPECT_EQ(p.stop_reason, CancelStopReason::AttemptsExhausted);

    // ... whereas the sync class is still going at attempt 3.
    CancelAttemptState sync = s;
    sync.last_class = TakeFailureClass::Unsynced;
    EXPECT_EQ(plan_cancel_retry(sync, cfg).verdict, CancelRetryVerdict::Retry);
}

// ---------------------------------------------------------------------------
// Partial success: retry the REMAINDER, never the whole set, never nothing.
// ---------------------------------------------------------------------------

// 4 of 7 cancelled is not "done" and it is not "abandon the set" either.
// The policy is driven by what is LEFT.
//
// MUTATION: key the Done branch on `attempts_made` instead of
// `remaining_offers` -> the first case stops and FAILS.
TEST(CancelRetry, APartialSuccessRetriesTheRemainderAndZeroLeftIsTheOnlyDone)
{
    const CancelRetryConfig cfg{};

    CancelAttemptState partial = incident_state();
    partial.remaining_offers = 3;              // 4 of the 7 went through
    const CancelRetryPlan p = plan_cancel_retry(partial, cfg);
    EXPECT_EQ(p.verdict, CancelRetryVerdict::Retry);
    EXPECT_EQ(p.next_attempt, 2u);

    CancelAttemptState finished = partial;
    finished.remaining_offers = 0;
    const CancelRetryPlan q = plan_cancel_retry(finished, cfg);
    EXPECT_EQ(q.verdict, CancelRetryVerdict::Stop);
    EXPECT_EQ(q.stop_reason, CancelStopReason::Done);

    // Even a fully exhausted, over-budget, funding-refused state reports Done
    // when nothing is left. Done is checked FIRST so a finished cancel can
    // never be reported through a failure enumerator.
    CancelAttemptState finished_late{};
    finished_late.attempts_made    = 99;
    finished_late.remaining_offers = 0;
    finished_late.elapsed_ms       = 10'000'000;
    finished_late.last_class       = TakeFailureClass::Funding;
    EXPECT_EQ(plan_cancel_retry(finished_late, cfg).stop_reason,
              CancelStopReason::Done);
}

// ---------------------------------------------------------------------------
// Defaults and degenerate inputs.
// ---------------------------------------------------------------------------

// A default-constructed plan must be inert: Stop, no sleep, and a stop_reason
// that does NOT read as success. This is the file's departure from "zero
// declines" and it only holds if `Unknown` never masquerades as `Done` --
// the caller alerts on everything that is not Done, so a zero-value that
// meant "finished" would be the silent fail-open in a new costume.
//
// MUTATION: reorder CancelStopReason so Done = 0 -> FAILS.
TEST(CancelRetry, ADefaultConstructedPlanIsInertAndDoesNotClaimSuccess)
{
    const CancelRetryPlan p{};
    EXPECT_EQ(p.verdict, CancelRetryVerdict::Stop);
    EXPECT_EQ(p.stop_reason, CancelStopReason::Unknown);
    EXPECT_NE(p.stop_reason, CancelStopReason::Done);
    EXPECT_EQ(p.delay_ms, 0u);
    EXPECT_EQ(p.next_attempt, 0u);

    EXPECT_EQ(static_cast<int>(CancelRetryVerdict::Stop), 0);
    EXPECT_EQ(static_cast<int>(CancelStopReason::Unknown), 0);
}

// Asking with nothing attempted is a caller bug, not an authorisation. It
// must not silently return "go" -- attempts_made is what the ladder indexes
// off, and a zero there would restart the schedule on every call.
//
// MUTATION: drop the attempts_made == 0 guard -> returns Retry and FAILS.
TEST(CancelRetry, NoAttemptMadeYetIsNotAnAuthorisation)
{
    CancelAttemptState s{};
    s.remaining_offers = 7;
    s.attempts_made    = 0;
    const CancelRetryPlan p = plan_cancel_retry(s, CancelRetryConfig{});
    EXPECT_EQ(p.verdict, CancelRetryVerdict::Stop);
    EXPECT_NE(p.stop_reason, CancelStopReason::Done);
}

// The ladder saturates instead of overflowing, and a zero base disables the
// sleeping entirely rather than producing a spin.
TEST(CancelRetry, TheBackoffSaturatesAndAZeroBaseDisablesRetries)
{
    const CancelRetryConfig cfg{};
    EXPECT_EQ(cancel_backoff_ms(0, cfg), 0u);
    EXPECT_EQ(cancel_backoff_ms(1, cfg), 5'000u);
    EXPECT_EQ(cancel_backoff_ms(4, cfg), 40'000u);
    EXPECT_EQ(cancel_backoff_ms(1'000'000, cfg), 40'000u);

    CancelRetryConfig off = cfg;
    off.base_delay_ms = 0;
    EXPECT_EQ(cancel_backoff_ms(3, off), 0u);
    CancelAttemptState s = incident_state();
    const CancelRetryPlan p = plan_cancel_retry(s, off);
    EXPECT_EQ(p.verdict, CancelRetryVerdict::Stop)
        << "a zero delay must stop, never spin";
    EXPECT_EQ(p.stop_reason, CancelStopReason::BudgetExhausted);
}

// A budget of zero means "do not retry at all" and must be expressible
// without any other change -- the operator-facing kill switch for this
// mechanism if it ever misbehaves in production.
TEST(CancelRetry, AZeroBudgetDisablesTheRetryCleanly)
{
    CancelRetryConfig off{};
    off.budget_ms = 0;
    const CancelRetryPlan p = plan_cancel_retry(incident_state(), off);
    EXPECT_EQ(p.verdict, CancelRetryVerdict::Stop);
    EXPECT_EQ(p.stop_reason, CancelStopReason::BudgetExhausted);
}

// ===========================================================================
// LAYER 2 -- the ladder state that engine.cpp used to own untested.
// ===========================================================================

namespace {

const std::vector<std::string> kSevenIncidentOffers = {
    "0x9a72dfe913", "0x1111111111", "0x2222222222", "0x3333333333",
    "0x4444444444", "0x5555555555", "0x6666666666",
};

constexpr const char* kSyncRefusal =
    "Wallet needs to be fully synced before making transactions.";
constexpr const char* kFundingRefusal =
    "Insufficient funds: spendable balance is 0";

/// One scripted wallet answer.
struct ScriptedAttempt {
    /// How many of the offered ids this attempt accepts (from the front).
    std::size_t              accept_n{0};
    /// Failure text for the ids it does not accept.
    std::string             error{kSyncRefusal};
    /// The class the OfferManager would have folded across the batch.
    TakeFailureClass        cls{TakeFailureClass::Unsynced};
    bool                    bulk_submitted{false};
    /// Ids reported as already having a cancel spend in flight.
    std::size_t             already_pending_n{0};
};

/// The result of driving CancelLadder exactly as Engine::shutdown() drives it.
struct LadderRun {
    std::vector<std::string> outstanding;
    std::vector<std::string> submitted;
    std::vector<std::string> already_pending;
    std::uint32_t            attempts{0};
    std::uint32_t            total_sleep_ms{0};
    CancelStopReason         stop_reason{CancelStopReason::Unknown};
    bool                     clean{false};
    /// Every attempt index the ladder actually authorised, in order. Proves
    /// the first attempt is the bulk one and retries are per-id.
    std::vector<std::uint32_t> attempt_order;
};

/// Drive the ladder with a scripted wallet and a virtual clock. Mirrors the
/// shape of the loop in Engine::shutdown(): next() -> co_await -> record().
/// The engine sleeps on a real timer; here the sleep just advances the clock,
/// which is what makes the whole thing testable without an io_context.
LadderRun run_ladder(const std::vector<std::string>& ids,
                     const CancelRetryConfig&        cfg,
                     const std::vector<ScriptedAttempt>& script)
{
    CancelLadder  ladder(ids, cfg);
    LadderRun     run{};
    std::uint32_t clock_ms = 0;

    for (int guard = 0; guard < 1000; ++guard) {
        const auto act = ladder.next(clock_ms);
        if (act.step == CancelLadderStep::Finish) break;

        if (act.step == CancelLadderStep::Sleep) {
            clock_ms            += act.delay_ms;
            run.total_sleep_ms  += act.delay_ms;
            continue;
        }

        run.attempt_order.push_back(act.attempt_index);

        // The script runs out -> the wallet keeps refusing everything.
        const ScriptedAttempt s =
            (act.attempt_index <= script.size())
                ? script[act.attempt_index - 1]
                : ScriptedAttempt{};

        const auto& live = ladder.outstanding();
        CancelAttemptOutcome oc{};
        oc.bulk_submitted = s.bulk_submitted;
        std::size_t i = 0;
        for (; i < live.size() && i < s.accept_n; ++i) {
            oc.cancelled.push_back(live[i]);
        }
        for (std::size_t k = 0; k < s.already_pending_n && i < live.size();
             ++k, ++i) {
            oc.already_pending.push_back(live[i]);
        }
        for (; i < live.size(); ++i) oc.failed.push_back(live[i]);
        if (!oc.failed.empty()) {
            oc.last_error  = s.error;
            oc.worst_class = s.cls;
        }
        ladder.record(std::move(oc));
    }

    run.outstanding     = ladder.outstanding();
    run.submitted       = ladder.submitted();
    run.already_pending = ladder.already_pending();
    run.attempts        = ladder.attempts();
    run.stop_reason     = ladder.stop_reason();
    run.clean           = ladder.clean();
    return run;
}

}  // namespace

// ---------------------------------------------------------------------------
// THE REGRESSION, at the layer that was actually broken.
// ---------------------------------------------------------------------------

// The 2026-09-02 incident end to end: seven offers, the wallet refuses
// everything on attempt 1 with the sync text, and then -- as observed -- the
// condition clears on its own and a later attempt takes the whole book.
//
// MUTATION (this is M3, the one that used to leave 1246/1246 green): in
// CancelLadder::record, replace `outstanding_ = std::move(oc.failed);` with
// `outstanding_.clear();`. The ladder then reports clean() == true,
// stop_reason Done, and ZERO offers outstanding after a single attempt in
// which the wallet accepted nothing -- exactly the "we tried once, call it
// done" shape. Every assertion below about attempts, outstanding and
// stop_reason FAILS.
TEST(CancelLadderState, TheFlapIsRiddenOutAndTheBookEndsEmpty)
{
    const CancelRetryConfig cfg{};
    // Refuse, refuse, then the flap clears and everything goes.
    const std::vector<ScriptedAttempt> script = {
        ScriptedAttempt{0, kSyncRefusal, TakeFailureClass::Unsynced, false, 0},
        ScriptedAttempt{0, kSyncRefusal, TakeFailureClass::Unsynced, false, 0},
        ScriptedAttempt{7, "",           TakeFailureClass::Unsynced, false, 0},
    };

    const LadderRun r = run_ladder(kSevenIncidentOffers, cfg, script);

    EXPECT_EQ(r.attempts, 3u) << "one attempt is the bug; three is the fix";
    EXPECT_TRUE(r.outstanding.empty());
    EXPECT_EQ(r.submitted.size(), 7u);
    EXPECT_EQ(r.stop_reason, CancelStopReason::Done);
    EXPECT_TRUE(r.clean);
    // 5 s + 10 s: the recovery observed at ~3 min is out of reach, but the
    // sub-15 s flap that produced the incident is squarely inside.
    EXPECT_EQ(r.total_sleep_ms, 15'000u);
    EXPECT_EQ(r.attempt_order, (std::vector<std::uint32_t>{1, 2, 3}));
}

// A cancel that never succeeds must END, must report itself UNCLEAN, and must
// still be naming the live ids -- that list is what the operator alert and the
// watchdog fallback are handed.
//
// MUTATION: `outstanding_.clear()` in record() -> clean() becomes true with a
// Done stop_reason and the fallback is never invoked. FAILS on three lines.
// MUTATION: make clean() return `stop_reason_ == Done` only -> still FAILS,
// because a half-populated ladder must not be able to claim a clean finish.
TEST(CancelLadderState, ATotalFailureStopsUncleanAndStillNamesTheOffers)
{
    const CancelRetryConfig cfg{};
    const LadderRun r = run_ladder(kSevenIncidentOffers, cfg, {});

    EXPECT_FALSE(r.clean) << "nothing was cancelled -- this is not a clean stop";
    EXPECT_EQ(r.outstanding.size(), 7u)
        << "the still-live ids must survive the ladder; the 2026-09-02 alert "
           "said 'cancel them by hand NOW' and named none of them";
    EXPECT_EQ(r.outstanding, kSevenIncidentOffers);
    EXPECT_TRUE(r.submitted.empty());
    EXPECT_EQ(r.stop_reason, CancelStopReason::AttemptsExhausted);
    EXPECT_NE(r.stop_reason, CancelStopReason::Done);
    EXPECT_EQ(r.attempts, cfg.max_attempts);
}

// A partial success must retry ONLY the remainder. Re-running the bulk
// endpoint would re-charge a fee against the offers that already went.
//
// MUTATION: in record(), append oc.failed to outstanding_ instead of
// assigning -> the second attempt is handed ids that already succeeded and
// the submitted count double-counts. FAILS.
TEST(CancelLadderState, OnlyTheRemainderIsRetried)
{
    const CancelRetryConfig cfg{};
    const std::vector<ScriptedAttempt> script = {
        ScriptedAttempt{4, kSyncRefusal, TakeFailureClass::Unsynced, false, 0},
        ScriptedAttempt{3, "",           TakeFailureClass::Unsynced, false, 0},
    };

    const LadderRun r = run_ladder(kSevenIncidentOffers, cfg, script);

    EXPECT_EQ(r.attempts, 2u);
    EXPECT_TRUE(r.outstanding.empty());
    ASSERT_EQ(r.submitted.size(), 7u) << "each id submitted exactly once";
    // The last three of the original order are the ones the retry took.
    EXPECT_EQ(r.submitted[4], kSevenIncidentOffers[4]);
    EXPECT_EQ(r.submitted[6], kSevenIncidentOffers[6]);
    EXPECT_TRUE(r.clean);
}

// ---------------------------------------------------------------------------
// The class fold, at the layer where the loss actually happened.
// ---------------------------------------------------------------------------

// Six offers refused for "not synced" and a seventh short of XCH. The batch
// must keep the SYNC ladder: waiting cannot refill a wallet, but one offer's
// funding problem must not cancel six other offers' retries.
//
// MUTATION: have cancel_ids report the LAST failure's class instead of the
// folded one -- i.e. pass TakeFailureClass::Funding here, which is what the
// old `out.last_error = std::move(err)` produced when the funding offer came
// last in State order -> the ladder stops after ONE attempt with
// NeedsEmergencyLadder and all seven offers are handed to the same-wallet
// fallback that failed identically on 2026-09-02. FAILS on attempts and on
// stop_reason.
TEST(CancelLadderState, OneFundingRefusalDoesNotStripTheBatchsSyncLadder)
{
    const CancelRetryConfig cfg{};

    // The fold that offer_manager performs across a heterogeneous batch.
    TakeFailureClass folded = classify_take_failure(kSyncRefusal);
    folded = more_retryable(folded, classify_take_failure(kFundingRefusal));
    ASSERT_EQ(folded, TakeFailureClass::Unsynced)
        << "sync outranks funding: it is the one that clears on its own";

    const std::vector<ScriptedAttempt> script = {
        ScriptedAttempt{0, kFundingRefusal, folded, false, 0},
        ScriptedAttempt{7, "",              folded, false, 0},
    };
    const LadderRun r = run_ladder(kSevenIncidentOffers, cfg, script);

    EXPECT_EQ(r.attempts, 2u) << "the sync ladder survives the funding text";
    EXPECT_TRUE(r.clean);

    // And the control: a batch that really is ALL funding still escalates
    // immediately rather than burning 90 s first.
    const std::vector<ScriptedAttempt> all_funding = {
        ScriptedAttempt{0, kFundingRefusal, TakeFailureClass::Funding,
                        false, 0},
    };
    const LadderRun f = run_ladder(kSevenIncidentOffers, cfg, all_funding);
    EXPECT_EQ(f.attempts, 1u);
    EXPECT_EQ(f.stop_reason, CancelStopReason::NeedsEmergencyLadder);
    EXPECT_EQ(f.total_sleep_ms, 0u) << "escalate now, do not wait";
    EXPECT_FALSE(f.clean);
}

// ---------------------------------------------------------------------------
// Offers already being cancelled are neither retried nor counted as done.
// ---------------------------------------------------------------------------

// An id whose cancel spend is already in flight must not be re-charged, and
// must not silently vanish either. It is its own bucket precisely so it can
// be reported.
//
// MUTATION: fold already_pending into cancelled_ -> the DB-stamp loop in
// Engine::shutdown() would mark them 'cancelled' on a spend that has not
// confirmed. MUTATION: drop already_pending entirely (the plain `continue`
// that the first fix reached for) -> the ids appear in NO list and the
// EXPECT_EQ on total accounting FAILS.
TEST(CancelLadderState, AnInFlightCancelIsNeitherRechargedNorLost)
{
    const CancelRetryConfig cfg{};
    const std::vector<ScriptedAttempt> script = {
        // 3 accepted, 2 already cancelling, 2 refused.
        ScriptedAttempt{3, kSyncRefusal, TakeFailureClass::Unsynced, false, 2},
        ScriptedAttempt{2, "",           TakeFailureClass::Unsynced, false, 0},
    };
    const LadderRun r = run_ladder(kSevenIncidentOffers, cfg, script);

    EXPECT_EQ(r.already_pending.size(), 2u);
    EXPECT_EQ(r.submitted.size(), 5u) << "3 on the first pass, 2 on the retry";
    EXPECT_TRUE(r.outstanding.empty());
    EXPECT_TRUE(r.clean)
        << "an in-flight cancel is not an outstanding offer -- retrying it "
           "pays a second fee for one spend";

    // Nothing is lost: every id ends in exactly one bucket.
    EXPECT_EQ(r.submitted.size() + r.already_pending.size()
                  + r.outstanding.size(),
              kSevenIncidentOffers.size());
}

// ---------------------------------------------------------------------------
// The inert default, at this layer too.
// ---------------------------------------------------------------------------

// A ladder nobody has stepped must not authorise an RPC, and an EMPTY book
// must finish immediately and cleanly without one either.
//
// MUTATION: make CancelLadderStep::Attempt the zero enumerator -> a
// default-constructed action authorises a cancel attempt. FAILS.
TEST(CancelLadderState, AnUnsteppedLadderAuthorisesNothing)
{
    // The ENUM's zero value, not just the struct's default member
    // initialiser. [mutation-check note] The first cut of this test asserted
    // only `CancelLadderAction{}.step == Finish`, which is VACUOUS with
    // respect to the enumerator ordering: the struct initialises `step` to
    // Finish explicitly, so reordering the enum to make Attempt = 0 left the
    // whole suite green. Value-initialising the enum is what actually pins
    // the repo's "the zero enumerator declines" rule.
    EXPECT_EQ(CancelLadderStep{}, CancelLadderStep::Finish)
        << "a value-initialised step must be inert; zero meaning Attempt is "
           "the wrong direction of dangerous here";
    EXPECT_EQ(static_cast<int>(CancelLadderStep::Finish), 0);
    EXPECT_EQ(CancelLadderAction{}.step, CancelLadderStep::Finish);

    CancelLadder empty(std::vector<std::string>{}, CancelRetryConfig{});
    EXPECT_EQ(empty.next(0).step, CancelLadderStep::Finish);
    EXPECT_EQ(empty.stop_reason(), CancelStopReason::Done)
        << "no book is the one genuinely clean start";
    EXPECT_TRUE(empty.clean());
    EXPECT_EQ(empty.attempts(), 0u);
}

// The first attempt is authorised WITHOUT consulting the retry policy: there
// is no failure to classify yet, and plan_cancel_retry correctly refuses to
// authorise anything from attempts_made == 0. If the ladder ever routed
// attempt 1 through the policy, the shutdown would cancel nothing at all.
//
// MUTATION: delete the `attempts_ == 0` branch in next() -> the very first
// call returns Finish with stop_reason Unknown and NO cancel is ever
// attempted. FAILS, loudly.
TEST(CancelLadderState, TheFirstAttemptIsNotGatedOnAPolicyItCannotSatisfy)
{
    CancelLadder l(kSevenIncidentOffers, CancelRetryConfig{});
    const auto first = l.next(0);
    EXPECT_EQ(first.step, CancelLadderStep::Attempt);
    EXPECT_EQ(first.attempt_index, 1u);
    EXPECT_EQ(l.attempts(), 0u) << "not counted until it is recorded";
}

// The wall-clock budget must bind through the ladder, not only through
// plan_cancel_retry in isolation: a slow wallet that eats the clock inside
// its attempts stops the ladder even with attempts to spare.
//
// MUTATION: pass a constant 0 as elapsed_ms in the engine's next() call ->
// the ladder never sees the clock and runs its full attempt count. The
// stop_reason assertion FAILS.
TEST(CancelLadderState, SlowAttemptsExhaustTheBudgetNotJustTheSleeps)
{
    const CancelRetryConfig cfg{};
    CancelLadder l(kSevenIncidentOffers, CancelRetryConfig{});

    // Attempt 1 goes out, and returns having burned most of the budget.
    ASSERT_EQ(l.next(0).step, CancelLadderStep::Attempt);
    CancelAttemptOutcome oc{};
    oc.failed      = kSevenIncidentOffers;
    oc.last_error  = kSyncRefusal;
    oc.worst_class = TakeFailureClass::Unsynced;
    l.record(std::move(oc));

    const auto act = l.next(cfg.budget_ms - 100);
    EXPECT_EQ(act.step, CancelLadderStep::Finish);
    EXPECT_EQ(l.stop_reason(), CancelStopReason::BudgetExhausted)
        << "four attempts remain unused -- the clock is what ran out";
    EXPECT_FALSE(l.clean());
    EXPECT_EQ(l.outstanding().size(), 7u);
}

// [F3 2026-09-03] CancelOutcome::deadline_hit was set by cancel_ids and READ
// BY NOBODY -- never copied across the engine boundary, never consulted here.
// It is the gate's own report that it ran out of time, and it is the only
// signal that distinguishes "out of time" from "the wallet refused".
//
// Without it the ladder inferred a reason from the failure class, and an
// attempt that issued no RPC classified nothing -- so worst_class fell back
// to Other, whose ceiling is other_max_attempts{3}, and the operator's
// CRITICAL alert announced "stopped because attempts-exhausted" for what was
// plainly a budget expiry.
//
// MUTATION: drop the deadline_hit_ check from next() -> the ladder keeps
// laddering and the stop_reason assertion FAILS.
// MUTATION: fold oc.worst_class in unconditionally in record() -> the
// worst_class assertion FAILS as the Unsynced flap is downgraded to Other.
TEST(CancelLadderState, ADeadlineHitStopsTheLadderAndIsReportedAsTheBudget)
{
    CancelLadder l(kSevenIncidentOffers, CancelRetryConfig{});

    // Attempt 1: a real sync refusal. The ladder learns the class.
    ASSERT_EQ(l.next(0).step, CancelLadderStep::Attempt);
    CancelAttemptOutcome first{};
    first.failed      = kSevenIncidentOffers;
    first.last_error  = kSyncRefusal;
    first.worst_class = TakeFailureClass::Unsynced;
    l.record(std::move(first));
    EXPECT_EQ(l.worst_class(), TakeFailureClass::Unsynced);

    // The ladder schedules a retry, and that retry finds the clock spent:
    // cancel_ids admits nothing, issues nothing, classifies nothing.
    const auto sleep_act = l.next(5'000);
    ASSERT_EQ(sleep_act.step, CancelLadderStep::Sleep);
    ASSERT_EQ(l.next(10'000).step, CancelLadderStep::Attempt);

    CancelAttemptOutcome starved{};
    starved.failed       = kSevenIncidentOffers;
    starved.deadline_hit = true;
    // No last_error and the default Other class -- exactly what a zero-RPC
    // attempt produces.
    l.record(std::move(starved));

    EXPECT_EQ(l.worst_class(), TakeFailureClass::Unsynced)
        << "an attempt that issued no RPC learned nothing about the failure "
           "class and must not downgrade the flap onto the short leash";

    const auto act = l.next(10'000);
    EXPECT_EQ(act.step, CancelLadderStep::Finish);
    EXPECT_EQ(l.stop_reason(), CancelStopReason::BudgetExhausted)
        << "the gate said it was out of time. Reporting 'attempts-exhausted' "
           "here sends the operator looking at the wrong constant";
    EXPECT_EQ(l.outstanding().size(), 7u) << "still live, and still named";
}

// A granted sleep is not a standing authorisation. plan_cancel_retry reserves
// an execution window so an overshoot should not happen on the schedule, but
// the sleep is a real timer on a machine that may be swapping during a
// shutdown. If it overshoots, the attempt it authorised would be rejected by
// cancel_ids without issuing an RPC -- so the clock is re-read AFTER the wait
// rather than the attempt being pre-committed before it.
//
// MUTATION: return the pre-committed Attempt without revalidating -> this
// FAILS with step == Attempt.
TEST(CancelLadderState, AnOvershotSleepDoesNotAuthoriseADeadAttempt)
{
    const CancelRetryConfig cfg{};
    CancelLadder l(kSevenIncidentOffers, CancelRetryConfig{});

    ASSERT_EQ(l.next(0).step, CancelLadderStep::Attempt);
    CancelAttemptOutcome oc{};
    oc.failed      = kSevenIncidentOffers;
    oc.last_error  = kSyncRefusal;
    oc.worst_class = TakeFailureClass::Unsynced;
    l.record(std::move(oc));

    // The ladder grants a sleep at t=5s...
    const auto sleep_act = l.next(5'000);
    ASSERT_EQ(sleep_act.step, CancelLadderStep::Sleep);

    // ...and the process is descheduled; we wake past the budget.
    const auto act = l.next(cfg.budget_ms + 250);
    EXPECT_EQ(act.step, CancelLadderStep::Finish)
        << "the authorised attempt would start at or past the deadline, so "
           "cancel_ids would reject every id before issuing an RPC";
    EXPECT_EQ(l.stop_reason(), CancelStopReason::BudgetExhausted);
    EXPECT_EQ(l.outstanding().size(), 7u);
}
