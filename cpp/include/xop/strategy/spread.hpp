// spread.hpp -- Four-component spread optimization engine for XOPTrader CHIA
//               DEX market-making bot.
//
// Implements the spread model described in CHIA_MARKET_MAKER_STRATEGY.md S6:
//
//     spread = s_adverse + s_inventory + s_cost + s_competition
//
// Each component is calibrated to CHIA-specific parameters (52-second blocks,
// ~$2K daily DEX volume, 5% daily volatility, PIN range 0.10-0.25).
//
// An optional Thompson Sampling module learns profitable spread levels from
// live fill outcomes using Beta-distribution posteriors over a discrete grid.
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets in spread data, audit-friendly outputs)
//   ISO/IEC 5055        (bounds-checked arithmetic, no UB on edge cases)
//   ISO/IEC 25000       (clear naming, fully documented public interface)
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++20)

#ifndef XOP_STRATEGY_SPREAD_HPP
#define XOP_STRATEGY_SPREAD_HPP

#include <cstdint>
#include <array>
#include <deque>
#include <optional>
#include <random>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// Venue -- DEX platform identifier for fee lookups.
// ---------------------------------------------------------------------------
enum class Venue : std::uint8_t {
    Dexie      = 0,   // 0% fee on regular offers
    TibetSwap  = 1,   // 0.7% fee
    Hashgreen  = 2,   // 0.9% fee
    OfferBin   = 3,   // 0% (bulletin board)
    Splash     = 4    // 0% (P2P protocol)
};

/// Return the aggregator fee as a decimal fraction for a given venue.
/// E.g. Dexie -> 0.0, TibetSwap -> 0.007, Hashgreen -> 0.009.
double venue_fee_fraction(Venue v) noexcept;

// ---------------------------------------------------------------------------
// VolatilityRegime -- qualitative regime label used by the dynamic multiplier.
// ---------------------------------------------------------------------------
enum class VolatilityRegime : std::uint8_t {
    Low    = 0,   // sigma < 0.7 * sigma_7d_avg  -> tighten 30%
    Normal = 1,   // within 0.7-1.3x of rolling average
    High   = 2    // sigma > 1.3 * sigma_7d_avg  -> widen 80%
};

// ---------------------------------------------------------------------------
// SpreadResult -- fully decomposed output of compute_spread().
//
// All spread values are in basis points (bps). 1 bps = 0.01%.
// half_spread = total_spread_bps / 2, representing the distance from
// mid-price to a single side's quote.
// ---------------------------------------------------------------------------
struct SpreadResult {
    double total_spread_bps;   // Final spread after all adjustments.
    double half_spread;        // total_spread_bps / 2.

    // Individual components (pre-multiplier, in bps).
    double s_adverse;          // Adverse selection component.
    double s_inventory;        // Inventory risk component.
    double s_cost;             // Transaction cost component.
    double s_competition;      // Competitive undercut cap (0 = no cap).

    // Multiplier applied to the raw sum of components.
    double regime_multiplier;  // 1.0 = neutral; <1 tightened; >1 widened.

    // -- Stoll (1989) three-component decomposition (T5-CR13) -----------------
    // Component fractions (each in [0, 1], summing to 1) quantifying which
    // factor dominates the current spread.  On thin DEX markets, order-
    // processing costs (blockchain fees, offer TTL) and inventory costs
    // (capital lockup) are often dominant over adverse selection — contrary
    // to the Glosten-Milgrom (1985) assumption that adverse selection is
    // the primary spread determinant.
    //
    // These fractions let the engine tune behaviour by dominant factor:
    //   - High frac_adverse  → widen more, reduce size (informed flow risk)
    //   - High frac_inventory → shed inventory, skew quotes
    //   - High frac_cost     → venue-hop to cheaper platforms, increase TTL
    double frac_adverse{0.0};       // s_adverse / raw_sum
    double frac_inventory{0.0};     // s_inventory / raw_sum
    double frac_cost{0.0};          // s_cost / raw_sum
};

// ---------------------------------------------------------------------------
// ThompsonSamplerConfig -- parameters that control the optional Thompson
//                          Sampling spread-learning module.
// ---------------------------------------------------------------------------
struct ThompsonSamplerConfig {
    /// Discrete spread levels (bps) to explore.
    /// Default grid: {30, 40, 50, 60, 80, 100}.
    std::vector<double> grid_bps{30.0, 40.0, 50.0, 60.0, 80.0, 100.0};

    /// Prior alpha (successes) for each grid level.
    /// Initialised to 1.0 (uniform Beta(1,1) prior) if left empty.
    std::vector<double> prior_alpha;

    /// Prior beta (failures) for each grid level.
    /// Initialised to 1.0 if left empty.
    std::vector<double> prior_beta;

    /// Discount factor (gamma) for discounted Thompson Sampling.
    /// Each update decays the existing posterior by this factor before
    /// adding the new observation, allowing the posterior to forget stale
    /// evidence from prior regimes.
    /// Valid range: (0.0, 1.0].  1.0 = no discounting (standard TS).
    /// Recommended: [0.95, 0.99].  Default: 0.97.
    /// Reference: Besbes, Gur & Zeevi (2014), "Stochastic Multi-Armed-
    /// Bandit Problem with Non-Stationary Rewards".
    /// ISO/IEC 27001:2022: configurable parameter, no secrets embedded.
    double thompson_discount_gamma{0.97};

    /// Enable / disable Thompson Sampling.  When disabled,
    /// compute_spread() uses the deterministic model exclusively.
    bool enabled{false};
};

// ---------------------------------------------------------------------------
// SpreadConfig -- tuning knobs for the four-component model and its dynamic
//                 adjustments.  Defaults reflect CHIA conditions as of
//                 March 2026 (Section 6 of the strategy document).
// ---------------------------------------------------------------------------
struct SpreadConfig {
    // --- Adverse selection ---
    /// Risk-aversion coefficient (gamma).  Shared with Avellaneda-Stoikov.
    double gamma{0.01};

    /// Default PIN (Probability of Informed Trading) when the Bayesian
    /// estimator has not yet converged.  Typical range 0.10-0.25.
    double default_pin{0.15};

    /// Default expected time-to-fill in seconds.  Calibrated from dexie
    /// historical fill data (strategy doc: ~2 hours at current volume).
    double default_expected_fill_seconds{7200.0};

    // --- Inventory ---
    /// Time horizon (tau) in seconds for inventory cost calculation.
    /// Represents the trader's planning horizon.
    double tau_seconds{3600.0};

    // --- Cost ---
    /// Blockchain settlement fee in XCH.  Typically 0.0001 XCH.
    double blockchain_fee_xch{0.0001};

    /// Default trade size in XCH for cost-per-trade calculation.
    double default_trade_size_xch{10.0};

    // --- Competition ---
    /// Minimum profitable spread floor in bps.  Strategy doc: 35-60 bps.
    double s_floor_bps{40.0};

    /// Minimum improvement over best competing spread (epsilon), in bps.
    double epsilon_bps{2.0};

    // --- Dynamic adjustment multipliers ---
    double high_vol_multiplier{1.80};     // Widen 80% under high volatility.
    double low_vol_multiplier{0.70};      // Tighten 30% under low volatility.
    double weekend_multiplier{1.175};     // Widen 17.5% on weekends (midpoint of 15-20%).
    double overlap_hours_multiplier{0.90};// Tighten 10% during US+EU overlap.
    double inventory_skew_threshold{0.60};// Fraction of q_max beyond which
                                          //   asymmetric widening kicks in.
    double inventory_overweight_multiplier{1.30}; // Applied to overweight side.

    // --- Thompson Sampling ---
    ThompsonSamplerConfig thompson;
};

// ---------------------------------------------------------------------------
// ThompsonSampler -- maintains Beta posteriors over a discrete spread grid,
//                    samples from them to select an exploration spread, and
//                    updates posteriors on fill outcomes.
//
// Usage:
//   1. Call sample() to draw a spread level from the posteriors.
//   2. Quote at or near that level.
//   3. On fill, call record_outcome(level_index, profitable).
//
// Thread safety: NOT thread-safe.  The caller (SpreadOptimizer) serialises
// access via its own compute_spread / record_fill API.
// ---------------------------------------------------------------------------
class ThompsonSampler {
public:
    /// Construct from configuration.  Validates and normalises priors.
    explicit ThompsonSampler(const ThompsonSamplerConfig& cfg);

    /// Sample from the Beta posteriors and return the index of the spread
    /// level with the highest sampled probability of profitability.
    /// Returns the grid index (into grid_bps).
    std::size_t sample();

    /// Return the spread value (bps) corresponding to a grid index.
    double spread_at(std::size_t index) const;

    /// Record the outcome of a fill at a given grid level.
    /// @param index   Grid level that was active when the fill occurred.
    /// @param profit  True if the fill was profitable (spread captured);
    ///                false if adverse selection was detected.
    void record_outcome(std::size_t index, bool profit);

    /// Return the posterior mean for a given grid level:
    ///   alpha / (alpha + beta).
    double posterior_mean(std::size_t index) const;

    /// Number of levels in the grid.
    std::size_t grid_size() const noexcept;

    /// Read-only access to the current grid (bps values).
    const std::vector<double>& grid() const noexcept;

    /// Read-only access to the current alpha (success) counts.
    const std::vector<double>& alphas() const noexcept;

    /// Read-only access to the current beta (failure) counts.
    const std::vector<double>& betas() const noexcept;

    /// [T8-06] Partially reset posteriors toward the prior on regime change.
    /// Multiplies all alpha/beta values by decay_factor and enforces the
    /// floor of 1.0 to keep the Beta distributions well-formed.
    /// @param decay_factor  In (0, 1]; 0.5 halves accumulated evidence.
    void partial_reset(double decay_factor);

private:
    std::vector<double> grid_bps_;   // Discrete spread levels.
    std::vector<double> alpha_;      // Beta posterior alpha per level.
    std::vector<double> beta_;       // Beta posterior beta per level.
    std::mt19937        rng_;        // Mersenne Twister PRNG.

    /// Geometric discount factor applied to alpha/beta on every update.
    /// Enables non-stationary adaptation per Besbes et al. (2014).
    /// Invariant: discount_gamma_ in (0.0, 1.0].
    /// ISO/IEC 27001:2022: no secret material; audit-safe numeric param.
    double discount_gamma_{0.97};
};

// ---------------------------------------------------------------------------
// [T5-CR15] SpreadVolatilityTracker -- rolling volatility-of-spread.
//
// Tracks the standard deviation of computed spreads over a rolling window.
// When the spread-of-spread is high, liquidity conditions are unstable
// and tier sizing should be more conservative.
//
// Usage:
//   SpreadVolatilityTracker tracker(50);  // 50-block window
//   tracker.update(total_spread_bps);
//   double sigma_spread = tracker.spread_volatility();  // std dev in bps
//   double multiplier   = tracker.safety_multiplier();  // >= 1.0
// ---------------------------------------------------------------------------
class SpreadVolatilityTracker {
public:
    /// @param window_size  Rolling window length in observations.
    ///                     Default 50 blocks (~43 min).
    explicit SpreadVolatilityTracker(std::size_t window_size = 50);

    /// Record a new spread observation.
    void update(double total_spread_bps);

    /// Rolling standard deviation of spreads (bps).
    /// Returns 0.0 if fewer than 2 observations.
    double spread_volatility() const noexcept;

    /// Rolling mean spread (bps).
    double spread_mean() const noexcept;

    /// Coefficient of variation = sigma / mean.
    /// Returns 0.0 if mean is near zero.
    double coefficient_of_variation() const noexcept;

    /// Safety multiplier for tier sizing: 1 + clamp(CV - 0.10, 0, 0.50).
    /// When CV is low (stable spreads), returns 1.0.
    /// When CV is high (unstable), returns up to 1.50 (50% wider sizing margin).
    double safety_multiplier() const noexcept;

    /// Number of observations in the window.
    std::size_t count() const noexcept;

private:
    std::size_t window_size_;
    std::deque<double> observations_;

    // Running statistics for O(1) update.
    double sum_{0.0};
    double sum_sq_{0.0};
};

// ---------------------------------------------------------------------------
// SpreadOptimizer -- the primary class that computes the four-component
//                    spread and applies dynamic adjustments.
//
// Typical usage (per block heartbeat):
//
//     SpreadOptimizer optimizer(config);
//     auto result = optimizer.compute_spread(
//         mid_price_xch, sigma_daily, inventory_q, q_max,
//         pin, venue_fee, best_competing_bps,
//         hour_utc, day_of_week);
//
// The optimizer is stateful only when Thompson Sampling is enabled;
// otherwise every call to compute_spread() is a pure function of its inputs.
// ---------------------------------------------------------------------------
class SpreadOptimizer {
public:
    /// Construct with the given spread configuration.
    explicit SpreadOptimizer(const SpreadConfig& cfg);

    // -----------------------------------------------------------------------
    // Primary interface
    // -----------------------------------------------------------------------

    /// Compute the optimal spread given current market conditions.
    ///
    /// @param mid_price_xch     Current mid-price in XCH (e.g. 2.75).
    /// @param sigma_daily       Annualised or daily volatility as a decimal
    ///                          fraction (e.g. 0.05 for 5% daily vol).
    ///                          Interpretation: daily standard deviation of
    ///                          returns.
    /// @param inventory_q       Current inventory in base-asset units.
    ///                          Positive = long, negative = short.
    /// @param q_max             Maximum tolerated absolute inventory.
    /// @param pin               Probability of Informed Trading [0,1].
    ///                          Pass <= 0 to use the configured default.
    /// @param venue             DEX venue for fee lookup.
    /// @param best_competing_bps Best observed competing spread (bps).
    ///                          Pass <= 0 if no competition data available.
    /// @param hour_utc          Current hour of day in UTC [0, 23].
    /// @param day_of_week       ISO day-of-week: 1=Monday .. 7=Sunday.
    ///
    /// @return Fully decomposed SpreadResult.
    SpreadResult compute_spread(
        double mid_price_xch,
        double sigma_daily,
        double inventory_q,
        double q_max,
        double pin,
        Venue  venue,
        double best_competing_bps,
        int    hour_utc,
        int    day_of_week) const;

    // -----------------------------------------------------------------------
    // Thompson Sampling interaction (only meaningful when enabled)
    // -----------------------------------------------------------------------

    /// Record a fill outcome for the Thompson Sampling module.
    /// No-op if Thompson Sampling is disabled.
    /// @param profitable  True if the fill captured positive spread PnL.
    void record_fill(bool profitable);

    /// Sample a spread level from the Thompson posteriors.
    /// Returns std::nullopt if Thompson Sampling is disabled.
    std::optional<double> thompson_sample();

    /// Read-only access to the Thompson Sampler (nullopt if disabled).
    const ThompsonSampler* sampler() const noexcept;

    /// [T8-06] Partially reset Thompson posteriors on regime transition.
    /// No-op if Thompson Sampling is disabled.
    /// @param decay_factor  In (0, 1]; forwarded to ThompsonSampler::partial_reset.
    void reset_thompson_posteriors(double decay_factor);

    // -----------------------------------------------------------------------
    // Component accessors (for diagnostics / unit testing)
    // -----------------------------------------------------------------------

    /// Adverse selection: gamma * sigma * sqrt(E[T_fill]) * PIN, in bps.
    static double calc_adverse_selection_bps(
        double gamma,
        double sigma_daily,
        double expected_fill_seconds,
        double pin);

    /// Inventory risk: gamma * sigma^2 * tau * |q| / q_max, in bps.
    static double calc_inventory_bps(
        double gamma,
        double sigma_daily,
        double tau_seconds,
        double inventory_q,
        double q_max);

    /// Transaction cost: (blockchain_fee + venue_fee) / trade_size, in bps.
    static double calc_cost_bps(
        double blockchain_fee_xch,
        double venue_fee_fraction,
        double trade_size_xch);

    /// Competition: undercut target = max(s_floor, best_competing - epsilon).
    ///
    /// Returns a target spread cap (not an additive component).  When
    /// competition data is present, this value caps the model-derived spread
    /// to undercut the best competitor by epsilon while respecting the
    /// minimum profitable floor.  Returns 0 when no competition data is
    /// available, signalling "no cap".
    ///
    /// T3-33: Corrected from (best_competing + epsilon) which widened vs.
    /// competitor, to (best_competing - epsilon) which undercuts/tightens.
    static double calc_competition_bps(
        double s_floor_bps,
        double best_competing_bps,
        double epsilon_bps);

    /// Regime multiplier: product of volatility, weekend, and overlap factors.
    static double calc_regime_multiplier(
        VolatilityRegime regime,
        int hour_utc,
        int day_of_week,
        double high_vol_mult,
        double low_vol_mult,
        double weekend_mult,
        double overlap_mult);

    // -----------------------------------------------------------------------
    // Configuration access
    // -----------------------------------------------------------------------

    /// Return a const reference to the active configuration.
    const SpreadConfig& config() const noexcept;

    /// [T5-CR15] Read-only access to the spread volatility tracker.
    const SpreadVolatilityTracker& spread_vol_tracker() const noexcept {
        return spread_vol_tracker_;
    }

private:
    SpreadConfig                        cfg_;
    mutable std::optional<ThompsonSampler> sampler_;

    /// Index of the most recently sampled Thompson grid level.
    /// Used to attribute fill outcomes to the correct grid bucket.
    mutable std::optional<std::size_t>  last_thompson_index_;

    /// [T5-CR15] Rolling volatility-of-spread tracker (50-block window).
    mutable SpreadVolatilityTracker spread_vol_tracker_{50};
};

}  // namespace xop

#endif  // XOP_STRATEGY_SPREAD_HPP
