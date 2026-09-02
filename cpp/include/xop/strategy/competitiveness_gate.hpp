#ifndef XOP_STRATEGY_COMPETITIVENESS_GATE_HPP
#define XOP_STRATEGY_COMPETITIVENESS_GATE_HPP
// ---------------------------------------------------------------------------
// competitiveness_gate.hpp -- the Step 8 competitiveness gate, in ONE place.
//
// [2026-09-02, review] WHY THIS FILE EXISTS
// -----------------------------------------
// The gate's base score was written out twice: once at the site that APPLIES
// it (Step 8, engine.cpp) and once at the site that PUBLISHES it (the Step 12
// strategy-metrics export). The publishing site carried the comment
//
//     "Kept in sync by the assertion in test_strategy_metrics.cpp,
//      not by hope."
//
// and test_strategy_metrics.cpp did not exist. Nothing asserted the mirror.
// A comment claiming a control that is not there is worse than no comment:
// it tells the next reader not to check.
//
// That is exactly the defect the live-strategy-metrics work was undertaken to
// eliminate -- telemetry that contradicts the engine. Retune the base at one
// site and the GUI's "Competitiveness Gate" row, and the Prometheus
// comp_gate_base gauge, silently keep reporting the other, with a green suite
// throughout.
//
// So the constant does not live at two sites any more. It lives here, both
// callers call this, and the value is pinned by static_assert (evaluated at
// BUILD time on GCC, which is the authority for this branch) and by
// test_competitiveness_gate.cpp.
//
// WHAT THE NUMBERS MEAN
// ---------------------
// Tier competitiveness is scored 0-10. A tier posts only if its score is at
// or above the gate. Stablecoin pairs trade inside a sub-1% range and rarely
// score 3+, which historically suppressed 100% of BYC tiers (151 sanity
// rejections in the 0.7.45 audit) -- hence the lower base for them.
//
// The competitiveness PID then offsets this base and the result is clamped
// back into the legal 0-10 range; effective 0 means the gate is fully open.
// Both the offset application and the clamp live here too, for the same
// reason the base does.
// ---------------------------------------------------------------------------

// No includes: the clamp is written out rather than pulled from <algorithm> so
// this header is self-contained and every assertion below is evaluable at
// build time with nothing else parsed.

namespace xop::strategy {

/// Lowest competitiveness score (0-10) at which a tier is allowed to post,
/// before the competitiveness PID's offset is applied.
///
/// MIRROR-FREE: engine.cpp Step 8 and the Step 12 metrics export both call
/// this. Do not re-inline the literal at either site.
constexpr int base_competitiveness_score(bool is_stablecoin) noexcept {
    return is_stablecoin ? 1 : 3;
}

/// The gate actually enforced: base score offset by the competitiveness PID
/// and clamped back into the legal 0-10 range.
///
/// A negative offset (underfilling) lowers the gate so more tiers post; a
/// positive offset (overfilling) raises it. 0 is the neutral state and is
/// common -- the post-creation warm-up window sits there, as does any block
/// where ema_fill_rate is at target.
constexpr int effective_competitiveness_gate(bool is_stablecoin,
                                             int  pid_offset) noexcept {
    const int raw = base_competitiveness_score(is_stablecoin) + pid_offset;
    return raw < 0 ? 0 : (raw > 10 ? 10 : raw);
}

// -- Pinned at build time. GCC evaluates these during compilation, so a
// -- retune that forgets one caller cannot reach CI green.
static_assert(base_competitiveness_score(false) == 3,
              "non-stablecoin competitiveness base is 3");
static_assert(base_competitiveness_score(true) == 1,
              "stablecoin competitiveness base is 1 (BYC suppression audit)");
static_assert(effective_competitiveness_gate(false, 0) == 3,
              "neutral offset must leave the base untouched");
static_assert(effective_competitiveness_gate(false, -3) == 0,
              "the live railed offset opens the gate fully");
static_assert(effective_competitiveness_gate(false, -99) == 0,
              "clamped at the bottom of the legal range");
static_assert(effective_competitiveness_gate(true, 99) == 10,
              "clamped at the top of the legal range");

}  // namespace xop::strategy

#endif  // XOP_STRATEGY_COMPETITIVENESS_GATE_HPP
