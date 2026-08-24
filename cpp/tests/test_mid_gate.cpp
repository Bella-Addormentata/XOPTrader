// ---------------------------------------------------------------------------
// test_mid_gate -- pins for the [S20 2026-08-24] published-mid plausibility
// gate, anchor chain, and triangulated implied cross (xop/execution/
// mid_gate.hpp).
//
// The replay tests encode the live incidents verbatim: the numbers are the
// ones from the logs, and each test names the failure it pins shut.
// ---------------------------------------------------------------------------

#include "xop/execution/mid_gate.hpp"

#include <gtest/gtest.h>

#include <xop/execution/market_data.hpp>
#include <xop/state.hpp>

#include <algorithm>
#include <limits>
#include <thread>
#include <vector>

using namespace xop::midgate;

namespace {

GateInputs base_inputs()
{
    GateInputs in;
    in.anchor_band_ratio           = 3.0;
    in.book_confirm_max_spread_bps = 5000.0;
    in.max_step_frac               = 0.5;
    return in;
}

}  // namespace

// ---------------------------------------------------------------------------
// Anchor chain
// ---------------------------------------------------------------------------

TEST(AnchorChainTest, PriorityOrder)
{
    AnchorCandidates c;
    c.cex_mid             = 1.01;
    c.implied_cross       = 1.02;
    c.amm_mid             = 1.03;
    c.fair_value_estimate = 1.04;
    c.peg_target          = 1.00;

    EXPECT_EQ(select_anchor(c).source, AnchorSource::Cex);
    c.cex_mid = 0.0;
    EXPECT_EQ(select_anchor(c).source, AnchorSource::ImpliedCross);
    c.implied_cross = 0.0;
    EXPECT_EQ(select_anchor(c).source, AnchorSource::Amm);
    c.amm_mid = 0.0;
    EXPECT_EQ(select_anchor(c).source, AnchorSource::FairValue);
    c.fair_value_estimate = 0.0;
    EXPECT_EQ(select_anchor(c).source, AnchorSource::Peg);
    EXPECT_DOUBLE_EQ(select_anchor(c).value, 1.00);
    c.peg_target = 0.0;
    EXPECT_EQ(select_anchor(c).source, AnchorSource::None);
    EXPECT_DOUBLE_EQ(select_anchor(c).value, 0.0);
}

// ---------------------------------------------------------------------------
// Implied cross (triangulation)
// ---------------------------------------------------------------------------

TEST(ImpliedCrossTest, BycUsdcViaXchLegs)
{
    // The motivating triangle: BYC/wUSDC.b implied through the two XCH
    // legs.  Orientation comes from the PRODUCTION helper rather than
    // hand-supplied flags -- an earlier version of this test set both
    // inverts itself, so it exercised only the multiplication and a
    // reversal in any orientation branch would have left it green.
    const auto o = orient_triangle(/*target*/ "byc", "usdc",
                                   /*p1*/     "xch", "byc",
                                   /*p2*/     "xch", "usdc");
    ASSERT_TRUE(o.has_value());
    EXPECT_TRUE(o->invert_first)   << "XCH/BYC is C/A -- must invert";
    EXPECT_FALSE(o->invert_second) << "XCH/wUSDC.b is C/B -- used as-is";

    // XCH at $1.42, BYC honest at ~$1.01: XCH/BYC ~ 1.4059 -> implied
    // (1/1.4059) * 1.42 ~ 1.0100.
    CrossLeg xch_byc{1.4059, 120.0, true, o->invert_first};
    CrossLeg xch_usdc{1.42, 80.0, true, o->invert_second};
    EXPECT_NEAR(implied_cross(xch_byc, xch_usdc, 300.0), 1.0100, 0.0005);
}

TEST(ImpliedCrossTest, OrientationCoversAllFourCombinations)
{
    // Target A/B through common asset C, in every configured direction.
    // Getting any one backwards yields a silently reciprocal anchor, so
    // each branch is asserted individually.
    {   // A/C and C/B -- neither inverted
        const auto o = orient_triangle("a", "b", "a", "c", "c", "b");
        ASSERT_TRUE(o.has_value());
        EXPECT_FALSE(o->invert_first);
        EXPECT_FALSE(o->invert_second);
    }
    {   // C/A and C/B -- first inverted
        const auto o = orient_triangle("a", "b", "c", "a", "c", "b");
        ASSERT_TRUE(o.has_value());
        EXPECT_TRUE(o->invert_first);
        EXPECT_FALSE(o->invert_second);
    }
    {   // A/C and B/C -- second inverted
        const auto o = orient_triangle("a", "b", "a", "c", "b", "c");
        ASSERT_TRUE(o.has_value());
        EXPECT_FALSE(o->invert_first);
        EXPECT_TRUE(o->invert_second);
    }
    {   // C/A and B/C -- both inverted
        const auto o = orient_triangle("a", "b", "c", "a", "b", "c");
        ASSERT_TRUE(o.has_value());
        EXPECT_TRUE(o->invert_first);
        EXPECT_TRUE(o->invert_second);
    }
}

TEST(ImpliedCrossTest, OrientationRejectsNonTriangles)
{
    // Leg 1 does not touch the target's base.
    EXPECT_FALSE(orient_triangle("a", "b", "x", "y", "c", "b").has_value());
    // Leg 2 does not reach the target's quote.
    EXPECT_FALSE(orient_triangle("a", "b", "a", "c", "c", "z").has_value());
    // The "triangle" through the target's own quote is the target itself.
    EXPECT_FALSE(orient_triangle("a", "b", "a", "b", "b", "b").has_value());
}

TEST(ImpliedCrossTest, OrientationRoundTripsAReciprocalPair)
{
    // Same economic triangle expressed both ways must imply the SAME
    // price -- the sharpest check that no branch is reciprocal.
    const auto fwd = orient_triangle("a", "b", "a", "c", "c", "b");
    const auto rev = orient_triangle("a", "b", "c", "a", "b", "c");
    ASSERT_TRUE(fwd.has_value());
    ASSERT_TRUE(rev.has_value());

    // rate(A->C) = 2.0, rate(C->B) = 3.0  =>  implied(A/B) = 6.0
    const double f = implied_cross({2.0, 50.0, true, fwd->invert_first},
                                   {3.0, 50.0, true, fwd->invert_second},
                                   300.0);
    // Same rates, pairs configured the other way round: C/A = 0.5, B/C = 1/3.
    const double r = implied_cross({0.5, 50.0, true, rev->invert_first},
                                   {1.0 / 3.0, 50.0, true, rev->invert_second},
                                   300.0);
    EXPECT_NEAR(f, 6.0, 1e-9);
    EXPECT_NEAR(r, 6.0, 1e-9);
}

TEST(ImpliedCrossTest, UnhealthyLegRefusesAnchor)
{
    CrossLeg good{1.42, 80.0, true, false};

    // Wide leg: spread above the cap.
    CrossLeg wide{1.4059, 900.0, true, true};
    EXPECT_DOUBLE_EQ(implied_cross(wide, good, 300.0), 0.0);

    // One-sided/crossed leg: compute_spread_bps reports 0.
    CrossLeg one_sided{1.4059, 0.0, true, true};
    EXPECT_DOUBLE_EQ(implied_cross(one_sided, good, 300.0), 0.0);

    // Stale leg.
    CrossLeg stale{1.4059, 120.0, false, true};
    EXPECT_DOUBLE_EQ(implied_cross(stale, good, 300.0), 0.0);

    // No mid at all.
    CrossLeg empty{0.0, 0.0, true, true};
    EXPECT_DOUBLE_EQ(implied_cross(empty, good, 300.0), 0.0);
}

TEST(ImpliedCrossTest, RealDepegMovesTheImpliedCross)
{
    // A GENUINE BYC collapse moves the XCH/BYC leg: more BYC per XCH.
    // BYC at $0.70 with XCH at $1.42 -> XCH/BYC ~ 2.0286.  The implied
    // cross follows the market honestly instead of pinning 1.0 -- which is
    // why anchoring on the triangle does not mask a real depeg.
    CrossLeg xch_byc{2.0286, 150.0, true, true};
    CrossLeg xch_usdc{1.42, 80.0, true, false};
    EXPECT_NEAR(implied_cross(xch_byc, xch_usdc, 300.0), 0.70, 0.001);
}

// ---------------------------------------------------------------------------
// Gate verdicts -- incident replays
// ---------------------------------------------------------------------------

TEST(MidGateTest, Incident_20260824_JunkPrintRejectedFromCycleOne)
{
    // 09:25 replay: fresh process, NO previous accepted mid, the peg anchor
    // exists from cycle 1 (stablecoin pair).  The old per-offer filter was
    // skipped entirely when no reference existed, which is how v0.9.20
    // re-poisoned within 40 minutes of a clean restart.  The junk book is
    // NOT two-sided (one absurd resting offer), so no book confirmation.
    auto in = base_inputs();
    in.candidate_mid    = 187.461980;
    in.anchor           = {1.0, AnchorSource::Peg};
    in.last_accepted_mid = 0.0;   // first cycle
    in.book_two_sided   = false;

    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);
}

TEST(MidGateTest, Incident_ByteIdenticalFreezeNeverReanchors)
{
    // The 12h frozen 187.461980: the gate rejects it on every one of 1000
    // cycles, and because the anchor chain never reads the pair's own mid
    // or history, no amount of repetition re-admits it (the old
    // self-referential reference "learned" the junk in one cycle).
    auto in = base_inputs();
    in.candidate_mid  = 187.461980;
    in.anchor         = {1.0, AnchorSource::Peg};
    in.book_two_sided = false;

    for (int cycle = 0; cycle < 1000; ++cycle) {
        ASSERT_EQ(gate_mid(in), GateVerdict::RejectAnchor)
            << "re-anchored on cycle " << cycle;
        // A rejected mid is never accepted, so last_accepted stays 0.
    }
}

TEST(MidGateTest, HonestMidAcceptedWhileJunkRejected)
{
    // The lock-in inversion: with the junk mid as reference, honest ~1.0
    // offers were the "outliers".  Against the peg anchor the verdicts
    // come out the right way around.
    auto in = base_inputs();
    in.anchor = {1.0, AnchorSource::Peg};

    in.candidate_mid = 1.011;   // honest dexie ticker level
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);

    in.candidate_mid = 187.461980;
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);

    in.candidate_mid = 452.2;   // the other overnight print
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);
}

TEST(MidGateTest, RealDepegInsideBandPasses)
{
    // A genuine 25% depeg (0.75 against a 1.0 peg) is INSIDE the 3x band:
    // the gate refuses absurdity, not repricing.  The mid publishes, the
    // depeg detector sees it, equity marks down honestly.  (The 0.75-class
    // incident is instead handled by the valuation-GRADE flag: a
    // last-trade-only print publishes but does not mark equity.)
    auto in = base_inputs();
    in.candidate_mid = 0.75;
    in.anchor        = {1.0, AnchorSource::Peg};
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);
}

TEST(MidGateTest, DeepCollapseWithLiveBookPassesViaConfirmation)
{
    // Beyond-band move WITH a fresh two-sided coherent book: the whole
    // market repriced (e.g. a true stablecoin failure trading at $0.20).
    // Executable two-sided evidence overrides the anchor.
    auto in = base_inputs();
    in.candidate_mid   = 0.20;               // 5x below peg -- outside 3x band
    in.anchor          = {1.0, AnchorSource::Peg};
    in.book_two_sided  = true;
    in.book_fresh      = true;
    in.book_fresh_independent = true;        // screened against the peg
    in.book_spread_bps = 800.0;
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);

    // Screened only against this pair's OWN history: not evidence against
    // an anchor.  An anchor can arrive after the offers were filtered
    // within one heartbeat, so without this distinction a self-screened
    // book would confirm an anchor-band breach on the recovery cycle.
    in.book_fresh_independent = false;
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);
    in.book_fresh_independent = true;

    // Same move on a stale book: no confirmation, rejected.  Independent
    // screening is a stricter form of freshness, never a substitute --
    // book_confirms requires book_fresh in both modes, so an incoherent
    // pair of flags cannot widen the escape.
    in.book_fresh = false;
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);

    // Same move with an incoherent (blown-out) spread: rejected.
    in.book_fresh      = true;
    in.book_spread_bps = 19898.4;   // the live incident's spread alert value
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);
}

TEST(MidGateTest, AnchorlessMaxStepFallback)
{
    // Non-peg pair with no external reference at all: the only defence is
    // the per-cycle step bound against the last ACCEPTED mid.
    auto in = base_inputs();
    in.anchor           = {};          // none
    in.last_accepted_mid = 2.84;

    in.candidate_mid = 2.90;           // ~2% move
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);

    in.candidate_mid = 28.4;           // 10x in one heartbeat
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectStep);

    // ...unless a fresh two-sided book confirms the move.  The STEP
    // escape accepts self-history screening -- an anchorless pair has
    // nothing better, and refusing would latch it silent forever.
    in.book_two_sided  = true;
    in.book_fresh      = true;
    in.book_fresh_independent = false;
    in.book_spread_bps = 400.0;
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);
}

TEST(MidGateTest, FirstCycleAnchorlessAccepts)
{
    // No anchor AND no accepted history: nothing to test against, accept.
    // (Stablecoin pairs never take this path -- the peg anchor exists from
    // cycle 1 -- and majors have CEX.)
    auto in = base_inputs();
    in.candidate_mid    = 2.84;
    in.anchor           = {};
    in.last_accepted_mid = 0.0;
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);
}

TEST(MidGateTest, DisabledKnobsPassEverything)
{
    auto in = base_inputs();
    in.candidate_mid = 187.461980;
    in.anchor        = {1.0, AnchorSource::Peg};

    in.anchor_band_ratio = 0.0;    // <= 1 disables the anchor test...
    in.max_step_frac     = 0.0;    // ...and the step test.
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);
}

TEST(MidGateTest, NoMidIsNotGated)
{
    auto in = base_inputs();
    in.candidate_mid = 0.0;
    in.anchor        = {1.0, AnchorSource::Peg};
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);
}

TEST(MidGateTest, StaleBookCannotConfirmAnAbsurdMid)
{
    // Review finding: the raw dexie ticker BBO (which includes our OWN
    // resting offers) survives a throwing full-offer fetch with a freshly
    // re-stamped poll time.  If that counted as book confirmation, the bot
    // would read its own quotes back as proof the market agrees -- and the
    // junk mid would publish anyway.  apply_mid_gate now measures
    // freshness on ob_updated_at (stamped only by the FILTERED ingest);
    // here the pure gate pins the consequence: without book_fresh, no
    // escape.
    auto in = base_inputs();
    in.candidate_mid   = 187.461980;
    in.anchor          = {1.0, AnchorSource::Peg};
    in.book_two_sided  = true;
    in.book_spread_bps = 300.0;   // looks coherent...
    in.book_fresh      = false;   // ...but is not third-party-fresh
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);
}

TEST(MidGateTest, NonFiniteCandidateNeverPublishes)
{
    // NaN fails every comparison, so an unguarded gate would wave it
    // through as "in band" and it would become the published mid.
    auto in = base_inputs();
    in.anchor = {1.0, AnchorSource::Peg};

    in.candidate_mid = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);

    in.candidate_mid = std::numeric_limits<double>::infinity();
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);
}

TEST(MidGateTest, NonFiniteAnchorDoesNotDisableTheGate)
{
    // A NaN anchor must not silently switch the anchor test off; the step
    // bound still applies.
    auto in = base_inputs();
    in.anchor = {std::numeric_limits<double>::quiet_NaN(), AnchorSource::Cex};
    in.last_accepted_mid = 1.00;
    in.candidate_mid     = 187.461980;
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectStep);
}

// ---------------------------------------------------------------------------
// Median selection (engine-side triangulation combines candidates this way)
// ---------------------------------------------------------------------------

TEST(ImpliedCrossTest, EvenCandidateCountAveragesTheMiddlePair)
{
    // Calls the SAME median_of that Engine::compute_implied_cross_anchor
    // uses -- an earlier version of this test reimplemented the formula in
    // a lambda and so would have stayed green through a production
    // regression.  With an even count the upper-middle observation would
    // let one high outlier become the entire anchor, and n == 2 (two
    // triangles through different common assets) is the common case here,
    // not a corner.
    std::vector<double> two{1.40, 1.00};
    EXPECT_NEAR(median_of(two), 1.20, 1e-12);

    std::vector<double> one{1.01};
    EXPECT_NEAR(median_of(one), 1.01, 1e-12);

    std::vector<double> three{1.40, 0.98, 1.00};
    EXPECT_NEAR(median_of(three), 1.00, 1e-12);

    std::vector<double> none;
    EXPECT_DOUBLE_EQ(median_of(none), 0.0);
}

// ===========================================================================
// Ingestion-path tests.
//
// The pure-gate tests above exercise gate_mid() directly, which -- as PR
// review correctly pointed out -- cannot catch a contradiction between the
// gate and the ingestion that FEEDS it.  These drive the real feed.
// ===========================================================================

namespace {

using namespace xop;

MarketDataConfig gate_cfg() {
    MarketDataConfig cfg;
    cfg.cex_freshness_threshold_sec = 0.0;   // no decay; exact blends
    cfg.amm_blend_weight            = 0.0;
    cfg.mid_gate_enabled            = true;
    return cfg;
}

CompetingOffer mk_offer(const std::string& id, Side side, double price,
                        Mojo size) {
    CompetingOffer o;
    o.offer_id = id;
    o.side     = side;
    o.price    = static_cast<Mojo>(std::llround(price * static_cast<double>(kMojosPerXch)));
    o.size     = size;
    return o;
}

}  // namespace

// A genuine beyond-band collapse, delivered as a real two-sided filtered
// book.  The offer filter must NOT strip the honest offers near the new
// market -- if it did, no book could ever confirm the move and a real
// depeg would be refused forever (the escape would be decorative).
TEST(MidGateIngestTest, RealCollapseSurvivesTheOfferFilterAndPublishes) {
    State state;
    MarketDataFeed feed(gate_cfg(), state);
    const std::string pair = "BYC/wUSDC.b";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, /*implied_cross=*/0.0,
                                 /*peg_target=*/1.0);

    // The whole market has repriced to ~0.20 (5x below peg, outside the
    // 3x gate band) and says so with a coherent two-sided book.
    std::vector<CompetingOffer> offers{
        mk_offer("b1", Side::Bid, 0.198, 5'000'000),
        mk_offer("a1", Side::Ask, 0.202, 5'000'000),
    };
    feed.ingest_competing_offers(pair, offers, {}, 1'000, 1'000);
    feed.refresh({pair});

    EXPECT_GT(feed.get_mid_price(pair), 0.0)
        << "a real collapse with executable two-sided evidence must publish";
    EXPECT_NEAR(feed.get_mid_price(pair), 0.20, 0.01);
}

// The same absurd print the incident produced, with NO third-party book
// behind it, must not publish -- and must not be rescued by the raw
// ticker BBO that ingest_dexie writes every heartbeat.
TEST(MidGateIngestTest, JunkTickerCannotConfirmItself) {
    State state;
    MarketDataFeed feed(gate_cfg(), state);
    const std::string pair = "BYC/wUSDC.b";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, 0.0, /*peg_target=*/1.0);

    // Raw ticker reports a two-sided book around the junk level.  These
    // values are self-inclusive (they may be our own resting offers) and
    // no filtered ingest has run, so they must not count as evidence.
    feed.ingest_dexie(pair, /*bid=*/4.00, /*ask=*/4.40,
                      /*last_trade=*/0.0, /*vol=*/0.0);
    feed.refresh({pair});

    EXPECT_DOUBLE_EQ(feed.get_mid_price(pair), 0.0)
        << "raw ticker BBO must not confirm its own band breach";
    EXPECT_FALSE(feed.mid_valuation_grade(pair));
    EXPECT_FALSE(feed.book_evidence_fresh(pair));
}

// Provenance flips back and forth with the two writers.
TEST(MidGateIngestTest, ProvenanceClearedByRawTickerRestoredByFilteredBook) {
    State state;
    MarketDataFeed feed(gate_cfg(), state);
    const std::string pair = "XCH/wUSDC.b";

    feed.ingest_block_height(100);
    // A CEX leg stands in for the anchor production injects before the
    // ingest loop; without one the book is unscreened and cannot serve as
    // evidence at all (see OffersFilteredWithoutAnAnchorCannotConfirm).
    feed.ingest_cex_reference(pair, 1.42);
    std::vector<CompetingOffer> offers{
        mk_offer("b1", Side::Bid, 1.40, 5'000'000'000'000LL),
        mk_offer("a1", Side::Ask, 1.44, 5'000'000'000'000LL),
    };
    feed.ingest_competing_offers(pair, offers, {}, kMojosPerXch, 1'000);
    EXPECT_TRUE(feed.book_evidence_fresh(pair));

    // A raw ticker poll lands: the BBO in the state is now self-inclusive.
    feed.ingest_dexie(pair, 1.30, 1.55, 0.0, 0.0);
    EXPECT_FALSE(feed.book_evidence_fresh(pair))
        << "raw ticker must invalidate book provenance until the filtered "
           "ingest runs again";

    feed.ingest_competing_offers(pair, offers, {}, kMojosPerXch, 1'000);
    EXPECT_TRUE(feed.book_evidence_fresh(pair));
}

// An infinite candidate must not win the anchor chain and thereby disable
// the plausibility check -- it must be skipped so a valid lower-priority
// anchor is used instead.
TEST(AnchorChainTest, NonFiniteCandidateSkippedNotSelected) {
    AnchorCandidates c;
    c.cex_mid    = std::numeric_limits<double>::infinity();
    c.peg_target = 1.0;

    const auto a = select_anchor(c);
    EXPECT_EQ(a.source, AnchorSource::Peg)
        << "an infinite CEX value must not win the chain";
    EXPECT_DOUBLE_EQ(a.value, 1.0);

    c.cex_mid       = std::numeric_limits<double>::quiet_NaN();
    c.implied_cross = 1.02;
    EXPECT_EQ(select_anchor(c).source, AnchorSource::ImpliedCross);
}

// The offer-absurdity bound must stay wider than the gate band for EVERY
// configurable band, or the book-confirmation escape becomes unreachable
// again at some setting (a fixed 10x bound breaks at band >= 10).
TEST(MidGateIngestTest, OfferBoundAlwaysExceedsTheGateBand) {
    // Calls the production helper, not a copy of it, so this catches the
    // bound being changed or disconnected from configuration.
    for (double band : {1.5, 3.0, 5.0, 9.9, 10.0, 25.0, 100.0}) {
        EXPECT_GT(offer_absurdity_ratio(band), band)
            << "offer filter would strip the confirming book at band " << band;
    }
}

// The bound must also clear the STEP limit, which is configured
// independently.  With max_step_frac 0.95 the gate permits a move to
// 0.05x, so a fixed 10x bound (rejecting below 0.1x) would strip the very
// book that move needs -- the two halves of one feature contradicting
// each other in a configuration the parser accepts.
TEST(MidGateIngestTest, OfferBoundAlsoClearsTheStepLimit) {
    for (double step : {0.5, 0.8, 0.95, 0.99}) {
        const double bound = offer_absurdity_ratio(3.0, step);
        const double gate_allows_down_to = 1.0 - step;
        EXPECT_LT(1.0 / bound, gate_allows_down_to)
            << "offers stripped inside the permitted step at max_step_frac "
            << step;
    }
    // Defaults are unchanged by the new term.
    EXPECT_DOUBLE_EQ(offer_absurdity_ratio(3.0, 0.5), 10.0);
}

// End-to-end version of the same invariant: at a RAISED band, an honest
// offer sitting between the gate band and the derived offer bound must
// survive the filter and still form a book.  A fixed 10x bound passed the
// arithmetic test above but failed this one for band >= 10.
TEST(MidGateIngestTest, OffersBetweenBandAndBoundSurviveTheFilter) {
    auto cfg = gate_cfg();
    cfg.mid_anchor_band_ratio = 12.0;   // deliberately above the old 10x floor
    State state;
    MarketDataFeed feed(cfg, state);
    const std::string pair = "BYC/wUSDC.b";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, 0.0, /*peg_target=*/1.0);

    // 15x the peg: outside the 12x gate band, inside the derived 24x offer
    // bound.  These must reach the book so the confirmation escape can see
    // them; with a fixed 10x bound they were stripped here.
    std::vector<CompetingOffer> offers{
        mk_offer("b1", Side::Bid, 14.9, 5'000'000),
        mk_offer("a1", Side::Ask, 15.1, 5'000'000),
    };
    feed.ingest_competing_offers(pair, offers, {}, 1'000, 1'000);

    const auto bbo = feed.get_dex_bbo(pair);
    EXPECT_GT(bbo.first, 0.0)
        << "bid between the gate band and the offer bound was filtered out";
    EXPECT_GT(bbo.second, 0.0)
        << "ask between the gate band and the offer bound was filtered out";
}

// -infinity satisfies `<= 0.0`, so a no-mid test placed ahead of the
// finiteness test would Accept it and hand a non-finite value to the
// snapshot conversion.  Ordering pin.
TEST(MidGateTest, NegativeInfinityIsRejectedNotTreatedAsNoMid) {
    auto in = base_inputs();
    in.anchor        = {1.0, AnchorSource::Peg};
    in.candidate_mid = -std::numeric_limits<double>::infinity();
    EXPECT_EQ(gate_mid(in), GateVerdict::RejectAnchor);

    // A genuine no-mid is still not gated.
    in.candidate_mid = 0.0;
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);
}

// A zero fair_value_max_age_sec means "expiry disabled" everywhere else in
// this feed (FairValueTest.ZeroMaxAgeDisablesExpiry).  Written as a bare
// `age < max_age`, the anchor chain rejected EVERY fair-value anchor under
// that valid setting -- no age is below zero -- silently dropping the
// source.  Here the fair value is the only anchor available, so the gate
// can only reject the junk mid if the anchor actually survived.
TEST(MidGateIngestTest, ZeroFairValueMaxAgeDoesNotDisableTheAnchor) {
    auto cfg = gate_cfg();
    cfg.fair_value_max_age_sec = 0.0;   // documented as "no expiry"
    State state;
    MarketDataFeed feed(cfg, state);
    const std::string pair = "wmilliETH.b/XCH";   // no peg, no CEX leg

    feed.ingest_block_height(100);
    feed.ingest_fair_value(pair, 1.66, FairValueTier::Triangulated);

    // A junk last-trade print far outside the 3x band around 1.66.
    feed.ingest_dexie(pair, /*bid=*/0.0, /*ask=*/0.0,
                      /*last_trade=*/187.46, /*vol=*/0.0);
    feed.refresh({pair});

    EXPECT_DOUBLE_EQ(feed.get_mid_price(pair), 0.0)
        << "fair-value anchor was dropped, so the junk print published";
}

// The byte-identical-freeze defence, exercised END TO END.
//
// The pure freeze test hands the gate `book_fresh = false` directly, and
// the other ingestion tests perform at most two ingests -- so nothing
// actually drove dex_print_age past kMaxConfirmingPrintAge, and the
// movement check could have been disconnected without failing the suite.
// This repeatedly re-ingests an UNCHANGED two-sided filtered book and
// asserts the expiry boundary from both sides.
TEST(MidGateIngestTest, UnchangedBookExpiresAsConfirmingEvidence) {
    State state;
    MarketDataFeed feed(gate_cfg(), state);
    const std::string pair = "BYC/wUSDC.b";

    feed.ingest_block_height(100);
    feed.ingest_reference_anchor(pair, 0.0, /*peg_target=*/1.0);

    const std::vector<CompetingOffer> frozen{
        mk_offer("b1", Side::Bid, 0.995, 5'000'000),
        mk_offer("a1", Side::Ask, 1.005, 5'000'000),
    };

    // First ingest establishes the print (age 0); each identical ingest
    // thereafter ages it by one.  61 ingests -> age 60, the last value
    // still accepted.
    for (int i = 0; i < 61; ++i) {
        feed.ingest_competing_offers(pair, frozen, {}, 1'000, 1'000);
    }
    ASSERT_EQ(feed.dex_print_age(pair), 60);
    EXPECT_TRUE(feed.book_evidence_fresh(pair))
        << "a book at exactly the age limit must still count";

    feed.refresh({pair});
    EXPECT_TRUE(feed.mid_valuation_grade(pair));

    // One more identical ingest crosses the boundary.
    feed.ingest_competing_offers(pair, frozen, {}, 1'000, 1'000);
    ASSERT_EQ(feed.dex_print_age(pair), 61);
    EXPECT_FALSE(feed.book_evidence_fresh(pair))
        << "a frozen book must stop counting as confirming evidence";

    feed.refresh({pair});
    EXPECT_FALSE(feed.mid_valuation_grade(pair))
        << "a frozen book must not keep marking equity";
}

// A non-finite CEX sample must never grant valuation grade.  The grade
// predicate is independent of select_anchor (which already skips
// non-finite values), so a bare `cex_mid > 0.0` there let +inf bless an
// otherwise-ungraded DEX candidate straight into equity and P&L.
TEST(MidGateIngestTest, NonFiniteCexCannotGrantValuationGrade) {
    State state;
    MarketDataFeed feed(gate_cfg(), state);
    const std::string pair = "XCH/wUSDC.b";

    feed.ingest_block_height(100);

    // Rejected at the ingester, so it never reaches the grade predicate.
    feed.ingest_cex_reference(pair, std::numeric_limits<double>::infinity());
    feed.ingest_cex_reference(pair, std::numeric_limits<double>::quiet_NaN());

    // Only a last-trade print on an empty book -- not book evidence.
    feed.ingest_dexie(pair, /*bid=*/0.0, /*ask=*/0.0,
                      /*last_trade=*/1.42, /*vol=*/0.0);
    feed.refresh({pair});

    EXPECT_FALSE(feed.mid_valuation_grade(pair))
        << "an invalid CEX leg blessed a last-trade-only mid into equity";
}

// The ordering hazard itself, reproduced at the feed level.
//
// Round 4 of review found that anchors were injected AFTER the ingest loop,
// so a first-cycle pair had none while its offers were filtered.  This pins
// the CONSEQUENCE from both sides: with no anchor a coherent two-sided junk
// book survives and publishes, and with the anchor injected first the same
// book is refused.  (Catching a reordering inside the engine heartbeat
// itself needs the engine test harness tracked as T4-07/T8-25; the runtime
// warn-once in ingest_competing_offers covers it in the meantime.)
TEST(MidGateIngestTest, OffersIngestedBeforeAnyAnchorAreUnfiltered) {
    const std::vector<CompetingOffer> junk{
        mk_offer("b1", Side::Bid, 187.0, 5'000'000),
        mk_offer("a1", Side::Ask, 188.0, 5'000'000),
    };

    // WRONG order: offers first, anchor never injected for this cycle.
    {
        State state;
        MarketDataFeed feed(gate_cfg(), state);
        const std::string pair = "BYC/wUSDC.b";
        feed.ingest_block_height(100);
        feed.ingest_competing_offers(pair, junk, {}, 1'000, 1'000);
        feed.refresh({pair});
        EXPECT_GT(feed.get_mid_price(pair), 100.0)
            << "expected the unanchored path to admit junk -- if this now "
               "fails the hazard is closed elsewhere and this test should "
               "be revisited, not deleted";
    }

    // RIGHT order: anchor first, then the identical book.
    {
        State state;
        MarketDataFeed feed(gate_cfg(), state);
        const std::string pair = "BYC/wUSDC.b";
        feed.ingest_block_height(100);
        feed.ingest_reference_anchor(pair, 0.0, /*peg_target=*/1.0);
        feed.ingest_competing_offers(pair, junk, {}, 1'000, 1'000);
        feed.refresh({pair});
        EXPECT_DOUBLE_EQ(feed.get_mid_price(pair), 0.0)
            << "anchor injected before ingest must refuse the junk book";
    }
}


// [S20 2026-08-24] A book filtered with NO anchor cannot confirm a later
// anchor breach.
//
// Round 4 moved the implied-cross and peg injection ahead of the ingest
// loop, but CEX and AMM legs are still ingested afterwards -- so on a
// process's first cycle a CEX- or AMM-anchored (non-stablecoin) pair has
// no anchor while its offers are filtered.  The junk book that results is
// genuinely third-party and genuinely fresh, so provenance alone would
// let it satisfy the confirmation escape and override the anchor that
// arrives moments later: one poisoned publication, one poisoned peak.
//
// Screening is therefore tracked as its own fact, which holds regardless
// of heartbeat ordering.
TEST(MidGateIngestTest, OffersFilteredWithoutAnAnchorCannotConfirm) {
    State state;
    MarketDataFeed feed(gate_cfg(), state);
    const std::string pair = "XCH/wUSDC.b";   // non-stablecoin: no peg anchor

    feed.ingest_block_height(100);

    // First cycle: offers arrive before any anchor exists.  Coherent,
    // two-sided, and absurd -- 100x the real level.
    const std::vector<CompetingOffer> junk{
        mk_offer("b1", Side::Bid, 140.0, 5'000'000'000'000LL),
        mk_offer("a1", Side::Ask, 144.0, 5'000'000'000'000LL),
    };
    feed.ingest_competing_offers(pair, junk, {}, kMojosPerXch, 1'000);

    EXPECT_FALSE(feed.book_evidence_fresh(pair))
        << "an unscreened book must not count as independent evidence";

    // The CEX anchor lands later in the same heartbeat, as it does in
    // production.  The junk book must NOT be able to override it.
    feed.ingest_cex_reference(pair, 1.42);
    feed.refresh({pair});

    EXPECT_DOUBLE_EQ(feed.get_mid_price(pair), 0.0)
        << "unscreened junk book confirmed its own band breach";
    EXPECT_FALSE(feed.mid_valuation_grade(pair));
}

// [S20 2026-08-24] An Unavailable-by-sigma fair value must not anchor the
// gate.
//
// ingest_fair_value deliberately keeps the raw ESTIMATE alive through such
// a solve -- "1.36 +- 467 bps" is a useful width instruction for quoting --
// while zeroing fair_value so that no caller reads a number the solve has
// just declared untrustworthy.  A gate anchor is exactly such a reader: it
// decides whether an honest mid is refused and which offers survive
// filtering.  Anchoring on the estimate would let a value the solver
// distrusts reject a perfectly good published mid.
TEST(MidGateIngestTest, UnavailableFairValueDoesNotAnchorTheGate) {
    State state;
    MarketDataFeed feed(gate_cfg(), state);
    const std::string pair = "wmilliETH.b/XCH";   // no peg, no CEX leg

    feed.ingest_block_height(100);

    // A solve that is anchored but far too uncertain to clamp against, and
    // whose price sits far from the market -- if it anchored the gate, the
    // honest mid below would be refused.
    FairValue fv;
    fv.price      = 20.0;
    fv.tier       = FairValueTier::Unavailable;
    fv.sigma_bps  = 4670.0;
    fv.observations = 3;
    feed.ingest_fair_value(pair, fv);

    // An honest two-sided book near 1.66.
    const std::vector<CompetingOffer> honest{
        mk_offer("b1", Side::Bid, 1.64, 5'000'000'000'000LL),
        mk_offer("a1", Side::Ask, 1.68, 5'000'000'000'000LL),
    };
    feed.ingest_competing_offers(pair, honest, {}, 1'000, kMojosPerXch);
    feed.refresh({pair});

    EXPECT_NEAR(feed.get_mid_price(pair), 1.66, 0.05)
        << "an Unavailable-by-sigma estimate anchored the gate and refused "
           "an honest mid";
}

// [S20 2026-08-24] A stale CEX sample must not lock the book out of
// recovering after a real move.
//
// anchor_candidates drops an EXPIRED CEX sample, but the anchorless
// fallback used to reintroduce that same stale value as the tight 20%
// near reference -- undoing the freshness check, and permanently: after a
// genuine move beyond 20% every honest offer is discarded against the
// obsolete price, so the filtered book can never rebuild and valuation
// grade can never return.
TEST(MidGateIngestTest, StaleCexDoesNotLockOutBookRecovery) {
    auto cfg = gate_cfg();
    cfg.cex_freshness_threshold_sec = 0.001;   // any sample is instantly stale
    State state;
    MarketDataFeed feed(cfg, state);
    const std::string pair = "XCH/wUSDC.b";

    feed.ingest_block_height(100);
    feed.ingest_cex_reference(pair, 1.40);     // will expire immediately
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // The market has genuinely moved ~40% -- far outside the tight 20%
    // near-reference test that the stale 1.40 would have imposed.
    const std::vector<CompetingOffer> moved{
        mk_offer("b1", Side::Bid, 1.96, 5'000'000'000'000LL),
        mk_offer("a1", Side::Ask, 2.00, 5'000'000'000'000LL),
    };
    feed.ingest_competing_offers(pair, moved, {}, kMojosPerXch, 1'000);

    const auto bbo = feed.get_dex_bbo(pair);
    EXPECT_GT(bbo.first, 0.0)
        << "honest bid discarded against an expired CEX price";
    EXPECT_GT(bbo.second, 0.0)
        << "honest ask discarded against an expired CEX price";
}

// [S20 2026-08-24] A truly anchorless pair must be able to RECOVER from a
// genuine large move.
//
// gate_mid promises that a fresh two-sided book can confirm a RejectStep,
// but two earlier fixes combined to make that unreachable: the anchorless
// offer filter used the tight 20% near test against last_accepted_mid --
// necessarily stripping every offer after a move past the 50% step limit
// -- and the screening flag required an independent ANCHOR, which an
// anchorless pair by definition lacks.  The pair stayed no-mid forever.
TEST(MidGateIngestTest, AnchorlessPairRecoversFromALargeGenuineMove) {
    auto cfg = gate_cfg();
    cfg.orderbook_mid_enabled = true;
    State state;
    MarketDataFeed feed(cfg, state);
    // No peg, no CEX, no AMM, no fair value, no sibling triangle.
    const std::string pair = "XCH/DBX";

    feed.ingest_block_height(100);

    // Establish an accepted mid near 100.
    const std::vector<CompetingOffer> before{
        mk_offer("b1", Side::Bid, 99.0, 5'000'000'000'000LL),
        mk_offer("a1", Side::Ask, 101.0, 5'000'000'000'000LL),
    };
    feed.ingest_competing_offers(pair, before, {}, kMojosPerXch, 1'000);
    feed.refresh({pair});
    ASSERT_NEAR(feed.get_mid_price(pair), 100.0, 2.0);

    // The market genuinely repriced ~120% -- past the 50% step limit --
    // and says so with a coherent two-sided book.
    const std::vector<CompetingOffer> after{
        mk_offer("b2", Side::Bid, 219.0, 5'000'000'000'000LL),
        mk_offer("a2", Side::Ask, 221.0, 5'000'000'000'000LL),
    };
    feed.ingest_competing_offers(pair, after, {}, kMojosPerXch, 1'000);

    const auto bbo = feed.get_dex_bbo(pair);
    EXPECT_GT(bbo.first, 0.0)  << "honest bid stripped by the near filter";
    EXPECT_GT(bbo.second, 0.0) << "honest ask stripped by the near filter";

    feed.refresh({pair});
    EXPECT_NEAR(feed.get_mid_price(pair), 220.0, 5.0)
        << "anchorless pair could not recover from a real move -- the "
           "book-confirmation escape gate_mid advertises is unreachable";
}

// [S20 2026-08-24] A junk cycle-1 publication must not launder itself into
// valuation evidence on cycle 2.
//
// On an anchorless pair, cycle 1 has no reference at all, so a coherent
// junk book publishes and becomes last_accepted_mid.  If the screening
// flag were collapsed into one, cycle 2 would find that junk value
// screening the unchanged book, promote it to valuation grade, and let it
// mark equity -- the self-referential lock-in this whole change exists to
// break, re-entering through the back door.  Confirming a step rejection
// may lean on our own history; marking equity may not.
TEST(MidGateIngestTest, SelfReferenceNeverBecomesValuationGrade) {
    State state;
    MarketDataFeed feed(gate_cfg(), state);
    const std::string pair = "XCH/DBX";   // anchorless by construction

    feed.ingest_block_height(100);

    const std::vector<CompetingOffer> junk{
        mk_offer("b1", Side::Bid, 187.0, 5'000'000'000'000LL),
        mk_offer("a1", Side::Ask, 188.0, 5'000'000'000'000LL),
    };

    // Cycle 1: nothing to screen against.
    feed.ingest_competing_offers(pair, junk, {}, kMojosPerXch, 1'000);
    feed.refresh({pair});
    EXPECT_FALSE(feed.mid_valuation_grade(pair))
        << "unscreened cycle-1 book marked equity";

    // Cycle 2: the same book, now 'screened' against cycle 1's own value.
    feed.ingest_competing_offers(pair, junk, {}, kMojosPerXch, 1'000);
    feed.refresh({pair});
    EXPECT_FALSE(feed.mid_valuation_grade(pair))
        << "a pair's own prior publication was laundered into valuation "
           "grade -- self-referential lock-in reintroduced";
    EXPECT_FALSE(feed.book_evidence_fresh(pair))
        << "self-screened book must not qualify as a triangle leg either";
}
