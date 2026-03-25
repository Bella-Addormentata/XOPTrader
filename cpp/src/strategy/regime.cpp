// regime.cpp -- Regime detection module for XOPTrader CHIA DEX market-making bot.
//
// Implements the variance ratio test (Lo & MacKinlay, 1988) and an optional
// three-state Gaussian Hidden Markov Model for market regime classification.
//
// Statistical notes:
//
//   The variance ratio VR(q) = Var(r_q) / (q * Var(r_1)) tests whether
//   returns follow a random walk.  Under the null hypothesis (i.i.d. returns),
//   VR(q) = 1.  Overlapping q-block returns are used because they are more
//   efficient (lower variance) than non-overlapping returns at the cost of
//   serial correlation that is accounted for in the asymptotic Z-statistic.
//
//   The Z-statistic formula:
//     Z = (VR(q) - 1) / sqrt( 2*(2q-1)*(q-1) / (3*q*n) )
//
//   is the homoskedasticity-consistent version from Lo & MacKinlay (1988),
//   Theorem 1.  For CHIA's ~52-second block returns, heteroskedasticity is
//   relatively mild, and this approximation is adequate.  The full
//   heteroskedasticity-robust version (Theorem 2) would require computing
//   sample autocovariances of squared returns, adding complexity without
//   materially improving classification accuracy at our window sizes.
//
// COUNTER-RESEARCH NOTE (CR-5, Lo & MacKinlay 1989):
//   The authors' own follow-up Monte Carlo study shows the VR test has
//   ~5–9% power to detect mean-reversion (VR=0.70) at n=50–200, which
//   are XOPTrader's typical window sizes.  This means regime
//   classification (mean-reverting / momentum / random-walk) relies on
//   raw VR thresholds (0.85, 1.15), not statistically significant
//   signals.  Richardson & Smith (1991) further show that overlapping
//   returns inflate apparent significance.
//
// COUNTER-RESEARCH NOTE (CR-14, Boldin 1996; Calvet & Fisher 2004):
//   Hamilton (1989) Markov regime-switching models suffer from
//   likelihood multimodality and regime identification fragility with
//   short-history crypto data (Boldin 1996).  Additionally, pure
//   Markov switching under-models multi-scale volatility dynamics;
//   multifractal models capture both short- and long-term regimes
//   more faithfully (Calvet & Fisher 2004).
//   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, §4 and §18.
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets processed; deterministic audit trail)
//   ISO/IEC 5055        (bounds-checked; no undefined behaviour; NaN guards)
//   ISO/IEC 25000       (tested public interface; clear error modes)
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++17)

#include "xop/strategy/regime.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace xop {

namespace {

/// Mathematical constant pi, defined locally for portability.
/// M_PI is not guaranteed by the C++ standard and requires _USE_MATH_DEFINES
/// on MSVC.  Using a constexpr constant avoids that dependency.
constexpr double kPi = 3.14159265358979323846;

}  // anonymous namespace

// ===========================================================================
// Construction
// ===========================================================================

RegimeDetector::RegimeDetector(const RegimeDetectorConfig& cfg)
    : cfg_(cfg)
{
    // -- Validate configuration invariants -----------------------------------

    if (cfg_.vr_q_short < 2) {
        throw std::invalid_argument(
            "RegimeDetector: vr_q_short must be >= 2, got "
            + std::to_string(cfg_.vr_q_short));
    }
    if (cfg_.vr_q_long < cfg_.vr_q_short) {
        throw std::invalid_argument(
            "RegimeDetector: vr_q_long (" + std::to_string(cfg_.vr_q_long)
            + ") must be >= vr_q_short (" + std::to_string(cfg_.vr_q_short)
            + ")");
    }
    if (cfg_.min_window_size < cfg_.vr_q_long + 1) {
        throw std::invalid_argument(
            "RegimeDetector: min_window_size must be > vr_q_long");
    }
    if (cfg_.max_window_size < cfg_.min_window_size) {
        throw std::invalid_argument(
            "RegimeDetector: max_window_size must be >= min_window_size");
    }
    if (cfg_.hysteresis_blocks < 1) {
        throw std::invalid_argument(
            "RegimeDetector: hysteresis_blocks must be >= 1");
    }
    if (cfg_.vr_lower_threshold <= 0.0 || cfg_.vr_lower_threshold >= 1.0) {
        throw std::invalid_argument(
            "RegimeDetector: vr_lower_threshold must be in (0, 1)");
    }
    if (cfg_.vr_upper_threshold <= 1.0) {
        throw std::invalid_argument(
            "RegimeDetector: vr_upper_threshold must be > 1.0");
    }
    if (cfg_.z_significance <= 0.0) {
        throw std::invalid_argument(
            "RegimeDetector: z_significance must be > 0");
    }

    // -- Initialise HMM with default parameters if enabled -------------------

    if (cfg_.hmm_enabled) {
        // Sensible initial parameters; these are overwritten by Baum-Welch
        // once enough data accumulates.  The initial transition matrix
        // favours staying in the current state (diagonal-dominant).
        hmm_.transition = {{
            {{0.90, 0.07, 0.03}},   // low-vol  -> {low, normal, high}
            {{0.05, 0.90, 0.05}},   // normal   -> {low, normal, high}
            {{0.03, 0.07, 0.90}}    // high-vol -> {low, normal, high}
        }};

        hmm_.initial_prob    = {0.25, 0.50, 0.25};
        hmm_.emission_mean   = {0.0, 0.0, 0.0};
        hmm_.emission_stddev = {0.001, 0.003, 0.008};  // low, normal, high vol
        hmm_.forward_prob    = hmm_.initial_prob;
        hmm_.log_likelihood  = 0.0;
        hmm_.fitted          = false;
    }
}

RegimeDetector::RegimeDetector()
    : RegimeDetector(RegimeDetectorConfig{})
{
}

// ===========================================================================
// Core interface
// ===========================================================================

void RegimeDetector::update(double log_return) {
    ++total_updates_;

    // -- Append return to rolling window, enforcing max size -----------------
    returns_.push_back(log_return);
    while (returns_.size() > cfg_.max_window_size) {
        returns_.pop_front();
    }

    // -- Increment regime duration counter -----------------------------------
    ++regime_duration_;

    // -- Guard: insufficient data for VR test --------------------------------
    if (returns_.size() < cfg_.min_window_size) {
        // Not enough data; remain in Normal regime.
        return;
    }

    // -- Compute variance ratios at both horizons ----------------------------
    last_vr_short_ = compute_vr(cfg_.vr_q_short);
    last_vr_long_  = compute_vr(cfg_.vr_q_long);

    // -- Compute Z-statistics for significance testing -----------------------
    const double z_short = compute_z(last_vr_short_, cfg_.vr_q_short,
                                     returns_.size());
    const double z_long  = compute_z(last_vr_long_, cfg_.vr_q_long,
                                     returns_.size());
    last_z_short_ = z_short;

    // -- Determine the raw (pre-hysteresis) regime signal --------------------
    const Regime new_signal = classify_vr(last_vr_short_, last_vr_long_,
                                          z_short, z_long);
    raw_signal_ = new_signal;

    // -- Hysteresis logic ----------------------------------------------------
    //
    // If the raw signal matches the current confirmed regime, reset the
    // pending counter (no transition in progress).
    //
    // If the raw signal differs from the confirmed regime but matches the
    // pending target, increment the counter.  Once the counter reaches
    // hysteresis_blocks, commit the transition.
    //
    // If the raw signal differs from BOTH the confirmed regime and the
    // pending target, restart the counter for the new signal.

    if (new_signal == confirmed_regime_) {
        // Stable -- no transition pending.  Reset any pending counter.
        pending_count_ = 0;
    } else if (new_signal == pending_regime_ && pending_count_ > 0) {
        // The raw signal consistently indicates the same pending target.
        ++pending_count_;

        if (pending_count_ >= cfg_.hysteresis_blocks) {
            // Transition confirmed: switch to the pending regime.
            confirmed_regime_ = pending_regime_;
            pending_count_ = 0;
            regime_duration_ = 1;  // Reset duration to 1 (this block).
        }
    } else {
        // The raw signal disagrees with both the confirmed regime AND the
        // previous pending target (or this is the first disagreement).
        // Start a new pending transition toward new_signal.
        pending_regime_ = new_signal;
        pending_count_ = 1;
    }

    // -- HMM forward step (if enabled) ---------------------------------------
    if (cfg_.hmm_enabled) {
        hmm_forward_step(log_return);

        // Periodically re-fit the HMM parameters via Baum-Welch.
        if (total_updates_ % kHmmRefitInterval == 0
            && returns_.size() >= cfg_.min_window_size) {
            hmm_baum_welch();
        }
    }
}

Regime RegimeDetector::get_regime() const noexcept {
    return confirmed_regime_;
}

RegimeMultipliers RegimeDetector::get_multipliers() const noexcept {
    switch (confirmed_regime_) {
        case Regime::MeanReverting: return cfg_.mr_multipliers;
        case Regime::Normal:        return cfg_.normal_multipliers;
        case Regime::Momentum:      return cfg_.momentum_multipliers;
    }
    // Defensive fallback (unreachable for valid enum values).
    return cfg_.normal_multipliers;
}

double RegimeDetector::get_variance_ratio(std::uint32_t q) const {
    if (q < 2 || returns_.size() < static_cast<std::size_t>(q) + 1) {
        return 1.0;  // Insufficient data or invalid q; assume random walk.
    }
    return compute_vr(q);
}

double RegimeDetector::get_confidence() const noexcept {
    return std::abs(last_z_short_);
}

int RegimeDetector::get_regime_duration_blocks() const noexcept {
    return static_cast<int>(regime_duration_);
}

// ===========================================================================
// Diagnostic accessors
// ===========================================================================

double RegimeDetector::get_z_statistic(std::uint32_t q) const {
    if (q < 2 || returns_.size() < static_cast<std::size_t>(q) + 1) {
        return 0.0;
    }
    const double vr = compute_vr(q);
    return compute_z(vr, q, returns_.size());
}

Regime RegimeDetector::get_raw_signal() const noexcept {
    return raw_signal_;
}

int RegimeDetector::get_pending_confirmation_count() const noexcept {
    return static_cast<int>(pending_count_);
}

std::size_t RegimeDetector::window_size() const noexcept {
    return returns_.size();
}

const RegimeDetectorConfig& RegimeDetector::config() const noexcept {
    return cfg_;
}

// ===========================================================================
// HMM accessors
// ===========================================================================

std::array<double, 3> RegimeDetector::get_hmm_state_probabilities()
    const noexcept
{
    if (!cfg_.hmm_enabled || !hmm_.fitted) {
        return {0.0, 0.0, 0.0};
    }
    return hmm_.forward_prob;
}

std::size_t RegimeDetector::get_hmm_most_likely_state() const noexcept {
    if (!cfg_.hmm_enabled || !hmm_.fitted) {
        return 1;  // Default to normal-vol state.
    }
    std::size_t best = 0;
    for (std::size_t s = 1; s < HmmState::kNumStates; ++s) {
        if (hmm_.forward_prob[s] > hmm_.forward_prob[best]) {
            best = s;
        }
    }
    return best;
}

std::vector<std::size_t> RegimeDetector::get_hmm_viterbi_path() const {
    if (!cfg_.hmm_enabled || !hmm_.fitted || returns_.empty()) {
        return {};
    }
    return hmm_viterbi();
}

// ===========================================================================
// Variance ratio helpers
// ===========================================================================

// ---------------------------------------------------------------------------
// compute_vr -- Compute VR(q) using overlapping returns.
//
// Given a sequence of n single-block log returns {r_1(1), r_1(2), ..., r_1(n)},
// construct overlapping q-block returns:
//
//   r_q(t) = r_1(t) + r_1(t-1) + ... + r_1(t-q+1)   for t = q-1, ..., n-1
//
// Then VR(q) = Var(r_q) / (q * Var(r_1)).
//
// We use sample variances (dividing by count-1) for both numerator and
// denominator.  The ratio is invariant to the choice of N or N-1 divisor
// because we use the same convention for both.
//
// NOTE ON OVERLAPPING VS NON-OVERLAPPING:
//   Overlapping returns are statistically more efficient (lower estimator
//   variance) because they use all available data.  The Lo-MacKinlay
//   Z-statistic is specifically designed for overlapping returns and
//   accounts for the induced serial correlation.  Using non-overlapping
//   returns would require a different asymptotic variance formula.
// ---------------------------------------------------------------------------

double RegimeDetector::compute_vr(std::uint32_t q) const {
    const std::size_t n = returns_.size();

    // Need at least q+1 single-block returns to form one q-block return
    // plus compute a meaningful variance.
    if (n < static_cast<std::size_t>(q) + 1 || q < 2) {
        return 1.0;
    }

    // -- Step 1: Compute Var(r_1) = sample variance of single-block returns --

    // Mean of single-block returns.
    double sum_r1 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum_r1 += returns_[i];
    }
    const double mean_r1 = sum_r1 / static_cast<double>(n);

    // Sample variance of single-block returns (Bessel-corrected, N-1).
    double ss_r1 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = returns_[i] - mean_r1;
        ss_r1 += d * d;
    }
    const double var_r1 = ss_r1 / static_cast<double>(n - 1);

    // Guard against degenerate case (zero variance => constant prices).
    if (var_r1 < std::numeric_limits<double>::epsilon()) {
        return 1.0;
    }

    // -- Step 2: Construct overlapping q-block returns -----------------------
    //
    // r_q(t) = sum_{i=0}^{q-1} r_1(t-i)  for t in [q-1, n-1]
    //
    // Number of overlapping q-block returns:
    const std::size_t m = n - q + 1;  // >= 2 (ensured by guard above)

    // Build overlapping returns via a sliding window sum.
    // Initialise with the first window.
    double window_sum = 0.0;
    for (std::size_t i = 0; i < q; ++i) {
        window_sum += returns_[i];
    }

    // Accumulate for variance computation.
    double sum_rq = window_sum;
    double ss_rq_raw = window_sum * window_sum;

    for (std::size_t t = 1; t < m; ++t) {
        // Slide the window: drop the oldest element, add the newest.
        window_sum += returns_[t + q - 1] - returns_[t - 1];
        sum_rq += window_sum;
        ss_rq_raw += window_sum * window_sum;
    }

    // Sample variance of q-block returns (Bessel-corrected, m-1).
    const double mean_rq = sum_rq / static_cast<double>(m);
    const double var_rq =
        (ss_rq_raw - static_cast<double>(m) * mean_rq * mean_rq)
        / static_cast<double>(m - 1);

    // Guard against negative variance from floating-point rounding.
    if (var_rq < 0.0) {
        return 1.0;
    }

    // -- Step 3: Compute VR(q) = Var(r_q) / (q * Var(r_1)) ------------------

    const double denominator = static_cast<double>(q) * var_r1;
    if (denominator < std::numeric_limits<double>::epsilon()) {
        return 1.0;
    }

    return var_rq / denominator;
}

// ---------------------------------------------------------------------------
// compute_z -- Z-statistic for the variance ratio test.
//
// Lo & MacKinlay (1988), Theorem 1 (homoskedasticity-consistent):
//
//   Z = (VR(q) - 1) / sqrt( 2*(2q-1)*(q-1) / (3*q*n) )
//
// where n is the number of single-block returns.
//
// Asymptotic distribution: Z ~ N(0, 1) under the random-walk null.
// Rejection at 95%: |Z| > 1.96.
// Rejection at 99%: |Z| > 2.576.
//
// Statistical correctness verification:
//   For q=5, n=100:
//     asymptotic_var = 2*(9)*(4) / (3*5*100) = 72/1500 = 0.048
//     se = sqrt(0.048) = 0.2191
//   If VR = 0.80 (mean-reverting):
//     Z = (0.80 - 1) / 0.2191 = -0.913
//     |Z| = 0.913 < 1.96 => NOT significant at 95%.
//   This means with n=100 single-block returns, a VR of 0.80 is not yet
//   statistically significant.  With n=200: se = 0.1549, Z = -1.29,
//   still not significant.  This is expected: detecting mean-reversion
//   with block-frequency data requires either larger windows or lower
//   confidence thresholds.  The hysteresis mechanism provides an
//   additional layer of confirmation that compensates for marginal Z.
// ---------------------------------------------------------------------------

double RegimeDetector::compute_z(double vr, std::uint32_t q, std::size_t n) {
    if (q < 2 || n < 2) {
        return 0.0;
    }

    const double q_d = static_cast<double>(q);
    const double n_d = static_cast<double>(n);

    // Asymptotic variance of VR(q) under the null.
    //   V = 2*(2q-1)*(q-1) / (3*q*n)
    const double numerator   = 2.0 * (2.0 * q_d - 1.0) * (q_d - 1.0);
    const double denominator = 3.0 * q_d * n_d;

    if (denominator < std::numeric_limits<double>::epsilon()) {
        return 0.0;
    }

    const double asymptotic_var = numerator / denominator;

    if (asymptotic_var <= 0.0) {
        return 0.0;
    }

    return (vr - 1.0) / std::sqrt(asymptotic_var);
}

// ---------------------------------------------------------------------------
// classify_vr -- Determine raw regime from VR values and Z-statistics.
//
// Logic:
//   1. If BOTH short and long horizon VR values are below the lower threshold
//      AND at least one Z-statistic exceeds the significance threshold
//      => MeanReverting.
//
//   2. If BOTH short and long horizon VR values are above the upper threshold
//      AND at least one Z-statistic exceeds the significance threshold
//      => Momentum.
//
//   3. Otherwise => Normal.
//
// Requiring agreement across two horizons reduces false positives from
// transient autocorrelation at a single frequency.
// ---------------------------------------------------------------------------

Regime RegimeDetector::classify_vr(double vr_short, double vr_long,
                                   double z_short, double z_long) const
{
    const bool short_below = (vr_short < cfg_.vr_lower_threshold);
    const bool long_below  = (vr_long  < cfg_.vr_lower_threshold);
    const bool short_above = (vr_short > cfg_.vr_upper_threshold);
    const bool long_above  = (vr_long  > cfg_.vr_upper_threshold);

    // Check statistical significance: at least one Z must exceed threshold.
    const bool z_significant =
        (std::abs(z_short) > cfg_.z_significance)
        || (std::abs(z_long) > cfg_.z_significance);

    // Mean-reverting: both horizons below lower threshold + significant.
    if (short_below && long_below && z_significant) {
        return Regime::MeanReverting;
    }

    // Momentum: both horizons above upper threshold + significant.
    if (short_above && long_above && z_significant) {
        return Regime::Momentum;
    }

    // Default: random walk / normal.
    return Regime::Normal;
}

// ===========================================================================
// HMM helpers
// ===========================================================================

// ---------------------------------------------------------------------------
// hmm_emission_prob -- Gaussian probability density N(x | mu, sigma).
//
// Returns the density, NOT the log-density.  Clamped to a minimum of 1e-300
// to prevent underflow in forward/Baum-Welch computations.
// ---------------------------------------------------------------------------

double RegimeDetector::hmm_emission_prob(double x, double mean, double stddev) {
    if (stddev <= 0.0) {
        return 1e-300;
    }
    const double z = (x - mean) / stddev;
    const double log_prob = -0.5 * z * z
                          - std::log(stddev)
                          - 0.5 * std::log(2.0 * kPi);
    // Clamp to prevent underflow.
    return std::max(std::exp(log_prob), 1e-300);
}

// ---------------------------------------------------------------------------
// hmm_initialise -- Set initial HMM emission parameters from data statistics.
//
// Uses the empirical mean and standard deviation of the return window to
// initialise the three states:
//   State 0 (low-vol):  mean = data_mean, stddev = 0.5 * data_stddev
//   State 1 (normal):   mean = data_mean, stddev = data_stddev
//   State 2 (high-vol): mean = data_mean, stddev = 2.0 * data_stddev
//
// Emission means are set identically because regime detection is primarily
// driven by volatility differences, not mean differences.
// ---------------------------------------------------------------------------

void RegimeDetector::hmm_initialise() {
    if (returns_.empty()) {
        return;
    }

    const std::size_t n = returns_.size();

    // Empirical mean.
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += returns_[i];
    }
    const double mean = sum / static_cast<double>(n);

    // Empirical standard deviation.
    double ss = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = returns_[i] - mean;
        ss += d * d;
    }
    const double stddev = (n > 1)
        ? std::sqrt(ss / static_cast<double>(n - 1))
        : 0.001;

    // Guard: minimum stddev to avoid degenerate Gaussians.
    const double safe_stddev = std::max(stddev, 1e-8);

    hmm_.emission_mean   = {mean, mean, mean};
    hmm_.emission_stddev = {
        0.5 * safe_stddev,   // low-vol
        safe_stddev,         // normal-vol
        2.0 * safe_stddev    // high-vol
    };

    // Reset forward probabilities to initial.
    hmm_.forward_prob = hmm_.initial_prob;
}

// ---------------------------------------------------------------------------
// hmm_forward_step -- Online forward algorithm for a single new observation.
//
// Updates the forward probabilities alpha[s] for each state s:
//
//   alpha'[j] = b_j(observation) * SUM_i{ alpha[i] * A[i][j] }
//
// Then normalise so that SUM_j alpha'[j] = 1 (to prevent underflow over
// many steps -- this gives the filtered posterior P(state_j | observations)).
// ---------------------------------------------------------------------------

void RegimeDetector::hmm_forward_step(double observation) {
    if (!hmm_.fitted && returns_.size() >= cfg_.min_window_size) {
        // First time we have enough data: initialise and fit.
        hmm_initialise();
        hmm_baum_welch();
        return;
    }

    if (!hmm_.fitted) {
        return;  // Not enough data yet.
    }

    constexpr std::size_t S = HmmState::kNumStates;

    std::array<double, S> new_alpha{};

    for (std::size_t j = 0; j < S; ++j) {
        double sum = 0.0;
        for (std::size_t i = 0; i < S; ++i) {
            sum += hmm_.forward_prob[i] * hmm_.transition[i][j];
        }
        new_alpha[j] = hmm_emission_prob(observation,
                                          hmm_.emission_mean[j],
                                          hmm_.emission_stddev[j]) * sum;
    }

    // Normalise to prevent underflow accumulation.
    double norm = 0.0;
    for (std::size_t j = 0; j < S; ++j) {
        norm += new_alpha[j];
    }
    if (norm > 0.0) {
        for (std::size_t j = 0; j < S; ++j) {
            new_alpha[j] /= norm;
        }
    } else {
        // Complete underflow: reset to uniform.
        for (std::size_t j = 0; j < S; ++j) {
            new_alpha[j] = 1.0 / static_cast<double>(S);
        }
    }

    hmm_.forward_prob = new_alpha;
}

// ---------------------------------------------------------------------------
// hmm_baum_welch -- Re-estimate HMM parameters via Expectation-Maximisation.
//
// Runs the Baum-Welch algorithm (forward-backward) on the entire return
// window for a fixed number of iterations (or until convergence).
//
// This is the batch version; it processes all returns in the rolling window.
// Called periodically (every kHmmRefitInterval blocks) rather than on every
// update to amortise the O(T * S^2) cost.
//
// After fitting, the forward probabilities are re-initialised by running
// the forward algorithm over the last few observations to maintain temporal
// continuity.
// ---------------------------------------------------------------------------

void RegimeDetector::hmm_baum_welch() {
    const std::size_t T = returns_.size();
    if (T < 2) {
        return;
    }

    constexpr std::size_t S = HmmState::kNumStates;

    // If not yet initialised from data, do so now.
    if (!hmm_.fitted) {
        hmm_initialise();
    }

    double prev_log_lik = -std::numeric_limits<double>::infinity();

    for (std::uint32_t iter = 0; iter < cfg_.hmm_em_iterations; ++iter) {

        // -- Forward pass ----------------------------------------------------
        // alpha[t][s] = P(o_1..o_t, state_t=s)
        // We normalise at each step to prevent underflow; the scaling factors
        // c[t] are used to recover the log-likelihood.

        std::vector<std::array<double, S>> alpha(T);
        std::vector<double> scale(T, 0.0);

        // t = 0
        for (std::size_t s = 0; s < S; ++s) {
            alpha[0][s] = hmm_.initial_prob[s]
                        * hmm_emission_prob(returns_[0],
                                             hmm_.emission_mean[s],
                                             hmm_.emission_stddev[s]);
            scale[0] += alpha[0][s];
        }
        if (scale[0] > 0.0) {
            for (std::size_t s = 0; s < S; ++s) {
                alpha[0][s] /= scale[0];
            }
        }

        // t = 1..T-1
        for (std::size_t t = 1; t < T; ++t) {
            for (std::size_t j = 0; j < S; ++j) {
                double sum = 0.0;
                for (std::size_t i = 0; i < S; ++i) {
                    sum += alpha[t - 1][i] * hmm_.transition[i][j];
                }
                alpha[t][j] = sum * hmm_emission_prob(returns_[t],
                                                       hmm_.emission_mean[j],
                                                       hmm_.emission_stddev[j]);
                scale[t] += alpha[t][j];
            }
            if (scale[t] > 0.0) {
                for (std::size_t j = 0; j < S; ++j) {
                    alpha[t][j] /= scale[t];
                }
            }
        }

        // Log-likelihood = SUM_t log(c_t).
        double log_lik = 0.0;
        for (std::size_t t = 0; t < T; ++t) {
            if (scale[t] > 0.0) {
                log_lik += std::log(scale[t]);
            }
        }

        // -- Convergence check -----------------------------------------------
        if (iter > 0 && (log_lik - prev_log_lik) < cfg_.hmm_convergence_tol) {
            break;
        }
        prev_log_lik = log_lik;

        // -- Backward pass ---------------------------------------------------
        // beta[t][s] = P(o_{t+1}..o_T | state_t=s), scaled.

        std::vector<std::array<double, S>> beta(T);

        // t = T-1
        for (std::size_t s = 0; s < S; ++s) {
            beta[T - 1][s] = 1.0;  // Normalised: beta[T-1] = 1/c[T-1].
            if (scale[T - 1] > 0.0) {
                beta[T - 1][s] /= scale[T - 1];
            }
        }

        // t = T-2..0
        for (std::size_t t = T - 2; t < T; --t) {  // Underflow wraps to large value, loop exits.
            for (std::size_t i = 0; i < S; ++i) {
                double sum = 0.0;
                for (std::size_t j = 0; j < S; ++j) {
                    sum += hmm_.transition[i][j]
                         * hmm_emission_prob(returns_[t + 1],
                                              hmm_.emission_mean[j],
                                              hmm_.emission_stddev[j])
                         * beta[t + 1][j];
                }
                beta[t][i] = sum;
            }
            // Scale beta[t] by 1/c[t].
            if (scale[t] > 0.0) {
                for (std::size_t i = 0; i < S; ++i) {
                    beta[t][i] /= scale[t];
                }
            }
        }

        // -- E-step: compute gamma and xi ------------------------------------
        // gamma[t][s] = P(state_t=s | observations) = alpha[t][s] * beta[t][s] * c[t]
        // xi[t][i][j] = P(state_t=i, state_{t+1}=j | observations)

        // Accumulators for M-step.
        std::array<double, S> gamma_sum{};        // SUM_t gamma[t][s]
        std::array<double, S> gamma_sum_obs{};    // SUM_t gamma[t][s] * o_t
        std::array<double, S> gamma_sum_sq{};     // SUM_t gamma[t][s] * (o_t - mu_s)^2
        std::array<std::array<double, S>, S> xi_sum{};  // SUM_t xi[t][i][j]

        for (std::size_t i = 0; i < S; ++i) {
            gamma_sum[i]     = 0.0;
            gamma_sum_obs[i] = 0.0;
            gamma_sum_sq[i]  = 0.0;
            for (std::size_t j = 0; j < S; ++j) {
                xi_sum[i][j] = 0.0;
            }
        }

        for (std::size_t t = 0; t < T; ++t) {
            // Compute gamma[t][s].
            std::array<double, S> gamma_t{};
            double gamma_norm = 0.0;
            for (std::size_t s = 0; s < S; ++s) {
                gamma_t[s] = alpha[t][s] * beta[t][s] * scale[t];
                gamma_norm += gamma_t[s];
            }
            if (gamma_norm > 0.0) {
                for (std::size_t s = 0; s < S; ++s) {
                    gamma_t[s] /= gamma_norm;
                }
            }

            for (std::size_t s = 0; s < S; ++s) {
                gamma_sum[s]     += gamma_t[s];
                gamma_sum_obs[s] += gamma_t[s] * returns_[t];
            }

            // Xi computation (only for t < T-1).
            if (t < T - 1) {
                double xi_norm = 0.0;
                std::array<std::array<double, S>, S> xi_t{};
                for (std::size_t i = 0; i < S; ++i) {
                    for (std::size_t j = 0; j < S; ++j) {
                        xi_t[i][j] = alpha[t][i]
                                   * hmm_.transition[i][j]
                                   * hmm_emission_prob(returns_[t + 1],
                                                        hmm_.emission_mean[j],
                                                        hmm_.emission_stddev[j])
                                   * beta[t + 1][j];
                        xi_norm += xi_t[i][j];
                    }
                }
                if (xi_norm > 0.0) {
                    for (std::size_t i = 0; i < S; ++i) {
                        for (std::size_t j = 0; j < S; ++j) {
                            xi_sum[i][j] += xi_t[i][j] / xi_norm;
                        }
                    }
                }
            }
        }

        // -- M-step: re-estimate parameters ----------------------------------

        // Update emission means.
        for (std::size_t s = 0; s < S; ++s) {
            if (gamma_sum[s] > 1e-10) {
                hmm_.emission_mean[s] = gamma_sum_obs[s] / gamma_sum[s];
            }
        }

        // Compute emission variance using updated means.
        for (std::size_t t = 0; t < T; ++t) {
            std::array<double, S> gamma_t{};
            double gamma_norm = 0.0;
            for (std::size_t s = 0; s < S; ++s) {
                gamma_t[s] = alpha[t][s] * beta[t][s] * scale[t];
                gamma_norm += gamma_t[s];
            }
            if (gamma_norm > 0.0) {
                for (std::size_t s = 0; s < S; ++s) {
                    gamma_t[s] /= gamma_norm;
                }
            }
            for (std::size_t s = 0; s < S; ++s) {
                const double d = returns_[t] - hmm_.emission_mean[s];
                gamma_sum_sq[s] += gamma_t[s] * d * d;
            }
        }

        // Update emission standard deviations with a floor.
        for (std::size_t s = 0; s < S; ++s) {
            if (gamma_sum[s] > 1e-10) {
                const double new_var = gamma_sum_sq[s] / gamma_sum[s];
                // Floor at 1e-10 to prevent degenerate zero-variance states.
                hmm_.emission_stddev[s] = std::sqrt(std::max(new_var, 1e-10));
            }
        }

        // Update transition matrix.
        for (std::size_t i = 0; i < S; ++i) {
            double row_sum = 0.0;
            for (std::size_t j = 0; j < S; ++j) {
                row_sum += xi_sum[i][j];
            }
            if (row_sum > 1e-10) {
                for (std::size_t j = 0; j < S; ++j) {
                    hmm_.transition[i][j] = xi_sum[i][j] / row_sum;
                }
            }
        }

        // Update initial state probabilities from gamma at t=0.
        {
            std::array<double, S> gamma_0{};
            double gamma_norm = 0.0;
            for (std::size_t s = 0; s < S; ++s) {
                gamma_0[s] = alpha[0][s] * beta[0][s] * scale[0];
                gamma_norm += gamma_0[s];
            }
            if (gamma_norm > 0.0) {
                for (std::size_t s = 0; s < S; ++s) {
                    hmm_.initial_prob[s] = gamma_0[s] / gamma_norm;
                }
            }
        }

        hmm_.log_likelihood = log_lik;
    }

    hmm_.fitted = true;

    // -- Sort states by emission stddev (ascending) --------------------------
    // Resolves the label-switching ambiguity inherent in EM: after
    // Baum-Welch converges, state indices have no guaranteed ordering.
    // Sorting by stddev ensures state 0 = low-vol, 1 = normal, 2 = high-vol,
    // which is required for correct regime classification downstream.
    // (T3-28: sort by stddev, not mean -- volatility level defines regime.)
    hmm_sort_states_by_stddev();

    // -- Re-run forward algorithm to set current state probabilities ---------
    // Run over the full window to ensure temporal consistency.
    hmm_.forward_prob = hmm_.initial_prob;
    for (std::size_t t = 0; t < T; ++t) {
        constexpr std::size_t S2 = HmmState::kNumStates;
        std::array<double, S2> new_fp{};
        for (std::size_t j = 0; j < S2; ++j) {
            double sum = 0.0;
            for (std::size_t i = 0; i < S2; ++i) {
                sum += hmm_.forward_prob[i] * hmm_.transition[i][j];
            }
            new_fp[j] = hmm_emission_prob(returns_[t],
                                           hmm_.emission_mean[j],
                                           hmm_.emission_stddev[j]) * sum;
        }
        double norm = 0.0;
        for (std::size_t j = 0; j < S2; ++j) {
            norm += new_fp[j];
        }
        if (norm > 0.0) {
            for (std::size_t j = 0; j < S2; ++j) {
                new_fp[j] /= norm;
            }
        }
        hmm_.forward_prob = new_fp;
    }
}

// ---------------------------------------------------------------------------
// hmm_sort_states_by_stddev -- Canonical ordering of HMM states post EM.
//
// The Baum-Welch algorithm is invariant to state permutations (the "label
// switching" problem): any permutation of the hidden state indices yields
// an identical likelihood.  Without a canonical ordering, the state index
// returned by get_hmm_most_likely_state() or the Viterbi path has no
// stable semantic meaning across successive re-fits.
//
// We sort states by ascending emission standard deviation so that:
//   state 0 = lowest  stddev  (low-volatility / quiet regime)
//   state 1 = middle  stddev  (normal-volatility regime)
//   state 2 = highest stddev  (high-volatility / turbulent regime)
//
// Sorting by stddev (not mean) is the correct criterion because regime
// classification is driven by volatility level, not return drift.  If the
// return distribution has non-zero drift, sorting by mean can misassign
// the low-vol label to a high-vol state that happens to have a lower mean.
//
// All HMM arrays (emission_mean, emission_stddev, initial_prob,
// transition matrix rows and columns, forward_prob) are permuted
// consistently according to the derived index mapping.
//
// Complexity: O(S^2) where S = kNumStates = 3.  Negligible.
// ---------------------------------------------------------------------------

void RegimeDetector::hmm_sort_states_by_stddev() {
    constexpr std::size_t S = HmmState::kNumStates;

    // -- Build permutation index sorted by ascending emission stddev ----------
    std::array<std::size_t, S> perm;
    for (std::size_t i = 0; i < S; ++i) {
        perm[i] = i;
    }
    std::sort(perm.begin(), perm.end(),
              [this](std::size_t a, std::size_t b) {
                  return hmm_.emission_stddev[a] < hmm_.emission_stddev[b];
              });

    // -- Check whether the permutation is already the identity ----------------
    // If so, no work is needed; skip the copy operations.
    bool already_sorted = true;
    for (std::size_t i = 0; i < S; ++i) {
        if (perm[i] != i) {
            already_sorted = false;
            break;
        }
    }
    if (already_sorted) {
        return;
    }

    // -- Apply the permutation to all per-state arrays ------------------------
    // Work on temporaries to avoid aliasing during the permutation.

    std::array<double, S> new_mean{};
    std::array<double, S> new_stddev{};
    std::array<double, S> new_initial{};
    std::array<double, S> new_forward{};

    for (std::size_t i = 0; i < S; ++i) {
        new_mean[i]    = hmm_.emission_mean[perm[i]];
        new_stddev[i]  = hmm_.emission_stddev[perm[i]];
        new_initial[i] = hmm_.initial_prob[perm[i]];
        new_forward[i] = hmm_.forward_prob[perm[i]];
    }

    hmm_.emission_mean   = new_mean;
    hmm_.emission_stddev = new_stddev;
    hmm_.initial_prob    = new_initial;
    hmm_.forward_prob    = new_forward;

    // -- Permute the transition matrix rows and columns -----------------------
    // A'[i][j] = A[perm[i]][perm[j]]
    // This preserves the transition semantics: the probability of moving
    // from new-state-i to new-state-j equals the probability of moving
    // from old-state-perm[i] to old-state-perm[j].

    std::array<std::array<double, S>, S> new_trans{};
    for (std::size_t i = 0; i < S; ++i) {
        for (std::size_t j = 0; j < S; ++j) {
            new_trans[i][j] = hmm_.transition[perm[i]][perm[j]];
        }
    }
    hmm_.transition = new_trans;
}

// ---------------------------------------------------------------------------
// hmm_viterbi -- Most likely state sequence via the Viterbi algorithm.
//
// Returns a vector of state indices (0, 1, or 2) of length T, where T is
// the current return window size.
// ---------------------------------------------------------------------------

std::vector<std::size_t> RegimeDetector::hmm_viterbi() const {
    const std::size_t T = returns_.size();
    if (T == 0) {
        return {};
    }

    constexpr std::size_t S = HmmState::kNumStates;

    // delta[t][s] = max log-probability of reaching state s at time t.
    // psi[t][s]   = argmax predecessor state.
    std::vector<std::array<double, S>> delta(T);
    std::vector<std::array<std::size_t, S>> psi(T);

    // -- Initialisation (t = 0) ----------------------------------------------
    for (std::size_t s = 0; s < S; ++s) {
        const double log_init = (hmm_.initial_prob[s] > 0.0)
            ? std::log(hmm_.initial_prob[s])
            : -1e30;
        const double log_emit = std::log(
            hmm_emission_prob(returns_[0],
                               hmm_.emission_mean[s],
                               hmm_.emission_stddev[s]));
        delta[0][s] = log_init + log_emit;
        psi[0][s]   = 0;
    }

    // -- Recursion (t = 1..T-1) ----------------------------------------------
    for (std::size_t t = 1; t < T; ++t) {
        for (std::size_t j = 0; j < S; ++j) {
            double best_val = -std::numeric_limits<double>::infinity();
            std::size_t best_i = 0;

            for (std::size_t i = 0; i < S; ++i) {
                const double log_trans = (hmm_.transition[i][j] > 0.0)
                    ? std::log(hmm_.transition[i][j])
                    : -1e30;
                const double val = delta[t - 1][i] + log_trans;
                if (val > best_val) {
                    best_val = val;
                    best_i   = i;
                }
            }

            const double log_emit = std::log(
                hmm_emission_prob(returns_[t],
                                   hmm_.emission_mean[j],
                                   hmm_.emission_stddev[j]));
            delta[t][j] = best_val + log_emit;
            psi[t][j]   = best_i;
        }
    }

    // -- Backtracking --------------------------------------------------------
    std::vector<std::size_t> path(T);

    // Find the best final state.
    double best_final = -std::numeric_limits<double>::infinity();
    std::size_t best_s = 0;
    for (std::size_t s = 0; s < S; ++s) {
        if (delta[T - 1][s] > best_final) {
            best_final = delta[T - 1][s];
            best_s     = s;
        }
    }
    path[T - 1] = best_s;

    // Trace back.
    for (std::size_t t = T - 2; t < T; --t) {  // unsigned underflow wraps; exits when t wraps.
        path[t] = psi[t + 1][path[t + 1]];
    }

    return path;
}

// ===========================================================================
// RegimeDetector-to-RegimeInfo bridge (T3-01)
// ===========================================================================

RegimeInfo to_regime_info(const RegimeDetector& detector) {
    // Map the canonical Regime enum to the consumer MarketRegime enum.
    MarketRegime market_regime = MarketRegime::Random;
    switch (detector.get_regime()) {
        case Regime::MeanReverting:
            market_regime = MarketRegime::MeanReverting;
            break;
        case Regime::Normal:
            market_regime = MarketRegime::Random;
            break;
        case Regime::Momentum:
            market_regime = MarketRegime::Momentum;
            break;
    }

    // Extract the multipliers from the canonical detector.
    const RegimeMultipliers mults = detector.get_multipliers();

    // Populate the RegimeInfo struct expected by strategy consumers.
    // variance_ratio: use the short-horizon VR value from the detector config.
    // spread_mult:    maps from RegimeMultipliers::spread_mult.
    // skew_mult:      maps from RegimeMultipliers::shedding_mult (the consumer
    //                 "skew_mult" controls inventory shedding aggressiveness,
    //                 which corresponds to the shedding_mult in the canonical
    //                 multiplier table).
    const double vr_short = detector.get_variance_ratio(
        detector.config().vr_q_short);

    return RegimeInfo{
        market_regime,
        vr_short,
        mults.spread_mult,
        mults.shedding_mult
    };
}

}  // namespace xop
