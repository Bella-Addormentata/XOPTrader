// test_watchdog.cpp -- [S31 2026-08-27]
//
// The watchdog cancels the whole book when it fires, so a false positive is
// expensive and a false negative is what we are trying to prevent.  These
// pin both edges.

#include <gtest/gtest.h>

#include <xop/risk/watchdog.hpp>

using xop::risk::Watchdog;
using xop::risk::WatchdogAction;
using xop::risk::WatchdogInput;
using xop::risk::watchdog_decide;

namespace {
constexpr std::int64_t kMin = 60'000;   // one minute in ms
}

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

TEST(Watchdog, HealthyWhileTheHeartbeatIsRecent) {
    EXPECT_EQ(watchdog_decide({1'000, 1'000 + 5 * kMin, 10 * kMin, false}),
              WatchdogAction::Healthy);
}

TEST(Watchdog, FiresOnceTheHeartbeatPassesTheThreshold) {
    EXPECT_EQ(watchdog_decide({1'000, 1'000 + 10 * kMin, 10 * kMin, false}),
              WatchdogAction::Fire);
}

TEST(Watchdog, NeverFiresBeforeTheFirstHeartbeat) {
    // Startup: the engine has not had a chance to beat yet.  Firing here
    // would cancel the book of an engine that has done nothing wrong.
    EXPECT_EQ(watchdog_decide({0, 999'999'999, 10 * kMin, false}),
              WatchdogAction::NotStartedYet);
}

TEST(Watchdog, ADisabledThresholdNeverFires) {
    EXPECT_EQ(watchdog_decide({1'000, 999'999'999, 0, false}),
              WatchdogAction::Healthy);
    EXPECT_EQ(watchdog_decide({1'000, 999'999'999, -1, false}),
              WatchdogAction::Healthy);
}

TEST(Watchdog, AClockGoingBackwardsIsNotAStall) {
    // A negative age must read as "no elapsed time", never as an infinite
    // one.  Cancelling the book because a clock was adjusted would be a
    // spectacular own goal -- same reasoning as the unsigned guard in
    // carry_expired.
    EXPECT_EQ(watchdog_decide({500'000, 1'000, 10 * kMin, false}),
              WatchdogAction::Healthy);
}

TEST(Watchdog, SaysAlreadyFiredRatherThanFiringAgain) {
    EXPECT_EQ(watchdog_decide({1'000, 1'000 + 30 * kMin, 10 * kMin, true}),
              WatchdogAction::AlreadyFired);
}

// ---------------------------------------------------------------------------
// The latch
// ---------------------------------------------------------------------------

TEST(Watchdog, OneStallProducesExactlyOneFire) {
    // Re-firing every tick would spray cancel RPCs at a wallet that is
    // probably already struggling -- and the 2026-08-25 outage ran for four
    // hours, which at a 30s tick is ~480 ticks.
    Watchdog wd(10 * kMin);
    const std::int64_t beat = 1'000;

    EXPECT_EQ(wd.tick(beat, beat + 5 * kMin),  WatchdogAction::Healthy);
    EXPECT_EQ(wd.tick(beat, beat + 10 * kMin), WatchdogAction::Fire);

    int fires = 0;
    for (int i = 11; i < 250; ++i) {
        if (wd.tick(beat, beat + i * kMin) == WatchdogAction::Fire) ++fires;
    }
    EXPECT_EQ(fires, 0) << "the stall must fire once, not once per tick";
    EXPECT_TRUE(wd.fired());
}

TEST(Watchdog, ANewerHeartbeatReArmsTheSwitch) {
    Watchdog wd(10 * kMin);
    ASSERT_EQ(wd.tick(1'000, 1'000 + 10 * kMin), WatchdogAction::Fire);
    ASSERT_TRUE(wd.fired());

    // Engine recovers and beats again.
    const std::int64_t new_beat = 1'000 + 11 * kMin;
    EXPECT_EQ(wd.tick(new_beat, new_beat + kMin), WatchdogAction::Healthy);
    EXPECT_FALSE(wd.fired());

    // And it can protect us a second time.
    EXPECT_EQ(wd.tick(new_beat, new_beat + 10 * kMin), WatchdogAction::Fire);
}

TEST(Watchdog, TheLatchDoesNotClearOnAnUnchangedHeartbeat) {
    // Only a STRICTLY NEWER beat proves recovery.  Clearing merely because
    // the age dropped would let a clock adjustment re-arm the switch while
    // the engine is still wedged, and it would then fire a second time on
    // the same stall.
    Watchdog wd(10 * kMin);
    const std::int64_t beat = 1'000;
    ASSERT_EQ(wd.tick(beat, beat + 10 * kMin), WatchdogAction::Fire);

    // Same beat, clock jumps backwards to look "recent".
    EXPECT_EQ(wd.tick(beat, beat + kMin), WatchdogAction::Healthy);
    EXPECT_TRUE(wd.fired()) << "unchanged heartbeat is not evidence of recovery";

    // Ages out again -- must NOT fire twice for the one stall.
    EXPECT_EQ(wd.tick(beat, beat + 40 * kMin), WatchdogAction::AlreadyFired);
}

// ---------------------------------------------------------------------------
// The incidents this exists for
// ---------------------------------------------------------------------------

TEST(Watchdog, TheFourHourNodeOutageFiresWellBeforeTheOffersAreStale) {
    // 2026-08-25: node unreachable ~4h, then six four-hour-old bids filled
    // in one second.  At a 10-minute threshold the switch fires ~3h50m
    // before those fills.
    Watchdog wd(10 * kMin);
    const std::int64_t beat = 1;   // NOT 0 -- that means "never beaten"
    EXPECT_EQ(wd.tick(beat, beat + 9 * kMin),   WatchdogAction::Healthy);
    EXPECT_EQ(wd.tick(beat, beat + 10 * kMin),  WatchdogAction::Fire);
    EXPECT_EQ(wd.tick(beat, beat + 240 * kMin), WatchdogAction::AlreadyFired);
}

TEST(Watchdog, NormalCycleDurationsDoNotTripIt) {
    // Observed live: normal cycles 3.2s-13.9s, and the pathological one
    // 14,241,482 ms (3.96 HOURS).  A 10-minute threshold sits three orders
    // of magnitude above normal and far below pathological, so the margin
    // is not a judgement call.
    Watchdog wd(10 * kMin);
    std::int64_t now = 1, beat = 1;   // a positive first beat: 0 means "never"
    for (int i = 0; i < 500; ++i) {
        now += 13'900;                                   // worst normal cycle
        EXPECT_EQ(wd.tick(beat, now), WatchdogAction::Healthy)
            << "tripped on a normal cycle at iteration " << i;
        beat = now;
    }
}
