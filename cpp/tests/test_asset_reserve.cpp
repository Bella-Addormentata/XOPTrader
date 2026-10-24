// test_asset_reserve.cpp -- Per-asset reserve arithmetic ([GAS-RESERVE
// 2026-10-24], execution/asset_reserve.hpp).
//
// The numbers are wmilliETH.b-shaped: 1 display unit = 1,000 mojos, and the
// reserve is what the warp rebalancer may unwrap into ETH for Base gas.
//
// ISO/IEC 5055 -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/execution/asset_reserve.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>

namespace {

using namespace xop::execution;
using xop::Mojo;

const std::string kWmilliEth =
    "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6";
const std::string kByc =
    "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac";

TEST(AssetReserveTest, AnEntryReplacesTheGlobalReserveForItsAssetOnly) {
    const std::unordered_map<std::string, double> per_asset{{kWmilliEth, 3.0}};
    EXPECT_DOUBLE_EQ(effective_reserve_units(per_asset, 1.0, kWmilliEth), 3.0);
    EXPECT_DOUBLE_EQ(effective_reserve_units(per_asset, 1.0, kByc), 1.0);
    EXPECT_DOUBLE_EQ(effective_reserve_units({}, 1.0, kWmilliEth), 1.0);
}

TEST(AssetReserveTest, AnEntryCanAlsoLowerTheReserve) {
    // "Replaces", not "raises": an operator can exempt an asset.
    const std::unordered_map<std::string, double> per_asset{{kByc, 0.0}};
    EXPECT_DOUBLE_EQ(effective_reserve_units(per_asset, 1.0, kByc), 0.0);
}

TEST(AssetReserveTest, UnitsConvertToMojosRoundedToNearest) {
    EXPECT_EQ(reserve_units_to_mojos(3.0, 1'000), 3'000);
    EXPECT_EQ(reserve_units_to_mojos(0.0026, 1'000), 3);   // 2.6 -> 3
    EXPECT_EQ(reserve_units_to_mojos(0.0024, 1'000), 2);   // 2.4 -> 2
    EXPECT_EQ(reserve_units_to_mojos(1.0, 1'000'000'000'000LL),
              1'000'000'000'000LL);
}

TEST(AssetReserveTest, NoReserveForNonsenseInputs) {
    EXPECT_EQ(reserve_units_to_mojos(0.0, 1'000), 0);
    EXPECT_EQ(reserve_units_to_mojos(-2.0, 1'000), 0);
    EXPECT_EQ(reserve_units_to_mojos(std::nan(""), 1'000), 0);
    EXPECT_EQ(reserve_units_to_mojos(3.0, 0), 0);
    EXPECT_EQ(reserve_units_to_mojos(3.0, -1'000), 0);
}

TEST(AssetReserveTest, AnOutOfRangeReserveSaturatesToKeepEverything) {
    // Never a wrapped negative, which would read as "no reserve".
    constexpr Mojo kMax = std::numeric_limits<Mojo>::max();
    EXPECT_EQ(reserve_units_to_mojos(1e9, 1'000'000'000'000LL), kMax);
    EXPECT_EQ(reserve_units_to_mojos(
                  std::numeric_limits<double>::infinity(), 1'000), kMax);
}

TEST(AssetReserveTest, ThePoolIsCappedToConfirmedMinusReserve) {
    // 5 units held, 3 reserved: at most 2 may be offered.
    EXPECT_EQ(cap_pool_above_reserve(4'000, 5'000, 3'000), 2'000);
    // A pool already inside the budget is left alone (never raised).
    EXPECT_EQ(cap_pool_above_reserve(1'500, 5'000, 3'000), 1'500);
}

TEST(AssetReserveTest, NothingIsOfferedAtOrBelowTheReserve) {
    EXPECT_EQ(cap_pool_above_reserve(4'000, 3'000, 3'000), 0);
    EXPECT_EQ(cap_pool_above_reserve(4'000, 2'000, 3'000), 0);
}

TEST(AssetReserveTest, NoReserveCapsAtTheBalanceAndClampsNegatives) {
    EXPECT_EQ(cap_pool_above_reserve(9'000, 5'000, 0), 5'000);
    EXPECT_EQ(cap_pool_above_reserve(-10, 5'000, 0), 0);
    EXPECT_EQ(cap_pool_above_reserve(9'000, -5, 0), 0);
}

}  // namespace
