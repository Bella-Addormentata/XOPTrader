// avellaneda.cpp -- Avellaneda-Stoikov optimal market-making implementation
//                   adapted for the CHIA blockchain.
//
// See avellaneda.hpp for the full mathematical derivation and references.
//
// ISO/IEC 27001:2022 -- no secrets handled.
// ISO/IEC 5055       -- no unchecked arithmetic; all edge cases guarded.
// ISO/IEC 25000      -- comprehensive inline documentation.

#include <xop/strategy/avellaneda.hpp>

#include <algorithm>
#include <cmath>
#include <shared_mutex>
#include <stdexcept>

namespace xop {

// ===========================================================================
// Construction
// ===========================================================================

AvellanedaStoikov::AvellanedaStoikov(const AvellanedaConfig& cfg)
    : cfg_(cfg)
    , min_margin_bps_(cfg.min_margin_bps)
{
    // Validate critical parameters.  Config values may come from user files
    // or command-line flags, so throw on invalid input rather than assert
    // (which is stripped in Release builds).
    // ISO/IEC 5055: fail-fast on invalid configuration.
    if (!(cfg_.gamma > 0.0)) {
        throw std::invalid_argument("AvellanedaConfig: gamma must be strictly positive");
    }
    if (!(cfg_.kappa > 0.0)) {
        throw std::invalid_argument("AvellanedaConfig: kappa must be strictly positive");
    }
    if (!(cfg_.A > 0.0)) {
        throw std::invalid_argument("AvellanedaConfig: fill intensity A must be strictly positive");
    }
    if (!(cfg_.q_max > 0.0)) {
        throw std::invalid_argument("AvellanedaConfig: q_max must be strictly positive");
    }
    if (!(cfg_.horizon_blocks > 0)) {
        throw std::invalid_argument("AvellanedaConfig: horizon_blocks must be at least 1");
    }
    if (!(cfg_.block_time_seconds > 0.0)) {
        throw std::invalid_argument("AvellanedaConfig: block_time_seconds must be positive");
    }

    // Initialize regime to sane defaults so the first compute_quotes() call
    // before any update_price() produces reasonable spreads (not zero).
    // ISO/IEC 5055: defensive initialization of derived state.
    regime_ = RegimeInfo{MarketRegime::Random, 1.0, 1.0, 1.0};

    // T3-01: create the internal canonical RegimeDetector with config
    // derived from the strategy's VR thresholds and regime multipliers.
    RegimeDetectorConfig rd_cfg;
    rd_cfg.min_window_size      = cfg_.regime_window_blocks / 2;
    rd_cfg.max_window_size      = cfg_.regime_window_blocks;
    rd_cfg.vr_lower_threshold   = cfg_.vr_mean_revert_threshold;
    rd_cfg.vr_upper_threshold   = cfg_.vr_momentum_threshold;
    rd_cfg.mr_multipliers       = {cfg_.regime_mr_spread_mult, 1.0,
                                   cfg_.regime_mr_skew_mult, 1.0};
    rd_cfg.momentum_multipliers = {cfg_.regime_mo_spread_mult, 1.0,
                                   cfg_.regime_mo_skew_mult, 1.0};
    internal_detector_ = std::make_unique<RegimeDetector>(rd_cfg);
}

// ===========================================================================
// Core interface -- compute_quotes
// ===========================================================================

QuoteResult AvellanedaStoikov::compute_quotes(double mid,
                                              double sigma,
                                              double q,
                                              BlockHeight block_height)
{
    // MEDIUM-1: Exclusive lock -- compute_quotes reads cost_basis_,
    // min_margin_bps_, regime_, and cfg_ (mutable strategy state).
    // ISO/IEC 27001:2022: protect shared mutable state.
    std::unique_lock lock(mtx_);

    // -------------------------------------------------------------------
    // Step 1: Compute remaining time in the rolling horizon (seconds).
    //
    //   tau = (N - n) * block_time
    //
    // where N = horizon_blocks and n = block_height mod N.
    // When n = 0 (start of window), tau = N * block_time (maximum).
    // When n = N-1 (end of window), tau = 1 * block_time (minimum).
    // The modular rollover prevents tau from ever reaching zero, which
    // would collapse the spread and cause division issues.
    // -------------------------------------------------------------------
    const double tau = compute_tau(block_height);

    // -------------------------------------------------------------------
    // Step 2: Compute the reservation price.
    //
    //   r = S - q * gamma * sigma^2 * tau
    //
    // Intuition:
    //   - When q > 0 (long inventory), r < S.  The reservation price is
    //     BELOW mid because we value selling more than buying.
    //   - When q < 0 (short inventory), r > S.  The reservation price is
    //     ABOVE mid because we value buying more than selling.
    //   - gamma controls how aggressively inventory tilts the price.
    //   - sigma^2 * tau is the expected variance over the remaining horizon;
    //     higher risk => stronger desire to flatten inventory.
    // -------------------------------------------------------------------
    const double r_raw = reservation_price(mid, sigma, q, tau);

    // -------------------------------------------------------------------
    // Step 3: Apply regime-dependent skew multiplier to the inventory
    //         component of the reservation price.
    //
    // In A-S, inventory adjustment lives entirely in the reservation
    // price shift: (r - S) = -q * gamma * sigma^2 * tau.  The regime
    // skew multiplier scales this shift:
    //
    //   r_adjusted = S + skew_mult * (r_raw - S)
    //              = S - skew_mult * q * gamma * sigma^2 * tau
    //
    // Mean-reverting (0.5x): halve the inventory shift -- mean-reversion
    //   will naturally correct inventory, so less aggressive rebalancing.
    // Momentum (2.0x): double the inventory shift -- shed inventory fast
    //   when prices are trending to avoid large directional exposure.
    // Random (1.0x): no change.
    // -------------------------------------------------------------------
    const double r = mid + regime_.skew_mult * (r_raw - mid);

    // -------------------------------------------------------------------
    // Step 4: Compute the optimal symmetric half-spread.
    //
    //   delta = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
    //
    // The first term is the "pure market-making" spread component derived
    // from the Poisson fill-intensity model:
    //   lambda(delta) = A * exp(-kappa * delta)
    //
    // It represents the optimal balance between earning a wider spread per
    // trade and receiving fewer fills.  It depends only on the ratio
    // kappa/gamma and is independent of volatility or time.
    //
    // The second term compensates for the inventory risk (variance) that
    // accumulates over the remaining horizon.  It grows with sigma^2 and
    // tau, widening the spread when conditions are more uncertain.
    // -------------------------------------------------------------------
    double delta = optimal_half_spread(sigma, tau);

    // -------------------------------------------------------------------
    // Step 5: Apply regime-dependent spread multiplier.
    //
    // The variance-ratio test classifies the current micro-structure:
    //   - Mean-reverting (VR < 0.85): prices tend to bounce back, so we
    //     can safely tighten spreads and reduce inventory shedding.
    //   - Momentum (VR > 1.15): prices trend, so we widen spreads to
    //     avoid adverse selection and shed inventory aggressively.
    //   - Random (else): no adjustment.
    // -------------------------------------------------------------------
    delta *= regime_.spread_mult;

    // -------------------------------------------------------------------
    // Step 6: Compute raw bid and ask.
    //
    //   bid = r - delta
    //   ask = r + delta
    //
    // Because r is already shifted by inventory (and scaled by the regime
    // skew multiplier), the final quotes are asymmetric when q != 0.
    // For example, with q > 0:
    //   r < S  =>  ask = r + delta  (closer to mid, easier to sell)
    //              bid = r - delta  (farther from mid, harder to buy)
    // -------------------------------------------------------------------
    double bid = r - delta;
    double ask = r + delta;

    // -------------------------------------------------------------------
    // Step 7: Apply the never-sell-at-loss constraint (optional).
    //
    //   ask = max(ask, cost_basis * (1 + min_margin_bps / 10000))
    //
    // This is a hard floor that prevents posting an ask below our
    // weighted-average acquisition cost plus a minimum profit margin.
    // When enabled, underwater inventory is held rather than liquidated.
    //
    // This transforms the unconstrained optimisation into a constrained
    // one; the bid side is unaffected (we always want to buy cheap).
    // -------------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint && cost_basis_ > 0.0) {
        const double min_ask = cost_basis_ * (1.0 + min_margin_bps_ / 10000.0);
        ask = std::max(ask, min_ask);
    }

    // -------------------------------------------------------------------
    // Step 8: Floor prices at zero (defensive -- should never trigger
    //         with sane parameters).
    // -------------------------------------------------------------------
    bid = std::max(bid, 0.0);
    ask = std::max(ask, bid + 1e-12);  // ask must always exceed bid

    // -------------------------------------------------------------------
    // Step 9: Compute sizes.
    //
    // Size scaling is proportional to the fraction of q_max not yet
    // consumed.  When inventory is near q_max, the overweight side
    // shrinks toward zero while the underweight side stays at full size.
    //
    //   bid_size = q_max * max(0, 1 - q / q_max)
    //   ask_size = q_max * max(0, 1 + q / q_max)
    //
    // Intuition: when q = +q_max, bid_size = 0 (stop buying) and
    //            ask_size = 2 * q_max (maximise selling).
    // -------------------------------------------------------------------
    const double q_ratio = q / cfg_.q_max;
    const double bid_size = cfg_.q_max * std::max(0.0, 1.0 - q_ratio);
    const double ask_size = cfg_.q_max * std::max(0.0, 1.0 + q_ratio);

    // -------------------------------------------------------------------
    // Step 10: Compute spread in basis points for reporting.
    //
    //   spread_bps = 10000 * (ask - bid) / mid
    // -------------------------------------------------------------------
    const double spread_bps = (mid > 0.0)
        ? 10000.0 * (ask - bid) / mid
        : 0.0;

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

// ===========================================================================
// Market data feed
// ===========================================================================

void AvellanedaStoikov::update_price(double mid, BlockHeight block_height)
{
    // MEDIUM-1: Exclusive lock -- update_price mutates price_buffer_,
    // last_mid_, and regime_ via active_detector().update() and
    // update_regime().  ISO/IEC 27001:2022: protect shared mutable state.
    std::unique_lock lock(mtx_);

    // Append to the rolling buffer (retained for backward compatibility
    // and potential diagnostic use).
    price_buffer_.push_back(PriceObs{block_height, mid});

    // Trim to the regime detection window size.
    while (price_buffer_.size() > cfg_.regime_window_blocks) {
        price_buffer_.pop_front();
    }

    // T3-01: feed the single-block log return to the active RegimeDetector.
    // The detector maintains its own rolling window and computes VR(q_short)
    // and VR(q_long) internally with proper Z-statistic significance testing
    // and hysteresis.  Guard against non-positive prices to prevent log(0)
    // or log(negative).
    // ISO/IEC 5055: guard against domain error in std::log.
    if (last_mid_ > 0.0 && mid > 0.0) {
        active_detector().update(std::log(mid / last_mid_));
    }
    last_mid_ = mid;

    // Recompute regime classification from the detector's updated state.
    update_regime();
}

// ===========================================================================
// Accessors
// ===========================================================================

RegimeInfo AvellanedaStoikov::current_regime() const
{
    // MEDIUM-1: Shared lock -- read-only access to regime_.
    std::shared_lock lock(mtx_);
    return regime_;
}

// [MEDIUM-2] Return by value so the caller does not hold a reference through
// an expiring shared_lock (ISO/IEC 5055 -- CWE-362).
std::string AvellanedaStoikov::name() const
{
    // MEDIUM-1: Shared lock -- read-only access to name_.
    std::shared_lock lock(mtx_);
    return name_;
}

void AvellanedaStoikov::set_cost_basis(double cost_basis,
                                       double min_margin_bps)
{
    // MEDIUM-1: Exclusive lock -- set_cost_basis mutates cost_basis_
    // and min_margin_bps_.  ISO/IEC 27001:2022: protect shared mutable state.
    std::unique_lock lock(mtx_);
    cost_basis_     = cost_basis;
    min_margin_bps_ = min_margin_bps;
}

// ===========================================================================
// A-S specific computations
// ===========================================================================

double AvellanedaStoikov::reservation_price(double mid, double sigma,
                                            double q, double tau) const
{
    // MEDIUM-1: No lock needed -- reads only cfg_ which is immutable after
    // construction.  Called internally from compute_quotes() which already
    // holds the exclusive lock; acquiring here would deadlock.
    // ISO/IEC 27001:2022: cfg_ is const-after-init, safe without lock.

    // r = S - q * gamma * sigma^2 * tau
    //
    // The inventory penalty q * gamma * sigma^2 * tau represents the
    // expected cost of carrying inventory q over the remaining horizon
    // tau at volatility sigma, scaled by risk aversion gamma.
    return mid - q * cfg_.gamma * sigma * sigma * tau;
}

double AvellanedaStoikov::optimal_half_spread(double sigma, double tau) const
{
    // MEDIUM-1: No lock needed -- reads only cfg_ which is immutable after
    // construction.  Called internally from compute_quotes() which already
    // holds the exclusive lock; acquiring here would deadlock.

    // delta = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
    //
    // Term 1: (1/kappa) * ln(1 + kappa/gamma)
    //   This is the optimal spread from the Poisson fill-intensity model
    //   alone, ignoring inventory risk.  It balances fill probability
    //   (higher delta => fewer fills) against spread revenue per fill.
    //   Derived by maximising E[spread * fill_probability].
    //
    // Term 2: 0.5 * gamma * sigma^2 * tau
    //   This compensates for the inventory risk: holding any position for
    //   tau more seconds at volatility sigma carries variance
    //   sigma^2 * tau, and the market maker demands gamma/2 times that
    //   variance as additional compensation.
    const double term1 = (1.0 / cfg_.kappa) * std::log(1.0 + cfg_.kappa / cfg_.gamma);
    const double term2 = 0.5 * cfg_.gamma * sigma * sigma * tau;
    return term1 + term2;
}

double AvellanedaStoikov::compute_tau(BlockHeight block_height) const
{
    // MEDIUM-1: No lock needed -- reads only cfg_ which is immutable after
    // construction.  Called internally from compute_quotes() which already
    // holds the exclusive lock; acquiring here would deadlock.

    // tau = (N - n) * block_time
    //
    // where n = block_height mod N (position within the rolling window).
    //
    // This gives a sawtooth pattern: tau starts at N*block_time, linearly
    // decreases to 1*block_time, then resets.  The minimum is 1 block
    // (never zero), preventing degenerate spread collapse.
    //
    // COUNTER-RESEARCH NOTE (CR-3, Cartea, Jaimungal & Penalva 2015 §10.3):
    //   The sawtooth tau creates a deterministic, exploitable cycle in 24/7
    //   markets.  Sophisticated adversaries aware of the modular periodicity
    //   can time orders to exploit the post-reset complacency window when
    //   inventory shedding is weakest.  The GLFT infinite-horizon formulation
    //   avoids this.  Alternative: replace sawtooth with exponential decay
    //   per Stoikov (2018) "The micro-price".
    //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, §2.2.
    const uint32_t n = block_height % cfg_.horizon_blocks;
    const uint32_t remaining = cfg_.horizon_blocks - n;
    return static_cast<double>(remaining) * cfg_.block_time_seconds;
}

/* static */
double AvellanedaStoikov::per_block_volatility(double sigma_annual,
                                               double block_time_seconds)
{
    // Volatility scales with the square root of time:
    //   sigma_block = sigma_annual * sqrt(block_time / seconds_per_year)
    //
    // For CHIA with block_time = 52 s:
    //   sigma_block = sigma_annual * sqrt(52 / 31,536,000)
    //               = sigma_annual * sqrt(1.6487e-6)
    //               = sigma_annual * 0.001284
    //
    // Example: 50% annual vol => 0.50 * 0.001284 = 0.000642 per block.
    return sigma_annual * std::sqrt(block_time_seconds / kSecondsPerYear);
}

double AvellanedaStoikov::fill_intensity(double delta) const
{
    // MEDIUM-1: No lock needed -- reads only cfg_ which is immutable after
    // construction.  Safe for concurrent callers.

    // lambda(delta) = A * exp(-kappa * delta)
    //
    // Models the arrival rate of counter-party fills as a decreasing
    // exponential in the distance of our quote from the mid-price.
    //
    //   delta = 0     => lambda = A          (maximum fill rate)
    //   delta = 1/kappa => lambda = A / e    (~37% of maximum)
    //   delta -> inf  => lambda -> 0         (no fills)
    //
    // kappa is calibrated from historical dexie offer-take data:
    //   Fit an exponential to (spread, fills-per-block) observations.
    return cfg_.A * std::exp(-cfg_.kappa * delta);
}

// ===========================================================================
// Regime detection (T3-01: delegated to shared canonical RegimeDetector)
// ===========================================================================

void AvellanedaStoikov::update_regime()
{
    // T3-01: delegate regime classification to the active RegimeDetector
    // (shared if injected via set_regime_detector(), else the internal
    // detector created in the constructor).  The canonical detector provides
    // dual-horizon VR, Z-statistic significance testing (Lo-MacKinlay 1988),
    // hysteresis to prevent regime whipsawing, and optional HMM.
    regime_ = to_regime_info(active_detector());
}

}  // namespace xop
