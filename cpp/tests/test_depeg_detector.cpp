// ---------------------------------------------------------------------------
// The PAIR-level depeg detector's streak accounting.
//
// [review round 8] This detector had NO tests at all -- test_peg_registry.cpp
// merely cites its threshold semantics in comments. It is the sibling of the
// asset-level observe_peg() in risk/peg_suspension.hpp, and the two disagreed
// about the one case that matters most: what a reading you cannot interpret
// does to an in-progress streak.
//
//   observe_peg  : guards finiteness FIRST and returns without writing, so
//                  an unreadable price HOLDS the streak.
//   update()     : let a non-finite deviation fall through both comparisons
//                  into the final `else`, which zeroed blocks_above_bail AND
//                  block_entered_warn and returned Normal -- a blind read
//                  published as a clean bill of health.
//
// That is the fail-open shape of the documented close_out family, arriving
// through a detector whose whole job is to notice bad news.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/strategy/depeg_detector.hpp"

#include <cmath>
#include <limits>

using xop::DepegConfig;
using xop::DepegDetector;
using xop::DepegStatus;
using xop::PairConfig;

namespace {

constexpr const char* kPair = "BYC/wUSDC.b";

// wUSDC.b-shaped: warn 2%, bail 10%, 3 sustained blocks (shortened from the
// shipped 30 so the streak arithmetic stays readable).
PairConfig stable_pair()
{
    PairConfig pc;
    pc.name                   = kPair;
    pc.is_stablecoin          = true;
    pc.peg_target             = 1.0;
    pc.depeg_warn_pct         = 2.0;
    pc.depeg_bail_pct         = 10.0;
    pc.depeg_sustained_blocks = 3;
    return pc;
}

DepegDetector make_detector(const DepegConfig& cfg)
{
    DepegDetector d(cfg);
    d.register_pair(stable_pair());
    return d;
}

}  // namespace

TEST(DepegDetector, ASustainedDepegBailsAndOneWickDoesNot)
{
    // The baseline the guard below must not disturb.
    const DepegConfig cfg;
    auto d = make_detector(cfg);

    EXPECT_EQ(d.update(kPair, 1.00, 1u), DepegStatus::Normal);
    EXPECT_EQ(d.update(kPair, 0.97, 2u), DepegStatus::Warning);
    EXPECT_FALSE(d.should_bail(kPair));

    // One deep print, then a recovery: the streak resets, no bail.
    EXPECT_EQ(d.update(kPair, 0.50, 3u), DepegStatus::Warning);
    EXPECT_EQ(d.update(kPair, 1.00, 4u), DepegStatus::Normal);
    EXPECT_FALSE(d.should_bail(kPair));

    // Three consecutive past bail latches.
    EXPECT_EQ(d.update(kPair, 0.50, 5u), DepegStatus::Warning);
    EXPECT_EQ(d.update(kPair, 0.50, 6u), DepegStatus::Warning);
    EXPECT_EQ(d.update(kPair, 0.50, 7u), DepegStatus::Bailed);
    EXPECT_TRUE(d.should_bail(kPair));
}

TEST(DepegDetector, ANonFinitePriceHoldsTheStreakInsteadOfClearingIt)
{
    // ***THE REGRESSION.*** Two observations past bail, one short of
    // latching, then an unreadable print arrives.
    const DepegConfig cfg;
    auto d = make_detector(cfg);

    EXPECT_EQ(d.update(kPair, 0.50, 1u), DepegStatus::Warning);
    EXPECT_EQ(d.update(kPair, 0.50, 2u), DepegStatus::Warning);

    for (const double blind : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
        // Not Normal: an unreadable price is not an all-clear.
        EXPECT_EQ(d.update(kPair, blind, 3u), DepegStatus::Warning)
            << "a blind read must report the state it already had, not "
               "manufacture a clean bill of health";
        EXPECT_FALSE(d.should_bail(kPair));
    }

    // The streak survived: ONE more genuine observation completes it. With
    // the guard removed, each blind read above zeroes blocks_above_bail and
    // this line reports Warning forever.
    EXPECT_EQ(d.update(kPair, 0.50, 4u), DepegStatus::Bailed);
    EXPECT_TRUE(d.should_bail(kPair));
}

TEST(DepegDetector, ANonPositivePriceIsAlsoADataGapRatherThanA100PctDepeg)
{
    // A zero or negative mid is not "the coin is worth nothing"; it is the
    // absence of a price. Reading it as a 100%-off deviation would ADVANCE
    // the bail streak on an outage and pull quotes on a feed failure -- the
    // mirror-image fail-open, and the reason the engine's own call site
    // guards `mid <= 0.0` before calling here. The detector no longer
    // depends on that upstream invariant holding.
    const DepegConfig cfg;
    auto d = make_detector(cfg);

    EXPECT_EQ(d.update(kPair, 0.50, 1u), DepegStatus::Warning);
    EXPECT_EQ(d.update(kPair, 0.0, 2u), DepegStatus::Warning);
    EXPECT_EQ(d.update(kPair, -1.0, 3u), DepegStatus::Warning);
    EXPECT_FALSE(d.should_bail(kPair))
        << "three 'observations' of which two were data gaps must not bail";

    // And the held streak still completes on real data.
    EXPECT_EQ(d.update(kPair, 0.50, 4u), DepegStatus::Warning);
    EXPECT_EQ(d.update(kPair, 0.50, 5u), DepegStatus::Bailed);
}

TEST(DepegDetector, ABlindReadDoesNotDisturbAHealthyPairEither)
{
    // The symmetric case: nothing in progress, so holding means staying
    // Normal rather than inventing a warning.
    const DepegConfig cfg;
    auto d = make_detector(cfg);

    EXPECT_EQ(d.update(kPair, 1.00, 1u), DepegStatus::Normal);
    EXPECT_EQ(d.update(kPair, std::numeric_limits<double>::quiet_NaN(), 2u),
              DepegStatus::Normal);
    EXPECT_FALSE(d.should_bail(kPair));
}

TEST(DepegDetector, TheMasterSwitchAndUnregisteredPairsStayNormal)
{
    DepegConfig off;
    off.enabled = false;
    auto disabled = make_detector(off);
    EXPECT_EQ(disabled.update(kPair, 0.10, 1u), DepegStatus::Normal);
    EXPECT_FALSE(disabled.should_bail(kPair));

    const DepegConfig cfg;
    auto d = make_detector(cfg);
    EXPECT_EQ(d.update("XCH/DBX", 0.10, 1u), DepegStatus::Normal)
        << "a pair that was never registered is not monitored";
}
