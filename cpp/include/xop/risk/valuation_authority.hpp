#pragma once
// ---------------------------------------------------------------------------
// valuation_authority -- [S20 2026-08-24] when an equity figure may move the
// drawdown peak.
//
// Extracted as pure logic for the same reason drawdown_breaker.hpp was: these
// are safety-critical semantics that an off-by-one or an inverted gate would
// reverse silently, and the engine has no test harness of its own.  Every
// rule below is therefore pinned in cpp/tests/test_valuation_authority.cpp
// rather than living only inside Step 13.
//
// The policy, stated once:
//
//   * A DEGRADED cycle -- one where a held asset's last-known price has been
//     carried past its TTL without a fresh valuation-grade print -- may not
//     move the peak.  A suspect number must never ratchet the high-water
//     mark, because every later drawdown is measured from it.
//
//   * Degradation does NOT disarm the breakers.  They keep comparing against
//     the frozen peak.  Disarming them would make a risk control fail OPEN:
//     the engine goes on quoting with the very prices whose valuation we
//     distrust, for a condition that can persist for hours.  A frozen peak
//     still detects a real loss; it simply cannot invent a higher one to
//     measure from.  (An earlier revision of S20 did disarm them, and review
//     was right to reject it.)
//
//   * Recovery is debounced.  Peak updates resume only after a run of clean
//     cycles, mirroring the S18 breaker lift-streak, so a feed alternating
//     between junk and honest cycles cannot ratchet the peak upward one
//     accepted cycle at a time.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace xop::risk {

/// Whether a carried price has outlived its usefulness as an authority.
///
/// `ttl_blocks == 0` disables the expiry entirely (documented config
/// semantics).  A REGRESSED height -- a reorg, or a node swap serving a
/// lower tip -- reads as "no elapsed time", never as an enormous age: the
/// subtraction is on an unsigned type, so an unguarded `current - last`
/// would wrap and instantly declare every asset degraded.
[[nodiscard]] inline bool carry_expired(std::uint32_t current_block,
                                        std::uint32_t last_live_block,
                                        std::uint32_t ttl_blocks) noexcept
{
    if (ttl_blocks == 0)               return false;
    if (current_block <= last_live_block) return false;
    return (current_block - last_live_block) > ttl_blocks;
}

/// Per-process gate deciding whether this cycle's equity may move the peak.
class ValuationAuthorityGate {
public:
    /// Consecutive clean cycles required before peak updates resume after a
    /// degraded episode.
    static constexpr int kRearmCleanCycles = 10;

    struct Step {
        bool may_update_peak{false};  ///< equity may raise the high-water mark
        bool entered_degraded{false}; ///< transition -> degraded (warn once)
        bool recovered{false};        ///< transition -> re-armed (log once)
    };

    /// Advance one cycle.  A process that has never degraded starts fully
    /// armed, so the first valued cycle seeds the peak exactly as before.
    Step step(bool degraded) noexcept
    {
        Step out;
        if (degraded) {
            out.entered_degraded = (clean_streak_ >= kRearmCleanCycles);
            clean_streak_        = 0;
            out.may_update_peak  = false;
            return out;
        }
        if (clean_streak_ < kRearmCleanCycles) {
            ++clean_streak_;
            out.recovered = (clean_streak_ == kRearmCleanCycles);
        }
        out.may_update_peak = (clean_streak_ >= kRearmCleanCycles);
        return out;
    }

    [[nodiscard]] int clean_streak() const noexcept { return clean_streak_; }

private:
    int clean_streak_{kRearmCleanCycles};
};

}  // namespace xop::risk
