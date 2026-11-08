// test_ratio_mode.cpp -- [RATIO-MODE-PERSIST 2026-11-08] the ratio rebalance
// hysteresis (xop/strategy/ratio_mode.hpp) that Step 7 and Step 8 share, and
// the text form the engine saves each pair's mode under.

#include <gtest/gtest.h>

#include "xop/strategy/ratio_mode.hpp"

#include <string>
#include <unordered_map>

namespace {

using xop::strategy::ratiomode::Bands;
using xop::strategy::ratiomode::Mode;
using xop::strategy::ratiomode::bands_for;
using xop::strategy::ratiomode::mode_name;
using xop::strategy::ratiomode::next_mode;
using xop::strategy::ratiomode::parse_mode;

// The live XCH/wUSDC.b bands on 2026-11-07: target 0.714, a per-pair enter
// band of 0.49 and the global exit band 0.04.
Bands live_wusdc()
{
    const std::unordered_map<std::string, double> targets{{"XCH/wUSDC.b", 0.7142857142857143}};
    const std::unordered_map<std::string, double> bands{{"XCH/wUSDC.b", 0.49}};
    return bands_for("XCH/wUSDC.b", 0.5, 0.08, 0.04, targets, bands);
}

}  // namespace

TEST(RatioModeBands, APairsOwnTargetAndEnterBandWinAndCapTheExitBand) {
    const Bands b = live_wusdc();
    EXPECT_DOUBLE_EQ(b.target, 0.7142857142857143);
    EXPECT_DOUBLE_EQ(b.enter, 0.49);
    EXPECT_DOUBLE_EQ(b.exit, 0.04);

    // A per-pair enter band narrower than twice the global exit band caps
    // the exit band at half of it.
    const std::unordered_map<std::string, double> none;
    const std::unordered_map<std::string, double> narrow{{"P", 0.06}};
    const Bands n = bands_for("P", 0.5, 0.08, 0.04, none, narrow);
    EXPECT_DOUBLE_EQ(n.target, 0.5);
    EXPECT_DOUBLE_EQ(n.enter, 0.06);
    EXPECT_DOUBLE_EQ(n.exit, 0.03);

    // No per-pair entries: the globals, untouched.
    const Bands g = bands_for("P", 0.5, 0.08, 0.04, none, none);
    EXPECT_DOUBLE_EQ(g.target, 0.5);
    EXPECT_DOUBLE_EQ(g.enter, 0.08);
    EXPECT_DOUBLE_EQ(g.exit, 0.04);
}

TEST(RatioModeHysteresis, NeutralEntersOnlyPastTheEnterBand) {
    const Bands b = live_wusdc();  // enters AcquireBase at <= 0.224
    // The 2026-11-07 restart: ratio 0.39-0.44 is far below target but inside
    // the wide enter band, so a fresh Neutral stays Neutral.  This is why the
    // mode has to be saved rather than re-derived.
    EXPECT_EQ(next_mode(Mode::Neutral, 0.439, b), Mode::Neutral);
    EXPECT_EQ(next_mode(Mode::Neutral, 0.225, b), Mode::Neutral);
    EXPECT_EQ(next_mode(Mode::Neutral, 0.224, b), Mode::AcquireBase);

    const std::unordered_map<std::string, double> none;
    const Bands g = bands_for("P", 0.5, 0.08, 0.04, none, none);
    EXPECT_EQ(next_mode(Mode::Neutral, 0.57, g), Mode::Neutral);
    EXPECT_EQ(next_mode(Mode::Neutral, 0.58, g), Mode::AcquireQuote);
    EXPECT_EQ(next_mode(Mode::Neutral, 0.42, g), Mode::AcquireBase);
}

TEST(RatioModeHysteresis, AnActiveModeHoldsUntilTheRatioIsBackNearTarget) {
    const Bands b = live_wusdc();  // AcquireBase exits at >= 0.674
    EXPECT_EQ(next_mode(Mode::AcquireBase, 0.439, b), Mode::AcquireBase);
    EXPECT_EQ(next_mode(Mode::AcquireBase, 0.673, b), Mode::AcquireBase);
    EXPECT_EQ(next_mode(Mode::AcquireBase, 0.675, b), Mode::Neutral);
    EXPECT_EQ(next_mode(Mode::AcquireBase, 0.714, b), Mode::Neutral);

    const std::unordered_map<std::string, double> none;
    const Bands g = bands_for("P", 0.5, 0.08, 0.04, none, none);
    EXPECT_EQ(next_mode(Mode::AcquireQuote, 0.55, g), Mode::AcquireQuote);
    EXPECT_EQ(next_mode(Mode::AcquireQuote, 0.53, g), Mode::Neutral);
}

TEST(RatioModeHysteresis, AModeOvershotPastItsBandExitsInsteadOfPushingFurther) {
    const std::unordered_map<std::string, double> none;
    const Bands g = bands_for("P", 0.5, 0.08, 0.04, none, none);
    // Overweight base now (a big bid fill, or a target edited while the
    // engine was off): AcquireBase must not keep boosting bids.  The old
    // rule only exited inside +/- exit and held AcquireBase here.
    EXPECT_EQ(next_mode(Mode::AcquireBase, 0.56, g), Mode::Neutral);
    // Past the opposite enter band: straight to the opposite mode.
    EXPECT_EQ(next_mode(Mode::AcquireBase, 0.60, g), Mode::AcquireQuote);
    EXPECT_EQ(next_mode(Mode::AcquireQuote, 0.44, g), Mode::Neutral);
    EXPECT_EQ(next_mode(Mode::AcquireQuote, 0.40, g), Mode::AcquireBase);
}

TEST(RatioModeText, NamesRoundTripAndUnknownTextIsRejected) {
    for (Mode m : {Mode::Neutral, Mode::AcquireBase, Mode::AcquireQuote}) {
        const auto parsed = parse_mode(mode_name(m));
        ASSERT_TRUE(parsed.has_value()) << mode_name(m);
        EXPECT_EQ(*parsed, m);
    }
    EXPECT_FALSE(parse_mode("").has_value());
    EXPECT_FALSE(parse_mode("acquirebase").has_value());
    EXPECT_FALSE(parse_mode("Unknown").has_value());
}
