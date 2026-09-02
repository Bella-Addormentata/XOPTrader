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
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "xop/execution/book_side_quality.hpp"
#include "xop/execution/market_data.hpp"
#include "xop/state.hpp"

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
