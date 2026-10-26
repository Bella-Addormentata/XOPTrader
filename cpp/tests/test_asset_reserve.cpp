// test_asset_reserve.cpp -- Per-asset reserve arithmetic ([GAS-RESERVE
// 2026-10-24], execution/asset_reserve.hpp).
//
// The numbers are wmilliETH.b-shaped: 1 display unit = 1,000 mojos, and the
// reserve is what the warp rebalancer may unwrap into ETH for Base gas.
//
// ISO/IEC 5055 -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/execution/asset_reserve.hpp>
#include <xop/execution/exposure_gate.hpp>
#include <xop/types.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

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

// -- [PAIRMIN 2026-10-26] the reserve Step 7 sizes CAT pools to ------------

TEST(SizingReserveTest, AnEntryBindsWhateverTheCapSwitchSays) {
    const std::unordered_map<std::string, double> per_asset{{kWmilliEth, 3.0}};
    EXPECT_DOUBLE_EQ(
        sizing_reserve_units(per_asset, 0.1, true, kWmilliEth).value_or(-1.0), 3.0);
    EXPECT_DOUBLE_EQ(
        sizing_reserve_units(per_asset, 0.1, false, kWmilliEth).value_or(-1.0), 3.0);
    // An entry of 0 still binds: Step 7 caps at the balance itself.
    const std::unordered_map<std::string, double> exempt{{kByc, 0.0}};
    EXPECT_DOUBLE_EQ(
        sizing_reserve_units(exempt, 1.0, false, kByc).value_or(-1.0), 0.0);
}

TEST(SizingReserveTest, TheGlobalReserveBindsWhileTheCatCapsAreOn) {
    EXPECT_DOUBLE_EQ(
        sizing_reserve_units({}, 0.1, true, kWmilliEth).value_or(-1.0), 0.1);
    const std::unordered_map<std::string, double> other{{kByc, 3.0}};
    EXPECT_DOUBLE_EQ(
        sizing_reserve_units(other, 0.1, true, kWmilliEth).value_or(-1.0), 0.1)
        << "another asset's entry changes nothing";
    // Caps opted out and no entry: Step 7 leaves the asset uncapped.
    EXPECT_FALSE(sizing_reserve_units({}, 0.1, false, kWmilliEth).has_value());
    EXPECT_FALSE(sizing_reserve_units(other, 0.1, false, kWmilliEth).has_value());
}

TEST(SizingReserveTest, IsStep8sReserveWhereverStep7Caps) {
    // Step 8's gates hold effective_reserve_units back for every asset; a
    // ladder Step 7 sizes to a different figure breaches them as a side.
    const std::unordered_map<std::string, double> per_asset{{kWmilliEth, 3.0}};
    for (const std::string& asset : {kWmilliEth, kByc}) {
        for (const bool caps : {true, false}) {
            const auto sized = sizing_reserve_units(per_asset, 0.1, caps, asset);
            if (sized.has_value()) {
                EXPECT_DOUBLE_EQ(*sized,
                                 effective_reserve_units(per_asset, 0.1, asset));
            }
        }
    }
}

TEST(SizingReserveTest, OneUnitHeldPostsAHalfUnitAskInsteadOfNone) {
    // wmilliETH.b/XCH on 2026-10-26: 1.0 held (1,000 mojos), the global
    // 0.1 reserve, every ask tier up-scaled to the pair's minimum.
    constexpr Mojo kHeld = 1'000;
    const auto units = sizing_reserve_units({}, 0.1, true, kWmilliEth);
    ASSERT_TRUE(units.has_value());
    const Mojo reserve = reserve_units_to_mojos(*units, 1'000);
    ASSERT_EQ(reserve, 100);
    const Mojo budget = cap_pool_above_reserve(kHeld, kHeld, reserve);
    EXPECT_EQ(budget, 900);

    // Before: a 1.0 minimum and the whole balance as the budget kept one
    // 1,000-mojo ask, and Step 8 suppressed it -- 1,000 - 1,000 < 100.
    EXPECT_EQ(xop::tiers_within_budget({1'000, 1'000}, kHeld), 1u);
    EXPECT_TRUE(exposure_breaches_reserve(kHeld, 0, 1'000, reserve));

    // After, at the pair's 0.5 minimum: the floored 500-mojo pool is under
    // the cap, the budget keeps the first up-scaled tier, and Step 8 passes it.
    EXPECT_EQ(cap_pool_above_reserve(500, kHeld, reserve), 500);
    EXPECT_EQ(xop::tiers_within_budget({500, 500, 500, 500, 500, 500}, budget), 1u);
    EXPECT_FALSE(exposure_breaches_reserve(kHeld, 0, 500, reserve));

    // A pool at the whole balance is cut to 900, so tier 0 (the whole pool
    // on a CAT base) still fits instead of being dropped.
    EXPECT_EQ(cap_pool_above_reserve(kHeld, kHeld, reserve), 900);
    EXPECT_EQ(xop::tiers_within_budget({900, 500}, budget), 1u);
    EXPECT_FALSE(exposure_breaches_reserve(kHeld, 0, 900, reserve));
}

// -- [review] the reserve kept SPENDABLE: coins held out of offers ---------

std::string id(char c) { return std::string(64, c); }

TEST(ReserveCoinsTest, TheSmallestCoveringCoinIsHeldBack) {
    // 3-unit reserve: the 4-unit coin, not the 9-unit one, stays out.
    EXPECT_EQ(choose_reserve_coins({{id('a'), 9'000}, {id('b'), 4'000},
                                    {id('c'), 1'000}}, 3'000),
              std::vector<std::string>{id('b')});
    // An exact fit is the smallest covering coin.
    EXPECT_EQ(choose_reserve_coins({{id('a'), 3'000}, {id('b'), 4'000}}, 3'000),
              std::vector<std::string>{id('a')});
}

TEST(ReserveCoinsTest, WithNoCoveringCoinTheFewestLargestCoinsAreHeld) {
    EXPECT_EQ(choose_reserve_coins({{id('a'), 1'000}, {id('b'), 2'000},
                                    {id('c'), 500}}, 3'000),
              (std::vector<std::string>{id('b'), id('a')}));
}

TEST(ReserveCoinsTest, TooLittleHoldsEverythingAndNoReserveHoldsNothing) {
    EXPECT_EQ(choose_reserve_coins({{id('a'), 1'000}, {id('b'), 500}}, 3'000),
              (std::vector<std::string>{id('a'), id('b')}));
    EXPECT_TRUE(choose_reserve_coins({{id('a'), 1'000}}, 0).empty());
    EXPECT_TRUE(choose_reserve_coins({}, 3'000).empty());
    // Junk records are ignored, not chosen.
    EXPECT_EQ(choose_reserve_coins({{"", 9'000}, {id('a'), -5},
                                    {id('b'), 4'000}}, 3'000),
              std::vector<std::string>{id('b')});
}

TEST(ReserveCoinsTest, OnlyOffersSpendingTheWalletExcludeItsCoins) {
    const std::unordered_map<std::string, std::vector<std::string>> held{
        {"7", {id('a')}}};
    // An ask on wmilliETH.b/XCH spends wallet 7 and receives XCH.
    const nlohmann::json ask = {{"7", -2'000}, {"1", 6'600'000'000'000}};
    EXPECT_EQ(reserved_coins_for_offer(ask, held),
              std::vector<std::string>{id('a')});
    // A bid on the same pair RECEIVES from wallet 7: nothing to exclude.
    const nlohmann::json bid = {{"7", 2'000}, {"1", -6'600'000'000'000}};
    EXPECT_TRUE(reserved_coins_for_offer(bid, held).empty());
    EXPECT_TRUE(reserved_coins_for_offer(ask, {}).empty());
    EXPECT_TRUE(reserved_coins_for_offer(nlohmann::json::array(), held).empty());
}

}  // namespace
