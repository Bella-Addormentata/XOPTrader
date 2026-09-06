// ---------------------------------------------------------------------------
// [S33 2026-09-05] Step 7's sigma width floor -- one delta PER SCHEDULE.
//
// The floor shift used to be computed ONCE from the static tier_spacing_bps
// and applied to all three schedules.  build_raw_ladder prices from the
// activity controller's _bid/_ask schedules whenever they are filled, and the
// controller interpolates those up toward tier_spacing_max_bps -- so in the
// low-activity case they are ALREADY wider than the floor.  Reusing the base
// delta stacked the whole shift on top of an already-compliant side schedule
// and widened a ladder the controller had deliberately sized, feeding straight
// back into the low-fill state that widened it.  The converse failed too: a
// side schedule NARROWER than the base one was not brought up to the floor.
//
// This sets the quoted spread on a live bot, so the documented numeric cases
// are asserted rather than described.
//
// MUTATION CHECK, and what it does and does not reach.  The pre-S33 code
// shifted an already-compliant schedule by someone else's delta; the same
// shape inside this function is dropping the "already at/outside the floor"
// early return, so a negative shift is applied:
//
//     const double shift = min_half_spread_bps - spacings.front();
//     for (double& s : spacings) s += shift;   // no guard
//     return shift;
//
// AnAdaptiveSchedulePastTheFloorIsNotWidenedFurther, AZeroFloorNeverShifts
// and TheThreeSchedulesAreShiftedIndependently go RED under it.
// AScheduleAtTheFloorIsUntouched does NOT -- at the boundary the guarded and
// unguarded forms agree -- it is a boundary document, not a killer.
//
// Be honest about the reach: which SCHEDULE each shift is derived from is a
// call-site property (step_generate_ladder passes each vector with the same
// floor), so restoring the shared-delta call site is not killable from here.
// What these tests pin is that the per-schedule rule is right, and that a
// schedule already past the floor is shifted by exactly zero -- the property
// the shared-delta version violated.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "xop/engine.hpp"

using xop::shift_schedule_to_floor;

namespace {

// The ordinary case the floor exists for: a tight base schedule inside a
// 400 bps minimum half-spread is pushed out bodily, keeping its gaps.
TEST(WidthFloor, ATightScheduleIsShiftedOutPreservingItsGaps)
{
    std::vector<double> spacings{100.0, 200.0, 300.0};

    EXPECT_DOUBLE_EQ(shift_schedule_to_floor(spacings, 400.0), 300.0);
    ASSERT_EQ(spacings.size(), 3u);
    EXPECT_DOUBLE_EQ(spacings[0], 400.0);
    EXPECT_DOUBLE_EQ(spacings[1], 500.0);
    EXPECT_DOUBLE_EQ(spacings[2], 600.0);
}

// THE mutation-killer, and the live defect: the activity controller has
// already put this side schedule outside the floor.  The old code shifted it
// by the BASE schedule's delta (+300 from {100,...}), turning 600 into 900.
TEST(WidthFloor, AnAdaptiveSchedulePastTheFloorIsNotWidenedFurther)
{
    std::vector<double> adaptive{600.0, 900.0};

    EXPECT_DOUBLE_EQ(shift_schedule_to_floor(adaptive, 400.0), 0.0);
    ASSERT_EQ(adaptive.size(), 2u);
    EXPECT_DOUBLE_EQ(adaptive[0], 600.0)
        << "the controller sized this deliberately; widening it feeds the "
           "low-fill state that widened it in the first place";
    EXPECT_DOUBLE_EQ(adaptive[1], 900.0);
}

// The converse failure: a side schedule NARROWER than the base one gets its
// own, larger delta.  The old code gave it the base delta (+300 -> {350,450}),
// leaving the innermost tier inside the 400 bps floor.
TEST(WidthFloor, ANarrowerSideScheduleGetsItsOwnLargerShift)
{
    std::vector<double> side{50.0, 150.0};

    EXPECT_DOUBLE_EQ(shift_schedule_to_floor(side, 400.0), 350.0);
    ASSERT_EQ(side.size(), 2u);
    EXPECT_DOUBLE_EQ(side[0], 400.0)
        << "the innermost tier must clear the floor on its OWN schedule";
    EXPECT_DOUBLE_EQ(side[1], 500.0);
}

// Boundary: the guard is `<=`, so a schedule exactly at the floor is left
// alone rather than shifted by zero-and-rewritten.
TEST(WidthFloor, AScheduleAtTheFloorIsUntouched)
{
    std::vector<double> spacings{400.0, 700.0};

    EXPECT_DOUBLE_EQ(shift_schedule_to_floor(spacings, 400.0), 0.0);
    EXPECT_DOUBLE_EQ(spacings[0], 400.0);
    EXPECT_DOUBLE_EQ(spacings[1], 700.0);
}

// An unfilled side schedule (the controller is off) must not be touched, and
// must not read front() on an empty vector.
TEST(WidthFloor, AnEmptyScheduleIsLeftEmpty)
{
    std::vector<double> spacings;

    EXPECT_DOUBLE_EQ(shift_schedule_to_floor(spacings, 400.0), 0.0);
    EXPECT_TRUE(spacings.empty());
}

// A disabled floor (0 bps) never shifts anything.
TEST(WidthFloor, AZeroFloorNeverShifts)
{
    std::vector<double> spacings{100.0, 200.0};

    EXPECT_DOUBLE_EQ(shift_schedule_to_floor(spacings, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(spacings[0], 100.0);
    EXPECT_DOUBLE_EQ(spacings[1], 200.0);
}

// The three schedules are independent: Step 7 shifts base, bid and ask each
// by their own delta in the same heartbeat.  This is the whole-call-site
// shape, asserted end to end.
TEST(WidthFloor, TheThreeSchedulesAreShiftedIndependently)
{
    std::vector<double> base{100.0, 200.0, 300.0};
    std::vector<double> bid{600.0, 900.0};        // controller: already wide
    std::vector<double> ask{50.0, 150.0};         // narrower than base

    const double base_shift = shift_schedule_to_floor(base, 400.0);
    const double bid_shift  = shift_schedule_to_floor(bid, 400.0);
    const double ask_shift  = shift_schedule_to_floor(ask, 400.0);

    EXPECT_DOUBLE_EQ(base_shift, 300.0);
    EXPECT_DOUBLE_EQ(bid_shift, 0.0);
    EXPECT_DOUBLE_EQ(ask_shift, 350.0);
    EXPECT_DOUBLE_EQ(base[0], 400.0);
    EXPECT_DOUBLE_EQ(bid[0], 600.0);
    EXPECT_DOUBLE_EQ(ask[0], 400.0);
    // Every side that quotes now clears the floor, and none was over-widened.
    EXPECT_GE(base.front(), 400.0);
    EXPECT_GE(bid.front(), 400.0);
    EXPECT_GE(ask.front(), 400.0);
}

// Monotonicity is a liquidity.cpp precondition (non-decreasing spacings);
// adding a constant preserves it, and preserves the inter-tier gaps.
TEST(WidthFloor, ShiftingPreservesOrderAndGaps)
{
    std::vector<double> spacings{10.0, 40.0, 41.0, 500.0};
    const std::vector<double> before = spacings;

    const double shift = shift_schedule_to_floor(spacings, 250.0);
    EXPECT_DOUBLE_EQ(shift, 240.0);
    for (std::size_t i = 0; i < spacings.size(); ++i) {
        EXPECT_DOUBLE_EQ(spacings[i], before[i] + shift);
        // Braces are required, not style: EXPECT_GE expands to an if/else, so
        // an unbraced `if` body trips GCC's -Werror=dangling-else.
        if (i > 0) {
            EXPECT_GE(spacings[i], spacings[i - 1]);
        }
    }
}

}  // namespace
