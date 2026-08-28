// watchdog.hpp -- Pure decision logic for the heartbeat dead man's switch.
//
// [S31 2026-08-27]
//
// WHY THIS EXISTS
// ---------------
// Nothing cancels our offers when the engine stops functioning.  Twice in
// two days that cost us:
//
//   2026-08-25  the full node was unreachable for ~4h while offers rested
//               on dexie.  When it returned, six four-hour-old XCH/BYC bids
//               filled in the same second and tripped the rolling-window
//               breaker (-$12.71 over 3 blocks).
//   2026-08-26  a stray --dry-run terminated the live process and left ten
//               offers on the book with nothing managing them.
//
// The offers outlive the process that is supposed to be managing them.
//
// WHY A SEPARATE THREAD IS NOT OPTIONAL
// -------------------------------------
// The engine runs a SINGLE `ioc_.run()` (engine.cpp:652).  Every coroutine
// -- poll loop, heartbeat, RPC -- shares that one thread.  So a watchdog
// posted to the same io_context is blocked by exactly the thing it exists
// to detect: the 2026-08-25 incident includes one cycle logged as
// "Block 9196847 processed in 14241482 ms" (3.96 HOURS) during which
// nothing else on that thread ran at all.
//
// A watchdog that can be starved by the stall it watches for is not a
// watchdog.  This header is therefore deliberately free of asio, engine
// types and I/O: it is the decision only, callable from a plain thread.
//
// WHAT IT DELIBERATELY DOES NOT DO
// --------------------------------
// It does not decide *how* to cancel.  On dexie there is no server-side
// dead man's switch (unlike Permuto's `schedule_cancel`), so the action is
// ours to perform -- and during the 2026-08-25 outage the WALLET RPC stayed
// healthy the whole time while the full node was unreachable, so a
// wallet-only cancel path is viable.  That belongs to the caller.

#ifndef XOP_RISK_WATCHDOG_HPP
#define XOP_RISK_WATCHDOG_HPP

#include <cstdint>

namespace xop::risk {

/// What the watchdog wants the caller to do this tick.
enum class WatchdogAction {
    /// Heartbeat is recent enough.  Nothing to do.
    Healthy,
    /// No heartbeat has been recorded yet.  The engine is still starting;
    /// firing here would cancel the book of an engine that has not had a
    /// chance to run.  Distinct from Stalled so the caller can log it
    /// differently -- an engine that NEVER beats is a different fault from
    /// one that stopped.
    NotStartedYet,
    /// The heartbeat has aged past the threshold and the switch has not
    /// yet fired.  Cancel the book.
    Fire,
    /// Already fired for this stall.  Stay quiet until a heartbeat proves
    /// the engine is alive again -- re-firing every tick would spray cancel
    /// RPCs at a wallet that is probably already struggling.
    AlreadyFired,
};

[[nodiscard]] inline const char* to_string(WatchdogAction a) noexcept {
    switch (a) {
        case WatchdogAction::Healthy:       return "healthy";
        case WatchdogAction::NotStartedYet: return "not_started_yet";
        case WatchdogAction::Fire:          return "fire";
        case WatchdogAction::AlreadyFired:  return "already_fired";
    }
    return "unknown";
}

/// Inputs for one watchdog tick.  All times are monotonic milliseconds from
/// the same clock; the header does not care which, only that they are
/// comparable and do not jump when the wall clock is adjusted.
struct WatchdogInput {
    /// When the last heartbeat cycle COMPLETED.  0 means "never".
    std::int64_t last_beat_ms{0};
    /// Now.
    std::int64_t now_ms{0};
    /// How stale the heartbeat may get before the switch fires.  0 or
    /// negative disables the watchdog entirely.
    std::int64_t threshold_ms{0};
    /// Whether the switch has already fired for the current stall.
    bool already_fired{false};
};

/// Decide what to do this tick.
///
/// Deliberately total and side-effect free: every combination of inputs
/// maps to exactly one action, so the caller has no judgement left to
/// exercise and the whole decision is testable without a wallet.
[[nodiscard]] constexpr WatchdogAction watchdog_decide(
    const WatchdogInput& in) noexcept
{
    if (in.threshold_ms <= 0) {
        return WatchdogAction::Healthy;   // disabled
    }
    if (in.last_beat_ms <= 0) {
        return WatchdogAction::NotStartedYet;
    }

    // A clock that has gone BACKWARDS yields a negative age.  Treat that as
    // "no elapsed time", never as a stall: an engine must not cancel its
    // book because a clock was adjusted.  Same reasoning as the unsigned
    // guard in carry_expired.
    const std::int64_t age = in.now_ms - in.last_beat_ms;
    if (age < in.threshold_ms) {
        return WatchdogAction::Healthy;
    }
    return in.already_fired ? WatchdogAction::AlreadyFired
                            : WatchdogAction::Fire;
}

/// Tracks fired-state across ticks so the caller does not have to.
///
/// One stall produces exactly one Fire.  The latch clears only when a
/// heartbeat lands that is NEWER than the one which triggered the fire --
/// not merely when the age drops, because a clock adjustment could
/// otherwise clear it without the engine having recovered.
class Watchdog {
public:
    explicit constexpr Watchdog(std::int64_t threshold_ms) noexcept
        : threshold_ms_(threshold_ms) {}

    /// Evaluate one tick and update internal state.
    [[nodiscard]] constexpr WatchdogAction tick(
        std::int64_t last_beat_ms, std::int64_t now_ms) noexcept
    {
        // A heartbeat strictly newer than the one we fired on means the
        // engine is beating again: re-arm.
        if (fired_ && last_beat_ms > fired_on_beat_ms_) {
            fired_ = false;
            fired_on_beat_ms_ = 0;
        }

        const WatchdogAction a = watchdog_decide(
            WatchdogInput{last_beat_ms, now_ms, threshold_ms_, fired_});

        if (a == WatchdogAction::Fire) {
            fired_ = true;
            fired_on_beat_ms_ = last_beat_ms;
        }
        return a;
    }

    [[nodiscard]] constexpr bool fired() const noexcept { return fired_; }
    [[nodiscard]] constexpr std::int64_t threshold_ms() const noexcept {
        return threshold_ms_;
    }

private:
    std::int64_t threshold_ms_{0};
    bool         fired_{false};
    std::int64_t fired_on_beat_ms_{0};
};

}  // namespace xop::risk

#endif  // XOP_RISK_WATCHDOG_HPP
