#include <gtest/gtest.h>

#include <xop/execution/coin_manager.hpp>

namespace {

using xop::Mojo;
using xop::execution::CoinInfo;
using xop::execution::CoinManager;

TEST(CoinManagerTest, PoolReadyBandUsesHalfToDoubleTarget) {
    const Mojo target = 500'000'000'000LL;

    EXPECT_FALSE(CoinManager::is_pool_ready_coin(249'999'999'999LL, target));
    EXPECT_TRUE(CoinManager::is_pool_ready_coin(250'000'000'000LL, target));
    EXPECT_TRUE(CoinManager::is_pool_ready_coin(500'000'000'000LL, target));
    EXPECT_TRUE(CoinManager::is_pool_ready_coin(1'000'000'000'000LL, target));
    EXPECT_FALSE(CoinManager::is_pool_ready_coin(1'000'000'000'001LL, target));
}

TEST(CoinManagerTest, CountPoolReadyCoinsIgnoresTinyAndOversizedCoins) {
    const Mojo target = 500'000'000'000LL;

    // Only the amount participates in the pool-ready predicate; the identity
    // fields are left value-initialised.  Naming just `.amount` in a braced
    // initialiser trips -Wmissing-field-initializers under GCC.
    const auto coin = [](Mojo amount) {
        CoinInfo c{};
        c.amount = amount;
        return c;
    };

    const std::vector<CoinInfo> coins = {
        coin(530'532'825LL),
        coin(66'982'204'562LL),
        coin(250'000'000'000LL),
        coin(500'000'000'000LL),
        coin(750'000'000'000LL),
        coin(1'000'000'000'000LL),
        coin(13'000'000'000'000LL),
    };

    EXPECT_EQ(CoinManager::count_pool_ready_coins(coins, target), 4U);
}

TEST(CoinManagerTest, PlanSplitFallsBackToHalvesInsideTheDeadlockWindow) {
    // [COIN-POOL-DEADLOCK 2026-08-23] target 1.5 XCH: a ~2.0 XCH coin funds
    // only a batch-1 target split whose 0.5 change is below the [0.75, 3.0]
    // band -- "no improvement" refused every such coin forever while the
    // live wallet held nothing larger, so ensure_split failed 76 times and
    // spendable XCH pinned at zero.  The amount below is the literal coin
    // from the incident log ("Largest free coin: 1999990919092 mojos").
    const Mojo target = 1'500'000'000'000LL;
    const Mojo amount = 1'999'990'919'092LL;

    const auto plan = CoinManager::plan_split_for_coin(amount, 10, target, 0);
    EXPECT_EQ(plan.batch, 1);
    EXPECT_EQ(plan.split_amount, amount / 2);
    // Both the split coin and its change land inside the pool-ready band, so
    // the ready count goes 1 -> 2 and no dust is created.
    EXPECT_TRUE(CoinManager::is_pool_ready_coin(plan.split_amount, target));
    EXPECT_TRUE(
        CoinManager::is_pool_ready_coin(amount - plan.split_amount, target));
}

TEST(CoinManagerTest, PlanSplitPrefersTargetDenominationWhenItImproves) {
    // A genuinely large coin keeps the historical behaviour: a full batch of
    // target-denomination coins, capped by `needed`.
    const Mojo target = 1'500'000'000'000LL;
    const auto plan = CoinManager::plan_split_for_coin(
        44'000'000'000'000LL, 10, target, 0);
    EXPECT_EQ(plan.split_amount, target);
    EXPECT_EQ(plan.batch, 10);
}

TEST(CoinManagerTest, PlanSplitJustAboveTheWindowUsesTheTargetPath) {
    // At 1.5x target the batch-1 target split's change re-enters the band,
    // so the historical path resumes and the fallback stays unused.
    const Mojo target = 1'500'000'000'000LL;
    const auto plan = CoinManager::plan_split_for_coin(
        2'250'000'000'000LL, 10, target, 0);
    EXPECT_EQ(plan.split_amount, target);
    EXPECT_EQ(plan.batch, 1);
}

TEST(CoinManagerTest, PlanSplitRefusesCoinsThatCannotFundOneTargetCoin) {
    const Mojo target = 1'500'000'000'000LL;
    // Below target: no plan.  Halving a sub-target coin would leave both
    // halves under the band floor, and the coin itself is already ready.
    EXPECT_EQ(CoinManager::plan_split_for_coin(
                  1'400'000'000'000LL, 10, target, 0).batch, 0);
    // The fee counts against the funding line: target+5 with fee 10 cannot
    // fund one target coin, so there is no plan.
    EXPECT_EQ(CoinManager::plan_split_for_coin(target + 5, 1, target, 10).batch,
              0);
}

TEST(CoinManagerTest, PlanSplitHalfFallbackAccountsForTheFee) {
    // In the deadlock window with a fee: the halves are of (amount - fee),
    // and both must stay in band -- the fee is spent, not dusted.
    const Mojo target = 1'500'000'000'000LL;
    const Mojo amount = 2'000'000'000'000LL;
    const Mojo fee    = 100'000'000LL;

    const auto plan = CoinManager::plan_split_for_coin(amount, 10, target, fee);
    EXPECT_EQ(plan.batch, 1);
    EXPECT_EQ(plan.split_amount, (amount - fee) / 2);
    EXPECT_TRUE(CoinManager::is_pool_ready_coin(plan.split_amount, target));
    EXPECT_TRUE(CoinManager::is_pool_ready_coin(
        amount - fee - plan.split_amount, target));
}

TEST(CoinManagerTest, SplitImprovesPoolReadyCountRejectsNoOpExactTargetSplit) {
    const Mojo target = 2'000'000'000'000LL;

    EXPECT_FALSE(CoinManager::split_improves_pool_ready_count(
        target, 1, target, 0));
}

TEST(CoinManagerTest, SplitImprovesPoolReadyCountAcceptsPoolReadyChange) {
    const Mojo target = 500'000'000'000LL;

    EXPECT_TRUE(CoinManager::split_improves_pool_ready_count(
        750'000'000'000LL, 1, target, 0));
}

TEST(CoinManagerTest, SplitImprovesPoolReadyCountRejectsDustChange) {
    const Mojo target = 500'000'000'000LL;

    EXPECT_FALSE(CoinManager::split_improves_pool_ready_count(
        600'000'000'000LL, 1, target, 0));
}

}  // namespace
