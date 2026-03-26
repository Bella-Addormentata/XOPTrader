// volatility.cpp -- Implementation of the Yang-Zhang hybrid volatility
//                   estimator and variance-ratio regime detector.
//
// Mathematical foundations documented in volatility.hpp.  This file contains
// the numerical kernels with inline derivation comments at every non-trivial
// step so that the mathematics can be audited against the original papers.
//
// ISO/IEC 27001:2022  (no secrets)
// ISO/IEC 5055        (bounds-checked, no UB)
// ISO/IEC 25000       (annotated computations)

#include <xop/data/volatility.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <mutex>
#include <numeric>
#include <shared_mutex>

namespace xop {

// ===========================================================================
// Constants
// ===========================================================================

// MEDIUM-3: 365-day year, consistent with strategy sigma_block conversion.
// All strategy files (avellaneda.hpp, glft.hpp) use 365.0 * 24 * 3600 =
// 31,536,000.  Previously this used 365.25 (Julian year, 31,557,600) which
// caused a ~0.07% inconsistency in annualised volatility between the
// estimator and the strategy layer.  Standardised to 365.0 for uniformity.
static constexpr double kSecondsPerYear = 365.0 * 24.0 * 3600.0;  // 31,536,000

// ===========================================================================
// Construction
// ===========================================================================

VolatilityEstimator::VolatilityEstimator(const VolatilityEstimatorConfig& cfg)
    : cfg_(cfg)
{
    // Precompute the annualisation factor.
    //
    // blocks_per_year = seconds_per_year / block_time_seconds
    // sigma_annual    = sigma_block * sqrt(blocks_per_year)
    //
    // MEDIUM-3: With block_time = 52 s and 365-day year (31,536,000 s):
    //   blocks_per_year = 31,536,000 / 52 = 606,461.54...
    //   sqrt(blocks_per_year) = 778.89...
    const double blocks_per_year = kSecondsPerYear / cfg_.block_time_seconds;
    sqrt_blocks_per_year_ = std::sqrt(blocks_per_year);
}

// ===========================================================================
// update(Candle)
// ===========================================================================

double VolatilityEstimator::update(const Candle& candle)
{
    // T2-02: Exclusive lock -- update mutates candles_, sigma_block_,
    // sigma_annual_, variance_ratio_, and regime_.
    std::unique_lock lock(mtx_);

    // Append the new candle to the rolling window.
    candles_.push_back(candle);

    // Enforce the maximum window size by evicting the oldest candle.
    while (candles_.size() > cfg_.lookback_blocks) {
        candles_.pop_front();
    }

    // Recompute volatility if we have enough data; otherwise leave estimates
    // at their initial zero values.
    // Note: is_ready() accesses candles_ which is already under the lock;
    // the private helpers recompute_yang_zhang / recompute_variance_ratio /
    // classify_regime are called within the same exclusive lock scope.
    if (candles_.size() >= cfg_.min_candles) {
        recompute_yang_zhang();
        // MEDIUM-2 / T3-01: suppress deprecation for internal call --
        // VolatilityEstimator still uses its own VR/regime methods for
        // backward compatibility until all callers migrate to RegimeDetector.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        recompute_variance_ratio();
        classify_regime();
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
    }

    return sigma_block_;
}

// ===========================================================================
// update(double) -- convenience overload
// ===========================================================================

double VolatilityEstimator::update(double price)
{
    // Degenerate candle: a single tick with O = H = L = C.
    // The Rogers-Satchell component will be zero for degenerate candles,
    // but the overnight and close-to-open components remain informative.
    //
    // COUNTER-RESEARCH NOTE (CR-6, Molnár 2012):
    //   On CHIA, >90% of blocks have zero fills, making nearly all
    //   candles degenerate (O=H=L=C).  The Rogers-Satchell component
    //   contributes nothing, so Yang-Zhang degenerates into a simple
    //   close-to-close estimator and loses its minimum-variance
    //   advantage.  Garman-Klass (1980) is empirically competitive
    //   for continuous 24/7 markets without overnight gaps.
    //   TODO: consider constructing coarser-grained candles (e.g.,
    //   10-block windows ~8.7 min) to ensure most candles have at
    //   least one fill and meaningful OHLC variation.
    //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, §5.
    //
    // T2-02: Constructs the Candle locally and delegates to the Candle
    // overload which acquires the exclusive lock (no double-lock risk).
    return update(Candle{price, price, price, price});
}

// ===========================================================================
// Accessors
// ===========================================================================

double VolatilityEstimator::get_sigma_block() const noexcept
{
    // T2-02: Shared lock -- read-only access to cached estimate.
    std::shared_lock lock(mtx_);
    return sigma_block_;
}

double VolatilityEstimator::get_sigma_annual() const noexcept
{
    // T2-02: Shared lock -- read-only access to cached estimate.
    std::shared_lock lock(mtx_);
    return sigma_annual_;
}

RegimeInfo VolatilityEstimator::get_regime() const noexcept
{
    // T2-02: Shared lock -- read-only access to regime classification.
    std::shared_lock lock(mtx_);
    return regime_;
}

double VolatilityEstimator::get_variance_ratio() const noexcept
{
    // T2-02: Shared lock -- read-only access to VR statistic.
    std::shared_lock lock(mtx_);
    return variance_ratio_;
}

std::size_t VolatilityEstimator::candle_count() const noexcept
{
    // T2-02: Shared lock -- read-only access to candle buffer size.
    std::shared_lock lock(mtx_);
    return candles_.size();
}

bool VolatilityEstimator::is_ready() const noexcept
{
    // T2-02: Shared lock -- read-only access to candle buffer size.
    std::shared_lock lock(mtx_);
    return candles_.size() >= cfg_.min_candles;
}

const VolatilityEstimatorConfig& VolatilityEstimator::config() const noexcept
{
    // T2-02: Shared lock -- read-only access to immutable-after-construction config.
    std::shared_lock lock(mtx_);
    return cfg_;
}

// ===========================================================================
// Yang-Zhang computation
// ===========================================================================
//
// The Yang-Zhang (2000) estimator decomposes total variance into:
//
//   sigma_yz^2 = sigma_overnight^2 + sigma_close^2 + k * sigma_rs^2
//
// where:
//
//   sigma_overnight^2 = (1/n) * sum_{i=1}^{n} [ ln(O_i / C_{i-1}) ]^2
//                       (variance of the overnight return: this block's open
//                       relative to the previous block's close)
//
//   sigma_close^2     = (1/n) * sum_{i=1}^{n} [ ln(C_i / O_i) ]^2
//                       (variance of the close-to-open intraday return)
//
//   sigma_rs^2        = (1/n) * sum_{i=1}^{n} [ ln(H_i/C_i)*ln(H_i/O_i)
//                                              + ln(L_i/C_i)*ln(L_i/O_i) ]
//                       (Rogers-Satchell estimator, which is unbiased and
//                       drift-independent even when the mean return is nonzero)
//
//   k = alpha / (1.0 + alpha + (n+1)/(n-1))
//       where alpha is a configurable parameter (default 0.34).  This is the
//       optimal blending weight derived in Yang-Zhang Theorem 1 under the
//       assumption that drift is zero.
//
// The output sigma_yz is a per-candle-period volatility.  Since each candle
// spans one block, sigma_yz IS the per-block volatility directly.
//
// Annualised: sigma_annual = sigma_yz * sqrt(blocks_per_year).
//
// Two formulations of the overnight and close-to-open variances exist:
//
//   (A) Raw second moment:  (1/n) * sum(x_i^2)
//       -- This is what the strategy specification prescribes.
//
//   (B) Mean-subtracted:    (1/n) * sum(x_i^2) - mean(x)^2
//       -- This is the population variance used in the YZ paper (Eq. 6-7).
//
// Under the assumption of zero drift (mu = 0), both formulations are
// equivalent because mean(x) -> 0.  In practice the difference is
// negligible for short windows and small drift.  We implement the
// mean-subtracted form (B) from the original paper, which is the more
// general and correct formulation -- it remains unbiased even when
// the underlying has nonzero drift over the estimation window.

void VolatilityEstimator::recompute_yang_zhang()
{
    const std::size_t n_candles = candles_.size();

    // We need at least 2 candles to compute the overnight component
    // (O_i vs C_{i-1}).  The min_candles config should enforce this.
    if (n_candles < 2) {
        sigma_block_ = 0.0;
        sigma_annual_ = 0.0;
        return;
    }

    // ------------------------------------------------------------------
    // Step 1: Compute log-return series
    // ------------------------------------------------------------------
    //
    // n_valid tracks the number of observation pairs that pass the
    // zero/negative price guard.  Using n_valid (not the raw n_candles - 1)
    // as the denominator ensures the variance estimate is unbiased when
    // degenerate candles are skipped.

    // o_i = ln(O_i / C_{i-1})  -- overnight return
    // c_i = ln(C_i / O_i)      -- close-to-open (intraday) return
    // rs_i = ln(H_i/C_i)*ln(H_i/O_i) + ln(L_i/C_i)*ln(L_i/O_i)

    double sum_o  = 0.0;   // sum of overnight returns (for mean)
    double sum_c  = 0.0;   // sum of close-to-open returns (for mean)
    double sum_o2 = 0.0;   // sum of squared overnight returns
    double sum_c2 = 0.0;   // sum of squared close-to-open returns
    double sum_rs = 0.0;   // sum of Rogers-Satchell terms
    std::size_t n_valid = 0;  // count of pairs with valid (positive) prices

    for (std::size_t i = 1; i < n_candles; ++i) {
        const Candle& prev = candles_[i - 1];
        const Candle& curr = candles_[i];

        // Guard against zero or negative prices (would produce NaN/inf).
        // In degenerate cases, skip this observation entirely.
        if (prev.close <= 0.0 || curr.open <= 0.0 ||
            curr.close <= 0.0 || curr.high <= 0.0 || curr.low <= 0.0) {
            continue;
        }

        ++n_valid;

        // Overnight return: open of this candle vs close of previous candle.
        const double o_i = std::log(curr.open / prev.close);
        sum_o  += o_i;
        sum_o2 += o_i * o_i;

        // Close-to-open (intraday) return: close of this candle vs its open.
        const double c_i = std::log(curr.close / curr.open);
        sum_c  += c_i;
        sum_c2 += c_i * c_i;

        // Rogers-Satchell component for this candle.
        // RS_i = ln(H/C) * ln(H/O) + ln(L/C) * ln(L/O)
        //
        // This is unbiased for variance even in the presence of nonzero drift.
        const double log_hc = std::log(curr.high / curr.close);
        const double log_ho = std::log(curr.high / curr.open);
        const double log_lc = std::log(curr.low  / curr.close);
        const double log_lo = std::log(curr.low  / curr.open);
        sum_rs += log_hc * log_ho + log_lc * log_lo;
    }

    // If all observations were skipped (degenerate data), bail out.
    if (n_valid < 2) {
        sigma_block_ = 0.0;
        sigma_annual_ = 0.0;
        return;
    }

    // Use n_valid (not n_candles - 1) as the denominator so the variance
    // estimate accounts for skipped zero/negative-price observations.
    const auto n = static_cast<double>(n_valid);

    // ------------------------------------------------------------------
    // Step 2: Compute the three variance components
    // ------------------------------------------------------------------
    //
    // Mean-subtracted population variance:
    //   Var(x) = (1/n) * sum(x_i^2) - [ (1/n) * sum(x_i) ]^2
    //          = E[x^2] - E[x]^2
    //
    // This equals the raw second-moment formulation when the mean is zero
    // (i.e. no drift).  Under nonzero drift the mean-subtracted form is
    // the correct variance estimator.

    const double mean_o = sum_o / n;
    const double mean_c = sum_c / n;

    // sigma_overnight^2: variance of overnight (open-to-previous-close) returns.
    const double sigma_overnight_sq = (sum_o2 / n) - (mean_o * mean_o);

    // sigma_close^2: variance of close-to-open (intraday) returns.
    const double sigma_close_sq = (sum_c2 / n) - (mean_c * mean_c);

    // sigma_rs^2 = (1/n) * sum(RS_i)
    // Rogers-Satchell is already a variance estimator; we just average.
    const double sigma_rs_sq = sum_rs / n;

    // ------------------------------------------------------------------
    // Step 3: Compute the optimal blending weight k
    // ------------------------------------------------------------------
    //
    // From Yang-Zhang (2000), Theorem 1:
    //
    //   k = alpha / (1 + alpha + (n+1)/(n-1))
    //
    // where alpha is a free parameter.  The value alpha = 0.34 minimises
    // the variance of the combined estimator under the zero-drift assumption.
    //
    // For n = 199 (200 candles, 199 pairs):
    //   k = 0.34 / (1.34 + 200/198) = 0.34 / (1.34 + 1.0101) = 0.34 / 2.3501
    //     = 0.1447

    // Guard: when n == 1 (only 2 candles), (n-1) == 0 which makes the
    // (n+1)/(n-1) term infinite.  In this edge case k -> 0, meaning the
    // Rogers-Satchell component receives zero weight.  We handle this
    // explicitly to avoid IEEE infinity in the denominator.
    double k = 0.0;
    if (n > 1.0) {
        k = cfg_.yz_alpha
          / (1.0 + cfg_.yz_alpha + (n + 1.0) / (n - 1.0));
    }

    // ------------------------------------------------------------------
    // Step 4: Combine into the Yang-Zhang variance
    // ------------------------------------------------------------------
    //
    //   sigma_yz^2 = sigma_overnight^2 + sigma_close^2 + k * sigma_rs^2
    //
    // All three components are per-candle (i.e., per-block) variances.

    double sigma_yz_sq = sigma_overnight_sq + sigma_close_sq + k * sigma_rs_sq;

    // Clamp to zero: numerical noise can produce tiny negative values when
    // the market is perfectly flat.
    if (sigma_yz_sq < 0.0) {
        sigma_yz_sq = 0.0;
    }

    // ------------------------------------------------------------------
    // Step 5: Convert to standard deviations
    // ------------------------------------------------------------------

    sigma_block_  = std::sqrt(sigma_yz_sq);
    sigma_annual_ = sigma_block_ * sqrt_blocks_per_year_;
}

// ===========================================================================
// Variance-ratio test
// ===========================================================================
//
// The Lo-MacKinlay (1988) variance ratio:
//
//   VR(q) = Var(q-period returns) / (q * Var(1-period returns))
//
// Under the null hypothesis of a random walk (returns are i.i.d.), VR = 1.
//
//   VR < 1  =>  negative autocorrelation (mean-reversion)
//   VR > 1  =>  positive autocorrelation (momentum / trending)
//
// Implementation:
//   1. Compute 1-period log-returns: r_i = ln(C_i / C_{i-1}).
//   2. Compute q-period log-returns: r_i^q = ln(C_i / C_{i-q}) = sum of
//      q consecutive 1-period returns (but we compute directly from prices
//      to avoid accumulation error).
//   3. Compute sample variances of each series.
//   4. VR = Var(q-period) / (q * Var(1-period)).
//
// We use close prices for both the 1-period and q-period returns.

// MEDIUM-2 / T3-01: This local VR implementation is superseded by the shared
// canonical RegimeDetector.  Retained for backward compatibility; callers
// should migrate to RegimeDetector.  Suppress deprecation on the definition.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
void VolatilityEstimator::recompute_variance_ratio()
{
    const std::size_t n_candles = candles_.size();
    const auto q = static_cast<std::size_t>(cfg_.vr_q);

    // Need at least vr_window candles to run the test.  Also need at least
    // q + 1 candles to compute a single q-period return.
    if (n_candles < cfg_.vr_window || n_candles < q + 1 || q < 2) {
        variance_ratio_ = 1.0;
        return;
    }

    // Determine the effective window (use at most vr_window candles from the
    // end of the buffer).
    const std::size_t start = n_candles - cfg_.vr_window;

    // ------------------------------------------------------------------
    // Step 1: Compute 1-period log-returns over the window
    // ------------------------------------------------------------------
    //
    // r1[i] = ln(C_{start+i+1} / C_{start+i}),  i = 0, ..., (window-2)
    //
    // n1_valid tracks the count of non-degenerate observations so the
    // variance denominator is correct when zero/negative prices are skipped.

    double sum_r1  = 0.0;
    double sum_r1_sq = 0.0;
    std::size_t n1_valid = 0;

    {
        const std::size_t n1_total = cfg_.vr_window - 1;
        for (std::size_t i = 0; i < n1_total; ++i) {
            const double c_prev = candles_[start + i].close;
            const double c_curr = candles_[start + i + 1].close;

            if (c_prev <= 0.0 || c_curr <= 0.0) {
                continue;  // skip degenerate prices
            }

            const double r = std::log(c_curr / c_prev);
            sum_r1    += r;
            sum_r1_sq += r * r;
            ++n1_valid;
        }
    }

    // Need at least 2 valid returns for a meaningful variance.
    if (n1_valid < 2) {
        variance_ratio_ = 1.0;
        return;
    }

    const double dn1    = static_cast<double>(n1_valid);
    const double mean_r1 = sum_r1 / dn1;

    // Var(1-period) = (1/n1_valid) * sum(r1^2) - mean^2
    const double var_1 = (sum_r1_sq / dn1) - (mean_r1 * mean_r1);

    // ------------------------------------------------------------------
    // Step 2: Compute q-period log-returns over the window
    // ------------------------------------------------------------------
    //
    // rq[j] = ln(C_{start+j+q} / C_{start+j}),  j = 0, ..., (window-1-q)
    //
    // nq_valid tracks valid observations analogously to n1_valid.

    double sum_rq  = 0.0;
    double sum_rq_sq = 0.0;
    std::size_t nq_valid = 0;

    {
        const std::size_t nq_total = cfg_.vr_window - q;
        for (std::size_t j = 0; j < nq_total; ++j) {
            const double c_prev = candles_[start + j].close;
            const double c_curr = candles_[start + j + q].close;

            if (c_prev <= 0.0 || c_curr <= 0.0) {
                continue;
            }

            const double r = std::log(c_curr / c_prev);
            sum_rq    += r;
            sum_rq_sq += r * r;
            ++nq_valid;
        }
    }

    // Need at least 2 valid q-period returns for a meaningful variance.
    if (nq_valid < 2) {
        variance_ratio_ = 1.0;
        return;
    }

    const double dnq    = static_cast<double>(nq_valid);
    const double mean_rq = sum_rq / dnq;

    // Var(q-period) = (1/nq_valid) * sum(rq^2) - mean_rq^2
    const double var_q = (sum_rq_sq / dnq) - (mean_rq * mean_rq);

    // ------------------------------------------------------------------
    // Step 3: Compute VR(q)
    // ------------------------------------------------------------------
    //
    //   VR = Var(q-period) / (q * Var(1-period))
    //
    // If the 1-period variance is essentially zero (perfectly flat market),
    // default to VR = 1.0 to avoid division by zero.

    const double denom = static_cast<double>(q) * var_1;

    if (denom < 1e-20) {
        // Flat market: no meaningful autocorrelation signal.
        variance_ratio_ = 1.0;
    } else {
        variance_ratio_ = var_q / denom;
    }
}

// ===========================================================================
// Regime classification
// ===========================================================================

// MEDIUM-2 / T3-01: This local regime classifier is superseded by the shared
// canonical RegimeDetector.  Retained for backward compatibility; callers
// should migrate to RegimeDetector.
void VolatilityEstimator::classify_regime()
{
    // Classification thresholds from strategy document section 5:
    //   VR < 0.85  =>  mean-reverting  (tighten spreads 0.8x, reduce skew 0.5x)
    //   VR > 1.15  =>  momentum        (widen spreads 1.5x, aggressive skew 2.0x)
    //   else       =>  random walk     (neutral 1.0x)

    if (variance_ratio_ < cfg_.vr_mean_revert_threshold) {
        regime_ = RegimeInfo{
            MarketRegime::MeanReverting,
            variance_ratio_,
            0.80,   // tighten spreads to 80% of neutral
            0.50    // reduce inventory shedding to 50% of neutral
        };
    } else if (variance_ratio_ > cfg_.vr_momentum_threshold) {
        regime_ = RegimeInfo{
            MarketRegime::Momentum,
            variance_ratio_,
            1.50,   // widen spreads to 150% of neutral
            2.00    // aggressive inventory shedding at 200%
        };
    } else {
        regime_ = RegimeInfo{
            MarketRegime::Random,
            variance_ratio_,
            1.00,   // neutral spread multiplier
            1.00    // neutral skew multiplier
        };
    }
}
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace xop
