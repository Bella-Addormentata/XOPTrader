// test_fee_controller.cpp -- [S67 2026-09-20] the closed-loop fee controller.
//
// Three layers, all pure (no Engine, no RPC, no clock):
//
//   1. the rules, one at a time -- error, warm-up, the ANSWERED rule, the dead
//      time, the velocity-form PID, anti-windup, the probe schedule, the
//      feed-forward floor, the budget, the guards;
//   2. the controller against a SIMULATED HIDDEN FLOOR (FloorSim): a spend
//      confirms in a few peak heights when its rate is at or above the floor
//      and never below it -- the threshold plant fee_controller.hpp describes.
//      Convergence, overpayment, probing down, a 10x jump, oscillation;
//   3. FeeTracker with the controller wired in: flag off is the legacy fee for
//      every class, flag on is never 0 and keeps cancels funded.
//
// Every delay is in PEAK heights (18.75 s).  The numeric bounds asserted in
// layer 2 were MEASURED from this harness and are stated with their slack;
// SimulatedFloor.ReportsItsNumbers prints the measurements.

#include <gtest/gtest.h>

#include <xop/execution/fee_feedback.hpp>
#include <xop/strategy/fee_controller.hpp>
#include <xop/strategy/fee_tracker.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

namespace fee = xop::strategy::fee;
using fee::ActionClass;
using fee::ChangeReason;
using fee::Controller;
using fee::ControllerConfig;
using fee::Observation;
using fee::Signal;

constexpr std::uint64_t kMinFee = 15'000'000ULL;       // live fees.min_fee_mojos
constexpr std::uint64_t kMaxFee = 2'000'000'000ULL;    // room above a full mempool
constexpr std::uint32_t kT      = 8;                   // default target delay

ControllerConfig on_config(std::uint32_t warmup = 0)
{
    ControllerConfig c;
    c.enabled             = true;
    c.warmup_observations = warmup;
    return c;
}

Observation confirmed(double delay, double submit_level, std::uint32_t now)
{
    Observation o;
    o.signal       = Signal::Confirmed;
    o.blocks       = delay;
    o.attributed   = true;
    o.submit_level = submit_level;
    o.now          = now;
    return o;
}

Observation pending(double age, double submit_level, std::uint32_t now)
{
    Observation o;
    o.signal       = Signal::Pending;
    o.blocks       = age;
    o.attributed   = true;
    o.submit_level = submit_level;
    o.now          = now;
    return o;
}

Observation hard(Signal s, std::uint32_t now)
{
    Observation o;
    o.signal = s;
    o.now    = now;
    return o;
}

/// Raise the controller by `steps` capped steps (1.0 each at the defaults) on
/// ATTRIBUTED evidence -- a spend submitted at the current level, two targets
/// late -- then return the next free height.
std::uint32_t lift(Controller& c, int steps, std::uint32_t now)
{
    for (int i = 0; i < steps; ++i) {
        now += 50;
        c.observe(pending(24.0, c.level(), now));
    }
    return now + 50;
}

// ===========================================================================
// 1. Outputs, anchor and bounds
// ===========================================================================

TEST(FeeController, LevelZeroPaysMinFeeForACatCancelAndScalesByCost)
{
    const Controller c{on_config(), kMinFee, kMaxFee};
    EXPECT_DOUBLE_EQ(c.level(), 0.0);
    // The anchor: a CAT cancel, the commonest spend, pays exactly min_fee.
    EXPECT_EQ(c.fee_for(ActionClass::CancelCat, 100), kMinFee);
    // A cheaper class computes below min_fee and is clamped UP to it.
    EXPECT_EQ(c.fee_for(ActionClass::CancelXch, 100), kMinFee);
    EXPECT_EQ(c.fee_for(ActionClass::OfferAttached, 100), kMinFee);
    // A dearer class pays in proportion to its cost: 125M / 42.3M of min_fee.
    const std::uint64_t take = c.fee_for(ActionClass::Take, 100);
    EXPECT_NEAR(static_cast<double>(take), 15'000'000.0 * 125.0 / 42.3, 2.0);
}

TEST(FeeController, DisabledIsAPassthroughAndHearsNothing)
{
    ControllerConfig cfg;   // enabled = false, the shipped default
    Controller c{cfg, kMinFee, kMaxFee};
    EXPECT_FALSE(c.enabled());
    for (const std::uint64_t legacy : {0ULL, 1ULL, 3'500'000ULL, 999'999'999ULL}) {
        EXPECT_EQ(c.fee_for(ActionClass::Take, 100, legacy), legacy);
        EXPECT_EQ(c.fee_for(ActionClass::CancelXch, 100, legacy), legacy);
    }
    EXPECT_EQ(c.observe(hard(Signal::ForceDelete, 100)).reason, ChangeReason::Disabled);
    EXPECT_EQ(c.set_feed_forward(9.0, 9.0, 100).reason, ChangeReason::Disabled);
    EXPECT_DOUBLE_EQ(c.level(), 0.0);
    EXPECT_EQ(c.fee_for(ActionClass::Take, 100, 7ULL), 7ULL);
}

TEST(FeeController, FeeNeverLeavesTheConfiguredBand)
{
    Controller c{on_config(), kMinFee, 100'000'000ULL};
    // Drive it as hard as it goes.
    const std::uint32_t now = lift(c, 200, 1'000);
    EXPECT_DOUBLE_EQ(c.level(), c.level_hi());
    for (const ActionClass cls : {ActionClass::OfferAttached, ActionClass::CancelXch,
                                  ActionClass::CancelCat, ActionClass::Take}) {
        EXPECT_LE(c.fee_for(cls, now), 100'000'000ULL);
        EXPECT_GE(c.fee_for(cls, now), kMinFee);
    }
    // At the top of the band even the CHEAPEST class is pinned at max_fee:
    // above it the level would have no authority, which is why it stops here.
    EXPECT_EQ(c.fee_for(ActionClass::CancelXch, now), 100'000'000ULL);
    EXPECT_TRUE(c.saturated(ActionClass::Take, now));
}

TEST(FeeController, AntiWindupTheLevelStopsAtTheBandAndComesStraightBack)
{
    Controller c{on_config(), kMinFee, 100'000'000ULL};
    std::uint32_t now = lift(c, 500, 1'000);
    const double top = c.level();
    EXPECT_DOUBLE_EQ(top, c.level_hi());
    // 500 late spends past the rail accumulated NOTHING: the first probe
    // after a run of on-target confirmations moves the level down at once.
    for (std::uint32_t i = 0; i < c.config().probe_after_confirmations; ++i) {
        c.observe(confirmed(3.0, c.level(), ++now));
    }
    EXPECT_LT(c.level(), top);
    EXPECT_TRUE(c.probing());
}

TEST(FeeController, LevelOfInvertsFeeForAndReadsClampsHonestly)
{
    const Controller c{on_config(), kMinFee, kMaxFee};
    EXPECT_NEAR(c.level_of(kMinFee, ActionClass::CancelCat), 0.0, 1e-9);
    EXPECT_NEAR(c.level_of(2 * kMinFee, ActionClass::CancelCat), 1.0, 1e-9);
    // An XCH cancel clamped UP to min_fee was really submitted ABOVE the level:
    // 15M over 8.4M cost is 5.04x the anchor rate.
    EXPECT_NEAR(c.level_of(kMinFee, ActionClass::CancelXch), std::log2(42.3 / 8.4), 1e-9);
    EXPECT_DOUBLE_EQ(c.level_of(0, ActionClass::Take), -Controller::kLevelAbsMax);
}

// ===========================================================================
// 2. Error, warm-up, guards
// ===========================================================================

TEST(FeeController, OnTargetConfirmationMovesNothing)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    const auto ch = c.observe(confirmed(3.0, 0.0, 100));
    EXPECT_EQ(ch.reason, ChangeReason::OnTarget);
    EXPECT_FALSE(ch.moved);
    // Exactly AT the target is on target: late means strictly past it.
    EXPECT_EQ(c.observe(confirmed(static_cast<double>(kT), 0.0, 101)).reason, ChangeReason::OnTarget);
    EXPECT_DOUBLE_EQ(c.level(), 0.0);
}

TEST(FeeController, PendingInsideTheTargetIsNotEvidence)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    const auto ch = c.observe(pending(static_cast<double>(kT), 0.0, 100));
    EXPECT_EQ(ch.reason, ChangeReason::None);
    EXPECT_DOUBLE_EQ(c.level(), 0.0);
    EXPECT_EQ(c.on_target_run(), 0U);   // and it is not a confirmation either
}

TEST(FeeController, CensoredObservationRaisesBeforeAnyConfirmationArrives)
{
    // The deadlock the design exists to break: a fee below the floor may never
    // confirm.  Not one Confirmed observation is fed here.
    Controller c{on_config(), kMinFee, kMaxFee};
    const std::uint64_t before = c.fee_for(ActionClass::CancelCat, 100);
    const auto ch = c.observe(pending(12.0, 0.0, 112));   // half a target late
    EXPECT_EQ(ch.reason, ChangeReason::CensoredDelay);
    EXPECT_TRUE(ch.moved);
    // e = 0.5: up = kp*0.5 + ki*0.5 + kd*0.5 = 1.0 at the default gains.
    EXPECT_NEAR(c.level(), 1.0, 1e-12);
    EXPECT_EQ(c.fee_for(ActionClass::CancelCat, 112), 2 * before);
}

TEST(FeeController, LateConfirmationRaisesInProportionToLateness)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    c.observe(confirmed(10.0, 0.0, 100));                 // e = 0.25
    EXPECT_NEAR(c.level(), 0.25 * (1.0 + 0.5 + 0.5), 1e-12);
}

TEST(FeeController, VelocityFormIntegralPersistsAndProportionalDoesNotRepeat)
{
    ControllerConfig cfg = on_config();
    cfg.min_raise   = 8.0;    // keep the ANSWERED rule out of this test
    cfg.max_step_up = 8.0;
    Controller c{cfg, kMinFee, kMaxFee};
    // The same error twice: the second tick adds ONLY ki*e (d1 = 0, d2 < 0).
    c.observe(pending(12.0, 0.0, 100));                    // e = 0.5 -> +1.0
    const double after_first = c.level();
    c.observe(pending(12.0, 0.0, 101));                    // e = 0.5 -> +0.25
    EXPECT_NEAR(c.level() - after_first, 0.5 * 0.5, 1e-12);
    // A FALLING error never lowers the level: negative increments are dropped.
    const double before_fall = c.level();
    c.observe(pending(9.0, 0.0, 102));                     // e = 0.125
    EXPECT_GT(c.level(), before_fall);
    EXPECT_NEAR(c.level() - before_fall, 0.5 * 0.125, 1e-12);
}

TEST(FeeController, DerivativeAddsOnAnAcceleratingError)
{
    ControllerConfig with_d = on_config();
    with_d.min_raise = 8.0;  with_d.max_step_up = 8.0;
    ControllerConfig no_d = with_d;
    no_d.kd = 0.0;
    Controller a{with_d, kMinFee, kMaxFee};
    Controller b{no_d, kMinFee, kMaxFee};
    for (Controller* c : {&a, &b}) {
        c->observe(pending(9.0, 0.0, 100));    // e = 0.125
        c->observe(pending(10.0, 0.0, 101));   // e = 0.25  (d2 = 0)
        c->observe(pending(14.0, 0.0, 102));   // e = 0.75  (d2 = +0.375)
    }
    // First tick d2 = 0.125, third tick d2 = 0.375; kd = 0.5.
    EXPECT_NEAR(a.level() - b.level(), 0.5 * (0.125 + 0.375), 1e-12);
}

TEST(FeeController, OneObservationCannotRaiseMoreThanMaxStepUp)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    c.observe(hard(Signal::ForceDelete, 100));   // e = 2: raw up = 4.0
    EXPECT_DOUBLE_EQ(c.level(), 1.0);            // capped at max_step_up
}

TEST(FeeController, WarmUpConsumesObservationsWithoutMovingTheLevel)
{
    Controller c{on_config(/*warmup=*/3), kMinFee, kMaxFee};
    EXPECT_FALSE(c.is_warm());
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto ch = c.observe(hard(Signal::ForceDelete, 100 + 50 * i));
        EXPECT_EQ(ch.reason, ChangeReason::WarmUp);
        EXPECT_FALSE(ch.moved);
    }
    EXPECT_DOUBLE_EQ(c.level(), 0.0);
    EXPECT_TRUE(c.is_warm());
    // The fourth is heard.  The history moved during warm-up (e was already at
    // max), so there is no proportional kick left: only ki * e, capped.
    c.observe(hard(Signal::ForceDelete, 400));
    EXPECT_DOUBLE_EQ(c.level(), 1.0);
}

TEST(FeeController, NonFiniteAndNegativeInputsAreRefused)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(c.observe(pending(nan, 0.0, 100)).reason, ChangeReason::Invalid);
    EXPECT_EQ(c.observe(pending(inf, 0.0, 100)).reason, ChangeReason::Invalid);
    EXPECT_EQ(c.observe(pending(-1.0, 0.0, 100)).reason, ChangeReason::Invalid);
    EXPECT_EQ(c.observe(confirmed(20.0, nan, 100)).reason, ChangeReason::Invalid);
    EXPECT_EQ(c.observe(confirmed(20.0, inf, 100)).reason, ChangeReason::Invalid);
    EXPECT_DOUBLE_EQ(c.level(), 0.0);
    EXPECT_TRUE(std::isfinite(c.effective_rate(100)));
    // A NaN feed-forward is "the node did not say", not a floor of NaN.
    c.set_feed_forward(nan, -inf, 100);
    EXPECT_DOUBLE_EQ(c.feed_forward_rate(100), 0.0);
    EXPECT_EQ(c.fee_for(ActionClass::CancelCat, 100), kMinFee);
}

TEST(FeeController, OverflowSaturatesInsteadOfWrapping)
{
    EXPECT_EQ(fee::fee_from_rate(1e300, 1'000'000'000ULL), fee::kFeeCeiling);
    EXPECT_EQ(fee::fee_from_rate(std::numeric_limits<double>::infinity(), 5), 0U);
    EXPECT_EQ(fee::fee_from_rate(std::numeric_limits<double>::quiet_NaN(), 5), 0U);
    EXPECT_EQ(fee::fee_from_rate(-1.0, 5), 0U);
    EXPECT_EQ(fee::fee_from_rate(1.0, 0), 0U);
    EXPECT_EQ(fee::fee_from_rate(0.3, 10), 3U);
    EXPECT_EQ(fee::fee_from_rate(0.31, 10), 4U);   // ceil: never below the requirement
    // A max_fee above 2^63 is clamped to it, so no fee can exceed it.
    Controller c{on_config(), 1, std::numeric_limits<std::uint64_t>::max()};
    EXPECT_EQ(c.max_fee(), fee::kFeeCeiling);
    c.set_feed_forward(1e300, 0.0, 100);
    EXPECT_EQ(c.fee_for(ActionClass::Take, 100), fee::kFeeCeiling);
}

TEST(FeeController, OutOfRangeTuningFallsBackToTheDefaults)
{
    ControllerConfig bad = on_config();
    bad.kp = std::numeric_limits<double>::quiet_NaN();
    bad.ki = -1.0;
    bad.max_error = 0.0;
    bad.probe_fraction = 1.5;
    bad.target_delay_blocks = 0;
    bad.costs.cancel_cat = 0;
    const Controller c{bad, kMinFee, kMaxFee};
    const ControllerConfig d{};
    EXPECT_DOUBLE_EQ(c.config().kp, d.kp);
    EXPECT_DOUBLE_EQ(c.config().ki, d.ki);
    EXPECT_DOUBLE_EQ(c.config().max_error, d.max_error);
    EXPECT_DOUBLE_EQ(c.config().probe_fraction, d.probe_fraction);
    EXPECT_EQ(c.config().target_delay_blocks, d.target_delay_blocks);
    EXPECT_EQ(c.config().costs.cancel_cat, d.costs.cancel_cat);
}

TEST(FeeController, SwappedBoundsAndZeroMinFeeStayWellDefined)
{
    const Controller swapped{on_config(), /*min=*/100'000'000ULL, /*max=*/5'000'000ULL};
    EXPECT_EQ(swapped.min_fee(), 5'000'000ULL);
    EXPECT_EQ(swapped.max_fee(), 100'000'000ULL);
    const Controller zero{on_config(), 0, kMaxFee};
    EXPECT_GT(zero.anchor_rate(), 0.0);
    EXPECT_GT(zero.fee_for(ActionClass::CancelCat, 1), 0U);
}

// ===========================================================================
// 3. Which evidence counts: the ANSWERED rule and the dead time
// ===========================================================================

TEST(FeeController, AStuckSpendStopsCountingOnceTheLevelHasAnsweredIt)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    // One spend, submitted at level 0, stuck.  Heard until the level is
    // min_raise (1.0 = x2) above what it was submitted at, then no more.
    std::uint32_t now = 109;
    while (c.level() < c.config().min_raise - Controller::kLevelEps) {
        const auto ch = c.observe(pending(static_cast<double>(now - 100), 0.0, now));
        EXPECT_EQ(ch.reason, ChangeReason::CensoredDelay);
        ++now;
    }
    const double answered_at = c.level();
    EXPECT_GE(answered_at, 1.0 - Controller::kLevelEps);
    EXPECT_LT(answered_at, 1.5);               // and it did not overshoot by much
    EXPECT_EQ(now, 113U);                      // four heights past the target + 1
    for (int i = 0; i < 100; ++i, ++now) {
        EXPECT_EQ(c.observe(pending(static_cast<double>(now - 100), 0.0, now)).reason,
                  ChangeReason::Answered);
    }
    EXPECT_DOUBLE_EQ(c.level(), answered_at);   // 100 more ticks wound up nothing
    // A spend submitted AT the new level that is late too IS new evidence.
    EXPECT_EQ(c.observe(pending(20.0, answered_at, now)).reason, ChangeReason::CensoredDelay);
    EXPECT_GT(c.level(), answered_at);
}

TEST(FeeController, ASaturatedSpendCannotWindTheLevelUp)
{
    // The fee was clamped (max_fee, or the budget) well below the level, so it
    // was submitted BELOW it.  Its lateness is not evidence about the level.
    Controller c{on_config(), kMinFee, kMaxFee};
    lift(c, 2, 100);
    const double level = c.level();
    ASSERT_GT(level, 1.5);
    const double paid_level = level - 1.5;   // what the clamp actually allowed
    for (std::uint32_t i = 0; i < 50; ++i) {
        EXPECT_EQ(c.observe(pending(40.0 + i, paid_level, 300 + i)).reason, ChangeReason::Answered);
    }
    EXPECT_DOUBLE_EQ(c.level(), level);
}

TEST(FeeController, UnattributedSignalsAreIgnoredForTwoTargetsAfterARaise)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    EXPECT_EQ(c.observe(hard(Signal::PendingChangeStuck, 100)).reason, ChangeReason::PendingChange);
    const double level = c.level();
    // The force-delete that follows 90 s later is the SAME stuck episode, and
    // so is one 9 heights on -- where Step 8's actually lands.
    EXPECT_EQ(c.observe(hard(Signal::ForceDelete, 100 + 5)).reason, ChangeReason::DeadTime);
    EXPECT_EQ(c.observe(hard(Signal::ForceDelete, 100 + 9)).reason, ChangeReason::DeadTime);
    EXPECT_EQ(c.observe(hard(Signal::ForceDelete, 100 + 2 * kT - 1)).reason, ChangeReason::DeadTime);
    EXPECT_DOUBLE_EQ(c.level(), level);
    // Two whole targets later it is a new one.
    EXPECT_EQ(c.observe(hard(Signal::ForceDelete, 100 + 2 * kT)).reason, ChangeReason::ForceDelete);
    EXPECT_GT(c.level(), level);
    // An ATTRIBUTED late spend is never subject to it.
    EXPECT_EQ(c.observe(pending(12.0, c.level(), 100 + 2 * kT + 1)).reason, ChangeReason::CensoredDelay);
}

TEST(FeeController, WalletLevelSignalsAloneCannotWalkTheFeeToTheCeiling)
{
    // pending_change can persist for reasons no fee cures.  Three force-deletes
    // in a row are heard (x8); after that they need corroboration.
    Controller c{on_config(), kMinFee, kMaxFee};
    std::uint32_t now = 1'000;
    for (std::uint32_t i = 0; i < Controller::kMaxUncorroboratedRaises; ++i) {
        now += 50;
        EXPECT_EQ(c.observe(hard(Signal::ForceDelete, now)).reason, ChangeReason::ForceDelete);
    }
    EXPECT_DOUBLE_EQ(c.level(), 3.0);
    for (int i = 0; i < 100; ++i) {
        now += 50;
        EXPECT_EQ(c.observe(hard(Signal::ForceDelete, now)).reason, ChangeReason::Uncorroborated);
    }
    EXPECT_DOUBLE_EQ(c.level(), 3.0);
    // A spend of ours, submitted at this level and late, re-arms them ...
    now += 50;
    EXPECT_EQ(c.observe(pending(12.0, c.level(), now)).reason, ChangeReason::CensoredDelay);
    now += 50;
    EXPECT_EQ(c.observe(hard(Signal::ForceDelete, now)).reason, ChangeReason::ForceDelete);
    // ... and so does one that confirmed on target at it.
    Controller d{on_config(), kMinFee, kMaxFee};
    now = 1'000;
    for (int i = 0; i < 3; ++i) { now += 50; d.observe(hard(Signal::MempoolRejected, now)); }
    now += 50;
    ASSERT_EQ(d.observe(hard(Signal::MempoolRejected, now)).reason, ChangeReason::Uncorroborated);
    d.observe(confirmed(3.0, d.level(), ++now));
    now += 50;
    EXPECT_EQ(d.observe(hard(Signal::MempoolRejected, now)).reason, ChangeReason::SentToError);
}

TEST(FeeController, HardSignalsReportMaxErrorAndPendingChangeReportsOne)
{
    ControllerConfig cfg = on_config();
    cfg.max_step_up = 8.0;
    Controller a{cfg, kMinFee, kMaxFee};
    Controller b{cfg, kMinFee, kMaxFee};
    Controller d{cfg, kMinFee, kMaxFee};
    a.observe(hard(Signal::ForceDelete, 100));
    b.observe(hard(Signal::MempoolRejected, 100));
    d.observe(hard(Signal::PendingChangeStuck, 100));
    EXPECT_DOUBLE_EQ(a.level(), 2.0 * (1.0 + 0.5 + 0.5));
    EXPECT_DOUBLE_EQ(b.level(), a.level());
    EXPECT_DOUBLE_EQ(d.level(), 1.0 * (1.0 + 0.5 + 0.5));
    EXPECT_EQ(b.observe(hard(Signal::MempoolRejected, 200)).reason, ChangeReason::SentToError);
}

TEST(FeeController, AConfirmationPaidAboveTheLevelSaysNothingAboutTheLevel)
{
    // XCH cancels clamped up to min_fee pay 5x the anchor rate.  Their prompt
    // confirmations must not feed the probe run: they did not test the level.
    Controller c{on_config(), kMinFee, kMaxFee};
    const double paid_above = c.level_of(kMinFee, ActionClass::CancelXch);
    for (std::uint32_t i = 0; i < 100; ++i) {
        EXPECT_EQ(c.observe(confirmed(3.0, paid_above, 100 + i)).reason, ChangeReason::OnTarget);
    }
    EXPECT_EQ(c.on_target_run(), 0U);
    EXPECT_FALSE(c.probing());
}

// ===========================================================================
// 4. The probe schedule
// ===========================================================================

TEST(FeeController, ProbesDownAfterARunOfOnTargetConfirmations)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    std::uint32_t now = lift(c, 3, 1'000);
    const double start = c.level();
    ASSERT_DOUBLE_EQ(start, 3.0);
    for (std::uint32_t i = 0; i + 1 < c.config().probe_after_confirmations; ++i) {
        EXPECT_EQ(c.observe(confirmed(3.0, c.level(), ++now)).reason, ChangeReason::OnTarget);
    }
    EXPECT_FALSE(c.probing());
    const auto ch = c.observe(confirmed(3.0, c.level(), ++now));
    EXPECT_EQ(ch.reason, ChangeReason::ProbeDown);
    EXPECT_TRUE(c.probing());
    EXPECT_DOUBLE_EQ(c.good_level(), start);
    // -15% in fee terms is log2(0.85) in level terms.
    EXPECT_NEAR(c.level(), start + std::log2(0.85), 1e-12);
    EXPECT_NEAR(ch.new_rate / ch.old_rate, 0.85, 1e-9);
}

TEST(FeeController, ASuccessfulProbeBecomesTheNewKnownGoodLevel)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    std::uint32_t now = lift(c, 3, 1'000);
    for (std::uint32_t i = 0; i < c.config().probe_after_confirmations; ++i) {
        c.observe(confirmed(3.0, c.level(), ++now));
    }
    ASSERT_TRUE(c.probing());
    const double probe_level = c.level();
    // Confirmations of spends submitted BEFORE the step do not validate it.
    for (int i = 0; i < 10; ++i) {
        c.observe(confirmed(3.0, 3.0, ++now));
    }
    EXPECT_TRUE(c.probing());
    for (std::uint32_t i = 0; i + 1 < c.config().probe_confirmations; ++i) {
        c.observe(confirmed(3.0, probe_level, ++now));
    }
    EXPECT_TRUE(c.probing());
    EXPECT_EQ(c.observe(confirmed(3.0, probe_level, ++now)).reason, ChangeReason::ProbeSucceeded);
    EXPECT_FALSE(c.probing());
    EXPECT_DOUBLE_EQ(c.good_level(), probe_level);
    EXPECT_EQ(c.probe_interval(), c.config().probe_after_confirmations);
}

TEST(FeeController, AFailedProbeRestoresLastGoodPlusTheBumpAndBacksOff)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    std::uint32_t now = lift(c, 3, 1'000);
    const double good = c.level();
    for (std::uint32_t i = 0; i < c.config().probe_after_confirmations; ++i) {
        c.observe(confirmed(3.0, c.level(), ++now));
    }
    ASSERT_TRUE(c.probing());
    const double probe_level = c.level();
    const auto ch = c.observe(pending(12.0, probe_level, now + 12));
    EXPECT_EQ(ch.reason, ChangeReason::ProbeFailed);
    EXPECT_FALSE(c.probing());
    // Back to last-good times 1.10 -- NOT last-good plus a PID step.
    EXPECT_NEAR(c.level(), good + std::log2(1.10), 1e-12);
    EXPECT_EQ(c.probe_interval(), 2 * c.config().probe_after_confirmations);
    EXPECT_EQ(c.on_target_run(), 0U);
    ASSERT_TRUE(c.bad_level_known());
    EXPECT_DOUBLE_EQ(c.bad_level(), probe_level);
}

/// Drive a controller against a FIXED floor given as a level: a spend at or
/// above it confirms on target, one below it sits until it is heard as late.
/// Returns, for every probe started, {confirmations fed since the last probe
/// ended, 1 if it was a retest of known-bad else 0, 1 if it failed else 0}.
struct ProbeRecord {
    std::uint32_t run{0};
    bool          retest{false};
    bool          failed{false};
};

std::vector<ProbeRecord> drive_fixed_floor(Controller& c, double floor_level,
                                           std::uint32_t observations, std::uint32_t& now)
{
    std::vector<ProbeRecord> out;
    std::uint32_t run = 0;
    for (std::uint32_t i = 0; i < observations; ++i) {
        now += 20;   // every spend is its own episode: outside any dead time
        const bool was_probing = c.probing();
        const bool bad_before  = c.bad_level_known();
        const double bad_level = c.bad_level();
        if (c.level() + Controller::kLevelEps < floor_level) {
            const auto ch = c.observe(pending(12.0, c.level(), now));
            if (ch.reason == ChangeReason::ProbeFailed && !out.empty()) { out.back().failed = true; }
            continue;
        }
        const auto ch = c.observe(confirmed(3.0, c.level(), now));
        ++run;
        if (!was_probing && ch.reason == ChangeReason::ProbeDown) {
            ProbeRecord r;
            r.run    = run;
            r.retest = bad_before && c.level() <= bad_level + Controller::kLevelEps;
            out.push_back(r);
            run = 0;
        }
    }
    return out;
}

TEST(FeeController, AtAFixedFloorRetestsBackOffExponentiallyToTheCap)
{
    ControllerConfig cfg = on_config();
    cfg.probe_after_confirmations = 4;
    cfg.probe_backoff_cap         = 40;
    Controller c{cfg, kMinFee, kMaxFee};
    std::uint32_t now = lift(c, 3, 1'000);          // level 3.0
    const double floor_level = 2.9;                 // just under it
    const auto probes = drive_fixed_floor(c, floor_level, 2'000, now);
    ASSERT_GE(probes.size(), 6U);
    // The first probe explores and fails (3.0 -> 2.77 < 2.9): known-bad 2.77.
    EXPECT_FALSE(probes[0].retest);
    EXPECT_TRUE(probes[0].failed);
    EXPECT_EQ(probes[0].run, 4U);
    // Every LATER failure is a retest, and the runs before retests double up
    // to the cap: a fixed floor is disturbed ever more rarely.
    std::vector<std::uint32_t> retest_runs;
    for (std::size_t i = 1; i < probes.size(); ++i) {
        if (probes[i].retest) {
            EXPECT_TRUE(probes[i].failed) << "probe " << i;
            retest_runs.push_back(probes[i].run);
        } else {
            EXPECT_EQ(probes[i].run, 4U) << "an exploring probe runs at the base interval";
        }
    }
    ASSERT_GE(retest_runs.size(), 4U);
    for (std::size_t i = 1; i < retest_runs.size(); ++i) {
        EXPECT_GE(retest_runs[i], retest_runs[i - 1]);
        EXPECT_LE(retest_runs[i], 40U);
    }
    EXPECT_EQ(retest_runs.back(), 40U);
    // And it never wandered: the level sits within one probe step plus the
    // bump of the floor.
    EXPECT_GE(c.level(), floor_level);
    EXPECT_LT(c.level(), floor_level + (-std::log2(0.85)) + std::log2(1.10) + 1e-9);
}

TEST(FeeController, ASuccessfulRetestForgetsKnownBadAndResetsTheInterval)
{
    ControllerConfig cfg = on_config();
    cfg.probe_after_confirmations = 4;
    cfg.probe_backoff_cap         = 40;
    Controller c{cfg, kMinFee, kMaxFee};
    std::uint32_t now = lift(c, 3, 1'000);
    drive_fixed_floor(c, 2.9, 300, now);
    while (c.probing()) { drive_fixed_floor(c, 2.9, 1, now); }   // let an in-flight probe fail
    ASSERT_TRUE(c.bad_level_known());
    ASSERT_GT(c.probe_interval(), 4U);
    // The mempool drains: the floor falls away.  The next retest succeeds.
    const auto probes = drive_fixed_floor(c, -10.0, 400, now);
    std::size_t first_retest = probes.size();
    for (std::size_t i = 0; i < probes.size(); ++i) {
        if (probes[i].retest) { first_retest = i; break; }
    }
    ASSERT_LT(first_retest, probes.size());
    EXPECT_FALSE(probes[first_retest].failed);
    EXPECT_FALSE(c.bad_level_known());
    EXPECT_EQ(c.probe_interval(), 4U);
    // ... and from there every probe explores at the base interval again: the
    // run before each is the 3 confirmations that made the last one good plus
    // at most the base interval.
    for (std::size_t i = first_retest + 1; i < probes.size(); ++i) {
        EXPECT_FALSE(probes[i].retest) << "probe " << i;
        EXPECT_FALSE(probes[i].failed) << "probe " << i;
        EXPECT_LE(probes[i].run, 4U + cfg.probe_confirmations) << "probe " << i;
    }
    EXPECT_NEAR(c.level(), c.level_lo(), 1e-9);      // it walked all the way down
}

TEST(FeeController, ARaiseWithoutAProbeForgetsKnownBad)
{
    // The floor rose past the level: what was known-bad below it is moot, and
    // keeping it would make every later probe a back-off retest.
    Controller c{on_config(), kMinFee, kMaxFee};
    std::uint32_t now = lift(c, 3, 1'000);
    drive_fixed_floor(c, 2.9, 40, now);
    ASSERT_TRUE(c.bad_level_known());
    ASSERT_FALSE(c.probing());
    now += 50;
    c.observe(pending(16.0, c.level(), now));
    EXPECT_FALSE(c.bad_level_known());
}

/// Lift to level 3, run one probe and fail it.  Returns the next free height;
/// on return the level is last-good (3.0) plus the bump and known-bad is the
/// failed probe level.
std::uint32_t fail_one_probe(Controller& c, double& probe_level_out)
{
    std::uint32_t now = lift(c, 3, 1'000);
    for (std::uint32_t i = 0; i < c.config().probe_after_confirmations; ++i) {
        c.observe(confirmed(3.0, c.level(), ++now));
    }
    probe_level_out = c.level();
    now += 12;
    c.observe(pending(12.0, probe_level_out, now));
    return now + 50;
}

TEST(FeeController, TheBumpIsGivenBackBeforeAnyRetest)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    double probe_level = 0.0;
    std::uint32_t now = fail_one_probe(c, probe_level);
    ASSERT_NEAR(c.level(), 3.0 + std::log2(1.10), 1e-12);
    // The next step down is to the level ALREADY verified good -- not 15%
    // below the bumped level, and not a retest: it runs at the base interval
    // although the retest interval has doubled.
    ASSERT_EQ(c.probe_interval(), 2 * c.config().probe_after_confirmations);
    for (std::uint32_t i = 0; i + 1 < c.config().probe_after_confirmations; ++i) {
        EXPECT_EQ(c.observe(confirmed(3.0, c.level(), ++now)).reason, ChangeReason::OnTarget);
    }
    EXPECT_EQ(c.observe(confirmed(3.0, c.level(), ++now)).reason, ChangeReason::ProbeDown);
    EXPECT_NEAR(c.level(), 3.0, 1e-12);
    EXPECT_GT(c.level(), probe_level);
}

TEST(FeeController, ASecondFailureRestoresToTheSamePlaceTheBumpNeverCompounds)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    double probe_level = 0.0;
    std::uint32_t now = fail_one_probe(c, probe_level);
    const double restored = c.level();
    // Give the bump back (a probe to 3.0) and have THAT fail too: the floor
    // rose a little.  Last-good is still 3.0, so the level returns to exactly
    // where the first failure put it, not 10% higher again.
    for (std::uint32_t i = 0; i < c.config().probe_after_confirmations; ++i) {
        c.observe(confirmed(3.0, c.level(), ++now));
    }
    ASSERT_TRUE(c.probing());
    ASSERT_NEAR(c.level(), 3.0, 1e-12);
    now += 12;
    EXPECT_EQ(c.observe(pending(12.0, c.level(), now)).reason, ChangeReason::ProbeFailed);
    EXPECT_NEAR(c.level(), restored, 1e-12);
    EXPECT_NEAR(c.bad_level(), 3.0, 1e-12);
}

TEST(FeeController, WhileProbingOnlyASpendThatPaidTheProbeLevelCanFailIt)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    std::uint32_t now = lift(c, 3, 1'000);
    for (std::uint32_t i = 0; i < c.config().probe_after_confirmations; ++i) {
        c.observe(confirmed(3.0, c.level(), ++now));
    }
    ASSERT_TRUE(c.probing());
    const double probe_level = c.level();
    // A spend stuck from long before, far below the probe level: "1.5 is too
    // low" says nothing about 2.77.  (It is not "answered" by min_raise either
    // here -- 2.77 - 1.9 < 1.0 -- which is what made this a bug.)
    EXPECT_EQ(c.observe(pending(40.0, 1.9, now + 1)).reason, ChangeReason::Answered);
    EXPECT_TRUE(c.probing());
    EXPECT_DOUBLE_EQ(c.level(), probe_level);
    // One that paid the probe level, or MORE, and is stuck does fail it.
    EXPECT_EQ(c.observe(pending(12.0, 3.0, now + 2)).reason, ChangeReason::ProbeFailed);
}

TEST(FeeController, SpendsLeftStuckAtAFailedProbeLevelDoNotPushTheRestoredLevel)
{
    // A failed probe leaves two or three spends stuck at the probe level.  The
    // first to speak fails it; the others must not then drive the restored
    // level up to min_raise above the PROBE level (x2.9 swing, measured).
    Controller c{on_config(), kMinFee, kMaxFee};
    double probe_level = 0.0;
    std::uint32_t now = fail_one_probe(c, probe_level);
    const double restored = c.level();
    ASSERT_LT(restored, probe_level + c.config().min_raise);   // min_raise alone would not answer it
    for (std::uint32_t i = 0; i < 40; ++i) {
        EXPECT_EQ(c.observe(pending(13.0 + i, probe_level, ++now)).reason, ChangeReason::Answered);
    }
    EXPECT_DOUBLE_EQ(c.level(), restored);
}

TEST(FeeController, NoProbeBelowTheBottomOfTheBand)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    std::uint32_t now = 1'000;
    for (int i = 0; i < 2'000; ++i) {
        c.observe(confirmed(3.0, c.level(), ++now));
    }
    // It walks down to level_lo (where even a take pays min_fee) and stops:
    // below it no class's fee would change.
    EXPECT_NEAR(c.level(), c.level_lo(), 1e-9);
    EXPECT_EQ(c.fee_for(ActionClass::Take, now), kMinFee);
    EXPECT_EQ(c.fee_for(ActionClass::CancelCat, now), kMinFee);
}

TEST(FeeController, AnUnattributedHardSignalDuringAProbeFailsItAtOnce)
{
    // The dead time protects a RAISE.  After a step DOWN, a wallet-level
    // "stuck" is about spends at the old, higher level or the new, lower one --
    // either way the lower one is too low.
    Controller c{on_config(), kMinFee, kMaxFee};
    std::uint32_t now = lift(c, 3, 1'000);
    const double good = c.level();
    for (std::uint32_t i = 0; i < c.config().probe_after_confirmations; ++i) {
        c.observe(confirmed(3.0, c.level(), ++now));
    }
    ASSERT_TRUE(c.probing());
    EXPECT_EQ(c.observe(hard(Signal::ForceDelete, now + 1)).reason, ChangeReason::ProbeFailed);
    EXPECT_NEAR(c.level(), good + std::log2(1.10), 1e-12);
}

// ===========================================================================
// 5. Feed-forward: a floor, never a multiplier
// ===========================================================================

TEST(FeeController, FeedForwardRaisesTheRateAndNeverLowersTheLearnedLevel)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    const double learned = c.learned_rate();
    // A full mempool: the node refuses below 5 mojos per cost.
    const auto up = c.set_feed_forward(0.37, 5.0, 100);
    EXPECT_EQ(up.reason, ChangeReason::FeedForward);
    EXPECT_TRUE(up.moved);
    EXPECT_NEAR(c.effective_rate(100), 5.0 * 1.10, 1e-12);
    // 5.5 mojos/cost x 42.3M = 232.65M for a CAT cancel.
    EXPECT_EQ(c.fee_for(ActionClass::CancelCat, 100), 232'650'000ULL);
    EXPECT_DOUBLE_EQ(c.level(), 0.0);                 // the level did not move
    // The mempool drains: the floor goes, the learned rate stands.
    const auto down = c.set_feed_forward(0.0, 0.0, 110);
    EXPECT_TRUE(down.moved);
    EXPECT_DOUBLE_EQ(c.effective_rate(110), learned);
    EXPECT_EQ(c.fee_for(ActionClass::CancelCat, 110), kMinFee);
}

TEST(FeeController, FeedForwardBelowTheLearnedRateChangesNothing)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    const auto ch = c.set_feed_forward(0.01, 0.0, 100);
    EXPECT_FALSE(ch.moved);
    EXPECT_EQ(c.fee_for(ActionClass::CancelCat, 100), kMinFee);
}

TEST(FeeController, AnUnreachableNodeAgesTheFloorOutAndTheLevelCarriesOn)
{
    Controller c{on_config(), kMinFee, kMaxFee};
    c.set_feed_forward(0.0, 5.0, 100);
    const std::uint32_t max_age = c.config().ff_max_age_blocks;
    EXPECT_GT(c.feed_forward_rate(100 + max_age), 0.0);
    EXPECT_DOUBLE_EQ(c.feed_forward_rate(100 + max_age + 1), 0.0);
    EXPECT_EQ(c.fee_for(ActionClass::CancelCat, 100 + max_age + 1), kMinFee);
    // No node at all, ever: the loop still raises on its own evidence.
    Controller alone{on_config(), kMinFee, kMaxFee};
    alone.observe(pending(16.0, 0.0, 200));
    EXPECT_GT(alone.fee_for(ActionClass::CancelCat, 200), kMinFee);
    // A height regression never makes a reading look fresh forever or wrap.
    EXPECT_GT(c.feed_forward_rate(50), 0.0);
    c.clear_feed_forward();
    EXPECT_DOUBLE_EQ(c.feed_forward_rate(100), 0.0);
}

// ===========================================================================
// 6. Budget, sent_to, cancel class, tickets, log gate, reachability
// ===========================================================================

TEST(FeeBudget, CancelsKeepTheReserveAndNothingIsEverZero)
{
    // Headroom 1,000; 900 of it is held for cancels.
    EXPECT_EQ(fee::apply_budget(200, 1'000, 900, 10, /*priority=*/true).fee, 200U);
    EXPECT_FALSE(fee::apply_budget(200, 1'000, 900, 10, true).bound);
    const auto attached = fee::apply_budget(200, 1'000, 900, 10, /*priority=*/false);
    EXPECT_EQ(attached.fee, 100U);
    EXPECT_TRUE(attached.bound);
    // Exhausted: both degrade to min_fee.  NOT zero -- zero is "skip Step 8".
    EXPECT_EQ(fee::apply_budget(200, 0, 900, 10, true).fee, 10U);
    EXPECT_EQ(fee::apply_budget(200, 0, 900, 10, false).fee, 10U);
    EXPECT_TRUE(fee::apply_budget(200, 0, 900, 10, true).bound);
    // The reserve larger than the headroom does not underflow.
    EXPECT_EQ(fee::apply_budget(200, 500, 900, 10, false).fee, 10U);
    // The floor never RAISES a fee above what was asked for.
    EXPECT_EQ(fee::apply_budget(5, 0, 0, 10, true).fee, 5U);
}

TEST(FeeFeedback, SentToIsReadFromItsLatestEntryOnly)
{
    using nlohmann::json;
    const auto row = [](json sent_to) { return json{{"confirmed", false}, {"sent_to", std::move(sent_to)}}; };
    // Verbatim shape from the live wallet log, 2026-09-20.
    EXPECT_TRUE(xop::execution::latest_sent_to_is_fee_rejection(
        row(json::array({json::array({"ec9e", 3, "INVALID_FEE_TOO_CLOSE_TO_ZERO"})}))));
    EXPECT_TRUE(xop::execution::latest_sent_to_is_fee_rejection(
        row(json::array({json::array({"ec9e", 3, "INVALID_FEE_LOW_FEE"})}))));
    // Refused, then accepted: it is in the mempool now.
    EXPECT_FALSE(xop::execution::latest_sent_to_is_fee_rejection(
        row(json::array({json::array({"a", 3, "INVALID_FEE_LOW_FEE"}), json::array({"a", 1, nullptr})}))));
    // Some other failure is not a fee signal.
    EXPECT_FALSE(xop::execution::latest_sent_to_is_fee_rejection(
        row(json::array({json::array({"a", 3, "MEMPOOL_CONFLICT"})}))));
    // A fee error name on a row that did not FAIL is not one either.
    EXPECT_FALSE(xop::execution::latest_sent_to_is_fee_rejection(
        row(json::array({json::array({"a", 2, "INVALID_FEE_LOW_FEE"})}))));
    // Unreadable is not evidence.
    EXPECT_FALSE(xop::execution::latest_sent_to_is_fee_rejection(row(json::array())));
    EXPECT_FALSE(xop::execution::latest_sent_to_is_fee_rejection(json{{"confirmed", false}}));
    EXPECT_FALSE(xop::execution::latest_sent_to_is_fee_rejection(row(json::array({json::array({"a", 3})}))));
    EXPECT_FALSE(xop::execution::latest_sent_to_is_fee_rejection(row(json::array({json::array({"a", "3", "INVALID_FEE_LOW_FEE"})}))));
    EXPECT_FALSE(xop::execution::latest_sent_to_is_fee_rejection(row("nope")));
    EXPECT_FALSE(xop::execution::latest_sent_to_is_fee_rejection(json::array()));
}

TEST(FeeCancelClass, FollowsTheOfferedAssetAndIsInertWithoutTheOverride)
{
    EXPECT_EQ(fee::cancel_class(true), ActionClass::CancelXch);
    EXPECT_EQ(fee::cancel_class(false), ActionClass::CancelCat);
    // No override: the legacy fee, whatever the offer is.
    EXPECT_EQ(fee::cancel_fee_for(false, 7, 1, 2, true, true), 7U);
    EXPECT_EQ(fee::cancel_fee_for(false, 7, 1, 2, false, false), 7U);
    EXPECT_EQ(fee::cancel_fee_for(true, 7, 1, 2, true, true), 1U);
    EXPECT_EQ(fee::cancel_fee_for(true, 7, 1, 2, true, false), 2U);
    // Unknown offer: the larger fee.
    EXPECT_EQ(fee::cancel_fee_for(true, 7, 1, 2, false, true), 2U);
    EXPECT_EQ(fee::cancel_fee_for(true, 7, 9, 2, false, false), 9U);
}

TEST(FeeTicket, PendingOncePerHeightPastTheTargetAndNoWrapOnARegression)
{
    fee::Ticket t;
    t.submit_block = 1'000;
    EXPECT_FALSE(fee::pending_observation_due(t, 1'000 + kT, kT));   // AT the target: not late
    EXPECT_TRUE(fee::pending_observation_due(t, 1'001 + kT, kT));
    t.last_pending_block = 1'001 + kT;
    EXPECT_FALSE(fee::pending_observation_due(t, 1'001 + kT, kT));   // same height: spoken
    EXPECT_TRUE(fee::pending_observation_due(t, 1'002 + kT, kT));
    t.awaiting_verdict = true;
    EXPECT_FALSE(fee::pending_observation_due(t, 1'100, kT));        // left the pending set
    // Height regression: age 0, delay 0 -- never a wrapped 4-billion.
    EXPECT_EQ(fee::ticket_age(t, 900), 0U);
    EXPECT_EQ(fee::confirmation_delay(t, 900), 0U);
    EXPECT_EQ(fee::confirmation_delay(t, 1'005), 5U);
    t.left_block = 2'000;
    EXPECT_FALSE(fee::verdict_expired(t, 2'000 + fee::kVerdictTtlBlocks));
    EXPECT_TRUE(fee::verdict_expired(t, 2'001 + fee::kVerdictTtlBlocks));
    EXPECT_FALSE(fee::verdict_expired(t, 1'500));                    // regression: not expired
}

TEST(FeeChangeLog, FoldsABurstIntoOneLineAndLetsHardReasonsThrough)
{
    fee::ChangeLogGate gate;
    fee::Change ch;
    ch.moved = true; ch.old_rate = 1.0; ch.new_rate = 1.1; ch.reason = ChangeReason::CensoredDelay;
    EXPECT_TRUE(gate.note(ch, 100));            // the first move is always due
    gate.emitted(100);
    ch.old_rate = 1.1; ch.new_rate = 1.2;
    EXPECT_FALSE(gate.note(ch, 101));           // inside the gap: folded
    ch.old_rate = 1.2; ch.new_rate = 1.3;
    EXPECT_FALSE(gate.note(ch, 102));
    ch.old_rate = 1.3; ch.new_rate = 1.4;
    EXPECT_TRUE(gate.note(ch, 104));            // gap passed
    EXPECT_DOUBLE_EQ(gate.first_old_rate, 1.1); // the line covers the whole burst
    EXPECT_DOUBLE_EQ(gate.latest_new_rate, 1.4);
    EXPECT_EQ(gate.folded, 3U);
    gate.emitted(104);
    ch.reason = ChangeReason::ForceDelete;
    EXPECT_TRUE(gate.note(ch, 105));            // hard: due at once
    gate.emitted(105);
    ch.moved = false;
    EXPECT_FALSE(gate.note(ch, 200));           // nothing moved: nothing to say
}

TEST(FeeReachability, TheLiveMaxFeeCannotClearAFullMempoolForCatSpends)
{
    // Live 2026-09-20: min 15M, max 100M.  100M / 42.3M = 2.36 < 5.
    const auto live = fee::reachability(on_config(), 15'000'000ULL, 100'000'000ULL);
    EXPECT_FALSE(live.cannot_raise);
    EXPECT_FALSE(live.max_fee_below_full_mempool[static_cast<std::size_t>(ActionClass::CancelXch)]);
    EXPECT_TRUE(live.max_fee_below_full_mempool[static_cast<std::size_t>(ActionClass::OfferAttached)]);
    EXPECT_TRUE(live.max_fee_below_full_mempool[static_cast<std::size_t>(ActionClass::CancelCat)]);
    EXPECT_TRUE(live.max_fee_below_full_mempool[static_cast<std::size_t>(ActionClass::Take)]);
    // 5 x 1.10 x 125M: what max_fee_mojos must be for a take to get in.
    EXPECT_EQ(live.max_fee_for_full_mempool, 687'500'000ULL);
    const auto roomy = fee::reachability(on_config(), 15'000'000ULL, 1'000'000'000ULL);
    for (std::size_t i = 0; i < fee::kActionClassCount; ++i) {
        EXPECT_FALSE(roomy.max_fee_below_full_mempool[i]);
    }
    // Band [-1.56, +8.31] at these bounds; hard signals raise 1.0 each.
    EXPECT_EQ(roomy.raises_to_span, 10U);
}

TEST(FeeReachability, GainsThatCannotSpanTheBandAreReported)
{
    ControllerConfig dead = on_config();
    dead.kp = 0.0; dead.ki = 0.0; dead.kd = 0.0;
    EXPECT_TRUE(fee::reachability(dead, kMinFee, kMaxFee).cannot_raise);
    // ki = 0: one proportional kick on the first signal, then a sustained
    // error adds nothing -- the band cannot be crossed.
    ControllerConfig no_i = on_config();
    no_i.ki = 0.0;
    EXPECT_TRUE(fee::reachability(no_i, kMinFee, kMaxFee).cannot_raise);
    Controller c{no_i, kMinFee, kMaxFee};
    std::uint32_t now = 1'000;
    for (int i = 0; i < 50; ++i) { now += 50; c.observe(pending(24.0, c.level(), now)); }
    EXPECT_DOUBLE_EQ(c.level(), 1.0);   // the simulation agrees with the algebra
}

// ===========================================================================
// 7. Against a simulated hidden floor
// ===========================================================================

/// The threshold plant.  One spend is submitted every `submit_every` peak
/// heights at the controller's current fee.  A pending spend confirms once it
/// is `confirm_delay` heights old AND its rate is at or above the floor AT
/// THAT HEIGHT; below the floor it just sits.  Every height, each pending
/// spend past the target speaks once (the engine's sweep).  A spend stuck for
/// `abandon_after` heights is dropped, as Step 8's force-delete would.
struct FloorSim {
    Controller                           ctl{};
    std::function<double(std::uint32_t)> floor_at{};   // mojos per cost
    ActionClass                          cls{ActionClass::CancelCat};
    std::uint32_t                        now{10'000};
    std::uint32_t                        confirm_delay{3};
    std::uint32_t                        submit_every{4};
    std::uint32_t                        abandon_after{64};
    /// Step 8's force-delete, as a wallet-level signal: fired while any spend
    /// has sat this many heights (live median 176 s = 9 heights), at most
    /// once per that many heights.  Off by default: the censored path alone
    /// is the conservative case.
    bool                                 force_delete_signals{false};
    std::uint32_t                        force_delete_after{9};
    std::uint32_t                        last_force_delete{0};

    struct Spend {
        std::uint32_t submit_block{0};
        double        rate{0.0};
        double        submit_level{0.0};
        bool          wiped{false};   ///< a force-delete took it out of the wallet
    };
    struct Sample {
        std::uint32_t block{0};
        double        rate{0.0};
        double        floor{0.0};
    };
    std::vector<Spend>  in_flight{};
    std::vector<Sample> submitted{};    // one per action, in order
    std::uint32_t       late_or_stuck{0};
    std::uint64_t       min_fee_seen{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t       max_fee_seen{0};

    void step()
    {
        ++now;
        if (now % submit_every == 0U) {
            const std::uint64_t f = ctl.fee_for(cls, now);
            min_fee_seen = std::min(min_fee_seen, f);
            max_fee_seen = std::max(max_fee_seen, f);
            const double cost = static_cast<double>(fee::cost_of(ctl.config().costs, cls));
            Spend s;
            s.submit_block = now;
            s.rate         = static_cast<double>(f) / cost;
            s.submit_level = ctl.level_of(f, cls);
            in_flight.push_back(s);
            submitted.push_back(Sample{now, s.rate, floor_at(now)});
        }
        std::vector<Spend> still;
        for (const Spend& s : in_flight) {
            const std::uint32_t age = now - s.submit_block;
            if (age >= confirm_delay && s.rate >= floor_at(now)) {
                if (age > ctl.config().target_delay_blocks) { ++late_or_stuck; }
                ctl.observe(confirmed(static_cast<double>(age), s.submit_level, now));
                continue;
            }
            if (age > abandon_after) {
                ++late_or_stuck;
                continue;
            }
            if (age > ctl.config().target_delay_blocks) {
                ctl.observe(pending(static_cast<double>(age), s.submit_level, now));
            }
            still.push_back(s);
        }
        in_flight = std::move(still);
        if (force_delete_signals && now - last_force_delete >= force_delete_after) {
            bool fire = false;
            for (const Spend& s : in_flight) {
                if (!s.wiped && now - s.submit_block >= force_delete_after) { fire = true; }
            }
            if (fire) {
                // delete_unconfirmed_transactions is wallet-wide: every spend in
                // flight leaves the wallet (so pending_change clears), while
                // the engine's tickets for them live on.
                for (Spend& s : in_flight) { s.wiped = true; }
                last_force_delete = now;
                ctl.observe(hard(Signal::ForceDelete, now));
            }
        }
    }

    void run(std::uint32_t heights) { for (std::uint32_t i = 0; i < heights; ++i) { step(); } }

    /// Index of the first action submitted at or above the floor at/after
    /// action `from`; submitted.size() when none.
    [[nodiscard]] std::size_t first_adequate(std::size_t from) const
    {
        for (std::size_t i = from; i < submitted.size(); ++i) {
            if (submitted[i].rate >= submitted[i].floor) { return i; }
        }
        return submitted.size();
    }
};

FloorSim make_sim(std::function<double(std::uint32_t)> floor_fn)
{
    FloorSim sim;
    sim.ctl      = Controller{on_config(), kMinFee, kMaxFee};
    sim.floor_at = std::move(floor_fn);
    return sim;
}

/// The anchor rate at these bounds: 15M / 42.3M = 0.3546 mojos per cost.
constexpr double kAnchor = 15'000'000.0 / 42'300'000.0;

TEST(SimulatedFloor, ConvergesAboveAFixedFloorWithinFourteenActions)
{
    // Floor 5.0 mojos/cost -- a full mempool -- against a start of 0.355: a
    // 14x gap, 3.8 doublings, with NO feed-forward to help.
    FloorSim sim = make_sim([](std::uint32_t) { return 5.0; });
    sim.run(400);
    const std::size_t n = sim.first_adequate(0);
    ASSERT_LT(n, sim.submitted.size());
    // Measured: 12 actions, 48 peak heights (15 min).  Each raise is x2 (the
    // ANSWERED margin) and the next cannot come until a spend at the new level
    // has itself run past the target: that dead time, not the gains, sets the
    // pace.  The node's floor, when it is reachable, makes it 0 actions -- see
    // TheNodeFloorMakesTheJumpCostNothing.
    EXPECT_LE(n, 14U) << "actions submitted below the floor before the first adequate one";
    EXPECT_LE(sim.submitted[n].block - sim.submitted[0].block, 64U);
    // THE OVERSHOOT, stated honestly.  Spends submitted part-way up the ramp
    // are stuck too, and each asks for min_raise above what IT paid, so the
    // level can end up to 2 x min_raise above the last level that was too low:
    // x4 in theory.  Measured x2.37 here (0.996 of the floor just missed, then
    // x1.68, then x2.37), walked back 15% per 11 confirmations.
    double worst = 0.0;
    for (const auto& a : sim.submitted) { worst = std::max(worst, a.rate / 5.0); }
    EXPECT_LT(worst, 3.0);
    EXPECT_GT(worst, 1.0);
}

TEST(SimulatedFloor, ForceDeleteSignalsDoNotDoubleCountTheSameStuckEpisode)
{
    // Live, a stuck cancel is heard twice: as a censored ticket AND as Step 8's
    // pending_change / force-delete.  The dead time makes the second voice
    // silent after a raise, so the pair converges exactly like the ticket alone
    // instead of running away.
    FloorSim quiet = make_sim([](std::uint32_t) { return 5.0; });
    FloorSim loud  = make_sim([](std::uint32_t) { return 5.0; });
    loud.force_delete_signals = true;
    quiet.run(40'000);
    loud.run(40'000);
    EXPECT_LE(loud.first_adequate(0), quiet.first_adequate(0));
    double worst_loud = 0.0;
    double worst_quiet = 0.0;
    for (const auto& a : loud.submitted) { worst_loud = std::max(worst_loud, a.rate / 5.0); }
    for (const auto& a : quiet.submitted) { worst_quiet = std::max(worst_quiet, a.rate / 5.0); }
    // The wipes added NOTHING to the overshoot the tickets alone produce.
    EXPECT_LE(worst_loud, worst_quiet * 1.0001);
    EXPECT_LT(worst_loud, 3.0);
}

TEST(SimulatedFloor, SteadyStateOverpaymentIsBounded)
{
    FloorSim sim = make_sim([](std::uint32_t) { return 5.0; });
    sim.run(4'000);                                   // converge and settle
    const std::size_t settle = sim.submitted.size();
    sim.run(40'000);                                  // ~8.7 days of chain time
    double sum = 0.0;
    double worst = 0.0;
    std::size_t below = 0;
    for (std::size_t i = settle; i < sim.submitted.size(); ++i) {
        const double ratio = sim.submitted[i].rate / sim.submitted[i].floor;
        sum += ratio;
        worst = std::max(worst, ratio);
        if (ratio < 1.0) { ++below; }
    }
    const double n    = static_cast<double>(sim.submitted.size() - settle);
    const double mean = sum / n;
    // Measured: mean x1.053, worst x1.157, 1.11% of actions below the floor
    // (the two or three submitted while a retest of known-bad is in flight,
    // once per 256 confirmations).  Bounds carry slack.
    EXPECT_LT(mean, 1.15);
    EXPECT_LT(worst, 1.30);
    EXPECT_GE(mean, 1.0);
    EXPECT_LT(static_cast<double>(below) / n, 0.02) << "retests are the only actions below the floor";
}

TEST(SimulatedFloor, NoOscillationBeyondTheProbeStepAtAFixedFloor)
{
    FloorSim sim = make_sim([](std::uint32_t) { return 5.0; });
    sim.run(4'000);
    const std::size_t settle = sim.submitted.size();
    sim.run(40'000);
    double lo = std::numeric_limits<double>::infinity();
    double hi = 0.0;
    for (std::size_t i = settle; i < sim.submitted.size(); ++i) {
        lo = std::min(lo, sim.submitted[i].rate);
        hi = std::max(hi, sim.submitted[i].rate);
    }
    // The settled rate moves only by a probe (x0.85) and its undo (last-good
    // x1.10): peak-to-trough is 1.10 / 0.85 = 1.294.  Stated bound 1.35.
    EXPECT_LT(hi / lo, 1.35);
}

TEST(SimulatedFloor, FollowsTheFloorDownByProbing)
{
    // Full mempool for a while, then it drains: the floor falls 10x.
    const std::uint32_t drop_at = 10'000 + 3'000;
    FloorSim sim = make_sim([drop_at](std::uint32_t h) { return h < drop_at ? 5.0 : 0.5; });
    sim.run(3'000);
    const double before = sim.submitted.back().rate;
    ASSERT_GE(before, 5.0);
    const std::size_t at_drop = sim.submitted.size();
    sim.run(3'000);                                   // 15.6 hours of chain time
    const double after = sim.submitted.back().rate;
    // Measured: within x1.45 of the new floor after 224 actions (~15 probes of
    // 15%, 8 + 3 confirmations apart once the first retest has succeeded), and
    // x1.081 at the end.
    EXPECT_LT(after / 0.5, 1.45);
    EXPECT_GE(after, 0.5 * 0.84);                     // a live probe may be one step under
    std::size_t reached = sim.submitted.size();
    for (std::size_t i = at_drop; i < sim.submitted.size(); ++i) {
        if (sim.submitted[i].rate < 0.5 * 1.45) { reached = i; break; }
    }
    ASSERT_LT(reached, sim.submitted.size());
    EXPECT_LE(reached - at_drop, 260U) << "actions to come within x1.45 of a 10x lower floor";
}

TEST(SimulatedFloor, RecoversFromATenfoldJumpWithinTenActions)
{
    const std::uint32_t jump_at = 10'000 + 4'000;
    FloorSim sim = make_sim([jump_at](std::uint32_t h) { return h < jump_at ? 0.5 : 5.0; });
    sim.run(4'000);
    const std::size_t at_jump = sim.submitted.size();
    ASSERT_LT(sim.submitted.back().rate, 0.5 * 1.45);  // settled on the low floor
    sim.run(600);
    const std::size_t ok = sim.first_adequate(at_jump);
    ASSERT_LT(ok, sim.submitted.size());
    // Measured: 8 actions, 32 peak heights (10 min); worst overshoot x1.455.
    EXPECT_LE(ok - at_jump, 10U) << "actions submitted below the new floor";
    // and the overshoot past the new floor is at most one capped step (x2).
    double worst = 0.0;
    for (std::size_t i = ok; i < sim.submitted.size(); ++i) {
        worst = std::max(worst, sim.submitted[i].rate / 5.0);
    }
    EXPECT_LT(worst, 2.0);
}

TEST(SimulatedFloor, TheNodeFloorMakesTheJumpCostNothing)
{
    // Same jump, but the node says its mempool is full the moment it is.
    const std::uint32_t jump_at = 10'000 + 4'000;
    FloorSim sim = make_sim([jump_at](std::uint32_t h) { return h < jump_at ? 0.5 : 5.0; });
    sim.run(3'999);
    const std::size_t at_jump = sim.submitted.size();
    for (int i = 0; i < 600; ++i) {
        sim.ctl.set_feed_forward(0.37, sim.now + 1 >= jump_at ? 5.0 : 0.0, sim.now);
        sim.step();
    }
    EXPECT_EQ(sim.first_adequate(at_jump), at_jump) << "no action was submitted below the floor";
}

TEST(SimulatedFloor, FeesStayInsideMinAndMaxThroughoutEverySimulation)
{
    // A floor no fee can reach: the loop rails at max_fee and stays finite.
    FloorSim sim = make_sim([](std::uint32_t) { return 1e9; });
    sim.run(5'000);
    EXPECT_GE(sim.min_fee_seen, kMinFee);
    EXPECT_LE(sim.max_fee_seen, kMaxFee);
    EXPECT_EQ(sim.max_fee_seen, kMaxFee);
    // ANTI-WINDUP, observed end to end: every stuck spend was submitted AT
    // max_fee, so once the level is min_raise above what max_fee buys it has
    // "answered" them all and stops -- 5,000 heights of failure did not push
    // it to the top of the band, let alone past it.
    const double paid = sim.ctl.level_of(kMaxFee, ActionClass::CancelCat);
    EXPECT_GE(sim.ctl.level(), paid);
    EXPECT_LE(sim.ctl.level(), paid + sim.ctl.config().min_raise + sim.ctl.config().max_step_up);
    EXPECT_LT(sim.ctl.level(), sim.ctl.level_hi());
    // And a floor of nothing: it walks to min_fee and stays.
    FloorSim calm = make_sim([](std::uint32_t) { return 0.0; });
    calm.run(20'000);
    EXPECT_EQ(calm.min_fee_seen, kMinFee);
    EXPECT_EQ(calm.submitted.back().rate * 42'300'000.0, static_cast<double>(kMinFee));
    EXPECT_EQ(calm.late_or_stuck, 0U);
}

TEST(SimulatedFloor, ReportsItsNumbers)
{
    // Not an assertion: the measurements the bounds above were set from, so a
    // retune can be re-measured from the test log (--gtest_filter=*Reports*).
    FloorSim fixed = make_sim([](std::uint32_t) { return 5.0; });
    fixed.run(400);
    const std::size_t conv = fixed.first_adequate(0);
    fixed.run(3'600);
    const std::size_t settle = fixed.submitted.size();
    fixed.run(40'000);
    double sum = 0.0, worst = 0.0, lo = 1e300, hi = 0.0;
    std::size_t below = 0;
    for (std::size_t i = settle; i < fixed.submitted.size(); ++i) {
        const double r = fixed.submitted[i].rate;
        sum += r / 5.0; worst = std::max(worst, r / 5.0);
        lo = std::min(lo, r); hi = std::max(hi, r);
        if (r < 5.0) { ++below; }
    }
    const double n = static_cast<double>(fixed.submitted.size() - settle);
    std::printf("[ S67 sim ] fixed floor 5.0 from %.3f: adequate at action %zu (height +%u); "
                "steady mean x%.3f worst x%.3f, peak/trough x%.3f, below floor %.2f%% of %zu\n",
                kAnchor, conv, fixed.submitted[conv].block - fixed.submitted[0].block,
                sum / n, worst, hi / lo, 100.0 * static_cast<double>(below) / n,
                fixed.submitted.size() - settle);

    FloorSim wiped = make_sim([](std::uint32_t) { return 5.0; });
    wiped.force_delete_signals = true;
    wiped.run(400);
    const std::size_t conv_fd = wiped.first_adequate(0);
    std::printf("[ S67 sim ] the same with Step 8 force-delete signals: adequate at action %zu "
                "(height +%u)\n", conv_fd,
                wiped.submitted[conv_fd].block - wiped.submitted[0].block);

    const std::uint32_t drop_at = 13'000;
    FloorSim down = make_sim([drop_at](std::uint32_t h) { return h < drop_at ? 5.0 : 0.5; });
    down.run(3'000);
    const std::size_t at_drop = down.submitted.size();
    down.run(3'000);
    std::size_t reached = down.submitted.size();
    for (std::size_t i = at_drop; i < down.submitted.size(); ++i) {
        if (down.submitted[i].rate < 0.5 * 1.45) { reached = i; break; }
    }
    std::printf("[ S67 sim ] floor 5.0 -> 0.5: within x1.45 after %zu actions; final x%.3f\n",
                reached - at_drop, down.submitted.back().rate / 0.5);

    const std::uint32_t jump_at = 14'000;
    FloorSim up = make_sim([jump_at](std::uint32_t h) { return h < jump_at ? 0.5 : 5.0; });
    up.run(4'000);
    const std::size_t at_jump = up.submitted.size();
    up.run(600);
    const std::size_t ok = up.first_adequate(at_jump);
    double over = 0.0;
    for (std::size_t i = ok; i < up.submitted.size(); ++i) { over = std::max(over, up.submitted[i].rate / 5.0); }
    std::printf("[ S67 sim ] floor 0.5 -> 5.0: %zu actions below the new floor (%u heights); "
                "worst overshoot x%.3f\n",
                ok - at_jump, up.submitted[ok].block - up.submitted[at_jump].block, over);
    SUCCEED();
}

// ===========================================================================
// 8. FeeTracker with the controller wired in
// ===========================================================================

xop::FeeConfig tracker_config(bool controller_on, bool cost_aware)
{
    xop::FeeConfig cfg;
    cfg.enabled                       = true;
    cfg.adaptive_enabled              = true;
    cfg.min_fee_mojos                 = kMinFee;
    cfg.max_fee_mojos                 = kMaxFee;
    cfg.daily_budget_mojos            = 10'000'000'000ULL;
    cfg.fee_window_blocks             = 1'662;
    cfg.controller_enabled            = controller_on;
    cfg.cost_aware_estimate           = cost_aware;
    cfg.controller_warmup_observations = 0;
    return cfg;
}

constexpr ActionClass kAllClasses[] = {ActionClass::OfferAttached, ActionClass::CancelXch,
                                       ActionClass::CancelCat, ActionClass::Take};

TEST(FeeTrackerController, BothFlagsOffEveryClassGetsTheLegacyFee)
{
    // The shipped default.  The legacy estimate, the clamp, the budget cap and
    // the "0 = skip" all behave as before, for EVERY class.
    xop::FeeTracker t{tracker_config(false, false)};
    EXPECT_FALSE(t.controller_active());
    EXPECT_FALSE(t.wants_rate_estimate());
    t.update_mempool_estimate(37'000'000);
    // A rate reading must not leak into the legacy path either.
    t.update_feed_forward(9.0, 9.0, 100);
    for (const ActionClass cls : kAllClasses) {
        EXPECT_EQ(t.get_recommended_fee(10'000'000, 100, cls), 37'000'000ULL);
    }
    // Observations are refused, tickets are inert.
    Observation o;
    o.signal = Signal::ForceDelete;
    EXPECT_EQ(t.observe(o).reason, ChangeReason::Disabled);
    // Budget exhaustion still returns 0 on the legacy path.
    t.record_fee(10'000'000'000ULL, 100);
    for (const ActionClass cls : kAllClasses) {
        EXPECT_EQ(t.get_recommended_fee(10'000'000, 101, cls), 0ULL);
    }
    EXPECT_FALSE(t.take_budget_bound_alert());
}

TEST(FeeTrackerController, CostAwareEstimateScalesTheNodeRateByClassCost)
{
    xop::FeeTracker t{tracker_config(false, true)};
    EXPECT_TRUE(t.wants_rate_estimate());
    EXPECT_FALSE(t.controller_active());
    // No reading yet: the static fee, clamped -- as the legacy path does.
    EXPECT_EQ(t.get_recommended_fee(20'000'000, 100, ActionClass::Take), 20'000'000ULL);
    t.update_feed_forward(0.373, 0.0, 100);   // the live node's rate, 2026-09-19
    // 0.373 x 42.3M = 15.78M; x 125M = 46.6M; x 8.4M = 3.13M -> clamped to 15M.
    EXPECT_EQ(t.get_recommended_fee(1, 100, ActionClass::CancelCat), 15'777'900ULL);
    EXPECT_EQ(t.get_recommended_fee(1, 100, ActionClass::Take), 46'625'000ULL);
    EXPECT_EQ(t.get_recommended_fee(1, 100, ActionClass::CancelXch), kMinFee);
    // The legacy single estimate is ignored in this mode.
    t.update_mempool_estimate(999'000'000);
    EXPECT_EQ(t.get_recommended_fee(1, 100, ActionClass::Take), 46'625'000ULL);
    // A later reading of 0 keeps the last rate, as Step 1 always skipped est == 0.
    t.update_feed_forward(0.0, 0.0, 101);
    EXPECT_EQ(t.get_recommended_fee(1, 101, ActionClass::Take), 46'625'000ULL);
}

TEST(FeeTrackerController, FeesEnabledFalseWinsOverControllerEnabled)
{
    xop::FeeConfig cfg = tracker_config(true, true);
    cfg.enabled = false;
    xop::FeeTracker t{cfg};
    EXPECT_FALSE(t.controller_active());
    EXPECT_FALSE(t.wants_rate_estimate());
    EXPECT_EQ(t.get_recommended_fee(123, 100, ActionClass::Take), 123ULL);
}

TEST(FeeTrackerController, ControllerOnRaisesOnEvidenceAndIsNeverZero)
{
    xop::FeeTracker t{tracker_config(true, false)};
    ASSERT_TRUE(t.controller_active());
    EXPECT_TRUE(t.wants_rate_estimate());   // implied by the controller
    const std::uint64_t before = t.get_recommended_fee(1, 100, ActionClass::CancelCat);
    EXPECT_EQ(before, kMinFee);
    const fee::Ticket ticket = t.make_ticket(ActionClass::CancelCat, before, 100);
    EXPECT_NEAR(ticket.submit_level, 0.0, 1e-9);
    EXPECT_EQ(ticket.submit_block, 100U);
    Observation o;
    o.signal = Signal::Pending; o.blocks = 12.0; o.attributed = true;
    o.submit_level = ticket.submit_level; o.now = 112;
    EXPECT_TRUE(t.observe(o).moved);
    EXPECT_EQ(t.get_recommended_fee(1, 112, ActionClass::CancelCat), 2 * kMinFee);

    // Exhaust the budget.  Legacy would return 0 = "skip all of Step 8".
    t.record_fee(10'000'000'000ULL, 112);
    for (const ActionClass cls : kAllClasses) {
        EXPECT_EQ(t.get_recommended_fee(1, 113, cls), kMinFee) << fee::to_string(cls);
    }
    // ... and the gate does not refuse every tier on budget grounds.
    EXPECT_TRUE(t.should_post_offer(10'000'000'000ULL, kMinFee, 113));
    // One alert per episode.
    EXPECT_TRUE(t.take_budget_bound_alert());
    EXPECT_FALSE(t.take_budget_bound_alert());
    t.get_recommended_fee(1, 114, ActionClass::CancelCat);
    EXPECT_FALSE(t.take_budget_bound_alert());
    EXPECT_EQ(t.last_bound_desired(), 2 * kMinFee);
    EXPECT_EQ(t.last_bound_allowed(), kMinFee);
}

TEST(FeeTrackerController, TheReserveKeepsCancelsFundedWhileAttachedFeesAreSqueezed)
{
    xop::FeeConfig cfg = tracker_config(true, false);
    cfg.daily_budget_mojos = 8'000'000'000ULL;          // 8G
    cfg.controller_budget_reserve_cancels = 25;
    cfg.controller_max_step_up = 8.0;
    xop::FeeTracker t{cfg};
    t.observe(hard(Signal::ForceDelete, 100));           // level 4.0: every fee x16
    ASSERT_DOUBLE_EQ(t.controller().level(), 4.0);
    const std::uint64_t cancel   = t.get_recommended_fee(1, 100, ActionClass::CancelCat);
    const std::uint64_t attached = t.get_recommended_fee(1, 100, ActionClass::OfferAttached);
    const std::uint64_t take     = t.get_recommended_fee(1, 100, ActionClass::Take);
    EXPECT_EQ(cancel, 16 * kMinFee);                     // 240M; reserve = 25 x 240M = 6G
    EXPECT_GT(attached, kMinFee);                        // 16 x 0.3546 x 21M = 119M
    EXPECT_FALSE(t.take_budget_bound_alert());

    t.record_fee(1'950'000'000ULL, 100);                 // headroom 6.05G: 50M above the reserve
    // An attached fee may use only the 50M above the reserve.
    EXPECT_EQ(t.get_recommended_fee(1, 101, ActionClass::OfferAttached), 50'000'000ULL);
    EXPECT_TRUE(t.take_budget_bound_alert());
    EXPECT_EQ(t.last_bound_desired(), attached);
    EXPECT_EQ(t.last_bound_allowed(), 50'000'000ULL);
    // Cancels and takes still pay in full: that is what the reserve is for.
    EXPECT_EQ(t.get_recommended_fee(1, 101, ActionClass::CancelCat), cancel);
    EXPECT_EQ(t.get_recommended_fee(1, 101, ActionClass::Take), take);
    EXPECT_EQ(t.get_recommended_fee(1, 101, ActionClass::CancelXch),
              t.controller().fee_for(ActionClass::CancelXch, 101));

    t.record_fee(45'000'000ULL, 101);                    // 5M above the reserve: under min_fee
    EXPECT_EQ(t.get_recommended_fee(1, 102, ActionClass::OfferAttached), kMinFee);
    EXPECT_FALSE(t.take_budget_bound_alert());           // same episode: no second alert

    // The window rolls, the headroom returns, the episode ends on an attached
    // fee that comes back whole -- and a NEW squeeze alerts again.
    EXPECT_EQ(t.get_recommended_fee(1, 101 + 1'662 + 1, ActionClass::OfferAttached), attached);
    EXPECT_FALSE(t.take_budget_bound_alert());
    t.record_fee(7'990'000'000ULL, 2'000);
    EXPECT_EQ(t.get_recommended_fee(1, 2'001, ActionClass::OfferAttached), kMinFee);
    EXPECT_TRUE(t.take_budget_bound_alert());
    // Priority classes get what is left, never less than min_fee, never 0.
    EXPECT_EQ(t.get_recommended_fee(1, 2'001, ActionClass::CancelCat), kMinFee);
}

TEST(FeeTrackerController, ConfigMapsEveryTuningKey)
{
    xop::FeeConfig cfg = tracker_config(true, false);
    cfg.controller_target_delay_blocks = 11;
    cfg.controller_kp = 1.5; cfg.controller_ki = 0.75; cfg.controller_kd = 0.25;
    cfg.controller_max_error = 3.0; cfg.controller_max_step_up = 1.5; cfg.controller_min_raise = 0.4;
    cfg.controller_warmup_observations = 5;
    cfg.controller_probe_fraction = 0.2; cfg.controller_probe_after_confirmations = 12;
    cfg.controller_probe_confirmations = 4; cfg.controller_probe_fail_bump = 0.05;
    cfg.controller_probe_backoff_cap = 99; cfg.controller_ff_margin = 1.25;
    cfg.controller_ff_max_age_blocks = 40;
    cfg.controller_cost_offer_attached = 1'000'001; cfg.controller_cost_cancel_xch = 1'000'002;
    cfg.controller_cost_cancel_cat = 1'000'003; cfg.controller_cost_take = 1'000'004;
    const ControllerConfig c = xop::fee_controller_config_from(cfg);
    EXPECT_TRUE(c.enabled);
    EXPECT_EQ(c.target_delay_blocks, 11U);
    EXPECT_DOUBLE_EQ(c.kp, 1.5); EXPECT_DOUBLE_EQ(c.ki, 0.75); EXPECT_DOUBLE_EQ(c.kd, 0.25);
    EXPECT_DOUBLE_EQ(c.max_error, 3.0); EXPECT_DOUBLE_EQ(c.max_step_up, 1.5);
    EXPECT_DOUBLE_EQ(c.min_raise, 0.4);
    EXPECT_EQ(c.warmup_observations, 5U);
    EXPECT_DOUBLE_EQ(c.probe_fraction, 0.2);
    EXPECT_EQ(c.probe_after_confirmations, 12U); EXPECT_EQ(c.probe_confirmations, 4U);
    EXPECT_DOUBLE_EQ(c.probe_fail_bump, 0.05);
    EXPECT_EQ(c.probe_backoff_cap, 99U);
    EXPECT_DOUBLE_EQ(c.ff_margin, 1.25); EXPECT_EQ(c.ff_max_age_blocks, 40U);
    EXPECT_EQ(c.costs.offer_attached, 1'000'001ULL); EXPECT_EQ(c.costs.cancel_xch, 1'000'002ULL);
    EXPECT_EQ(c.costs.cancel_cat, 1'000'003ULL); EXPECT_EQ(c.costs.take, 1'000'004ULL);
    cfg.enabled = false;
    EXPECT_FALSE(xop::fee_controller_config_from(cfg).enabled);
}

}  // namespace
