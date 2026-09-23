// test_wallet_sync_watch.cpp -- [WALLET-RESTART-LIVELOCK 2026-09-22]
//
// When may the engine restart an unsynced wallet
// (execution/wallet_sync_watch.hpp)?  The old rule, 20 unsynced heartbeats,
// restarted a SYNCING wallet every 6-10 minutes; a Chia long sync records
// progress only when it completes and every restart rolls the wallet back 256
// blocks, so the wallet could never finish.  These tests pin the replacement:
// a wallet that says it is syncing keeps its long budget, one that is not even
// trying gets the short one, each restart doubles both, and time is measured
// continuously or not at all.

#include <gtest/gtest.h>

#include <xop/execution/wallet_sync_watch.hpp>

#include <cstdint>
#include <limits>

namespace {

using xop::execution::observe_wallet_sync;
using xop::execution::record_failed_wallet_restart;
using xop::execution::wallet_restart_budget;
using xop::execution::WalletRestartPolicy;
using xop::execution::WalletSyncAction;
using xop::execution::WalletSyncVerdict;
using xop::execution::WalletSyncWatch;

constexpr bool kSynced   = true;
constexpr bool kUnsynced = false;
constexpr bool kSyncing  = true;
constexpr bool kIdle     = false;

WalletSyncVerdict see(WalletSyncWatch& watch, bool synced, bool syncing, std::int64_t t)
{
    return observe_wallet_sync(watch, synced, syncing, t);
}

// ---------------------------------------------------------------------------
// wallet_restart_budget
// ---------------------------------------------------------------------------

TEST(WalletRestartBudget, DoublesOncePerRestart)
{
    EXPECT_EQ(wallet_restart_budget(900, 0, 86400), 900);
    EXPECT_EQ(wallet_restart_budget(900, 1, 86400), 1800);
    EXPECT_EQ(wallet_restart_budget(900, 2, 86400), 3600);
    EXPECT_EQ(wallet_restart_budget(7200, 3, 86400), 57600);
}

TEST(WalletRestartBudget, StopsAtTheCap)
{
    EXPECT_EQ(wallet_restart_budget(7200, 4, 86400), 86400);     // 115,200 capped
    EXPECT_EQ(wallet_restart_budget(7200, 1000, 86400), 86400);
    EXPECT_EQ(wallet_restart_budget(100000, 0, 86400), 86400);   // a base above the cap
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    EXPECT_EQ(wallet_restart_budget(kMax / 2 + 1, 1, kMax), kMax);  // no overflow
}

// ---------------------------------------------------------------------------
// observe_wallet_sync
// ---------------------------------------------------------------------------

TEST(WalletSyncWatch, ASyncingWalletIsNotRestartedWithinTheSyncingBudget)
{
    // Heartbeats 30 s apart.  The old rule restarted at the 20th (600 s).
    WalletSyncWatch watch;
    for (std::int64_t t = 0; t < 7200; t += 30) {
        const WalletSyncVerdict v = see(watch, kUnsynced, kSyncing, t);
        ASSERT_EQ(v.action, WalletSyncAction::Wait) << "t=" << t;
        EXPECT_EQ(v.unsynced_for_s, t);
        EXPECT_EQ(v.idle_for_s, 0);
    }
    const WalletSyncVerdict v = see(watch, kUnsynced, kSyncing, 7200);
    EXPECT_EQ(v.action, WalletSyncAction::Restart);
    EXPECT_EQ(v.unsynced_for_s, 7200);
    EXPECT_EQ(v.restarts, 0U);
}

TEST(WalletSyncWatch, AWalletThatIsNotSyncingGetsTheIdleBudget)
{
    WalletSyncWatch watch;
    EXPECT_EQ(see(watch, kUnsynced, kIdle, 0).action, WalletSyncAction::Wait);
    EXPECT_EQ(see(watch, kUnsynced, kIdle, 450).action, WalletSyncAction::Wait);
    const WalletSyncVerdict last_wait = see(watch, kUnsynced, kIdle, 899);
    EXPECT_EQ(last_wait.action, WalletSyncAction::Wait);
    EXPECT_EQ(last_wait.idle_for_s, 899);
    const WalletSyncVerdict v = see(watch, kUnsynced, kIdle, 900);
    EXPECT_EQ(v.action, WalletSyncAction::Restart);
    EXPECT_EQ(v.idle_for_s, 900);
    EXPECT_EQ(v.idle_budget_s, 900);
}

TEST(WalletSyncWatch, ASyncAttemptResetsTheIdleRun)
{
    WalletSyncWatch watch;
    for (std::int64_t t = 0; t <= 800; t += 50) {
        ASSERT_EQ(see(watch, kUnsynced, kIdle, t).action, WalletSyncAction::Wait);
    }
    ASSERT_EQ(see(watch, kUnsynced, kSyncing, 830).action, WalletSyncAction::Wait);
    // A new idle run starts at 860; the streak itself keeps counting from 0.
    for (std::int64_t t = 860; t < 1760; t += 50) {
        const WalletSyncVerdict v = see(watch, kUnsynced, kIdle, t);
        ASSERT_EQ(v.action, WalletSyncAction::Wait) << "t=" << t;
        EXPECT_EQ(v.idle_for_s, t - 860);
        EXPECT_EQ(v.unsynced_for_s, t);
    }
    EXPECT_EQ(see(watch, kUnsynced, kIdle, 1760).action, WalletSyncAction::Restart);
}

TEST(WalletSyncWatch, AFlappingWalletStillMeetsTheSyncingBudget)
{
    // Syncing and not syncing on alternate heartbeats: no idle run ever lasts,
    // so only the syncing budget -- the total unsynced time -- can fire.
    WalletSyncWatch watch;
    bool syncing = true;
    for (std::int64_t t = 0; t < 7200; t += 30) {
        ASSERT_EQ(see(watch, kUnsynced, syncing, t).action, WalletSyncAction::Wait)
            << "t=" << t;
        syncing = !syncing;
    }
    EXPECT_EQ(see(watch, kUnsynced, syncing, 7200).action, WalletSyncAction::Restart);
}

TEST(WalletSyncWatch, EachRestartStartsAFreshStreakWithDoubledBudgets)
{
    WalletSyncWatch watch;
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 0).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 450).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 900).action, WalletSyncAction::Restart);

    // The next reading opens a new streak: nothing carried over.
    WalletSyncVerdict v = see(watch, kUnsynced, kIdle, 930);
    EXPECT_EQ(v.action, WalletSyncAction::Wait);
    EXPECT_EQ(v.unsynced_for_s, 0);
    EXPECT_EQ(v.idle_for_s, 0);
    EXPECT_EQ(v.restarts, 1U);
    EXPECT_EQ(v.idle_budget_s, 1800);
    EXPECT_EQ(v.syncing_budget_s, 14400);

    // Within the doubled idle budget -- 930 + 900 included -- nothing fires.
    // Readings stay 30 s apart: a gap over 600 s would start a new streak.
    for (std::int64_t t = 960; t < 930 + 1800; t += 30) {
        ASSERT_EQ(see(watch, kUnsynced, kIdle, t).action, WalletSyncAction::Wait)
            << "t=" << t;
    }
    v = see(watch, kUnsynced, kIdle, 930 + 1800);
    EXPECT_EQ(v.action, WalletSyncAction::Restart);
    EXPECT_EQ(v.restarts, 1U);

    v = see(watch, kUnsynced, kIdle, 930 + 1830);
    EXPECT_EQ(v.restarts, 2U);
    EXPECT_EQ(v.idle_budget_s, 3600);
    EXPECT_EQ(v.syncing_budget_s, 28800);
}

TEST(WalletSyncWatch, SyncedClearsTheStreakAndTheBackoff)
{
    WalletSyncWatch watch;
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 0).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 450).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 900).action, WalletSyncAction::Restart);
    ASSERT_EQ(see(watch, kUnsynced, kSyncing, 960).action, WalletSyncAction::Wait);

    const WalletSyncVerdict synced = see(watch, kSynced, kIdle, 1500);
    EXPECT_EQ(synced.action, WalletSyncAction::Synced);
    EXPECT_EQ(synced.restarts, 1U);         // what the recovery took
    EXPECT_EQ(synced.unsynced_for_s, 540);  // the streak that ended (960 to 1500)

    const WalletSyncVerdict next = see(watch, kUnsynced, kIdle, 1530);
    EXPECT_EQ(next.action, WalletSyncAction::Wait);
    EXPECT_EQ(next.restarts, 0U);
    EXPECT_EQ(next.unsynced_for_s, 0);
    EXPECT_EQ(next.idle_budget_s, 900);
    EXPECT_EQ(next.syncing_budget_s, 7200);
}

TEST(WalletSyncWatch, ASyncedWalletReportsNoStreak)
{
    WalletSyncWatch watch;
    const WalletSyncVerdict v = see(watch, kSynced, kIdle, 100);
    EXPECT_EQ(v.action, WalletSyncAction::Synced);
    EXPECT_EQ(v.unsynced_for_s, 0);
    EXPECT_EQ(v.restarts, 0U);
}

TEST(WalletSyncWatch, ALongSilenceStartsANewStreak)
{
    // Step 8 not reached for 5000 s (paused, breaker open): the wallet may
    // have synced and dropped again in between, so the gap is not counted.
    WalletSyncWatch watch;
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 0).action, WalletSyncAction::Wait);
    const WalletSyncVerdict v = see(watch, kUnsynced, kIdle, 5000);
    EXPECT_EQ(v.action, WalletSyncAction::Wait);
    EXPECT_EQ(v.unsynced_for_s, 0);
    EXPECT_EQ(v.idle_for_s, 0);
}

TEST(WalletSyncWatch, AGapUpToTheLimitStillCounts)
{
    WalletSyncWatch watch;
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 0).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 600).action, WalletSyncAction::Wait);
    // 300 s later: within the 600 s gap limit, so the run is 900 s long.
    EXPECT_EQ(see(watch, kUnsynced, kIdle, 900).action, WalletSyncAction::Restart);
}

TEST(WalletSyncWatch, ASilenceKeepsTheBackoff)
{
    WalletSyncWatch watch;
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 0).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 450).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 900).action, WalletSyncAction::Restart);
    const WalletSyncVerdict v = see(watch, kUnsynced, kIdle, 900 + 5000);
    EXPECT_EQ(v.restarts, 1U);
    EXPECT_EQ(v.idle_budget_s, 1800);
}

TEST(WalletSyncWatch, AFailedRestartDoesNotAdvanceTheBackoff)
{
    // Review, PR #170: a restart command that failed restarted nothing, so
    // the next attempt must not wait double.
    WalletSyncWatch watch;
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 0).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 450).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 900).action, WalletSyncAction::Restart);
    record_failed_wallet_restart(watch);
    EXPECT_EQ(watch.restarts, 0U);
    EXPECT_EQ(watch.failed_restarts, 1U);

    const WalletSyncVerdict v = see(watch, kUnsynced, kIdle, 930);
    EXPECT_EQ(v.restarts, 0U);
    EXPECT_EQ(v.idle_budget_s, 900);       // not 1800
    EXPECT_EQ(v.syncing_budget_s, 7200);   // not 14400
    for (std::int64_t t = 960; t < 930 + 900; t += 30) {
        ASSERT_EQ(see(watch, kUnsynced, kIdle, t).action, WalletSyncAction::Wait)
            << "t=" << t;
    }
    EXPECT_EQ(see(watch, kUnsynced, kIdle, 930 + 900).action, WalletSyncAction::Restart);
}

TEST(WalletSyncWatch, AFailedRestartStillWaitsAFullBudgetBeforeRetrying)
{
    // The alternative -- restoring the streak -- would decide Restart again at
    // the very next heartbeat: a blocking command per heartbeat while it fails.
    WalletSyncWatch watch;
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 0).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 450).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 900).action, WalletSyncAction::Restart);
    record_failed_wallet_restart(watch);
    const WalletSyncVerdict v = see(watch, kUnsynced, kIdle, 930);
    EXPECT_EQ(v.action, WalletSyncAction::Wait);
    EXPECT_EQ(v.unsynced_for_s, 0);
    EXPECT_EQ(v.idle_for_s, 0);
}

TEST(WalletSyncWatch, AFailedRestartCannotUnderflowTheBackoff)
{
    WalletSyncWatch watch;
    record_failed_wallet_restart(watch);
    EXPECT_EQ(watch.restarts, 0U);
    EXPECT_EQ(watch.failed_restarts, 1U);
    EXPECT_EQ(see(watch, kUnsynced, kIdle, 0).idle_budget_s, 900);
}

TEST(WalletSyncWatch, SyncedClearsTheFailedAttempts)
{
    WalletSyncWatch watch;
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 0).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 450).action, WalletSyncAction::Wait);
    ASSERT_EQ(see(watch, kUnsynced, kIdle, 900).action, WalletSyncAction::Restart);
    record_failed_wallet_restart(watch);
    ASSERT_EQ(watch.failed_restarts, 1U);
    EXPECT_EQ(see(watch, kSynced, kIdle, 960).action, WalletSyncAction::Synced);
    EXPECT_EQ(watch.failed_restarts, 0U);
}

TEST(WalletSyncWatch, TheEveningOf20260922IsNoLongerALivelock)
{
    // What Step 8 saw from 17:08:37 to 18:27:20: 189 readings of synced=false,
    // syncing=true, ~25 s apart.  The 20-heartbeat rule restarted the wallet
    // 9 times in that window, each restart discarding the sync in progress and
    // rolling the wallet back 256 blocks.  The watch restarts it zero times.
    WalletSyncWatch watch;
    int restarts = 0;
    std::int64_t t = 0;
    for (int reading = 0; reading < 189; ++reading, t += 25) {
        if (see(watch, kUnsynced, kSyncing, t).action == WalletSyncAction::Restart) {
            ++restarts;
        }
    }
    EXPECT_EQ(restarts, 0);
    EXPECT_LT(t, WalletRestartPolicy{}.syncing_budget_s);
}

TEST(WalletSyncWatch, APolicyOverridesEveryBudget)
{
    WalletRestartPolicy policy;
    policy.idle_budget_s         = 60;
    policy.syncing_budget_s      = 120;
    policy.max_budget_s          = 200;
    policy.max_observation_gap_s = 1000;
    WalletSyncWatch watch;
    ASSERT_EQ(observe_wallet_sync(watch, kUnsynced, kSyncing, 0, policy).action,
              WalletSyncAction::Wait);
    EXPECT_EQ(observe_wallet_sync(watch, kUnsynced, kSyncing, 119, policy).action,
              WalletSyncAction::Wait);
    EXPECT_EQ(observe_wallet_sync(watch, kUnsynced, kSyncing, 120, policy).action,
              WalletSyncAction::Restart);
    const WalletSyncVerdict v = observe_wallet_sync(watch, kUnsynced, kSyncing, 130, policy);
    EXPECT_EQ(v.syncing_budget_s, 200);   // 240, capped
    EXPECT_EQ(v.idle_budget_s, 120);
}

}  // namespace
