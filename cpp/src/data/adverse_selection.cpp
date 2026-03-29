// adverse_selection.cpp -- Implementation of the Bayesian PIN estimator.
//
// Mathematical foundations documented in adverse_selection.hpp.  This file
// contains the fill classification logic and Beta posterior updating.
//
// ISO/IEC 27001:2022  (no secrets)
// ISO/IEC 5055        (bounds-checked, no UB)
// ISO/IEC 25000       (annotated computations)

#include <xop/data/adverse_selection.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace xop {

// ===========================================================================
// Construction
// ===========================================================================

AdverseSelectionEstimator::AdverseSelectionEstimator(
    const AdverseSelectionConfig& cfg)
    : cfg_(cfg)
    , alpha_(cfg.prior_alpha)
    , beta_(cfg.prior_beta)
{
    // Validate hyperparameters.  Alpha and beta must be strictly positive
    // for the Beta distribution to be well-defined.
    if (cfg_.prior_alpha <= 0.0) {
        throw std::invalid_argument(
            "AdverseSelectionEstimator: prior_alpha must be > 0 (got "
            + std::to_string(cfg_.prior_alpha) + ")");
    }
    if (cfg_.prior_beta <= 0.0) {
        throw std::invalid_argument(
            "AdverseSelectionEstimator: prior_beta must be > 0 (got "
            + std::to_string(cfg_.prior_beta) + ")");
    }
    // Validate decay factor is in [0, 1).
    if (cfg_.decay_factor < 0.0 || cfg_.decay_factor >= 1.0) {
        if (cfg_.decay_factor != 0.0) {
            throw std::invalid_argument(
                "AdverseSelectionEstimator: decay_factor must be in [0, 1) (got "
                + std::to_string(cfg_.decay_factor) + ")");
        }
    }
}

// ===========================================================================
// record_fill
// ===========================================================================

void AdverseSelectionEstimator::record_fill(
    double fill_price,
    Side side,
    const std::vector<double>& subsequent_prices,
    BlockHeight block_height)
{
    // T2-02: Exclusive lock -- record_fill mutates history_, total_fills_,
    // alpha_, and beta_.
    std::unique_lock lock(mtx_);

    // Classify this fill as adverse or non-adverse.
    auto [is_adverse, max_adverse_move] =
        classify_fill(fill_price, side, subsequent_prices);

    // Create the fill record.
    FillRecord record{};
    record.fill_price   = fill_price;
    record.side         = side;
    record.adverse      = is_adverse;
    record.max_adverse  = max_adverse_move;
    record.block_height = block_height;

    // Append to the rolling history.
    history_.push_back(record);

    // Enforce the maximum history size by evicting the oldest record.
    while (history_.size() > cfg_.max_history) {
        history_.pop_front();
    }

    ++total_fills_;

    // Update the Beta posterior.
    recompute_posterior();
}

// ===========================================================================
// Estimator outputs
// ===========================================================================

double AdverseSelectionEstimator::get_pin() const noexcept
{
    // T2-02: Shared lock -- read-only access to alpha_ and beta_.
    std::shared_lock lock(mtx_);

    // Posterior mean of the Beta distribution:
    //
    //   E[PIN] = alpha / (alpha + beta)
    //
    // The Beta(alpha, beta) distribution has:
    //   mean     = alpha / (alpha + beta)
    //   mode     = (alpha - 1) / (alpha + beta - 2)  for alpha, beta > 1
    //   variance = alpha * beta / ((alpha + beta)^2 * (alpha + beta + 1))

    const double total = alpha_ + beta_;
    if (total <= 0.0) {
        return 0.0;  // degenerate case (should not happen with valid priors)
    }
    return alpha_ / total;
}

double AdverseSelectionEstimator::get_adverse_rate() const noexcept
{
    // T2-02: Shared lock -- read-only access to history_.
    std::shared_lock lock(mtx_);

    // Raw (non-Bayesian) adverse fill rate over the rolling window.
    if (history_.empty()) {
        return 0.0;
    }

    std::size_t adverse_count = 0;
    for (const auto& rec : history_) {
        if (rec.adverse) {
            ++adverse_count;
        }
    }

    return static_cast<double>(adverse_count)
         / static_cast<double>(history_.size());
}

double AdverseSelectionEstimator::get_alpha() const noexcept
{
    // T2-02: Shared lock -- read-only access to alpha_.
    std::shared_lock lock(mtx_);
    return alpha_;
}

double AdverseSelectionEstimator::get_beta() const noexcept
{
    // T2-02: Shared lock -- read-only access to beta_.
    std::shared_lock lock(mtx_);
    return beta_;
}

std::pair<double, double>
AdverseSelectionEstimator::get_credible_interval() const noexcept
{
    // T2-02: Shared lock -- read-only access to alpha_ and beta_.
    std::shared_lock lock(mtx_);

    // Normal approximation to the 95% credible interval of the Beta posterior.
    //
    // For Beta(alpha, beta):
    //   mean = alpha / (alpha + beta)
    //   var  = alpha * beta / ((alpha + beta)^2 * (alpha + beta + 1))
    //
    // 95% CI ~ mean +/- 1.96 * sqrt(var), clamped to [0, 1].
    //
    // This approximation is accurate when both alpha and beta are
    // sufficiently large (> 5).  For small counts the true Beta quantiles
    // should be used, but the normal approx suffices for our monitoring
    // purposes and avoids pulling in a special-functions library.

    const double total = alpha_ + beta_;
    if (total <= 0.0) {
        return {0.0, 1.0};
    }

    const double mean = alpha_ / total;
    const double var  = (alpha_ * beta_) / (total * total * (total + 1.0));

    // Guard against negative variance (impossible in theory but protect
    // against floating-point edge cases).
    if (var <= 0.0) {
        return {mean, mean};
    }

    const double sd = std::sqrt(var);

    // 1.96 standard deviations for 95% confidence.
    const double lower = std::max(0.0, mean - 1.96 * sd);
    const double upper = std::min(1.0, mean + 1.96 * sd);

    return {lower, upper};
}

// ===========================================================================
// Diagnostics
// ===========================================================================

std::uint64_t AdverseSelectionEstimator::total_fills() const noexcept
{
    // T2-02: Shared lock -- read-only access to total_fills_.
    std::shared_lock lock(mtx_);
    return total_fills_;
}

std::size_t AdverseSelectionEstimator::history_size() const noexcept
{
    // T2-02: Shared lock -- read-only access to history_.
    std::shared_lock lock(mtx_);
    return history_.size();
}

std::deque<FillRecord>
AdverseSelectionEstimator::history() const noexcept
{
    // T2-02: Shared lock -- read-only access to history_.
    // MEDIUM-4: Returns a COPY of history_ so the caller holds an
    // independent snapshot.  The previous implementation returned a const
    // reference that became unprotected once the shared_lock went out of
    // scope, creating a data race if a concurrent writer mutated history_
    // while the caller was iterating.
    // ISO/IEC 27001:2022: eliminate TOCTOU race on shared container.
    std::shared_lock lock(mtx_);
    return history_;
}

const AdverseSelectionConfig&
AdverseSelectionEstimator::config() const noexcept
{
    // T2-02: Shared lock -- read-only access to config.
    std::shared_lock lock(mtx_);
    return cfg_;
}

// ===========================================================================
// validate_predictive_power -- T5-CR4 cross-validation
// ===========================================================================
//
// Duarte & Young (2009) and Collin-Dufresne & Fos (2015) challenge PIN's
// ability to detect genuine informed trading.  This method checks how well
// the PIN level at each fill predicts the fill's adverse outcome, using the
// existing fill history as ground truth.
//
// For each fill, we use the rolling PIN *at that point in the history*
// (approximated by the cumulative adverse rate up to that fill), and check
// whether high-PIN fills were actually adverse more often than low-PIN fills.

AdverseSelectionEstimator::ValidationResult
AdverseSelectionEstimator::validate_predictive_power(
    double pin_threshold) const noexcept
{
    std::shared_lock lock(mtx_);

    ValidationResult result;
    result.sample_size = history_.size();

    if (history_.size() < 2) {
        return result;
    }

    // Build per-fill running PIN estimate and adverse outcome.
    std::size_t true_pos  = 0;  // high-PIN AND adverse
    std::size_t false_pos = 0;  // high-PIN AND non-adverse
    std::size_t false_neg = 0;  // low-PIN  AND adverse
    std::size_t total_adverse = 0;

    double running_alpha = cfg_.prior_alpha;
    double running_beta  = cfg_.prior_beta;

    // For Pearson correlation: accumulate sums.
    double sum_pin = 0.0, sum_adv = 0.0;
    double sum_pin2 = 0.0, sum_adv2 = 0.0, sum_pin_adv = 0.0;
    const auto n = static_cast<double>(history_.size());

    for (const auto& rec : history_) {
        const double pin_at_fill = running_alpha / (running_alpha + running_beta);
        const double adv_val = rec.adverse ? 1.0 : 0.0;
        const bool high_pin = pin_at_fill >= pin_threshold;

        if (high_pin && rec.adverse) ++true_pos;
        if (high_pin && !rec.adverse) ++false_pos;
        if (!high_pin && rec.adverse) ++false_neg;
        if (rec.adverse) ++total_adverse;

        sum_pin     += pin_at_fill;
        sum_adv     += adv_val;
        sum_pin2    += pin_at_fill * pin_at_fill;
        sum_adv2    += adv_val * adv_val;
        sum_pin_adv += pin_at_fill * adv_val;

        // Update running posterior (simple counting, no decay).
        if (rec.adverse) running_alpha += 1.0;
        else             running_beta  += 1.0;
    }

    // Precision = TP / (TP + FP).
    if (true_pos + false_pos > 0) {
        result.precision = static_cast<double>(true_pos)
                         / static_cast<double>(true_pos + false_pos);
    }
    // Recall = TP / (TP + FN).
    if (true_pos + false_neg > 0) {
        result.recall = static_cast<double>(true_pos)
                      / static_cast<double>(true_pos + false_neg);
    }

    // Pearson correlation between PIN and adverse outcome.
    const double denom_pin = n * sum_pin2 - sum_pin * sum_pin;
    const double denom_adv = n * sum_adv2 - sum_adv * sum_adv;
    if (denom_pin > 0.0 && denom_adv > 0.0) {
        result.correlation = (n * sum_pin_adv - sum_pin * sum_adv)
                           / (std::sqrt(denom_pin) * std::sqrt(denom_adv));
    }

    result.reliable = (result.sample_size >= 30);
    return result;
}

void AdverseSelectionEstimator::reset()
{
    // T2-02: Exclusive lock -- reset mutates all internal state.
    std::unique_lock lock(mtx_);
    history_.clear();
    total_fills_ = 0;
    alpha_ = cfg_.prior_alpha;
    beta_  = cfg_.prior_beta;
}

// ===========================================================================
// set_sigma_block -- update the per-block volatility for dynamic threshold
// ===========================================================================

void AdverseSelectionEstimator::set_sigma_block(double sigma_block) noexcept
{
    // T2-02: Exclusive lock -- set_sigma_block mutates sigma_block_.
    std::unique_lock lock(mtx_);
    sigma_block_ = sigma_block;
}

// ===========================================================================
// effective_adverse_threshold -- T3-27 dynamic threshold based on volatility
// ===========================================================================
//
// When a valid per-block volatility estimate is available (sigma_block > 0):
//
//   threshold = 1.5 * sigma_block * sqrt(observation_blocks)
//
// This scales the classification boundary with realised volatility so that
// in low-vol environments the threshold tightens (fewer false negatives) and
// in high-vol environments it widens (fewer false positives from random noise).
//
// The fixed 30 bps default classifies ~22% of random noise as "adverse" under
// typical conditions.  The dynamic formula maintains a consistent ~6.7% false-
// positive rate under a Gaussian random walk (1.5-sigma one-tail).
//
// Falls back to cfg_.adverse_threshold when sigma_block is unavailable.

double AdverseSelectionEstimator::effective_adverse_threshold() const noexcept
{
    // COUNTER-RESEARCH NOTE (CR-4, Duarte & Young 2009;
    //   Collin-Dufresne & Fos 2015):
    //   The PIN model may measure illiquidity friction rather than
    //   genuine informed trading.  Collin-Dufresne & Fos (2015) find
    //   PIN is lowest precisely when Schedule 13D filers (known informed
    //   traders) are most active — sophisticated informed traders
    //   minimise their market impact, reducing observable imbalance.
    //   Treat the resulting adverse-selection estimate as a rough
    //   heuristic, not a calibrated information-theoretic measure.
    //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, §6.

    if (sigma_block_ > 0.0 && cfg_.observation_blocks > 0) {
        // Dynamic threshold: 1.5 * sigma_block * sqrt(observation_blocks).
        // ISO/IEC 5055: observation_blocks is uint32_t, always non-negative.
        return 1.5 * sigma_block_
               * std::sqrt(static_cast<double>(cfg_.observation_blocks));
    }
    // Fallback: use the fixed threshold from configuration.
    return cfg_.adverse_threshold;
}

// ===========================================================================
// classify_fill -- determine if a fill was adversely selected
// ===========================================================================
//
// Adverse selection occurs when an informed trader takes our offer and the
// price subsequently moves against us:
//
//   - BID fill (we bought base): adverse if the price DROPS after our buy,
//     because the informed trader sold to us knowing the price would fall.
//     Adverse move = (fill_price - min_subsequent) / fill_price.
//
//   - ASK fill (we sold base): adverse if the price RISES after our sell,
//     because the informed trader bought from us knowing the price would rise.
//     Adverse move = (max_subsequent - fill_price) / fill_price.
//
// T3-27: A fill is classified as adverse if the maximum adverse move exceeds
// the effective threshold, which now scales dynamically with volatility when
// sigma_block is available, falling back to the fixed cfg_.adverse_threshold.

std::pair<bool, double> AdverseSelectionEstimator::classify_fill(
    double fill_price,
    Side side,
    const std::vector<double>& subsequent_prices) const
{
    if (subsequent_prices.empty() || fill_price <= 0.0) {
        // No subsequent data available; conservatively classify as
        // non-adverse (the prior will handle the uncertainty).
        return {false, 0.0};
    }

    double max_adverse_move = 0.0;

    if (side == Side::Bid) {
        // We bought (bid).  Adverse = price dropped after our buy.
        //
        // For each subsequent price p_j:
        //   adverse_move_j = (fill_price - p_j) / fill_price
        //                  = 1 - p_j / fill_price
        //
        // We track the worst (largest positive) adverse move.

        for (const double p : subsequent_prices) {
            if (p <= 0.0) continue;
            const double move = (fill_price - p) / fill_price;
            if (move > max_adverse_move) {
                max_adverse_move = move;
            }
        }
    } else {
        // We sold (ask).  Adverse = price rose after our sell.
        //
        // For each subsequent price p_j:
        //   adverse_move_j = (p_j - fill_price) / fill_price
        //
        // We track the worst (largest positive) adverse move.

        for (const double p : subsequent_prices) {
            if (p <= 0.0) continue;
            const double move = (p - fill_price) / fill_price;
            if (move > max_adverse_move) {
                max_adverse_move = move;
            }
        }
    }

    // T3-27: Use the dynamic volatility-scaled threshold when available,
    // falling back to the fixed cfg_.adverse_threshold otherwise.
    const double threshold = effective_adverse_threshold();
    const bool is_adverse = max_adverse_move > threshold;

    return {is_adverse, max_adverse_move};
}

// ===========================================================================
// recompute_posterior -- update the effective Beta posterior
// ===========================================================================
//
// Two modes:
//
// 1. No decay (decay_factor == 0): simple counting.
//      alpha = prior_alpha + count(adverse fills in history)
//      beta  = prior_beta  + count(non-adverse fills in history)
//
// 2. Exponential decay (0 < decay_factor < 1):
//      alpha = prior_alpha + sum_{i=0}^{n-1} decay^(n-1-i) * adverse_i
//      beta  = prior_beta  + sum_{i=0}^{n-1} decay^(n-1-i) * (1 - adverse_i)
//
//    where i=0 is the oldest observation and i=n-1 is the newest.
//    The newest observation has weight 1, the second-newest has weight
//    decay, the third-newest decay^2, and so on.
//
//    The half-life of the decay is:
//      h = -ln(2) / ln(decay_factor)
//
//    For decay_factor = 0.995:
//      h = -0.6931 / (-0.005013) = 138.3 fills
//
//    This means observations from ~138 fills ago contribute half the weight
//    of the most recent observation.

void AdverseSelectionEstimator::recompute_posterior()
{
    const bool use_decay =
        (cfg_.decay_factor > 0.0 && cfg_.decay_factor < 1.0);

    if (!use_decay) {
        // ------------------------------------------------------------------
        // Simple counting (no decay)
        // ------------------------------------------------------------------
        //
        // Count adverse and non-adverse fills in the current history window
        // and add to the prior.
        //
        // This is the standard Beta-Bernoulli conjugate update:
        //   posterior = Beta(alpha_0 + successes, beta_0 + failures)

        double adverse_count     = 0.0;
        double non_adverse_count = 0.0;

        for (const auto& rec : history_) {
            if (rec.adverse) {
                adverse_count += 1.0;
            } else {
                non_adverse_count += 1.0;
            }
        }

        alpha_ = cfg_.prior_alpha + adverse_count;
        beta_  = cfg_.prior_beta  + non_adverse_count;
    } else {
        // ------------------------------------------------------------------
        // Exponential decay
        // ------------------------------------------------------------------
        //
        // Iterate from newest to oldest, accumulating decayed weights.
        //
        //   weight_j = decay^(n-1-j)  for the j-th observation (0-indexed
        //              from oldest).
        //
        // Equivalently, iterating from the back (newest first):
        //   weight starts at 1.0 and is multiplied by decay_factor for each
        //   step backward in time.

        double decayed_adverse     = 0.0;
        double decayed_non_adverse = 0.0;
        double weight = 1.0;

        // Iterate from newest (back) to oldest (front).
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            if (it->adverse) {
                decayed_adverse += weight;
            } else {
                decayed_non_adverse += weight;
            }
            weight *= cfg_.decay_factor;
        }

        alpha_ = cfg_.prior_alpha + decayed_adverse;
        beta_  = cfg_.prior_beta  + decayed_non_adverse;
    }
}

}  // namespace xop
