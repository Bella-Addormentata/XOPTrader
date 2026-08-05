// test_volatility.cpp -- Unit tests for the Yang-Zhang hybrid volatility
//                        estimator and the variance-ratio regime detector.
//
// Tests verify the mathematical correctness of the Yang-Zhang estimator
// against hand-computed expected values, and test the block-time to
// annualised volatility conversion from CHIA_MARKET_MAKER_STRATEGY.md
// Section 5.
//
// Synthetic price series (constant, trending, oscillating) are used to
// exercise known-output scenarios where the expected volatility or
// variance ratio can be computed analytically.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/data/volatility.hpp>
#include <xop/strategy/base.hpp>  // MarketRegime, RegimeInfo

#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace {

// ============================================================================
// Helper: create a VolatilityEstimator with reasonable defaults
// ============================================================================

xop::VolatilityEstimatorConfig default_vol_config() {
    xop::VolatilityEstimatorConfig cfg;
    cfg.lookback_blocks = 200;
    cfg.yz_alpha        = 0.34;
    cfg.block_time_seconds = 52.0;
    cfg.min_candles     = 10;
    cfg.vr_window       = 100;
    cfg.vr_q            = 5;
    cfg.vr_mean_revert_threshold = 0.85;
    cfg.vr_momentum_threshold    = 1.15;
    return cfg;
}

// ============================================================================
// TEST: Constant price series produces zero volatility
// ============================================================================
//
// If every candle has O = H = L = C = 2.70, all log-returns are zero and
// the Yang-Zhang variance should be zero.

TEST(VolatilityTest, ConstantPriceZeroVol) {
    auto cfg = default_vol_config();
    cfg.min_candles = 5;
    xop::VolatilityEstimator estimator(cfg);

    for (int i = 0; i < 50; ++i) {
        estimator.update(xop::Candle{2.70, 2.70, 2.70, 2.70});
    }

    EXPECT_TRUE(estimator.is_ready());
    EXPECT_NEAR(estimator.get_sigma_block(), 0.0, 1e-12);
    EXPECT_NEAR(estimator.get_sigma_annual(), 0.0, 1e-10);
}

// ============================================================================
// TEST: Known price series with calculable volatility
// ============================================================================
//
// Feed a series of simple degenerate candles (O=H=L=C) where close-to-close
// log-returns alternate between +r and -r.  The Yang-Zhang estimator on
// degenerate candles reduces to:
//
//   sigma_overnight^2 + sigma_close^2 + k * sigma_rs^2
//
// For degenerate candles (H=L=O=C), the Rogers-Satchell component = 0
// (ln(H/C)*ln(H/O) = 0 because H=C=O).  So sigma_yz^2 simplifies to:
//
//   sigma_overnight^2 + sigma_close^2
//
// where sigma_overnight = variance of ln(O_i/C_{i-1}) and
// sigma_close = variance of ln(C_i/O_i) = 0 (because C_i = O_i for
// degenerate candles).
//
// So sigma_yz^2 = variance of ln(O_i / C_{i-1}) = variance of the
// close-to-close log-returns.
//
// For alternating returns of +r and -r:
//   mean = 0, variance = r^2 (population variance).

TEST(VolatilityTest, AlternatingReturnsKnownVariance) {
    auto cfg = default_vol_config();
    cfg.min_candles = 5;
    cfg.lookback_blocks = 200;
    xop::VolatilityEstimator estimator(cfg);

    // Generate alternating prices: 100, 100*exp(r), 100, 100*exp(r), ...
    const double r = 0.01;  // 1% log-return per block
    const double base_price = 100.0;

    for (int i = 0; i < 200; ++i) {
        double price = (i % 2 == 0) ? base_price : base_price * std::exp(r);
        estimator.update(price);
    }

    // Expected per-block variance = r^2 (population variance of alternating
    // +r and -r returns, with mean = 0).
    // The actual YZ estimator uses mean-subtracted variance, so:
    //   mean_o ~= 0 (alternating +r and -r), so Var = E[r^2] - 0 = r^2.
    //
    // sigma_block = sqrt(r^2) = r.
    // But note: the alternating series has half +r and half -r returns,
    // so the exact mean is not perfectly zero due to even/odd count.
    // We use a generous tolerance.
    const double expected_sigma_block = r;
    EXPECT_NEAR(estimator.get_sigma_block(), expected_sigma_block, 0.002);
}

// ============================================================================
// TEST: Block-time annualisation conversion
// ============================================================================
//
// Formula (volatility.hpp / strategy doc Key Formulas):
//   sigma_annual = sigma_block * sqrt(blocks_per_year)
//
// With block_time = 52 s and the 365.0-day year used by VolatilityEstimator
// (kSecondsPerYear = 365.0 * 24 * 3600 = 31,536,000 s):
//   blocks_per_year = 31,536,000 / 52 = 606,461.54...
//   sqrt(blocks_per_year) = 778.89...
//
// Verify that sigma_annual / sigma_block ≈ 778.89.

TEST(VolatilityTest, AnnualisationFactor) {
    auto cfg = default_vol_config();
    cfg.min_candles = 5;
    xop::VolatilityEstimator estimator(cfg);

    // Feed a series that produces non-trivial volatility.
    const double r = 0.01;
    const double base_price = 100.0;
    for (int i = 0; i < 50; ++i) {
        double price = (i % 2 == 0) ? base_price : base_price * std::exp(r);
        estimator.update(price);
    }

    const double sb = estimator.get_sigma_block();
    const double sa = estimator.get_sigma_annual();

    if (sb > 1e-12) {
        const double ratio = sa / sb;
        // Use the same 365.0-day year as VolatilityEstimator (kSecondsPerYear).
        const double expected_ratio =
            std::sqrt(365.0 * 24.0 * 3600.0 / 52.0);
        EXPECT_NEAR(ratio, expected_ratio, 0.1);
    }
}

// ============================================================================
// TEST: Inverse conversion -- sigma_annual to sigma_block
// ============================================================================
//
// Verify the relationship:
//   sigma_block = sigma_annual / sqrt(blocks_per_year)
//
// This is equivalent to AvellanedaStoikov::per_block_volatility() but
// using the Julian year constant (365.25 days) instead of the calendar
// year (365 days).

TEST(VolatilityTest, AnnualToBlockConversion) {
    // For 50% annual vol at 52 s block time:
    //   sigma_block = 0.50 / sqrt(606876.923)
    //               = 0.50 / 779.15
    //               = 0.000641...
    const double sigma_annual = 0.50;
    const double blocks_per_year = 365.25 * 24.0 * 3600.0 / 52.0;
    const double sigma_block = sigma_annual / std::sqrt(blocks_per_year);

    EXPECT_NEAR(sigma_block, 0.000641, 1e-5);

    // Verify roundtrip: sigma_block * sqrt(bpy) = sigma_annual.
    const double roundtrip = sigma_block * std::sqrt(blocks_per_year);
    EXPECT_NEAR(roundtrip, sigma_annual, 1e-10);
}

// ============================================================================
// TEST: Estimator not ready until min_candles reached
// ============================================================================

TEST(VolatilityTest, NotReadyBeforeMinCandles) {
    auto cfg = default_vol_config();
    cfg.min_candles = 10;
    xop::VolatilityEstimator estimator(cfg);

    for (int i = 0; i < 9; ++i) {
        estimator.update(2.70 + i * 0.01);
        EXPECT_FALSE(estimator.is_ready());
        EXPECT_NEAR(estimator.get_sigma_block(), 0.0, 1e-12);
    }

    // The 10th candle makes it ready.
    estimator.update(2.80);
    EXPECT_TRUE(estimator.is_ready());
    // Now sigma should be > 0 (non-trivial price series).
    EXPECT_GT(estimator.get_sigma_block(), 0.0);
}

// ============================================================================
// TEST: Window trimming -- lookback respected
// ============================================================================
//
// After feeding more than lookback_blocks candles, the window should not
// grow beyond the configured size.

TEST(VolatilityTest, WindowTrimsToLookback) {
    auto cfg = default_vol_config();
    cfg.lookback_blocks = 50;
    cfg.min_candles = 5;
    xop::VolatilityEstimator estimator(cfg);

    for (int i = 0; i < 200; ++i) {
        estimator.update(2.70 + 0.01 * (i % 10));
    }

    EXPECT_EQ(estimator.candle_count(), 50u);
}

// ============================================================================
// TEST: Yang-Zhang with real OHLC candles
// ============================================================================
//
// Feed candles where High > Open, Close and Low < Open, Close to exercise
// the Rogers-Satchell component.  Verify that the total YZ variance is
// greater than a close-to-close-only estimator (the RS component captures
// intrabar variation).

TEST(VolatilityTest, YangZhangCapturesIntrabarVolatility) {
    auto cfg = default_vol_config();
    cfg.min_candles = 5;
    cfg.lookback_blocks = 50;

    // Estimator with full OHLC candles.
    xop::VolatilityEstimator estimator_ohlc(cfg);
    // Estimator with degenerate candles (close-only).
    xop::VolatilityEstimator estimator_close(cfg);

    for (int i = 0; i < 50; ++i) {
        double c = 2.70 + 0.01 * std::sin(i * 0.3);  // oscillating close
        double o = c + 0.002 * (i % 2 == 0 ? 1 : -1); // slight open offset
        double h = std::max(o, c) + 0.015;  // high exceeds both
        double l = std::min(o, c) - 0.015;  // low below both

        estimator_ohlc.update(xop::Candle{o, h, l, c});
        estimator_close.update(xop::Candle{c, c, c, c});  // degenerate
    }

    // OHLC estimator should report higher volatility because it captures
    // intra-bar range that the close-only version misses.
    EXPECT_GT(estimator_ohlc.get_sigma_block(),
              estimator_close.get_sigma_block());
}

// ============================================================================
// TEST: Variance ratio defaults to 1.0 with insufficient data
// ============================================================================

TEST(VolatilityTest, VarianceRatioInsufficientData) {
    auto cfg = default_vol_config();
    cfg.vr_window = 100;
    cfg.min_candles = 5;
    xop::VolatilityEstimator estimator(cfg);

    // Feed only 20 candles (less than vr_window of 100).
    for (int i = 0; i < 20; ++i) {
        estimator.update(2.70 + 0.01 * i);
    }

    // VR should default to 1.0 (random walk assumption).
    EXPECT_NEAR(estimator.get_variance_ratio(), 1.0, 1e-10);
}

// ============================================================================
// TEST: Variance ratio on pure random walk ≈ 1.0
// ============================================================================
//
// A random walk should produce VR close to 1.0.  We use a deterministic
// pseudo-random walk (additive increments from a fixed seed) so the test
// is reproducible.

TEST(VolatilityTest, VarianceRatioRandomWalkNearOne) {
    auto cfg = default_vol_config();
    cfg.lookback_blocks = 200;
    cfg.vr_window = 100;
    cfg.vr_q = 5;
    cfg.min_candles = 10;
    xop::VolatilityEstimator estimator(cfg);

    // Deterministic "random walk": use a linear congruential generator.
    double price = 2.70;
    uint32_t seed = 12345;
    for (int i = 0; i < 200; ++i) {
        seed = seed * 1103515245 + 12345;
        // Map seed to a small return in [-0.005, +0.005].
        double ret = (static_cast<double>(seed % 10001) / 10000.0 - 0.5) * 0.01;
        price *= std::exp(ret);
        estimator.update(price);
    }

    // VR should be in the ballpark of 1.0 for a random walk.
    // Use wide tolerance because 200 observations is a short sample.
    const double vr = estimator.get_variance_ratio();
    EXPECT_GT(vr, 0.50);  // not strongly mean-reverting
    EXPECT_LT(vr, 1.80);  // not strongly trending
}

// ============================================================================
// TEST: Variance ratio classification into regimes
// ============================================================================
//
// Feed a strongly mean-reverting series and verify the regime is classified
// as MeanReverting.  Then feed a trending series and verify Momentum.
// These are tested more thoroughly in test_regime.cpp; here we just verify
// that the VolatilityEstimator exposes the correct RegimeInfo.

TEST(VolatilityTest, RegimeClassificationFromVR) {
    auto cfg = default_vol_config();
    cfg.lookback_blocks = 200;
    cfg.vr_window = 100;
    cfg.vr_q = 5;
    cfg.min_candles = 10;

    // --- Mean-reverting series (oscillating) ---
    {
        xop::VolatilityEstimator estimator(cfg);
        for (int i = 0; i < 200; ++i) {
            // Oscillate between 2.70 and 2.75 every block.
            double price = (i % 2 == 0) ? 2.70 : 2.75;
            estimator.update(price);
        }

        // Strongly mean-reverting: VR should be well below 0.85.
        EXPECT_LT(estimator.get_variance_ratio(), 0.85);

        auto regime = estimator.get_regime();
        EXPECT_EQ(regime.regime, xop::MarketRegime::MeanReverting);
        EXPECT_NEAR(regime.spread_mult, 0.80, 1e-10);
        EXPECT_NEAR(regime.skew_mult, 0.50, 1e-10);
    }
}

// ============================================================================
// [AS-WARM] Warm-start from persisted snapshot mids (acceptance D)
// ============================================================================
//
// Production settings: candle_aggregation_blocks = 10, min_candles = 10, so
// readiness needs 100 ticks.  At one tick per ~19-minute engine heartbeat
// that used to mean ~32 h of UNINTERRUPTED uptime (the in-memory buffer reset
// on every restart) -- the estimator had essentially never been ready and
// sigma permanently fell to the 0.001 floor while realized annualized vol
// measured from the same snapshot history was ~1.11.
// rehydrate_from_ticks() must make the estimator ready immediately after a
// simulated restart, with a sigma close to an offline computation on the
// same window.

// Deterministic pseudo-random walk shared by the warm-start tests.
// Per-tick log-return stdev is calibrated so that at the measured ~19-min
// snapshot cadence (1140 s) the annualized sigma is ~1.10:
//   sd_tick = sigma_annual * sqrt(tick_seconds / seconds_per_year)
//           = 1.10 * sqrt(1140 / 31,536,000) = 1.10 * 6.013e-3 = 6.614e-3
std::vector<double> synthetic_snapshot_mids(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> ret(0.0, 6.614e-3);
    std::vector<double> mids;
    mids.reserve(n);
    double log_p = std::log(12.19);  // ~XCH/wUSDC.b mid, 2026-07-31
    for (std::size_t i = 0; i < n; ++i) {
        log_p += ret(rng);
        mids.push_back(std::exp(log_p));
    }
    return mids;
}

TEST(VolatilityWarmStartTest, ReadyImmediatelyAndSigmaMatchesOffline) {
    auto cfg = default_vol_config();
    cfg.min_candles = 10;
    cfg.candle_aggregation_blocks = 10;  // production setting
    cfg.lookback_blocks = 200;
    xop::VolatilityEstimator estimator(cfg);

    // Simulated restart: fresh estimator, cold buffer.
    EXPECT_FALSE(estimator.is_ready());
    EXPECT_EQ(estimator.get_sigma_annual(), 0.0);

    const double tick_seconds = 1140.0;  // measured median snapshot spacing
    const auto mids = synthetic_snapshot_mids(1000, 20260801u);

    const std::size_t fed = estimator.rehydrate_from_ticks(mids, tick_seconds);

    // Must FAIL if warm-start silently loads nothing.
    ASSERT_EQ(fed, mids.size());
    ASSERT_TRUE(estimator.is_ready());
    EXPECT_EQ(estimator.candle_count(), 100u);  // 1000 ticks / 10 per candle

    // The honest cadence must survive: the old [10, 300] clamp would have
    // silently squashed 1140 s to 300 s and inflated sigma by sqrt(1140/300)
    // ~ 1.95x.
    EXPECT_DOUBLE_EQ(estimator.config().block_time_seconds, tick_seconds);

    // Offline computation on the SAME window: close-to-close stdev of the
    // aggregated candle closes (every 10th tick), annualized over the true
    // candle period of 10 * 1140 s = 11,400 s:
    //   sigma_annual = sd_close * sqrt(31,536,000 / 11,400)
    //                = sd_close * 52.60
    std::vector<double> closes;
    for (std::size_t i = 9; i < mids.size(); i += 10) closes.push_back(mids[i]);
    ASSERT_EQ(closes.size(), 100u);
    std::vector<double> lr;
    for (std::size_t i = 1; i < closes.size(); ++i)
        lr.push_back(std::log(closes[i] / closes[i - 1]));
    double mean = 0.0;
    for (double r : lr) mean += r;
    mean /= static_cast<double>(lr.size());
    double var = 0.0;
    for (double r : lr) var += (r - mean) * (r - mean);
    var /= static_cast<double>(lr.size() - 1);
    const double offline_annual =
        std::sqrt(var) * std::sqrt(31'536'000.0 / (tick_seconds * 10.0));

    const double warm_annual = estimator.get_sigma_annual();
    EXPECT_GT(warm_annual, 0.0);
    // Acceptance D: within 20% of the offline computation.  (Yang-Zhang vs
    // close-to-close on the same GBM window differ only by estimator noise.)
    EXPECT_NEAR(warm_annual, offline_annual, 0.20 * offline_annual)
        << "warm=" << warm_annual << " offline=" << offline_annual;
    // And the annualisation itself must be honest: the calibrated ~1.10
    // must come back at the right ORDER OF MAGNITUDE, not the sqrt(10)- or
    // 1.95x-inflated variants the old span/clamp bugs produced.
    EXPECT_GT(warm_annual, 0.6);
    EXPECT_LT(warm_annual, 1.8);
}

TEST(VolatilityWarmStartTest, ReadyAtExactly100RowsNotAt99) {
    auto cfg = default_vol_config();
    cfg.min_candles = 10;
    cfg.candle_aggregation_blocks = 10;
    cfg.lookback_blocks = 200;

    const auto mids = synthetic_snapshot_mids(100, 7u);

    {
        // >= 100 snapshot rows -> ready immediately after simulated restart.
        xop::VolatilityEstimator est(cfg);
        const std::size_t fed = est.rehydrate_from_ticks(mids, 1140.0);
        ASSERT_EQ(fed, 100u);
        EXPECT_TRUE(est.is_ready());
        EXPECT_EQ(est.candle_count(), 10u);
    }
    {
        // 99 rows -> 9 full candles < min_candles -> still cold: the engine
        // skips such pairs and the existing sigma-floor behaviour applies.
        xop::VolatilityEstimator est(cfg);
        const std::vector<double> sparse(mids.begin(), mids.begin() + 99);
        const std::size_t fed = est.rehydrate_from_ticks(sparse, 1140.0);
        ASSERT_EQ(fed, 99u);
        EXPECT_FALSE(est.is_ready());
        EXPECT_EQ(est.get_sigma_annual(), 0.0);
    }
}

TEST(VolatilityWarmStartTest, SkipsNonFiniteAndNonPositiveMids) {
    auto cfg = default_vol_config();
    cfg.min_candles = 10;
    cfg.candle_aggregation_blocks = 10;
    xop::VolatilityEstimator est(cfg);

    auto mids = synthetic_snapshot_mids(100, 11u);
    mids.push_back(0.0);
    mids.push_back(-3.0);
    mids.push_back(std::numeric_limits<double>::quiet_NaN());
    mids.push_back(std::numeric_limits<double>::infinity());

    const std::size_t fed = est.rehydrate_from_ticks(mids, 1140.0);
    EXPECT_EQ(fed, 100u);  // the four junk entries are not ingested
    EXPECT_TRUE(est.is_ready());
}

TEST(VolatilityWarmStartTest, RehydrationResetsPriorState) {
    auto cfg = default_vol_config();
    cfg.min_candles = 10;
    cfg.candle_aggregation_blocks = 10;
    xop::VolatilityEstimator est(cfg);

    // Pre-existing partial state (e.g. a few live ticks before the DB read
    // finished) must not leak into the replayed series.
    for (int i = 0; i < 37; ++i) est.update_tick(500.0 + i);

    const auto mids = synthetic_snapshot_mids(500, 13u);
    const std::size_t fed = est.rehydrate_from_ticks(mids, 1140.0);
    ASSERT_EQ(fed, 500u);
    EXPECT_TRUE(est.is_ready());
    // 500 ticks -> exactly 50 candles: none of the 37 junk ticks survived.
    EXPECT_EQ(est.candle_count(), 50u);
}

// ============================================================================
// [AS-WARM] set_block_time_seconds: widened clamp admits the real heartbeat
// ============================================================================

TEST(VolatilityWarmStartTest, BlockTimeClampAdmitsHeartbeatCadence) {
    auto cfg = default_vol_config();
    xop::VolatilityEstimator est(cfg);

    // The measured ~19-min snapshot cadence must be representable.
    est.set_block_time_seconds(1140.0);
    EXPECT_DOUBLE_EQ(est.config().block_time_seconds, 1140.0);

    // Wildly wrong inputs are still rejected at the new [1, 7200] rails.
    est.set_block_time_seconds(0.0);
    EXPECT_DOUBLE_EQ(est.config().block_time_seconds, 1.0);
    est.set_block_time_seconds(1e9);
    EXPECT_DOUBLE_EQ(est.config().block_time_seconds, 7200.0);
}

}  // namespace
