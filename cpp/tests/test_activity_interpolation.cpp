// ---------------------------------------------------------------------------
// [S33 2026-09-05] Step 7's 24h activity-adaptive controller --
// interpolate_activity_schedules.
//
// This function sets the LIVE bid/ask profit margins and the live tier-spacing
// ladders on a running market maker, and until now nothing in ctest touched
// it: the added tests covered the database count query (test_database.cpp) and
// the width-floor shift (test_width_floor.cpp), both DOWNSTREAM of it.  A
// reversed interpolation or a same-side "fix" to the coupling would have
// changed quoted prices with every test still green.
//
// TWO PROPERTIES ARE PINNED HERE.
//
// 1. THE COUPLING IS CROSS-SIDE.  alpha_bid (bid fills) drives the ASK
//    schedule; alpha_ask drives the BID schedule.  It reads like a typo and it
//    is not: bid fills replenish base inventory, which is what makes it safe
//    to tighten the ask and sell it.  AsymmetricActivityDrivesTheOppositeSide
//    and CrossSideCouplingIsNotSameSide go RED if someone "corrects" it to
//        bid_margin = max - a_bid * (max - min)   // same-side
//        ask_margin = max - a_ask * (max - min)
//    (both were run under exactly that mutation -- see the report).
//
// 2. AN INVERTED CONFIGURED RANGE IS CLAMPED, NEVER APPLIED BACKWARDS.
//    min_profit_margin_max_bps_override / tier_spacing_max_bps_override are
//    validated only as positive at config load, so max < min is a reachable,
//    "valid" configuration.  Applied as written the interpolation runs in
//    reverse and ZERO activity TIGHTENS quotes -- the exact inversion of the
//    controller's protective contract, on a live book.
//
// MUTATION CHECK.  Each named test below was re-run with its specific defect
// reinstated (the same-side coupling above; and for the clamps, restoring the
// raw `max_margin_bps` / `max_spacings[i]` in place of the std::max) and
// confirmed RED.  Results are reported honestly in the handover, including
// which tests are boundary documents that do NOT kill a mutation.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "xop/engine.hpp"

using xop::ActivitySchedules;
using xop::interpolate_activity_schedules;

namespace {

// The controller's headline contract: with no activity on either side, both
// sides sit at the WIDE end.  A reversed interpolation puts them at min.
TEST(ActivityInterpolation, ZeroActivityWidensBothSidesToTheMaximum)
{
    const std::vector<double> min_s{100.0, 200.0, 300.0};
    const std::vector<double> max_s{600.0, 900.0, 1200.0};

    const ActivitySchedules s = interpolate_activity_schedules(
        0.0, 0.0, 50.0, 250.0, min_s, max_s, 3);

    EXPECT_DOUBLE_EQ(s.bid_margin_bps, 250.0);
    EXPECT_DOUBLE_EQ(s.ask_margin_bps, 250.0);
    ASSERT_EQ(s.bid_spacings.size(), 3u);
    ASSERT_EQ(s.ask_spacings.size(), 3u);
    EXPECT_DOUBLE_EQ(s.bid_spacings[0], 600.0);
    EXPECT_DOUBLE_EQ(s.bid_spacings[2], 1200.0);
    EXPECT_DOUBLE_EQ(s.ask_spacings[0], 600.0);
    EXPECT_DOUBLE_EQ(s.ask_spacings[2], 1200.0);
    EXPECT_FALSE(s.margin_range_inverted);
    EXPECT_EQ(s.spacing_tiers_inverted, 0u);
}

// At or above target activity on both sides, both collapse to the tight end.
TEST(ActivityInterpolation, TargetActivityTightensBothSidesToTheMinimum)
{
    const std::vector<double> min_s{100.0, 200.0};
    const std::vector<double> max_s{600.0, 900.0};

    const ActivitySchedules s = interpolate_activity_schedules(
        1.0, 1.0, 50.0, 250.0, min_s, max_s, 2);

    EXPECT_DOUBLE_EQ(s.bid_margin_bps, 50.0);
    EXPECT_DOUBLE_EQ(s.ask_margin_bps, 50.0);
    EXPECT_DOUBLE_EQ(s.bid_spacings[0], 100.0);
    EXPECT_DOUBLE_EQ(s.bid_spacings[1], 200.0);
    EXPECT_DOUBLE_EQ(s.ask_spacings[0], 100.0);
    EXPECT_DOUBLE_EQ(s.ask_spacings[1], 200.0);
}

// THE cross-side killer.  Bids are filling at target (alpha_bid = 1), asks are
// dead (alpha_ask = 0).  We are replenishing base inventory, so the ASK side
// tightens to sell it and the BID side widens.  Same-side coupling produces
// exactly the opposite assignment.
TEST(ActivityInterpolation, AsymmetricActivityDrivesTheOppositeSide)
{
    const std::vector<double> min_s{100.0, 200.0};
    const std::vector<double> max_s{600.0, 900.0};

    const ActivitySchedules s = interpolate_activity_schedules(
        /*alpha_bid=*/1.0, /*alpha_ask=*/0.0, 50.0, 250.0, min_s, max_s, 2);

    EXPECT_DOUBLE_EQ(s.ask_margin_bps, 50.0)
        << "bid fills replenish base inventory, so the ASK tightens to sell it";
    EXPECT_DOUBLE_EQ(s.bid_margin_bps, 250.0)
        << "no ask fills means no quote accumulation, so the BID widens";
    EXPECT_DOUBLE_EQ(s.ask_spacings[0], 100.0);
    EXPECT_DOUBLE_EQ(s.ask_spacings[1], 200.0);
    EXPECT_DOUBLE_EQ(s.bid_spacings[0], 600.0);
    EXPECT_DOUBLE_EQ(s.bid_spacings[1], 900.0);
}

// The mirror of the above, stated as an inequality so it survives any retuning
// of the numbers: with strictly more bid activity than ask activity, the ASK
// side must be the TIGHTER one.  Same-side coupling reverses both.
TEST(ActivityInterpolation, CrossSideCouplingIsNotSameSide)
{
    const std::vector<double> min_s{100.0, 200.0, 300.0};
    const std::vector<double> max_s{600.0, 900.0, 1200.0};

    const ActivitySchedules s = interpolate_activity_schedules(
        /*alpha_bid=*/0.75, /*alpha_ask=*/0.25, 50.0, 250.0, min_s, max_s, 3);

    EXPECT_LT(s.ask_margin_bps, s.bid_margin_bps)
        << "the side driven by the BUSY (bid) side must be the tight one";
    EXPECT_DOUBLE_EQ(s.ask_margin_bps, 100.0);   // 250 - 0.75 * 200
    EXPECT_DOUBLE_EQ(s.bid_margin_bps, 200.0);   // 250 - 0.25 * 200
    for (std::size_t i = 0; i < s.ask_spacings.size(); ++i) {
        EXPECT_LT(s.ask_spacings[i], s.bid_spacings[i]);
    }
    EXPECT_DOUBLE_EQ(s.ask_spacings[0], 225.0);  // 600 - 0.75 * 500
    EXPECT_DOUBLE_EQ(s.bid_spacings[0], 475.0);  // 600 - 0.25 * 500
}

// Item 2: min_profit_margin_max_bps_override below the effective minimum.
// Applied as configured, alpha = 0 would quote 40bps -- TIGHTER than the
// 200bps floor the operator set as the minimum.  Clamped, zero activity gives
// the minimum and the controller simply stops widening this pair.
TEST(ActivityInterpolation, AnInvertedMarginRangeIsClampedNotAppliedBackwards)
{
    const std::vector<double> min_s{100.0};
    const std::vector<double> max_s{100.0};

    const ActivitySchedules zero = interpolate_activity_schedules(
        0.0, 0.0, /*min_margin=*/200.0, /*max_margin=*/40.0, min_s, max_s, 1);
    const ActivitySchedules busy = interpolate_activity_schedules(
        1.0, 1.0, 200.0, 40.0, min_s, max_s, 1);

    EXPECT_TRUE(zero.margin_range_inverted);
    EXPECT_DOUBLE_EQ(zero.bid_margin_bps, 200.0);
    EXPECT_DOUBLE_EQ(zero.ask_margin_bps, 200.0);
    // The contract that must never break, whatever the config says.
    EXPECT_GE(zero.bid_margin_bps, busy.bid_margin_bps)
        << "zero activity must never quote tighter than target activity";
    EXPECT_GE(zero.ask_margin_bps, busy.ask_margin_bps);
}

// Item 3, same class: a tier_spacing_max_bps_override entry below its base
// spacing.  Only tier 1 is inverted here (50 < 200); tier 0 is a legitimate
// widening and must be left alone, so a blanket "ignore the override" fix does
// not pass either.
TEST(ActivityInterpolation, AnInvertedSpacingEntryIsClampedPerTier)
{
    const std::vector<double> min_s{100.0, 200.0};
    const std::vector<double> max_s{600.0, 50.0};

    const ActivitySchedules zero = interpolate_activity_schedules(
        0.0, 0.0, 50.0, 250.0, min_s, max_s, 2);
    const ActivitySchedules busy = interpolate_activity_schedules(
        1.0, 1.0, 50.0, 250.0, min_s, max_s, 2);

    EXPECT_EQ(zero.spacing_tiers_inverted, 1u);
    EXPECT_DOUBLE_EQ(zero.bid_spacings[0], 600.0) << "tier 0 is well-formed";
    EXPECT_DOUBLE_EQ(zero.bid_spacings[1], 200.0) << "tier 1 clamped up to min";
    EXPECT_DOUBLE_EQ(zero.ask_spacings[1], 200.0);
    for (std::size_t i = 0; i < zero.bid_spacings.size(); ++i) {
        EXPECT_GE(zero.bid_spacings[i], busy.bid_spacings[i])
            << "the zero-activity schedule must never be narrower than the "
               "active one, tier " << i;
        EXPECT_GE(zero.ask_spacings[i], busy.ask_spacings[i]);
    }
}

// The no-override case: max_spacings IS min_spacings (the call site passes the
// base schedule when tier_spacing_max_bps_override is absent), so spacing is
// activity-independent and nothing counts as inverted.
TEST(ActivityInterpolation, WithNoSpacingOverrideTheScheduleIsActivityIndependent)
{
    const std::vector<double> min_s{100.0, 200.0, 300.0};

    const ActivitySchedules zero = interpolate_activity_schedules(
        0.0, 0.0, 50.0, 250.0, min_s, min_s, 3);
    const ActivitySchedules busy = interpolate_activity_schedules(
        1.0, 1.0, 50.0, 250.0, min_s, min_s, 3);

    EXPECT_EQ(zero.spacing_tiers_inverted, 0u);
    for (std::size_t i = 0; i < min_s.size(); ++i) {
        EXPECT_DOUBLE_EQ(zero.bid_spacings[i], min_s[i]);
        EXPECT_DOUBLE_EQ(busy.ask_spacings[i], min_s[i]);
    }
    // The margin still adapts -- only the spacing is pinned.
    EXPECT_DOUBLE_EQ(zero.bid_margin_bps, 250.0);
    EXPECT_DOUBLE_EQ(busy.bid_margin_bps, 50.0);
}

// num_tiers past the end of both schedules: the call site's historical default
// is 100 * (tier + 1) bps, and with no maximum for that tier it is flat.
// Boundary document -- it pins the fallback, it kills no mutation on its own.
TEST(ActivityInterpolation, TiersPastTheScheduleFallBackToTheDefaultSpacing)
{
    const std::vector<double> min_s{100.0};
    const std::vector<double> max_s{600.0};

    const ActivitySchedules s = interpolate_activity_schedules(
        0.0, 0.0, 50.0, 250.0, min_s, max_s, 3);

    ASSERT_EQ(s.bid_spacings.size(), 3u);
    EXPECT_DOUBLE_EQ(s.bid_spacings[0], 600.0);
    EXPECT_DOUBLE_EQ(s.bid_spacings[1], 200.0);
    EXPECT_DOUBLE_EQ(s.bid_spacings[2], 300.0);
    EXPECT_EQ(s.spacing_tiers_inverted, 0u)
        << "a tier with no configured maximum is not a misconfiguration";
}

// Zero tiers is reachable (a pair configured with an empty ladder) and must
// not read past the end of either schedule.
TEST(ActivityInterpolation, ZeroTiersProducesEmptySchedules)
{
    const std::vector<double> empty;

    const ActivitySchedules s = interpolate_activity_schedules(
        0.5, 0.5, 50.0, 250.0, empty, empty, 0);

    EXPECT_TRUE(s.bid_spacings.empty());
    EXPECT_TRUE(s.ask_spacings.empty());
    EXPECT_EQ(s.spacing_tiers_inverted, 0u);
    EXPECT_DOUBLE_EQ(s.bid_margin_bps, 150.0);
}

// Out-of-range alphas are clamped defensively rather than extrapolating the
// margin outside [min, max].  The call site already clamps; this is the
// second line.
TEST(ActivityInterpolation, AlphasOutsideTheUnitIntervalAreClamped)
{
    const std::vector<double> min_s{100.0};
    const std::vector<double> max_s{600.0};

    const ActivitySchedules s = interpolate_activity_schedules(
        /*alpha_bid=*/3.0, /*alpha_ask=*/-2.0, 50.0, 250.0, min_s, max_s, 1);

    EXPECT_DOUBLE_EQ(s.ask_margin_bps, 50.0);    // alpha_bid clamped to 1
    EXPECT_DOUBLE_EQ(s.bid_margin_bps, 250.0);   // alpha_ask clamped to 0
    EXPECT_DOUBLE_EQ(s.ask_spacings[0], 100.0);
    EXPECT_DOUBLE_EQ(s.bid_spacings[0], 600.0);
}

}  // namespace
