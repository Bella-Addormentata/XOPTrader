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
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++20)

#ifndef XOP_DATA_VOLATILITY_HPP
#define XOP_DATA_VOLATILITY_HPP

#include <xop/types.hpp>
#include <xop/strategy/base.hpp>   // MarketRegime, RegimeInfo

#include <cstdint>
#include <deque>
#include <shared_mutex>
#include <vector>

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

    /// [T5-CR6] Number of single-block ticks to aggregate into one OHLC
    /// candle before feeding the Yang-Zhang estimator.  Default 10 blocks
    /// (~8.7 min).  1 = no aggregation (legacy behaviour).
    std::uint32_t candle_aggregation_blocks{10};
};

// ---------------------------------------------------------------------------
// VolatilityEstimator -- maintains a rolling window of OHLC candles,
//                        computes Yang-Zhang volatility, and classifies the
//                        market regime via the variance-ratio test.
//
// Thread safety: thread-safe via std::shared_mutex (T2-02).
// Read operations (get_*, is_*, candle_count, config) acquire a shared lock.
// Write operations (update) acquire an exclusive lock.
// Follows the State class locking pattern from state.hpp.
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

    /// [T5-CR6] Ingest a single tick and aggregate into multi-block candles.
    ///
    /// Buffers individual price observations and produces a proper OHLC
    /// candle every `candle_aggregation_blocks` ticks.  This dramatically
    /// improves the Yang-Zhang estimator when >90% of single-block candles
    /// are degenerate (O=H=L=C) because it recovers meaningful open-high-
    /// low-close variation across the aggregation window.
    ///
    /// When candle_aggregation_blocks == 1, this is equivalent to
    /// update(double price) with zero overhead.
    ///
    /// @param price  Latest trade or mid-price.
    /// @return Current per-block volatility estimate (sigma_block).
    ///         The estimate is only updated when a full aggregated candle
    ///         is completed; intermediate ticks return the last-known sigma.
    double update_tick(double price);

    /// [AS-WARM 2026-08-01] Warm-start the estimator from persisted history.
    ///
    /// Before this existed, the candle buffer was memory-only: with one tick
    /// per ~19-minute engine heartbeat and min_candles(10) candles of
    /// candle_aggregation_blocks(10) ticks each, readiness required ~32 hours
    /// of UNINTERRUPTED uptime, and every restart reset the clock.  In
    /// production the estimator had essentially never been ready, so sigma
    /// permanently fell back to the configured floor (0.001) while realized
    /// annualized volatility measured from the same snapshot history was
    /// ~1.11 -- a ~1,100x understatement, squared into ~1.2e6x in any
    /// sigma^2 term.
    ///
    /// This method replays a persisted mid-price series through the exact
    /// same update_tick() aggregation pipeline the live feed uses, after
    /// resetting all rolling state, so the estimator is ready on the FIRST
    /// live tick after a restart.  `tick_seconds` is the measured spacing of
    /// the persisted ticks (e.g. the median inter-snapshot interval) and is
    /// applied via set_block_time_seconds() so annualisation reflects the
    /// true cadence of the data actually fed.
    ///
    /// @param mids          Mid prices in ASCENDING time order.  Non-finite
    ///                      and non-positive entries are skipped.
    /// @param tick_seconds  Observed spacing between consecutive mids, in
    ///                      seconds.  <= 0 leaves the current block time.
    /// @return Number of ticks actually ingested.
    std::size_t rehydrate_from_ticks(const std::vector<double>& mids,
                                     double tick_seconds);

    // -- Volatility accessors ------------------------------------------------

    /// Per-block volatility: the standard deviation of log-returns over one
    /// block interval (~52 s).  This is the native output of the Yang-Zhang
    /// estimator divided by sqrt(blocks_per_candle_period).
    double get_sigma_block() const noexcept;

    /// Annualised volatility.
    ///
    /// [AS-WARM 2026-08-01] Dimensional analysis (previously wrong for the
    /// aggregated-candle path):
    ///
    ///   sigma_block_ is the stdev of log-returns over ONE CANDLE PERIOD.
    ///   A candle spans candle_span_ticks ticks of block_time_seconds each:
    ///
    ///     candle_seconds  = block_time_seconds * candle_span_ticks
    ///     candles_per_year = seconds_per_year / candle_seconds     [1/year]
    ///     sigma_annual     = sigma_block * sqrt(candles_per_year)
    ///                        [1/sqrt(candle)] * [sqrt(candle/year)]
    ///                      = [1/sqrt(year)]                        OK
    ///
    /// The old code annualised with sqrt(seconds_per_year /
    /// block_time_seconds) regardless of aggregation, overstating sigma by
    /// sqrt(candle_aggregation_blocks) (= sqrt(10) ~ 3.16x at the production
    /// setting) on the update_tick() path.  candle_span_ticks is 1 for
    /// candles fed directly via update() and candle_aggregation_blocks for
    /// candles emitted by update_tick().
    ///
    /// MEDIUM-3: Uses 365-day year (31,536,000 s), consistent with strategy
    /// sigma_block conversion in avellaneda.hpp and glft.hpp.
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

    /// Number of consecutive blocks the current regime has persisted.
    /// Returns 1 on the first block after a regime transition.
    std::uint32_t get_regime_duration_blocks() const noexcept;

    /// Read-only access to the configuration.
    const VolatilityEstimatorConfig& config() const noexcept;

    /// [T4-15] Update the assumed block time (seconds) used for annualisation.
    ///
    /// Called by the engine when the adaptive block-time estimator produces
    /// a new EMA.  Recaches sqrt_blocks_per_year_ so that subsequent
    /// get_sigma_annual() calls reflect the observed cadence.
    ///
    /// @param seconds  New estimated mean inter-block interval; clamped
    ///                 to [10.0, 300.0] for safety.
    void set_block_time_seconds(double seconds) noexcept;

private:
    // -- Yang-Zhang computation helpers --------------------------------------

    /// Recompute the three YZ variance components and the combined estimate
    /// from the current candle window.  Called by update() after appending
    /// a new candle.
    void recompute_yang_zhang();

    /// Recompute the variance-ratio statistic from the rolling log-return
    /// buffer.  Called by update() after appending a new candle.
    ///
    /// MEDIUM-2 / T3-01: This local VR implementation is superseded by the
    /// shared canonical RegimeDetector (T3-01 consolidation).  Canonical
    /// regime detection should use RegimeDetector, which provides dual-horizon
    /// VR, Z-statistic significance testing (Lo-MacKinlay 1988), and
    /// hysteresis.  This method is retained for backward compatibility --
    /// existing callers (e.g. VolatilityEstimator::update()) still invoke it.
    /// New code should NOT rely on this; use RegimeDetector instead.
    [[deprecated("Use shared RegimeDetector (T3-01)")]]
    void recompute_variance_ratio();

    /// Classify the regime from the latest VR value and populate regime_.
    ///
    /// MEDIUM-2 / T3-01: This local regime classifier is superseded by the
    /// shared canonical RegimeDetector (T3-01 consolidation).  Canonical
    /// regime classification should use RegimeDetector, which provides
    /// configurable multipliers, hysteresis, and optional HMM.  This method
    /// is retained for backward compatibility -- existing callers (e.g.
    /// VolatilityEstimator::update()) still invoke it.
    /// New code should NOT rely on this; use RegimeDetector instead.
    [[deprecated("Use shared RegimeDetector (T3-01)")]]
    void classify_regime();

    // -- Thread safety (T2-02) -----------------------------------------------
    // Mutable to allow shared (read) locking in const accessor methods.
    // Follows the State class locking pattern: single mutex, no nesting.
    mutable std::shared_mutex mtx_;

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

    /// Number of consecutive blocks the current regime has persisted.
    std::uint32_t regime_duration_blocks_{0};

    /// Previous regime, used to detect transitions.
    MarketRegime last_regime_{MarketRegime::Random};

    // -- Precomputed constants -----------------------------------------------

    /// sqrt(blocks_per_year) = sqrt(365.0 * 24 * 3600 / block_time).
    /// MEDIUM-3: 365-day year, consistent with strategy sigma_block conversion.
    /// Cached at construction for annualisation.
    double sqrt_blocks_per_year_{0.0};

    /// [AS-WARM] Ticks spanned by each candle currently in the window: 1.0
    /// for candles fed directly via update(), candle_aggregation_blocks for
    /// candles emitted by update_tick().  Annualisation divides by
    /// sqrt(candle_span_ticks_) so sigma_annual reflects the true candle
    /// period (see get_sigma_annual() docs).
    double candle_span_ticks_{1.0};

    // -- [T5-CR6] Multi-block candle accumulator state -----------------------
    // Buffers individual ticks within the current aggregation window.
    // When agg_tick_count_ reaches cfg_.candle_aggregation_blocks, a proper
    // OHLC candle is constructed from the accumulated high/low/open/close
    // and fed into update(Candle).  Reset after each emission.

    double   agg_open_{0.0};         ///< First price in the aggregation window.
    double   agg_high_{0.0};         ///< Highest price in the window.
    double   agg_low_{0.0};          ///< Lowest price in the window.
    double   agg_close_{0.0};        ///< Most recent price in the window.
    uint32_t agg_tick_count_{0};     ///< Ticks accumulated so far.
};

}  // namespace xop

#endif  // XOP_DATA_VOLATILITY_HPP
