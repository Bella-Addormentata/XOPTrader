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

#include <cstdint>
#include <limits>

namespace {

// [review #163 r7] The uint64 boundary the rolling total is kept exact across.
constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

// [S67] get_recommended_fee now names an action class (no default, so a call
// site that forgets one does not compile).  With fees.cost_aware_estimate and
// the controller both off -- every test in this file -- the class is never
// read.  Take is used rather than the zero enumerator so that a regression
// which starts reading it changes these expectations, none of which moved.
constexpr auto kAnyClass = xop::strategy::fee::ActionClass::Take;

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
    EXPECT_EQ(tracker.get_recommended_fee(10'000'000, 100, kAnyClass), 10'000'000ULL);
    EXPECT_EQ(tracker.get_recommended_fee(1, 100, kAnyClass), 1ULL);
    EXPECT_EQ(tracker.get_recommended_fee(999'999'999, 100, kAnyClass), 999'999'999ULL);
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
    EXPECT_EQ(tracker.get_recommended_fee(100, 100, kAnyClass), 50'000ULL);
    EXPECT_EQ(tracker.get_recommended_fee(1, 100, kAnyClass), 50'000ULL);
}

TEST(FeeTrackerTest, ClampsToMaxFee) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false);
    xop::FeeTracker tracker(cfg);

    // Static fee above max → clamped down.
    EXPECT_EQ(tracker.get_recommended_fee(500'000'000, 100, kAnyClass), 100'000'000ULL);
}

TEST(FeeTrackerTest, FeeWithinBandPassesThrough) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false);
    xop::FeeTracker tracker(cfg);

    // Static fee inside [min, max] → returned unchanged.
    EXPECT_EQ(tracker.get_recommended_fee(5'000'000, 100, kAnyClass), 5'000'000ULL);
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
    EXPECT_EQ(tracker.get_recommended_fee(10'000'000, 100, kAnyClass), 200'000ULL);
}

TEST(FeeTrackerTest, AdaptiveMempoolBelowMinClampsUp) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/true);
    xop::FeeTracker tracker(cfg);

    // Mempool estimate is below min_fee → clamped up to min.
    tracker.update_mempool_estimate(5'661);
    EXPECT_EQ(tracker.get_recommended_fee(10'000'000, 100, kAnyClass), 50'000ULL);
}

TEST(FeeTrackerTest, AdaptiveMempoolAboveMaxClampsDown) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/true);
    xop::FeeTracker tracker(cfg);

    // Mempool estimate exceeds max_fee → clamped down.
    tracker.update_mempool_estimate(500'000'000);
    EXPECT_EQ(tracker.get_recommended_fee(10'000'000, 100, kAnyClass), 100'000'000ULL);
}

TEST(FeeTrackerTest, AdaptiveNoEstimateFallsBackToStatic) {
    auto cfg = make_config(true, 10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/true);
    xop::FeeTracker tracker(cfg);

    // No mempool estimate fed → falls back to static (clamped).
    EXPECT_EQ(tracker.get_recommended_fee(5'000'000, 100, kAnyClass), 5'000'000ULL);
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

    const uint64_t fee = tracker.get_recommended_fee(10'000'000, 100, kAnyClass);
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

    const uint64_t fee = tracker.get_recommended_fee(10'000'000, 100, kAnyClass);
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
    EXPECT_EQ(tracker.get_recommended_fee(50'000, 102, kAnyClass), 0ULL);
}

TEST(FeeTrackerTest, BudgetWithinLimitsAllowsFee) {
    auto cfg = make_config(true, /*budget=*/10'000'000'000ULL, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false);
    xop::FeeTracker tracker(cfg);

    tracker.record_fee(1'000'000, 100);

    // Budget = 10B, spent = 1M → plenty of headroom.
    EXPECT_EQ(tracker.get_recommended_fee(5'000'000, 101, kAnyClass), 5'000'000ULL);
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
    EXPECT_NE(tracker.get_recommended_fee(50'000, 115, kAnyClass), 0ULL);
}

// ============================================================================
// [review #163 r7] The rolling total across the uint64 boundary
//
// THE POST-CONDITION these three pin: get_rolling_total() returns the TRUE sum
// of the fees still inside the window, or UINT64_MAX if and only if that true
// sum genuinely exceeds UINT64_MAX.
//
// Round 6 made record_fee()'s addition saturate (to stop a WRAP reading as a
// huge headroom) and made prune() clamp to match:
//     cached_total_ = (cached_total_ > oldest) ? cached_total_ - oldest : 0U;
// Saturating addition is lossy and therefore not invertible, so no subtraction
// is its inverse.  These tests exist to fail with that line restored.
// ============================================================================

TEST(FeeTrackerTest, PruningASaturatingEntryLeavesTheRestOfTheWindowIntact) {
    auto cfg = make_config(true, /*budget=*/100'000, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false,
                           /*window=*/10);
    xop::FeeTracker tracker(cfg);

    // The reviewer's exact shape: an entry that saturates the uint64 total,
    // then a small one, then a prune that removes only the large entry.
    tracker.record_fee(kU64Max, 100);
    tracker.record_fee(100, 101);

    // Both are inside the window and the true sum is UINT64_MAX + 100, which
    // genuinely exceeds UINT64_MAX -- so UINT64_MAX is the RIGHT answer here,
    // and the budget is correctly exhausted.
    EXPECT_EQ(tracker.get_rolling_total(105), kU64Max);
    EXPECT_EQ(tracker.budget_remaining(105), 0ULL);

    // Block 111 -> cutoff 101: the block-100 entry expires, the block-101
    // entry does not.  The true sum is now exactly 100.
    //
    // The r6 clamp answered 0 -- `UINT64_MAX > UINT64_MAX` is FALSE, so the
    // else branch fired -- and reopened the entire budget with 100 mojos still
    // inside the window.  That is a fail-OPEN on the number the budget is.
    EXPECT_EQ(tracker.get_rolling_total(111), 100ULL);
    EXPECT_EQ(tracker.budget_remaining(111), 99'900ULL);
}

TEST(FeeTrackerTest, RollingTotalIsExactAcrossASaturationAndBackDown) {
    auto cfg = make_config(true, /*budget=*/100'000, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false,
                           /*window=*/10);
    xop::FeeTracker tracker(cfg);

    constexpr std::uint64_t kHalf = 9'223'372'036'854'775'807ULL;  // 2^63 - 1

    tracker.record_fee(kHalf, 100);  // window sum: 2^63 - 1
    tracker.record_fee(kHalf, 101);  // window sum: 2^64 - 2, still fits
    EXPECT_EQ(tracker.get_rolling_total(105), kU64Max - 1ULL);

    tracker.record_fee(1, 102);  // window sum: 2^64 - 1, UINT64_MAX EXACTLY
    EXPECT_EQ(tracker.get_rolling_total(106), kU64Max);

    tracker.record_fee(10, 103);  // window sum: 2^64 + 9, genuinely over
    EXPECT_EQ(tracker.get_rolling_total(107), kU64Max);

    // Block 111 -> cutoff 101: the first 2^63-1 expires.  True sum is
    // (2^63 - 1) + 1 + 10 = 2^63 + 10.  The r6 clamp answered 2^63: the 11
    // mojos it had lost to saturation never came back, so every later answer
    // in this window was wrong too.
    EXPECT_EQ(tracker.get_rolling_total(111), 9'223'372'036'854'775'818ULL);

    // Block 112 -> cutoff 102: the second 2^63-1 expires.  True sum is 11.
    EXPECT_EQ(tracker.get_rolling_total(112), 11ULL);

    // Block 113 -> cutoff 103: the 1 expires.  True sum is 10.
    EXPECT_EQ(tracker.get_rolling_total(113), 10ULL);
}

TEST(FeeTrackerTest, PrunedWindowStillBindsTheBudgetAfterASaturation) {
    auto cfg = make_config(true, /*budget=*/100'000, 0.30, 2.0,
                           50'000, 100'000'000, /*adaptive=*/false,
                           /*window=*/10);
    xop::FeeTracker tracker(cfg);

    // Engine::cancel_fees_paid() really does hand record_fee a SATURATED
    // UINT64_MAX (it clamps `count x fee` rather than wrapping it), so this is
    // the live input path and not an invented one.
    tracker.record_fee(kU64Max, 100);
    tracker.record_fee(99'990, 101);

    // While saturated the budget fails CLOSED: nothing fits, nothing posts.
    EXPECT_FALSE(tracker.is_within_budget(105, 1));
    EXPECT_FALSE(tracker.should_post_offer(1'000'000, 50'000, 105));

    // Once the saturating entry expires, 99,990 of the 100,000 budget is still
    // spent inside the window: 10 mojos fit and 11 do not.  Under the r6 clamp
    // the total read 0 and the FULL budget was handed back.
    EXPECT_EQ(tracker.get_rolling_total(111), 99'990ULL);
    EXPECT_TRUE(tracker.is_within_budget(111, 10));
    EXPECT_FALSE(tracker.is_within_budget(111, 11));
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
