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

TEST(BookSideQuality, bypass_threshold_can_never_be_stricter_than_the_gate)
{
    using xop::bookside::effective_agree_max_spread_bps;

    // [review] The reviewer's exact case: gate confirming at 5000, this knob
    // lowered to 750. A 4000 bps book is accepted by book_confirms() as "the
    // whole market repriced" -- so disqualifying both its sides here would
    // strip the confirmation the gate is waiting for, and Step 8 would take
    // its both-sides-disqualified path instead of honouring it.
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(750.0, 5000.0), 5000.0);

    // And the consequence that actually matters: with the derived value, a
    // 4000 bps book far from the anchor is still trusted whole.
    const double derived = effective_agree_max_spread_bps(750.0, 5000.0);
    const auto q = classify_sides(5.60, 5.60 * 1.4918, kAnchor, kBand,
                                  derived);   // ~4000 bps apart, ~4x anchor
    EXPECT_TRUE(q.bypassed)
        << "a book the mid gate would accept as confirmation must not have "
           "its sides disqualified underneath it";
}

TEST(BookSideQuality, raising_the_bypass_above_the_gate_does_take_effect)
{
    using xop::bookside::effective_agree_max_spread_bps;
    // More permissive than the gate is harmless -- it only ever adds trust --
    // so the operator's larger value stands.
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(8000.0, 5000.0), 8000.0);
}

TEST(BookSideQuality, derived_bypass_ignores_unusable_inputs)
{
    using xop::bookside::effective_agree_max_spread_bps;
    // A disabled/absent side of the pair contributes nothing rather than
    // poisoning the result; both unusable means the bypass is simply off,
    // which is the safe direction because it only ever ADDS trust.
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(0.0, 5000.0), 5000.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(5000.0, 0.0), 5000.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(kNaN, 5000.0), 5000.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(kInf, 5000.0), 5000.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(-1.0, 5000.0), 5000.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(effective_agree_max_spread_bps(kNaN, kNaN), 0.0);
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
