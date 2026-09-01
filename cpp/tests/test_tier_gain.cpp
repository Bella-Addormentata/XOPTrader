// ---------------------------------------------------------------------------
// [FEEGAIN] How much edge a tier carries, and against which centre.
//
// Step 8's fee-to-gain gate scores every tier by distance from the PUBLISHED
// MID, justified in its own comment as avoiding Avellaneda-Stoikov skew bias.
// The comment names a real effect and then admits a larger one: measured on
// XCH/DBX the A-S skew is mean 11.8 bps (max 29.9) while the frame error
// between the published mid and the fair-value centre is mean 143.2 bps
// (max 959).
//
// These pin all three frames, the sign trap that couples the two available
// fixes, and the live arithmetic that must not change while the shadow runs.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <limits>

#include "xop/strategy/tier_gain.hpp"

using xop::strategy::tier_expected_gain;
using xop::strategy::tier_expected_gain_legacy;

namespace {
constexpr bool kBid = false;
constexpr bool kAsk = true;

// A live XCH/DBX cycle: published mid 84.57014286, fair centre 87.32501942,
// post-A-S 87.24717084. Mojo-scaled, XCH base (1e12 mojos per unit).
constexpr double kMojo   = 1'000'000'000'000.0;
constexpr double kPubMid = 84.57014286 * kMojo;
constexpr double kFair   = 87.32501942 * kMojo;
constexpr double kPostAS = 87.24717084 * kMojo;
constexpr double kSize   = 1.0 * kMojo;      // the 1.00-unit minimum
constexpr double kBase   = kMojo;            // XCH

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

// -- The frame error, on real numbers ---------------------------------------

TEST(TierGain, ThePublishedMidOverstatesEdgeOnTheLiveCycle)
{
    // An ask at 88.5 carries 1.35% of edge over fair value. Scored from the
    // published mid it reports 4.65% -- a 3.4x overstatement, and in the
    // permissive direction, because this gate only ever DROPS tiers.
    const double ask = 88.5 * kMojo;

    const auto fair = tier_expected_gain(ask, kSize, kAsk, kFair, kBase);
    ASSERT_TRUE(fair.usable);
    EXPECT_NEAR(fair.edge_fraction, 0.01345, 1e-4);

    const auto legacy = tier_expected_gain_legacy(ask, kSize, kPubMid, kBase);
    ASSERT_TRUE(legacy.usable);
    EXPECT_NEAR(legacy.edge_fraction, 0.04647, 1e-4);

    EXPECT_GT(legacy.expected_gain_mojos, fair.expected_gain_mojos * 3)
        << "the published-mid frame credits more than three times the edge";
}

TEST(TierGain, TheAsSkewIsRealButSmallerThanTheFrameError)
{
    // What the gate's comment warns about, quantified against what it
    // accepts. Both measured on the same live cycle.
    const double frame_err = (kFair - kPubMid) / kPubMid * 10'000.0;
    const double as_skew   = (kFair - kPostAS) / kFair * 10'000.0;
    EXPECT_NEAR(frame_err, 325.7, 1.0);
    EXPECT_NEAR(as_skew, 8.9, 1.0);
    EXPECT_GT(frame_err, as_skew * 10)
        << "the comment names the small term and admits the large one";
}

TEST(TierGain, TheAsCentreReportsTheNominalSpacingOnBothSides)
{
    // Why frame (3) is wrong, structurally rather than by magnitude: tier
    // price is C'(1 +/- s), so measuring from C' returns s identically on
    // both sides and erases the inventory-skew cost exactly where A-S puts
    // it. Same spacing in, same edge out, regardless of side.
    const double s = 0.0200;                      // 200 bps
    const auto a = tier_expected_gain(kPostAS * (1.0 + s), kSize, kAsk,
                                      kPostAS, kBase);
    const auto b = tier_expected_gain(kPostAS * (1.0 - s), kSize, kBid,
                                      kPostAS, kBase);
    EXPECT_NEAR(a.edge_fraction, s, 1e-9);
    EXPECT_NEAR(b.edge_fraction, s, 1e-9);
    EXPECT_NEAR(a.edge_fraction, b.edge_fraction, 1e-9)
        << "identical by construction -- the skew cost has vanished";

    // Against the UNDISPLACED centre the two sides differ by the shift, which
    // is the cost A-S deliberately paid and the gate should be able to see.
    const auto a_fair = tier_expected_gain(kPostAS * (1.0 + s), kSize, kAsk,
                                           kFair, kBase);
    const auto b_fair = tier_expected_gain(kPostAS * (1.0 - s), kSize, kBid,
                                           kFair, kBase);
    EXPECT_LT(a_fair.edge_fraction, b_fair.edge_fraction)
        << "the centre shifted DOWN, so asks give up edge and bids gain it";
}

// -- THE SIGN TRAP ----------------------------------------------------------

TEST(TierGain, SignedEdgeExposesAWrongSideTierThatTheAbsoluteValueHides)
{
    // An ask priced BELOW fair value carries no edge -- it gives edge away.
    // The live arithmetic takes std::abs, so it credits that tier for its
    // distance, and its own std::max(0.0, ...) is dead code.
    const double bad_ask = kFair * 0.98;          // 200 bps THROUGH fair

    const auto signed_g = tier_expected_gain(bad_ask, kSize, kAsk, kFair,
                                             kBase);
    EXPECT_TRUE(signed_g.usable);
    EXPECT_LT(signed_g.edge_fraction, 0.0);
    EXPECT_EQ(signed_g.expected_gain_mojos, 0u)
        << "a tier priced through fair value is worth nothing";

    const auto legacy = tier_expected_gain_legacy(bad_ask, kSize, kFair,
                                                  kBase);
    EXPECT_NEAR(legacy.edge_fraction, 0.02, 1e-9);
    EXPECT_GT(legacy.expected_gain_mojos, 0u)
        << "the absolute value credits it in full -- this is the defect";
}

TEST(TierGain, TheSignFixIsUnsafeWithoutTheFrameFix)
{
    // WHY THE TWO CHANGES ARE COUPLED, and why "just remove the abs" would
    // be a catastrophe on its own.
    //
    // Under the published-mid frame, bid tiers sit ABOVE the mid whenever
    // the centre shift exceeds the tier spacing -- measured on 81.4% of
    // cycles. A signed edge measured from the published mid then goes
    // NEGATIVE for those bids and clamps to zero, dropping very nearly every
    // one of them.
    const double bid = kFair * (1.0 - 0.0050);    // 50 bps inside fair
    EXPECT_GT(bid, kPubMid) << "fixture: the bid really is above the mid";

    const auto signed_wrong_frame =
        tier_expected_gain(bid, kSize, kBid, kPubMid, kBase);
    EXPECT_LT(signed_wrong_frame.edge_fraction, 0.0);
    EXPECT_EQ(signed_wrong_frame.expected_gain_mojos, 0u)
        << "signed + published mid = this bid scores zero and is dropped";

    // The same bid, in the frame it was actually built in, is healthy.
    const auto signed_right_frame =
        tier_expected_gain(bid, kSize, kBid, kFair, kBase);
    EXPECT_NEAR(signed_right_frame.edge_fraction, 0.0050, 1e-6);
    EXPECT_GT(signed_right_frame.expected_gain_mojos, 0u);
}

// -- Refusals ---------------------------------------------------------------

TEST(TierGain, NoCentreMeansSkipTheGateNotScoreAgainstNothing)
{
    for (const double c : {0.0, -1.0, kNaN, kInf}) {
        const auto g = tier_expected_gain(88.0 * kMojo, kSize, kAsk, c, kBase);
        EXPECT_FALSE(g.usable)
            << "an unusable centre must make the caller SKIP, not score 0";
        EXPECT_EQ(g.expected_gain_mojos, 0u);
    }
}

TEST(TierGain, DegenerateSizeAndScaleAreRefused)
{
    EXPECT_FALSE(tier_expected_gain(88.0 * kMojo, 0.0, kAsk, kFair, kBase)
                     .usable);
    EXPECT_FALSE(tier_expected_gain(88.0 * kMojo, kSize, kAsk, kFair, 0.0)
                     .usable);
    EXPECT_FALSE(tier_expected_gain(kNaN, kSize, kAsk, kFair, kBase).usable);
}

TEST(TierGain, CatScalingMatchesTheLiveGate)
{
    // A CAT base is 1e3 mojos per unit, so the same fractional edge on the
    // same nominal size must scale up by 1e9 to be comparable with an
    // XCH-denominated fee. Pinned because this repo has already shipped one
    // overflow on this exact conversion.
    const double cat_base = 1'000.0;
    const auto xch = tier_expected_gain(kFair * 1.01, kSize, kAsk, kFair,
                                        kBase);
    const auto cat = tier_expected_gain(kFair * 1.01, kSize, kAsk, kFair,
                                        cat_base);
    ASSERT_TRUE(xch.usable);
    ASSERT_TRUE(cat.usable);
    EXPECT_NEAR(cat.edge_fraction, xch.edge_fraction, 1e-12);
    EXPECT_GT(cat.expected_gain_mojos, xch.expected_gain_mojos);
}

TEST(TierGain, AnAbsurdProductClampsRatherThanWrapping)
{
    // The product is a fraction times a size times a scale factor and no
    // operand alone bounds it. Wrapping a uint64 here would turn an enormous
    // gain into a tiny one and drop the tier.
    const auto g = tier_expected_gain(1e18, 1e18, kAsk, 1.0, 1e-6);
    EXPECT_TRUE(g.usable);
    EXPECT_GT(g.expected_gain_mojos, 0u) << "must not wrap to a small value";
}
