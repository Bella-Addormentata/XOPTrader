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
using xop::risk::unvaluable_book_must_fail_closed;
using xop::risk::unpriced_asset_is_written_off;
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

// ===========================================================================
// [S27 2026-08-27] The inert-breaker hazard
//
// These do not test the engine-side fix (marking a never-valued asset as
// degrading the cycle) -- that needs Engine plus an inventory tracker and is
// blocked on the T4-07 harness.  They pin the PURE property that makes the
// engine-side bug dangerous, so the hazard is documented where the maths
// lives rather than only in a TODO.
//
// On 2026-08-25, disabling the wUSDC.b pairs left every held asset without
// an enabled pricing pair.  Equity became exactly $0, the peak tracked to
// $0 with it, and the breaker stopped being able to fire AT ALL -- not a
// nuisance trip, silence.
// ===========================================================================

TEST(DrawdownBreakerTest, S27_ZeroPeakMakesTheBreakerInertNotTrigger) {
    // The specific shape of the 2026-08-25 failure: nothing priced, so both
    // equity and peak are 0.  A non-positive peak yields 0.0 drawdown, so
    // no threshold can ever be crossed.
    EXPECT_DOUBLE_EQ(equity_drawdown_frac(0.0, 0.0), 0.0);

    // And it stays inert however bad things get, because the peak never
    // rose above zero to measure against.
    EXPECT_DOUBLE_EQ(equity_drawdown_frac(0.0, -5000.0), 0.0);
}

TEST(DrawdownBreakerTest, S27_APricedPeakStillProtectsAgainstACollapseToZero) {
    // Contrast: once ANY real peak exists, a collapse to zero reads as a
    // full 100% drawdown and trips everything.  The danger is not a wrong
    // number -- it is having no number at all.
    EXPECT_DOUBLE_EQ(equity_drawdown_frac(500.0, 0.0), 1.0);
}

TEST(DrawdownBreakerTest, S27_UnvaluedAssetsSumToZeroEquity) {
    // Held units with neither a live nor a carried price contribute
    // nothing, so a book full of them is indistinguishable from an empty
    // one at this layer.  That is correct for the pure function -- which is
    // exactly why the ENGINE must flag the condition instead of letting it
    // pass as a healthy $0.
    std::vector<AssetValuationInput> book;
    AssetValuationInput xch;   xch.units = 70.0;    // no prices at all
    AssetValuationInput dbx;   dbx.units = 519.0;
    book.push_back(xch);
    book.push_back(dbx);
    EXPECT_DOUBLE_EQ(portfolio_equity_usd(book), 0.0);

    // One priced asset is enough to make the total non-zero -- the $63.82
    // equity observed live was exactly this: one asset counted, the rest
    // silently absent.
    AssetValuationInput byc;
    byc.units = 63.818;
    byc.live_usd_per_unit = 1.0;
    book.push_back(byc);
    EXPECT_NEAR(portfolio_equity_usd(book), 63.818, 1e-9);
}

// ===========================================================================
// [S27 2026-08-27] Fail closed when the book cannot be valued
//
// Review finding 117-2: setting valuation_degraded_ makes the engine KNOW it
// is blind, but freezing a peak that is already zero leaves the breakers
// exactly as inert as before.  The first version of the S27 fix therefore
// did not close its own stated hole.  These pin the decision that does.
// ===========================================================================

TEST(DrawdownBreakerTest, S27_FailsClosedWhenDegradedWithNoPeakEverEstablished) {
    // The 2026-08-25 shape on a fresh process: nothing priced, so no peak
    // was ever seeded and no drawdown can ever be measured.
    EXPECT_TRUE(unvaluable_book_must_fail_closed(
        /*grace_elapsed=*/true, /*valuation_degraded=*/true,
        /*all_held_assets_unpriced=*/false, /*peak_equity_usd=*/0.0));
}

TEST(DrawdownBreakerTest, S27_DoesNotFailClosedWhileAValidPeakExists) {
    // A frozen peak is still a real reference, and the ordinary comparison
    // keeps protecting us.  Pausing here would be a false positive on a
    // book that is measurably fine.
    EXPECT_FALSE(unvaluable_book_must_fail_closed(true, true, false, 500.0));
}

TEST(DrawdownBreakerTest, S27_DoesNotFailClosedDuringStartupGrace) {
    // Valuations warm up over the first cycles; pausing before grace
    // expires would stop every start.
    EXPECT_FALSE(unvaluable_book_must_fail_closed(false, true, false, 0.0));
}

TEST(DrawdownBreakerTest, S27_DoesNotFailClosedOnACleanValuationWithAPeak) {
    EXPECT_FALSE(unvaluable_book_must_fail_closed(true, false, false, 500.0));
}

TEST(DrawdownBreakerTest, S27_ACleanValuationWithNoPeakStillFailsClosed) {
    // [review] This asserted the opposite and was wrong.
    //
    // Start degraded, become clean near the end of grace: valuation_degraded
    // is false, but ValuationAuthorityGate still refuses peak updates for its
    // debounce run -- so the peak stays at zero and equity_drawdown_frac()
    // returns 0.0 against it for up to nine post-grace cycles. The breaker is
    // not blunted, it cannot fire at all, which is the inert state S27 exists
    // to end.
    EXPECT_TRUE(unvaluable_book_must_fail_closed(
        true, false, false, 0.0, /*holding_anything=*/true));
}

TEST(DrawdownBreakerTest, S27_AnEmptyBookWithNoPeakIsNotAFault) {
    // A zero peak is honest when there is nothing to protect, and pausing on
    // it would make an idle engine unable to start.
    EXPECT_FALSE(unvaluable_book_must_fail_closed(
        true, false, false, 0.0, /*holding_anything=*/false));
    // Degraded but empty is still not a fault: degradation can be raised by
    // an asset that has since been sold. all_held_assets_unpriced is NOT
    // tested here because it is defined as "something held and nothing
    // live", so it cannot be true of an empty book -- asserting on that
    // combination would be asserting on a state the engine cannot produce.
    EXPECT_FALSE(unvaluable_book_must_fail_closed(
        true, true, false, 0.0, /*holding_anything=*/false));
}

TEST(DrawdownBreakerTest, S27_ANegativeOrNanPeakCountsAsNoPeak) {
    // `!(peak > 0)` rather than `peak <= 0` so NaN -- which fails every
    // comparison -- also reads as "no usable peak" instead of slipping
    // through as if it were valid.
    EXPECT_TRUE(unvaluable_book_must_fail_closed(true, true, false, -1.0));
    EXPECT_TRUE(unvaluable_book_must_fail_closed(
        true, true, false, std::numeric_limits<double>::quiet_NaN()));
}

TEST(DrawdownBreakerTest, S27_AllHeldAssetsUnpricedFailsClosedEvenWithARealPeak) {
    // Review finding: the first version of this helper assumed a frozen peak
    // still protects us.  It does not.  effective_usd_per_unit carries the
    // last known price with NO expiry check -- expiry only raises the
    // degraded flag -- so when nothing is live, equity holds at the very
    // value the peak was frozen at and the drawdown reads 0 forever.
    // Comparing a frozen equity against a frozen peak detects nothing.
    EXPECT_TRUE(unvaluable_book_must_fail_closed(
        /*grace_elapsed=*/true, /*valuation_degraded=*/true,
        /*all_held_assets_unpriced=*/true, /*peak_equity_usd=*/500.0));
}

TEST(DrawdownBreakerTest, S27_PartialDegradationWithARealPeakStillTrades) {
    // At least one asset live means equity still moves, so the ordinary
    // comparison genuinely works.  Pausing here would be a false positive.
    EXPECT_FALSE(unvaluable_book_must_fail_closed(true, true, false, 500.0));
}

TEST(DrawdownBreakerTest, S27_AllUnpricedStillRespectsStartupGrace) {
    EXPECT_FALSE(unvaluable_book_must_fail_closed(false, true, true, 500.0));
}

TEST(DrawdownBreakerTest, S27_ATransientAllUnpricedTickWithFreshCarriesDoesNotLatch) {
    // Review: "all unpriced this heartbeat" is not "we cannot value the
    // book".  A momentary feed gap with every carry still inside
    // valuation_carry_ttl_blocks is exactly what the carry mechanism exists
    // to bridge -- config.hpp: "a data gap must not read as a crash".
    // Without requiring degradation, one bad tick would permanently latch
    // the breaker, and a configured TTL of 0 ("never expire") would be
    // ignored outright.
    EXPECT_FALSE(unvaluable_book_must_fail_closed(
        /*grace_elapsed=*/true, /*valuation_degraded=*/false,
        /*all_held_assets_unpriced=*/true, /*peak_equity_usd=*/500.0));

    // Same tick, but the carries have now expired -> genuinely unvaluable.
    EXPECT_TRUE(unvaluable_book_must_fail_closed(true, true, true, 500.0));
}

// ---------------------------------------------------------------------------
// S32: "no price" is two conditions, and they need opposite responses.
// ---------------------------------------------------------------------------

TEST(DrawdownBreakerTest, S32_NoConfiguredPathIsWrittenOff) {
    // wUSDC.b after the warp.green compromise: held, both pairs disabled, so
    // no market event can ever price it.  Writing it off at $0 is what keeps
    // the rest of the book valuable and the breakers armed.
    EXPECT_TRUE(unpriced_asset_is_written_off(
        /*has_pricing_path=*/false, /*has_carry=*/false));
}

TEST(DrawdownBreakerTest, S32_AQuietFeedIsNeverWrittenOff) {
    // THE dangerous case.  A configured pair that simply has no usable mid
    // this heartbeat -- CoinGecko down, junk book, failed valuation grade --
    // must ride its carry and degrade, never mark to zero.  On a fresh
    // process this is also the pre-first-fetch state, so writing it off
    // would seed the drawdown peak from a near-zero equity and leave the
    // breaker under-protective for the rest of the run.
    EXPECT_FALSE(unpriced_asset_is_written_off(
        /*has_pricing_path=*/true, /*has_carry=*/false));
    EXPECT_FALSE(unpriced_asset_is_written_off(true, true));
}

TEST(DrawdownBreakerTest, S32_ACarryOutranksTheWriteOff) {
    // A carry can only exist if the asset was priced earlier this run, which
    // contradicts "no path".  If the two ever disagree, the real number the
    // engine once observed beats a synthetic zero.
    EXPECT_FALSE(unpriced_asset_is_written_off(
        /*has_pricing_path=*/false, /*has_carry=*/true));
}

TEST(DrawdownBreakerTest, S32_WriteOffsDoNotRescueATotallyUnpriceableBook) {
    // Written-off assets stay out of live_count, so a book made up ENTIRELY
    // of them still reports all-unpriced and still fails closed.  Equity is
    // $0, no peak can seed, and the engine must not trade.  This is the
    // guard against the write-off quietly becoming a way to disable the
    // breaker for the whole portfolio.
    EXPECT_TRUE(unvaluable_book_must_fail_closed(
        /*grace_elapsed=*/true, /*valuation_degraded=*/true,
        /*all_held_assets_unpriced=*/true, /*peak_equity_usd=*/0.0));
    EXPECT_TRUE(unvaluable_book_must_fail_closed(true, true, false, 0.0));
}

TEST(DrawdownBreakerTest, S32_ThePartialBookThatUnblocksResume) {
    // The live 2026-08-27 shape: XCH, BYC and DBX price fine; wUSDC.b and
    // wmilliETH.b are written off.  Nothing degrades, so the authority gate
    // stays armed, the peak seeds from real equity, and the ordinary
    // drawdown comparison protects the run.
    EXPECT_FALSE(unvaluable_book_must_fail_closed(
        /*grace_elapsed=*/true, /*valuation_degraded=*/false,
        /*all_held_assets_unpriced=*/false, /*peak_equity_usd=*/1234.56));
}

// ---------------------------------------------------------------------------
// S33: the degradation guard must not be satisfied by a different asset than
// the one that went quiet.
// ---------------------------------------------------------------------------

TEST(DrawdownBreakerTest, S33_AFreshCarryElsewhereStopsTheLatch) {
    // The reported shape: asset A sits on a long-expired carry (so the
    // aggregate degraded flag is permanently true) while asset B is live.
    // B then misses ONE tick with its carry still fresh.  The caller must
    // report all_held_assets_unpriced=false, because B is being bridged --
    // otherwise these inputs latch the breaker on a transient.
    EXPECT_FALSE(unvaluable_book_must_fail_closed(
        /*grace_elapsed=*/true, /*valuation_degraded=*/true,
        /*all_held_assets_unpriced=*/false, /*peak_equity_usd=*/500.0));

    // Once B's carry expires too, nothing is bridged and the book really is
    // unvaluable.
    EXPECT_TRUE(unvaluable_book_must_fail_closed(true, true, true, 500.0));
}

// ===========================================================================
// [S33 2026-09-05] Rolling-window auto-cooldown: WHO OWNS THE PAUSE
//
// This is the decision that resumes a live bot, and it shipped with no test.
// `breaker_pause_active_` is a SHARED latch: the max-drawdown, unvaluable-book
// and ledger-divergence breakers set the same flag, and only the rolling-window
// breaker has a self-clearing path.  `equity_healthy` covers the first two ONLY
// WHILE THEY STILL HOLD, so a recovered-but-unacknowledged drawdown trip, or a
// ledger divergence that has since re-baselined itself, was lifted by five
// quiet window samples -- the cooldown resuming a bot another breaker had
// deliberately stopped.  `window_loss_latched` is the ownership record.
//
// MUTATION CHECKS (each reinstates the exact pre-S33 expression):
//   1. Drop the ownership term: `if (!breaker_pause_active) { ... }` instead of
//      `if (!breaker_pause_active || !window_loss_latched)`.  This is the old
//      `} else if (breaker_pause_active_) {` verbatim.
//      -> WindowCooldownDoesNotLiftADrawdownPause and
//         LedgerEscalationRevokesWindowOwnership go RED.
//   2. Drop the operator-pause guard: return `{true, true}` instead of
//      `{true, !gui_pause_active}` (the old unconditional
//      `state_->set_status(BotStatus::Running)`).
//      -> LatchClearedButStatusStaysPausedUnderGuiPause goes RED.
//   3. Weaken the streak to 1 (`recover_streak < 1`).
//      -> AQuietWindowMustNormalizeForFiveConsecutiveSamples goes RED.
// ===========================================================================

using xop::risk::evaluate_window_cooldown;
using xop::risk::kWindowLossRecoverStreak;

namespace {

// Five consecutive quiet evaluations, returning the final decision.  Every
// intermediate decision is asserted to be a no-op, so a cooldown that fires
// EARLY is caught as well as one that fires when it must not.
xop::risk::WindowCooldownDecision run_quiet_window(bool breaker_pause_active,
                                                   bool window_loss_latched,
                                                   bool gui_pause_active,
                                                   int& streak,
                                                   int  samples = 5)
{
    xop::risk::WindowCooldownDecision d;
    for (int i = 0; i < samples; ++i) {
        d = evaluate_window_cooldown(breaker_pause_active,
                                     window_loss_latched, gui_pause_active,
                                     /*window_loss_usd=*/0.10,
                                     /*threshold_usd=*/3.75,
                                     /*equity_healthy=*/true, streak);
        if (i + 1 < samples) {
            EXPECT_FALSE(d.clear_latch) << "fired early at sample " << (i + 1);
        }
    }
    return d;
}

}  // namespace

// (a) The headline: a max-drawdown pause is not the window's to lift, however
// quiet the loss window goes.  The drawdown breaker has no self-clearing path
// at all -- it ends in operator acknowledgement.
TEST(BreakerOwnership, WindowCooldownDoesNotLiftADrawdownPause)
{
    int streak = 0;
    const auto d = run_quiet_window(/*breaker_pause_active=*/true,
                                    /*window_loss_latched=*/false,
                                    /*gui_pause_active=*/false, streak,
                                    /*samples=*/20);
    EXPECT_FALSE(d.clear_latch)
        << "another breaker owns this pause; five quiet window samples are "
           "not an acknowledgement of it";
    EXPECT_FALSE(d.set_running);
    EXPECT_EQ(streak, 0)
        << "no streak may accumulate toward lifting someone else's latch";
}

// (b) The window breaker trips FIRST and is then escalated over.  Ownership is
// revoked at the escalation, and cannot be re-taken while the pause holds
// (it is only taken on the window breaker's own false-to-true transition).
TEST(BreakerOwnership, LedgerEscalationRevokesWindowOwnership)
{
    int streak = 0;

    // Window breaker owns the pause and is part-way through its cooldown.
    for (int i = 0; i < 3; ++i) {
        const auto d = evaluate_window_cooldown(true, true, false, 0.10, 3.75,
                                                true, streak);
        EXPECT_FALSE(d.clear_latch);
    }
    EXPECT_EQ(streak, 3);

    // LEDGER CONTROL fires: engine sets window_loss_latched_ = false.
    const auto after = run_quiet_window(/*breaker_pause_active=*/true,
                                        /*window_loss_latched=*/false,
                                        /*gui_pause_active=*/false, streak,
                                        /*samples=*/10);
    EXPECT_FALSE(after.clear_latch)
        << "this pause now ends in reconciliation, not in the loss window "
           "going quiet";
    EXPECT_EQ(streak, 0) << "the part-built streak is discarded, not banked";
}

// (c) Clearing this breaker's latch is not the same as resuming.  Step 13 runs
// during a GUI pause by design, and check_pause_flag() only re-pauses on the
// flag's false-to-true EDGE -- so publishing Running here would leave the
// status Running with Step 8 skipped, and nothing would ever correct it.
TEST(BreakerOwnership, LatchClearedButStatusStaysPausedUnderGuiPause)
{
    int streak = 0;
    const auto d = run_quiet_window(/*breaker_pause_active=*/true,
                                    /*window_loss_latched=*/true,
                                    /*gui_pause_active=*/true, streak);
    EXPECT_TRUE(d.clear_latch)
        << "the breaker's own condition really has normalized";
    EXPECT_FALSE(d.set_running)
        << "the operator's pause flag is still set -- status stays Paused";
}

// The ordinary, intended case, for contrast: the window owns the pause, no
// operator flag, so the cooldown both clears the latch and resumes trading.
TEST(BreakerOwnership, AnOwnedPauseWithNoOperatorFlagResumesTrading)
{
    int streak = 0;
    const auto d = run_quiet_window(true, true, false, streak);
    EXPECT_TRUE(d.clear_latch);
    EXPECT_TRUE(d.set_running);
    EXPECT_EQ(streak, 0) << "the counter re-arms for the next episode";
}

// (d) Ownership is meaningless without a pause: nothing to lift, no streak.
// This is the pure-function reading of the engine invariant
// "window_loss_latched_ is false whenever breaker_pause_active_ is false" --
// even if that invariant were violated, no cooldown could fire from it.
TEST(BreakerOwnership, NoPauseMeansNothingToLift)
{
    int streak = 4;
    const auto d = evaluate_window_cooldown(/*breaker_pause_active=*/false,
                                            /*window_loss_latched=*/true,
                                            false, 0.10, 3.75, true, streak);
    EXPECT_FALSE(d.clear_latch);
    EXPECT_FALSE(d.set_running);
    EXPECT_EQ(streak, 0);
}

// The streak is a debounce: it must be CONSECUTIVE.
TEST(BreakerOwnership, AQuietWindowMustNormalizeForFiveConsecutiveSamples)
{
    EXPECT_EQ(kWindowLossRecoverStreak, 5);

    int streak = 0;
    for (int i = 0; i < 4; ++i) {
        EXPECT_FALSE(
            evaluate_window_cooldown(true, true, false, 0.10, 3.75, true,
                                     streak).clear_latch);
    }
    EXPECT_EQ(streak, 4);

    // One evaluation with the loss back over the threshold resets it.
    EXPECT_FALSE(evaluate_window_cooldown(true, true, false, 9.99, 3.75, true,
                                          streak).clear_latch);
    EXPECT_EQ(streak, 0);

    // So four more quiet samples are still not enough.
    for (int i = 0; i < 4; ++i) {
        EXPECT_FALSE(
            evaluate_window_cooldown(true, true, false, 0.10, 3.75, true,
                                     streak).clear_latch);
    }
    EXPECT_TRUE(evaluate_window_cooldown(true, true, false, 0.10, 3.75, true,
                                         streak).clear_latch);
}

// Equity health is the other half of the gate, and it is what blocks the
// cooldown WHILE a drawdown or unvaluable-book condition still holds --
// ownership is what blocks it after such a condition has numerically
// recovered but was never acknowledged.
TEST(BreakerOwnership, AnUnhealthyEquityBlocksTheCooldownEvenWhenOwned)
{
    int streak = 0;
    for (int i = 0; i < 10; ++i) {
        const auto d = evaluate_window_cooldown(true, true, false, 0.10, 3.75,
                                                /*equity_healthy=*/false,
                                                streak);
        EXPECT_FALSE(d.clear_latch);
    }
    EXPECT_EQ(streak, 0);
}

// A loss exactly AT the threshold is normalized (the trip test is strictly
// greater), and a NaN reading resets rather than counting toward a resume.
TEST(BreakerOwnership, ThresholdBoundaryAndNanReadings)
{
    int streak = 0;
    EXPECT_FALSE(evaluate_window_cooldown(true, true, false, 3.75, 3.75, true,
                                          streak).clear_latch);
    EXPECT_EQ(streak, 1) << "loss == threshold is inside the limit";

    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(evaluate_window_cooldown(true, true, false, nan, 3.75, true,
                                          streak).clear_latch);
    EXPECT_EQ(streak, 0) << "an unordered comparison must not resume trading";
}
