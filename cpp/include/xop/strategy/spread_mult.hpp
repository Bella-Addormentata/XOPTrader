#ifndef XOP_STRATEGY_SPREAD_MULT_HPP
#define XOP_STRATEGY_SPREAD_MULT_HPP
// ---------------------------------------------------------------------------
// spread_mult.hpp -- "how much wider or tighter than baseline are we quoting?"
//
// [2026-09-02, review] WHY THIS IS NOT A PRODUCT OF MULTIPLIERS
// -------------------------------------------------------------
// The live-strategy-metrics work published a gauge named
// `effective_spread_mult`, computed as
//
//     analysis_spread_mult * (spread_pid_active ? spread_pid_mult : 1.0)
//
// and documented -- in the exporter, and in the GUI row label "Effective
// Multiplier (x)" -- as the multiplier actually applied to the spread.
//
// IT IS NOT. Step 5 touches total_spread_bps at TEN sites. Eight are
// multiplications (niche premium, warm-up defensive, whale x VPIN x OFI,
// startup analysis, staleness, divergence, spread PID, chia edge) and the
// gauge captured two of them. The remaining two sites are ASSIGNMENTS:
//
//     the order-book tactician:      total_spread_bps = adj.spread_bps;
//     the global half-spread cap:    total_spread_bps = max_hs * 2.0;
//
// Either one DISCARDS every multiplier that ran before it, both captured
// factors included. When the cap binds, the GUI would read e.g. 1.230x while
// the posted spread was the capped 500 bps, unrelated to that number. That is
// the exact defect class the live-metrics work exists to eliminate --
// telemetry that contradicts the engine -- re-created behind a comment
// asserting the gauge was the authoritative one.
//
// So the applied multiplier is not derived from the chain at all. It is
// MEASURED, from the two numbers that bracket the chain: the spread the
// optimizer produced before any adjustment, and the spread Step 5 finished
// with. Whatever is added to, removed from, or reordered inside the chain --
// multiplication, assignment, clamp, a site nobody has written yet -- this
// ratio follows it, because it never looks at the chain.
//
// WHY 0 AND NOT 1 WHEN IT CANNOT BE COMPUTED
// ------------------------------------------
// 1.0 is a legal, extremely common value meaning "quoting at baseline". A
// fallback of 1.0 would report a healthy neutral reading for a block where
// the quote was never computed -- an unobservable failure wearing the most
// reassuring value in the range. This is the fail-open shape documented
// across this codebase, so the sentinel is 0.0, which is not a reachable
// multiplier, and consumers render it as "no reading".
// ---------------------------------------------------------------------------

namespace xop::strategy {

/// Largest spread, in bps, this function will treat as a real reading.
/// 1e9 bps is 10,000,000% -- nothing the optimizer, the multiplier chain or
/// the half-spread cap can legitimately produce. Values at or beyond it (and
/// infinities, which fail the comparison) are rejected rather than published.
inline constexpr double kMaxPlausibleSpreadBps = 1.0e9;

/// The multiplier Step 5 ACTUALLY applied: final spread / pre-adjustment
/// spread. Returns 0.0 -- never 1.0 -- when there is no reading, i.e. when
/// either input is non-positive, NaN (all comparisons false), or implausibly
/// large.
///
/// `base_bps`    SpreadOptimizer::compute_spread()'s total_spread_bps, captured
///               before the first adjustment site.
/// `applied_bps` total_spread_bps at the end of Step 5, after every multiplier,
///               the tactician's assignment and the global half-spread cap.
constexpr double applied_spread_mult(double base_bps,
                                     double applied_bps) noexcept {
    if (!(base_bps > 0.0) || !(base_bps < kMaxPlausibleSpreadBps))    return 0.0;
    if (!(applied_bps > 0.0) || !(applied_bps < kMaxPlausibleSpreadBps)) return 0.0;
    return applied_bps / base_bps;
}

// -- Pinned at build time; GCC evaluates these during compilation.
static_assert(applied_spread_mult(100.0, 100.0) == 1.0,
              "unchanged spread is 1.0x");
static_assert(applied_spread_mult(100.0, 50.0) == 0.5,
              "halved spread is 0.5x");
static_assert(applied_spread_mult(100.0, 250.0) == 2.5,
              "widened spread is 2.5x");
static_assert(applied_spread_mult(0.0, 100.0) == 0.0,
              "no base => no reading, and the sentinel is 0 not 1");
static_assert(applied_spread_mult(100.0, 0.0) == 0.0,
              "no applied value => no reading");
static_assert(applied_spread_mult(-1.0, 100.0) == 0.0,
              "negative base is not a reading");

}  // namespace xop::strategy

#endif  // XOP_STRATEGY_SPREAD_MULT_HPP
