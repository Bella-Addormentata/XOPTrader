// [ALERT-NOISE] The hedge-efficiency window and the drawdown latch.
//
// Two alerts fired on nearly every cycle: Natural Hedge Efficiency, worked out
// from one block's fills (one fill always scored 0), and the equity drawdown
// warning, sent on every cycle above 5%.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "xop/database.hpp"
#include "xop/monitoring/alerts.hpp"
#include "xop/risk/hedging.hpp"

using xop::DrawdownAlertLatch;
using xop::NheAlertLatch;
using xop::NheWindow;
using xop::prune_and_rearm;

namespace {

// A moment for the drawdown latch, `minutes` after an arbitrary start.
DrawdownAlertLatch::Clock::time_point at_minute(int minutes)
{
    return DrawdownAlertLatch::Clock::time_point{} + std::chrono::hours(1000)
         + std::chrono::minutes(minutes);
}

}  // namespace

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

TEST(NheWindow, ReadsBackJournalledMakerFills)
{
    // [review #251] Rows as Database::query_pace_fills returns them.
    std::vector<xop::DbPaceFillRow> rows(4);
    rows[0].side_lower   = "bid";
    rows[0].size_mojos   = 5;
    rows[0].block_height = 10;
    rows[1].side_lower   = "ask";
    rows[1].size_mojos   = 3;
    rows[1].block_height = 11;
    rows[2].is_taker         = true;  // a taker fill: not Step 2's
    rows[2].we_bought_base   = true;
    rows[2].base_delta_mojos = 7;
    rows[2].block_height     = 12;
    rows[3].size_mojos   = 9;  // no side
    rows[3].block_height = 12;

    NheWindow window(100);
    EXPECT_EQ(window.seed(rows), 2U);
    EXPECT_EQ(window.fills(), 2U);
    EXPECT_DOUBLE_EQ(window.net(), 2.0);
    EXPECT_DOUBLE_EQ(window.volume(), 8.0);
}

TEST(NheWindow, ARestartReadsTheLastWindowBackFromTheTradeLog)
{
    // [review #251] Five one-sided fills before a restart and one after it
    // are six fills in the window, not one.
    const auto path = std::filesystem::temp_directory_path()
        / ("xop_nhe_read_back_"
           + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count())
           + ".sqlite");
    {
        xop::Database db(path.string());
        auto journal = [&](const std::string& id, const std::string& pair,
                           const std::string& side, xop::BlockHeight block) {
            xop::DbTradeRecord tr;
            tr.timestamp    = "2026-11-21T12:00:00Z";
            tr.trade_id     = id;
            tr.pair_name    = pair;
            tr.side         = side;
            tr.price_mojos  = 1500000000000LL;
            tr.size_mojos   = 1000000000000LL;
            tr.block_height = block;
            db.insert_trade(tr);
        };
        journal("old", "XCH/BYC", "ask", 300);  // older than the window
        for (int i = 0; i < 5; ++i) {
            journal("t" + std::to_string(i), "XCH/BYC", "bid",
                    static_cast<xop::BlockHeight>(1000 + i));
        }
        journal("other", "XCH/wUSDC.b", "bid", 1002);  // another pair

        const xop::BlockHeight now = 1500;
        NheWindow window;
        EXPECT_EQ(window.seed(db.query_pace_fills("XCH/BYC", now - NheWindow::kDefaultBlocks)), 5U);
        window.add(now, 1e12);  // the first fill after the restart
        window.prune(now);
        EXPECT_EQ(window.fills(), 6U);
        NheAlertLatch latch;
        EXPECT_TRUE(latch.should_alert(window.nhe(), window.fills(), now));
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}

TEST(NheWindow, FillsAboveThePruneHeightAreKeptButNotCounted)
{
    // [review #251] Read back from trade_log while the node is still behind
    // the heights it had journalled before the restart.
    NheWindow window(100);
    window.add(40, 5.0);
    window.add(60, 3.0);
    window.prune(50);
    EXPECT_EQ(window.fills(), 1U) << "the fill from block 60 is not reached yet";
    EXPECT_DOUBLE_EQ(window.net(), 5.0);
    EXPECT_DOUBLE_EQ(window.volume(), 5.0);
    window.prune(60);
    EXPECT_EQ(window.fills(), 2U);
    EXPECT_DOUBLE_EQ(window.net(), 8.0);
}

TEST(NheAlertLatch, AnOldRunExpiringAsANewOneStartsDoesNotHideIt)
{
    // [review #251] Six fills at block 1000 first alert at 1100, after a
    // restart. At 1577 they expire just as six new ones arrive: pruned before
    // the new fills go in, the window shows empty and the latch re-arms.
    NheWindow window;
    NheAlertLatch latch;
    for (int i = 0; i < 6; ++i) window.add(1000, 1.0);
    prune_and_rearm(window, latch, 1100);
    EXPECT_TRUE(latch.should_alert(window.nhe(), window.fills(), 1100));

    prune_and_rearm(window, latch, 1577);  // before Step 2 adds
    EXPECT_EQ(window.fills(), 0U);
    for (int i = 0; i < 6; ++i) window.add(1577, 1.0);
    prune_and_rearm(window, latch, 1577);  // Step 10
    EXPECT_TRUE(latch.should_alert(window.nhe(), window.fills(), 1577))
        << "not held back to 1677 by the old run's reminder";
}

TEST(NheAlertLatch, RecoveryRearmsEvenWithFewFills)
{
    // [review #251] Two balanced fills left after the one-sided ones expired.
    NheAlertLatch latch;
    EXPECT_TRUE(latch.should_alert(0.1, NheWindow::kMinFills, 100));
    EXPECT_FALSE(latch.should_alert(1.0, 2, 101)) << "recovered, below the fill count";
    EXPECT_TRUE(latch.should_alert(0.2, NheWindow::kMinFills, 102)) << "so a new run alerts";
    latch.reset();  // the pair went quiet
    EXPECT_TRUE(latch.should_alert(0.2, NheWindow::kMinFills, 103));
}

TEST(NheAlertLatch, OncePerWindowAndOnlyWithEnoughFills)
{
    NheAlertLatch latch;
    EXPECT_FALSE(latch.should_alert(0.0, 1, 100)) << "a single fill says nothing";
    EXPECT_FALSE(latch.should_alert(0.2, NheWindow::kMinFills - 1, 101));
    EXPECT_TRUE(latch.should_alert(0.2, NheWindow::kMinFills, 102));
    EXPECT_FALSE(latch.should_alert(0.1, NheWindow::kMinFills + 3, 103)) << "the same window";
    EXPECT_FALSE(latch.should_alert(0.9, NheWindow::kMinFills, 104)) << "back above the target";
    EXPECT_TRUE(latch.should_alert(0.3, NheWindow::kMinFills, 105)) << "a new episode";
}

TEST(NheAlertLatch, AStandingRunIsSentAgainOnceItsWindowHasPassed)
{
    // [review #251] Each message is about fills the one before did not count,
    // and one lost on the way is sent again.
    NheAlertLatch latch;
    const xop::BlockHeight blocks = NheWindow::kDefaultBlocks;
    EXPECT_TRUE(latch.should_alert(0.2, NheWindow::kMinFills, 100));
    EXPECT_FALSE(latch.should_alert(0.2, NheWindow::kMinFills, 100 + blocks))
        << "a fill from block 100 is still in the window";
    EXPECT_TRUE(latch.should_alert(0.2, NheWindow::kMinFills, 101 + blocks));
    EXPECT_FALSE(latch.should_alert(0.2, NheWindow::kMinFills, 102 + blocks));
}

TEST(NheAlertLatch, AWindowThatExpiredBetweenHeartbeatsRearms)
{
    // [review #251] Latched at 100. The next heartbeat is at 700, and Step 2
    // adds buys from 686-691 before the prune, so the window never shows empty.
    NheWindow window;
    NheAlertLatch latch;
    for (xop::BlockHeight b = 95; b <= 100; ++b) window.add(b, 1.0);
    window.prune(100);
    EXPECT_TRUE(latch.should_alert(window.nhe(), window.fills(), 100));
    for (xop::BlockHeight b = 686; b <= 691; ++b) window.add(b, 1.0);
    window.prune(700);
    ASSERT_EQ(window.fills(), 6U) << "only the new run is left";
    EXPECT_TRUE(latch.should_alert(window.nhe(), window.fills(), 700));
}

TEST(DrawdownAlertLatch, OncePerStepAndAgainInANewEpisode)
{
    DrawdownAlertLatch latch;
    const double threshold = 0.05;
    const auto t = at_minute(0);
    EXPECT_FALSE(latch.should_alert(0.04, threshold, t));
    EXPECT_TRUE(latch.should_alert(0.06, threshold, t));
    for (double d : {0.062, 0.083, 0.071, 0.099}) {
        EXPECT_FALSE(latch.should_alert(d, threshold, t)) << d << ": the same 5% step";
    }
    EXPECT_TRUE(latch.should_alert(0.101, threshold, t)) << "a deeper step";
    EXPECT_FALSE(latch.should_alert(0.045, threshold, t)) << "eased, but not below 80% of 5%";
    EXPECT_FALSE(latch.should_alert(0.06, threshold, t)) << "so not yet a new episode";
    EXPECT_FALSE(latch.should_alert(0.039, threshold, t)) << "below 4%: the episode is over";
    EXPECT_TRUE(latch.should_alert(0.055, threshold, t)) << "and the next one alerts again";
}

TEST(DrawdownAlertLatch, AStandingDrawdownIsSentAgainEverySixHours)
{
    // [review #251] So a message lost on the way is not lost for the episode.
    DrawdownAlertLatch latch;
    const double threshold = 0.05;
    EXPECT_TRUE(latch.should_alert(0.11, threshold, at_minute(0)));
    EXPECT_FALSE(latch.should_alert(0.11, threshold, at_minute(359)));
    EXPECT_TRUE(latch.should_alert(0.07, threshold, at_minute(360))) << "six hours on, past 5%";
    EXPECT_EQ(latch.alerted_step(), 1);
    EXPECT_TRUE(latch.should_alert(0.105, threshold, at_minute(361)))
        << "deeper than the last message said";
    EXPECT_FALSE(latch.should_alert(0.045, threshold, at_minute(2000)))
        << "eased below 5%: nothing to remind of";
    EXPECT_TRUE(latch.should_alert(0.06, threshold, at_minute(2001)))
        << "past 5% again, hours after the last message";
}

TEST(DrawdownAlertLatch, ResetSendsAStandingDrawdownAgain)
{
    // [review #251] Telegram was just switched on: the last message never
    // reached it.
    DrawdownAlertLatch latch;
    EXPECT_TRUE(latch.should_alert(0.06, 0.05, at_minute(0)));
    EXPECT_FALSE(latch.should_alert(0.06, 0.05, at_minute(1)));
    latch.reset();
    EXPECT_TRUE(latch.should_alert(0.06, 0.05, at_minute(2)));
}

TEST(DrawdownAlertLatch, NothingWithoutAThresholdOrANumber)
{
    DrawdownAlertLatch latch;
    const auto t = at_minute(0);
    EXPECT_FALSE(latch.should_alert(0.5, 0.0, t));
    EXPECT_FALSE(latch.should_alert(std::numeric_limits<double>::quiet_NaN(), 0.05, t));
    EXPECT_FALSE(latch.should_alert(std::numeric_limits<double>::infinity(), 0.05, t));
    EXPECT_FALSE(latch.should_alert(0.5, std::numeric_limits<double>::infinity(), t));
}

TEST(DrawdownAlertLatch, StepsAreExactAtTheirBoundariesAndBounded)
{
    // [review #251] 0.15 / 0.05 reads 2.9999999999999996; 0.04 / 0.05 reads
    // just under 0.8 -- neither may decide a step or the end of an episode.
    DrawdownAlertLatch latch;
    const auto t = at_minute(0);
    EXPECT_TRUE(latch.should_alert(0.05, 0.05, t));
    EXPECT_TRUE(latch.should_alert(0.10, 0.05, t));
    EXPECT_TRUE(latch.should_alert(0.15, 0.05, t)) << "exactly 15% is the third step";
    EXPECT_EQ(latch.alerted_step(), 3);
    EXPECT_FALSE(latch.should_alert(0.04, 0.05, t)) << "exactly 4% does not end the episode";
    EXPECT_EQ(latch.alerted_step(), 3);
    EXPECT_FALSE(latch.should_alert(0.0399, 0.05, t));
    EXPECT_EQ(latch.alerted_step(), 0);

    DrawdownAlertLatch tiny;
    EXPECT_TRUE(tiny.should_alert(0.5, 1e-12, t)) << "bounded, not overflowed";
    EXPECT_EQ(tiny.alerted_step(), DrawdownAlertLatch::kMaxStep);
    EXPECT_FALSE(tiny.should_alert(0.6, 1e-12, t));
}

TEST(AlertManager, SwitchingTelegramOnOrToAnotherChatReArmsTheLatches)
{
    // [review #251] No init(): no worker, nothing is sent.
    xop::AlertManager alerts;
    EXPECT_EQ(alerts.delivery_epoch(), 0U);
    alerts.set_credentials("token", "chat");
    EXPECT_EQ(alerts.delivery_epoch(), 1U);
    alerts.set_credentials("token", "chat");
    EXPECT_EQ(alerts.delivery_epoch(), 1U) << "nothing changed";
    alerts.set_credentials("", "");
    EXPECT_EQ(alerts.delivery_epoch(), 1U) << "switched off: nowhere to deliver";
    alerts.set_credentials("token", "other-chat");
    EXPECT_EQ(alerts.delivery_epoch(), 2U);
}

TEST(AlertRules, HedgeImbalanceIsAWarningOfItsOwn)
{
    EXPECT_EQ(xop::tier_for_rule(xop::AlertRule::HedgeImbalance), xop::AlertTier::WARNING);
    EXPECT_STREQ(xop::to_string(xop::AlertRule::HedgeImbalance), "HedgeImbalance");
}

TEST(AlertRules, TheSelfPacedRulesSkipTheCooldown)
{
    // [review #251] A latch has already decided these are to be sent.
    EXPECT_TRUE(xop::alert_rule_skips_cooldown(xop::AlertRule::PnlDrawdown));
    EXPECT_TRUE(xop::alert_rule_skips_cooldown(xop::AlertRule::HedgeImbalance));
    EXPECT_TRUE(xop::alert_rule_skips_cooldown(xop::AlertRule::ConfigReload));
    EXPECT_FALSE(xop::alert_rule_skips_cooldown(xop::AlertRule::SpreadWidening));
    EXPECT_FALSE(xop::alert_rule_skips_cooldown(xop::AlertRule::ExposureBreach));
}
