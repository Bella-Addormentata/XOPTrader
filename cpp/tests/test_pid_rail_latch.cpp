// ---------------------------------------------------------------------------
// [S39] The saturation detector: one line when a controller loses authority,
// one when it gets it back, and a heartbeat in between.
//
// WHY THE HEARTBEAT TESTS ARE NOT CEREMONY
// ----------------------------------------
// engine.cpp:3135-3141 is this repo's one existing edge-triggered warning and
// it has no off-edge -- breaker_skip_warned_ is cleared only at three unrelated
// sites. On 2026-08-31 it compressed a 4h10m total-quoting outage into a single
// log line that nobody saw. HeartbeatFiresWhileLatched is the regression test
// for that shipped bug, transplanted here so the same mistake cannot be made
// twice.
//
// The counter-pressure is real too: the live log carries 3,129 warnings in 5.4
// hours from only 18 distinct messages, which is why the naive "warn whenever
// saturated" design is what we are avoiding.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "xop/strategy/pid_rail_latch.hpp"

using xop::strategy::PidRailLatch;
using xop::strategy::RailEvent;
using xop::strategy::RailSide;

namespace {

constexpr double kImax = 2.0;

/// Pinned against the rail, error still pushing the same way.
constexpr double kSatI   = 2.0;      // |I| = 1.00 * I_max
constexpr double kSatErr = 0.10;

/// Well inside the exit band.
constexpr double kClearI   = 0.5;    // 0.25 * I_max, below exit_frac 0.80
constexpr double kClearErr = 0.10;

/// Drive `n` ticks and return how many events of each kind came back.
struct EventCounts {
    int enters{0};
    int exits{0};
};

EventCounts drive(PidRailLatch& l, int n, double integral, double error,
                  std::uint32_t& block)
{
    EventCounts c{};
    for (int i = 0; i < n; ++i) {
        const auto ev = l.update(integral, kImax, error, ++block);
        if (ev == RailEvent::Enter) ++c.enters;
        if (ev == RailEvent::Exit)  ++c.exits;
    }
    return c;
}

}  // namespace

// -- Dwell ------------------------------------------------------------------

TEST(PidRailLatch, EnterRequiresFullDwell)
{
    PidRailLatch l{};
    std::uint32_t b = 0;

    const auto before = drive(l, 49, kSatI, kSatErr, b);
    EXPECT_EQ(before.enters, 0) << "49 saturated ticks must be silent";
    EXPECT_FALSE(l.latched);

    EXPECT_EQ(l.update(kSatI, kImax, kSatErr, ++b), RailEvent::Enter);
    EXPECT_TRUE(l.latched);
    EXPECT_EQ(l.side, RailSide::High);
}

TEST(PidRailLatch, ExitRequiresFullDwell)
{
    PidRailLatch l{};
    std::uint32_t b = 0;
    drive(l, 50, kSatI, kSatErr, b);
    ASSERT_TRUE(l.latched);

    const auto before = drive(l, 49, kClearI, kClearErr, b);
    EXPECT_EQ(before.exits, 0) << "49 clear ticks must not un-latch";
    EXPECT_TRUE(l.latched);

    EXPECT_EQ(l.update(kClearI, kImax, kClearErr, ++b), RailEvent::Exit);
    EXPECT_FALSE(l.latched);
}

// -- Hysteresis -------------------------------------------------------------

TEST(PidRailLatch, DeadZoneHoldsBothCounters)
{
    // 0.90 * I_max sits between exit_frac (0.80) and enter_frac (0.98).
    // A controller parked there emits nothing AND keeps the counters it had --
    // resetting them would make a slow drift across the band look like a fresh
    // event every time it wobbled.
    PidRailLatch l{};
    std::uint32_t b = 0;
    drive(l, 10, kSatI, kSatErr, b);
    ASSERT_EQ(l.n_sat, 10u);
    ASSERT_FALSE(l.latched);

    const auto parked = drive(l, 500, 0.90 * kImax, kSatErr, b);
    EXPECT_EQ(parked.enters, 0);
    EXPECT_EQ(parked.exits, 0);
    EXPECT_EQ(l.n_sat, 10u)   << "the dead zone must HOLD n_sat, not reset it";
    EXPECT_EQ(l.n_clear, 0u)  << "and must not advance n_clear either";
    EXPECT_FALSE(l.latched);
}

TEST(PidRailLatch, HysteresisBandIsAsymmetric)
{
    // Draining to 0.85 * I_max is not enough to clear -- exit_frac is 0.80.
    // Equal enter/exit thresholds would flap on a controller hovering at the
    // rail, which is exactly the noise this design exists to avoid.
    PidRailLatch l{};
    std::uint32_t b = 0;
    drive(l, 50, kSatI, kSatErr, b);
    ASSERT_TRUE(l.latched);

    const auto held = drive(l, 500, 0.85 * kImax, kSatErr, b);
    EXPECT_EQ(held.exits, 0);
    EXPECT_TRUE(l.latched);
}

// -- False positives --------------------------------------------------------

TEST(PidRailLatch, RecoveringControllerIsNotFlagged)
{
    // A full integrator whose error has already reversed is not stuck -- it is
    // draining. error * I <= 0 must disqualify it, or every controller that
    // ever reaches the clamp gets reported on its way back down.
    PidRailLatch l{};
    std::uint32_t b = 0;

    const auto c = drive(l, 200, kSatI, -kSatErr, b);
    EXPECT_EQ(c.enters, 0);
    EXPECT_FALSE(l.latched);
}

TEST(PidRailLatch, SignFlipClosesTheOldEventBeforeOpeningANewOne)
{
    // The integrator leaving +I_max for -I_max is a different fault, even
    // though |I| never left the rail. Silently relabelling `side` would emit
    // an Exit that never matches its Enter.
    PidRailLatch l{};
    l.rate_limit_blocks = 0;          // isolate: rate limiting is tested below
    std::uint32_t b = 0;

    drive(l, 50, kSatI, kSatErr, b);
    ASSERT_TRUE(l.latched);
    ASSERT_EQ(l.side, RailSide::High);

    EXPECT_EQ(l.update(-kSatI, kImax, -kSatErr, ++b), RailEvent::Exit)
        << "the High event must be closed before a Low one opens";
    EXPECT_FALSE(l.latched);

    const auto again = drive(l, 50, -kSatI, -kSatErr, b);
    EXPECT_EQ(again.enters, 1);
    EXPECT_TRUE(l.latched);
    EXPECT_EQ(l.side, RailSide::Low);
}

// -- Rate limiting ----------------------------------------------------------

TEST(PidRailLatch, RateLimitSuppressesEmissionButNotState)
{
    // Suppression must apply to the LINE, never to the latch -- and a
    // suppressed Enter must suppress its Exit too, or the log fills with
    // orphaned Exits that imply a recovery nobody was told about.
    PidRailLatch l{};
    std::uint32_t b = 0;
    int enters = 0;
    int exits  = 0;

    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < 50; ++i) {
            if (l.update(kSatI, kImax, kSatErr, ++b) == RailEvent::Enter) ++enters;
        }
        EXPECT_TRUE(l.latched) << "cycle " << cycle << ": latch must hold "
                                  "regardless of whether the line was emitted";
        for (int i = 0; i < 50; ++i) {
            if (l.update(kClearI, kImax, kClearErr, ++b) == RailEvent::Exit) ++exits;
        }
        EXPECT_FALSE(l.latched);
    }

    EXPECT_EQ(enters, 1) << "3 rail entries inside the 800-block window";
    EXPECT_EQ(l.suppressed, 2u);
    EXPECT_EQ(exits, enters) << "emitted lines must always pair";
}

// -- Heartbeat --------------------------------------------------------------

TEST(PidRailLatch, HeartbeatFiresWhileLatched)
{
    // THE breaker_skip_warned_ REGRESSION TEST. An edge without a heartbeat
    // turned a 4h10m outage into one unread line on 2026-08-31.
    PidRailLatch l{};
    std::uint32_t b = 0;
    drive(l, 50, kSatI, kSatErr, b);
    ASSERT_TRUE(l.latched);

    int beats = 0;
    for (int i = 0; i < 1000; ++i) {
        l.update(kSatI, kImax, kSatErr, ++b);
        if (l.heartbeat_due(b)) ++beats;
    }
    EXPECT_EQ(beats, 7) << "1000 blocks / 133 = 7 heartbeats while railed";
    EXPECT_EQ(l.latched_blocks(b), 1000u);
}

TEST(PidRailLatch, NoHeartbeatWhenNotLatched)
{
    PidRailLatch l{};
    std::uint32_t b = 0;
    for (int i = 0; i < 500; ++i) {
        l.update(kClearI, kImax, kClearErr, ++b);
        EXPECT_FALSE(l.heartbeat_due(b));
    }
    EXPECT_EQ(l.latched_blocks(b), 0u);
}

TEST(PidRailLatch, HeartbeatCanBeDisabledButLatchStillWorks)
{
    PidRailLatch l{};
    l.heartbeat_blocks = 0;
    std::uint32_t b = 0;
    drive(l, 50, kSatI, kSatErr, b);
    ASSERT_TRUE(l.latched);
    for (int i = 0; i < 500; ++i) {
        l.update(kSatI, kImax, kSatErr, ++b);
        EXPECT_FALSE(l.heartbeat_due(b));
    }
}

// -- Refusals ---------------------------------------------------------------

TEST(PidRailLatch, DegenerateInputsAreIgnored)
{
    PidRailLatch l{};
    std::uint32_t b = 0;
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    for (int i = 0; i < 200; ++i) {
        EXPECT_EQ(l.update(kSatI, 0.0, kSatErr, ++b), RailEvent::None)
            << "integral_max <= 0 disables detection rather than dividing";
        EXPECT_EQ(l.update(kNaN, kImax, kSatErr, ++b), RailEvent::None);
        EXPECT_EQ(l.update(kSatI, kImax, kNaN, ++b), RailEvent::None);
    }
    EXPECT_FALSE(l.latched);
}
