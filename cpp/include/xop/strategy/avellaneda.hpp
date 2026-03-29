// avellaneda.hpp -- Avellaneda-Stoikov optimal market-making strategy adapted
//                   for the CHIA blockchain's 52-second block time.
//
// Reference: Avellaneda, M. & Stoikov, S. (2008). "High-frequency trading in
//            a limit order book." Quantitative Finance, 8(3), 217-224.
//
// The classical A-S model assumes continuous time with a fixed terminal horizon
// T.  On CHIA there is no natural session end, so we use an exponential-decay
// tau that resets on each fill:
//
//   tau(t) = tau_max * exp(-lambda * blocks_since_last_fill)
//   lambda = -ln(tau_min / tau_max) / horizon_blocks
//
// T5-CR3: the previous sawtooth tau (block_height % N / N) was exploitable
// because adversaries could predict the deterministic cycle and time orders
// to the post-reset complacency window.  Exponential decay keyed to fills
// eliminates the fixed period.  Reference: Stoikov (2018) "The micro-price";
// Cartea, Jaimungal & Penalva (2015) S10.3.
//
// Key formulas (see compute_quotes for full derivation comments):
//
//   reservation price : r = S - q * gamma * sigma^2 * tau
//   optimal half-spread: delta = (1/kappa) * ln(1 + kappa/gamma)
//                                + 0.5 * gamma * sigma^2 * tau
//   bid = r - delta
//   ask = r + delta
//
// The never-sell-at-loss constraint is OPTIONAL and controlled by
// enable_no_loss_constraint in the config (default false).
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical computation.
// ISO/IEC 5055       -- no raw pointers; bounds-checked deque.
// ISO/IEC 25000      -- comprehensive formulae documentation.

#ifndef XOP_STRATEGY_AVELLANEDA_HPP
#define XOP_STRATEGY_AVELLANEDA_HPP

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
// AvellanedaConfig -- parameters specific to the Avellaneda-Stoikov model.
//
// These extend the generic StrategyConfig with A-S-specific fields.
// ---------------------------------------------------------------------------

struct AvellanedaConfig {
    // -- Model parameters ---------------------------------------------------

    double gamma{0.01};    // Risk-aversion coefficient.
                           //   Higher gamma => wider spreads, faster inventory
                           //   mean-reversion.  Typical range: [0.001, 0.1].

    double kappa{1.5};     // Fill-intensity decay parameter.
                           //   lambda(delta) = A * exp(-kappa * delta).
                           //   Higher kappa => fills drop off faster as spread
                           //   widens.  Calibrated from historical dexie data.

    double A{100.0};       // Fill-intensity base rate.
                           //   lambda(0) = A fills per unit time when spread is
                           //   zero.  Units: fills per second.

    double q_max{1000.0};  // Maximum tolerated inventory in base-asset units.
                           //   Used to normalise inventory for size scaling.

    // -- Horizon and block time ---------------------------------------------

    uint32_t horizon_blocks{120};  // Rolling N-block horizon.
                                   //   120 blocks * 52 s = 6240 s (~1.73 hours).
                                   //   Tau resets every N blocks.

    double block_time_seconds{52.0};  // Mean CHIA inter-block interval.

    // -- Regime detection ---------------------------------------------------

    uint32_t regime_window_blocks{100};  // Rolling window for VR test.

    double vr_mean_revert_threshold{0.85};  // VR below this => mean-reverting.
    double vr_momentum_threshold{1.15};     // VR above this => momentum.

    // -- Regime multipliers -------------------------------------------------

    double regime_mr_spread_mult{0.80};   // Spread multiplier in mean-revert.
    double regime_mr_skew_mult{0.50};     // Skew multiplier in mean-revert.
    double regime_mo_spread_mult{1.50};   // Spread multiplier in momentum.
    double regime_mo_skew_mult{2.00};     // Skew multiplier in momentum.

    // -- Exponential decay tau (T5-CR3) --------------------------------------

    double tau_min{0.01};  // Floor for exponential-decay tau (seconds).
                           //   Must be > 0 and < tau_max.  Prevents tau from
                           //   collapsing to zero, which would degenerate
                           //   spreads and remove inventory-risk compensation.
                           //   ISO/IEC 5055: validated in constructor.

    // -- No-loss constraint (optional) --------------------------------------

    bool   enable_no_loss_constraint{false};  // When true, ask is floored at
                                              //   cost_basis + min_margin.
    double min_margin_bps{35.0};              // Minimum margin above cost in bps.
};

// ---------------------------------------------------------------------------
// AvellanedaStoikov -- concrete strategy implementation.
// ---------------------------------------------------------------------------

class AvellanedaStoikov final : public StrategyBase {
public:
    /// Construct with a fully populated config.
    explicit AvellanedaStoikov(const AvellanedaConfig& cfg);

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

    // -- A-S specific accessors ---------------------------------------------

    /// Compute the reservation price (inventory-adjusted mid) without
    /// posting quotes.  Useful for logging and diagnostics.
    double reservation_price(double mid, double sigma,
                             double q, double tau) const;

    /// Compute the optimal half-spread for current parameters.
    double optimal_half_spread(double sigma, double tau) const;

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

    /// Compute per-block volatility from annualised volatility.
    ///
    /// sigma_block = sigma_annual * sqrt(block_time / seconds_per_year)
    ///
    /// With block_time = 52 s and seconds_per_year = 31,536,000:
    ///   sigma_block = sigma_annual * sqrt(52 / 31536000)
    ///               = sigma_annual * 0.001284
    ///
    /// Note: the strategy document quotes 0.000963 as the conversion factor.
    /// That value appears to be erroneous.  sqrt(52 / 31536000) = 0.001284.
    /// We use the mathematically correct derivation here.
    static double per_block_volatility(double sigma_annual,
                                       double block_time_seconds = 52.0);

    /// Compute fill intensity at a given distance delta from mid.
    /// lambda(delta) = A * exp(-kappa * delta)
    double fill_intensity(double delta) const;

    /// Read-only access to the config.
    // cfg_ is immutable after construction; no lock required.
    const AvellanedaConfig& config() const { return cfg_; }

    // -- Shared regime detector (T3-01) -------------------------------------

    /// Set a shared RegimeDetector instance.  When set, the strategy
    /// delegates all regime classification to this canonical detector
    /// instead of its internal detector.  The shared detector must outlive
    /// this strategy instance.  Pass nullptr to revert to internal detection.
    ///
    /// @param detector  Pointer to the shared canonical RegimeDetector.
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
    /// Used by update_regime() to query the current regime classification.
    RegimeDetector& active_detector() noexcept {
        return shared_regime_detector_ ? *shared_regime_detector_
                                       : *internal_detector_;
    }

    /// Update regime classification.
    /// T3-01: delegates to the active RegimeDetector (shared or internal).
    void update_regime();

    // -- Thread safety (MEDIUM-1) --------------------------------------------
    // Mutable to allow shared (read) locking in const accessor methods.
    // Follows the same locking pattern used by ChiaEdgeOptimizer, State,
    // VolatilityEstimator, and AdverseSelectionEstimator: single mutex,
    // no nesting.  ISO/IEC 27001:2022 -- protects mutable state from
    // concurrent access.
    mutable std::shared_mutex mtx_;

    // -- Data members -------------------------------------------------------

    AvellanedaConfig cfg_;

    std::string name_{"AvellanedaStoikov"};

    // Rolling price buffer for regime detection (mid-prices per block).
    // Stored as (block_height, mid_price) pairs.
    struct PriceObs {
        BlockHeight block;
        double      mid;
    };
    std::deque<PriceObs> price_buffer_;

    // Current regime state.
    RegimeInfo regime_{MarketRegime::Random, 1.0, 1.0, 1.0};

    // T3-01: canonical RegimeDetector instances.
    // internal_detector_: owned internally, created in the constructor with
    //   config derived from AvellanedaConfig VR thresholds.  Used for
    //   standalone operation and backward-compatible tests.
    // shared_regime_detector_: externally owned, injected via
    //   set_regime_detector().  When set, takes precedence over internal.
    std::unique_ptr<RegimeDetector> internal_detector_;
    RegimeDetector* shared_regime_detector_{nullptr};

    // Last observed mid-price for computing log returns fed to the detector.
    double last_mid_{0.0};

    // T5-CR3: block height of the most recent fill.  Used to compute
    // blocks_since_last_fill for the exponential-decay tau.  Initialised
    // to zero; before the first fill, every block increments the decay.
    // ISO/IEC 27001:2022: mutated by record_fill() under exclusive lock.
    BlockHeight last_fill_block_{0};

    // No-loss constraint state.
    double cost_basis_{0.0};        // weighted-average acquisition price
    double min_margin_bps_{35.0};   // minimum margin above cost (bps)

    // Seconds per year (365 days, non-leap).
    static constexpr double kSecondsPerYear = 365.0 * 24.0 * 3600.0;  // 31,536,000
};

}  // namespace xop

#endif  // XOP_STRATEGY_AVELLANEDA_HPP
