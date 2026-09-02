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
//  2026-09-02 -- FOUR ASSERTIONS IN THIS FILE WERE INVERTED ON PURPOSE.
// =============================================================================
//
//  The estimator was corrected to weight the TOUCH PRICES rather than each
//  side's top-N VWAP (see orderbook_mid.hpp).  The micro-price is now a convex
//  combination of best_bid and best_ask and is therefore INTERIOR TO THE BOOK
//  BY CONSTRUCTION.  Assertions that pinned the old, unbounded behaviour were
//  asserting the defect, and have been flipped:
//
//    1. ThousandToOneBidHeavyStaysInsideTheBook
//         was  EXPECT_GT(r.microprice, r.best_ask)
//         now  strictly interior, and provably so
//    2. ThousandToOneAskHeavyStaysInsideTheBook
//         was  EXPECT_LT(r.microprice, r.best_bid)
//         now  strictly interior, and provably so
//    3. TheBycCaseCannotProduceAMidAboveTheAsk
//         was  EXPECT_TRUE(r.clamped) and EXPECT_DOUBLE_EQ(r.mid, r.best_ask)
//         now  !clamped, mid ~= 1.06257
//    4. OrderbookMidLayer2.WeightIsOneAtOrBelowNarrow
//         was  EXPECT_NEAR(r.mid, std::min(r.microprice, r.best_ask), 1e-12)
//         now  EXPECT_NEAR(r.mid, r.microprice, 1e-12) + EXPECT_FALSE(clamped)
//         The std::min was written for the clamping regime and is BLIND: on
//         any mutation that pushes the micro-price above the ask, mid clamps
//         to best_ask and std::min(micro, best_ask) evaluates to best_ask
//         too, so the assertion passes on the broken estimator.  It was
//         missed in the first pass over this file.
//
//  (3) is the one that matters, and it is an IMPROVEMENT, not a regression.
//  That book's truth was ~$1.01.  The old code clamped to the best ask,
//  1.07500; the corrected estimator returns ~1.06257, which is ~115 bps
//  CLOSER to the truth.  The clamp no longer binds there because nothing is
//  out of bounds to clamp.
//
//  A note on the vacuity that let the original defect survive: the sweep test
//  HoldsAcrossEveryDepthRatioAndSpread asserted only best_bid <= mid <=
//  best_ask, which the Layer-1 clamp guarantees UNCONDITIONALLY -- it could
//  never fail, whatever the estimator did.  It now also asserts the clamp did
//  not fire, AND that the micro-price itself is interior.  MEASURED AGAINST
//  THE UNFIXED ESTIMATOR, the clamp assertion failed at 26 of the 56 grid
//  points:
//
//      spread   10 bps -- all 7 depth ratios clamped
//      spread   50 bps -- all 7 depth ratios clamped
//      spread  200 bps -- 6 of 7 clamped (only ratio 1.0, balanced, survived)
//      spread  500 bps -- 6 of 7 clamped (only ratio 1.0, balanced, survived)
//      spread  800/1259/2114/5000 bps -- 0 of 7 clamped
//
//  The four wide spreads are all at or beyond microprice_wide_bps, where
//  w_micro is 0 and the micro-price is discarded entirely -- so the CLAMP
//  assertion cannot test the estimator at those 28 points at all.  Of the 28
//  points where it can, 26 escaped the book.  Note the direction: the escapes
//  are worst at the TIGHTEST spreads, which is the opposite of what the
//  spread taper was built to defend against.
//
//  THE MICRO-PRICE INTERIORITY ASSERTIONS IN THAT SWEEP ARE THE STRICTLY
//  STRONGER GUARD, NOT DECORATION.  Measured against the same unfixed
//  estimator they failed at 50 of the 56 grid points -- 26 below the bid and
//  24 above the ask -- versus 26 for the clamp assertion, and the two sets
//  are disjoint per point.  24 of those 50 are at the four WIDE spreads,
//  where w_micro is 0, the micro-price is discarded by the blend, and a
//  broken estimator therefore cannot trip the clamp at all: 28 grid points
//  that the clamp assertion cannot test and these assertions catch 24 of.
//  They are the only coverage the estimator has at spreads >=
//  microprice_wide_bps.  Do not delete them as redundant over the clamp
//  assertion; the clamp assertion is the subset.
//
//  A NOTE ON THE ACCEPTANCE METRIC.  The centre shift this fix produces is
//  S/(1+R), not spread/2 -- see the header of orderbook_mid.hpp and
//  OrderbookMidAcceptance below, which pins the formula against the
//  estimator so the wrong number cannot be re-derived from a memory of the
//  original write-up.
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

    // INVERTED 2026-09-02.  This used to read EXPECT_GT(r.microprice,
    // r.best_ask) -- it asserted that the estimator escaped the book, which
    // was the defect, not a requirement.  Weighting the touch prices makes the
    // micro-price a convex combination of best_bid and best_ask, so on this
    // 1000:1 bid-heavy book it is dragged hard toward the ask but cannot pass
    // it:  bid_depth = 3000 quote units, ask_depth = 1.10+1.30+1.60 = 4.00,
    //      micro = (4.00*1.00 + 3000*1.10) / 3004 = 1.099867
    // i.e. within 13 bps of the ask, and still strictly inside.
    EXPECT_LT(r.microprice, r.best_ask);
    EXPECT_GT(r.microprice, r.best_bid);
    EXPECT_NEAR(r.microprice, 1.099867, 1e-5);
    EXPECT_FALSE(r.clamped);
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

    // INVERTED 2026-09-02, mirror of the above.  Was EXPECT_LT(r.microprice,
    // r.best_bid).  bid_depth = 3.00 quote units, ask_depth = 1000*(1.10 +
    // 1.12 + 1.14) = 3360, so
    //      micro = (3360*1.00 + 3.00*1.10) / 3363 = 1.000089
    // pinned just above the bid, and strictly inside.
    EXPECT_GT(r.microprice, r.best_bid);
    EXPECT_LT(r.microprice, r.best_ask);
    EXPECT_NEAR(r.microprice, 1.000089, 1e-5);
    EXPECT_FALSE(r.clamped);
}

// Sweep the depth ratio across six orders of magnitude in both directions and
// across spreads from 10 bps to 5000 bps.  The invariant is unconditional, so
// this asserts it unconditionally rather than at hand-picked points.
//
// THE CLAMP ASSERTION IS THE POINT OF THIS TEST.  Asserting only
// best_bid <= mid <= best_ask here is VACUOUS: Layer 1 clamps to exactly that
// interval as its last action, so those two bounds hold no matter how wrong
// the estimator is, and for months they did exactly that.  ASSERT_FALSE(
// r.clamped) is what actually tests the estimator, because it asserts the
// clamp had NOTHING TO DO.
//
// Verified non-vacuous by construction and by measurement: against the
// pre-2026-09-02 estimator this assertion failed at 26 of the 56 grid points
// (and at 26 of the 28 where w_micro > 0 -- see the file header for the
// per-spread breakdown).  The grid carries a real d_b: each side's five levels
// step 5% away from the touch, so bid_vwap sits 10% below best_bid.  If this
// test ever passes with the touch prices swapped back to the VWAPs, the grid
// has lost its depth ladder and must be rebuilt before it is trusted again.
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
            // EXPECT, not ASSERT.  A regression here needs its full
            // SIGNATURE to be diagnosable -- escapes at every ratio across
            // the four narrow spreads and none at the four wide ones is what
            // identifies the defect as unboundedness rather than a broken
            // fixture.  ASSERT aborts the whole test at the first point and
            // reports 1 of up to 26 (measured).  Nothing below dereferences
            // anything a failure would invalidate, so there is no
            // abort-safety reason to ASSERT here.
            EXPECT_FALSE(r.clamped)
                << "the estimator left the book and Layer 1 had to drag it "
                   "back: ratio=" << ratio << " spread=" << sp
                << " micro=" << r.microprice << " bid=" << r.best_bid
                << " ask=" << r.best_ask << " w_micro=" << r.w_micro;

            // NOT belt and braces -- this is the STRONGER assertion of the
            // two, and the only one with any force at the four wide spreads.
            // There w_micro is 0, the blend discards the micro-price, and the
            // clamp above therefore cannot fire however wrong the estimator
            // is: 28 of the 56 grid points are untested by the clamp check
            // and tested only here.  Measured against the pre-2026-09-02
            // estimator this pair failed at 50 of 56 points versus 26 for the
            // clamp check (disjoint per point), 24 of the 50 being at those
            // wide spreads.  Deleting these as redundant would silently drop
            // half the grid.
            EXPECT_GE(r.microprice, r.best_bid)
                << "ratio=" << ratio << " spread=" << sp
                << " micro=" << r.microprice << " w_micro=" << r.w_micro;
            EXPECT_LE(r.microprice, r.best_ask)
                << "ratio=" << ratio << " spread=" << sp
                << " micro=" << r.microprice << " w_micro=" << r.w_micro;
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
    // roughly half the micro-price survives.
    EXPECT_NEAR(r.midpoint, (kBid + kAsk) / 2.0, 1e-9);
    EXPECT_NEAR(r.w_micro, 1.0 - (490.0 - 200.0) / 600.0, 0.02);  // ~0.517

    // ===================================================================
    //  INVERTED 2026-09-02.  THIS IS AN IMPROVEMENT, NOT A REGRESSION.
    // ===================================================================
    //  These two lines used to read
    //
    //      EXPECT_TRUE(r.clamped);
    //      EXPECT_DOUBLE_EQ(r.mid, r.best_ask);
    //
    //  and were described as "a binding clamp here is the expected
    //  outcome".  It was not an expected outcome, it was the defect
    //  announcing itself.  With the touch-price micro-price:
    //
    //      bid_depth = 5 x 500                       = 2500     quote units
    //      ask_depth = 0.5 x (1.0750 + 1.0965 + 1.1180
    //                         + 1.1395 + 1.1610)     =    2.795 quote units
    //      micro     = (2.795*1.023622 + 2500*1.075)
    //                  / 2502.795                    = 1.074946
    //      mid       = 0.5173*1.074946 + 0.4827*1.049311
    //                                                = 1.062572
    //
    //  Nothing leaves the book, so the clamp has nothing to do.  The book's
    //  true value was ~$1.01: the old clamped answer was 1.07500, the new
    //  one is 1.06257, which is ~115 bps CLOSER to the truth.
    EXPECT_FALSE(r.clamped) << "the corrected estimator is interior by "
                               "construction; a clamp here means it regressed";
    EXPECT_LT(r.mid, r.best_ask);
    EXPECT_GT(r.mid, r.best_bid);
    EXPECT_NEAR(r.mid, 1.062572, 1e-5);

    // Third inversion in this test: the micro-price no longer escapes above
    // the ask.  Extreme bid dominance still pins it hard against the ask
    // (within ~0.5 bps), which is the correct lean -- just not past it.
    EXPECT_LT(r.microprice, r.best_ask);
    EXPECT_NEAR(r.microprice, 1.074946, 1e-5);

    EXPECT_GT((kBadMid / r.mid - 1.0) * 10000.0, 600.0);  // >600 bps better
}

namespace {

// Build a level whose NORMALIZED depth (quote units, as the estimator sees it
// after unit normalization) is exactly `quote_units`.  Bid sizes are already
// quote-denominated; ask sizes are base-denominated and get valued at px.
CompetingOffer mk_quote(Side side, double price, double quote_units) {
    return mk(side, price,
              side == Side::Bid ? quote_units : quote_units / price);
}

}  // namespace

// =============================================================================
//  THE LIVE MODAL STATE -- XCH/DBX, the only enabled pair.
// =============================================================================
//
//  Reconstructed from the modal live book over the ~39h to 2026-09-02:
//  bid 84.502400, ask 84.570143 (8.01 bps -- the modal spread), normalized
//  quote depths 2048 (bid) and 5844 (ask).  Over that window the Layer-1 clamp
//  fired 1,849 times, 98.38% of them BELOW THE BID, binding on 46.9% of
//  ingests.  This fixture reproduces that exactly, so it is a real regression
//  test and not a restatement of the fix.
//
//  Five levels a side, stepping 4 bps, so bid_vwap really does sit below
//  best_bid (d_b = 0.0676, against a spread of only 0.0677 -- which is the
//  whole reason a NARROW book escaped so easily).  Under the OLD VWAP form
//  this book produced
//
//      micro_old = (5844*84.43480 + 2048*84.63779) / 7892 = 84.48748
//
//  which is 84.48748 < best_bid 84.50240: outside the book, clamped, exactly
//  the production symptom.  Under the corrected touch-price form it produces
//  84.519979, strictly interior.
TEST(OrderbookMidRegression, LiveXchDbxModalBookIsInteriorAndBelowTheMidpoint) {
    constexpr double kBid = 84.502400;
    constexpr double kAsk = 84.570143;

    std::vector<CompetingOffer> book;
    for (int i = 0; i < 5; ++i) {
        book.push_back(mk_quote(Side::Bid, kBid * (1.0 - 0.0004 * i),
                                2048.0 / 5.0));
        book.push_back(mk_quote(Side::Ask, kAsk * (1.0 + 0.0004 * i),
                                5844.0 / 5.0));
    }

    const OrderbookMid r = compute_orderbook_mid(book, defaults(), "XCH/DBX");

    EXPECT_NEAR(r.best_bid, kBid, 1e-6);
    EXPECT_NEAR(r.best_ask, kAsk, 1e-6);
    EXPECT_NEAR(r.spread_bps, 8.01, 0.05);   // the modal 8 bps
    EXPECT_DOUBLE_EQ(r.w_micro, 1.0);        // far inside the narrow band

    // 1. The clamp must not fire.  It fired on 46.9% of live ingests.
    EXPECT_FALSE(r.clamped);

    // 2. Strictly interior -- not merely within, but off both touches.
    EXPECT_GT(r.mid, r.best_bid);
    EXPECT_LT(r.mid, r.best_ask);

    // 3. AND STRICTLY BELOW THE PLAIN MIDPOINT.  This is the assertion that
    //    stops anyone "fixing" the estimator by returning the midpoint and
    //    calling the invariant satisfied.  Ask depth is 2.85x bid depth, so
    //    the thin side is the BID and a correct micro-price must lean down.
    //    A midpoint return would sit at 84.5362715 and fail here.
    EXPECT_LT(r.mid, r.midpoint);
    EXPECT_NEAR(r.midpoint, (kBid + kAsk) / 2.0, 1e-9);

    //  micro = (5844*84.502400 + 2048*84.570143) / 7892 = 84.519979
    EXPECT_NEAR(r.mid, 84.519979, 1e-4);
    EXPECT_NEAR(r.microprice, 84.519979, 1e-4);

    // And the ladder really is a ladder: if these VWAPs ever collapse onto the
    // touches, the fixture has stopped discriminating between the two formulas
    // and the test above is vacuous.  d_b here is ~0.0676, ~1x the spread.
    EXPECT_LT(84.43480, r.best_bid);
    EXPECT_GT(84.63779, r.best_ask);
}

// =============================================================================
//  ACCEPTANCE -- what the corrected estimator is expected to MOVE.
// =============================================================================
//
//  The original write-up of this fix predicted "ladder-centre bias is
//  spread/2: p50 4.0 bps".  THAT NUMBER IS WRONG and this test exists so it
//  cannot be re-derived from memory.  spread/2 assumes the corrected centre
//  lands on the MIDPOINT.  It lands on the MICRO-PRICE, which leans to the
//  thin side.  With R = ask_depth / bid_depth and w_micro = 1,
//
//      micro = (R*B + A) / (1 + R) = B + S/(1 + R)
//
//  so the shift off a centre that the old code clamped to the bid is
//  S/(1+R) -- and it DECAYS TOWARD ZERO exactly in the deep-asymmetry cases
//  where the old clamp bound hardest.  An operator who measures against
//  4.0 bps will see roughly half that and wrongly conclude the fix did not
//  deploy.
//
//  The sound acceptance number is the OTHER one: clamp firings go to zero.
//  That is asserted throughout this file.
TEST(OrderbookMidAcceptance, CentreShiftIsSpreadOverOnePlusRatioNotHalfTheSpread) {
    // The live modal XCH/DBX book.  Bid depth held at the observed 2048 quote
    // units; ask depth swept to move R.
    constexpr double kBid = 84.502400;
    constexpr double kAsk = 84.570143;
    constexpr double kBidDepth = 2048.0;

    const double S = kAsk - kBid;

    // p50 of the live below-bid population, the clamp threshold itself, and
    // three points into the deep tail.  R = 0.46 is the observed minimum on
    // the above-ask population, where the shift REVERSES SIGN.
    const double ratios[] = {0.46, 1.0, 2.00, 2.85, 10.0, 1000.0};

    for (double R : ratios) {
        std::vector<CompetingOffer> book;
        for (int i = 0; i < 5; ++i) {
            book.push_back(mk_quote(Side::Bid, kBid * (1.0 - 0.0004 * i),
                                    kBidDepth / 5.0));
            book.push_back(mk_quote(Side::Ask, kAsk * (1.0 + 0.0004 * i),
                                    kBidDepth * R / 5.0));
        }

        const OrderbookMid r =
            compute_orderbook_mid(book, defaults(), "XCH/DBX");

        ASSERT_DOUBLE_EQ(r.w_micro, 1.0) << "R=" << R;   // 8 bps: micro used whole
        EXPECT_FALSE(r.clamped) << "R=" << R;

        // THE FORMULA.  mid - best_bid == S / (1 + R), to within the rounding
        // the mojo-quantised fixture introduces (~1e-4 relative).
        const double predicted = S / (1.0 + R);
        EXPECT_NEAR(r.mid - r.best_bid, predicted, std::abs(predicted) * 2e-3)
            << "R=" << R << " mid=" << r.mid << " bid=" << r.best_bid;

        // And it is NOT spread/2 anywhere except R == 1 exactly.  At the p50
        // R = 2.85 the two differ by more than a factor of 1.9, which is the
        // whole point: 2.08 bps realized against 4.0 bps predicted.
        if (std::abs(R - 1.0) > 1e-9) {
            EXPECT_GT(std::abs((r.mid - r.best_bid) - S / 2.0),
                      S * 1e-3)
                << "R=" << R << ": the shift coincided with spread/2, which "
                   "means the fixture stopped discriminating";
        }
    }
}

// The two directional claims that follow from the formula, pinned separately
// so a regression names which one broke.
TEST(OrderbookMidAcceptance, ShiftIsCappedAtSpreadOverThreeOnTheBelowBidPopulation) {
    constexpr double kBid = 84.502400;
    constexpr double kAsk = 84.570143;
    constexpr double kBidDepth = 2048.0;
    const double S = kAsk - kBid;

    // A below-bid escape under the OLD estimator required R > 2.00 on this
    // ladder, so S/(1+R) < S/3 bounds the entire below-bid population --
    // 98.38% of the 1,849 live firings.  ~2.7 bps against the modal 8 bps
    // spread, not the 4.0 bps that was predicted.
    for (double R : {2.0001, 2.85, 10.0, 100.0, 1000.0}) {
        std::vector<CompetingOffer> book;
        for (int i = 0; i < 5; ++i) {
            book.push_back(mk_quote(Side::Bid, kBid * (1.0 - 0.0004 * i),
                                    kBidDepth / 5.0));
            book.push_back(mk_quote(Side::Ask, kAsk * (1.0 + 0.0004 * i),
                                    kBidDepth * R / 5.0));
        }
        const OrderbookMid r =
            compute_orderbook_mid(book, defaults(), "XCH/DBX");

        const double shift = r.mid - r.best_bid;
        EXPECT_GT(shift, 0.0) << "R=" << R;              // centre RISES
        EXPECT_LT(shift, S / 3.0) << "R=" << R;          // and is capped
        EXPECT_LT(shift, S / 2.0) << "R=" << R;          // never spread/2
    }
}

TEST(OrderbookMidAcceptance, ShiftReversesOnTheAboveAskPopulation) {
    constexpr double kBid = 84.502400;
    constexpr double kAsk = 84.570143;
    constexpr double kBidDepth = 2048.0;

    // The ~1.62% of firings that clamped ABOVE the ask needed R < ~0.4995.
    // There the old centre sat ON the ask and the corrected centre FALLS.
    // Direction, not just magnitude, differs -- an acceptance check that
    // assumes a one-way move will misread these.
    for (double R : {0.46, 0.30, 0.10}) {
        std::vector<CompetingOffer> book;
        for (int i = 0; i < 5; ++i) {
            book.push_back(mk_quote(Side::Bid, kBid * (1.0 - 0.0004 * i),
                                    kBidDepth / 5.0));
            book.push_back(mk_quote(Side::Ask, kAsk * (1.0 + 0.0004 * i),
                                    kBidDepth * R / 5.0));
        }
        const OrderbookMid r =
            compute_orderbook_mid(book, defaults(), "XCH/DBX");

        EXPECT_FALSE(r.clamped) << "R=" << R;
        EXPECT_LT(r.mid, r.best_ask) << "R=" << R;   // strictly below the ask
        EXPECT_GT(r.mid, r.midpoint) << "R=" << R;   // thin side is the ASK
    }
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

    // INVERTED 2026-09-02 (fourth site, missed in the first pass).  This read
    //     EXPECT_NEAR(r.mid, std::min(r.microprice, r.best_ask), 1e-12);
    // which was written for the clamping regime and is BLIND to the very
    // failure this file exists to catch: if the micro-price escapes above the
    // ask, mid is clamped to best_ask and std::min(micro, best_ask) is ALSO
    // best_ask, so the assertion passes on a broken estimator.  With w = 1
    // the estimate simply IS the micro-price, and the micro-price is interior
    // by construction -- assert exactly that, with no clamp in the way.
    EXPECT_NEAR(r.mid, r.microprice, 1e-12);
    EXPECT_FALSE(r.clamped);
    EXPECT_GT(r.microprice, r.best_bid);
    EXPECT_LT(r.microprice, r.best_ask);
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

    // Hand-computed micro-price in quote units (touch-price form, 2026-09-02):
    //   bid_depth = 500 + 300                       = 800     quote units
    //   ask_depth = 200*2.250 + 100*2.255           = 675.5   quote units
    //   micro     = (675.5*best_bid + 800*best_ask) / 1475.5
    //             = (675.5*2.230 + 800*2.250) / 1475.5
    //             = 2.2408438...
    // The old VWAP form gave 2.2408937 on this book -- the two agree to 2 bps
    // here precisely because the book is tight and near-balanced, which is the
    // regime where the defect was invisible.  This test was never the one that
    // could have caught it; the sweep and the XCH/DBX modal fixture are.
    EXPECT_NEAR(r.mid, 2.2408438, 1e-6);
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
