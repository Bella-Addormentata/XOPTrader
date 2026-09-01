// ---------------------------------------------------------------------------
// [SIDEQUALITY] Per-side agreement with an independent anchor.
//
// Pinned to the live XCH/BYC book of 2026-09-01 (price is BYC per XCH,
// anchor solved WITHOUT book input as 1.41022765):
//
//     bid stack   1.5000  1.4283  1.4066  1.3793  1.3699  1.3514
//     ask stack   4.9995  5.0000  9.7500 10.0000 10.0000 10.0000
//
// Every bid is within 6.4% of the anchor; every ask is 3.5x to 7.1x it.
// The 10,769 bps "spread" is an absent ask side, not a wide market.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "xop/execution/book_side_quality.hpp"

using xop::bookside::classify_sides;

namespace {
constexpr double kAnchor = 1.41022765;  // solved, no book input
constexpr double kBand   = 3.0;         // book_side_anchor_band_ratio
constexpr double kAgree  = 5000.0;      // book_side_agree_max_spread_bps
constexpr double kInf    = std::numeric_limits<double>::infinity();
constexpr double kNaN    = std::numeric_limits<double>::quiet_NaN();
}  // namespace

// -- The live case ----------------------------------------------------------

TEST(BookSideQuality, live_xch_byc_disqualifies_the_ask_side_only)
{
    const auto q = classify_sides(1.5000, 4.9995, kAnchor, kBand, kAgree);
    EXPECT_TRUE(q.bid_ok);    // 1.5000 / 1.41022765 = 1.064x
    EXPECT_FALSE(q.ask_ok);   // 4.9995 / 1.41022765 = 3.545x > 3.0
    EXPECT_FALSE(q.bypassed); // spread is 10,769 bps -- nowhere near coherent
    EXPECT_DOUBLE_EQ(q.ref, kAnchor);
}

TEST(BookSideQuality, the_deeper_junk_asks_are_also_disqualified)
{
    // 9.75 -> 6.914x, 10.0 -> 7.091x. Both far past the band.
    EXPECT_FALSE(classify_sides(1.5000, 9.7500, kAnchor, kBand, kAgree).ask_ok);
    EXPECT_FALSE(classify_sides(1.5000, 10.000, kAnchor, kBand, kAgree).ask_ok);
}

TEST(BookSideQuality, every_honest_bid_level_survives)
{
    for (const double bid : {1.5000, 1.4283, 1.4066, 1.3793, 1.3699, 1.3514}) {
        const auto q = classify_sides(bid, 4.9995, kAnchor, kBand, kAgree);
        EXPECT_TRUE(q.bid_ok) << "bid level " << bid << " must survive";
    }
}

// -- The bypass, which is the part that must not regress --------------------

TEST(BookSideQuality, a_coherent_book_is_trusted_whole_however_far_it_sits)
{
    // The whole market repriced 4x and stayed tight. Both sides are far
    // outside the band individually, and neither may be disqualified: this
    // is exactly the evidence mid_gate::book_confirms() accepts to override
    // an anchor breach, and stripping it here would make that escape
    // unreachable -- the two halves of one feature contradicting each other.
    const auto q = classify_sides(5.60, 5.70, kAnchor, kBand, kAgree);
    EXPECT_TRUE(q.bypassed);
    EXPECT_TRUE(q.bid_ok);
    EXPECT_TRUE(q.ask_ok);
    EXPECT_DOUBLE_EQ(q.ref, kAnchor);  // screened, and recorded as screened
}

TEST(BookSideQuality, the_bypass_needs_a_tight_book_not_merely_a_two_sided_one)
{
    // 1.50 / 4.9995: two-sided, uncrossed, and 10,769 bps wide. A book this
    // incoherent is not confirming anything.
    EXPECT_FALSE(classify_sides(1.5000, 4.9995, kAnchor, kBand, kAgree).bypassed);
}

TEST(BookSideQuality, bypass_boundary_is_inclusive)
{
    // Construct a book whose spread is exactly kAgree bps. With mid m and
    // spread s, bid = m*(1 - s/2e4), ask = m*(1 + s/2e4).
    const double m = 5.0;
    const double bid = m * (1.0 - kAgree / 20000.0);
    const double ask = m * (1.0 + kAgree / 20000.0);
    EXPECT_TRUE(classify_sides(bid, ask, kAnchor, kBand, kAgree).bypassed);
}

TEST(BookSideQuality, a_crossed_book_does_not_earn_the_bypass)
{
    // ask <= bid is not a market agreeing with itself. Falls through to the
    // per-side band, matching compute_spread_bps's convention that a crossed
    // book has no measurable spread.
    const auto q = classify_sides(6.0, 5.0, kAnchor, kBand, kAgree);
    EXPECT_FALSE(q.bypassed);
    EXPECT_FALSE(q.bid_ok);   // 4.25x
    EXPECT_FALSE(q.ask_ok);   // 3.55x
}

// -- Refusals: nothing may be disqualified without an independent anchor ----

TEST(BookSideQuality, no_anchor_disqualifies_nothing)
{
    // The self-reference guard. offer_ref_used falls back to the pair's own
    // last accepted mid; letting that disqualify a side is the lock-in that
    // made the 187.461980 mid unkillable. The caller passes ref_price
    // (independent only), so absent one we must trust everything.
    for (const double anchor : {0.0, -1.0, kNaN, kInf}) {
        const auto q = classify_sides(1.5, 4.9995, anchor, kBand, kAgree);
        EXPECT_TRUE(q.bid_ok);
        EXPECT_TRUE(q.ask_ok);
        EXPECT_DOUBLE_EQ(q.ref, 0.0)
            << "ref must stay 0 so consumers can tell 'trusted because "
               "verified' from 'trusted because unexamined'";
    }
}

TEST(BookSideQuality, band_at_or_below_one_disables_the_test)
{
    for (const double band : {0.0, 1.0, -3.0}) {
        const auto q = classify_sides(1.5, 4.9995, kAnchor, band, kAgree);
        EXPECT_TRUE(q.bid_ok);
        EXPECT_TRUE(q.ask_ok);
        EXPECT_DOUBLE_EQ(q.ref, 0.0);
    }
}

TEST(BookSideQuality, an_absent_side_is_not_a_disqualified_side)
{
    // "No third-party offer here" is already reported as 0 and every
    // consumer handles it. Marking it not-ok would conflate two different
    // failures and re-reference a consumer onto an anchor when the honest
    // answer is "no data".
    const auto no_ask = classify_sides(1.5, 0.0, kAnchor, kBand, kAgree);
    EXPECT_TRUE(no_ask.bid_ok);
    EXPECT_TRUE(no_ask.ask_ok);
    EXPECT_FALSE(no_ask.bypassed);  // one-sided: no spread to measure

    const auto no_bid = classify_sides(0.0, 1.42, kAnchor, kBand, kAgree);
    EXPECT_TRUE(no_bid.bid_ok);
    EXPECT_TRUE(no_bid.ask_ok);
    EXPECT_FALSE(no_bid.bypassed);

    const auto empty = classify_sides(0.0, 0.0, kAnchor, kBand, kAgree);
    EXPECT_TRUE(empty.bid_ok);
    EXPECT_TRUE(empty.ask_ok);
}

TEST(BookSideQuality, non_finite_prices_are_not_disqualified_by_this_gate)
{
    // The ingesters accept infinities today (see mid_gate::select_anchor's
    // note); the absurdity filter and the published-mid gate own those.
    // This gate must not be the thing that decides a NaN is a junk side.
    const auto q = classify_sides(kNaN, kInf, kAnchor, kBand, kAgree);
    EXPECT_TRUE(q.bid_ok);
    EXPECT_TRUE(q.ask_ok);
}

// -- Band edges -------------------------------------------------------------

TEST(BookSideQuality, band_is_symmetric_in_ratio)
{
    // One-sided inputs so the bypass cannot fire and mask the band test.
    //
    // Deliberately NOT asserting behaviour exactly AT the boundary.
    // anchor*3.0 divided back by anchor is 3.0000000000000004 for this
    // anchor, so a strict `ratio <= band` rejects it -- the boundary is
    // inclusive only up to floating-point representation. Pinning the
    // exact-equality case would either encode that rounding artifact as
    // intended behaviour or push a magic epsilon into production code, and
    // 3.0 is an arbitrary threshold where neither buys anything: what
    // matters is that 1.064x passes and 3.545x does not.
    const double hi = kAnchor * kBand;
    const double lo = kAnchor / kBand;
    EXPECT_TRUE(classify_sides(0.0, hi * 0.999, kAnchor, kBand, kAgree).ask_ok);
    EXPECT_TRUE(classify_sides(lo * 1.001, 0.0, kAnchor, kBand, kAgree).bid_ok);
    EXPECT_FALSE(classify_sides(0.0, hi * 1.001, kAnchor, kBand, kAgree).ask_ok);
    EXPECT_FALSE(classify_sides(lo * 0.999, 0.0, kAnchor, kBand, kAgree).bid_ok);
}

TEST(BookSideQuality, a_collapsed_side_is_disqualified_the_same_way_as_a_spiked_one)
{
    // Dislocation has two directions. A dust bid at 1/10th the anchor is as
    // much "not a reference" as a 3.5x ask, and the bid-side consumers
    // (bbo_mid's passive rule, bid_cap) are the ones that would be misled.
    const auto q = classify_sides(0.141, 0.0, kAnchor, kBand, kAgree);
    EXPECT_FALSE(q.bid_ok);
}

// -- The band's relationship to the filters around it -----------------------

TEST(BookSideQuality, disqualification_sits_well_inside_the_offer_absurdity_bound)
{
    // offer_absurdity_ratio(3.0, 0.5) = max(10.0, 6.0, 4.0) = 10.0, so the
    // 4.9995 ask (3.545x) is NOT removed from the book -- it is only
    // demoted as a reference. Both facts must hold at once, and this pins
    // the gap between them: a side can be junk-as-evidence while its offers
    // remain perfectly legal book entries.
    EXPECT_LT(kBand, 10.0);
    const auto q = classify_sides(1.5, 4.9995, kAnchor, kBand, kAgree);
    EXPECT_FALSE(q.ask_ok);
    EXPECT_LT(4.9995 / kAnchor, 10.0);  // survives the absurdity filter
}
