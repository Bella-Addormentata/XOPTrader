// adverse_selection.hpp -- Bayesian Probability-of-Informed-Trading (PIN)
//                          estimator for XOPTrader CHIA DEX market-making bot.
//
// The PIN model quantifies the fraction of order flow that originates from
// informed traders (those who know something about future price direction that
// the market maker does not).  A higher PIN means more adverse selection risk,
// which should be reflected in wider spreads.
//
// Classical PIN estimation (Easley, Kiefer, O'Hara, and Paperman 1996)
// requires maximum-likelihood estimation over buyer/seller-initiated trade
// counts.  On CHIA's low-volume DEX, we use a simplified Bayesian approach:
//
//   1. After each of our fills, observe the price trajectory over the next
//      N blocks.
//   2. If the price moved adversely (against us) by more than a configurable
//      threshold, classify the fill as "informed" (adverse).
//   3. Maintain a Beta-distributed posterior over the adverse-fill rate:
//        Prior:     Beta(alpha_0, beta_0)  -- default Beta(2, 8) => E[PIN] = 0.20
//        Update:    adverse fill   => alpha += 1
//                   non-adverse    => beta  += 1
//        Estimate:  PIN = alpha / (alpha + beta)  (posterior mean)
//
// The Beta-Bernoulli conjugate model ensures closed-form updating with no
// iterative solver.  The prior encodes the strategy document's assumption
// that approximately 15-25% of fills are adversely selected.
//
// Reference: Easley, D., Kiefer, N., O'Hara, M. & Paperman, J. (1996).
//            "Liquidity, information, and infrequently traded stocks."
//            Journal of Finance, 51(4), 1405-1436.
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets; pure numerical computation)
//   ISO/IEC 5055        (no raw pointers; bounds-checked containers)
//   ISO/IEC 25000       (comprehensive documentation, clear naming)
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++20)

#ifndef XOP_DATA_ADVERSE_SELECTION_HPP
#define XOP_DATA_ADVERSE_SELECTION_HPP

#include <xop/types.hpp>

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// AdverseSelectionConfig -- parameters for the Bayesian PIN estimator.
// ---------------------------------------------------------------------------

struct AdverseSelectionConfig {
    // -- Beta prior hyperparameters ------------------------------------------

    /// Initial alpha (pseudo-count of adverse fills).
    /// Beta(2, 8) => prior mean PIN = 2/(2+8) = 0.20.
    double prior_alpha{2.0};

    /// Initial beta (pseudo-count of non-adverse fills).
    double prior_beta{8.0};

    // -- Adverse classification ----------------------------------------------

    /// Number of blocks after a fill to observe the subsequent price
    /// trajectory.  52 seconds per block; 10 blocks ~ 8.7 minutes.
    std::uint32_t observation_blocks{10};

    /// Minimum adverse price movement (as a fraction of fill price) required
    /// to classify a fill as "informed."
    ///
    /// For a BID fill (we bought), the price moving DOWN by more than this
    /// fraction is adverse.  For an ASK fill (we sold), the price moving UP
    /// by more than this fraction is adverse.
    ///
    /// Default 0.003 = 0.3% = 30 bps.  This is approximately one standard
    /// deviation of a single-block return at 5% daily volatility.
    double adverse_threshold{0.003};

    // -- Rolling window ------------------------------------------------------

    /// Maximum number of fill observations retained.  Older observations are
    /// discarded FIFO.  The Beta posterior accumulates over the full history,
    /// but the raw adverse_rate metric (non-Bayesian) is computed over only
    /// the most recent max_history fills.
    std::uint32_t max_history{500};

    // -- Decay (optional) ----------------------------------------------------

    /// If > 0, apply exponential decay to the posterior so that recent fills
    /// carry more weight than distant ones.  The posterior is scaled:
    ///
    ///   alpha_effective = prior_alpha + sum_{i} decay^(n-i) * adverse_i
    ///   beta_effective  = prior_beta  + sum_{i} decay^(n-i) * (1 - adverse_i)
    ///
    /// where i is the fill index and n is the latest fill.
    ///
    /// Set to 0.0 to disable decay (pure Bayesian counting).
    /// Typical value: 0.995 (half-life ~ 138 fills).
    double decay_factor{0.0};
};

// ---------------------------------------------------------------------------
// FillRecord -- internal record of a single fill observation and its
//               subsequent price trajectory classification.
// ---------------------------------------------------------------------------

struct FillRecord {
    double        fill_price;    // Execution price (quote per base).
    Side          side;          // Bid (we bought) or Ask (we sold).
    bool          adverse;       // True if subsequent prices moved against us.
    double        max_adverse;   // Largest adverse move observed (fractional).
    BlockHeight   block_height;  // Block at which the fill occurred.
};

// ---------------------------------------------------------------------------
// AdverseSelectionEstimator -- maintains a Bayesian posterior over the
//                               probability of informed trading (PIN).
//
// Thread safety: NOT thread-safe.  The caller (engine heartbeat loop)
// serialises access.
//
// Usage:
//     AdverseSelectionEstimator as(config);
//     // after each fill and observing subsequent prices:
//     as.record_fill(fill_price, side, subsequent_prices);
//     double pin = as.get_pin();
// ---------------------------------------------------------------------------

class AdverseSelectionEstimator {
public:
    /// Construct with the given configuration.  Validates prior
    /// hyperparameters (alpha, beta must be > 0).
    explicit AdverseSelectionEstimator(const AdverseSelectionConfig& cfg);

    // -- Primary interface ---------------------------------------------------

    /// Record a fill and classify it based on subsequent price observations.
    ///
    /// @param fill_price          Execution price (quote per base asset).
    /// @param side                Bid (we bought) or Ask (we sold).
    /// @param subsequent_prices   Prices observed in the blocks following the
    ///                            fill.  The vector should contain one price
    ///                            per block for cfg_.observation_blocks blocks.
    ///                            If fewer prices are available (e.g. recent
    ///                            fill), the available data is used.
    /// @param block_height        Block at which the fill settled.
    void record_fill(double fill_price,
                     Side side,
                     const std::vector<double>& subsequent_prices,
                     BlockHeight block_height = 0);

    // -- Estimator outputs ---------------------------------------------------

    /// Bayesian PIN estimate: posterior mean of the Beta distribution.
    ///
    ///   PIN = alpha / (alpha + beta)
    ///
    /// When decay is enabled, the effective alpha and beta incorporate
    /// exponential weighting of past observations.
    double get_pin() const noexcept;

    /// Raw adverse-fill rate over the rolling window (non-Bayesian):
    ///
    ///   adverse_rate = count(adverse fills) / count(all fills)
    ///
    /// Returns 0.0 if no fills have been recorded.
    double get_adverse_rate() const noexcept;

    /// Posterior alpha (effective, after optional decay).
    double get_alpha() const noexcept;

    /// Posterior beta (effective, after optional decay).
    double get_beta() const noexcept;

    /// 95% credible interval for PIN, computed from the Beta quantile
    /// function.  Returns (lower, upper).
    ///
    /// Uses the normal approximation to the Beta distribution:
    ///   mean = alpha / (alpha + beta)
    ///   var  = alpha * beta / ((alpha + beta)^2 * (alpha + beta + 1))
    ///   CI   = mean +/- 1.96 * sqrt(var)
    /// Clamped to [0, 1].
    std::pair<double, double> get_credible_interval() const noexcept;

    // -- Diagnostics ---------------------------------------------------------

    /// Total number of fills recorded (including those evicted from history).
    std::uint64_t total_fills() const noexcept;

    /// Number of fills currently in the rolling window.
    std::size_t history_size() const noexcept;

    /// Read-only access to the fill history (most recent max_history fills).
    const std::deque<FillRecord>& history() const noexcept;

    /// Read-only access to the configuration.
    const AdverseSelectionConfig& config() const noexcept;

    /// Reset the estimator to its prior state, clearing all fill history.
    void reset();

private:
    // -- Helpers -------------------------------------------------------------

    /// Classify whether a fill was adversely selected based on subsequent
    /// prices.  Returns (is_adverse, max_adverse_move).
    std::pair<bool, double> classify_fill(
        double fill_price,
        Side side,
        const std::vector<double>& subsequent_prices) const;

    /// Recompute the effective alpha and beta from the history, applying
    /// decay if configured.
    void recompute_posterior();

    // -- Configuration -------------------------------------------------------

    AdverseSelectionConfig cfg_;

    // -- Fill history --------------------------------------------------------

    /// Rolling window of classified fill observations.
    std::deque<FillRecord> history_;

    /// Total fills ever recorded (monotonically increasing).
    std::uint64_t total_fills_{0};

    // -- Posterior state ------------------------------------------------------

    /// Effective posterior alpha (after prior + observations + optional decay).
    double alpha_;

    /// Effective posterior beta (after prior + observations + optional decay).
    double beta_;
};

}  // namespace xop

#endif  // XOP_DATA_ADVERSE_SELECTION_HPP
