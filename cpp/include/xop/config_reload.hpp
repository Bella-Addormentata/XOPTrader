// ---------------------------------------------------------------------------
// config_reload.hpp -- what a saved config may change on a RUNNING engine.
//
// [RELOAD 2026-08-29] The engine reads config.yaml once, at startup, and the
// GUI's Settings save wrote the file and said nothing -- so an operator who
// disabled a pair mid-incident watched the engine keep quoting it with the
// old config it was born with. That is how four wmilliETH.b bids were posted
// on a pair whose disable was already on disk.
//
// The live-applicable subset is deliberately tiny and asymmetric:
//
//   * DISABLING a pair applies live. Every quoting and taker path already
//     gates on `pair.enabled`, flipping the flag in place invalidates no
//     pointers (pair_config_map_ points into config_.pairs, which is not
//     reallocated), and the transition cancels the pair's resting offers --
//     the same off-means-flat rule every other disable path follows.
//     Turning things OFF must never wait on a restart.
//
//   * ENABLING a pair requires a restart. Its strategy object, depeg
//     registration and allocator seat were never constructed -- the engine
//     builds those only for pairs enabled at startup, and conjuring them
//     mid-flight is a money-path change that deserves a boot, not a flag.
//
//   * EVERYTHING ELSE requires a restart, and the reload says so rather
//     than silently applying nothing.
//
// Pure and total: the diff lives here, testable without an engine. The
// Engine owns the wiring (the flag file, the re-parse, the cancel).
// ---------------------------------------------------------------------------

#ifndef XOP_CONFIG_RELOAD_HPP
#define XOP_CONFIG_RELOAD_HPP

#include <string>
#include <vector>

namespace xop {

/// The reload verdict: what applies live, what waits for a restart.
struct ReloadDiff {
    /// Pairs the saved file DISABLES that are enabled in the running
    /// config. Applied live: flag flipped, resting offers cancelled.
    std::vector<std::string> to_disable;

    /// Pairs the saved file ENABLES that are disabled in the running
    /// config. NOT applied -- reported as restart-required, because their
    /// per-pair subsystems were never constructed.
    std::vector<std::string> to_enable;

    /// A pair present on one side only. Never applied live; a new pair has
    /// no subsystems and a removed one still holds seats everywhere.
    std::vector<std::string> structural;
};

/// Compare pair enable-flags by NAME between the running config's pairs and
/// a freshly parsed file's. Anything else the two configs disagree on is
/// outside this function's contract -- the caller reports "restart to
/// apply" wholesale rather than pretending to know which fields matter.
template <typename PairT>
[[nodiscard]] inline ReloadDiff diff_pair_enables(
    const std::vector<PairT>& running,
    const std::vector<PairT>& saved)
{
    ReloadDiff out;
    for (const auto& r : running) {
        bool found = false;
        for (const auto& s : saved) {
            if (s.name != r.name) continue;
            found = true;
            if (r.enabled && !s.enabled) out.to_disable.push_back(r.name);
            if (!r.enabled && s.enabled) out.to_enable.push_back(r.name);
            break;
        }
        if (!found) out.structural.push_back(r.name);
    }
    for (const auto& s : saved) {
        bool found = false;
        for (const auto& r : running) {
            if (r.name == s.name) { found = true; break; }
        }
        if (!found) out.structural.push_back(s.name);
    }
    return out;
}

}  // namespace xop

#endif  // XOP_CONFIG_RELOAD_HPP
