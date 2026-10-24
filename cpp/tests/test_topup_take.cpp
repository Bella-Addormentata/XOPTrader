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

TEST(TopupTakeTest, ALegBooksOnlyAfterAnOpeningObservedBeforeTheSubmit) {
    EXPECT_TRUE(take_leg_after_opening("2026-10-24T12:00:00+00:00",
                                       "2026-10-24T11:00:00Z"));
    // A tie, or an opening observed after the submit (the take may already
    // be inside it): skipped -- under-booking is the safe direction.
    EXPECT_FALSE(take_leg_after_opening("2026-10-24T12:00:00+00:00",
                                        "2026-10-24T12:00:00Z"));
    EXPECT_FALSE(take_leg_after_opening("2026-10-24T12:00:00+00:00",
                                        "2026-10-24T12:01:00Z"));
    EXPECT_FALSE(take_leg_after_opening("", "2026-10-24T11:00:00Z"));
}

TEST(TopupTakeTest, OverflowingAmountsBookNothing) {
    constexpr Mojo kMax = std::numeric_limits<Mojo>::max();
    auto r = live_take();
    r.xch_mojos = kMax - 5;
    EXPECT_FALSE(classify_topup_take(r).valid);    // xch + fee overflows
    r = live_take();
    r.fee_mojos = 0;
    r.xch_mojos = kMax / 1000 + 1;
    r.cat_mojos = 1;
    EXPECT_FALSE(classify_topup_take(r).valid);    // the price overflows
}

TEST(TopupTakeTest, AFeeCountsWhileItsBlockIsInsideTheBudgetWindow) {
    // FeeTracker::prune drops entries below now - window.
    EXPECT_TRUE(take_fee_in_window(9'000, 10'000, 1'662));
    EXPECT_TRUE(take_fee_in_window(10'000 - 1'662, 10'000, 1'662));
    EXPECT_FALSE(take_fee_in_window(10'000 - 1'663, 10'000, 1'662));
    EXPECT_TRUE(take_fee_in_window(0, 10'000, 1'662));      // unknown: counts
    EXPECT_TRUE(take_fee_in_window(5, 100, 1'662));         // young chain
}

TEST(TopupTakeTest, ALargeBuyWhosePriceFitsIsBooked) {
    // [review] 10,000 XCH for 3,000,000 mojos: xch * 1000 overflows, the
    // price (3,333,333,333,333.33 -> ...333) does not.
    auto r = live_take();
    r.fee_mojos = 0;
    r.xch_mojos = 10'000LL * 1'000'000'000'000LL;
    r.cat_mojos = 3'000'000;
    const auto legs = classify_topup_take(r);
    ASSERT_TRUE(legs.valid);
    EXPECT_EQ(legs.price_mojos, 3'333'333'333'333);
    r.cat_mojos = 6;                                  // ...666.67 rounds up
    EXPECT_EQ(classify_topup_take(r).price_mojos,
              1'666'666'666'666'666'667);
}

TEST(TopupTakeTest, LongDivisionMatchesTheDirectFormula) {
    for (const Mojo xch : {Mojo{1}, Mojo{999}, Mojo{1'827'700'000'000},
                           Mojo{7'777'777'777}}) {
        for (const Mojo cat : {Mojo{1}, Mojo{2}, Mojo{3}, Mojo{547},
                               Mojo{2'000}, Mojo{999'999}}) {
            auto r = live_take();
            r.xch_mojos = xch;
            r.cat_mojos = cat;
            const Mojo scaled = xch * 1000;
            const Mojo q = scaled / cat, m = scaled % cat;
            EXPECT_EQ(classify_topup_take(r).price_mojos,
                      q + (m >= cat - m ? 1 : 0)) << xch << "/" << cat;
        }
    }
}

}  // namespace
