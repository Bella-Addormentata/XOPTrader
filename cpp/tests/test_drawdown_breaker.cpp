// test_drawdown_breaker.cpp -- Unit tests for the equity-based circuit-
// breaker math ([DRAWDOWN-USD 2026-08-02], [DRAWDOWN-EQUITY 2026-08-04]).
//
// The headline scenario is the live false trip of 2026-08-04 04:14:
// XCH retraced ~5% overnight ($1.575 -> $1.498) with ~54 XCH held on a
// ~$158 portfolio whose accumulated P&L peak was only ~$25.  The
// unrealized mark move (~-$8) was ~5% of EQUITY but 32-60% of the P&L
// peak, so the old P&L-anchored breaker paused a healthy engine (the
// inventory basis $1.4711 sat BELOW the $1.4984 mid).
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/config.hpp>
#include <xop/risk/drawdown_breaker.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using namespace std::chrono_literals;
using xop::risk::equity_drawdown_frac;
using xop::risk::window_loss_threshold_usd;
using xop::risk::portfolio_equity_usd;
using xop::risk::effective_usd_per_unit;
using xop::risk::AssetValuationInput;
using xop::risk::BreakerRealertGate;
using xop::risk::kWindowAnchorFallbackUsd;

// ============================================================================
// (a) The live 2026-08-04 04:14 scenario: OLD math fired, NEW math must not
// ============================================================================
//
// Portfolio at the overnight peak (XCH $1.575):
//   54 XCH x $1.575 = $85.05, plus ~$72.95 of stables/CATs -> $158.00 equity
// P&L accumulators: high-water mark ~$25.
// Overnight XCH retrace to $1.498 moves the marks ~-$8: equity $150.00.
//
// OLD math (drawdown against the P&L HWM):
//   -$8 swing:            8 / 25 = 32.0%   } both blow through the
//   live-logged episode: 15 / 25 = 60.0%   } configured 5% threshold
//   -> PAUSE on a healthy book.  The denominator is accumulated PROFIT,
//   so the same dollar wiggle reads larger the less profit the bot has
//   banked -- backwards as a portfolio-risk control.
//
// NEW math (drawdown against portfolio equity):
//   (158.00 - 150.00) / 158.00 = 8 / 158 = 5.063%
//   -> comfortably below the recalibrated 10% threshold: no trip.

TEST(EquityDrawdownTest, LiveFalseTripScenarioReadsFivePercentNotSixty) {
    // The OLD arithmetic, reproduced verbatim so the failure mode stays
    // documented: P&L peak $25, mark swings of -$8 and -$15.
    const double old_frac_8  = (25.0 - 17.0) / 25.0;   // 0.32
    const double old_frac_15 = (25.0 - 10.0) / 25.0;   // 0.60
    EXPECT_NEAR(old_frac_8, 0.32, 1e-12);
    EXPECT_NEAR(old_frac_15, 0.60, 1e-12);
    EXPECT_GT(old_frac_8, 0.05);    // fired the configured 5%
    EXPECT_GT(old_frac_15, 0.05);   // the live log's "drawdown 60%"

    // The NEW arithmetic on the same book.
    const double peak_equity = 158.00;
    const double equity_now  = 150.00;   // -$8 of unrealized marks
    const double frac = equity_drawdown_frac(peak_equity, equity_now);
    EXPECT_NEAR(frac, 8.0 / 158.0, 1e-12);       // 5.063%
    EXPECT_NEAR(frac, 0.050633, 1e-6);

    // Recalibrated threshold: 10% of equity.  A ~5% overnight retrace is
    // normal volatility, not an emergency.
    const xop::RiskConfig defaults{};
    EXPECT_DOUBLE_EQ(defaults.max_drawdown_frac, 0.10);
    EXPECT_FALSE(frac > defaults.max_drawdown_frac);   // engine gate: no trip
}

// ============================================================================
// (b) A genuine 12% equity drop DOES trip the 10% threshold
// ============================================================================

TEST(EquityDrawdownTest, GenuineTwelvePercentEquityDropTrips) {
    // $158.00 peak -> $139.04: (158 - 139.04)/158 = 12.0%.
    const double frac = equity_drawdown_frac(158.00, 139.04);
    EXPECT_NEAR(frac, 0.12, 1e-9);
    EXPECT_TRUE(frac > xop::RiskConfig{}.max_drawdown_frac);   // trips

    // Boundary discipline: strictly-greater, so exactly-10% does not trip.
    EXPECT_FALSE(equity_drawdown_frac(100.0, 90.0) > 0.10);
    EXPECT_TRUE(equity_drawdown_frac(100.0, 89.99) > 0.10);
}

TEST(EquityDrawdownTest, PreValuationAndDegenerateInputs) {
    // Nothing valued yet (equity is non-negative by construction, so a
    // non-positive peak means no valuations): no measurable drawdown.
    EXPECT_DOUBLE_EQ(equity_drawdown_frac(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(equity_drawdown_frac(-1.0, 5.0), 0.0);
    // NaN fails closed.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_DOUBLE_EQ(equity_drawdown_frac(nan, 100.0), 0.0);
}

// ============================================================================
// (c) A conversion vanishing mid-cycle must NOT fake a drawdown
// ============================================================================

TEST(EquityValuationTest, MissingConversionCarriesLastKnownValue) {
    // The live book: 54 XCH, ~$65 of wUSDC.b, ~978.6 DBX.
    const std::vector<AssetValuationInput> all_live = {
        {54.0, 1.498, 1.575},        // XCH: live price, stale cache entry
        {65.0, 1.0, 1.0},            // wUSDC.b
        {978.576, 0.0137, 0.0137},   // DBX
    };
    const double equity_live = portfolio_equity_usd(all_live);
    // 54*1.498 + 65 + 978.576*0.0137 = 80.892 + 65 + 13.4065 = 159.2985
    EXPECT_NEAR(equity_live, 159.2985, 1e-3);

    // Same cycle, but the DBX conversion vanishes (empty book / cold
    // feed): live = 0, last-known carries the position.
    const std::vector<AssetValuationInput> dbx_gap = {
        {54.0, 1.498, 1.575},
        {65.0, 1.0, 1.0},
        {978.576, 0.0, 0.0137},      // live conversion GONE
    };
    // Equity is IDENTICAL -- the $13.41 of DBX does not evaporate, so the
    // breaker sees 0% drawdown instead of a fake -8.4% cliff.
    EXPECT_NEAR(portfolio_equity_usd(dbx_gap), equity_live, 1e-9);
    EXPECT_DOUBLE_EQ(
        equity_drawdown_frac(equity_live, portfolio_equity_usd(dbx_gap)),
        0.0);

    // An asset never valued at all contributes nothing (and never inflated
    // the peak either).
    const std::vector<AssetValuationInput> never_valued = {
        {54.0, 1.498, 1.575},
        {1000.0, 0.0, 0.0},          // no live, no history
    };
    EXPECT_NEAR(portfolio_equity_usd(never_valued), 54.0 * 1.498, 1e-9);

    // Guards: non-positive or NaN units contribute 0.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_DOUBLE_EQ(portfolio_equity_usd({{-5.0, 1.0, 1.0}}), 0.0);
    EXPECT_DOUBLE_EQ(portfolio_equity_usd({{nan, 1.0, 1.0}}), 0.0);
    // Price selection: live wins over last; NaN live falls to last.
    EXPECT_DOUBLE_EQ(effective_usd_per_unit(1.498, 1.575), 1.498);
    EXPECT_DOUBLE_EQ(effective_usd_per_unit(0.0, 1.575), 1.575);
    EXPECT_DOUBLE_EQ(effective_usd_per_unit(nan, 1.575), 1.575);
    EXPECT_DOUBLE_EQ(effective_usd_per_unit(nan, 0.0), 0.0);
}

// ============================================================================
// (d) Alert suppression: one alert per episode, re-raised on the interval
// ============================================================================

TEST(BreakerRealertGateTest, EmitsOncePerIntervalNotPerCycle) {
    BreakerRealertGate gate;
    const auto t0 = std::chrono::steady_clock::time_point{} + 1000s;
    const auto interval = 30min;   // risk.breaker_realert_minutes default

    // First trip alerts immediately.
    EXPECT_TRUE(gate.should_alert(t0, interval));

    // The condition persists every ~10-30 s while paused (the measured
    // 04:14 spam cadence): every one of those cycles is suppressed.
    for (int cycle = 1; cycle <= 60; ++cycle) {
        EXPECT_FALSE(gate.should_alert(t0 + cycle * 20s, interval))
            << "cycle " << cycle;
    }

    // After the re-alert interval the CRITICAL alert is raised again...
    EXPECT_TRUE(gate.should_alert(t0 + 30min, interval));
    // ...and suppression resumes.
    EXPECT_FALSE(gate.should_alert(t0 + 30min + 20s, interval));

    // When the condition clears, the gate re-arms: a NEW episode alerts
    // immediately even if the interval has not elapsed.
    gate.clear();
    EXPECT_TRUE(gate.should_alert(t0 + 31min, interval));
}

TEST(BreakerRealertGateTest, ConfigDefaultIsThirtyMinutes) {
    EXPECT_EQ(xop::RiskConfig{}.breaker_realert_minutes, 30u);
}

// ============================================================================
// Rolling-window loss threshold: equity anchor + early-run fallback
// ============================================================================

TEST(WindowLossThresholdUsdTest, AnchorsToPortfolioEquity) {
    // The live book: 250 bps of ~$150 equity = $3.75 -- vs the $1.09 the
    // retired |P&L-HWM| anchor produced when it tripped spuriously on the
    // 08-02 overnight mark wiggle (250 bps of the ~$43.6 P&L figure it
    // was fed).
    const double threshold = window_loss_threshold_usd(
        150.0, kWindowAnchorFallbackUsd, 250.0);
    EXPECT_NEAR(threshold, 3.75, 1e-12);

    // Engine gate is loss > threshold: a $3.00 window loss no longer
    // trips; a $4.00 one still does.
    EXPECT_FALSE(3.00 > threshold);
    EXPECT_TRUE(4.00 > threshold);
}

TEST(WindowLossThresholdUsdTest, EarlyRunFallbackAnchorPath) {
    // No equity valued yet: anchor falls back to the live 1-XCH USD value
    // the engine passes in (usd_per_xch()), then the fixed nominal.
    EXPECT_NEAR(window_loss_threshold_usd(0.0, 1.498, 250.0),
                0.03745, 1e-9);
    EXPECT_NEAR(window_loss_threshold_usd(0.0, kWindowAnchorFallbackUsd,
                                          500.0), 0.075, 1e-12);
    EXPECT_DOUBLE_EQ(kWindowAnchorFallbackUsd, 1.50);
    // A $0.10 early-run loss trips the $0.075 fallback threshold.
    EXPECT_TRUE(0.10 > window_loss_threshold_usd(0.0, 1.50, 500.0));
    // Negative "equity" (impossible, but fail-safe) takes the fallback.
    EXPECT_NEAR(window_loss_threshold_usd(-3.0, 1.50, 500.0), 0.075, 1e-12);
}

TEST(WindowLossThresholdUsdTest, DisabledAndDegenerateInputs) {
    // bps = 0 disables (threshold 0; engine also gates on threshold > 0).
    EXPECT_DOUBLE_EQ(window_loss_threshold_usd(150.0, 1.50, 0.0), 0.0);
    // No usable anchor at all -> 0 -> engine treats as disabled this cycle.
    EXPECT_DOUBLE_EQ(window_loss_threshold_usd(0.0, 0.0, 250.0), 0.0);
    EXPECT_DOUBLE_EQ(window_loss_threshold_usd(-1.0, -2.0, 250.0), 0.0);
    // NaN bps fails closed.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_DOUBLE_EQ(window_loss_threshold_usd(150.0, 1.50, nan), 0.0);
}

}  // namespace
