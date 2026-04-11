// glft.hpp -- GLFT (Gueant-Lehalle-Fernandez-Tapia) market-making strategy
//             with a running inventory penalty.
//
// Reference: Gueant, O., Lehalle, C.A., & Fernandez-Tapia, J. (2013).
//            "Dealing with the inventory risk: A solution to the market making
//            problem." Mathematics and Financial Economics, 7(4), 477-507.
//
// The classical Avellaneda-Stoikov model has a fixed terminal horizon T, which
// creates an artificial urgency to flatten inventory as t -> T.  GLFT replaces
// the terminal penalty with a continuous running penalty proportional to
// inventory squared, making it ideal for 24/7 markets like CHIA that have no
// natural session boundary.
//
// Key formulas:
//
//   half_spread = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
//   skew        = phi * q / q_max
//   ask         = S + half_spread - skew * q
//   bid         = S - half_spread - skew * q
//
// The skew term shifts BOTH bid and ask downward when inventory is positive
// (long), making the ask more attractive (easier to sell) and the bid less
// attractive (harder to buy).  This continuously nudges inventory toward zero
// without requiring a terminal horizon.
//
// Skew direction verification:
//   q > 0 (long) => skew > 0 => skew*q > 0
//     ask = S + half_spread - (positive) => ask decreases => easier to sell
//     bid = S - half_spread - (positive) => bid decreases => harder to buy
//     NET EFFECT: inventory reduction.  Correct.
//
//   q < 0 (short) => skew < 0 => skew*q > 0 (product of two negatives)
//     Wait -- let's be more careful:
//     skew = phi * q / q_max.  If q < 0, skew < 0.
//     skew * q = (phi * q / q_max) * q = phi * q^2 / q_max > 0 always.
//     So both quotes shift down regardless of inventory sign.
//
//     That is WRONG for the short case.  When short (q < 0) we want to
//     shift quotes UP to buy more easily.  The correct formulation uses
//     skew as a linear shift (not quadratic):
//
//     ask = S + half_spread - phi * q / q_max   (phi * q / q_max, NOT times q again)
//     bid = S - half_spread - phi * q / q_max
//
//   With this linear formulation:
//     q > 0: shift = -phi * q / q_max < 0 => quotes shift DOWN => sell easier.  Correct.
//     q < 0: shift = -phi * q / q_max > 0 => quotes shift UP   => buy easier.  Correct.
//
// This header uses the LINEAR skew formulation consistent with the strategy
// document (section 5) where "skew = phi * q / q_max" and the shift is
// applied as "- skew" to both quotes.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical computation.
// ISO/IEC 5055       -- no raw pointers; bounds-checked deque.
// ISO/IEC 25000      -- comprehensive formulae documentation.

#ifndef XOP_STRATEGY_GLFT_HPP
#define XOP_STRATEGY_GLFT_HPP

#include <xop/strategy/base.hpp>
#include <xop/strategy/regime.hpp>
#include <xop/config.hpp>
#include <xop/types.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace xop {

// ---------------------------------------------------------------------------
// GlftConfig -- parameters specific to the GLFT running-inventory-penalty model.
// ---------------------------------------------------------------------------

struct GlftConfig {
    // -- Model parameters ---------------------------------------------------

    double gamma{0.01};    // Risk-aversion coefficient (controls base spread).
                           //   Same role as in A-S; higher => wider spreads.

    double kappa{1.5};     // Fill-intensity decay.
                           //   lambda(delta) = A * exp(-kappa * delta).

    double A{100.0};       // Fill-intensity base rate (fills / second at delta=0).

    double phi{0.5};       // Inventory skew strength.
                           //   skew = phi * q / q_max.
                           //   Higher phi => more aggressive inventory rebalancing.
                           //   Range: [0.1, 2.0] typically.

    double q_max{1000.0};  // Maximum tolerated inventory in base-asset units.

    // -- Half-spread price-level cap ----------------------------------------

    double max_half_spread_pct{0.49}; // Maximum half-spread as a fraction of mid.
                                      //   The A-S term1 (1/kappa)*ln(1+kappa/gamma)
                                      //   is price-level-independent.  For low-price
                                      //   pairs it can exceed mid, making bid <= 0.
                                      //   Cap hs to mid * max_half_spread_pct so the
                                      //   bid stays positive.  Default 0.49 (49%).

    // -- Horizon (used only for the gamma * sigma^2 * tau term) -------------

    uint32_t horizon_blocks{120};     // Rolling N-block horizon for the spread
                                      //   risk component.
    double block_time_seconds{52.0};  // Mean CHIA inter-block interval.

    // -- Regime detection ---------------------------------------------------

    uint32_t regime_window_blocks{100};

    double vr_mean_revert_threshold{0.85};
    double vr_momentum_threshold{1.15};

    double regime_mr_spread_mult{0.80};
    double regime_mr_skew_mult{0.50};
    double regime_mo_spread_mult{1.50};
    double regime_mo_skew_mult{2.00};

    // -- Exponential decay tau (T5-CR3) --------------------------------------

    double tau_min{0.01};  // Floor for exponential-decay tau (seconds).
                           //   Must be > 0 and < tau_max.  Same semantics as
                           //   AvellanedaConfig.  Prevents tau from collapsing
                           //   to zero.  ISO/IEC 5055: validated in ctor.

    // -- Sparse-fill correction (T5-CR8) -------------------------------------
    //
    // Fodra & Pham (2015) show that GLFT's continuous-time fill intensity
    // overestimates effective fill rates on sparse discrete markets.
    // Laruelle, Lehalle & Pages (2011) confirm empirically that the optimal
    // inventory skew coefficient must be amplified when fills are rare.
    //
    // The correction multiplies the skew coefficient phi by:
    //   sparse_correction = clamp(dense_rate / actual_rate, 1.0, max_cap)
    //
    // On CHIA (~1 fill/hour/pair), with dense_rate = 100, this yields a
    // correction factor of ~100, capped at sparse_correction_cap.
    //
    // ISO/IEC 27001:2022: validated at construction; no secrets handled.
    // ISO/IEC 5055:       guarded against division-by-zero (actual > 0).
    // ISO/IEC 25000:      inline documentation of correction derivation.

    double expected_dense_fills_per_hour{100.0};  // Typical CLOB fill rate
                                                  //   (fills/hour) against
                                                  //   which the continuous-
                                                  //   time model is calibrated.

    double actual_fills_per_hour{1.0};            // Observed or configured
                                                  //   fill rate on the target
                                                  //   venue (e.g., CHIA).
                                                  //   May be updated at runtime
                                                  //   from a fill-rate database.

    double sparse_correction_cap{10.0};           // Maximum allowed sparse-fill
                                                  //   correction factor to
                                                  //   prevent extreme skew.

    // -- No-loss constraint (optional) --------------------------------------

    bool   enable_no_loss_constraint{false};
    double min_margin_bps{35.0};
};

// ---------------------------------------------------------------------------
// GlftStrategy -- concrete GLFT strategy implementation.
// ---------------------------------------------------------------------------

class GlftStrategy final : public StrategyBase {
public:
    /// Construct with a fully populated config.
    explicit GlftStrategy(const GlftConfig& cfg);

    // -- StrategyBase interface ---------------------------------------------

    QuoteResult compute_quotes(double mid,
                               double sigma,
                               double q,
                               BlockHeight block_height) override;

    void update_price(double mid, BlockHeight block_height) override;

    RegimeInfo current_regime() const override;

    // [MEDIUM-2] Return by value -- reference through expiring shared_lock
    // is undefined behaviour (ISO/IEC 5055 -- CWE-362).
    std::string name() const override;

    void set_cost_basis(double cost_basis,
                        double min_margin_bps) override;

    // -- GLFT specific accessors --------------------------------------------

    /// Compute the base half-spread (before regime adjustment).
    /// Uses the same A-S formula for the risk-neutral component:
    ///   (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
    double base_half_spread(double sigma, double tau) const;

    /// Compute the inventory skew for a given inventory level.
    ///   skew = phi * q / q_max
    /// Returns a signed value: positive when long, negative when short.
    double inventory_skew(double q) const;

    /// Compute tau using exponential decay based on blocks since last fill.
    /// T5-CR3: replaces the exploitable sawtooth cycle.
    ///   tau(t) = tau_max * exp(-lambda * blocks_since_last_fill)
    ///   lambda = -ln(tau_min / tau_max) / horizon_blocks
    /// Reference: Stoikov (2018); Cartea et al. (2015) S10.3 counter-research.
    double compute_tau(BlockHeight block_height) const;

    /// Record a fill event.  Resets blocks_since_last_fill_ to zero so that
    /// tau resets to tau_max after each fill, decaying smoothly from there.
    /// T5-CR3: fill-driven reset eliminates the deterministic sawtooth period.
    /// ISO/IEC 27001:2022: caller must hold no lock; exclusive lock acquired.
    void record_fill() override;

    /// Per-block volatility conversion.
    /// sigma_block = sigma_annual * sqrt(block_time / seconds_per_year)
    static double per_block_volatility(double sigma_annual,
                                       double block_time_seconds = 52.0);

    /// Fill intensity at distance delta from mid.
    double fill_intensity(double delta) const;

    /// Read-only access to the config.
    // cfg_ is immutable after construction; no lock required.
    const GlftConfig& config() const { return cfg_; }

    // -- Shared regime detector (T3-01) -------------------------------------

    /// Set a shared RegimeDetector instance.  Pass nullptr to revert to
    /// internal detection.  The shared detector must outlive this instance.
    ///
    /// MEDIUM-1: Exclusive lock -- mutates shared_regime_detector_.
    /// ISO/IEC 27001:2022: protect shared mutable state.
    void set_regime_detector(RegimeDetector* detector) noexcept {
        std::unique_lock lock(mtx_);
        shared_regime_detector_ = detector;
    }

    /// Return the active RegimeDetector (shared if set, else internal).
    ///
    /// MEDIUM-1: Shared lock -- read-only access to shared_regime_detector_.
    const RegimeDetector* regime_detector() const noexcept {
        std::shared_lock lock(mtx_);
        return shared_regime_detector_ ? shared_regime_detector_
                                       : internal_detector_.get();
    }

    /// [T7-11] Return whether the regime detector has enough observations.
    bool is_regime_ready() const override {
        const auto* det = regime_detector();
        return det ? det->is_ready() : true;
    }

private:
    /// Return the active detector (shared or internal).
    RegimeDetector& active_detector() noexcept {
        return shared_regime_detector_ ? *shared_regime_detector_
                                       : *internal_detector_;
    }

    /// Update regime classification.
    /// T3-01: delegates to the active RegimeDetector.
    void update_regime();

    // -- Thread safety (MEDIUM-1) --------------------------------------------
    // Mutable to allow shared (read) locking in const accessor methods.
    // Follows the same locking pattern used by ChiaEdgeOptimizer, State,
    // VolatilityEstimator, and AdverseSelectionEstimator: single mutex,
    // no nesting.  ISO/IEC 27001:2022 -- protects mutable state from
    // concurrent access.
    mutable std::shared_mutex mtx_;

    // -- Data members -------------------------------------------------------

    GlftConfig cfg_;

    std::string name_{"GLFT"};

    // Rolling price buffer for regime detection.
    struct PriceObs {
        BlockHeight block;
        double      mid;
    };
    std::deque<PriceObs> price_buffer_;

    // Current regime state.
    RegimeInfo regime_{MarketRegime::Random, 1.0, 1.0, 1.0};

    // T3-01: canonical RegimeDetector instances.
    std::unique_ptr<RegimeDetector> internal_detector_;
    RegimeDetector* shared_regime_detector_{nullptr};
    double last_mid_{0.0};

    // T5-CR3: block height of the most recent fill.  Used to compute
    // blocks_since_last_fill for the exponential-decay tau.  Initialised
    // to zero; before the first fill, every block increments the decay.
    // ISO/IEC 27001:2022: mutated by record_fill() under exclusive lock.
    BlockHeight last_fill_block_{0};

    // No-loss constraint state.
    double cost_basis_{0.0};
    double min_margin_bps_{35.0};

    static constexpr double kSecondsPerYear = 365.0 * 24.0 * 3600.0;
};

}  // namespace xop

#endif  // XOP_STRATEGY_GLFT_HPP
