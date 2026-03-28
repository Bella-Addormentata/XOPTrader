// avellaneda.cpp -- Avellaneda-Stoikov optimal market-making implementation
//                   adapted for the CHIA blockchain.
//
// See avellaneda.hpp for the full mathematical derivation and references.
//
// ISO/IEC 27001:2022 -- no secrets handled.
// ISO/IEC 5055       -- no unchecked arithmetic; all edge cases guarded.
// ISO/IEC 25000      -- comprehensive inline documentation.

#include <xop/strategy/avellaneda.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <mutex>
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
    // T5-CR3: tau_min must be strictly positive to prevent log(0) in lambda
    // computation and to guarantee tau never collapses to zero.
    // ISO/IEC 5055: fail-fast on invalid configuration.
    if (!(cfg_.tau_min > 0.0)) {
        throw std::invalid_argument("AvellanedaConfig: tau_min must be strictly positive");
    }
    // T5-CR3: tau_min must be less than tau_max (= horizon_blocks * block_time)
    // to ensure lambda > 0 (tau decays rather than grows after each fill).
    // If tau_min >= tau_max, log(tau_min/tau_max) >= 0, lambda <= 0.
    // ISO/IEC 5055: fail-fast on misconfiguration.
    {
        const double tau_max = static_cast<double>(cfg_.horizon_blocks)
                             * cfg_.block_time_seconds;
        if (!(cfg_.tau_min < tau_max)) {
            throw std::invalid_argument(
                "AvellanedaConfig: tau_min must be < horizon_blocks * block_time_seconds");
        }
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
    // NaN/Inf guard at the public API boundary.  If any input is non-finite,
    // return a zero-spread quote that the engine will skip (spread_bps = 0).
    // This prevents NaN from propagating through all downstream arithmetic.
    // ISO/IEC 5055: CWE-754 -- check for exceptional conditions.
    if (!std::isfinite(mid) || !std::isfinite(sigma) || !std::isfinite(q)) {
        spdlog::warn("[AvellanedaStoikov] compute_quotes: non-finite input "
                     "(mid={}, sigma={}, q={}) -- returning zero quote",
                     mid, sigma, q);
        return QuoteResult{0.0, 0.0, 0.0, 0.0, 0.0};
    }

    // MEDIUM-1: Exclusive lock -- compute_quotes reads cost_basis_,
    // min_margin_bps_, regime_, and cfg_ (mutable strategy state).
    // ISO/IEC 27001:2022: protect shared mutable state.
    std::unique_lock lock(mtx_);

    // -------------------------------------------------------------------
    // Step 1: Compute remaining time via exponential decay (seconds).
    //
    // T5-CR3: tau decays exponentially from tau_max after each fill:
    //   tau = tau_max * exp(-lambda * blocks_since_last_fill)
    //
    // This replaces the deterministic sawtooth that adversaries could
    // exploit.  See compute_tau() for the full derivation.
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
// Fill tracking (T5-CR3)
// ===========================================================================

void AvellanedaStoikov::record_fill()
{
    // T5-CR3: record a fill event by snapshotting the latest observed block
    // height.  compute_tau() uses (block_height - last_fill_block_) to
    // compute the exponential-decay tau, so resetting last_fill_block_
    // effectively resets tau to tau_max.
    //
    // MEDIUM-1: Exclusive lock -- mutates last_fill_block_.
    // ISO/IEC 27001:2022: protect shared mutable state.
    std::unique_lock lock(mtx_);

    // Use the block height of the most recent price observation as the
    // fill block.  If no prices have been observed yet, leave at zero.
    if (!price_buffer_.empty()) {
        last_fill_block_ = price_buffer_.back().block;
    }
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
    // This is the GLFT (Guéant-Lehalle-Fernandez-Tapia) corrected form of the
    // Avellaneda-Stoikov optimal half-spread.  The original A-S (2008) result
    // used an approximation delta ≈ (1/gamma) * ln(1 + gamma/kappa); the GLFT
    // correction (Guéant, Lehalle & Fernandez-Tapia, "Dealing with the
    // Inventory Risk", Math. Finance, 2013; extended in Guéant, "Optimal
    // Market Making", Applied Mathematical Finance, 2016) derives the exact
    // closed-form solution for the fill-intensity model.
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
    // MEDIUM-1: No lock needed -- reads only cfg_ (immutable after
    // construction) and last_fill_block_ (read under the exclusive lock
    // already held by the caller, compute_quotes()).
    // ISO/IEC 27001:2022: no additional lock required; caller holds mtx_.

    // -----------------------------------------------------------------------
    // T5-CR3: Exponential-decay tau (replaces exploitable sawtooth).
    //
    //   tau(t) = tau_max * exp(-lambda * blocks_since_last_fill)
    //   lambda = -ln(tau_min / tau_max) / horizon_blocks
    //
    // where tau_max = horizon_blocks * block_time_seconds (the maximum tau
    // at the instant of a fill), and tau_min is a configurable floor
    // (default 0.01) that prevents tau from reaching zero.
    //
    // After each fill, tau resets to tau_max and decays smoothly toward
    // tau_min over the next horizon_blocks blocks.  This eliminates the
    // deterministic sawtooth cycle that adversaries could predict and
    // exploit in 24/7 markets (CHIA ~52s blocks, no session boundaries).
    //
    // COUNTER-RESEARCH NOTE (CR-3, Cartea, Jaimungal & Penalva 2015 S10.3):
    //   FIXED.  The previous sawtooth tau created a deterministic, exploitable
    //   cycle.  Adversaries aware of the modular periodicity could time orders
    //   to the post-reset complacency window when inventory shedding was
    //   weakest.  Exponential decay keyed to fills removes the fixed period.
    //   Reference: Stoikov (2018) "The micro-price".
    //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, S2.2.
    // -----------------------------------------------------------------------

    // Maximum tau: full horizon in seconds.
    const double tau_max = static_cast<double>(cfg_.horizon_blocks)
                         * cfg_.block_time_seconds;

    // Decay rate: lambda = -ln(tau_min / tau_max) / horizon_blocks.
    // At blocks_since_last_fill == horizon_blocks, tau decays to tau_min.
    // ISO/IEC 5055: tau_min > 0 validated in constructor; log is safe.
    const double lambda = -std::log(cfg_.tau_min / tau_max)
                        / static_cast<double>(cfg_.horizon_blocks);

    // Blocks elapsed since the most recent fill.
    // If block_height < last_fill_block_ (should not happen under normal
    // operation), clamp to zero defensively.
    // ISO/IEC 5055: underflow guard on unsigned subtraction.
    const uint32_t blocks_since_fill =
        (block_height >= last_fill_block_)
            ? (block_height - last_fill_block_)
            : 0u;

    // Exponential decay: tau = tau_max * exp(-lambda * blocks_since_fill).
    const double tau = tau_max * std::exp(-lambda
                     * static_cast<double>(blocks_since_fill));

    // Floor at tau_min to prevent degenerate zero-spread conditions.
    // ISO/IEC 5055: defensive clamp; mathematically tau >= tau_min already
    // holds for blocks_since_fill <= horizon_blocks, but floating-point
    // drift could violate this for very large elapsed counts.
    return std::max(tau, cfg_.tau_min);
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
