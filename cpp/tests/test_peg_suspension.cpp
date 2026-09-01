// ---------------------------------------------------------------------------
// Asset-level peg suspension: detect, latch, re-enable.
//
// The three properties the operator was promised: a sustained depeg latches
// (one wick does not), the latch never clears itself (only the button
// does), and the button re-arms detection rather than granting amnesty.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/risk/peg_suspension.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

using xop::risk::PegObservation;
using xop::risk::PegRuntime;
using xop::risk::observe_peg;
using xop::risk::reenable_peg;

namespace {

// wUSDC.b-shaped thresholds: warn 2%, bail 10%, 3 sustained observations.
PegObservation obs(PegRuntime& rt, double usd, std::uint32_t block = 100)
{
    return observe_peg(rt, usd, 1.0, 10.0, 2.0, 3, block);
}

}  // namespace

TEST(PegSuspension, HoldsInsideWarnAndWarnsPastIt)
{
    PegRuntime rt;
    EXPECT_EQ(obs(rt, 1.005), PegObservation::Holding);
    EXPECT_EQ(obs(rt, 0.97), PegObservation::Warn);   // 3% > warn, < bail
    EXPECT_FALSE(rt.suspended);
    EXPECT_EQ(rt.above_bail, 0u) << "warn is a message, not a streak";
}

TEST(PegSuspension, OneWickPastBailDoesNotLatch)
{
    PegRuntime rt;
    EXPECT_EQ(obs(rt, 0.80), PegObservation::Warn);   // 20% -- 1 of 3
    EXPECT_EQ(obs(rt, 1.00), PegObservation::Holding);
    EXPECT_EQ(rt.above_bail, 0u) << "recovery resets the streak";
    EXPECT_FALSE(rt.suspended);
}

TEST(PegSuspension, ASustainedBailLatchesExactlyAtTheThreshold)
{
    PegRuntime rt;
    EXPECT_EQ(obs(rt, 0.85, 501), PegObservation::Warn);
    EXPECT_EQ(obs(rt, 0.84, 502), PegObservation::Warn);
    EXPECT_EQ(obs(rt, 0.86, 503), PegObservation::JustSuspended);
    EXPECT_TRUE(rt.suspended);
    EXPECT_EQ(rt.suspended_at_block, 503u);

    // And only ONCE: the transition is what triggers the cancel, so a
    // second JustSuspended would cancel the same book twice.
    EXPECT_EQ(obs(rt, 0.86, 504), PegObservation::Suspended);
}

TEST(PegSuspension, TheLatchNeverClearsItself)
{
    PegRuntime rt;
    obs(rt, 0.85);
    obs(rt, 0.85);
    obs(rt, 0.85);
    ASSERT_TRUE(rt.suspended);

    // The chart heals completely. The bridge behind the wrapper may not
    // have -- that judgement is the operator's, not a counter's.
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(obs(rt, 1.0), PegObservation::Suspended);
    }
    EXPECT_TRUE(rt.suspended);
}

TEST(PegSuspension, ReenableRearmsDetectionRatherThanGrantingAmnesty)
{
    PegRuntime rt;
    obs(rt, 0.85);
    obs(rt, 0.85);
    obs(rt, 0.85);
    ASSERT_TRUE(rt.suspended);

    reenable_peg(rt);
    EXPECT_FALSE(rt.suspended);
    EXPECT_EQ(rt.above_bail, 0u);

    // Still depegged -> the next sustained run re-suspends. This is what
    // makes the button safe to offer: it means "look again", never "trust
    // it forever".
    EXPECT_EQ(obs(rt, 0.85), PegObservation::Warn);
    EXPECT_EQ(obs(rt, 0.85), PegObservation::Warn);
    EXPECT_EQ(obs(rt, 0.85), PegObservation::JustSuspended);
    EXPECT_TRUE(rt.suspended);
}

TEST(PegSuspension, ADataGapIsNotEvidenceInEitherDirection)
{
    PegRuntime rt;
    obs(rt, 0.85);
    obs(rt, 0.85);
    ASSERT_EQ(rt.above_bail, 2u);

    // NaN, zero, negative, infinite: the streak HOLDS -- an unpriceable
    // tick must neither suspend on an outage nor blind-reset a genuine run.
    for (double junk : {std::numeric_limits<double>::quiet_NaN(), 0.0, -1.0,
                        std::numeric_limits<double>::infinity()}) {
        obs(rt, junk);
        EXPECT_EQ(rt.above_bail, 2u) << junk;
        EXPECT_FALSE(rt.suspended) << junk;
    }

    // The next real observation continues the streak.
    EXPECT_EQ(obs(rt, 0.85), PegObservation::JustSuspended);
}

TEST(PegSuspension, ZeroSustainedMeansNowNotNever)
{
    PegRuntime rt;
    EXPECT_EQ(observe_peg(rt, 0.5, 1.0, 10.0, 2.0, /*sustained=*/0, 7),
              PegObservation::JustSuspended);
    EXPECT_TRUE(rt.suspended);
}

TEST(PegSuspension, TheDisplayStaysHonestWhileLatched)
{
    PegRuntime rt;
    obs(rt, 0.85);
    obs(rt, 0.85);
    obs(rt, 0.85);
    ASSERT_TRUE(rt.suspended);

    obs(rt, 0.70);
    EXPECT_NEAR(rt.last_deviation_pct, 30.0, 1e-9)
        << "the operator deciding whether to re-enable needs the CURRENT "
           "deviation, not the one that latched";
}

// ===========================================================================
// peg_usd_observation -- the route that twice suspended a healthy peg.
//
// [review, PR #134] The suspension tests all drove observe_peg() with
// ALREADY-SCALED doubles, so nothing exercised the 1e12 conversion or the
// side-quality skip that sit in front of it -- the exact path that cancelled
// every offer on every pair touching wUSDC.b (2026-08-29) and BYC
// (2026-08-30). These drive it directly.
// ===========================================================================

namespace {

// XCH/BYC on 2026-08-30: XCH at $1.43, BYC honest at par.
constexpr double kUsdXch = 1.43;
constexpr double kScale  = 1e12;          // kMojosPerXch

// A near-par cross: 1.4142 BYC per XCH => BYC = 1.43 / 1.4142 = $1.0112.
constexpr double kParMidMojos = 1.4142 * kScale;

// The dislocated published mid on the same pair: 3.25 BYC per XCH, which is
// the mean of an honest 1.50 bid and a junk 4.9995 ask.
constexpr double kJunkMidMojos = 3.25 * kScale;

}  // namespace

TEST(PegUsdObservation, MojoScaledNearParCrossReadsAsPar)
{
    // THE UNIT BUG. Before the fix this route divided usd_per_xch by the RAW
    // mojo-scaled mid, giving 1.43 / 1.4142e12 = 1.01e-12 -- reported as
    // "observed $0.0000 ... 100.0% off" and latched as a depeg.
    const auto obs = xop::risk::peg_usd_observation(
        kParMidMojos, kScale, /*xch_base=*/true,
        /*mid_valuation_grade=*/true, /*bid_side_ok=*/true,
        /*ask_side_ok=*/true, kUsdXch);
    ASSERT_TRUE(obs.has_value());
    EXPECT_NEAR(*obs, 1.0112, 1e-3)
        << "a near-par cross must read as ~$1.01, not ~1e-12";
}

TEST(PegUsdObservation, ANearParObservationDoesNotSuspend)
{
    // End to end through observe_peg: the value this route produces must not
    // trip bail_pct, however many heartbeats it repeats for.
    const auto obs = xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true, kUsdXch);
    ASSERT_TRUE(obs.has_value());

    xop::risk::PegRuntime rt{};
    for (int i = 0; i < 60; ++i) {
        const auto what = xop::risk::observe_peg(
            rt, *obs, /*peg_target=*/1.0, /*bail_pct=*/10.0,
            /*warn_pct=*/2.0, /*sustained_observations=*/30,
            /*block_height=*/1000u + static_cast<std::uint32_t>(i));
        ASSERT_NE(what, xop::risk::PegObservation::JustSuspended)
            << "suspended on heartbeat " << i << " at an observation of "
            << *obs;
    }
    EXPECT_FALSE(rt.suspended);
    EXPECT_EQ(rt.above_bail, 0u);
}

TEST(PegUsdObservation, UnscaledMidWouldHaveSuspended)
{
    // The counterfactual, pinned so the regression cannot come back quietly:
    // feed observe_peg what the OLD code computed and confirm it latches.
    // If this ever stops suspending, the bail threshold moved and this whole
    // test file needs rereading.
    const double old_broken_value = kUsdXch / kParMidMojos;   // ~1.01e-12
    xop::risk::PegRuntime rt{};
    bool suspended = false;
    for (int i = 0; i < 40 && !suspended; ++i) {
        suspended = xop::risk::observe_peg(
            rt, old_broken_value, 1.0, 10.0, 2.0, 30,
            1000u + static_cast<std::uint32_t>(i))
            == xop::risk::PegObservation::JustSuspended;
    }
    EXPECT_TRUE(suspended)
        << "the pre-fix arithmetic is expected to latch a false depeg";
}

TEST(PegUsdObservation, ADisqualifiedSideYieldsNothing)
{
    // THE SOURCE BUG. Scale alone was not enough: on the dislocated book the
    // correctly scaled observation is 1.43 / 3.25 = $0.44, still 56% off par
    // and still past bail_pct 10. So a poisoned midpoint must produce NO
    // observation rather than a plausible-looking one.
    EXPECT_NEAR(kUsdXch / 3.25, 0.44, 0.01)
        << "sanity: the correctly-scaled junk observation really is ~$0.44";

    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kJunkMidMojos, kScale, true, true,
        /*bid_side_ok=*/true, /*ask_side_ok=*/false, kUsdXch).has_value());
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kJunkMidMojos, kScale, true, true,
        /*bid_side_ok=*/false, /*ask_side_ok=*/true, kUsdXch).has_value());
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kJunkMidMojos, kScale, true, true, false, false, kUsdXch)
            .has_value());
}

TEST(PegUsdObservation, ADisqualifiedSideHoldsTheStreakRatherThanResettingIt)
{
    // The data-gap contract. A skipped observation must neither advance the
    // streak toward suspension nor clear a genuine one already building --
    // absence of evidence is not evidence either way.
    xop::risk::PegRuntime rt{};

    // Three genuinely bad observations build a streak.
    for (int i = 0; i < 3; ++i) {
        static_cast<void>(xop::risk::observe_peg(
            rt, 0.50, 1.0, 10.0, 2.0, 30,
            1000u + static_cast<std::uint32_t>(i)));
    }
    ASSERT_EQ(rt.above_bail, 3u);

    // Now the book goes junk-sided: no observation at all.
    const auto gap = xop::risk::peg_usd_observation(
        kJunkMidMojos, kScale, true, true, true, /*ask_side_ok=*/false,
        kUsdXch);
    ASSERT_FALSE(gap.has_value());

    const double nan_obs = std::numeric_limits<double>::quiet_NaN();
    for (int i = 0; i < 10; ++i) {
        static_cast<void>(
            xop::risk::observe_peg(rt, nan_obs, 1.0, 10.0, 2.0, 30, 2000u));
    }
    EXPECT_EQ(rt.above_bail, 3u)
        << "a data gap must HOLD the streak -- neither advance nor reset";
    EXPECT_FALSE(rt.suspended);
}

TEST(PegUsdObservation, UngradedMidYieldsNothing)
{
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, /*mid_valuation_grade=*/false,
        true, true, kUsdXch).has_value());
}

TEST(PegUsdObservation, XchQuoteOrientationMultipliesInsteadOfDividing)
{
    // <asset>/XCH: the price is XCH per asset, so the USD value is the
    // PRODUCT. Getting this backwards yields a plausible reciprocal, which
    // is the failure mode mid_gate's orient_triangle comment warns about.
    // 0.7071 XCH per BYC * $1.43 = $1.0112.
    const auto obs = xop::risk::peg_usd_observation(
        0.7071 * kScale, kScale, /*xch_base=*/false, true, true, true,
        kUsdXch);
    ASSERT_TRUE(obs.has_value());
    EXPECT_NEAR(*obs, 1.0112, 1e-3);
}

TEST(PegUsdObservation, DegenerateInputsYieldNothing)
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // usd_per_xch unavailable -- the engine's own guard, restated here.
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true, 0.0).has_value());
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true, nan).has_value());
    // No mid.
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        0.0, kScale, true, true, true, true, kUsdXch).has_value());
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        inf, kScale, true, true, true, true, kUsdXch).has_value());
    // Nonsense scale.
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kParMidMojos, 0.0, true, true, true, true, kUsdXch).has_value());
}
