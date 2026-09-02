// =============================================================================
//  test_competitiveness_gate.cpp -- the Step 8 gate base, pinned.
// =============================================================================
//
//  [2026-09-02, review] WHAT THIS REPLACES
//  ---------------------------------------
//  The Step 12 metrics export carried the comment
//
//      "The Step 8 gate's base score, mirrored from the site that applies
//       it.  Kept in sync by the assertion in test_strategy_metrics.cpp,
//       not by hope."
//
//  test_strategy_metrics.cpp did not exist. `grep -rn test_strategy_metrics
//  cpp/` returned exactly one hit: the comment itself. The base score was
//  genuinely duplicated -- `(is_stablecoin) ? 1 : 3` written out at both the
//  applying site and the publishing site -- and no test related them.
//
//  The mirror has been removed rather than tested: both sites now call
//  xop::strategy::base_competitiveness_score(). This file pins the values
//  that function returns, so a retune of the gate has to come through here.
//
//  MUTATION CHECK (run before this file was accepted): flipping the header's
//  non-stablecoin base from 3 to 4 fails the static_assert in the header at
//  COMPILE time -- the build stops, so the mutation can never reach a green
//  suite. Flipping only the runtime test expectations below (leaving the
//  header alone) fails these tests. Both directions are covered.
//
// =============================================================================

#include <gtest/gtest.h>

#include "xop/strategy/competitiveness_gate.hpp"

using xop::strategy::base_competitiveness_score;
using xop::strategy::effective_competitiveness_gate;

// -----------------------------------------------------------------------
//  The base score.
// -----------------------------------------------------------------------

TEST(CompetitivenessGate, NonStablecoinBaseIsThree) {
    EXPECT_EQ(base_competitiveness_score(false), 3);
}

// Stablecoin pairs trade inside a sub-1% range and rarely score 3+, which
// suppressed 100% of BYC tiers in the 0.7.45 audit (151 sanity rejections).
TEST(CompetitivenessGate, StablecoinBaseIsOne) {
    EXPECT_EQ(base_competitiveness_score(true), 1);
}

// -----------------------------------------------------------------------
//  The effective gate: base + PID offset, clamped to 0..10.
// -----------------------------------------------------------------------

// The neutral case. This is the state the GUI could not previously render
// (offset 0 lost its sign separator and printed "30 -> 3"), so it is worth
// having a named test that it is a legal, common, identity-shaped input.
TEST(CompetitivenessGate, ZeroOffsetLeavesTheBaseUntouched) {
    EXPECT_EQ(effective_competitiveness_gate(false, 0), 3);
    EXPECT_EQ(effective_competitiveness_gate(true, 0), 1);
}

// Negative offset = underfilling = open the gate so more tiers post.
// -3 is the value the live controller has been railed at across the whole
// retained log, and it drives the non-stablecoin gate fully open.
TEST(CompetitivenessGate, NegativeOffsetOpensTheGate) {
    EXPECT_EQ(effective_competitiveness_gate(false, -1), 2);
    EXPECT_EQ(effective_competitiveness_gate(false, -3), 0);
    EXPECT_EQ(effective_competitiveness_gate(true, -1), 0);
}

TEST(CompetitivenessGate, PositiveOffsetRaisesTheGate) {
    EXPECT_EQ(effective_competitiveness_gate(false, 2), 5);
    EXPECT_EQ(effective_competitiveness_gate(true, 4), 5);
}

// The clamp is part of the contract: Step 8 compares a 0-10 score against
// this, so an unclamped value would either suppress everything or open the
// gate to scores that cannot exist.
TEST(CompetitivenessGate, ClampsIntoTheLegalRange) {
    EXPECT_EQ(effective_competitiveness_gate(false, -100), 0);
    EXPECT_EQ(effective_competitiveness_gate(true, -100), 0);
    EXPECT_EQ(effective_competitiveness_gate(false, 100), 10);
    EXPECT_EQ(effective_competitiveness_gate(true, 100), 10);

    // Exactly at the rails, from both directions.
    EXPECT_EQ(effective_competitiveness_gate(false, -3), 0);
    EXPECT_EQ(effective_competitiveness_gate(false, 7), 10);
    EXPECT_EQ(effective_competitiveness_gate(false, 8), 10);
}

// The whole point of the header: one definition, two callers, and the
// relationship between them is arithmetic rather than a copied literal.
TEST(CompetitivenessGate, EffectiveIsAlwaysDerivedFromTheSameBase) {
    for (bool stable : {false, true}) {
        for (int offset = -15; offset <= 15; ++offset) {
            const int base = base_competitiveness_score(stable);
            int expect = base + offset;
            if (expect < 0) expect = 0;
            if (expect > 10) expect = 10;
            EXPECT_EQ(effective_competitiveness_gate(stable, offset), expect)
                << "stable=" << stable << " offset=" << offset;
        }
    }
}
