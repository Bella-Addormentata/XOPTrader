#ifndef XOP_STRATEGY_PID_RAIL_LATCH_HPP
#define XOP_STRATEGY_PID_RAIL_LATCH_HPP
// ---------------------------------------------------------------------------
// pid_rail_latch.hpp -- "this controller has lost authority", edge-triggered.
//
// [S39 2026-09-01] Both fill-rate PIDs have sat at their most aggressive value
// for the entire retained log window and nothing reported it. A saturated
// controller is a constant with extra steps: it is not controlling, it is
// emitting a fixed offset that happens to be recomputed every heartbeat. This
// detects that state and says so ONCE.
//
// WHY THE PREDICATE IS ON THE INTEGRATOR AND NOT ON THE OUTPUT CLAMP
// ------------------------------------------------------------------
// The obvious predicate -- "output is at min_mult" -- would fire NEVER on the
// one controller that has been railed for 40 hours, because config.yaml's
// `pid_min_mult: 0.7` is unreachable (see pid_reachability.hpp). The condition
// that actually characterises lost authority is the ANTI-WINDUP CLAMP firing:
// the integrator is pinned and the error still points the same way, so further
// error accumulates into nothing.
//
// WHY THERE IS A HEARTBEAT, AND WHY IT IS NOT OPTIONAL
// ----------------------------------------------------
// engine.cpp:3135-3141 is this repo's one existing edge-triggered warning. It
// has no off-edge -- `breaker_skip_warned_` is cleared only at three unrelated
// sites -- and on 2026-08-31 it compressed a 4h10m total-quoting outage into a
// SINGLE log line that nobody saw. The lesson is not "log edges, not levels";
// it is "log edges AND heartbeat the level". A latch without a heartbeat trades
// one failure mode for a worse one.
//
// Shared verbatim by CompetitivenessPid and Engine::SpreadPidState -- both hold
// the same five quantities, so one detector serves both. Pure header, no engine
// types; driven directly by cpp/tests/test_pid_rail_latch.cpp with no PID and
// no Engine in the loop.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xop::strategy {

enum class RailSide : int { Low = -1, None = 0, High = +1 };

enum class RailEvent : int {
    None      = 0,
    Enter     = 1,   ///< the controller just lost authority
    Exit      = 2,   ///< it just got it back
};

/// Edge detector with hysteresis, dwell, rate limiting and a heartbeat.
///
/// Definitions, per tick:
///   sat(t)   := |I| >= enter_frac * I_max   AND   error * I > 0
///   clear(t) := |I| <= exit_frac  * I_max   OR    error * I <= 0
///
/// The `error * I > 0` conjunct excludes a full integrator whose error has
/// already reversed -- that controller is actively recovering and flagging it
/// would be a false positive.
///
/// Between the two bands NEITHER counter advances and the latch is unchanged.
/// A controller parked mid-band emits nothing and keeps whatever state it had;
/// resetting a counter there would make a slow drift across the band look like
/// a fresh event every time it wobbled.
struct PidRailLatch {
    // -- tuning ------------------------------------------------------------
    double        enter_frac{0.98};        ///< |I| >= this fraction of I_max
    double        exit_frac{0.80};         ///< |I| <= this fraction to clear
    std::uint32_t enter_dwell{50};         ///< ticks of sat() before Enter
    std::uint32_t exit_dwell{50};          ///< ticks of clear() before Exit
    std::uint32_t rate_limit_blocks{800};  ///< min blocks between Enter lines
    std::uint32_t heartbeat_blocks{133};   ///< ~1h; 0 disables (do not)

    // -- state -------------------------------------------------------------
    bool          latched{false};
    RailSide      side{RailSide::None};
    std::uint32_t n_sat{0};
    std::uint32_t n_clear{0};
    std::uint32_t latch_start_block{0};
    std::uint32_t last_emit_block{0};
    /// Count of Enter events suppressed by the rate limit. Their paired Exits
    /// are suppressed too, so emitted lines always pair.
    std::uint32_t suppressed{0};
    double        peak_abs_integral{0.0};

    /// Feed one tick. Returns the edge to log, or RailEvent::None.
    ///
    /// @param integral      the anti-windup accumulator, AFTER this tick.
    /// @param integral_max  the clamp bound; <= 0 disables detection.
    /// @param error         this tick's error term.
    /// @param block         monotonic block/tick index, used for rate limiting
    ///                      and the heartbeat.
    RailEvent update(double integral,
                     double integral_max,
                     double error,
                     std::uint32_t block) noexcept
    {
        if (!std::isfinite(integral) || !std::isfinite(integral_max)
            || !std::isfinite(error) || !(integral_max > 0.0)) {
            return RailEvent::None;
        }

        const double abs_i    = std::abs(integral);
        const bool   pushing  = (error * integral) > 0.0;
        const bool   sat      = (abs_i >= enter_frac * integral_max) && pushing;
        const bool   clear    = (abs_i <= exit_frac * integral_max) || !pushing;
        const RailSide cur    = integral > 0.0 ? RailSide::High
                              : integral < 0.0 ? RailSide::Low
                                               : RailSide::None;

        if (latched) {
            peak_abs_integral = std::max(peak_abs_integral, abs_i);

            // A sign flip means the integrator left the rail it was on, even
            // if it immediately pinned to the opposite one. Close the old
            // event now rather than silently relabelling it.
            if (sat && cur != side && cur != RailSide::None) {
                const bool emit = enter_was_emitted_;
                reset_latch_();
                return emit ? RailEvent::Exit : RailEvent::None;
            }
        }

        if (sat) {
            ++n_sat;
            n_clear = 0;
        } else if (clear) {
            ++n_clear;
            n_sat = 0;
        }
        // else: dead zone -- hold BOTH counters and the latch.

        if (!latched && sat && n_sat >= enter_dwell) {
            latched            = true;
            side               = cur;
            latch_start_block  = block;
            peak_abs_integral  = abs_i;
            n_clear            = 0;
            const bool first   = (last_emit_block == 0 && suppressed == 0);
            const bool spaced  = block >= last_emit_block
                              && (block - last_emit_block) >= rate_limit_blocks;
            if (first || spaced) {
                enter_was_emitted_ = true;
                last_emit_block    = block;
                return RailEvent::Enter;
            }
            enter_was_emitted_ = false;
            ++suppressed;
            return RailEvent::None;
        }

        if (latched && clear && n_clear >= exit_dwell) {
            const bool emit = enter_was_emitted_;
            reset_latch_();
            if (emit) {
                last_emit_block = block;
                return RailEvent::Exit;
            }
            return RailEvent::None;
        }

        return RailEvent::None;
    }

    /// How long the current latch has been held, in blocks. 0 when not latched.
    [[nodiscard]] std::uint32_t latched_blocks(std::uint32_t block) const noexcept
    {
        if (!latched || block < latch_start_block) {
            return 0;
        }
        return block - latch_start_block;
    }

    /// True once every `heartbeat_blocks` while latched, so a persistent rail
    /// keeps saying so without saying it every tick. See the header note: the
    /// absence of this is a shipped regression, not a hypothetical one.
    [[nodiscard]] bool heartbeat_due(std::uint32_t block) const noexcept
    {
        if (!latched || heartbeat_blocks == 0 || block <= latch_start_block) {
            return false;
        }
        return ((block - latch_start_block) % heartbeat_blocks) == 0;
    }

private:
    bool enter_was_emitted_{false};

    void reset_latch_() noexcept
    {
        latched            = false;
        side               = RailSide::None;
        n_sat              = 0;
        n_clear            = 0;
        peak_abs_integral  = 0.0;
        enter_was_emitted_ = false;
    }
};

}  // namespace xop::strategy

#endif  // XOP_STRATEGY_PID_RAIL_LATCH_HPP
