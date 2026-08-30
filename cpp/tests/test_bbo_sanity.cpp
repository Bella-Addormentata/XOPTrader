// ---------------------------------------------------------------------------
// [ALWAYSOFFER] Side-aware BBO sanity routing.
//
// The two risks are different animals: an aggressive tier EXECUTES at a
// dislocated price (the 2026-08-01 microprice sweep), a passive tier
// merely RESTS far from a thin book (the 2026-08-30 XCH/BYC case, where
// the symmetric check kept a cost-floored ask -- guaranteed profitable on
// any fill -- permanently off the book).
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/strategy/bbo_sanity.hpp"

using xop::strategy::BboVerdict;
using xop::strategy::classify_tier;

namespace {
constexpr double kAgg = 0.10;
constexpr double kPas = 0.30;
}  // namespace

TEST(BboSanity, cost_floored_ask_above_a_crashed_book_rests)
{
    // The live 2026-08-30 numbers: ask 11.74 vs best ask 9.75 (+20.4%),
    // bbo mid 5.575. Passive direction, within 30% -> rests.
    EXPECT_EQ(classify_tier(true, 11.74, 9.75, 5.575, kAgg, kPas),
              BboVerdict::Pass);
}

TEST(BboSanity, pathologically_far_passive_ask_is_still_suppressed)
{
    EXPECT_EQ(classify_tier(true, 13.0, 9.75, 5.575, kAgg, kPas),
              BboVerdict::SuppressPassive);  // +33.3%
}

TEST(BboSanity, aggressive_ask_below_the_book_keeps_the_tight_cap)
{
    // The incident class: an ask 20% BELOW best ask would execute at a
    // dislocated price. 10% cap holds regardless of the passive widening.
    EXPECT_EQ(classify_tier(true, 7.8, 9.75, 5.575, kAgg, kPas),
              BboVerdict::SuppressAggressive);  // -20%
    EXPECT_EQ(classify_tier(true, 9.0, 9.75, 5.575, kAgg, kPas),
              BboVerdict::Pass);                // -7.7% within cap
}

TEST(BboSanity, bid_below_bbo_mid_is_passive)
{
    // Sigma-floored bid far under a dust best bid: resting, not sweeping.
    // Live case: bid 1.37 vs best bid 1.40 (mid 5.575) -> passive, rests.
    EXPECT_EQ(classify_tier(false, 1.37, 1.40, 5.575, kAgg, kPas),
              BboVerdict::Pass);
    // And as vol decays the bid walks toward mid: 4.97 vs best bid 1.40
    // is +255% from best bid but still BELOW the midpoint -- the book can
    // trade into it; it must rest (this is the fragility the review
    // named: best-bid-relative passivity would kill it).
    EXPECT_EQ(classify_tier(false, 4.97, 1.40, 5.575, kAgg, kPas),
              BboVerdict::Pass);
}

TEST(BboSanity, bid_above_bbo_mid_is_aggressive)
{
    EXPECT_EQ(classify_tier(false, 6.2, 1.40, 5.575, kAgg, kPas),
              BboVerdict::SuppressAggressive);
}

TEST(BboSanity, degenerate_inputs_pass_through)
{
    EXPECT_EQ(classify_tier(true, 0.0, 9.75, 5.575, kAgg, kPas),
              BboVerdict::Pass);
    EXPECT_EQ(classify_tier(true, 11.74, 0.0, 5.575, kAgg, kPas),
              BboVerdict::Pass);
    // Missing mid: bid passivity falls back to best-bid comparison.
    EXPECT_EQ(classify_tier(false, 1.37, 1.40, 0.0, kAgg, kPas),
              BboVerdict::Pass);
}
