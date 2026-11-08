// ratio_mode.hpp -- [RATIO-MODE-PERSIST 2026-11-08] The per-pair ratio
// rebalance mode (strategy.ratio_rebalance_enabled): its hysteresis, the
// bands it uses, and the text form the engine saves it under.
//
// The mode was kept only in memory and every process started Neutral.  The
// enter bands are wide on purpose (XCH/wUSDC.b enters AcquireBase at ratio
// <= target - 0.49) and the exit bands narrow (+/- 0.04), so a pair that had
// been tapering its overweight side for days could not get back in after a
// restart: on 2026-11-07 the v0.10.44 install dropped XCH/wUSDC.b, XCH/DBX
// and XCH/BYC from AcquireBase to Neutral at ratios of 0.39-0.68, and the
// XCH/wUSDC.b ask pool went from ~4 XCH (tapered x0.56) to 33 XCH, selling
// the asset the pair was short of.  The engine now saves every transition
// (Database::save_ratio_mode) and restores the saved modes at startup, so a
// restart resumes the hysteresis where it left off.  A restored mode makes
// no transition until the pair's holdings have been checked against the
// wallet (Engine::ratio_mode_held), and both Step 7 and Step 8 decide on the
// pair's own holdings ratio, never the cross-pair-adjusted one, so the two
// sites cannot pull one mode in opposite directions.
//
// A restored mode is only as current as the moment it was saved: the target
// may have been edited, or the balances moved, while the engine was off.  So
// a non-Neutral mode now exits as soon as the ratio is back on the near side
// of its exit band, not only while it sits INSIDE the band.  Before, a pair
// in AcquireBase whose ratio jumped from below the band to above it (a large
// bid fill, or a target lowered by a live config reload) kept tapering asks
// and boosting bids while already overweight base, because it never saw a
// ratio inside +/- exit_band.  On exiting, the Neutral entry rule runs in the
// same step, so a ratio already past the opposite enter band goes straight
// to the opposite mode.

#ifndef XOP_STRATEGY_RATIO_MODE_HPP
#define XOP_STRATEGY_RATIO_MODE_HPP

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace xop::strategy::ratiomode {

enum class Mode : std::uint8_t {
    Neutral      = 0,
    AcquireBase  = 1,
    AcquireQuote = 2
};

/// The thresholds around one pair's target ratio (base value / total).
struct Bands {
    double target{0.5};
    double enter{0.10};  ///< Neutral leaves at target -/+ enter.
    double exit{0.05};   ///< A non-Neutral mode returns at target -/+ exit.
};

/// The pair's bands: strategy.ratio_target_by_pair (else ratio_target) and
/// strategy.ratio_band_enter_by_pair (else ratio_band_enter).  A per-pair
/// enter band caps the exit band at half of it, so the hysteresis stays
/// consistent however wide or narrow the pair's band is.
[[nodiscard]] inline Bands bands_for(
    const std::string& pair_name,
    double ratio_target,
    double band_enter,
    double band_exit,
    const std::unordered_map<std::string, double>& target_by_pair,
    const std::unordered_map<std::string, double>& band_enter_by_pair)
{
    Bands b;
    b.target = ratio_target;
    if (auto it = target_by_pair.find(pair_name); it != target_by_pair.end()) {
        b.target = it->second;
    }
    b.enter = band_enter;
    b.exit = band_exit;
    if (auto it = band_enter_by_pair.find(pair_name);
        it != band_enter_by_pair.end()) {
        b.enter = it->second;
        b.exit = std::min(b.exit, b.enter * 0.5);
        if (b.exit <= 0.0) {
            b.exit = b.enter * 0.5;
        }
    }
    return b;
}

/// The mode after observing @p ratio in mode @p prev.
///
/// Neutral enters AcquireBase at ratio <= target - enter and AcquireQuote at
/// ratio >= target + enter.  AcquireBase exits once ratio >= target - exit
/// (back near the target, or past it); AcquireQuote once ratio <= target +
/// exit.  An exit applies the Neutral rule in the same step.
[[nodiscard]] inline Mode next_mode(Mode prev, double ratio,
                                    const Bands& b) noexcept
{
    Mode cur = prev;
    if (cur == Mode::AcquireBase && ratio >= b.target - b.exit) {
        cur = Mode::Neutral;
    } else if (cur == Mode::AcquireQuote && ratio <= b.target + b.exit) {
        cur = Mode::Neutral;
    }
    if (cur == Mode::Neutral) {
        if (ratio >= b.target + b.enter) return Mode::AcquireQuote;
        if (ratio <= b.target - b.enter) return Mode::AcquireBase;
    }
    return cur;
}

/// The text the mode is logged and saved under.
[[nodiscard]] inline const char* mode_name(Mode m) noexcept
{
    switch (m) {
        case Mode::Neutral:      return "Neutral";
        case Mode::AcquireBase:  return "AcquireBase";
        case Mode::AcquireQuote: return "AcquireQuote";
    }
    return "Unknown";
}

/// The mode a saved text names; nullopt for anything else, so a corrupt or
/// future value is ignored rather than guessed at.
[[nodiscard]] inline std::optional<Mode> parse_mode(std::string_view s) noexcept
{
    if (s == "Neutral")      return Mode::Neutral;
    if (s == "AcquireBase")  return Mode::AcquireBase;
    if (s == "AcquireQuote") return Mode::AcquireQuote;
    return std::nullopt;
}

}  // namespace xop::strategy::ratiomode

#endif  // XOP_STRATEGY_RATIO_MODE_HPP
