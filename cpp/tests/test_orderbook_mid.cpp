// =============================================================================
//  test_orderbook_mid.cpp -- the order-book mid, Layers 1-3
// =============================================================================
//
//  Every numeric fixture in here is either a reconstruction of a real recorded
//  book or a deliberately adversarial one.  The real ones are reconstructed
//  from data/xop_trader.db: offer_log.book_best_bid / book_best_ask record
//  ps.dex_best_bid / dex_best_ask verbatim at the block the offer was created,
//  and snapshots.spread_bps at the same block is
//  (ask - bid) / ((ask + bid) / 2) * 10000 computed from those same two
//  numbers -- verified to agree to the reported precision on all 133
//  same-block observations in the week to 2026-08-01.
//
// =============================================================================

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "xop/execution/orderbook_mid.hpp"

using namespace xop;

namespace {

// Price is carried as mojos-per-unit scaled by kMojosPerXch, matching
// CompetingOffer::price as ingest_competing_offers() receives it.
CompetingOffer mk(Side side, double price, double size_units) {
    CompetingOffer o;
    o.offer_id = std::to_string(price) + (side == Side::Bid ? "b" : "a")
                 + std::to_string(size_units);
    o.side     = side;
    o.price    = static_cast<Mojo>(price * static_cast<double>(kMojosPerXch));
    o.size     = static_cast<Mojo>(size_units
                                   * static_cast<double>(kMojosPerXch));
    return o;
}

// Default schedule -- exactly the shipped defaults, so these tests fail if a
// default is changed without the reasoning being revisited.
OrderbookMidParams defaults() { return OrderbookMidParams{}; }

}  // namespace

// =============================================================================
//  LAYER 1 -- the invariant.  best_bid <= mid <= best_ask, always.
// =============================================================================

// Adversarial depth ratio, 1000:1 with bids dominating.  This is the direction
// that broke BYC/wUSDC.b: bid depth dominating drags the estimate toward
// ask_vwap, which lies at or above best_ask by construction.
TEST(OrderbookMidInvariant, ThousandToOneBidHeavyStaysInsideTheBook) {
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 1.00, 1000.0));
    book.push_back(mk(Side::Bid, 0.98, 1000.0));
    book.push_back(mk(Side::Bid, 0.96, 1000.0));
    book.push_back(mk(Side::Ask, 1.10, 1.0));
    book.push_back(mk(Side::Ask, 1.30, 1.0));
    book.push_back(mk(Side::Ask, 1.60, 1.0));

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "ADV/BIDHEAVY");

    EXPECT_DOUBLE_EQ(r.best_bid, 1.00);
    EXPECT_DOUBLE_EQ(r.best_ask, 1.10);
    EXPECT_GE(r.mid, r.best_bid);
    EXPECT_LE(r.mid, r.best_ask);

    // The unweighted micro-price really does escape the book here -- that is
    // the whole defect, and the fixture is worthless if it does not reproduce it.
    EXPECT_GT(r.microprice, r.best_ask);
}

// The mirror image: 1000:1 with asks dominating drags the estimate toward
// bid_vwap, which lies at or below best_bid.
TEST(OrderbookMidInvariant, ThousandToOneAskHeavyStaysInsideTheBook) {
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 1.00, 1.0));
    book.push_back(mk(Side::Bid, 0.80, 1.0));
    book.push_back(mk(Side::Bid, 0.60, 1.0));
    book.push_back(mk(Side::Ask, 1.10, 1000.0));
    book.push_back(mk(Side::Ask, 1.12, 1000.0));
    book.push_back(mk(Side::Ask, 1.14, 1000.0));

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "ADV/ASKHEAVY");

    EXPECT_DOUBLE_EQ(r.best_bid, 1.00);
    EXPECT_DOUBLE_EQ(r.best_ask, 1.10);
    EXPECT_GE(r.mid, r.best_bid);
    EXPECT_LE(r.mid, r.best_ask);
    EXPECT_LT(r.microprice, r.best_bid);
}

// Sweep the depth ratio across six orders of magnitude in both directions and
// across spreads from 10 bps to 5000 bps.  The invariant is unconditional, so
// this asserts it unconditionally rather than at hand-picked points.
TEST(OrderbookMidInvariant, HoldsAcrossEveryDepthRatioAndSpread) {
    const double ratios[] = {1e-3, 1e-2, 0.1, 1.0, 10.0, 100.0, 1000.0};
    const double spreads_bps[] = {10.0, 50.0, 200.0, 500.0, 800.0,
                                  1259.0, 2114.0, 5000.0};

    for (double ratio : ratios) {
        for (double sp : spreads_bps) {
            const double midpoint = 1.0;
            const double half     = midpoint * sp / 2.0 / 10000.0;
            const double bid      = midpoint - half;
            const double ask      = midpoint + half;

            std::vector<CompetingOffer> book;
            for (int i = 0; i < 5; ++i) {
                book.push_back(mk(Side::Bid, bid * (1.0 - 0.05 * i),
                                  100.0 * ratio));
                book.push_back(mk(Side::Ask, ask * (1.0 + 0.05 * i), 100.0));
            }

            const OrderbookMid r =
                compute_orderbook_mid(book, defaults(), "SWEEP/PAIR");

            ASSERT_GE(r.mid, r.best_bid)
                << "ratio=" << ratio << " spread=" << sp;
            ASSERT_LE(r.mid, r.best_ask)
                << "ratio=" << ratio << " spread=" << sp;
            ASSERT_TRUE(std::isfinite(r.mid))
                << "ratio=" << ratio << " spread=" << sp;
        }
    }
}

// The specific case named in the defect report.  BYC/wUSDC.b published
// mid 1.144407 at block 9086927 while the last recorded third-party BBO was
// bid 1.023622 / ask 1.075000 (offer_log, 2026-07-31 12:23:47).  A mid above
// the best ask is definitionally impossible; assert the estimator cannot
// produce one from that book no matter how lopsided the depth is.
TEST(OrderbookMidInvariant, TheBycCaseCannotProduceAMidAboveTheAsk) {
    constexpr double kBid = 1.023622047244;
    constexpr double kAsk = 1.075000;
    constexpr double kBadMid = 1.144407488285;   // what was actually published

    // ~65 deep bids against ~9 thin asks, the recorded shape of that book.
    std::vector<CompetingOffer> book;
    for (int i = 0; i < 65; ++i) {
        book.push_back(mk(Side::Bid, kBid * (1.0 - 0.002 * i), 500.0));
    }
    for (int i = 0; i < 9; ++i) {
        book.push_back(mk(Side::Ask, kAsk * (1.0 + 0.02 * i), 0.5));
    }

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "BYC/wUSDC.b");

    EXPECT_NEAR(r.best_bid, kBid, 1e-9);
    EXPECT_NEAR(r.best_ask, kAsk, 1e-9);
    EXPECT_LE(r.mid, r.best_ask);
    EXPECT_GE(r.mid, r.best_bid);
    EXPECT_LT(r.mid, kBadMid) << "the published 1.144407 must be unreachable";

    // 490 bps of spread sits in the middle of the 200..800 blend band, so
    // roughly half the micro-price survives -- and at a 1000:1 depth ratio
    // even half of it is enough to leave the book.  That is precisely the
    // case Layer 1 exists for: the clamp binds, and it must be visible.
    EXPECT_NEAR(r.midpoint, (kBid + kAsk) / 2.0, 1e-9);
    EXPECT_NEAR(r.w_micro, 1.0 - (490.0 - 200.0) / 600.0, 0.02);  // ~0.517
    EXPECT_TRUE(r.clamped) << "a binding clamp here is the expected outcome, "
                              "and it is logged at warning level";
    EXPECT_DOUBLE_EQ(r.mid, r.best_ask);

    // Layer 2 alone already removed most of the damage: the raw micro-price
    // wanted 1.1179, the blend brought it to 1.0848, the clamp finished the
    // job at 1.0750.  All three are far below the 1.1444 that was published.
    EXPECT_GT(r.microprice, r.best_ask);
    EXPECT_GT((kBadMid / r.mid - 1.0) * 10000.0, 600.0);  // >600 bps better
}

// The exact book at the sweep block, reconstructed from
// snapshots(9087661, BYC/wUSDC.b) = mid 1.144727818835, spread 1258.964 bps,
// with the published mid sitting on the best ask.  The plain midpoint that
// falls out, 1.076942, is corroborated independently: its implied best bid of
// 1.009156 matches the recorded executable bid depth of 1.000-1.014.
TEST(OrderbookMidLayer2, BycAtTheSweepBlockLandsOnThePlainMidpoint) {
    constexpr double kBid = 1.009156;
    constexpr double kAsk = 1.144728;

    std::vector<CompetingOffer> book;
    for (int i = 0; i < 65; ++i) {
        book.push_back(mk(Side::Bid, kBid * (1.0 - 0.002 * i), 500.0));
    }
    for (int i = 0; i < 9; ++i) {
        book.push_back(mk(Side::Ask, kAsk * (1.0 + 0.02 * i), 0.5));
    }

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "BYC/wUSDC.b");

    // 1259 bps is far beyond microprice_wide_bps, so the micro-price is gone.
    EXPECT_NEAR(r.spread_bps, 1258.96, 1.0);
    EXPECT_DOUBLE_EQ(r.w_micro, 0.0);
    EXPECT_NEAR(r.mid, (kBid + kAsk) / 2.0, 1e-9);
    EXPECT_NEAR(r.mid, 1.076942, 1e-5);

    // And it is a long way below what was actually published.
    EXPECT_LT(r.mid, 1.144728);
    EXPECT_GT((1.144728 / r.mid - 1.0) * 10000.0, 600.0);  // >600 bps better
}

// =============================================================================
//  LAYER 2 -- the blend schedule itself.
// =============================================================================

TEST(OrderbookMidLayer2, WeightIsOneAtOrBelowNarrow) {
    OrderbookMidParams p = defaults();
    // 100 bps book: inside the narrow band.
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 0.995, 10.0));
    book.push_back(mk(Side::Ask, 1.005, 1.0));

    const OrderbookMid r = compute_orderbook_mid(book, p, "TIGHT/PAIR");
    EXPECT_NEAR(r.spread_bps, 100.0, 0.01);
    EXPECT_DOUBLE_EQ(r.w_micro, 1.0);
    // With w = 1 the estimate IS the micro-price (modulo the invariant).
    EXPECT_NEAR(r.mid, std::min(r.microprice, r.best_ask), 1e-12);
}

TEST(OrderbookMidLayer2, WeightIsZeroAtOrAboveWide) {
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 0.96, 10.0));
    book.push_back(mk(Side::Ask, 1.04, 1.0));   // 800 bps exactly

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "WIDE/PAIR");
    EXPECT_NEAR(r.spread_bps, 800.0, 0.01);
    EXPECT_DOUBLE_EQ(r.w_micro, 0.0);
    EXPECT_NEAR(r.mid, r.midpoint, 1e-12);
}

TEST(OrderbookMidLayer2, WeightInterpolatesLinearlyBetweenNarrowAndWide) {
    // 500 bps is the exact midpoint of the 200..800 band -> w = 0.5.
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 0.975, 10.0));
    book.push_back(mk(Side::Ask, 1.025, 1.0));

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "MID/PAIR");
    EXPECT_NEAR(r.spread_bps, 500.0, 0.01);
    EXPECT_NEAR(r.w_micro, 0.5, 1e-6);
    EXPECT_NEAR(r.mid, 0.5 * r.microprice + 0.5 * r.midpoint, 1e-12);
}

// The healthy, profitable pair must keep substantially all of its micro-price
// behaviour.  Its measured 7-day spread distribution is p10 130 / p50 237 /
// p90 315 / p99 365 bps.
TEST(OrderbookMidLayer2, HealthyPairKeepsSubstantiallyAllMicropriceWeight) {
    struct Case { double spread_bps; double min_w; };
    const Case cases[] = {
        {130.0, 1.00},   // p10 -- inside narrow, full weight
        {237.0, 0.93},   // p50 -- 0.938
        {315.0, 0.80},   // p90 -- 0.808
        {365.0, 0.72},   // p99 -- 0.725
    };

    for (const Case& cs : cases) {
        const double half = cs.spread_bps / 2.0 / 10000.0;
        std::vector<CompetingOffer> book;
        book.push_back(mk(Side::Bid, 1.0 - half, 10.0));
        book.push_back(mk(Side::Ask, 1.0 + half, 3.0));

        const OrderbookMid r =
            compute_orderbook_mid(book, defaults(), "XCH/wUSDC.b");
        EXPECT_GE(r.w_micro, cs.min_w)
            << "spread=" << cs.spread_bps << " bps";
    }
}

// XCH/DBX (p50 63 bps, p90 123 bps) sits entirely inside the narrow band.
TEST(OrderbookMidLayer2, TightestPairIsCompletelyUnaffected) {
    for (double sp : {41.0, 63.0, 123.0, 134.0}) {
        const double half = sp / 2.0 / 10000.0;
        std::vector<CompetingOffer> book;
        book.push_back(mk(Side::Bid, 100.0 * (1.0 - half), 10.0));
        book.push_back(mk(Side::Ask, 100.0 * (1.0 + half), 2.0));

        const OrderbookMid r = compute_orderbook_mid(book, defaults(), "XCH/DBX");
        EXPECT_DOUBLE_EQ(r.w_micro, 1.0) << "spread=" << sp;
    }
}

// =============================================================================
//  LAYER 3 -- degenerate and one-sided books.
// =============================================================================

// 14 of 44 recorded XCH/BYC book observations in the week to 2026-08-01 had
// bid == ask exactly.  There is one price and it is unambiguous.
TEST(OrderbookMidLayer3, TouchingBookReportsThatSinglePrice) {
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 1.251979, 10.0));
    book.push_back(mk(Side::Ask, 1.251979, 0.1));

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "XCH/BYC");
    EXPECT_TRUE(r.degenerate);
    EXPECT_NEAR(r.mid, 1.251979, 1e-9);
    EXPECT_GE(r.mid, r.best_bid);
    EXPECT_LE(r.mid, r.best_ask);
    EXPECT_DOUBLE_EQ(r.w_micro, 0.0);
}

// Dexie has no matching engine, so a crossed book is a normal observation.
TEST(OrderbookMidLayer3, CrossedBookUsesTheMidpointAndNeverWeightsDepth) {
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 1.10, 1000.0));
    book.push_back(mk(Side::Ask, 1.00, 1.0));

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "CROSS/PAIR");
    EXPECT_TRUE(r.degenerate);
    EXPECT_NEAR(r.mid, 1.05, 1e-12);
    EXPECT_DOUBLE_EQ(r.w_micro, 0.0);
    EXPECT_LT(r.spread_bps, 0.0);
    // Inside [ask, bid] -- the only interval either side supports.
    EXPECT_GE(r.mid, r.best_ask);
    EXPECT_LE(r.mid, r.best_bid);
}

// A one-sided book bounds fair value; it does not locate it.  Reporting the
// surviving side AS the mid is the fabrication that let the bot quote against
// its own print, because after 5e1ceb4 an empty side means specifically
// "no THIRD-PARTY offer here".
TEST(OrderbookMidLayer3, BidOnlyBookReportsNoMidButStillReportsTheBid) {
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 1.00, 10.0));
    book.push_back(mk(Side::Bid, 0.99, 10.0));

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "ONE/SIDED");
    EXPECT_TRUE(r.one_sided);
    EXPECT_DOUBLE_EQ(r.mid, 0.0);
    EXPECT_DOUBLE_EQ(r.best_bid, 1.00);
    EXPECT_DOUBLE_EQ(r.best_ask, 0.0);
}

TEST(OrderbookMidLayer3, AskOnlyBookReportsNoMidButStillReportsTheAsk) {
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Ask, 1.20, 10.0));

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "ONE/SIDED");
    EXPECT_TRUE(r.one_sided);
    EXPECT_DOUBLE_EQ(r.mid, 0.0);
    EXPECT_DOUBLE_EQ(r.best_ask, 1.20);
    EXPECT_DOUBLE_EQ(r.best_bid, 0.0);
}

TEST(OrderbookMidLayer3, EmptyBookReportsNoMid) {
    const OrderbookMid r =
        compute_orderbook_mid({}, defaults(), "EMPTY/PAIR");
    EXPECT_DOUBLE_EQ(r.mid, 0.0);
    EXPECT_FALSE(r.one_sided);
    EXPECT_EQ(r.bid_levels, 0u);
    EXPECT_EQ(r.ask_levels, 0u);
}

// Zero and negative sizes cannot be weighted and must not reach the VWAP
// denominator, where they would zero or invert it.
TEST(OrderbookMidLayer3, ZeroAndNegativeSizesAreDropped) {
    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 1.00, 10.0));
    CompetingOffer zero = mk(Side::Bid, 1.05, 1.0);
    zero.size = 0;
    book.push_back(zero);
    CompetingOffer neg = mk(Side::Ask, 0.90, 1.0);
    neg.size = -5;
    book.push_back(neg);
    book.push_back(mk(Side::Ask, 1.10, 10.0));

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "DUST/PAIR");
    // The zero-size 1.05 bid and the negative-size 0.90 ask are both gone.
    EXPECT_EQ(r.bid_levels, 1u);
    EXPECT_EQ(r.ask_levels, 1u);
    EXPECT_DOUBLE_EQ(r.best_bid, 1.00);
    EXPECT_DOUBLE_EQ(r.best_ask, 1.10);
    EXPECT_TRUE(std::isfinite(r.mid));
    EXPECT_GE(r.mid, 1.00);
    EXPECT_LE(r.mid, 1.10);
}

TEST(OrderbookMidLayer3, NonPositivePricesAreDropped) {
    std::vector<CompetingOffer> book;
    CompetingOffer zero_px = mk(Side::Bid, 1.0, 1.0);
    zero_px.price = 0;
    book.push_back(zero_px);
    CompetingOffer neg_px = mk(Side::Bid, 1.0, 1.0);
    neg_px.price = -1;
    book.push_back(neg_px);
    book.push_back(mk(Side::Bid, 1.00, 10.0));
    book.push_back(mk(Side::Ask, 1.02, 10.0));

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "BADPX/PAIR");
    EXPECT_EQ(r.bid_levels, 1u);
    EXPECT_DOUBLE_EQ(r.best_bid, 1.00);
    EXPECT_GE(r.mid, 1.00);
    EXPECT_LE(r.mid, 1.02);
}

// =============================================================================
//  Schedule robustness -- the estimator must not divide by zero if it is ever
//  handed a band config that validation would have rejected.
// =============================================================================

TEST(OrderbookMidSchedule, InvertedBandDegradesToAStepNotANaN) {
    OrderbookMidParams p = defaults();
    p.narrow_bps = 800.0;
    p.wide_bps   = 200.0;   // inverted; ConfigError would reject this upstream

    std::vector<CompetingOffer> tight;
    tight.push_back(mk(Side::Bid, 0.995, 10.0));
    tight.push_back(mk(Side::Ask, 1.005, 1.0));
    const OrderbookMid a = compute_orderbook_mid(tight, p, "STEP/PAIR");
    EXPECT_TRUE(std::isfinite(a.mid));
    EXPECT_DOUBLE_EQ(a.w_micro, 1.0);      // 100 bps <= narrow

    std::vector<CompetingOffer> wide;
    wide.push_back(mk(Side::Bid, 0.90, 10.0));
    wide.push_back(mk(Side::Ask, 1.10, 1.0));
    const OrderbookMid b = compute_orderbook_mid(wide, p, "STEP/PAIR");
    EXPECT_TRUE(std::isfinite(b.mid));
    EXPECT_DOUBLE_EQ(b.w_micro, 0.0);      // 2000 bps > narrow
    EXPECT_NEAR(b.mid, b.midpoint, 1e-12);
}

TEST(OrderbookMidSchedule, ZeroDepthParamStillProducesAUsableMid) {
    OrderbookMidParams p = defaults();
    p.depth = 0;   // config validation rejects this; the estimator must cope

    std::vector<CompetingOffer> book;
    book.push_back(mk(Side::Bid, 1.00, 10.0));
    book.push_back(mk(Side::Ask, 1.01, 10.0));

    const OrderbookMid r = compute_orderbook_mid(book, p, "ZDEPTH/PAIR");
    EXPECT_TRUE(std::isfinite(r.mid));
    EXPECT_GE(r.mid, 1.00);
    EXPECT_LE(r.mid, 1.01);
}

// =============================================================================
//  Regression: the healthy pair's actual recorded books.
// =============================================================================
//
//  Twelve XCH/wUSDC.b (bid, ask) pairs read straight out of offer_log for the
//  fortnight to 2026-08-01.  For every one of them the mid must stay inside
//  the book and keep most of its micro-price weight.
// =============================================================================

// =============================================================================
//  Unit normalization -- finding 2 of the 2026-08-01 adversarial review.
// =============================================================================
//
//  CompetingOffer sizes are denominated in the OFFERED asset: bid sizes in
//  quote-asset mojos, ask sizes in base-asset mojos.  On an XCH/CAT pair the
//  quote CAT has 1e3 mojos per unit while base XCH has 1e12, so raw-mojo
//  depths differed by ~1e9 across sides, the micro-price collapsed to
//  bid_vwap, and the Layer-1 invariant clamp -- documented as "always a
//  defect signal" -- fired on essentially every ingest of a normal tight
//  book.  The fixture mk() above sizes BOTH sides at 1e12, which is exactly
//  why the original suite never caught it; these fixtures use the real
//  per-side denominations.
// =============================================================================

namespace {

// XCH/CAT book with the REAL denominations ingest_competing_offers()
// produces: bid sizes in quote-CAT mojos (1e3 per unit), ask sizes in
// base-XCH mojos (1e12 per unit).
CompetingOffer mk_xch_cat(Side side, double price, double size_units) {
    CompetingOffer o;
    o.offer_id = std::to_string(price) + (side == Side::Bid ? "b" : "a")
                 + std::to_string(size_units);
    o.side     = side;
    o.price    = static_cast<Mojo>(price * static_cast<double>(kMojosPerXch));
    o.size     = static_cast<Mojo>(size_units
                                   * (side == Side::Bid ? 1e3 : 1e12));
    return o;
}

OrderbookMidParams xch_cat_params() {
    OrderbookMidParams p;  // shipped blend schedule
    p.base_mojos_per_unit  = 1'000'000'000'000LL;  // XCH
    p.quote_mojos_per_unit = 1'000LL;              // CAT
    return p;
}

}  // namespace

// A normal tight XCH/wUSDC.b book (~89 bps, well inside the narrow band, so
// w_micro = 1 and the mid IS the micro-price).  With raw-mojo depths the
// ask side outweighed the bid side ~4e8:1 here, the micro-price collapsed
// to bid_vwap (2.228125, below best_bid) and the invariant clamp pinned the
// mid at best_bid.  Normalized, the two sides carry comparable value
// (800 vs ~675 quote units) and the mid lands strictly inside the book.
TEST(OrderbookMidUnits, XchCatTightBookDoesNotCollapseToBidVwapOrClamp) {
    std::vector<CompetingOffer> book;
    book.push_back(mk_xch_cat(Side::Bid, 2.230, 500.0));  // 500 wUSDC
    book.push_back(mk_xch_cat(Side::Bid, 2.225, 300.0));  // 300 wUSDC
    book.push_back(mk_xch_cat(Side::Ask, 2.250, 200.0));  // 200 XCH
    book.push_back(mk_xch_cat(Side::Ask, 2.255, 100.0));  // 100 XCH

    const OrderbookMid r =
        compute_orderbook_mid(book, xch_cat_params(), "XCH/wUSDC.b");

    // ~89 bps: micro-price used whole.
    EXPECT_NEAR(r.spread_bps, 89.29, 0.1);
    EXPECT_DOUBLE_EQ(r.w_micro, 1.0);

    // The finding itself: no clamp on a normal tight book, and the mid does
    // NOT collapse to the bid side.
    EXPECT_FALSE(r.clamped);
    const double bid_vwap = (2.230 * 500.0 + 2.225 * 300.0) / 800.0;
    EXPECT_GT(r.microprice, bid_vwap);
    EXPECT_GT(r.mid, r.best_bid);
    EXPECT_LT(r.mid, r.best_ask);

    // Hand-computed micro-price in quote units:
    //   bid_depth = 500 + 300                       = 800     quote units
    //   ask_depth = 200*2.250 + 100*2.255           = 675.5   quote units
    //   bid_vwap  = (2.230*500 + 2.225*300) / 800   = 2.228125
    //   ask_vwap  = (2.250*450 + 2.255*225.5)/675.5 = 2.2516691...
    //   micro     = (675.5*bid_vwap + 800*ask_vwap) / 1475.5
    //             = 2.2408937...
    EXPECT_NEAR(r.mid, 2.24089, 5e-4);
}

// Same shape at a two-orders-of-magnitude pseudo-price (XCH/DBX-like) --
// the normalization must not depend on px being near 1.  Book value is
// balanced by construction, so the micro-price must sit near the middle
// rather than on either VWAP.
TEST(OrderbookMidUnits, LargePseudoPriceStaysBalancedAndInsideTheBook) {
    std::vector<CompetingOffer> book;
    book.push_back(mk_xch_cat(Side::Bid, 99.5, 1000.0));  // 1000 DBX
    book.push_back(mk_xch_cat(Side::Ask, 100.5, 10.0));   // 10 XCH ~ 1005 DBX
    const OrderbookMid r =
        compute_orderbook_mid(book, xch_cat_params(), "XCH/DBX");

    EXPECT_DOUBLE_EQ(r.w_micro, 1.0);   // 100 bps, inside narrow
    EXPECT_FALSE(r.clamped);
    EXPECT_GT(r.mid, r.best_bid);
    EXPECT_LT(r.mid, r.best_ask);
    // Depths 1000 vs 1005 quote units: micro ~ the plain midpoint, not a
    // VWAP.  micro = (1005*99.5 + 1000*100.5)/2005 = 99.99875...
    EXPECT_NEAR(r.mid, 100.0, 0.01);
}

// CAT/CAT pairs (BYC/wUSDC.b) have 1e3 mojos per unit on BOTH sides; the
// normalization must reduce to the near-symmetric case there, not distort it.
TEST(OrderbookMidUnits, CatCatPairRemainsSymmetricUnderNormalization) {
    OrderbookMidParams p;
    p.base_mojos_per_unit  = 1'000LL;
    p.quote_mojos_per_unit = 1'000LL;

    std::vector<CompetingOffer> book;
    CompetingOffer b = mk_xch_cat(Side::Bid, 1.005, 100.0);  // quote mojos, 1e3
    CompetingOffer a = mk_xch_cat(Side::Bid, 1.015, 100.0);
    a.side = Side::Ask;
    a.size = static_cast<Mojo>(100.0 * 1e3);  // base mojos at 1e3 per unit
    book.push_back(b);
    book.push_back(a);

    const OrderbookMid r = compute_orderbook_mid(book, p, "BYC/wUSDC.b");

    EXPECT_FALSE(r.clamped);
    EXPECT_GE(r.mid, r.best_bid);
    EXPECT_LE(r.mid, r.best_ask);
    // 100 quote units vs 100 * 1.015 = 101.5 quote units: near-equal weights,
    // micro = (101.5*1.005 + 100*1.015)/201.5 = 1.00996...
    EXPECT_NEAR(r.mid, 1.010, 1e-3);
}

TEST(OrderbookMidRegression, EveryRecordedHealthyBookKeepsItsMicropriceAndItsInvariant) {
    struct Rec { double bid; double ask; };
    const Rec recs[] = {
        {1.403713, 1.447412}, {1.393913, 1.424182}, {1.379803, 1.404842},
        {1.362998, 1.387530}, {1.362998, 1.386514}, {1.391084, 1.420000},
        {1.439691, 1.460000}, {1.400779, 1.443523}, {1.387032, 1.422158},
        {1.411142, 1.440000}, {1.431240, 1.460000}, {1.445571, 1.495439},
    };

    for (const Rec& rec : recs) {
        // Modest, realistic asymmetry: 3:1 bid-heavy.
        std::vector<CompetingOffer> book;
        for (int i = 0; i < 5; ++i) {
            book.push_back(mk(Side::Bid, rec.bid * (1.0 - 0.003 * i), 30.0));
            book.push_back(mk(Side::Ask, rec.ask * (1.0 + 0.003 * i), 10.0));
        }
        const OrderbookMid r =
            compute_orderbook_mid(book, defaults(), "XCH/wUSDC.b");

        ASSERT_GE(r.mid, r.best_bid) << rec.bid << "/" << rec.ask;
        ASSERT_LE(r.mid, r.best_ask) << rec.bid << "/" << rec.ask;
        // Recorded spreads here run 140-339 bps, all inside the blend band.
        ASSERT_GT(r.w_micro, 0.70) << rec.bid << "/" << rec.ask;
        ASSERT_FALSE(r.clamped) << rec.bid << "/" << rec.ask;
    }
}
