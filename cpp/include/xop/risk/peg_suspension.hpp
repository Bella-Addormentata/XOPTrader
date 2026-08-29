// ---------------------------------------------------------------------------
// peg_suspension.hpp -- asset-level peg failure: detect, latch, re-enable.
//
// [PEGSUSPEND 2026-08-29] The pair-level DepegDetector watches a
// stable/stable pair's OWN mid against its own peg target, which is the
// right instrument for BYC/wUSDC.b and useless for XCH/wUSDC.b -- that mid
// moves with XCH, so the wrapper can lose its peg entirely while every pair
// that prices things IN it looks healthy. warp.green was compromised on
// 2026-08-25 and wUSDC.b depegged; nothing at the asset level noticed.
//
// This is the asset-level half. Each declared, enforced pegged asset is
// observed in USD through a route that DOES NOT pass through its own par
// (usd_per_xch / mid(XCH/asset) -- a par-based observation of the par would
// be circular and blind). Sustained deviation past the asset's bail
// threshold latches a SUSPENSION:
//
//   * valuation stops honouring the declared par (declared_usd_par ->
//     nullopt; the asset values off the market like any unpegged CAT);
//   * every pair whose base OR quote is the asset stops quoting, and its
//     resting offers are cancelled at the transition -- off means flat;
//   * the latch is STICKY. A depeg that heals on the chart is not a bridge
//     that healed; whether the peg is trustworthy again is the operator's
//     judgement, exercised through the GUI's re-enable button, not a
//     counter's.
//
// Pure and total: the decisions live here, testable without an engine; the
// Engine owns the wiring (observation route, cancels, alerts, the
// re-enable flag file).
// ---------------------------------------------------------------------------

#ifndef XOP_RISK_PEG_SUSPENSION_HPP
#define XOP_RISK_PEG_SUSPENSION_HPP

#include <cmath>
#include <cstdint>
#include <string>

namespace xop::risk {

// ---------------------------------------------------------------------------
// Runtime state for one declared asset's peg.
// ---------------------------------------------------------------------------
struct PegRuntime {
    /// Consecutive observations at or past the bail threshold.
    std::uint32_t above_bail{0};

    /// The sticky latch. Set by observe() on a sustained bail, cleared ONLY
    /// by reenable() -- never by the price recovering, because a chart that
    /// heals says nothing about whether the bridge behind the wrapper did.
    bool suspended{false};

    /// Block at which the suspension latched (0 = not suspended). For the
    /// operator-facing display and the audit trail.
    std::uint32_t suspended_at_block{0};

    /// Last observed deviation, percent. For display; never for decisions.
    double last_deviation_pct{0.0};
};

/// What one observation asks the caller to do.
enum class PegObservation {
    Holding,        ///< within warn threshold
    Warn,           ///< past warn, not (yet sustained) past bail
    JustSuspended,  ///< THIS observation latched the suspension -- cancel now
    Suspended,      ///< latch already held; nothing new to do
};

inline const char* to_string(PegObservation o)
{
    switch (o) {
        case PegObservation::Holding:       return "holding";
        case PegObservation::Warn:          return "warn";
        case PegObservation::JustSuspended: return "just_suspended";
        case PegObservation::Suspended:     return "suspended";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// One observation of one asset.
//
// `usd_observed` is the asset's market-derived USD price, from a route that
// does not consult the asset's own par. Non-finite or non-positive
// observations are IGNORED rather than counted in either direction: an
// unpriceable tick is a data gap, not evidence about the peg -- counting it
// as deviation would suspend on an outage, and counting it as health would
// reset a genuine streak with a blind read. The streak simply holds.
//
// `sustained_observations` guards against suspending on one junk print or a
// momentary wick: the deviation must persist. Zero is clamped to one --
// "latch immediately" -- rather than "never latch", because a threshold an
// operator set to zero means now, not never.
// ---------------------------------------------------------------------------
[[nodiscard]] inline PegObservation observe_peg(
    PegRuntime&   rt,
    double        usd_observed,
    double        peg_target,
    double        bail_pct,
    double        warn_pct,
    std::uint32_t sustained_observations,
    std::uint32_t block_height) noexcept
{
    if (rt.suspended) {
        // Keep the display honest while latched, but no state transitions:
        // the latch is the operator's to clear.
        if (std::isfinite(usd_observed) && usd_observed > 0.0
                && peg_target > 0.0) {
            rt.last_deviation_pct =
                std::fabs(usd_observed - peg_target) / peg_target * 100.0;
        }
        return PegObservation::Suspended;
    }

    if (!(std::isfinite(usd_observed) && usd_observed > 0.0)
            || !(std::isfinite(peg_target) && peg_target > 0.0)) {
        return rt.above_bail > 0 ? PegObservation::Warn
                                 : PegObservation::Holding;
    }

    const double dev_pct =
        std::fabs(usd_observed - peg_target) / peg_target * 100.0;
    rt.last_deviation_pct = dev_pct;

    if (dev_pct >= bail_pct) {
        ++rt.above_bail;
        const std::uint32_t needed =
            sustained_observations == 0 ? 1u : sustained_observations;
        if (rt.above_bail >= needed) {
            rt.suspended = true;
            rt.suspended_at_block = block_height;
            return PegObservation::JustSuspended;
        }
        return PegObservation::Warn;
    }

    // Below bail: the streak resets. Warn is stateless -- it is a message,
    // not a latch, and half-latching it would re-create the S17 alert spam.
    rt.above_bail = 0;
    return dev_pct >= warn_pct ? PegObservation::Warn
                               : PegObservation::Holding;
}

// ---------------------------------------------------------------------------
// The operator's re-enable. Clears the latch AND the streak, so detection
// re-arms from zero -- if the asset is still depegged, the very next
// sustained run re-suspends it, which is the safety property that makes the
// button safe to offer at all: re-enabling is never "trust it forever",
// only "look again".
// ---------------------------------------------------------------------------
inline void reenable_peg(PegRuntime& rt) noexcept
{
    rt.suspended = false;
    rt.suspended_at_block = 0;
    rt.above_bail = 0;
}

}  // namespace xop::risk

#endif  // XOP_RISK_PEG_SUSPENSION_HPP
