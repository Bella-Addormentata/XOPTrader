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
    return alpha_;
}

double AdverseSelectionEstimator::get_beta() const noexcept
{
    return beta_;
}

std::pair<double, double>
AdverseSelectionEstimator::get_credible_interval() const noexcept
{
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
    return total_fills_;
}

std::size_t AdverseSelectionEstimator::history_size() const noexcept
{
    return history_.size();
}

const std::deque<FillRecord>&
AdverseSelectionEstimator::history() const noexcept
{
    return history_;
}

const AdverseSelectionConfig&
AdverseSelectionEstimator::config() const noexcept
{
    return cfg_;
}

void AdverseSelectionEstimator::reset()
{
    history_.clear();
    total_fills_ = 0;
    alpha_ = cfg_.prior_alpha;
    beta_  = cfg_.prior_beta;
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
// A fill is classified as adverse if the maximum adverse move exceeds the
// configured threshold (default 30 bps = 0.003).

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

    const bool is_adverse = max_adverse_move > cfg_.adverse_threshold;

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
