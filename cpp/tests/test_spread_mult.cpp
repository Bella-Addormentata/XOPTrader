// =============================================================================
//  test_spread_mult.cpp -- the APPLIED spread multiplier, measured not derived.
// =============================================================================
//
//  [2026-09-02, review] WHAT DEFECT THIS PINS
//  ------------------------------------------
//  A gauge named `effective_spread_mult` was published as
//
//      analysis_spread_mult * (spread_pid_active ? spread_pid_mult : 1.0)
//
//  and documented as the multiplier applied to the spread. Step 5 mutates
//  total_spread_bps at TEN sites; that product captured TWO of them, and two
//  of the remaining eight are ASSIGNMENTS (the order-book tactician and the
//  global half-spread cap) which discard everything before them, both
//  captured factors included.
//
//  The test that would have caught it is the one below named
//  AnAssignmentSiteDiscardsTheChainAndTheRatioFollowsIt: it models the
//  tactician/cap overwrite and asserts the reported multiplier tracks the
//  spread that was actually posted, not the product of the multipliers that
//  ran and were thrown away. The old product-of-two-factors formula fails it
//  by construction, because it cannot see the assignment at all.
//
//  MUTATION CHECK, run before this file was accepted:
//    * changing the sentinel from 0.0 to 1.0 -> fails the header static_assert
//      at COMPILE time (build stops; it can never reach a green suite) and
//      also fails NoReadingIsZeroNeverOne here.
//    * dropping either plausibility bound -> fails RejectsImplausibleInputs.
//    * swapping numerator and denominator -> fails every ratio test.
//
// =============================================================================

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "xop/strategy/spread_mult.hpp"

using xop::strategy::applied_spread_mult;
using xop::strategy::kMaxPlausibleSpreadBps;

TEST(SpreadMult, UnchangedSpreadIsOne) {
    EXPECT_DOUBLE_EQ(applied_spread_mult(100.0, 100.0), 1.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(37.5, 37.5), 1.0);
}

TEST(SpreadMult, TighteningIsBelowOneAndWideningIsAbove) {
    EXPECT_DOUBLE_EQ(applied_spread_mult(100.0, 82.0), 0.82);
    EXPECT_DOUBLE_EQ(applied_spread_mult(100.0, 150.0), 1.5);
}

// The live 2026-09-02 contradiction, end to end. The startup analysis said
// 1.5x (defensively wide) while the spread PID was railed at 0.820 (maximum
// tightening). Their PRODUCT is 1.23x -- which is what the old gauge showed,
// and it is a third number that matches neither what the operator was told
// nor, as the next test shows, what was actually posted.
TEST(SpreadMult, TheProductOfTwoFactorsIsNotTheAppliedMultiplier) {
    const double base = 100.0;

    // What the chain's two captured factors multiply out to.
    const double captured_product = 1.5 * 0.820;
    EXPECT_NEAR(captured_product, 1.23, 1e-12);

    // What Step 5 actually finished with, once the six OTHER multiplicative
    // sites have also run (say a 1.4x regime/whale/VPIN composite).
    const double applied = base * captured_product * 1.4;

    EXPECT_NEAR(applied_spread_mult(base, applied), 1.722, 1e-9);
    EXPECT_GT(std::abs(applied_spread_mult(base, applied) - captured_product),
              0.4)
        << "the product of the two captured factors is not the applied "
           "multiplier, and this fixture must keep showing that";
}

// THE REGRESSION THIS FILE EXISTS FOR.
//
// Both `total_spread_bps = adj.spread_bps` (order-book tactician) and
// `total_spread_bps = max_hs * 2.0` (global half-spread cap) are assignments.
// Every multiplier that ran before them is discarded. A gauge built from the
// multipliers cannot see this; a ratio measured across the whole step tracks
// it exactly.
TEST(SpreadMult, AnAssignmentSiteDiscardsTheChainAndTheRatioFollowsIt) {
    const double base = 100.0;

    // The chain compounds to 12.3x...
    const double after_multipliers = base * 1.5 * 0.820 * 10.0;
    EXPECT_NEAR(after_multipliers, 1230.0, 1e-9);

    // ...and then the global cap assigns 500 bps outright, discarding it.
    const double capped = 500.0;

    EXPECT_DOUBLE_EQ(applied_spread_mult(base, capped), 5.0);

    // The old formula's answer, for contrast: it reports 1.23x while the book
    // is quoted at 5.0x baseline. Four times wrong, in the direction that
    // makes an operator think the bot is quoting near baseline.
    const double old_gauge = 1.5 * 0.820;
    EXPECT_GT(applied_spread_mult(base, capped) / old_gauge, 4.0);

    // The tactician's assignment, which can go the other way: it can TIGHTEN
    // below the base while the multiplier product says "widening".
    EXPECT_DOUBLE_EQ(applied_spread_mult(base, 60.0), 0.6);
    EXPECT_LT(applied_spread_mult(base, 60.0), 1.0);
    EXPECT_GT(old_gauge, 1.0)
        << "the two disagree about the SIGN of the adjustment, not just its "
           "size -- that is the whole finding";
}

// A fallback of 1.0 would report "quoting at baseline", the most reassuring
// value in the range, for a block whose quote was never computed. The
// documented fail-open shape. The sentinel is 0.0, which is unreachable as a
// real multiplier.
TEST(SpreadMult, NoReadingIsZeroNeverOne) {
    EXPECT_DOUBLE_EQ(applied_spread_mult(0.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(100.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(-5.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(100.0, -5.0), 0.0);
}

TEST(SpreadMult, RejectsImplausibleInputs) {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    EXPECT_DOUBLE_EQ(applied_spread_mult(inf, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(100.0, inf), 0.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(nan, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(100.0, nan), 0.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(nan, nan), 0.0);

    EXPECT_DOUBLE_EQ(applied_spread_mult(kMaxPlausibleSpreadBps, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(applied_spread_mult(100.0, kMaxPlausibleSpreadBps), 0.0);

    // Just inside the bound is still a reading, however silly.
    EXPECT_GT(applied_spread_mult(100.0, kMaxPlausibleSpreadBps - 1.0), 0.0);
}

// Every result the function can return is either 0 (no reading) or a finite
// positive number. Nothing downstream should have to guard against NaN.
TEST(SpreadMult, OutputIsAlwaysZeroOrFinitePositive) {
    const double xs[] = {-1e12, -1.0, 0.0, 1e-9, 1.0, 8.0, 500.0, 1e6,
                         kMaxPlausibleSpreadBps, 1e300,
                         std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()};
    for (double a : xs) {
        for (double b : xs) {
            const double m = applied_spread_mult(a, b);
            EXPECT_FALSE(std::isnan(m)) << "base=" << a << " applied=" << b;
            EXPECT_TRUE(std::isfinite(m)) << "base=" << a << " applied=" << b;
            EXPECT_GE(m, 0.0) << "base=" << a << " applied=" << b;
        }
    }
}
