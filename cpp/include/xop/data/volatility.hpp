// volatility.hpp -- Yang-Zhang hybrid volatility estimator and variance-ratio
//                   regime detector for XOPTrader CHIA DEX market-making bot.
//
// The Yang-Zhang (2000) estimator combines three independent variance
// components -- overnight (open-to-previous-close), close-to-open (intraday),
// and Rogers-Satchell -- into a minimum-variance unbiased estimator of the
// true diffusion variance.  It is significantly more efficient than simple
// close-to-close estimators, requiring fewer observations for a given
// confidence level.
//
// Reference: Yang, D. & Zhang, Q. (2000). "Drift-independent volatility
//            estimation based on high, low, open and close prices."
//            Journal of Business, 73(3), 477-491.
//
// CHIA adaptation:
//   - "Candles" are constructed from block-level price data (52 s per block).
//   - The lookback window is configurable (default 200 blocks ~ 2.9 hours).
//   - Output is per-block volatility (sigma_block) and annualised volatility.
//
// Regime detection uses the Lo-MacKinlay (1988) variance ratio test:
//   VR(q) = Var(q-period returns) / (q * Var(1-period returns))
//
// Under a pure random walk, VR = 1.  Significant deviations indicate
// serial correlation:
//   VR < 0.85  =>  mean-reverting (negative autocorrelation)
//   VR > 1.15  =>  momentum / trending (positive autocorrelation)
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets; pure numerical computation)
//   ISO/IEC 5055        (no raw pointers; bounds-checked containers)
//   ISO/IEC 25000       (comprehensive mathematical documentation)
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++17)

#ifndef XOP_DATA_VOLATILITY_HPP
#define XOP_DATA_VOLATILITY_HPP

#include <xop/types.hpp>
#include <xop/strategy/base.hpp>   // MarketRegime, RegimeInfo

#include <cstdint>
#include <deque>

namespace xop {

// ---------------------------------------------------------------------------
// Candle -- OHLC price bar constructed from block-level data.
//
// For the CHIA DEX, each "candle" corresponds to one block interval (~52 s).
// When multiple trades occur within a block, the first fill price is the
// open and the last is the close.  High and low are the extremes within
// the block.  If no trade occurs in a block, the previous close carries
// forward to all four fields.
// ---------------------------------------------------------------------------

struct Candle {
    double open;    // First trade price within the block interval.
    double high;    // Highest trade price within the block interval.
    double low;     // Lowest trade price within the block interval.
    double close;   // Last trade price within the block interval.
};

// ---------------------------------------------------------------------------
// VolatilityConfig -- parameters for the Yang-Zhang estimator and the
//                     variance-ratio regime detector.
//
// Defaults are calibrated for CHIA at ~$2.70 XCH, 52-second blocks, and
// approximately 5% daily volatility (strategy document section 5 & 6).
// ---------------------------------------------------------------------------

struct VolatilityEstimatorConfig {
    // -- Yang-Zhang estimator ------------------------------------------------

    /// Rolling lookback window in blocks for volatility estimation.
    /// 200 blocks * 52 s = 10,400 s ~ 2.9 hours.
    std::uint32_t lookback_blocks{200};

    /// Yang-Zhang blending parameter (alpha).  The optimal weight k is
    /// computed as:
    ///
    ///   k = alpha / (1 + alpha + (n+1)/(n-1))
    ///
    /// where alpha defaults to 0.34 (the value that minimises variance of the
    /// combined estimator under the assumption of zero drift, per Yang-Zhang
    /// Theorem 1).  Configurable to allow calibration on live data.
    double yz_alpha{0.34};

    /// Mean CHIA inter-block interval in seconds.
    double block_time_seconds{52.0};

    /// Minimum number of candles required before the estimator produces a
    /// non-zero output.  Must be >= 2 (we need at least one pair of
    /// consecutive candles for the overnight component).
    std::uint32_t min_candles{10};

    // -- Variance-ratio regime detector --------------------------------------

    /// Number of single-period returns used in the VR denominator.
    /// Must be >= 2 * vr_q to produce a meaningful statistic.
    std::uint32_t vr_window{100};

    /// Aggregation period for the VR numerator.  VR(q) compares the
    /// variance of q-period returns to q times the variance of 1-period
    /// returns.  A value of q = 5 means we compare 5-block returns to
    /// 1-block returns.
    std::uint32_t vr_q{5};

    /// Threshold below which VR indicates mean-reversion.
    double vr_mean_revert_threshold{0.85};

    /// Threshold above which VR indicates momentum.
    double vr_momentum_threshold{1.15};
};

// ---------------------------------------------------------------------------
// VolatilityEstimator -- maintains a rolling window of OHLC candles,
//                        computes Yang-Zhang volatility, and classifies the
//                        market regime via the variance-ratio test.
//
// Thread safety: NOT thread-safe.  The caller (engine heartbeat loop)
// serialises access.  All internal state is private and mutation occurs
// only through the public update() method.
//
// Usage:
//     VolatilityEstimator vol(config);
//     for each block:
//         double sigma = vol.update(candle);
//         MarketRegime r = vol.get_regime().regime;
// ---------------------------------------------------------------------------

class VolatilityEstimator {
public:
    /// Construct with the given configuration.
    explicit VolatilityEstimator(const VolatilityEstimatorConfig& cfg);

    // -- Primary interface ---------------------------------------------------

    /// Ingest a new OHLC candle and recompute all estimates.
    ///
    /// @param candle  OHLC bar for the latest block interval.
    /// @return Current per-block volatility estimate (sigma_block).
    ///         Returns 0.0 if fewer than min_candles have been ingested.
    double update(const Candle& candle);

    /// Ingest a single price observation (convenience overload).
    /// Constructs a degenerate candle with O = H = L = C = price.
    /// Useful when only tick-level data is available (no intra-block OHLC).
    ///
    /// @param price  Latest trade or mid-price.
    /// @return Current per-block volatility estimate (sigma_block).
    double update(double price);

    // -- Volatility accessors ------------------------------------------------

    /// Per-block volatility: the standard deviation of log-returns over one
    /// block interval (~52 s).  This is the native output of the Yang-Zhang
    /// estimator divided by sqrt(blocks_per_candle_period).
    double get_sigma_block() const noexcept;

    /// Annualised volatility:
    ///   sigma_annual = sigma_block * sqrt(blocks_per_year)
    ///
    /// where blocks_per_year = seconds_per_year / block_time_seconds.
    /// With block_time = 52 s:
    ///   blocks_per_year = 365.25 * 24 * 3600 / 52 = 606,461.54...
    ///   sqrt(blocks_per_year) = 778.89...
    ///
    /// Note: we use 365.25 days/year to account for leap years.
    double get_sigma_annual() const noexcept;

    // -- Regime detection ----------------------------------------------------

    /// Current regime classification from the variance-ratio test.
    RegimeInfo get_regime() const noexcept;

    /// Raw variance ratio VR(q).  Returns 1.0 (random walk) if insufficient
    /// data is available.
    double get_variance_ratio() const noexcept;

    // -- Diagnostics ---------------------------------------------------------

    /// Number of candles currently in the rolling window.
    std::size_t candle_count() const noexcept;

    /// Whether the estimator has accumulated enough data to produce a
    /// non-trivial volatility estimate (candle_count() >= min_candles).
    bool is_ready() const noexcept;

    /// Read-only access to the configuration.
    const VolatilityEstimatorConfig& config() const noexcept;

private:
    // -- Yang-Zhang computation helpers --------------------------------------

    /// Recompute the three YZ variance components and the combined estimate
    /// from the current candle window.  Called by update() after appending
    /// a new candle.
    void recompute_yang_zhang();

    /// Recompute the variance-ratio statistic from the rolling log-return
    /// buffer.  Called by update() after appending a new candle.
    void recompute_variance_ratio();

    /// Classify the regime from the latest VR value and populate regime_.
    void classify_regime();

    // -- Configuration -------------------------------------------------------

    VolatilityEstimatorConfig cfg_;

    // -- Rolling candle window -----------------------------------------------

    /// Circular buffer of OHLC candles, capped at cfg_.lookback_blocks.
    std::deque<Candle> candles_;

    // -- Cached estimates ----------------------------------------------------

    /// Per-block standard deviation (sqrt of YZ combined variance per block).
    double sigma_block_{0.0};

    /// Annualised standard deviation.
    double sigma_annual_{0.0};

    /// Raw VR(q) statistic.
    double variance_ratio_{1.0};

    /// Current regime classification.
    RegimeInfo regime_{MarketRegime::Random, 1.0, 1.0, 1.0};

    // -- Precomputed constants -----------------------------------------------

    /// sqrt(blocks_per_year) = sqrt(365.25 * 24 * 3600 / block_time).
    /// Cached at construction for annualisation.
    double sqrt_blocks_per_year_{0.0};
};

}  // namespace xop

#endif  // XOP_DATA_VOLATILITY_HPP
