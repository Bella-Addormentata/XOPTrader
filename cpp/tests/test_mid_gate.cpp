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

#include <algorithm>
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

// ---------------------------------------------------------------------------
// Median selection (engine-side triangulation combines candidates this way)
// ---------------------------------------------------------------------------

TEST(ImpliedCrossTest, EvenCandidateCountAveragesTheMiddlePair)
{
    // Mirrors Engine::compute_implied_cross_anchor's selection: with an
    // even count the upper-middle observation would let one high outlier
    // become the entire anchor.  n == 2 is the common case (two triangles
    // through different common assets), so this is not a corner.
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        const std::size_t n = v.size();
        return (n % 2 == 0) ? (v[n / 2 - 1] + v[n / 2]) / 2.0 : v[n / 2];
    };

    EXPECT_NEAR(median({1.00, 1.40}), 1.20, 1e-12);
    EXPECT_NEAR(median({1.01}), 1.01, 1e-12);
    EXPECT_NEAR(median({0.98, 1.00, 1.40}), 1.00, 1e-12);
}
