// reservation_offset.hpp -- Pure Avellaneda-Stoikov reservation-price offset
// for the Step 7 ladder centre.
//
// [AS-RES 2026-08-01] The A-S reservation price r = S - q*gamma*sigma^2*tau
// was computed every heartbeat in Step 4 and then DISCARDED: Step 7 built
// the ladder around the market mid and read only .bid_size/.ask_size from
// the risk quote.  The measured consequence (2026-07-31): ~116 XCH held
// against an ~80 XCH target with no selling lean anywhere in the posted
// ladder.  This header expresses the inventory term as a dimensionless RATE
// applied multiplicatively to the ladder centre, so the bot leans its whole
// book toward reducing imbalance:
//
//     centre' = centre * (1 - rate),   rate = q * gamma * sigma^2 * tau
//
// DIMENSIONAL ANALYSIS (every factor, so this cannot regress the way the
// daily-sigma-times-sqrt(seconds) mix previously flagged in
// spread.cpp:372-378 did):
//
//     q            [1]         signed inventory imbalance normalized to the
//                              pair's target ratio; +1 = holding 2x target.
//     gamma        [1]         risk aversion, dimensionless IN THESE
//                              NORMALIZED UNITS (classical A-S gamma carries
//                              1/price units because its q is in lots and
//                              its sigma in absolute price; ours are both
//                              relative, so gamma absorbs that scale).
//     sigma        [1/sqrt(yr)] ANNUALIZED log-return volatility.
//     sigma^2      [1/yr]
//     tau          [yr]        quote-refresh horizon as a fraction of a
//                              year (~19 min heartbeat = 1140 s / 31.536e6 s
//                              = 3.615e-5 yr).
//     sigma^2*tau  [1]         log-return VARIANCE over one refresh horizon.
//     rate         [1]         dimensionless -- a relative price shift.
//
// MEASURED MAGNITUDES (2026-07-31 state, gamma = 1000):
//     sigma = 1.11  ->  sigma^2*tau = 1.2321 * 3.615e-5 = 4.454e-5
//                       (1-sigma over 19 min = sqrt = 0.667%)
//     q = (116 - 80) / 80 = 0.45
//     raw rate = 0.45 * 1000 * 4.454e-5 = 2.004e-2  (200.4 bps)
//     -> capped at the owner-approved 100 bps rail; centre shifts DOWN 1%,
//        so bids AND asks shift down: the book leans to sell excess base.
//     At sigma = 0.5:  raw = 40.7 bps (uncapped).
//     At the 0.001 sigma floor (pair with no history): raw rate = 1.6e-8
//     (0.00016 bps) -- indistinguishable from zero, i.e. cold pairs degrade
//     to today's behaviour with no cliff.
//     For contrast, reusing strategy.gamma (0.003) here would give
//     0.0006 bps -- decorative, which is why as_reservation_gamma is a
//     separate, separately-documented calibration (config.hpp).
//
// Compliant with:
//   ISO/IEC 5055  -- no UB: NaN-guarded, clamped inputs, pure function
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
//                    (tests/test_reservation_offset.cpp)

#ifndef XOP_STRATEGY_RESERVATION_OFFSET_HPP
#define XOP_STRATEGY_RESERVATION_OFFSET_HPP

namespace xop::strategy {

/// Result of the reservation-offset computation.  `rate` is the bounded
/// dimensionless shift to apply as centre' = centre * (1 - rate); positive
/// rate (long base) moves the whole ladder DOWN (selling pressure), negative
/// rate (short base) moves it UP (buying pressure).
struct ReservationOffset {
    double q{0.0};         ///< Signed imbalance normalized to target.
    double raw_rate{0.0};  ///< Unbounded q * gamma * sigma^2 * tau.
    double rate{0.0};      ///< raw_rate clamped to +/- max_offset_bps.
    bool   capped{false};  ///< True when the rail bound the rate.
};

/// Signed inventory imbalance normalized to the pair's target ratio:
///
///     q = (inv_ratio - ratio_target) / ratio_target
///
/// With base holdings H valued against a target T at fixed portfolio value,
/// this equals H/T - 1 (e.g. 116 XCH vs 80 target -> q = +0.45).  Positive
/// = long base.  Defensive rails: inv_ratio outside [0,1] is clamped, a
/// degenerate target returns 0, and |q| is capped at 4 so a tiny target
/// cannot manufacture an enormous imbalance (the offset cap below is the
/// real bound; this just keeps intermediate values sane).
[[nodiscard]] constexpr double normalized_imbalance(
    double inv_ratio, double ratio_target) noexcept
{
    // NaN-guard (NaN != NaN) and degenerate-target guard.
    if (!(inv_ratio == inv_ratio)) return 0.0;
    if (!(ratio_target > 0.0) || !(ratio_target < 1.0)) return 0.0;

    if (inv_ratio < 0.0) inv_ratio = 0.0;
    if (inv_ratio > 1.0) inv_ratio = 1.0;

    double q = (inv_ratio - ratio_target) / ratio_target;
    if (q > 4.0)  q = 4.0;
    if (q < -4.0) q = -4.0;
    return q;
}

/// The bounded A-S reservation offset (see file header for the dimensional
/// analysis and measured magnitudes).  Any non-positive or non-finite
/// gamma / sigma / tau / cap yields a zero offset -- the ladder centre is
/// then exactly what it is today.
[[nodiscard]] constexpr ReservationOffset reservation_offset(
    double inv_ratio,
    double ratio_target,
    double gamma,
    double sigma_annual,
    double tau_years,
    double max_offset_bps) noexcept
{
    ReservationOffset out{};
    if (!(gamma > 0.0) || !(sigma_annual > 0.0)
        || !(tau_years > 0.0) || !(max_offset_bps > 0.0)) {
        return out;
    }

    out.q        = normalized_imbalance(inv_ratio, ratio_target);
    out.raw_rate = out.q * gamma * sigma_annual * sigma_annual * tau_years;
    out.rate     = out.raw_rate;

    const double cap = max_offset_bps / 10'000.0;
    if (out.rate > cap)  { out.rate = cap;  out.capped = true; }
    if (out.rate < -cap) { out.rate = -cap; out.capped = true; }
    return out;
}

/// Minimum |rate| that is actually applied to the ladder centre: 1e-6
/// (0.01 bps).  Below this the shift is an EXACT no-op (bitwise-identical
/// centre), so sigma-floor pairs degrade to precisely today's behaviour --
/// no epsilon drift, no cliff.
inline constexpr double kMinAppliedOffsetRate = 1e-6;

/// THE application formula Step 7 posts:
///
///     centre' = centre * (1 - rate)
///
/// Positive rate (long base) moves the whole ladder DOWN (selling
/// pressure); negative rate (short base) moves it UP (buying pressure).
/// The engine (engine.cpp step_generate_ladder, [AS-RES]) calls this
/// function rather than re-implementing the arithmetic, so the direction
/// tests in tests/test_reservation_offset.cpp guard the exact code path
/// that reaches the posted ladder -- a sign flip here fails the suite
/// instead of shipping an inventory AMPLIFIER.
///
/// Non-positive or non-finite centre, or |rate| below
/// kMinAppliedOffsetRate (including NaN rate), returns centre unchanged.
[[nodiscard]] constexpr double apply_reservation_offset(
    double centre, const ReservationOffset& ro) noexcept
{
    if (!(centre > 0.0)) return centre;
    // NaN-safe threshold: a NaN rate satisfies neither comparison.
    if (!(ro.rate >= kMinAppliedOffsetRate)
        && !(ro.rate <= -kMinAppliedOffsetRate)) {
        return centre;
    }
    return centre * (1.0 - ro.rate);
}

}  // namespace xop::strategy

#endif  // XOP_STRATEGY_RESERVATION_OFFSET_HPP
