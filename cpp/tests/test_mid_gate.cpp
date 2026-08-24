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
    // legs.  XCH/BYC mid = BYC per XCH; XCH/wUSDC.b mid = USDC per XCH.
    // implied(BYC/wUSDC.b) = (1 / (BYC per XCH)) * (USDC per XCH).
    // With XCH at $1.42 and BYC honest at ~$1.01: XCH/BYC ~ 1.4059,
    // XCH/wUSDC.b ~ 1.42 -> implied ~ 1.0100.
    CrossLeg xch_byc{1.4059, 120.0, /*fresh=*/true, /*invert=*/true};
    CrossLeg xch_usdc{1.42, 80.0, /*fresh=*/true, /*invert=*/false};

    const double implied = implied_cross(xch_byc, xch_usdc, 300.0);
    EXPECT_NEAR(implied, 1.0100, 0.0005);
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
    in.book_spread_bps = 800.0;
    EXPECT_EQ(gate_mid(in), GateVerdict::Accept);

    // Same move on a stale book: no confirmation, rejected.
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

    // ...unless a fresh two-sided book confirms the move.
    in.book_two_sided  = true;
    in.book_fresh      = true;
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
