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
    const std::vector<CoinInfo> coins = {
        {.amount = 530'532'825LL},
        {.amount = 66'982'204'562LL},
        {.amount = 250'000'000'000LL},
        {.amount = 500'000'000'000LL},
        {.amount = 750'000'000'000LL},
        {.amount = 1'000'000'000'000LL},
        {.amount = 13'000'000'000'000LL},
    };

    EXPECT_EQ(CoinManager::count_pool_ready_coins(coins, target), 4U);
}

}  // namespace
