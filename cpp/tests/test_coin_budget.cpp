// test_coin_budget.cpp -- the split of a coin several pair sides spend
// (xop/strategy/coin_budget.hpp).
//
// [COINBUDGET 2026-10-30] XCH/wUSDC.b, XCH/BYC and XCH/DBX asks sell the same
// XCH and the wmilliETH.b/XCH bids spend it too; Step 7 sized each side as if
// it held the whole coin and Step 8 posted first come, first served.  These
// tests pin the split: nothing is capped while the claims fit; a short coin
// gives each side its weighted share, never more than it asked for, passes
// what a side leaves to the others, and never hands out more than the
// budget.  The engine wiring is pinned in test_sizing_wiring.cpp.

#include <gtest/gtest.h>

#include <xop/strategy/coin_budget.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace {

using xop::Mojo;
using xop::strategy::coin_budget::Claim;
using xop::strategy::coin_budget::split;

Mojo sum(const std::vector<Mojo>& v) {
    Mojo s = 0;
    for (const Mojo x : v) s += x;
    return s;
}

TEST(CoinBudget, NothingIsCappedWhileTheClaimsFit) {
    EXPECT_FALSE(split(100, {{40.0, 40.0}, {60.0, 60.0}}).has_value())
        << "exactly the budget fits";
    EXPECT_FALSE(split(100, {{10.0, 10.0}}).has_value());
    EXPECT_FALSE(split(100, {}).has_value());
    EXPECT_FALSE(split(-1, {{10.0, 10.0}}).has_value()) << "a negative budget is no budget";
}

TEST(CoinBudget, InProportionToTheClaimsEverySideGetsTheSameFraction) {
    // 92 XCH: XCH/DBX asks want 60, XCH/wUSDC.b asks 40, wmilliETH.b/XCH bids
    // 8.7 -- 108.7 in all, so each gets 92 / 108.7 of its claim.
    const Mojo xch = 1'000'000'000'000LL;
    const std::vector<Claim> claims{{60.0 * xch, 60.0 * xch},
                                    {40.0 * xch, 40.0 * xch},
                                    {8.7 * xch, 8.7 * xch}};
    const auto shares = split(92 * xch, claims);
    ASSERT_TRUE(shares.has_value());
    ASSERT_EQ(shares->size(), 3u);
    const double fraction = 92.0 / 108.7;
    for (std::size_t i = 0; i < claims.size(); ++i) {
        EXPECT_NEAR(static_cast<double>((*shares)[i]), claims[i].demand * fraction, 1.0);
        EXPECT_LE(static_cast<double>((*shares)[i]), claims[i].demand);
    }
    EXPECT_LE(sum(*shares), 92 * xch);
    EXPECT_GE(sum(*shares), 92 * xch - 3) << "only the flooring is left over";
}

TEST(CoinBudget, WhatASideLeavesPassesToTheOthers) {
    // Equal weights: a third each would give the small claim more than it
    // asked for, so it keeps its 10 and the other two split the 90 left.
    const auto shares = split(100, {{10.0, 1.0}, {80.0, 1.0}, {80.0, 1.0}});
    ASSERT_TRUE(shares.has_value());
    EXPECT_EQ((*shares)[0], 10);
    EXPECT_EQ((*shares)[1], 45);
    EXPECT_EQ((*shares)[2], 45);
    // Two levels: 25 each covers the 10-claim; 32.5 each of the 65 left then
    // covers the 30-claim, and the 100-claim gets the last 35.
    const auto two_levels = split(75, {{10.0, 1.0}, {30.0, 1.0}, {100.0, 1.0}});
    ASSERT_TRUE(two_levels.has_value());
    EXPECT_EQ((*two_levels)[0], 10);
    EXPECT_EQ((*two_levels)[1], 30);
    EXPECT_EQ((*two_levels)[2], 35);
}

TEST(CoinBudget, AClaimThatAskedForNothingOrHasNoWeightGetsNothing) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto shares = split(50, {{0.0, 1.0}, {-5.0, 1.0}, {nan, 1.0},
                                   {40.0, 0.0}, {60.0, 1.0}});
    ASSERT_TRUE(shares.has_value());
    EXPECT_EQ((*shares)[0], 0);
    EXPECT_EQ((*shares)[1], 0);
    EXPECT_EQ((*shares)[2], 0);
    EXPECT_EQ((*shares)[3], 0) << "no weight: nothing while the coin is short";
    EXPECT_EQ((*shares)[4], 50);
}

TEST(CoinBudget, AnEmptyCoinGivesEverySideZero) {
    const auto shares = split(0, {{10.0, 10.0}, {20.0, 20.0}});
    ASSERT_TRUE(shares.has_value());
    EXPECT_EQ((*shares)[0], 0);
    EXPECT_EQ((*shares)[1], 0);
}

TEST(CoinBudget, SharesNeverSumPastTheBudget) {
    // Thirds of a budget that does not divide evenly floor down.
    const auto shares = split(100, {{50.0, 1.0}, {50.0, 1.0}, {50.0, 1.0}});
    ASSERT_TRUE(shares.has_value());
    EXPECT_EQ((*shares)[0], 33);
    EXPECT_EQ((*shares)[1], 33);
    EXPECT_EQ((*shares)[2], 33);
    // Large balances at proportional weights.
    const Mojo big = 93'000'000'000'000LL;
    const auto large = split(big, {{7.1e13, 7.1e13}, {3.3e13, 3.3e13},
                                   {1.9e13, 1.9e13}, {9.0e12, 9.0e12}});
    ASSERT_TRUE(large.has_value());
    EXPECT_LE(sum(*large), big);
}

TEST(CoinBudget, PastTwoToTheFiftyThirdTheSharesStillFitTheBudget) {
    // Two halves of 9007199254740995 both round up to 4503599627370498 in
    // double: the second takes only what the first left.
    const Mojo odd = 9'007'199'254'740'995LL;
    const auto halves = split(odd, {{1e16, 1e16}, {1e16, 1e16}});
    ASSERT_TRUE(halves.has_value());
    EXPECT_LE(sum(*halves), odd);
    EXPECT_GE((*halves)[0], 0);
    EXPECT_GE((*halves)[1], 0);
    // At Mojo max the double budget is 2^63, which no Mojo holds.
    const Mojo max = std::numeric_limits<Mojo>::max();
    const auto at_max = split(max, {{1e19, 1e19}, {1e17, 1e17}});
    ASSERT_TRUE(at_max.has_value());
    EXPECT_GE((*at_max)[0], 0);
    EXPECT_GE((*at_max)[1], 0);
    EXPECT_LE((*at_max)[0], max - (*at_max)[1]);
}

}  // namespace
