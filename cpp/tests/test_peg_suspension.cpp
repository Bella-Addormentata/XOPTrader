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
