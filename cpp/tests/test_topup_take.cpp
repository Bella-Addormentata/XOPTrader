// test_topup_take.cpp -- The warp rebalancer's Dexie buys, as the engine
// books them ([MILLIETH-TOPUP 2026-10-24], accounting/topup_take.hpp).
//
// The numbers are the cheapest live wmilliETH.b ask on 2026-10-24:
// 0.547 wmilliETH.b for 1.8277 XCH, taken with a 0.0001 XCH fee.
//
// ISO/IEC 5055 -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/accounting/topup_take.hpp>

#include <limits>
#include <string>

namespace {

using namespace xop::accounting;
using xop::Mojo;

TopupTakeRow live_take() {
    TopupTakeRow r;
    r.id         = 1;
    r.trade_id   = std::string(64, 'd');
    r.asset_id   =
        "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6";
    r.cat_mojos  = 547;
    r.xch_mojos  = 1'827'700'000'000;
    r.fee_mojos  = 100'000'000;
    r.created_at = "2026-10-24T12:00:00+00:00";
    return r;
}

TEST(TopupTakeTest, ATakeBooksTheEngineTakeShape) {
    const auto legs = classify_topup_take(live_take());
    ASSERT_TRUE(legs.valid);
    EXPECT_EQ(legs.event_id, "take:" + std::string(64, 'd'));
    EXPECT_EQ(legs.base_delta, 547);
    EXPECT_EQ(legs.quote_delta, -1'827'700'000'000);
    EXPECT_EQ(legs.fee_delta, -100'000'000);
    // 3.3413 XCH per wmilliETH.b, in XCH mojos.
    EXPECT_EQ(legs.price_mojos, 3'341'316'270'567);
}

TEST(TopupTakeTest, NoFeeIsAZeroFeeLeg) {
    auto r = live_take();
    r.fee_mojos = 0;
    const auto legs = classify_topup_take(r);
    ASSERT_TRUE(legs.valid);
    EXPECT_EQ(legs.fee_delta, 0);
}

TEST(TopupTakeTest, MalformedRowsBookNothing) {
    const auto bad = [](auto mutate) {
        auto r = live_take();
        mutate(r);
        return classify_topup_take(r).valid;
    };
    EXPECT_FALSE(bad([](TopupTakeRow& r) { r.trade_id = "0x" + r.trade_id; }));
    EXPECT_FALSE(bad([](TopupTakeRow& r) { r.trade_id = std::string(64, 'D'); }));
    EXPECT_FALSE(bad([](TopupTakeRow& r) { r.asset_id = "xch"; }));
    EXPECT_FALSE(bad([](TopupTakeRow& r) { r.cat_mojos = 0; }));
    EXPECT_FALSE(bad([](TopupTakeRow& r) { r.xch_mojos = -1; }));
    EXPECT_FALSE(bad([](TopupTakeRow& r) { r.fee_mojos = -1; }));
}

TEST(TopupTakeTest, ThePriceRoundsWithoutOverflowNearTheLimit) {
    // [review] xch * 1000 fits but adding half the divisor would not.
    constexpr Mojo kMax = std::numeric_limits<Mojo>::max();
    auto r = live_take();
    r.fee_mojos = 0;
    r.xch_mojos = kMax / 1000;
    r.cat_mojos = 2'000;
    const auto legs = classify_topup_take(r);
    ASSERT_TRUE(legs.valid);
    const Mojo scaled = (kMax / 1000) * 1000;
    EXPECT_EQ(legs.price_mojos,
              scaled / 2'000 + ((scaled % 2'000) * 2 >= 2'000 ? 1 : 0));
}

TEST(TopupTakeTest, ALegIsJudgedByTheConfirmationBlockWhenKnown) {
    // Submitted 12:00, opening observed 12:01 at block 500, trade landed in
    // block 502: the opening holds the PRE-take balance, so the leg books.
    EXPECT_TRUE(take_leg_after_opening(502, "2026-10-24T12:00:00+00:00",
                                       500, "2026-10-24T12:01:00Z"));
    // Landed in 499 (before the opening read): already inside it.
    EXPECT_FALSE(take_leg_after_opening(499, "2026-10-24T12:00:00+00:00",
                                        500, "2026-10-24T12:01:00Z"));
    // A tie is treated as inside, the safe direction.
    EXPECT_FALSE(take_leg_after_opening(500, "2026-10-24T12:00:00+00:00",
                                        500, "2026-10-24T11:00:00Z"));
}

TEST(TopupTakeTest, WithoutABlockTheSubmitTimeDecides) {
    EXPECT_TRUE(take_leg_after_opening(0, "2026-10-24T12:00:00+00:00",
                                       500, "2026-10-24T11:00:00Z"));
    EXPECT_FALSE(take_leg_after_opening(0, "2026-10-24T12:00:00+00:00",
                                        500, "2026-10-24T12:00:00Z"));
    EXPECT_FALSE(take_leg_after_opening(502, "2026-10-24T12:00:00+00:00",
                                        0, "2026-10-24T12:01:00Z"));
}

TEST(TopupTakeTest, OverflowingAmountsBookNothing) {
    constexpr Mojo kMax = std::numeric_limits<Mojo>::max();
    auto r = live_take();
    r.xch_mojos = kMax - 5;
    EXPECT_FALSE(classify_topup_take(r).valid);    // xch + fee overflows
    r = live_take();
    r.fee_mojos = 0;
    r.xch_mojos = kMax / 1000 + 1;
    EXPECT_FALSE(classify_topup_take(r).valid);    // the price overflows
}

}  // namespace
