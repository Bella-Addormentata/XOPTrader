// test_target_gap.cpp -- the target-gap bid floor (xop/strategy/target_gap.hpp).
//
// [TARGET-GAP 2026-10-26] On 2026-10-26 a 100 wUSDC.b bridge took wUSDC.b to
// 59% of the portfolio against a 20% target while XCH sat at 13.8% against
// 50%, and XCH/wUSDC.b posted nothing: the strategy's bid was q_max 20 - 34.76
// XCH held = 0 and the single-CAT cap zeroed the ask.  The floor is
// min(base deficit, quote excess); these tests pin that arithmetic, that it
// overshoots neither coin, and that every unusable input fails closed to 0
// (today's sizing).  The Step 4 wiring is pinned in test_sizing_wiring.cpp.

#include <gtest/gtest.h>

#include <xop/strategy/target_gap.hpp>

#include <limits>
#include <string>
#include <vector>

namespace {

using xop::strategy::target_gap::Holding;
using xop::strategy::target_gap::Valuation;
using xop::strategy::target_gap::bid_floor_units;
using xop::strategy::target_gap::value_targeted;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

Holding held(const std::string& key, double target, double xch_value)
{
    Holding h{};
    h.key = key;
    h.target = target;
    h.valued = true;
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

}  // namespace

TEST(TargetGapValuation, SumsTheTargetedCoins) {
    const Valuation v = value_targeted(live_book());
    ASSERT_TRUE(v.ok);
    EXPECT_NEAR(v.total_xch, 251.9, 1e-9);
    EXPECT_DOUBLE_EQ(v.xch_by_key.at("WUSDC.B"), 148.9);
    EXPECT_DOUBLE_EQ(v.target_by_key.at("XCH"), 0.5);
}

TEST(TargetGapValuation, AnyUnvaluedCoinMakesItUnusable) {
    std::vector<Holding> book = live_book();
    book[3].valued = false;  // DBX: no validated balance, or a balance and no mark
    EXPECT_FALSE(value_targeted(book).ok);
}

TEST(TargetGapValuation, BadValuesAndTargetsMakeItUnusable) {
    for (const double bad_value : {kNaN, kInf, -1.0}) {
        SCOPED_TRACE(bad_value);
        std::vector<Holding> book = live_book();
        book[1].xch_value = bad_value;
        EXPECT_FALSE(value_targeted(book).ok);
    }
    for (const double bad_target : {kNaN, -0.01, 1.01}) {
        SCOPED_TRACE(bad_target);
        std::vector<Holding> book = live_book();
        book[1].target = bad_target;
        EXPECT_FALSE(value_targeted(book).ok);
    }
}

TEST(TargetGapValuation, EmptyOrZeroPortfolioIsUnusable) {
    EXPECT_FALSE(value_targeted({}).ok);
    EXPECT_FALSE(value_targeted({held("XCH", 0.5, 0.0), held("WUSDC.B", 0.5, 0.0)}).ok);
}

TEST(TargetGapFloor, XchWusdcIsTheXchDeficit) {
    // XCH deficit 0.5 x 251.9 - 34.76 = 91.19; wUSDC.b excess 148.9 - 0.2 x
    // 251.9 = 98.52.  The smaller binds: 91.19 XCH, one XCH per unit.
    const Valuation v = value_targeted(live_book());
    EXPECT_NEAR(bid_floor_units(v, "XCH", "WUSDC.B", 1.0), 91.19, 1e-9);
}

TEST(TargetGapFloor, XchBycIsTheBycExcess) {
    // BYC excess 28.0 - 0.05 x 251.9 = 15.405 < the XCH deficit: selling all
    // of it brings BYC exactly to its target and no lower.
    const Valuation v = value_targeted(live_book());
    EXPECT_NEAR(bid_floor_units(v, "XCH", "BYC", 1.0), 15.405, 1e-9);
}

TEST(TargetGapFloor, ZeroUnlessTheBidMovesBothCoinsTowardTarget) {
    const Valuation v = value_targeted(live_book());
    // DBX is at 15.3% against 15%: a tiny excess, a tiny floor.
    EXPECT_NEAR(bid_floor_units(v, "XCH", "DBX", 1.0), 38.6 - 0.15 * 251.9, 1e-9);
    // BYC/wUSDC.b's bid buys BYC, which is already over its target.
    EXPECT_EQ(bid_floor_units(v, "BYC", "WUSDC.B", 1.0 / 1.65), 0.0);
    // wmilliETH.b/XCH's bid spends XCH, which is under its target.
    EXPECT_EQ(bid_floor_units(v, "WMILLIETH.B", "XCH", 1.64), 0.0);
}

TEST(TargetGapFloor, ConvertsXchToBaseUnits) {
    // Once XCH is over its target, wmilliETH.b/XCH bids for the deficit:
    // XCH excess 160 - 0.5 x 200 = 60, wmilliETH.b deficit 0.1 x 200 - 2 = 18
    // XCH, at 1.6 XCH per wmilliETH.b = 11.25 units.
    const Valuation v = value_targeted({held("XCH", 0.5, 160.0), held("WMILLIETH.B", 0.1, 2.0),
                                        held("WUSDC.B", 0.4, 38.0)});
    ASSERT_TRUE(v.ok);
    EXPECT_NEAR(bid_floor_units(v, "WMILLIETH.B", "XCH", 1.6), 11.25, 1e-9);
}

TEST(TargetGapFloor, FailsClosedOnUnusableInputs) {
    const Valuation v = value_targeted(live_book());
    EXPECT_EQ(bid_floor_units(Valuation{}, "XCH", "WUSDC.B", 1.0), 0.0);
    EXPECT_EQ(bid_floor_units(v, "XCH", "OTHER", 1.0), 0.0) << "an untargeted quote";
    EXPECT_EQ(bid_floor_units(v, "OTHER", "WUSDC.B", 1.0), 0.0) << "an untargeted base";
    EXPECT_EQ(bid_floor_units(v, "XCH", "XCH", 1.0), 0.0);
    for (const double bad_rate : {0.0, -1.0, kNaN, kInf}) {
        SCOPED_TRACE(bad_rate);
        EXPECT_EQ(bid_floor_units(v, "XCH", "WUSDC.B", bad_rate), 0.0);
    }
}
