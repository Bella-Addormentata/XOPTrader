// test_fee_tracker.cpp -- Unit tests for FeeTracker: fee selection, clamping,
//                         budget enforcement, and fee-vs-gain gating.
//
// These tests verify that the fee tracker correctly selects fees from
// mempool estimates, applies min/max clamps, enforces daily budgets,
// and gates offers based on fee-to-gain ratios.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/strategy/fee_tracker.hpp>

namespace {

// ---------------------------------------------------------------------------
// Helper: build a FeeConfig with overridable fields.
// ---------------------------------------------------------------------------
xop::FeeConfig make_config(
    bool     enabled           = true,
    uint64_t daily_budget      = 10'000'000'000ULL,
    double   gain_ratio        = 0.30,
    double   cancel_mult       = 2.0,
    uint64_t min_fee           = 50'000ULL,
    uint64_t max_fee           = 100'000'000ULL,
    bool     adaptive          = true,
    uint32_t window            = 1662,
    uint32_t estimate_target_s = 300)
{
    xop::FeeConfig cfg;
    cfg.enabled                    = enabled;
    cfg.daily_budget_mojos         = daily_budget;
    cfg.fee_to_gain_max_ratio      = gain_ratio;
    cfg.cancel_cost_multiplier     = cancel_mult;
    cfg.min_fee_mojos              = min_fee;
    cfg.max_fee_mojos              = max_fee;
    cfg.adaptive_enabled           = adaptive;
    cfg.fee_window_blocks          = window;
    cfg.fee_estimate_target_seconds = estimate_target_s;
    return cfg;
}

// ============================================================================
// Disabled tracker: passthrough behaviour
// ============================================================================

TEST(FeeTrackerTest, DisabledReturnsStaticFee) {
    auto cfg = make_config(/*enabled=*/false);
    xop::FeeTracker tracker(cfg);

    // When disabled, get_recommended_fee returns static_fee unchanged.
    EXPECT_EQ(tracker.get_recommended_fee(10'000'000, 100), 10'000'000ULL);
    EXPECT_EQ(tracker.get_recommended_fee(1, 100), 1ULL);
    EXPECT_EQ(tracker.get_recommended_fee(999'999'999, 100), 999'999'999ULL);
}

TEST(FeeTrackerTest, DisabledAlwaysAllowsPosting) {
    auto cfg = make_config(/*enabled=*/false);
    xop::FeeTracker tracker(cfg);

    // Even zero-gain, high fee should be allowed when disabled.
    EXPECT_TRUE(tracker.should_post_offer(0, 100'000'000, 100));
    EXPECT_TRUE(tracker.should_post_offer(100, 100'000'000, 100));
}

// ============================================================================
// Min/max clamping
// ============================================================================

TEST(FeeTrackerTest, ClampsToMinFee) {
    // min=50000, adaptive off → uses static fee, clamped to min.
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false);
    xop::FeeTracker tracker(cfg);

    // Static fee below min → clamped up.
    EXPECT_EQ(tracker.get_recommended_fee(100, 100), 50'000ULL);
    EXPECT_EQ(tracker.get_recommended_fee(1, 100), 50'000ULL);
}

TEST(FeeTrackerTest, ClampsToMaxFee) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false);
    xop::FeeTracker tracker(cfg);

    // Static fee above max → clamped down.
    EXPECT_EQ(tracker.get_recommended_fee(500'000'000, 100), 100'000'000ULL);
}

TEST(FeeTrackerTest, FeeWithinBandPassesThrough) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false);
    xop::FeeTracker tracker(cfg);

    // Static fee inside [min, max] → returned unchanged.
    EXPECT_EQ(tracker.get_recommended_fee(5'000'000, 100), 5'000'000ULL);
}

// ============================================================================
// Adaptive mode: mempool estimate usage
// ============================================================================

TEST(FeeTrackerTest, AdaptiveUsesLowMempoolEstimate) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/true);
    xop::FeeTracker tracker(cfg);

    // Feed a mempool estimate within [min, max] range.
    tracker.update_mempool_estimate(200'000);

    // Should use the mempool estimate instead of static.
    EXPECT_EQ(tracker.get_recommended_fee(10'000'000, 100), 200'000ULL);
}

TEST(FeeTrackerTest, AdaptiveMempoolBelowMinClampsUp) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/true);
    xop::FeeTracker tracker(cfg);

    // Mempool estimate is below min_fee → clamped up to min.
    tracker.update_mempool_estimate(5'661);
    EXPECT_EQ(tracker.get_recommended_fee(10'000'000, 100), 50'000ULL);
}

TEST(FeeTrackerTest, AdaptiveMempoolAboveMaxClampsDown) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/true);
    xop::FeeTracker tracker(cfg);

    // Mempool estimate exceeds max_fee → clamped down.
    tracker.update_mempool_estimate(500'000'000);
    EXPECT_EQ(tracker.get_recommended_fee(10'000'000, 100), 100'000'000ULL);
}

TEST(FeeTrackerTest, AdaptiveNoEstimateFallsBackToStatic) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/true);
    xop::FeeTracker tracker(cfg);

    // No mempool estimate fed → falls back to static (clamped).
    EXPECT_EQ(tracker.get_recommended_fee(5'000'000, 100), 5'000'000ULL);
}

// ============================================================================
// High min_fee floor detection (the exact bug that prompted this):
// When min_fee is 5M but mempool says 5661, the clamped fee is 880x
// higher than necessary.  With min_fee=50K, the ratio is only 8.8x.
// ============================================================================

TEST(FeeTrackerTest, LowMinFeeAllowsCheapMempoolFee) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/true);
    xop::FeeTracker tracker(cfg);

    tracker.update_mempool_estimate(5'661);

    const uint64_t fee = tracker.get_recommended_fee(10'000'000, 100);
    // With min=50000, the mempool estimate 5661 is clamped up to 50000.
    // This is 50000/5661 = ~8.8x, much better than the old 5M/5661 = ~883x.
    EXPECT_EQ(fee, 50'000ULL);
    EXPECT_LT(fee, 100'000ULL);  // Sanity: well under 0.0001 XCH
}

TEST(FeeTrackerTest, HighMinFeeCausesMassiveOverpay) {
    // This test documents the OLD problematic behaviour with min_fee=5M.
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           5'000'000, 100'000'000, /*adaptive=*/true);
    xop::FeeTracker tracker(cfg);

    tracker.update_mempool_estimate(5'661);

    const uint64_t fee = tracker.get_recommended_fee(10'000'000, 100);
    // With min=5M, the mempool estimate 5661 is clamped up to 5M: 883x overpay.
    EXPECT_EQ(fee, 5'000'000ULL);
    EXPECT_GT(fee, 5'661ULL * 100);  // Proves overpay > 100x
}

// ============================================================================
// Budget enforcement
// ============================================================================

TEST(FeeTrackerTest, BudgetExhaustedReturnsZero) {
    auto cfg = make_config(true, /*budget=*/100'000, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false);
    xop::FeeTracker tracker(cfg);

    // Record enough fees to exhaust the budget.
    tracker.record_fee(60'000, 100);
    tracker.record_fee(60'000, 101);

    // Budget = 100k, spent = 120k → exhausted.
    // get_recommended_fee should return 0 (headroom < min_fee).
    EXPECT_EQ(tracker.get_recommended_fee(50'000, 102), 0ULL);
}

TEST(FeeTrackerTest, BudgetWithinLimitsAllowsFee) {
    auto cfg = make_config(true, /*budget=*/10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false);
    xop::FeeTracker tracker(cfg);

    tracker.record_fee(1'000'000, 100);

    // Budget = 10B, spent = 1M → plenty of headroom.
    EXPECT_EQ(tracker.get_recommended_fee(5'000'000, 101), 5'000'000ULL);
}

TEST(FeeTrackerTest, OldFeesExpireFromWindow) {
    auto cfg = make_config(true, /*budget=*/100'000, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false,
                           /*window=*/10);
    xop::FeeTracker tracker(cfg);

    // Record a fee at block 100.
    tracker.record_fee(90'000, 100);

    // At block 105 (within window), budget nearly exhausted.
    EXPECT_EQ(tracker.get_rolling_total(105), 90'000ULL);

    // At block 115 (block 100 has expired from 10-block window), budget freed.
    EXPECT_EQ(tracker.get_rolling_total(115), 0ULL);
    EXPECT_NE(tracker.get_recommended_fee(50'000, 115), 0ULL);
}

// ============================================================================
// Fee-vs-gain gating
// ============================================================================

TEST(FeeTrackerTest, GateSkipsWhenFeeExceedsGainRatio) {
    auto cfg = make_config(true, 10'000'000'000ULL,
                           /*gain_ratio=*/0.30, /*cancel_mult=*/2.0,
                           50'000, 100'000'000);
    xop::FeeTracker tracker(cfg);

    // Fee=100'000, gain=100'000. Round-trip = 100k × 2.0 = 200k.
    // Ratio = 200k / 100k = 2.0 > 0.30 → skip.
    EXPECT_FALSE(tracker.should_post_offer(100'000, 100'000, 100));
}

TEST(FeeTrackerTest, GateAllowsWhenFeeWithinRatio) {
    auto cfg = make_config(true, 10'000'000'000ULL,
                           /*gain_ratio=*/0.30, /*cancel_mult=*/2.0,
                           50'000, 100'000'000);
    xop::FeeTracker tracker(cfg);

    // Fee=50'000, gain=1'000'000. Round-trip = 50k × 2.0 = 100k.
    // Ratio = 100k / 1M = 0.10 < 0.30 → allow.
    EXPECT_TRUE(tracker.should_post_offer(1'000'000, 50'000, 100));
}

TEST(FeeTrackerTest, GateSkipsZeroGainWithFee) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000);
    xop::FeeTracker tracker(cfg);

    // Zero gain with non-zero fee → always skip.
    EXPECT_FALSE(tracker.should_post_offer(0, 50'000, 100));
}

TEST(FeeTrackerTest, GateAllowsZeroFee) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000);
    xop::FeeTracker tracker(cfg);

    // Zero fee with zero gain → technically free, no cost → allow.
    EXPECT_TRUE(tracker.should_post_offer(0, 0, 100));
}

}  // namespace
