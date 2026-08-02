// drawdown_breaker.hpp -- Pure decision math for the two P&L circuit
// breakers in Engine::step_check_alerts ([DRAWDOWN-USD 2026-08-02]).
//
// UNITS.  Every P&L figure here is USD (double).  The breakers previously
// consumed PnLSummary::total_pnl, a RAW SUM OF QUOTE-ASSET MOJOS across
// pairs with different quote currencies (a DBX mojo is worth ~1/73rd of a
// wUSDC.b mojo: $0.0137/1e3 vs $1.00/1e3 per mojo).  All three consumers
// are ratio-based, so units cancel only when the numerator and denominator
// are the SAME unit -- but a peak built on wUSDC.b profits divided into a
// drawdown containing DBX losses mixes units and misstates the fraction by
// the full ~73x value ratio (worked example in
// tests/test_drawdown_breaker.cpp: measured-scale fills give a true 30%
// USD drawdown that the raw sum reports as 2,190%).  Either failure
// direction is live: a spurious pause during a healthy rally, or a real
// loss spiral hidden inside a cheap-mojo pair.  The USD-normalized
// total_pnl_usd (PNL-USD-TOTALS 2026-08-01) is the unit-coherent series;
// these helpers exist so the arithmetic is pinned by tests and cannot
// half-revert to mojos at one call site.
//
// Compliant with:
//   ISO/IEC 5055  -- pure, NaN-guarded, division-by-zero-guarded
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation

#ifndef XOP_RISK_DRAWDOWN_BREAKER_HPP
#define XOP_RISK_DRAWDOWN_BREAKER_HPP

namespace xop::risk {

/// High-water-mark drawdown fraction, USD in / dimensionless out:
///
///     peak > 0 :  (peak - total) / peak
///     peak <= 0:  1.0 when total < 0 (losing from a never-profitable
///                 baseline counts as full drawdown -- MEDIUM-7), else 0.0.
///
/// The caller compares against max_drawdown_pct and applies the startup
/// grace window; this function is just the unit-critical arithmetic.
/// NaN inputs fail every comparison and land in the 0.0 branch (no trip).
[[nodiscard]] constexpr double hwm_drawdown_frac(
    double peak_pnl_usd, double total_pnl_usd) noexcept
{
    if (peak_pnl_usd > 0.0) {
        return (peak_pnl_usd - total_pnl_usd) / peak_pnl_usd;
    }
    return (total_pnl_usd < 0.0) ? 1.0 : 0.0;
}

/// Rolling-window loss threshold in USD:
///
///     anchor = peak_pnl_usd            when the bot has been profitable,
///              anchor_fallback_usd     otherwise (early-run case).
///     threshold = anchor * max_window_loss_bps / 10,000
///
/// The fallback replaces the old "1 XCH nominal" (kMojosPerXch) anchor:
/// the engine passes the LIVE 1-XCH USD value (usd_per_xch()) when market
/// data is warm, and a conservative fixed nominal otherwise -- see
/// kWindowAnchorFallbackUsd.  Non-positive inputs yield a 0 threshold,
/// which the caller treats as "check disabled this cycle" (matching the
/// old threshold_mojos > 0 gate).
[[nodiscard]] constexpr double window_loss_threshold_usd(
    double peak_pnl_usd,
    double anchor_fallback_usd,
    double max_window_loss_bps) noexcept
{
    const double anchor =
        (peak_pnl_usd > 0.0)        ? peak_pnl_usd
      : (anchor_fallback_usd > 0.0) ? anchor_fallback_usd
                                    : 0.0;
    if (!(max_window_loss_bps > 0.0)) return 0.0;   // NaN-safe
    return anchor * max_window_loss_bps / 10'000.0;
}

/// Fixed nominal for the window-loss anchor when the live 1-XCH USD value
/// is unavailable (usd_per_xch() == 0: cold market data, no XCH/stable
/// book).  $1.50 is deliberately CONSERVATIVE: near the 2026-08 spot
/// (~$1.39-1.48) and far below the retired 2.70 constant, and a LOWER
/// anchor means a LOWER loss threshold -- the breaker fires earlier, the
/// safe failure direction for a nominal that only matters in the first
/// cycles of a never-profitable run.
inline constexpr double kWindowAnchorFallbackUsd = 1.50;

}  // namespace xop::risk

#endif  // XOP_RISK_DRAWDOWN_BREAKER_HPP
