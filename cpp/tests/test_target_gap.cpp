// test_target_gap.cpp -- the target-gap bid floor (xop/strategy/target_gap.hpp).
//
// [TARGET-GAP 2026-10-26] On 2026-10-26 a 100 wUSDC.b bridge took wUSDC.b to
// 59% of the portfolio against a 20% target while XCH sat at 13.8% against
// 50%, and XCH/wUSDC.b posted nothing: the strategy's bid was q_max 20 - 34.76
// XCH held = 0 and the single-CAT cap zeroed the ask.  A pair's floor is
// min(base deficit, quote excess), and the pairs share each coin's gap; these
// tests pin that arithmetic, that the floors of all pairs together take no
// coin past its target, and that every unusable input fails closed to 0
// (today's sizing).  The Step 4 wiring is pinned in test_sizing_wiring.cpp.

#include <gtest/gtest.h>

#include <xop/peg_registry.hpp>  // to_mojo_checked, Step 6's range
#include <xop/strategy/target_gap.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using xop::strategy::target_gap::Holding;
using xop::strategy::target_gap::PairCoins;
using xop::strategy::target_gap::Valuation;
using xop::strategy::target_gap::deficit_xch;
using xop::strategy::target_gap::excess_xch;
using xop::strategy::target_gap::floor_units;
using xop::strategy::target_gap::kMaxFloorBaseMojos;
using xop::strategy::target_gap::shared_bid_floors_xch;
using xop::strategy::target_gap::unshared_bid_floor_xch;
using xop::strategy::target_gap::value_targeted;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::uint64_t kMaxAge = 20;  // strategy.pace_max_balance_age_blocks' default

Holding held(const std::string& key, double target, double xch_value)
{
    Holding h{};
    h.key = key;
    h.target = target;
    h.balance_known = true;
    h.balance_age_blocks = 1;
    h.priced = true;
    h.xch_value = xch_value;
    return h;
}

// The live book of 2026-10-26 18:48Z in XCH (total ~251.9): XCH 34.76 (13.8%),
// wUSDC.b 148.9 (59.1%), BYC 28.0 (11.1%), DBX 38.6 (15.3%), wmilliETH.b 1.64
// (0.65%), against targets 0.5 / 0.2 / 0.05 / 0.15 / 0.1.
std::vector<Holding> live_book()
{
    return {held("XCH", 0.5, 34.76), held("WUSDC.B", 0.2, 148.9), held("BYC", 0.05, 28.0),
            held("DBX", 0.15, 38.6), held("WMILLIETH.B", 0.1, 1.64)};
}

// The enabled pairs of that day, in cycle order.
std::vector<PairCoins> live_pairs()
{
    return {{"XCH", "WUSDC.B"}, {"XCH", "BYC"}, {"XCH", "DBX"}, {"BYC", "WUSDC.B"}, {"WMILLIETH.B", "XCH"}};
}

}  // namespace

TEST(TargetGapValuation, SumsTheTargetedCoins) {
    const Valuation v = value_targeted(live_book(), kMaxAge);
    ASSERT_TRUE(v.ok) << v.unusable_reason;
    EXPECT_NEAR(v.total_xch, 251.9, 1e-9);
    EXPECT_DOUBLE_EQ(v.xch_by_key.at("WUSDC.B"), 148.9);
    EXPECT_DOUBLE_EQ(v.target_by_key.at("XCH"), 0.5);
    EXPECT_TRUE(v.unusable_reason.empty());
}

TEST(TargetGapValuation, AMissingStaleOrUnpricedBalanceMakesItUnusable) {
    {
        std::vector<Holding> book = live_book();
        book[3].balance_known = false;  // DBX: no validated balance cached
        const Valuation v = value_targeted(book, kMaxAge);
        EXPECT_FALSE(v.ok);
        EXPECT_EQ(v.unusable_reason, "DBX: no validated wallet balance");
    }
    {
        std::vector<Holding> book = live_book();
        book[4].balance_age_blocks = kMaxAge;  // as old as allowed: still usable
        EXPECT_TRUE(value_targeted(book, kMaxAge).ok);
        book[4].balance_age_blocks = kMaxAge + 1;  // e.g. its only pair disabled
        const Valuation v = value_targeted(book, kMaxAge);
        EXPECT_FALSE(v.ok);
        EXPECT_EQ(v.unusable_reason, "WMILLIETH.B: wallet balance more than 20 blocks old");
    }
    {
        std::vector<Holding> book = live_book();
        book[2].priced = false;  // BYC: a balance and no XCH mark
        const Valuation v = value_targeted(book, kMaxAge);
        EXPECT_FALSE(v.ok);
        EXPECT_EQ(v.unusable_reason, "BYC: no XCH price");
    }
}

TEST(TargetGapValuation, BadValuesAndTargetsMakeItUnusable) {
    for (const double bad_value : {kNaN, kInf, -1.0}) {
        SCOPED_TRACE(bad_value);
        std::vector<Holding> book = live_book();
        book[1].xch_value = bad_value;
        EXPECT_FALSE(value_targeted(book, kMaxAge).ok);
    }
    for (const double bad_target : {kNaN, -0.01, 1.01}) {
        SCOPED_TRACE(bad_target);
        std::vector<Holding> book = live_book();
        book[1].target = bad_target;
        EXPECT_FALSE(value_targeted(book, kMaxAge).ok);
    }
}

TEST(TargetGapValuation, EmptyOrZeroPortfolioIsUnusable) {
    EXPECT_EQ(value_targeted({}, kMaxAge).unusable_reason, "no asset_target_allocations");
    const Valuation zero = value_targeted({held("XCH", 0.5, 0.0), held("WUSDC.B", 0.5, 0.0)}, kMaxAge);
    EXPECT_FALSE(zero.ok);
    EXPECT_EQ(zero.unusable_reason, "the targeted coins are worth nothing");
}

TEST(TargetGapFloor, DeficitAndExcessAreTheGapsToTarget) {
    const Valuation v = value_targeted(live_book(), kMaxAge);
    EXPECT_NEAR(deficit_xch(v, "XCH"), 0.5 * 251.9 - 34.76, 1e-9);
    EXPECT_EQ(excess_xch(v, "XCH"), 0.0);
    EXPECT_NEAR(excess_xch(v, "WUSDC.B"), 148.9 - 0.2 * 251.9, 1e-9);
    EXPECT_EQ(deficit_xch(v, "WUSDC.B"), 0.0);
    EXPECT_EQ(deficit_xch(v, "OTHER"), 0.0);
    EXPECT_EQ(excess_xch(Valuation{}, "WUSDC.B"), 0.0);
}

TEST(TargetGapFloor, XchWusdcIsTheXchDeficit) {
    // XCH deficit 0.5 x 251.9 - 34.76 = 91.19; wUSDC.b excess 148.9 - 0.2 x
    // 251.9 = 98.52.  The smaller binds.
    const Valuation v = value_targeted(live_book(), kMaxAge);
    EXPECT_NEAR(unshared_bid_floor_xch(v, "XCH", "WUSDC.B"), 91.19, 1e-9);
}

TEST(TargetGapFloor, XchBycIsTheBycExcess) {
    // BYC excess 28.0 - 0.05 x 251.9 = 15.405 < the XCH deficit: selling all
    // of it brings BYC exactly to its target and no lower.
    const Valuation v = value_targeted(live_book(), kMaxAge);
    EXPECT_NEAR(unshared_bid_floor_xch(v, "XCH", "BYC"), 15.405, 1e-9);
}

TEST(TargetGapFloor, ZeroUnlessTheBidMovesBothCoinsTowardTarget) {
    const Valuation v = value_targeted(live_book(), kMaxAge);
    // DBX is at 15.3% against 15%: a tiny excess, a tiny floor.
    EXPECT_NEAR(unshared_bid_floor_xch(v, "XCH", "DBX"), 38.6 - 0.15 * 251.9, 1e-9);
    // BYC/wUSDC.b's bid buys BYC, which is already over its target.
    EXPECT_EQ(unshared_bid_floor_xch(v, "BYC", "WUSDC.B"), 0.0);
    // wmilliETH.b/XCH's bid spends XCH, which is under its target.
    EXPECT_EQ(unshared_bid_floor_xch(v, "WMILLIETH.B", "XCH"), 0.0);
}

TEST(TargetGapFloor, UnusableInputsGiveNoFloor) {
    const Valuation v = value_targeted(live_book(), kMaxAge);
    EXPECT_EQ(unshared_bid_floor_xch(Valuation{}, "XCH", "WUSDC.B"), 0.0);
    EXPECT_EQ(unshared_bid_floor_xch(v, "XCH", "OTHER"), 0.0) << "an untargeted quote";
    EXPECT_EQ(unshared_bid_floor_xch(v, "OTHER", "WUSDC.B"), 0.0) << "an untargeted base";
    EXPECT_EQ(unshared_bid_floor_xch(v, "XCH", "XCH"), 0.0);
}

TEST(TargetGapShared, ThePairsBuyingXchShareItsDeficit) {
    // Unshared, the three XCH-buying pairs want 91.19 + 15.405 + 0.815 =
    // 107.41 XCH of an XCH deficit of 91.19: each is scaled by 91.19/107.41.
    const Valuation v = value_targeted(live_book(), kMaxAge);
    const std::vector<double> floors = shared_bid_floors_xch(v, live_pairs());
    ASSERT_EQ(floors.size(), 5u);
    const double want_wusdc = 91.19;
    const double want_byc = 15.405;
    const double want_dbx = 38.6 - 0.15 * 251.9;
    const double scale = 91.19 / (want_wusdc + want_byc + want_dbx);
    EXPECT_NEAR(floors[0], want_wusdc * scale, 1e-9);
    EXPECT_NEAR(floors[1], want_byc * scale, 1e-9);
    EXPECT_NEAR(floors[2], want_dbx * scale, 1e-9);
    EXPECT_EQ(floors[3], 0.0);
    EXPECT_EQ(floors[4], 0.0);
    // Filling all three takes XCH exactly to its target.
    EXPECT_NEAR(floors[0] + floors[1] + floors[2], deficit_xch(v, "XCH"), 1e-9);
}

TEST(TargetGapShared, ThePairsSpendingOneCoinShareItsExcess) {
    // Total 100: XCH 10 and BYC 10 are 20 under their 0.3 targets, wUSDC.b 50
    // is 30 over its 0.2 and DBX 30 is 10 over its 0.2.  XCH/wUSDC.b and
    // BYC/wUSDC.b each want min(20, 30) = 20 -- 40 of wUSDC.b's 30 of excess
    // -- so both are scaled by 30/40, and filling both takes wUSDC.b exactly
    // to its target.
    const Valuation v = value_targeted({held("XCH", 0.3, 10.0), held("BYC", 0.3, 10.0),
                                        held("WUSDC.B", 0.2, 50.0), held("DBX", 0.2, 30.0)}, kMaxAge);
    ASSERT_TRUE(v.ok);
    const std::vector<double> floors = shared_bid_floors_xch(v, {{"XCH", "WUSDC.B"}, {"BYC", "WUSDC.B"}});
    ASSERT_EQ(floors.size(), 2u);
    EXPECT_NEAR(floors[0], 15.0, 1e-9);
    EXPECT_NEAR(floors[1], 15.0, 1e-9);
    EXPECT_NEAR(floors[0] + floors[1], excess_xch(v, "WUSDC.B"), 1e-9);
}

TEST(TargetGapShared, APairTakesTheSmallerOfItsTwoShares) {
    // The book above plus XCH/DBX, which wants min(20, 10) = 10.  XCH's two
    // buyers want 30 of its deficit of 20 (scale 2/3); wUSDC.b's two spenders
    // want 40 of its excess of 30 (scale 3/4).  XCH/wUSDC.b is in both and
    // takes 2/3; XCH/DBX takes 2/3 (DBX is not contended); BYC/wUSDC.b takes
    // 3/4 (BYC is not).
    const Valuation v = value_targeted({held("XCH", 0.3, 10.0), held("BYC", 0.3, 10.0),
                                        held("WUSDC.B", 0.2, 50.0), held("DBX", 0.2, 30.0)}, kMaxAge);
    ASSERT_TRUE(v.ok);
    const std::vector<double> floors =
        shared_bid_floors_xch(v, {{"XCH", "WUSDC.B"}, {"XCH", "DBX"}, {"BYC", "WUSDC.B"}});
    ASSERT_EQ(floors.size(), 3u);
    EXPECT_NEAR(floors[0], 20.0 * 2.0 / 3.0, 1e-9);
    EXPECT_NEAR(floors[1], 10.0 * 2.0 / 3.0, 1e-9);
    EXPECT_NEAR(floors[2], 20.0 * 0.75, 1e-9);
    EXPECT_NEAR(floors[0] + floors[1], deficit_xch(v, "XCH"), 1e-9) << "XCH bought exactly to target";
    EXPECT_LE(floors[0] + floors[2], excess_xch(v, "WUSDC.B")) << "wUSDC.b spent no further than target";
}

TEST(TargetGapShared, NoCoinIsTakenPastItsTarget) {
    // Every pair between five coins, on three books: for each coin, the
    // floors buying it add up to no more than its deficit and the floors
    // spending it to no more than its excess.
    const std::vector<std::string> coins = {"XCH", "WUSDC.B", "BYC", "DBX", "WMILLIETH.B"};
    std::vector<PairCoins> pairs;
    for (const auto& base : coins) {
        for (const auto& quote : coins) {
            if (base != quote) {
                pairs.push_back({base, quote});
            }
        }
    }
    const std::vector<std::vector<Holding>> books = {
        live_book(),
        {held("XCH", 0.5, 160.0), held("WUSDC.B", 0.2, 10.0), held("BYC", 0.05, 1.0),
         held("DBX", 0.15, 20.0), held("WMILLIETH.B", 0.1, 9.0)},
        {held("XCH", 0.2, 5.0), held("WUSDC.B", 0.2, 5.0), held("BYC", 0.2, 80.0),
         held("DBX", 0.2, 70.0), held("WMILLIETH.B", 0.2, 0.0)},
    };
    for (std::size_t b = 0; b < books.size(); ++b) {
        SCOPED_TRACE(b);
        const Valuation v = value_targeted(books[b], kMaxAge);
        ASSERT_TRUE(v.ok);
        const std::vector<double> floors = shared_bid_floors_xch(v, pairs);
        for (const auto& coin : coins) {
            SCOPED_TRACE(coin);
            double bought = 0.0;
            double spent = 0.0;
            for (std::size_t i = 0; i < pairs.size(); ++i) {
                EXPECT_GE(floors[i], 0.0);
                EXPECT_LE(floors[i], unshared_bid_floor_xch(v, pairs[i].base_key, pairs[i].quote_key));
                if (pairs[i].base_key == coin) bought += floors[i];
                if (pairs[i].quote_key == coin) spent += floors[i];
            }
            EXPECT_LE(bought, deficit_xch(v, coin) + 1e-9);
            EXPECT_LE(spent, excess_xch(v, coin) + 1e-9);
        }
    }
}

TEST(TargetGapShared, OrderDoesNotMatter) {
    const Valuation v = value_targeted(live_book(), kMaxAge);
    const std::vector<PairCoins> forward = live_pairs();
    const std::vector<PairCoins> reverse(forward.rbegin(), forward.rend());
    const std::vector<double> a = shared_bid_floors_xch(v, forward);
    const std::vector<double> b = shared_bid_floors_xch(v, reverse);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_DOUBLE_EQ(a[i], b[a.size() - 1 - i]) << i;
    }
}

TEST(TargetGapShared, ALonePairKeepsItsWholeFloor) {
    const Valuation v = value_targeted(live_book(), kMaxAge);
    const std::vector<double> floors = shared_bid_floors_xch(v, {{"XCH", "BYC"}});
    ASSERT_EQ(floors.size(), 1u);
    EXPECT_NEAR(floors[0], 15.405, 1e-9);
    EXPECT_EQ(shared_bid_floors_xch(v, {}).size(), 0u);
}

TEST(TargetGapShared, AnUnusableValuationGivesNoFloors) {
    const std::vector<double> floors = shared_bid_floors_xch(Valuation{}, live_pairs());
    ASSERT_EQ(floors.size(), 5u);
    for (const double f : floors) {
        EXPECT_EQ(f, 0.0);
    }
}

TEST(TargetGapUnits, ConvertsXchToBaseUnits) {
    // Once XCH is over its target, wmilliETH.b/XCH bids for the deficit:
    // XCH excess 160 - 0.5 x 200 = 60, wmilliETH.b deficit 0.1 x 200 - 2 = 18
    // XCH, at 1.6 XCH per wmilliETH.b = 11.25 units.
    const Valuation v = value_targeted({held("XCH", 0.5, 160.0), held("WMILLIETH.B", 0.1, 2.0),
                                        held("WUSDC.B", 0.4, 38.0)}, kMaxAge);
    ASSERT_TRUE(v.ok);
    EXPECT_NEAR(floor_units(unshared_bid_floor_xch(v, "WMILLIETH.B", "XCH"), 1.6, 1e3), 11.25, 1e-9);
    EXPECT_DOUBLE_EQ(floor_units(91.19, 1.0, 1e12), 91.19) << "an XCH base: one XCH per unit";
}

TEST(TargetGapUnits, CapsTheSizeWhereStep6CanConvertIt) {
    // 20,000 XCH and none of a 1,000-mojo CAT marked at 1e-12 XCH per unit,
    // 50/50: a 10,000 XCH floor is 1e16 units, 1e19 mojos -- past the Mojo
    // maximum, and Step 6 rounds the size to mojos before any cap.
    const Valuation v = value_targeted({held("XCH", 0.5, 20'000.0), held("CAT", 0.5, 0.0)}, kMaxAge);
    ASSERT_TRUE(v.ok);
    const double floor_xch = unshared_bid_floor_xch(v, "CAT", "XCH");
    ASSERT_DOUBLE_EQ(floor_xch, 10'000.0);
    EXPECT_FALSE(xop::to_mojo_checked(floor_xch / 1e-12 * 1e3).has_value()) << "uncapped, it overflows";
    const double units = floor_units(floor_xch, 1e-12, 1e3);
    EXPECT_DOUBLE_EQ(units, kMaxFloorBaseMojos / 1e3);
    ASSERT_TRUE(xop::to_mojo_checked(units * 1e3).has_value());
    // With room for the pool's later scale-ups: the ratio rebalancer's 3x and
    // the market allocator's 1.5x.
    EXPECT_TRUE(xop::to_mojo_checked(units * 1e3 * 3.0 * 1.5).has_value());
    // An XCH base caps at 2^53 mojos, about 9,007 XCH: far above six 5-XCH tiers.
    EXPECT_NEAR(floor_units(50'000.0, 1.0, 1e12), 9007.199254740992, 1e-9);
    // A rate so small the division overflows still gives a capped size.
    EXPECT_DOUBLE_EQ(floor_units(1e300, 1e-300, 1e3), kMaxFloorBaseMojos / 1e3);
    // Below the cap nothing changes.
    EXPECT_DOUBLE_EQ(floor_units(91.19, 1.0, 1e12), 91.19);
}

TEST(TargetGapUnits, UnusableRatesAndFloorsGiveZero) {
    for (const double bad_rate : {0.0, -1.0, kNaN, kInf}) {
        SCOPED_TRACE(bad_rate);
        EXPECT_EQ(floor_units(91.19, bad_rate, 1e3), 0.0);
        EXPECT_EQ(floor_units(91.19, 1.0, bad_rate), 0.0) << "as mojos per unit";
    }
    for (const double bad_floor : {0.0, -1.0, kNaN, kInf}) {
        SCOPED_TRACE(bad_floor);
        EXPECT_EQ(floor_units(bad_floor, 1.0, 1e12), 0.0);
    }
}
