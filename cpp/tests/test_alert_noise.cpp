// [ALERT-NOISE] The hedge-efficiency window and the drawdown latch.
//
// Two alerts fired on nearly every cycle: Natural Hedge Efficiency, worked out
// from one block's fills (one fill always scored 0), and the equity drawdown
// warning, sent on every cycle above 5%.

#include <gtest/gtest.h>

#include <limits>

#include "xop/monitoring/alerts.hpp"
#include "xop/risk/hedging.hpp"

using xop::DrawdownAlertLatch;
using xop::NheAlertLatch;
using xop::NheWindow;

TEST(NheWindow, OneSidedFillsScoreLowAndBalancedOnesHigh)
{
    NheWindow window(100);
    window.add(10, 5.0);
    EXPECT_EQ(window.fills(), 1U);
    EXPECT_DOUBLE_EQ(window.nhe(), 0.0) << "one fill is one-sided by nature";
    window.add(11, -5.0);
    EXPECT_DOUBLE_EQ(window.nhe(), 1.0) << "a buy and a sale of the same size net out";
    window.add(12, 10.0);
    EXPECT_DOUBLE_EQ(window.net(), 10.0);
    EXPECT_DOUBLE_EQ(window.volume(), 20.0);
    EXPECT_DOUBLE_EQ(window.nhe(), 0.5);
}

TEST(NheWindow, FillsOlderThanTheWindowAreForgotten)
{
    NheWindow window(100);
    window.add(10, 5.0);
    window.add(60, -5.0);
    window.prune(110);
    EXPECT_EQ(window.fills(), 2U) << "100 blocks back is still inside";
    window.prune(111);
    EXPECT_EQ(window.fills(), 1U);
    window.prune(500);
    EXPECT_EQ(window.fills(), 0U);
    EXPECT_DOUBLE_EQ(window.nhe(), 1.0) << "no volume: nothing one-sided";
}

TEST(NheWindow, FillsOutOfHeightOrderAreForgottenToo)
{
    // [review #251] A newer fill recorded first must not shield an older one.
    NheWindow window(100);
    window.add(200, 5.0);
    window.add(50, -5.0);
    window.add(190, 2.0);
    window.prune(201);
    EXPECT_EQ(window.fills(), 2U) << "the fill from block 50 is out of the window";
    EXPECT_DOUBLE_EQ(window.net(), 7.0);
}

TEST(NheAlertLatch, RecoveryRearmsEvenWithFewFills)
{
    // [review #251] Two balanced fills left after the one-sided ones expired.
    NheAlertLatch latch;
    EXPECT_TRUE(latch.should_alert(0.1, NheWindow::kMinFills));
    EXPECT_FALSE(latch.should_alert(1.0, 2)) << "recovered, below the fill count";
    EXPECT_TRUE(latch.should_alert(0.2, NheWindow::kMinFills)) << "so a new run alerts";
    latch.reset();  // the pair went quiet
    EXPECT_TRUE(latch.should_alert(0.2, NheWindow::kMinFills));
}

TEST(NheAlertLatch, OncePerEpisodeAndOnlyWithEnoughFills)
{
    NheAlertLatch latch;
    EXPECT_FALSE(latch.should_alert(0.0, 1)) << "a single fill says nothing";
    EXPECT_FALSE(latch.should_alert(0.2, NheWindow::kMinFills - 1));
    EXPECT_TRUE(latch.should_alert(0.2, NheWindow::kMinFills));
    EXPECT_FALSE(latch.should_alert(0.1, NheWindow::kMinFills + 3)) << "still the same episode";
    EXPECT_FALSE(latch.should_alert(0.9, NheWindow::kMinFills)) << "back above the target";
    EXPECT_TRUE(latch.should_alert(0.3, NheWindow::kMinFills)) << "a new episode";
}

TEST(DrawdownAlertLatch, OncePerStepAndAgainInANewEpisode)
{
    DrawdownAlertLatch latch;
    const double threshold = 0.05;
    EXPECT_FALSE(latch.should_alert(0.04, threshold));
    EXPECT_TRUE(latch.should_alert(0.06, threshold));
    for (double d : {0.062, 0.083, 0.071, 0.099}) {
        EXPECT_FALSE(latch.should_alert(d, threshold)) << d << ": the same 5% step";
    }
    EXPECT_TRUE(latch.should_alert(0.101, threshold)) << "a deeper step";
    EXPECT_FALSE(latch.should_alert(0.045, threshold)) << "eased, but not below 80% of 5%";
    EXPECT_FALSE(latch.should_alert(0.06, threshold)) << "so not yet a new episode";
    EXPECT_FALSE(latch.should_alert(0.039, threshold)) << "below 4%: the episode is over";
    EXPECT_TRUE(latch.should_alert(0.055, threshold)) << "and the next one alerts again";
}

TEST(DrawdownAlertLatch, NothingWithoutAThresholdOrANumber)
{
    DrawdownAlertLatch latch;
    EXPECT_FALSE(latch.should_alert(0.5, 0.0));
    EXPECT_FALSE(latch.should_alert(std::numeric_limits<double>::quiet_NaN(), 0.05));
    EXPECT_FALSE(latch.should_alert(std::numeric_limits<double>::infinity(), 0.05));
    EXPECT_FALSE(latch.should_alert(0.5, std::numeric_limits<double>::infinity()));
}

TEST(DrawdownAlertLatch, StepsAreExactAtTheirBoundariesAndBounded)
{
    // [review #251] 0.15 / 0.05 reads 2.9999999999999996; 0.04 / 0.05 reads
    // just under 0.8 -- neither may decide a step or the end of an episode.
    DrawdownAlertLatch latch;
    EXPECT_TRUE(latch.should_alert(0.05, 0.05));
    EXPECT_TRUE(latch.should_alert(0.10, 0.05));
    EXPECT_TRUE(latch.should_alert(0.15, 0.05)) << "exactly 15% is the third step";
    EXPECT_EQ(latch.alerted_step(), 3);
    EXPECT_FALSE(latch.should_alert(0.04, 0.05)) << "exactly 4% does not end the episode";
    EXPECT_EQ(latch.alerted_step(), 3);
    EXPECT_FALSE(latch.should_alert(0.0399, 0.05));
    EXPECT_EQ(latch.alerted_step(), 0);

    DrawdownAlertLatch tiny;
    EXPECT_TRUE(tiny.should_alert(0.5, 1e-12)) << "bounded, not overflowed";
    EXPECT_EQ(tiny.alerted_step(), DrawdownAlertLatch::kMaxStep);
    EXPECT_FALSE(tiny.should_alert(0.6, 1e-12));
}

TEST(AlertRules, HedgeImbalanceIsAWarningOfItsOwn)
{
    EXPECT_EQ(xop::tier_for_rule(xop::AlertRule::HedgeImbalance), xop::AlertTier::WARNING);
    EXPECT_STREQ(xop::to_string(xop::AlertRule::HedgeImbalance), "HedgeImbalance");
}

TEST(AlertRules, TheSelfPacedRulesSkipTheCooldown)
{
    // [review #251] A latch has already decided these are news.
    EXPECT_TRUE(xop::alert_rule_skips_cooldown(xop::AlertRule::PnlDrawdown));
    EXPECT_TRUE(xop::alert_rule_skips_cooldown(xop::AlertRule::HedgeImbalance));
    EXPECT_TRUE(xop::alert_rule_skips_cooldown(xop::AlertRule::ConfigReload));
    EXPECT_FALSE(xop::alert_rule_skips_cooldown(xop::AlertRule::SpreadWidening));
    EXPECT_FALSE(xop::alert_rule_skips_cooldown(xop::AlertRule::ExposureBreach));
}
