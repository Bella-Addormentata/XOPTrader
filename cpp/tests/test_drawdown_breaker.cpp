// test_drawdown_breaker.cpp -- Unit tests for the USD-normalized P&L
// circuit-breaker math ([DRAWDOWN-USD 2026-08-02]).
//
// The mixed-pair scenario uses measured production scales:
//   wUSDC.b quote mojo = $1.00 / 1e3  = $0.001      per mojo
//   DBX     quote mojo = $0.0137 / 1e3 = $0.0000137 per mojo
//   value ratio = 73.0x
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/risk/drawdown_breaker.hpp>

#include <cmath>
#include <limits>

namespace {

using xop::risk::hwm_drawdown_frac;
using xop::risk::window_loss_threshold_usd;
using xop::risk::kWindowAnchorFallbackUsd;

// ============================================================================
// (i) Mixed-pair scenario: the old raw quote-mojo sum misstates drawdown 73x
// ============================================================================
//
// History: the bot builds its peak on XCH/wUSDC.b spread capture of
// +10,000 wUSDC.b quote mojos = $10.00, then takes XCH/DBX losses of
// -219,000 DBX quote mojos = -$3.0003 (219,000 * $0.0000137).
//
// OLD raw-mojo math (what step_check_alerts used to compute):
//   peak_hwm  = 10,000 mojos            (wUSDC.b mojos)
//   total     = 10,000 - 219,000 = -209,000 "mojos"  (mixed units!)
//   drawdown  = (10,000 - (-209,000)) / 10,000 = 21.9   -> 2,190%
//
// TRUE USD math:
//   peak      = $10.00
//   total     = $10.00 - $3.0003 = $6.9997
//   drawdown  = 3.0003 / 10.00 = 0.30003                -> 30.0%
//
// Misstatement factor: 21.9 / 0.30003 = 73.0x -- exactly the DBX/wUSDC.b
// per-mojo value ratio.  At any drawdown threshold in (30%, 2190%) the old
// math pauses a healthy engine; with the pairs swapped it hides a real
// loss spiral instead.

TEST(HwmDrawdownUsdTest, MixedPairScenarioIsUnitCoherent) {
    // The raw-mojo arithmetic the old code performed, reproduced verbatim:
    const double raw_peak_mojos  = 10'000.0;                  // wUSDC.b mojos
    const double raw_total_mojos = 10'000.0 - 219'000.0;      // minus DBX mojos
    const double raw_drawdown =
        (raw_peak_mojos - raw_total_mojos) / raw_peak_mojos;
    EXPECT_NEAR(raw_drawdown, 21.9, 1e-12);   // 2,190% "drawdown"

    // The USD-normalized inputs the code now uses:
    const double peak_usd  = 10.00;                  // wUSDC.b profits
    const double loss_usd  = 219'000.0 * 0.0000137;  // DBX loss = $3.0003
    const double total_usd = peak_usd - loss_usd;    // $6.9997

    const double usd_drawdown = hwm_drawdown_frac(peak_usd, total_usd);
    EXPECT_NEAR(usd_drawdown, 0.30003, 1e-9);        // 30.0%, the truth

    // The misstatement factor is the per-mojo value ratio (73x).
    EXPECT_NEAR(raw_drawdown / usd_drawdown, 73.0, 0.01);
}

// ============================================================================
// (ii) The breaker trips at the configured threshold in USD terms
// ============================================================================

TEST(HwmDrawdownUsdTest, TripsAtConfiguredUsdThreshold) {
    const double peak_usd  = 10.00;
    const double total_usd = 6.9997;   // the 30.0% scenario above
    const double frac = hwm_drawdown_frac(peak_usd, total_usd);

    // Mirrors the engine's gate: drawdown_frac > max_drawdown_pct_.
    EXPECT_GT(frac, 0.25);   // trips a 25% threshold
    EXPECT_GT(frac, 0.10);   // trips the 10% default
    EXPECT_LT(frac, 0.35);   // does NOT trip a 35% threshold

    // Exact-threshold boundary: strictly-greater comparison, no trip at
    // exactly the threshold.
    EXPECT_FALSE(hwm_drawdown_frac(10.0, 7.0) > 0.30);
    EXPECT_TRUE(hwm_drawdown_frac(10.0, 6.99) > 0.30);
}

TEST(HwmDrawdownUsdTest, NeverProfitableEdgeCases) {
    // [MEDIUM-7] Losing from a zero/negative peak is full drawdown.
    EXPECT_DOUBLE_EQ(hwm_drawdown_frac(0.0, -0.01), 1.0);
    EXPECT_DOUBLE_EQ(hwm_drawdown_frac(-2.0, -3.0), 1.0);
    // Flat or recovering from a non-positive peak is no drawdown.
    EXPECT_DOUBLE_EQ(hwm_drawdown_frac(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(hwm_drawdown_frac(-2.0, 0.5), 0.0);
    // NaN poisoning fails closed (no spurious trip).
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_DOUBLE_EQ(hwm_drawdown_frac(nan, -1.0), 1.0);   // total<0 branch
    EXPECT_DOUBLE_EQ(hwm_drawdown_frac(nan, 1.0), 0.0);
}

// ============================================================================
// (iii) Rolling-window threshold: peak anchor and the early-run fallback
// ============================================================================

TEST(WindowLossThresholdUsdTest, AnchorsToUsdPeakWhenProfitable) {
    // Profitable bot: threshold scales with the USD peak.
    // $10 peak at 500 bps -> $0.50.
    EXPECT_NEAR(window_loss_threshold_usd(10.0, kWindowAnchorFallbackUsd,
                                          500.0), 0.50, 1e-12);
    // A $0.60 window loss trips, $0.40 does not (engine gate is
    // window_loss > threshold).
    EXPECT_GT(0.60, window_loss_threshold_usd(10.0, 1.50, 500.0));
    EXPECT_LT(0.40, window_loss_threshold_usd(10.0, 1.50, 500.0));
}

TEST(WindowLossThresholdUsdTest, EarlyRunFallbackAnchorPath) {
    // Never-profitable (peak <= 0): the anchor falls back to the 1-XCH USD
    // nominal the engine passes in.
    //
    // Live-market case: usd_per_xch() = $1.39 -> 500 bps = $0.0695.
    EXPECT_NEAR(window_loss_threshold_usd(0.0, 1.39, 500.0), 0.0695, 1e-12);
    // Cold-market case: the fixed conservative nominal $1.50 -> $0.075.
    EXPECT_NEAR(window_loss_threshold_usd(0.0, kWindowAnchorFallbackUsd,
                                          500.0), 0.075, 1e-12);
    EXPECT_DOUBLE_EQ(kWindowAnchorFallbackUsd, 1.50);

    // The old anchor was "1 XCH nominal" in MOJOS (1e12): against a USD
    // window series that threshold (500 bps of 1e12 = 5e10) could never
    // fire on dollar-scale losses.  The USD fallback restores the intended
    // scale: a $0.10 early-run loss trips a $0.075 threshold.
    EXPECT_GT(0.10, window_loss_threshold_usd(0.0, 1.50, 500.0));

    // Negative peak also takes the fallback.
    EXPECT_NEAR(window_loss_threshold_usd(-3.0, 1.50, 500.0), 0.075, 1e-12);
}

TEST(WindowLossThresholdUsdTest, DisabledAndDegenerateInputs) {
    // bps = 0 disables (threshold 0; engine also gates on threshold > 0).
    EXPECT_DOUBLE_EQ(window_loss_threshold_usd(10.0, 1.50, 0.0), 0.0);
    // No usable anchor at all -> 0 -> engine treats as disabled this cycle.
    EXPECT_DOUBLE_EQ(window_loss_threshold_usd(0.0, 0.0, 500.0), 0.0);
    EXPECT_DOUBLE_EQ(window_loss_threshold_usd(-1.0, -2.0, 500.0), 0.0);
    // NaN bps fails closed.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_DOUBLE_EQ(window_loss_threshold_usd(10.0, 1.50, nan), 0.0);
}

}  // namespace
