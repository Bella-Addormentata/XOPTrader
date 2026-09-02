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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "xop/config.hpp"
#include "xop/execution/book_side_quality.hpp"
#include "xop/execution/market_data.hpp"
#include "xop/state.hpp"
#include "xop/strategy/bbo_sanity.hpp"
#include "xop/strategy/liquidity.hpp"

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

// -- Step 8 reference selection: the regression that shipped ----------------
//
// [review round 5] Four review rounds and 1000 green tests missed a change
// here that turned a check which never fired into one that cleared every
// tier on every block. Nothing in cpp/tests constructs an Engine, so Step 8
// had no coverage at all. These pin the branch table directly.

namespace {
// The live XCH/BYC book, 2026-08-30/09-01.
constexpr double kS8Bid  = 1.5000;
constexpr double kS8Ask  = 4.9995;
constexpr double kS8Mid  = (kS8Bid + kS8Ask) / 2.0;   // 3.24975
}  // namespace

TEST(Step8References, CheckOneIsSkippedNotRepointedWhenASideIsDisqualified)
{
    // THE REGRESSION, pinned. Re-pointing Check 1 at the surviving bid gave
    // |3.24975 - 1.5| / 1.5 = 116.7% against a 50% cap -- clearing every
    // tier on every block, on the very pair this feature exists for.
    const auto r = xop::bookside::step8_references(
        /*bid_ok=*/true, /*ask_ok=*/false, kAnchor, kS8Mid, kS8Bid, kS8Ask);

    EXPECT_FALSE(r.run_mid_check)
        << "Check 1 must be SKIPPED on a disqualified side, never re-pointed: "
           "both its operands are book-derived and move together";

    // Demonstrate the arithmetic the skip avoids, so the reason survives
    // even if someone later doubts the comment.
    const double repointed_dev = std::abs(kS8Mid - kS8Bid) / kS8Bid;
    EXPECT_GT(repointed_dev, 1.0)
        << "sanity: re-pointing really would produce >100% deviation";
}

TEST(Step8References, CheckOneRunsNormallyOnAHealthyBook)
{
    const auto r = xop::bookside::step8_references(
        true, true, kAnchor, kS8Mid, kS8Bid, kS8Ask);
    EXPECT_TRUE(r.run_mid_check);
    EXPECT_DOUBLE_EQ(r.effective_mid, kS8Mid) << "healthy: untouched";
    EXPECT_DOUBLE_EQ(r.bid_tier_ref, kS8Bid);
    EXPECT_DOUBLE_EQ(r.ask_tier_ref, kS8Ask);
}

TEST(Step8References, CheckTwoReReferencesOnlyTheDisqualifiedSide)
{
    // Check 2 CAN be re-referenced -- it compares our own tier price, which
    // is built around the same centre the anchor represents, so this is
    // like-for-like. The healthy side keeps its own touch.
    const auto ask_bad = xop::bookside::step8_references(
        true, false, kAnchor, kS8Mid, kS8Bid, kS8Ask);
    EXPECT_DOUBLE_EQ(ask_bad.bid_tier_ref, kS8Bid) << "honest side untouched";
    EXPECT_DOUBLE_EQ(ask_bad.ask_tier_ref, kAnchor) << "junk side re-anchored";

    const auto bid_bad = xop::bookside::step8_references(
        false, true, kAnchor, kS8Mid, kS8Bid, kS8Ask);
    EXPECT_DOUBLE_EQ(bid_bad.bid_tier_ref, kAnchor);
    EXPECT_DOUBLE_EQ(bid_bad.ask_tier_ref, kS8Ask);
}

TEST(Step8References, TheEffectiveMidpointMovesOffAPoisonedBook)
{
    // classify_tier's bid passive rule reads the midpoint. Left at 3.24975
    // it would read any bid up to 3.24975 as a safe passive rest -- against
    // a fair value of ~1.41.
    const auto r = xop::bookside::step8_references(
        true, false, kAnchor, kS8Mid, kS8Bid, kS8Ask);
    EXPECT_DOUBLE_EQ(r.effective_mid, kAnchor);
    EXPECT_LT(r.effective_mid, kS8Mid);
}

TEST(Step8References, WithoutAnAnchorNothingIsSubstituted)
{
    // A disqualification cannot occur without an anchor, but the fallback is
    // written out rather than assumed: substituting a zero would be worse
    // than any book.
    for (const double ref : {0.0, -1.0, kNaN}) {
        const auto r = xop::bookside::step8_references(
            true, false, ref, kS8Mid, kS8Bid, kS8Ask);
        EXPECT_DOUBLE_EQ(r.effective_mid, kS8Mid);
        EXPECT_DOUBLE_EQ(r.ask_tier_ref, kS8Ask);
        EXPECT_DOUBLE_EQ(r.bid_tier_ref, kS8Bid);
    }
}

TEST(Step8References, BothSidesDisqualifiedStillSkipsCheckOne)
{
    const auto r = xop::bookside::step8_references(
        false, false, kAnchor, kS8Mid, kS8Bid, kS8Ask);
    EXPECT_FALSE(r.run_mid_check);
    EXPECT_DOUBLE_EQ(r.bid_tier_ref, kAnchor);
    EXPECT_DOUBLE_EQ(r.ask_tier_ref, kAnchor);
}

// -- The derived bypass threshold -------------------------------------------

// [review round 6, PR #134] These three tests pinned max(). max() is the
// FAIL-OPEN direction: both knobs are ceilings on an accept condition over the
// same spread scalar, so the larger is the more permissive and the effective
// value must be the SMALLER. They are rewritten, not re-baselined -- the old
// contract was the defect.

TEST(BookSideQuality, bypass_threshold_can_never_be_wider_than_the_gate)
{
    using xop::bookside::effective_agree_max_spread_bps;

    // The knob raised above the gate must NOT take effect. Above 5000 the
    // gate confirms nothing, so any extra width is trust nothing granted.
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(8000.0, 5000.0), 5000.0);

    // Hardening the gate on its own must tighten the bypass with it. Under
    // max() this returned 5000 -- the operator lowered the gate to 750 and
    // silently left the bypass eight times wider than what they had just
    // asked the gate to confirm.
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(5000.0, 750.0), 750.0);
}

// THE MUTATION CHECK. Arithmetic on the helper alone would still pass with
// max() reinstated "in spirit" (the numbers merely swap), so this drives
// classify_sides on a book with the incident's own geometry and asserts the
// DECISION, not the threshold.
TEST(BookSideQuality, a_wider_configured_bypass_cannot_re_enable_a_junk_ask)
{
    using xop::bookside::effective_agree_max_spread_bps;

    // Constructed so that:
    //   spread            = 6000.0 bps   (between the gate's 5000 and 8000)
    //   bbo_mid / anchor  = 2.80         -> INSIDE mid_anchor_band_ratio 3.0,
    //                                       so gate_mid Accepts at its early
    //                                       exit and never calls
    //                                       book_confirms at all
    //   ask / anchor      = 3.64         -> OUT of band, must be disqualified
    //   bid / anchor      = 1.96         -> in band
    const double bid = 2.764048;
    const double ask = 5.133232;
    const double mid = (bid + ask) / 2.0;
    ASSERT_NEAR((ask - bid) / mid * 10000.0, 6000.0, 1e-6);
    ASSERT_LT(mid / kAnchor, 3.0) << "the mid gate must not be what saves us";
    ASSERT_GT(ask / kAnchor, 3.0);

    const double derived = effective_agree_max_spread_bps(8000.0, kAgree);
    const auto q = classify_sides(bid, ask, kAnchor, kBand, derived);

    EXPECT_FALSE(q.bypassed)
        << "6000 bps is wider than the gate confirms; the bypass must not "
           "fire on an operator's larger number";
    EXPECT_FALSE(q.ask_ok)
        << "THE POISONED BBO. With max() this side came back trusted, Step 8 "
           "measured ask tiers against 5.133232, and the liquidity bid cap "
           "was pinned to the 3.948640 midpoint";
    EXPECT_TRUE(q.bid_ok) << "the honest side is untouched";
}

TEST(BookSideQuality, a_stricter_bypass_takes_effect_and_this_costs_something)
{
    using xop::bookside::effective_agree_max_spread_bps;

    // The operator's stricter value now stands.
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(750.0, 5000.0), 750.0);

    // ...and here is the price, pinned rather than denied. A coherent
    // 4000 bps book far from the anchor IS accepted by book_confirms() as
    // "the whole market repriced", yet both its sides are now disqualified,
    // so Step 8 takes its both-sides-disqualified path and the bot quotes
    // conservatively through a genuine repricing. That is FAIL-CLOSED, which
    // is the trade this repo makes on purpose. Anyone who reverts to max()
    // to recover this case is re-opening the poisoned-BBO path above.
    const double derived = effective_agree_max_spread_bps(750.0, 5000.0);
    const auto q = classify_sides(5.60, 5.60 * 1.4918, kAnchor, kBand,
                                  derived);   // ~4000 bps apart, ~4x anchor
    EXPECT_FALSE(q.bypassed);
    EXPECT_FALSE(q.bid_ok);
    EXPECT_FALSE(q.ask_ok);
}

TEST(BookSideQuality, derived_bypass_ignores_unusable_inputs)
{
    using xop::bookside::effective_agree_max_spread_bps;

    // The asymmetry IS the contract, and it is the whole reason this helper
    // is not a bare std::min. THE TWO OPERANDS ARE NOT INTERCHANGEABLE:
    //
    //   garbage on the OPERATOR knob -- which the config parser already
    //   refuses, so this is defence in depth -- contributes no constraint
    //   and the GATE's own ceiling governs alone;
    //
    //   garbage on the GATE side is NOT neutral. mid_gate::book_confirms
    //   evaluates `spread <= threshold`, and that is false for every book
    //   when the threshold is NaN or negative -- so an unusable gate
    //   confirms NOTHING, and a bypass that still returned `configured`
    //   would be infinitely wider than it. Under min() the neutral element
    //   is +infinity, not the other operand. Fail closed: 0.
    //
    //   an explicit 0 is a SETTING ("bypass off") and BINDS, on either knob.
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(kNaN, 5000.0), 5000.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(kInf, 5000.0), 5000.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(-1.0, 5000.0), 5000.0);

    // [review round 7] These two returned 5000.0 until the direction
    // argument that retired max() was applied to the sanitising branches as
    // well. A knob you cannot trust must not license trust.
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(5000.0, kNaN), 0.0)
        << "an unusable gate confirms nothing, so the bypass grants nothing";
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(5000.0, -1.0), 0.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(kNaN, kNaN), 0.0);

    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(0.0, 5000.0), 0.0)
        << "0 is the documented off switch, not an absent value";
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(5000.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(0.0, 0.0), 0.0);

    // And 0 must actually disable the bypass at the classifier, not merely
    // arrive there as a number. This is the assertion test_config.cpp's
    // SideQualityKnobs_DisabledBandAccepted comment promised and could not
    // make while the helper returned max().
    const auto q = classify_sides(5.60, 5.70, kAnchor, kBand,
                                  effective_agree_max_spread_bps(0.0, kAgree));
    EXPECT_FALSE(q.bypassed)
        << "0 on the agreement cap must mean NO two-sided book is ever "
           "narrow enough to be trusted whole";
    EXPECT_FALSE(q.ask_ok) << "and the per-side band then decides: 4.04x";
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

// ===========================================================================
// FEED-LEVEL: the verdict must never outlive the book it was measured on.
//
// [review, PR #134] ingest_dexie() replaces dex_best_bid/ask with the RAW
// ticker and clears the three S20 provenance flags, because raw values are
// self-inclusive and unfiltered. The per-side verdicts were originally NOT
// cleared alongside them, so when the full-offer fetch throws -- the exact
// scenario those flags exist for -- Step 8 would pair a CURRENT raw BBO with
// the PREVIOUS cycle's disqualification and anchor, and re-reference a tier
// against a book_side_ref measured on prices that no longer exist. Fresh
// numbers, stale evidence: the S20 shape, reintroduced by the feature meant
// to close it.
// ===========================================================================

namespace {

using namespace xop;

MarketDataConfig sq_cfg() {
    MarketDataConfig cfg;
    cfg.cex_freshness_threshold_sec = 0.0;
    cfg.amm_blend_weight            = 0.0;
    cfg.mid_gate_enabled            = true;
    // Dust filter off: these fixtures are about the side verdict, not sizing.
    cfg.min_competitor_offer_size   = 0;
    return cfg;
}

CompetingOffer sq_offer(const std::string& id, Side side, double price,
                        Mojo size) {
    CompetingOffer o;
    o.offer_id = id;
    o.side     = side;
    o.price    = static_cast<Mojo>(
        std::llround(price * static_cast<double>(kMojosPerXch)));
    o.size     = size;
    return o;
}

// The live XCH/BYC shape: honest bids at the anchor, a junk ask stack 3.5x
// above it.
std::vector<CompetingOffer> dislocated_book() {
    return {
        sq_offer("b1", Side::Bid, 1.5000, 5'000'000'000'000LL),
        sq_offer("a1", Side::Ask, 4.9995, 5'000'000'000'000LL),
    };
}

}  // namespace

TEST(BookSideQualityFeed, DislocatedAskSideIsPublishedAsDisqualified)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    // Independent anchor. A CEX reference is the simplest injection
    // point: select_anchor ranks it first, and it is what the sibling
    // gate tests use for the same purpose.
    feed.ingest_cex_reference(pair, kAnchor);
    feed.ingest_competing_offers(pair, dislocated_book(), {},
                                 kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    EXPECT_TRUE(snap.bid_side_anchor_ok)  << "1.5000 is 1.06x the anchor";
    EXPECT_FALSE(snap.ask_side_anchor_ok) << "4.9995 is 3.55x the anchor";
    EXPECT_GT(snap.book_side_ref, 0)
        << "a disqualification requires an anchor, so ref must be published";
}

// ---------------------------------------------------------------------------
// [review round 6, PR #134] VALUATION GRADE MUST RESPECT THE SIDE VERDICTS.
//
// apply_mid_gate granted mid_valuation_grade on `book_two_sided &&
// dex_fresh_grade` alone.  "Two-sided" is a PRESENCE test with no price sanity
// in it, and the per-side verdicts were read by Step 8 and the ladder but by
// nothing on the valuation path.  So the dislocated book below graded, its
// 3.24975 mid reached compute_portfolio_equity_usd and the mark-to-market
// callback, and both breakers could be fed a 2.3x valuation error.
//
// The mid gate does not save this: 3.24975 / 1.41022765 = 2.3044 is INSIDE
// mid_anchor_band_ratio (3.0), so gate_mid returns Accept at its early exit
// and never calls book_confirms -- the 10,769 bps spread is never examined.
//
// The anchor here is injected with ingest_reference_anchor, NOT
// ingest_cex_reference.  sq_cfg() sets cex_freshness_threshold_sec = 0.0
// ("expiry disabled"), so a CEX leg would make cex_fresh true and an
// EXPECT_FALSE below would pass through the `|| cex_fresh` branch regardless
// of whether the book branch was fixed -- green by accident, in the wrong
// direction.  An implied-cross anchor models the shipped XCH/DBX pair, which
// has no CoinGecko mapping and anchors off the AMM/fair-value chain.
// ---------------------------------------------------------------------------

TEST(BookSideQualityFeed, ADislocatedBookIsRefusedValuationGrade)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, /*implied_cross=*/kAnchor,
                                 /*peg_target=*/0.0);
    feed.ingest_competing_offers(pair, dislocated_book(), {},
                                 kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    ASSERT_FALSE(snap.ask_side_anchor_ok)
        << "precondition: the junk ask side must be disqualified";

    // The mid STILL PUBLISHES. Withholding valuation is not the same as
    // going no-mid: quoting logic needs a price, and Step 8 re-references
    // off the junk side rather than losing the pair. If this ever flips to
    // 0 the test below stops meaning anything.
    ASSERT_GT(snap.mid_price, 0)
        << "the fix must withhold VALUATION, not suppress the mid";

    const double mid =
        static_cast<double>(snap.mid_price) / static_cast<double>(kMojosPerXch);
    EXPECT_LT(mid / kAnchor, 3.0)
        << "the poisoned mid is INSIDE the gate's band -- the mid gate is "
           "not what stops this, which is the whole point";

    EXPECT_FALSE(snap.mid_valuation_grade)
        << "a book with one disqualified side must not mark equity or P&L: "
           "this mid reaches the drawdown breaker and the rolling-window "
           "loss breaker";
}

// Non-vacuity control. If dex_fresh_grade were false in this fixture for some
// unrelated reason, the assertion above would pass no matter what the grade
// predicate did. Same feed, same anchor, same ingest path -- only the book is
// coherent -- and the grade must be GRANTED.
TEST(BookSideQualityFeed, ACoherentBookStillEarnsValuationGrade)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, kAnchor, 0.0);
    feed.ingest_competing_offers(
        pair,
        {sq_offer("b1", Side::Bid, 1.40, 5'000'000'000'000LL),
         sq_offer("a1", Side::Ask, 1.42, 5'000'000'000'000LL)},
        {}, kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    ASSERT_TRUE(snap.bid_side_anchor_ok);
    ASSERT_TRUE(snap.ask_side_anchor_ok);
    EXPECT_TRUE(snap.mid_valuation_grade)
        << "the side gate must not deny grade to a healthy book -- if this "
           "fails, the assertion in the sibling test proves nothing";
}

// The `|| cex_fresh` branch must be gated too, not just the book branch.
// mid_price is the 70/30 blend, so a fresh CEX leg does not make a
// 70%-junk-book mid side-safe. A fix applied only to the book branch leaves
// this green in the wrong direction.
TEST(BookSideQualityFeed, AFreshCexLegDoesNotRescueADislocatedBook)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_cex_reference(pair, kAnchor);   // expiry disabled -> always fresh
    feed.ingest_competing_offers(pair, dislocated_book(), {},
                                 kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    ASSERT_FALSE(snap.ask_side_anchor_ok);
    ASSERT_GT(snap.mid_price, 0);
    EXPECT_FALSE(snap.mid_valuation_grade)
        << "the CEX leg is only 30% of this mid; it cannot bless the 70% of "
           "it that came from a disqualified book side";
}

TEST(BookSideQualityFeed, RawTickerIngestClearsAStaleDisqualification)
{
    // THE REGRESSION. Disqualify a side, then let a raw ticker poll land
    // without a successful offers fetch behind it -- which is what happens
    // when the offers request throws.
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_cex_reference(pair, kAnchor);
    feed.ingest_competing_offers(pair, dislocated_book(), {},
                                 kMojosPerXch, 1'000);
    feed.refresh({pair});
    ASSERT_FALSE(state.get_market(pair).ask_side_anchor_ok)
        << "precondition: the ask side must start out disqualified";

    // A raw ticker poll lands with COMPLETELY DIFFERENT prices and no
    // filtered book behind it.
    feed.ingest_dexie(pair, 1.38, 1.44, 0.0, 0.0);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    EXPECT_TRUE(snap.bid_side_anchor_ok);
    EXPECT_TRUE(snap.ask_side_anchor_ok)
        << "the previous cycle's verdict describes a book that no longer "
           "exists; carrying it forward is the S20 defect shape";
    EXPECT_EQ(snap.book_side_ref, 0)
        << "ref must return to 0 -- nothing screened this raw book, and a "
           "stale ref would re-reference a tier against vanished prices";
}

TEST(BookSideQualityFeed, AFreshFilteredIngestReinstatesTheVerdict)
{
    // The reset must not be sticky: once the offers fetch succeeds again the
    // verdict has to come back, or one failed poll would disarm the feature
    // until process restart.
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_cex_reference(pair, kAnchor);
    feed.ingest_competing_offers(pair, dislocated_book(), {},
                                 kMojosPerXch, 1'000);
    feed.ingest_dexie(pair, 1.38, 1.44, 0.0, 0.0);
    feed.refresh({pair});
    ASSERT_TRUE(state.get_market(pair).ask_side_anchor_ok);

    feed.ingest_competing_offers(pair, dislocated_book(), {},
                                 kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    EXPECT_FALSE(snap.ask_side_anchor_ok)
        << "a successful filtered ingest must re-disqualify the junk side";
    EXPECT_GT(snap.book_side_ref, 0);
}

TEST(BookSideQualityFeed, AHealthyBookPublishesBothSidesTrusted)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/DBX";

    feed.ingest_block_height(100);
    feed.ingest_cex_reference(pair, kAnchor);
    feed.ingest_competing_offers(
        pair,
        {sq_offer("b1", Side::Bid, 1.40, 5'000'000'000'000LL),
         sq_offer("a1", Side::Ask, 1.42, 5'000'000'000'000LL)},
        {}, kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    EXPECT_TRUE(snap.bid_side_anchor_ok);
    EXPECT_TRUE(snap.ask_side_anchor_ok);
}

// ===========================================================================
// [review round 7, PR #134] THE THREE DEFECTS IN THE ROUND-6 GRADE PREDICATE.
//
// Round 6 shipped `mid > 0 && book_sides_ok && ((two_sided && grade) || cex)`.
// The five tests below pin the corrections. Each was RED before the fix; the
// last two pin the two deliberate escapes, so neither can be deleted as dead
// weight.
// ===========================================================================

// (1) OVER-STRICT: the side verdicts gated mids the book never entered.
//
// A one-sided book publishes NO dex mid at all (compute_mid Case 2 refuses
// to derive one), so the published mid here is EXACTLY the CEX anchor -- and
// round 6 withheld its grade because of a book side that contributed zero to
// it. On the thin pairs being the only offer on a side is the normal state,
// and engine.cpp records a prior incident where requiring grade on such a
// pair zeroed every USD figure in the bot.
TEST(BookSideQualityFeed, AOneSidedBookDoesNotForfeitAPureCexValuation)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_cex_reference(pair, kAnchor);   // expiry disabled -> fresh
    // Third-party ASK only, far outside the 3.0x band. No third-party bid,
    // so dex_best_bid is the authoritative 0 and the book is NOT two-sided.
    feed.ingest_competing_offers(
        pair,
        {sq_offer("a1", Side::Ask, kAnchor * 4.0, 5'000'000'000'000LL)},
        {}, kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    ASSERT_FALSE(snap.ask_side_anchor_ok)
        << "precondition: the lone ask is 4x the anchor and must be junk";
    ASSERT_EQ(snap.best_bid, 0)
        << "precondition: no third-party bid, so the book is one-sided";

    // The published mid must be the anchor itself -- proof that the
    // disqualified ask contributed nothing to the number being graded.
    const double mid =
        static_cast<double>(snap.mid_price) / static_cast<double>(kMojosPerXch);
    ASSERT_NEAR(mid, kAnchor, kAnchor * 1e-9)
        << "w_dex must be 0 here: a one-sided book yields no dex mid";

    EXPECT_TRUE(snap.mid_valuation_grade)
        << "this mid is bit-for-bit the CEX anchor; withholding its grade "
           "over a book side that never entered it starves the drawdown "
           "breaker on the pair's NORMAL state";
}

// (2) VACUOUS: on the `|| cex_fresh` branch the verdicts were never computed.
//
// ingest_dexie writes the RAW self-inclusive ticker BBO and resets both side
// verdicts to TRUE with book_side_ref to 0. If the competing-offer fetch then
// throws -- the exact failure the S20 provenance flags exist for --
// classify_sides never runs, so `book_sides_ok` is trivially true and round 6
// graded a 70%-raw, unscreened, half-dislocated BBO off the CEX leg alone.
//
// No ingest_competing_offers call in this fixture. That absence IS the test.
//
// The raw book here is deliberately TIGHT (99 bps) and therefore internally
// coherent, so the round-7 coherence conjunct does NOT fire and this fixture
// isolates the witness. A dislocated raw book would be refused by coherence
// alone and would prove nothing about `sides_examined` -- the first version
// of this test made exactly that mistake and survived the mutation.
//
// A tight raw book is not a safe book: ingest_dexie's BBO includes OUR OWN
// resting offers, so on a thin pair "tight" can mean nothing more than that
// we are the only two offers and are quoting 2.1x the anchor to ourselves.
TEST(BookSideQualityFeed, AnUnexaminedRawBookIsNotRescuedByAFreshCexLeg)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_cex_reference(pair, kAnchor);   // expiry disabled -> fresh
    // Coherent, self-inclusive, and 2.1x the anchor. Never screened.
    feed.ingest_dexie(pair, 3.0000, 3.0300, 0.0, 0.0);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    ASSERT_TRUE(snap.bid_side_anchor_ok);
    ASSERT_TRUE(snap.ask_side_anchor_ok)
        << "precondition: the verdicts are TRIVIALLY true -- nothing ran";
    ASSERT_EQ(snap.book_side_ref, 0)
        << "precondition: book_side_ref is the witness, and it is 0 because "
           "classify_sides never examined this book";
    ASSERT_GT(snap.spread_bps, 0);
    ASSERT_LT(snap.spread_bps, 5000.0)
        << "precondition: this book IS coherent, so the spread conjunct "
           "cannot be what refuses it -- otherwise this test is vacuous";
    ASSERT_GT(snap.mid_price, 0);

    EXPECT_FALSE(snap.mid_valuation_grade)
        << "70% of this mid is a raw, self-inclusive, never-screened BBO "
           "sitting at 2.1x the anchor; two default-true flags are not "
           "evidence that anything checked it";
}

// (3) TOO LOOSE: the 3.0x per-side band alone does not mean "not dislocated".
//
// Round 6's comment promised "A HALF-DISLOCATED BOOK MUST NOT MARK EQUITY".
// With an ask at 2.978x the anchor BOTH sides sit inside the band, so both
// stand -- while the book's own spread is 9,474 bps and its mid is 2.02x the
// anchor. The incident ask was 3.545x; an ask only 16% lower defeated the
// entire fix. The header's own diagnostic (one side moving alone leaves a
// wide spread behind) was computed and never consulted on this path.
TEST(BookSideQualityFeed, AWideBookWithBothSidesInsideTheBandIsStillRefused)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, /*implied_cross=*/kAnchor,
                                 /*peg_target=*/0.0);
    feed.ingest_competing_offers(
        pair,
        {sq_offer("b1", Side::Bid, 1.5000, 5'000'000'000'000LL),
         sq_offer("a1", Side::Ask, 4.2000, 5'000'000'000'000LL)},
        {}, kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    // The per-side band is NOT what catches this -- both sides survive it.
    ASSERT_TRUE(snap.bid_side_anchor_ok) << "1.5000 / anchor = 1.064";
    ASSERT_TRUE(snap.ask_side_anchor_ok) << "4.2000 / anchor = 2.978 < 3.0";
    ASSERT_GT(snap.book_side_ref, 0) << "precondition: the book WAS examined";

    // Nor is the mid gate: 2.02x is inside mid_anchor_band_ratio, so gate_mid
    // returns Accept at its early exit and never calls book_confirms.
    const double mid =
        static_cast<double>(snap.mid_price) / static_cast<double>(kMojosPerXch);
    ASSERT_GT(mid, 0);
    ASSERT_LT(mid / kAnchor, 3.0);
    ASSERT_GT(mid / kAnchor, 2.0) << "a 102% valuation error, and it grades";

    EXPECT_FALSE(snap.mid_valuation_grade)
        << "a 9,474-bps book is one side standing alone; the book must agree "
           "with ITSELF before its mid may mark equity";
}

// ESCAPE 1, PINNED: a crossed book must not become a valuation blackout.
//
// compute_spread_bps returns 0 for a crossed book, and classify_sides
// deliberately falls such a book through to the per-side band rather than
// judging its coherence. Crossed books are normal on Dexie (no matching
// engine), so the round-7 coherence conjunct must stand down here and let the
// per-side band govern alone -- exactly as classify_sides does. Without this
// escape every crossed cycle would lose its grade.
TEST(BookSideQualityFeed, ACrossedBookIsStillJudgedByTheBandAlone)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/DBX";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, kAnchor, 0.0);
    // Crossed: bid ABOVE ask. Both touches sit on the anchor.
    feed.ingest_competing_offers(
        pair,
        {sq_offer("b1", Side::Bid, 1.42, 5'000'000'000'000LL),
         sq_offer("a1", Side::Ask, 1.40, 5'000'000'000'000LL)},
        {}, kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    ASSERT_GT(snap.best_bid, snap.best_ask)
        << "precondition: the book must actually be crossed";
    ASSERT_DOUBLE_EQ(snap.spread_bps, 0.0)
        << "precondition: a crossed book has no measurable spread";
    ASSERT_TRUE(snap.bid_side_anchor_ok);
    ASSERT_TRUE(snap.ask_side_anchor_ok);

    EXPECT_TRUE(snap.mid_valuation_grade)
        << "an unmeasurable spread is not evidence of dislocation; both "
           "touches are on the anchor and the band is what governs";
}

// ESCAPE 2, PINNED: agree_max 0 is "bypass off", not "distrust every book".
//
// 0 is the documented setting for withdrawing coherence as a reason to TRUST
// a book. It must not silently become a reason to DISTRUST one: at 0 the
// coherence conjunct stands down and the per-side band governs alone, which
// is the round-6 behaviour. Without this escape the knob would black out
// valuation bot-wide -- every two-sided book on every pair.
TEST(BookSideQualityFeed, DisablingTheBypassDoesNotBlackOutValuation)
{
    MarketDataConfig cfg = sq_cfg();
    cfg.book_side_agree_max_spread_bps = 0.0;   // documented "off"
    ASSERT_DOUBLE_EQ(
        xop::bookside::effective_agree_max_spread_bps(
            cfg.book_side_agree_max_spread_bps,
            cfg.mid_gate_book_confirm_max_spread_bps),
        0.0) << "precondition: min() carries the 0 through";

    State state;
    MarketDataFeed feed(cfg, state);
    const std::string pair = "XCH/DBX";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, kAnchor, 0.0);
    // A perfectly healthy 142-bps book sitting on the anchor.
    feed.ingest_competing_offers(
        pair,
        {sq_offer("b1", Side::Bid, 1.40, 5'000'000'000'000LL),
         sq_offer("a1", Side::Ask, 1.42, 5'000'000'000'000LL)},
        {}, kMojosPerXch, 1'000);
    feed.refresh({pair});

    const auto snap = state.get_market(pair);
    ASSERT_GT(snap.spread_bps, 0) << "precondition: spread IS measurable here";
    ASSERT_TRUE(snap.bid_side_anchor_ok);
    ASSERT_TRUE(snap.ask_side_anchor_ok);

    EXPECT_TRUE(snap.mid_valuation_grade)
        << "turning the bypass off must not zero every USD figure in the bot";
}

// ===========================================================================
// [review round 8, PR #134] THE MIXED-GENERATION READ, PINNED END TO END.
//
// Step 7 and Step 8 read the per-side verdict from TWO DIFFERENT STORES:
//
//   Step 7  engine.cpp -> market_data_->get_competing_book(pair)
//             cached filtered offers + the verdict measured on THOSE offers,
//             under mtx_competitors_, written only by ingest_competing_offers.
//   Step 8  engine.cpp -> state_->get_market(pair)
//             a MarketSnapshot published from PairState under mtx_pairs_,
//             whose verdict ingest_dexie() resets to (true, true, ref=0) on
//             EVERY raw ticker poll.
//
// Step 1 calls both ingests inside one try block and its catch only WARNS, so
// after [successful filtered ingest -> raw ticker -> offers fetch failure] the
// two stores hold different generations and the cycle proceeds anyway. That is
// deterministic, not a race.
//
// RawTickerIngestClearsAStaleDisqualification above pins only the snapshot
// half. It says nothing about what get_competing_book() returns in that state,
// what ladder Step 7 then builds, or what Step 8 does to it. This fixture pins
// all of that TOGETHER, because the safety argument is a property of the PAIR
// and cannot be stated about either half alone:
//
//   Step 7's verdict re-references the ladder: a disqualified side replaces
//   the BBO midpoint with the model mid for BOTH the bid cap and the ask
//   floor (liquidity.cpp, the competitive anchor block). That is USUALLY a
//   tightening, but it is a re-reference, not a direction -- on a crossed
//   competing book the BBO midpoint falls BELOW the model mid and the
//   disqualified branch raises the bid cap instead. Nothing in compute_ladder
//   enforces the uncrossed precondition; the `mid` parameter is whatever the
//   caller passes.
//   Step 8's verdict likewise re-references: it decides which price each tier
//   is measured against, and only then vetoes (classify_tier returns Pass or
//   Suppress; Check 1 only ever clears). It never prices a tier.
//
// [review round 9] BOTH halves RE-REFERENCE BEFORE THEY JUDGE, so the two
// policies DO NOT NEST. An earlier revision of this comment, and of the one
// at the Step 7 site in engine.cpp, claimed the mix "yields a SUBSET of what
// adopting the stale verdict in Step 8 would admit. Fail-closed." That is
// false on this fixture's OWN book -- see TheTwoPoliciesAdmitNonNestedSets
// below, which measures the admitted intervals directly:
//
//     BID  fresh [1.0500, 3.2498]  vs  stale [0.9872, 1.6500]  -- neither
//                                                                 contains
//                                                                 the other
//     ASK  fresh [4.4996, 6.4994]  vs  stale [1.2692, 1.8333]  -- DISJOINT
//
// The subset relation observed in Step8VetoesAsksTheStaleVerdictWouldAdmit is
// a property of WHERE THIS FIXTURE'S SIX TIERS LAND, not of the policies. The
// mix is still kept deliberately, on a bound rather than a nesting: the reset
// triple is the legacy pre-SIDEQUALITY verdict, so Step 8 is never worse than
// what shipped before this work. See the round-9 comment in engine.cpp.
//
// The book does NOT move in this fixture, and that is the point: the offers
// ENDPOINT threw, the market did not reprice. So the raw ticker reports the
// same 1.50 / 4.9995 the filtered fetch saw last cycle -- self-inclusive and
// unscreened, hence the reset.
// ===========================================================================

namespace {

// Mirrors make_anchor_config(3) + make_sidequality_config() in
// test_liquidity.cpp, where the ladder-side behaviour asserted below is
// independently pinned (BookSideQualityLadder.*). Production runs 8000 bps,
// not the 500 default, and the gate matters: |1.50 - 1.41141912| / 1.41141912
// = 628 bps, which 500 would reject outright and mask the behaviour.
LiquidityConfig mg_ladder_config() {
    LiquidityConfig cfg;
    cfg.num_tiers        = 3;
    cfg.tier_spacing_bps = {50.0, 100.0, 150.0};
    cfg.tier_size_pct    = {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    cfg.competitive_anchor_enabled          = true;
    cfg.competitive_anchor_stride_bps       = 65.0;
    cfg.competitive_anchor_max_distance_bps = 8000.0;
    cfg.gap_aware_spacing        = false;
    cfg.adverse_selection_sizing = false;
    cfg.fill_rate_sizing         = false;
    return cfg;
}

// The Step-7 fair-value centre: 1.41141912, solved WITHOUT book input. Not
// the published mid -- on this book those are wildly different numbers, which
// is the round-5 correction recorded in book_side_quality.hpp.
//
// [review round 9] WHY THIS IS THE REACHABLE VALUE AND get_mid_price() IS NOT.
// Two reviews have now read engine.cpp:6295 --
//
//     double market_mid = market_data_->get_mid_price(pair_name);
//
// -- concluded that Step 7 centres on the published mid (3.24975 here), and
// filed this fixture as pinning a state the code cannot reach. It does not
// stop there. engine.cpp REASSIGNS market_mid before the ladder ever sees it:
//
//     market_mid = blend.center;      // fv::blend_quote_center
//
// and only then converts it to mid_mojos for compute_ladder. The blend is an
// inverse-variance weighted mean in log space of (book mid, book sigma) and
// (external solve, solve sigma), and quote_center_blend_enabled defaults true.
// On THIS book the 1.5000/4.9995 spread is 10,768 bps, so book_sigma is
// ~5,384 bps and the book's own opinion is worth almost nothing. Running the
// real blend_quote_center against it:
//
//     ext_sigma  100bps -> w_external 0.9997 -> centre 1.41063
//     ext_sigma  200bps -> w_external 0.9986 -> centre 1.41185
//     ext_sigma  467bps -> w_external 0.9925 -> centre 1.41905
//
// 1.41141912 sits inside that range (ext_sigma ~180 bps). A centre near the
// anchor is not merely reachable on a book this dislocated, it is the ONLY
// thing the blend can produce. The published mid is what the blend takes as
// its low-confidence input, not what Step 7 quotes around.
constexpr Mojo kMgCentre = 1'411'419'120'000LL;

Mojo mg_to_mojos(double price) {
    return static_cast<Mojo>(
        std::llround(price * static_cast<double>(kMojosPerXch)));
}

std::vector<TierQuote> mg_ladder(bool bid_side_ok, bool ask_side_ok,
                                 const std::vector<CompetingOffer>& offers) {
    auto cfg = mg_ladder_config();
    cfg.book_bid_side_anchor_ok = bid_side_ok;
    cfg.book_ask_side_anchor_ok = ask_side_ok;
    LiquidityEngine engine("XCH/BYC", cfg);
    return engine.compute_ladder(
        kMgCentre, 0.03, 0.5,
        100'000'000'000'000LL, 100'000'000'000'000LL,
        offers, cfg);
}

// A faithful replica of engine.cpp's Step 8 BBO-proximity block: pick the
// references ONCE via bookside::step8_references, run Check 1 (all-or-nothing
// on the PUBLISHED mid), then Check 2 (per tier, per side). The Engine cannot
// be constructed from a test (TODO S36), so the policy is reproduced from the
// same two pure functions the Engine calls, in the same order.
struct Step8Outcome {
    std::vector<TierQuote> survivors{};
    bool   mid_check_ran{false};
    bool   mid_check_fired{false};
    double mid_dev{0.0};
};

Step8Outcome mg_step8(const std::vector<TierQuote>& tiers,
                      bool bid_ok, bool ask_ok, Mojo side_ref,
                      Mojo published_mid, Mojo best_bid, Mojo best_ask) {
    // The shipped thresholds, not invented ones.
    //
    // [review round 9] MODELLING LIMIT, stated so it is not mistaken for
    // coverage: engine.cpp prefers the PER-PAIR overrides
    // (bbo_sanity_max_aggressive_dev_override / _max_passive_dev_override on
    // PairConfig) and falls back to these strategy-level values only when the
    // pair sets none. This replica models the fallback path only. Since every
    // relation pinned in this section turns on whether a tier routes to the
    // aggressive cap or the passive one, a pair carrying overrides can sit on
    // the other side of that boundary with these tests still green. The
    // mid-dev threshold is correctly global-only in both.
    const StrategyConfig sc{};
    const double kMaxAggressiveDev = sc.bbo_sanity_max_aggressive_dev;
    const double kMaxPassiveDev    = sc.bbo_sanity_max_passive_dev;
    const double kMaxMidDev        = sc.bbo_sanity_max_mid_dev;

    const Mojo bbo_mid_m = (best_bid + best_ask) / 2;
    const auto s8 = xop::bookside::step8_references(
        bid_ok, ask_ok,
        static_cast<double>(side_ref),
        static_cast<double>(bbo_mid_m),
        static_cast<double>(best_bid),
        static_cast<double>(best_ask));
    const Mojo eff_bbo_mid =
        static_cast<Mojo>(std::llround(s8.effective_mid));

    Step8Outcome out;
    out.mid_check_ran = s8.run_mid_check;
    std::vector<TierQuote> working = tiers;

    // Check 1: model mid vs BBO mid. Clears EVERYTHING when it fires.
    if (published_mid > 0) {
        out.mid_dev = std::abs(static_cast<double>(published_mid)
                               - static_cast<double>(bbo_mid_m))
                    / static_cast<double>(bbo_mid_m);
        if (s8.run_mid_check && out.mid_dev > kMaxMidDev) {
            out.mid_check_fired = true;
            working.clear();
        }
    }

    // Check 2: per-tier BBO proximity, referenced per side.
    for (const auto& tier : working) {
        const Mojo ref = static_cast<Mojo>(std::llround(
            tier.side == Side::Bid ? s8.bid_tier_ref : s8.ask_tier_ref));
        const auto verdict = xop::strategy::classify_tier(
            tier.side != Side::Bid,
            static_cast<double>(tier.price),
            static_cast<double>(ref),
            static_cast<double>(eff_bbo_mid),
            kMaxAggressiveDev, kMaxPassiveDev);
        if (verdict == xop::strategy::BboVerdict::Pass) {
            out.survivors.push_back(tier);
        }
    }
    return out;
}

std::string mg_key(const TierQuote& tq) {
    return std::string(tq.side == Side::Bid ? "B" : "A")
         + std::to_string(static_cast<int>(tq.tier_index))
         + "@" + std::to_string(tq.price);
}

std::vector<std::string> mg_keys(const std::vector<TierQuote>& tiers) {
    std::vector<std::string> out;
    out.reserve(tiers.size());
    for (const auto& tq : tiers) out.push_back(mg_key(tq));
    std::sort(out.begin(), out.end());
    return out;
}

bool mg_is_subset(const std::vector<TierQuote>& sub,
                  const std::vector<TierQuote>& super) {
    const auto a = mg_keys(sub);
    const auto b = mg_keys(super);
    return std::includes(b.begin(), b.end(), a.begin(), a.end());
}

// The exact sequence: filtered ingest SUCCEEDS, then a raw ticker lands, then
// the offers fetch THROWS (modelled as the absence of a second
// ingest_competing_offers -- a throw is the only way to skip that write; an
// EMPTY result still writes, and would clear the verdict honestly).
//
// ingest_reference_anchor, not ingest_cex_reference: with no CEX leg the
// published mid IS the BBO midpoint, so Check 1's deviation is identically
// zero and it cannot fire. That isolates the Check 2 asymmetry below instead
// of letting an all-or-nothing Check 1 mask it -- and it models XCH/DBX, the
// shipped pair, which has no CoinGecko mapping.

}  // namespace

TEST(BookSideQualityMixedGeneration, Step7AndStep8SeeDifferentVerdicts)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, /*implied_cross=*/kAnchor,
                                 /*peg_target=*/0.0);
    feed.ingest_competing_offers(pair, dislocated_book(), {},
                                 kMojosPerXch, 1'000);
    feed.refresh({pair});
    ASSERT_FALSE(state.get_market(pair).ask_side_anchor_ok)
        << "precondition: the filtered ingest must disqualify the junk ask";

    // The offers fetch throws; only the raw ticker lands, reporting the same
    // book (self-inclusive, unscreened).
    feed.ingest_dexie(pair, 1.5000, 4.9995, 0.0, 0.0);
    feed.refresh({pair});

    const auto comp_book = feed.get_competing_book(pair);  // Step 7's read
    const auto snap      = state.get_market(pair);         // Step 8's read

    // -- Step 7's half: the cached book AND the verdict that measured it.
    ASSERT_FALSE(comp_book.offers.empty())
        << "nothing clears competing_offers_, so the stale book must survive";
    ASSERT_TRUE(comp_book.bid_side_anchor_ok);
    ASSERT_FALSE(comp_book.ask_side_anchor_ok)
        << "PRECONDITION, not an expectation: if the two-sides-agree bypass "
           "ever fires on this book both sides come back trusted, the two "
           "arms below collapse onto each other, and this whole fixture "
           "becomes vacuous";
    ASSERT_GT(comp_book.book_side_ref, 0.0)
        << "the verdict must carry the anchor that produced it";

    // -- Step 8's half: reset to the legacy triple by the raw ticker.
    EXPECT_TRUE(snap.bid_side_anchor_ok);
    EXPECT_TRUE(snap.ask_side_anchor_ok);
    EXPECT_EQ(snap.book_side_ref, 0);

    // -- The divergence itself, stated as the fact the engine consumes.
    EXPECT_NE(comp_book.ask_side_anchor_ok, snap.ask_side_anchor_ok)
        << "the two stores must be observably out of step here -- if they "
           "agree, either ingest_dexie now clears the offer store (option b, "
           "which re-opens the round-5 desync) or Step 7 has been re-pointed "
           "at the snapshot (the tertiary mutation)";
}

TEST(BookSideQualityMixedGeneration, Step7BuildsTheConservativeLadderFromTheCachedPairing)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, kAnchor, 0.0);
    feed.ingest_competing_offers(pair, dislocated_book(), {},
                                 kMojosPerXch, 1'000);
    feed.refresh({pair});
    feed.ingest_dexie(pair, 1.5000, 4.9995, 0.0, 0.0);
    feed.refresh({pair});

    const auto comp_book = feed.get_competing_book(pair);
    const auto snap      = state.get_market(pair);
    ASSERT_FALSE(comp_book.ask_side_anchor_ok);

    // ARM 2 -- what ships. Step 7 pairs the cached offers with the verdict
    // that measured them, so the disqualified ask side may not build bbo_ref;
    // it falls back to the model mid and bid_cap tightens to it.
    const auto shipped = mg_ladder(comp_book.bid_side_anchor_ok,
                                   comp_book.ask_side_anchor_ok,
                                   comp_book.offers);
    ASSERT_FALSE(shipped.empty())
        << "a disqualified side must not empty the ladder -- quoting "
           "correctly beats not quoting";
    for (const auto& tq : shipped) {
        if (tq.side != Side::Bid) continue;
        EXPECT_LE(tq.price, kMgCentre)
            << "bid tier " << static_cast<int>(tq.tier_index) << " at "
            << tq.price << " sits above the fair-value centre " << kMgCentre
            << " -- the ~6.3% overpay the cap exists to stop";
    }
    const bool shipped_has_ask = std::any_of(
        shipped.begin(), shipped.end(),
        [](const TierQuote& tq) { return tq.side == Side::Ask; });
    EXPECT_TRUE(shipped_has_ask) << "the ask side must still be quoted";

    // ARM 3 -- the "fix it back onto one snapshot" edit: same stale offers,
    // but the verdict taken from the snapshot (true/true). This is the legacy
    // path, and on this book it SELF-CROSSES: every bid anchors at
    // best_comp_bid + 1 tick = 1.5001, above the ask tiers around 1.4185, and
    // the post-adjustment cross check drops all six tiers.
    const auto collapsed = mg_ladder(snap.bid_side_anchor_ok,
                                     snap.ask_side_anchor_ok,
                                     comp_book.offers);
    EXPECT_TRUE(collapsed.empty())
        << "re-pointing Step 7 at the snapshot must reproduce the legacy "
           "self-cross (see BookSideQualityLadder."
           "UnexaminedBookSelfCrossesAndLosesTheWholeLadder); if this now "
           "quotes, the stale book is no longer poisonous and the arm above "
           "proves nothing";

    // [review round 9] DELIBERATELY NOTHING FURTHER HERE. This test used to
    // close with
    //
    //     ASSERT_NE(shipped.size(), collapsed.size());
    //
    // described as "the price relation, not merely as inequality" while in
    // fact comparing cardinalities. It could not fail for any reason the
    // ASSERT_FALSE(shipped.empty()) and EXPECT_TRUE(collapsed.empty()) above
    // had not already reported -- the "asserted what an earlier check
    // guaranteed" shape this project mutation-hunts for.
    //
    // It was briefly replaced with a max-bid-vs-kMgCentre check, which is no
    // better: the disqualified branch sets bid_cap = min(bbo_ref, mid) = the
    // centre, so every bid is BELOW the centre by clamp, unconditionally, and
    // the per-bid EXPECT_LE loop in ARM 2 already states it. Verified by
    // mutation: reverting the SIDEQUALITY bbo_ref/bid_cap fallback in
    // liquidity.cpp is caught at ASSERT_FALSE(shipped.empty()) instead --
    // the ladder self-crosses and empties before any price relation is
    // reachable. The difference in KIND between the two arms (one quotes,
    // the other quotes nothing) IS the assertion, and it is already made.
}

// Named for what it measures. It was called
// Step8VetoesAStrictSubsetOfWhatTheStaleVerdictWouldAdmit and labelled "THE
// INVARIANT"; it is neither an invariant nor primarily about subsetting --
// see TheTwoPoliciesAdmitNonNestedSets below.
TEST(BookSideQualityMixedGeneration, Step8VetoesAsksTheStaleVerdictWouldAdmit)
{
    State state;
    MarketDataFeed feed(sq_cfg(), state);
    const std::string pair = "XCH/BYC";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, kAnchor, 0.0);
    feed.ingest_competing_offers(pair, dislocated_book(), {},
                                 kMojosPerXch, 1'000);
    feed.refresh({pair});
    feed.ingest_dexie(pair, 1.5000, 4.9995, 0.0, 0.0);
    feed.refresh({pair});

    const auto comp_book = feed.get_competing_book(pair);
    const auto snap      = state.get_market(pair);
    ASSERT_FALSE(comp_book.ask_side_anchor_ok);
    ASSERT_TRUE(snap.ask_side_anchor_ok);

    // The ladder Step 7 actually hands to Step 8 in this state.
    const auto ladder = mg_ladder(comp_book.bid_side_anchor_ok,
                                  comp_book.ask_side_anchor_ok,
                                  comp_book.offers);
    ASSERT_FALSE(ladder.empty());

    // WHAT SHIPS: Step 8 polices that ladder against the CURRENT snapshot.
    const auto fresh = mg_step8(
        ladder, snap.bid_side_anchor_ok, snap.ask_side_anchor_ok,
        snap.book_side_ref, snap.mid_price, snap.best_bid, snap.best_ask);

    // THE PRIMARY MUTATION: Step 8 sourcing the verdict from comp_book --
    // i.e. a generation stamp that detects the mismatch and "adapts" by
    // adopting the stale verdict. This is the fail-OPEN direction.
    const auto stale = mg_step8(
        ladder, comp_book.bid_side_anchor_ok, comp_book.ask_side_anchor_ok,
        mg_to_mojos(comp_book.book_side_ref),
        snap.mid_price, snap.best_bid, snap.best_ask);

    // Check 1 is inert on this fixture BY CONSTRUCTION (no CEX leg, so the
    // published mid IS the BBO midpoint). Asserted, not assumed: if it ever
    // fires, `fresh` empties and the subset relation below becomes trivially
    // true while measuring nothing.
    ASSERT_TRUE(fresh.mid_check_ran)
        << "the reset verdict is the LEGACY verdict: Check 1 must RUN";
    ASSERT_FALSE(stale.mid_check_ran)
        << "the stale verdict skips Check 1 -- one of the THREE changes "
           "adopting it would make (ask_tier_ref -> side_ref, Check 1 "
           "skipped, and effective_mid -> side_ref, which pulls the other "
           "way; see the round-9 note at the top of this section)";
    ASSERT_FALSE(fresh.mid_check_fired)
        << "degenerate fixture: Check 1 cleared every tier (mid_dev="
        << fresh.mid_dev << "), so the Check 2 comparison below is vacuous";

    // [review round 9] THE LOAD-BEARING PRECONDITION, ASSERTED FIRST.
    // The subset relation below is NOT an invariant of the two policies --
    // TheTwoPoliciesAdmitNonNestedSets proves they admit non-nested sets on
    // this exact book. It holds HERE only because every bid tier this ladder
    // produces lands inside the region where the two policies happen to
    // agree: at or below best_bid * (1 + max_aggressive_dev), which is the
    // point above which the reset's poisoned-high effective_mid starts
    // routing bids through the WIDE passive cap that the stale verdict's
    // effective_mid would route through the tight aggressive one.
    //
    // Assert it, so that a future fixture change which breaks the premise
    // fails ON THE PREMISE instead of misreporting a code regression.
    {
        const StrategyConfig sc{};
        const auto bid_divergence_floor = static_cast<Mojo>(std::llround(
            static_cast<double>(snap.best_bid)
            * (1.0 + sc.bbo_sanity_max_aggressive_dev)));
        for (const auto& tq : ladder) {
            if (tq.side != Side::Bid) continue;
            ASSERT_LE(tq.price, bid_divergence_floor)
                << "PRECONDITION BROKEN, NOT A CODE REGRESSION: bid tier "
                << static_cast<int>(tq.tier_index) << " at " << tq.price
                << " sits above best_bid*(1+max_aggressive_dev) = "
                << bid_divergence_floor << ", which is where the two Step 8 "
                   "policies stop agreeing on bids. Above that line the mix "
                   "is the MORE permissive of the two and the relation below "
                   "inverts -- correctly. Re-read the round-9 comment at the "
                   "top of this section before 'fixing' anything.";
        }
    }

    // Given that precondition, the delta is entirely on the ask side and the
    // mix is the tighter policy. This is a statement about THIS ladder on
    // THIS book, not a general property.
    EXPECT_TRUE(mg_is_subset(fresh.survivors, stale.survivors))
        << "on this ladder the mixed-source policy admitted a tier the stale "
           "verdict would have vetoed -- given the precondition above holds, "
           "that means the ask-side references moved";
    EXPECT_LT(fresh.survivors.size(), stale.survivors.size())
        << "the two policies agree on this fixture, so the subset assertion "
           "above is vacuous -- the fixture must be repaired before it can "
           "be trusted";

    // And the direction, named concretely rather than left as a cardinality.
    // The fresh raw best_ask is the junk 4.9995 touch, so every honest ask
    // tier around 1.42 reads as ~72% "aggressive" and dies. Under the stale
    // verdict those same tiers reference the 1.41022765 anchor, read as
    // passive, and live. Asks are the whole delta; bids survive both ways.
    const auto ask_count = [](const std::vector<TierQuote>& v) {
        return std::count_if(v.begin(), v.end(), [](const TierQuote& tq) {
            return tq.side == Side::Ask;
        });
    };
    EXPECT_EQ(ask_count(fresh.survivors), 0)
        << "measured against the junk 4.9995 touch, no ask tier may survive";
    EXPECT_GT(ask_count(stale.survivors), 0)
        << "referenced to the stale anchor those same asks pass -- which is "
           "exactly the extra permission adopting the stale verdict grants";
}

// ---------------------------------------------------------------------------
// [review round 9] THE CLAIM THAT WAS WRONG, PINNED SO IT CANNOT COME BACK.
//
// Round 8 shipped a comment at the Step 7 site in engine.cpp, duplicated at
// the top of this section, asserting that the mixed-generation read "produces
// a SUBSET of the quotes the stale verdict alone would admit -- fewer, never a
// quote either policy alone would refuse". It was offered as THE safety
// argument for keeping the two stores split.
//
// It is false. The derivation ("Step 7 can only tighten, Step 8 can only
// veto") does not compose, because BOTH halves RE-REFERENCE BEFORE THEY JUDGE.
// bookside::step8_references does not just decide whether to veto -- it picks
// effective_mid and the per-side tier reference, and classify_tier's verdict
// is a function of those. Different references admit sets that are not nested
// in either direction.
//
// This test measures the admitted sets directly off the two pure functions the
// Engine calls, on the fixture's own live XCH/BYC state, and asserts that BOTH
// exclusive directions are non-empty. If anyone restates the subset claim --
// in a comment, or by "fixing" step8_references to make it true -- this goes
// red and names the price that breaks it.
//
// The ladder is deliberately NOT involved. The previous test is bounded by
// where its six tiers happen to land; this one is about the policies.
// ---------------------------------------------------------------------------
TEST(BookSideQualityMixedGeneration, TheTwoPoliciesAdmitNonNestedSets)
{
    const StrategyConfig sc{};
    const double kAgg = sc.bbo_sanity_max_aggressive_dev;
    const double kPas = sc.bbo_sanity_max_passive_dev;

    // The state the fetch failure leaves behind: raw self-inclusive touches
    // from the ticker, and the anchor the (now stale) verdict was measured on.
    const double best_bid = 1.5000;
    const double best_ask = 4.9995;
    const double bbo_mid  = (best_bid + best_ask) / 2.0;

    // WHAT SHIPS: ingest_dexie reset the snapshot verdict to the legacy
    // triple, so Step 8 reads (true, true, ref=0).
    const auto fresh = xop::bookside::step8_references(
        true, true, 0.0, bbo_mid, best_bid, best_ask);
    // THE REJECTED OPTION: Step 8 adopting Step 7's cached verdict.
    const auto stale = xop::bookside::step8_references(
        true, false, kAnchor, bbo_mid, best_bid, best_ask);

    // Non-vacuity: the two policies must actually be different policies.
    ASSERT_NE(fresh.effective_mid, stale.effective_mid)
        << "the two reference sets are identical, so every count below is "
           "zero and this test measures nothing";
    ASSERT_NE(fresh.ask_tier_ref, stale.ask_tier_ref);

    const auto admits = [&](const xop::bookside::Step8References& r,
                            bool is_ask, double price) {
        return xop::strategy::classify_tier(
                   is_ask, price,
                   is_ask ? r.ask_tier_ref : r.bid_tier_ref,
                   r.effective_mid, kAgg, kPas)
               == xop::strategy::BboVerdict::Pass;
    };

    // ---- Sweep both sides and collect the two exclusive directions. -------
    struct Excl {
        int    count{0};
        double lo{0.0};
        double hi{0.0};
    };
    const auto sweep = [&](bool is_ask, bool fresh_only) {
        Excl e{};
        for (int i = 0; i <= 80'000; ++i) {
            const double p = 0.20 + (7.0 * static_cast<double>(i)) / 80'000.0;
            const bool f = admits(fresh, is_ask, p);
            const bool s = admits(stale, is_ask, p);
            const bool hit = fresh_only ? (f && !s) : (s && !f);
            if (!hit) continue;
            if (e.count == 0) e.lo = p;
            e.hi = p;
            ++e.count;
        }
        return e;
    };

    // ---- BIDS: neither policy's admitted set contains the other's. --------
    const Excl bid_fresh_only = sweep(/*is_ask=*/false, /*fresh_only=*/true);
    const Excl bid_stale_only = sweep(/*is_ask=*/false, /*fresh_only=*/false);

    EXPECT_GT(bid_fresh_only.count, 0)
        << "THE SUBSET CLAIM WOULD BE TRUE IF THIS WERE ZERO. The reset "
           "verdict restores effective_mid to the BBO midpoint " << bbo_mid
        << ", poisoned high by the junk ask; classify_tier's bid passive "
           "rule keys on that midpoint, so bids above best_bid*(1+"
        << kAgg << ") = " << best_bid * (1.0 + kAgg)
        << " get the WIDE passive cap instead of the tight aggressive one. "
           "If this is zero, either step8_references changed or the caps did "
           "-- do NOT restore the subset language without re-deriving it.";
    EXPECT_GT(bid_stale_only.count, 0)
        << "the stale verdict admits low bids the reset refuses (its "
           "effective_mid is the anchor, which supplies a second passive "
           "safe harbor down at anchor*(1-" << kPas << "))";

    // The mix is the MORE permissive policy for bids in this band -- the
    // fail-open direction the round-8 comment claimed could not exist.
    EXPECT_GT(bid_fresh_only.hi, bid_stale_only.hi)
        << "the shipped mix must be the one that reaches higher on bids";

    // ---- ASKS: the two sets are not merely non-nested, they are DISJOINT. -
    const Excl ask_fresh_only = sweep(/*is_ask=*/true, /*fresh_only=*/true);
    const Excl ask_stale_only = sweep(/*is_ask=*/true, /*fresh_only=*/false);

    ASSERT_GT(ask_fresh_only.count, 0);
    ASSERT_GT(ask_stale_only.count, 0);
    EXPECT_GT(ask_fresh_only.lo, ask_stale_only.hi)
        << "the ask sets should not overlap at all: fresh references the raw "
           "4.9995 touch, stale references the " << kAnchor << " anchor, and "
           "the two windows sit either side of each other. fresh=["
        << ask_fresh_only.lo << "," << ask_fresh_only.hi << "] stale=["
        << ask_stale_only.lo << "," << ask_stale_only.hi << "]";

    // One named price, so a failure report carries a reproducible case
    // rather than only a count.
    const double kWitness = 1.66;  // inside (best_bid*1.10, bbo_mid]
    EXPECT_TRUE(admits(fresh, /*is_ask=*/false, kWitness))
        << "the shipped mix admits a " << kWitness << " bid";
    EXPECT_FALSE(admits(stale, /*is_ask=*/false, kWitness))
        << "and the stale verdict refuses it -- which is precisely the "
           "'never a quote either policy alone would refuse' guarantee that "
           "round 8 claimed and round 9 removed";
}

// ---------------------------------------------------------------------------
// [review round 8] The same invariant, reached WITHOUT any fetch failure.
//
// mtx_competitors_ guards a PAIR of stores. The move operations moved
// competing_offers_ and left competing_book_quality_ behind, so a moved-to
// feed held the junk book with NO quality entry -- and get_competing_book()
// falls back to the struct defaults, both sides TRUSTED. Offers survive, the
// evidence that condemned them does not: a pure fail-open, and the round-5
// desync without needing a throw.
// ---------------------------------------------------------------------------
TEST(BookSideQualityMixedGeneration, MovingTheFeedCarriesTheVerdictWithTheBook)
{
    State state;
    const std::string pair = "XCH/BYC";

    MarketDataFeed source(sq_cfg(), state);
    source.ingest_block_height(100);
    source.ingest_reference_anchor(pair, kAnchor, 0.0);
    source.ingest_competing_offers(pair, dislocated_book(), {},
                                   kMojosPerXch, 1'000);
    source.refresh({pair});
    ASSERT_FALSE(source.get_competing_book(pair).ask_side_anchor_ok)
        << "precondition: the verdict exists before the move";

    MarketDataFeed moved(std::move(source));
    const auto book = moved.get_competing_book(pair);

    ASSERT_FALSE(book.offers.empty())
        << "the offers moved across; if they did not, the assertion below "
           "passes for the wrong reason";
    EXPECT_FALSE(book.ask_side_anchor_ok)
        << "the junk book moved but its verdict did not -- get_competing_book "
           "fell back to the both-sides-trusted defaults beside a book it has "
           "no evidence for";
    EXPECT_GT(book.book_side_ref, 0.0)
        << "the anchor that produced the verdict must travel with it";
}

// [review round 9] THE MOVE-ASSIGNMENT ARM. The test above exercises only the
// move CONSTRUCTOR. MarketDataFeed has two move operations and the fix touched
// both, but reverting ONLY
//
//     competing_book_quality_ = std::move(other.competing_book_quality_);
//
// in operator=(MarketDataFeed&&) left the whole suite green -- a surviving
// mutant of exactly the fail-open shape the fix exists to close. Verified by
// mutation, both arms, before and after this test was added.
TEST(BookSideQualityMixedGeneration, MoveAssigningTheFeedCarriesTheVerdictToo)
{
    State state;
    const std::string pair = "XCH/BYC";

    MarketDataFeed source(sq_cfg(), state);
    source.ingest_block_height(100);
    source.ingest_reference_anchor(pair, kAnchor, 0.0);
    source.ingest_competing_offers(pair, dislocated_book(), {},
                                   kMojosPerXch, 1'000);
    source.refresh({pair});
    ASSERT_FALSE(source.get_competing_book(pair).ask_side_anchor_ok)
        << "precondition: the verdict exists before the move-assign";

    // A destination that has never seen this pair, so nothing it already
    // holds can mask a verdict that fails to arrive.
    MarketDataFeed dest(sq_cfg(), state);
    ASSERT_TRUE(dest.get_competing_book(pair).offers.empty())
        << "precondition: the destination starts empty for this pair";

    dest = std::move(source);
    const auto book = dest.get_competing_book(pair);

    ASSERT_FALSE(book.offers.empty())
        << "the offers move-assigned across; if they did not, the assertions "
           "below pass for the wrong reason";
    EXPECT_FALSE(book.ask_side_anchor_ok)
        << "operator=(MarketDataFeed&&) moved the junk book without the "
           "verdict that condemned it -- get_competing_book fell back to the "
           "both-sides-trusted defaults, the round-5 desync with no fetch "
           "failure required";
    EXPECT_GT(book.book_side_ref, 0.0)
        << "the anchor that produced the verdict must travel with it";
}
