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
