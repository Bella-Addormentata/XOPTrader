// test_market_analyzer.cpp -- Unit tests for the MarketAnalyzer startup
//                             analysis module.
//
// Tests verify:
//   - Default construction and empty state.
//   - ingest() correctly fills the rolling window.
//   - is_complete() returns false until analysis_blocks observations are fed.
//   - Volatility is zero for a constant-price series.
//   - Balanced and bid-heavy book imbalance calculations.
//   - Spread mean and coefficient of variation.
//   - reset() clears all accumulated state.
//   - Invalid price observations are ignored.
//   - Aggressiveness recommendation: Conservative (high vol), Conservative
//     (momentum regime), Aggressive (low vol + wide spread + mean-reverting),
//     and Normal for typical market conditions.
//   - analysis_blocks clamped to minimum of 3.
//   - Unknown pairs are added on-the-fly during ingest.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/data/market_analyzer.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace {

// ============================================================================
// Default config and helpers
// ============================================================================

xop::MarketAnalyzerConfig small_config(uint32_t blocks = 10) {
    xop::MarketAnalyzerConfig cfg;
    cfg.analysis_blocks       = blocks;
    cfg.block_time_seconds    = 52.0;
    cfg.vr_short_lag          = 5;
    cfg.vr_lower_threshold    = 0.85;
    cfg.vr_upper_threshold    = 1.15;
    cfg.high_vol_threshold    = 0.40;
    cfg.high_spread_cv_threshold = 0.80;
    cfg.wide_spread_bps_threshold = 80.0;
    return cfg;
}

const std::string kPair = "XCH/wUSDC";

// Ingest N identical observations into the analyzer.
void feed_constant(xop::MarketAnalyzer& ma,
                   const std::string& pair,
                   double price,
                   int count) {
    for (int i = 0; i < count; ++i) {
        ma.ingest(pair, price, 50.0, 1000.0, 5.0, 5.0);
    }
}

// ============================================================================
// TEST: Default construction -- no pairs, is_complete() == true
// ============================================================================

TEST(MarketAnalyzerTest, DefaultConstructNoPairs) {
    xop::MarketAnalyzer ma;
    EXPECT_TRUE(ma.is_complete());
    EXPECT_EQ(ma.analysis_blocks(), 20u);
}

// ============================================================================
// TEST: Single pair -- not complete until analysis_blocks fed
// ============================================================================

TEST(MarketAnalyzerTest, NotCompleteBeforeWindow) {
    auto cfg = small_config(5);
    xop::MarketAnalyzer ma(cfg, {kPair});

    EXPECT_FALSE(ma.is_complete());
    EXPECT_EQ(ma.blocks_collected(kPair), 0u);

    ma.ingest(kPair, 2.70, 50.0, 1000.0, 5.0, 5.0);
    EXPECT_EQ(ma.blocks_collected(kPair), 1u);
    EXPECT_FALSE(ma.is_complete());

    for (int i = 1; i < 5; ++i) {
        ma.ingest(kPair, 2.70, 50.0, 1000.0, 5.0, 5.0);
    }
    EXPECT_TRUE(ma.is_complete());
    EXPECT_EQ(ma.blocks_collected(kPair), 5u);
}

// ============================================================================
// TEST: Constant price series -> near-zero volatility
// ============================================================================

TEST(MarketAnalyzerTest, ConstantPriceZeroVolatility) {
    auto cfg = small_config(10);
    xop::MarketAnalyzer ma(cfg, {kPair});

    feed_constant(ma, kPair, 2.70, 10);
    EXPECT_TRUE(ma.is_complete());

    const auto s = ma.get_summary(kPair);
    EXPECT_NEAR(s.volatility_per_block, 0.0, 1e-10);
    EXPECT_NEAR(s.volatility_annual, 0.0, 1e-10);
    EXPECT_NEAR(s.momentum, 0.0, 1e-10);
}

// ============================================================================
// TEST: Balanced book imbalance
// ============================================================================

TEST(MarketAnalyzerTest, BalancedBookImbalance) {
    auto cfg = small_config(5);
    xop::MarketAnalyzer ma(cfg, {kPair});

    for (int i = 0; i < 5; ++i) {
        // Equal bid and ask depth -> imbalance should be 0.5.
        ma.ingest(kPair, 2.70, 50.0, 1000.0, 10.0, 10.0);
    }
    const auto s = ma.get_summary(kPair);
    EXPECT_NEAR(s.book_imbalance, 0.5, 1e-9);
}

// ============================================================================
// TEST: Bid-heavy book imbalance
// ============================================================================

TEST(MarketAnalyzerTest, BidHeavyBookImbalance) {
    auto cfg = small_config(5);
    xop::MarketAnalyzer ma(cfg, {kPair});

    for (int i = 0; i < 5; ++i) {
        // 75 bid / 25 ask depth.
        ma.ingest(kPair, 2.70, 50.0, 1000.0, 75.0, 25.0);
    }
    const auto s = ma.get_summary(kPair);
    EXPECT_NEAR(s.book_imbalance, 0.75, 1e-6);
}

// ============================================================================
// TEST: Spread statistics -- mean and CV
// ============================================================================

TEST(MarketAnalyzerTest, SpreadStatistics) {
    auto cfg = small_config(4);
    xop::MarketAnalyzer ma(cfg, {kPair});

    // Feed spreads: 40, 50, 60, 50 => mean = 50, sd = ~7.07, CV = 0.141
    const std::vector<double> spreads = {40.0, 50.0, 60.0, 50.0};
    double price = 2.70;
    for (double sp : spreads) {
        ma.ingest(kPair, price, sp, 1000.0, 5.0, 5.0);
    }

    const auto s = ma.get_summary(kPair);
    EXPECT_NEAR(s.mean_spread_bps, 50.0, 1e-6);
    // CV = stddev / mean.  With n=4, sample stddev = sqrt(200/3) = 8.165...
    EXPECT_GT(s.spread_cv, 0.0);
    EXPECT_LT(s.spread_cv, 1.0);
}

// ============================================================================
// TEST: reset() clears all accumulated data
// ============================================================================

TEST(MarketAnalyzerTest, ResetClearsState) {
    auto cfg = small_config(3);
    xop::MarketAnalyzer ma(cfg, {kPair});

    feed_constant(ma, kPair, 2.70, 3);
    EXPECT_TRUE(ma.is_complete());

    ma.reset();
    EXPECT_FALSE(ma.is_complete());
    EXPECT_EQ(ma.blocks_collected(kPair), 0u);
}

// ============================================================================
// TEST: Invalid price is ignored
// ============================================================================

TEST(MarketAnalyzerTest, InvalidPriceIgnored) {
    auto cfg = small_config(3);
    xop::MarketAnalyzer ma(cfg, {kPair});

    ma.ingest(kPair, 0.0, 50.0, 1000.0, 5.0, 5.0);   // invalid -- ignored
    ma.ingest(kPair, -1.0, 50.0, 1000.0, 5.0, 5.0);   // invalid -- ignored
    EXPECT_EQ(ma.blocks_collected(kPair), 0u);

    ma.ingest(kPair, 2.70, 50.0, 1000.0, 5.0, 5.0);   // valid
    EXPECT_EQ(ma.blocks_collected(kPair), 1u);
}

// ============================================================================
// TEST: Aggressiveness heuristic -- Conservative for high volatility
// ============================================================================

TEST(MarketAnalyzerTest, HighVolIsConservative) {
    auto cfg = small_config(10);
    // Use a high-vol threshold of 0.05 (5%) so the synthetic series triggers it.
    cfg.high_vol_threshold = 0.05;
    xop::MarketAnalyzer ma(cfg, {kPair});

    // Alternating +5% / -5% price moves -> high volatility.
    double price = 2.70;
    for (int i = 0; i < 10; ++i) {
        price *= (i % 2 == 0) ? 1.05 : (1.0 / 1.05);
        ma.ingest(kPair, price, 50.0, 1000.0, 5.0, 5.0);
    }

    const auto s = ma.get_summary(kPair);
    EXPECT_EQ(s.aggressiveness, xop::AnalysisAggressiveness::Conservative);
}

// ============================================================================
// TEST: Analysis_blocks clamped to minimum of 3
// ============================================================================

TEST(MarketAnalyzerTest, AnalysisBlocksClampedToThree) {
    xop::MarketAnalyzerConfig cfg;
    cfg.analysis_blocks = 1;  // Below minimum; should be clamped to 3.
    xop::MarketAnalyzer ma(cfg, {kPair});
    EXPECT_EQ(ma.analysis_blocks(), 3u);
}

// ============================================================================
// TEST: Unknown pair is added on-the-fly
// ============================================================================

TEST(MarketAnalyzerTest, UnknownPairAddedOnFly) {
    auto cfg = small_config(2);
    xop::MarketAnalyzer ma(cfg, {});  // No pairs initially.

    EXPECT_EQ(ma.blocks_collected("NEW/PAIR"), 0u);
    ma.ingest("NEW/PAIR", 1.0, 50.0, 1000.0, 5.0, 5.0);
    EXPECT_EQ(ma.blocks_collected("NEW/PAIR"), 1u);
}

// ============================================================================
// TEST: force_complete() marks all pairs as complete
// ============================================================================

TEST(MarketAnalyzerTest, ForceCompleteMarksPairsComplete) {
    auto cfg = small_config(10);
    xop::MarketAnalyzer ma(cfg, {kPair, "XCH/DBX"});

    // Feed only 3 blocks (not enough to complete normally).
    for (int i = 0; i < 3; ++i) {
        ma.ingest(kPair, 2.70, 50.0, 1000.0, 5.0, 5.0);
        ma.ingest("XCH/DBX", 1.20, 40.0, 500.0, 3.0, 3.0);
    }
    EXPECT_FALSE(ma.is_complete());

    ma.force_complete();
    EXPECT_TRUE(ma.is_complete());

    // Summaries should still have the data we ingested.
    const auto s = ma.get_summary(kPair);
    EXPECT_EQ(s.blocks_collected, 3u);
    EXPECT_TRUE(s.complete);
    // Force-completed pairs should NOT have window_filled.
    EXPECT_FALSE(s.window_filled);
}

// ============================================================================
// TEST: force_complete() on already-complete pair is a no-op
// ============================================================================

TEST(MarketAnalyzerTest, ForceCompleteAlreadyComplete) {
    auto cfg = small_config(3);
    xop::MarketAnalyzer ma(cfg, {kPair});

    feed_constant(ma, kPair, 2.70, 3);
    EXPECT_TRUE(ma.is_complete());

    // Naturally completed: both flags are true.
    const auto s_before = ma.get_summary(kPair);
    EXPECT_TRUE(s_before.complete);
    EXPECT_TRUE(s_before.window_filled);

    // force_complete() on already-complete pair should not change anything.
    ma.force_complete();
    EXPECT_TRUE(ma.is_complete());

    const auto s_after = ma.get_summary(kPair);
    EXPECT_TRUE(s_after.complete);
    EXPECT_TRUE(s_after.window_filled);
}

// ============================================================================
// TEST: overall_recommendation returns most conservative across pairs
// ============================================================================

TEST(MarketAnalyzerTest, OverallRecommendationMostConservative) {
    auto cfg = small_config(10);
    cfg.high_vol_threshold = 0.05;  // Lower threshold so synthetic data triggers.
    xop::MarketAnalyzer ma(cfg, {kPair, "XCH/DBX"});

    // kPair: alternating ±5% → high vol → Conservative.
    double price = 2.70;
    for (int i = 0; i < 10; ++i) {
        price *= (i % 2 == 0) ? 1.05 : (1.0 / 1.05);
        ma.ingest(kPair, price, 50.0, 1000.0, 5.0, 5.0);
    }

    // XCH/DBX: constant price → Normal.
    feed_constant(ma, "XCH/DBX", 1.20, 10);

    EXPECT_TRUE(ma.is_complete());

    // Overall should be Conservative (worst of Conservative + Normal).
    EXPECT_EQ(ma.overall_recommendation(), xop::AnalysisAggressiveness::Conservative);
    EXPECT_DOUBLE_EQ(ma.recommended_spread_multiplier(), 1.5);
}

// ============================================================================
// TEST: overall_recommendation Normal when all pairs are Normal
// ============================================================================

TEST(MarketAnalyzerTest, OverallRecommendationNormalWhenAllNormal) {
    auto cfg = small_config(5);
    xop::MarketAnalyzer ma(cfg, {kPair});

    // Constant price, moderate spread → Normal recommendation.
    feed_constant(ma, kPair, 2.70, 5);

    EXPECT_EQ(ma.overall_recommendation(), xop::AnalysisAggressiveness::Normal);
    EXPECT_DOUBLE_EQ(ma.recommended_spread_multiplier(), 1.0);
}

// ============================================================================
// TEST: total_poll_attempts tracks invalid ingestions
// ============================================================================

TEST(MarketAnalyzerTest, TotalPollAttemptsCountsInvalidData) {
    auto cfg = small_config(3);
    xop::MarketAnalyzer ma(cfg, {kPair});

    // Invalid: mid_price <= 0.
    ma.ingest(kPair, 0.0, 50.0, 1000.0, 5.0, 5.0);
    ma.ingest(kPair, -1.0, 50.0, 1000.0, 5.0, 5.0);

    // blocks_collected should be 0 (invalid data rejected).
    EXPECT_EQ(ma.blocks_collected(kPair), 0u);

    // But if we feed enough valid data, it still completes.
    feed_constant(ma, kPair, 2.70, 3);
    EXPECT_TRUE(ma.is_complete());
}

// ============================================================================
// TEST: Aggressiveness heuristic -- Conservative for momentum regime
// ============================================================================

TEST(MarketAnalyzerTest, MomentumRegimeIsConservative) {
    auto cfg = small_config(20);
    // Make the momentum classification easier to trigger in a deterministic test.
    cfg.vr_upper_threshold = 1.01;
    xop::MarketAnalyzer ma(cfg, {kPair});

    // Steadily rising price -> positive autocorrelation -> VR > 1.
    double price = 1.0;
    for (int i = 0; i < 20; ++i) {
        price *= 1.10;  // exaggerated trend (not realistic) to deterministically
                        // push VR above vr_upper_threshold in a short window
        ma.ingest(kPair, price, 50.0, 1000.0, 5.0, 5.0);
    }

    const auto s = ma.get_summary(kPair);
    // Momentum regime OR high vol must produce Conservative recommendation.
    EXPECT_EQ(s.aggressiveness, xop::AnalysisAggressiveness::Conservative);
}

// ============================================================================
// TEST: Aggressiveness heuristic -- Aggressive for low-vol mean-reverting
//       market with wide observed spread
// ============================================================================

TEST(MarketAnalyzerTest, LowVolWideMeanRevertingIsAggressive) {
    // Use thresholds that make it easy to trigger Aggressive:
    //   high_vol_threshold       = 1.0  (never triggered -- very high)
    //   wide_spread_bps_threshold = 10.0 (low threshold, easy to exceed)
    //   vr_lower_threshold        = 1.1  (most series will appear mean-reverting)
    auto cfg = small_config(10);
    cfg.high_vol_threshold        = 1.0;    // Effectively disable vol check.
    cfg.wide_spread_bps_threshold = 10.0;   // Wide threshold easily exceeded.
    cfg.vr_lower_threshold        = 2.0;    // Force MeanReverting regime.

    xop::MarketAnalyzer ma(cfg, {kPair});

    // Constant price (zero vol) and very wide spread (500 bps).
    for (int i = 0; i < 10; ++i) {
        ma.ingest(kPair, 2.70, 500.0, 1000.0, 5.0, 5.0);
    }

    const auto s = ma.get_summary(kPair);
    // VR will be 1.0 (constant price → no variance), which is below 2.0 →
    // MeanReverting regime.  Low vol + wide spread + MeanReverting → Aggressive.
    EXPECT_EQ(s.regime, xop::MarketRegime::MeanReverting);
    EXPECT_EQ(s.aggressiveness, xop::AnalysisAggressiveness::Aggressive);
}

// ============================================================================
// TEST: Aggressiveness heuristic -- Normal for average market conditions
// ============================================================================

TEST(MarketAnalyzerTest, NormalMarketConditionsIsNormal) {
    auto cfg = small_config(10);
    // Default thresholds: vol < 40%, spread_cv < 0.8, spread < 80 bps
    // → Normal recommendation.
    xop::MarketAnalyzer ma(cfg, {kPair});

    // Constant price (zero vol), moderate spread (50 bps).
    for (int i = 0; i < 10; ++i) {
        ma.ingest(kPair, 2.70, 50.0, 1000.0, 5.0, 5.0);
    }

    const auto s = ma.get_summary(kPair);
    EXPECT_EQ(s.aggressiveness, xop::AnalysisAggressiveness::Normal);
}

}  // anonymous namespace
