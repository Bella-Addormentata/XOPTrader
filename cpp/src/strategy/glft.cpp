// glft.cpp -- GLFT running-inventory-penalty market-making implementation.
//
// See glft.hpp for the full mathematical derivation, skew direction proof,
// and references (Gueant, Lehalle, Fernandez-Tapia 2013).
//
// ISO/IEC 27001:2022 -- no secrets handled.
// ISO/IEC 5055       -- no unchecked arithmetic; all edge cases guarded.
// ISO/IEC 25000      -- comprehensive inline documentation.

#include <xop/strategy/glft.hpp>

#include <algorithm>
#include <cmath>
#include <shared_mutex>
#include <stdexcept>

namespace xop {

// ===========================================================================
// Construction
// ===========================================================================

GlftStrategy::GlftStrategy(const GlftConfig& cfg)
    : cfg_(cfg)
    , min_margin_bps_(cfg.min_margin_bps)
{
    // Validate critical parameters.  Config values may come from user files
    // or command-line flags, so throw on invalid input rather than assert
    // (which is stripped in Release builds).
    // ISO/IEC 5055: fail-fast on invalid configuration.
    if (!(cfg_.gamma > 0.0)) {
        throw std::invalid_argument("GlftConfig: gamma must be strictly positive");
    }
    if (!(cfg_.kappa > 0.0)) {
        throw std::invalid_argument("GlftConfig: kappa must be strictly positive");
    }
    if (!(cfg_.A > 0.0)) {
        throw std::invalid_argument("GlftConfig: fill intensity A must be strictly positive");
    }
    if (!(cfg_.phi >= 0.0)) {
        throw std::invalid_argument("GlftConfig: phi must be non-negative");
    }
    if (!(cfg_.q_max > 0.0)) {
        throw std::invalid_argument("GlftConfig: q_max must be strictly positive");
    }
    if (!(cfg_.horizon_blocks > 0)) {
        throw std::invalid_argument("GlftConfig: horizon_blocks must be at least 1");
    }
    if (!(cfg_.block_time_seconds > 0.0)) {
        throw std::invalid_argument("GlftConfig: block_time_seconds must be positive");
    }

    // Initialize regime to sane defaults so the first compute_quotes() call
    // before any update_price() produces reasonable spreads (not zero).
    // ISO/IEC 5055: defensive initialization of derived state.
    regime_ = RegimeInfo{MarketRegime::Random, 1.0, 1.0, 1.0};

    // T3-01: create the internal canonical RegimeDetector with config
    // derived from the strategy's VR thresholds and regime multipliers.
    // This detector is used when no shared detector has been injected
    // via set_regime_detector(), preserving backward compatibility for
    // standalone use and unit tests.
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

QuoteResult GlftStrategy::compute_quotes(double mid,
                                         double sigma,
                                         double q,
                                         BlockHeight block_height)
{
    // MEDIUM-1: Exclusive lock -- compute_quotes reads cost_basis_,
    // min_margin_bps_, regime_, and cfg_ (mutable strategy state).
    // ISO/IEC 27001:2022: protect shared mutable state.
    std::unique_lock lock(mtx_);

    // -------------------------------------------------------------------
    // Step 1: Compute tau for the risk term.
    //
    // Even though GLFT removes the terminal penalty, the base half-spread
    // formula still uses tau to quantify the per-block inventory risk:
    //   half_spread = (1/kappa)*ln(1 + kappa/gamma) + 0.5*gamma*sigma^2*tau
    //
    // We use the same rolling-horizon tau as A-S so the risk premium term
    // adapts to how far we are into the current evaluation window.
    // -------------------------------------------------------------------
    const double tau = compute_tau(block_height);

    // -------------------------------------------------------------------
    // Step 2: Compute the base (symmetric) half-spread.
    //
    //   half_spread = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
    //
    // This is identical to the A-S formula.  The first term captures the
    // optimal pure-spread from fill intensity; the second compensates for
    // inventory variance risk.
    // -------------------------------------------------------------------
    double hs = base_half_spread(sigma, tau);

    // -------------------------------------------------------------------
    // Step 3: Apply regime-dependent spread multiplier.
    //
    // Mean-reverting: 0.8x (safe to tighten -- prices bounce back).
    // Momentum:       1.5x (widen to avoid adverse selection from trends).
    // -------------------------------------------------------------------
    hs *= regime_.spread_mult;

    // -------------------------------------------------------------------
    // Step 4: Compute the inventory skew.
    //
    //   skew = phi * q / q_max
    //
    // This is the KEY difference from A-S.  Instead of a terminal penalty
    // that creates urgency as t -> T, GLFT applies a continuous linear
    // shift to both quotes proportional to current inventory.
    //
    // The shift is SUBTRACTED from both bid and ask:
    //   ask = S + half_spread - skew
    //   bid = S - half_spread - skew
    //
    // Direction verification:
    //
    //   q > 0 (long):
    //     skew = phi * q / q_max > 0
    //     ask = S + hs - skew  =>  ask moves DOWN  => easier to sell.
    //     bid = S - hs - skew  =>  bid moves DOWN  => harder to buy.
    //     Net: reduces long inventory.  CORRECT.
    //
    //   q < 0 (short):
    //     skew = phi * q / q_max < 0
    //     ask = S + hs - skew  =>  ask moves UP    => harder to sell.
    //     bid = S - hs - skew  =>  bid moves UP    => easier to buy.
    //     Net: reduces short inventory.  CORRECT.
    //
    //   q = 0 (flat):
    //     skew = 0.  Quotes are symmetric around mid.  CORRECT.
    // -------------------------------------------------------------------
    double skew = inventory_skew(q);

    // Apply regime-dependent skew multiplier.
    // Mean-reverting: 0.5x (less shedding -- mean-reversion helps naturally).
    // Momentum:       2.0x (aggressive shedding -- trending markets punish
    //                       inventory that is on the wrong side).
    skew *= regime_.skew_mult;

    // -------------------------------------------------------------------
    // Step 5: Compute raw bid and ask.
    //
    //   ask = mid + half_spread - skew
    //   bid = mid - half_spread - skew
    //
    // where skew = phi * q / q_max (already incorporates inventory q).
    //
    // IMPORTANT NOTATIONAL CLARIFICATION:
    // The strategy document writes "- skew * q_t" with skew defined as
    // "phi * q / q_max".  Substituting literally gives -phi*q^2/q_max
    // which is ALWAYS negative (shifts both quotes down regardless of
    // inventory sign).  That is wrong for short inventory (q < 0).
    //
    // The correct GLFT formulation from the literature (Gueant et al. 2013)
    // applies a LINEAR shift "- phi * q / q_max" to both quotes.  We
    // interpret the spec's "skew * q_t" as a notational shorthand where
    // "skew" means the coefficient phi/q_max and the full shift is
    // (phi / q_max) * q = phi * q / q_max, i.e. what we call `skew`.
    //
    // Unlike A-S where the reservation price r already encodes inventory,
    // here the mid is the raw market mid and inventory adjustment is
    // entirely through the skew term.
    // -------------------------------------------------------------------
    double ask = mid + hs - skew;
    double bid = mid - hs - skew;

    // -------------------------------------------------------------------
    // Step 6: Apply the never-sell-at-loss constraint (optional).
    //
    //   ask = max(ask, cost_basis * (1 + min_margin_bps / 10000))
    //
    // Controlled by enable_no_loss_constraint (default false).  When
    // enabled, underwater inventory is held rather than sold at a loss.
    // -------------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint && cost_basis_ > 0.0) {
        const double min_ask = cost_basis_ * (1.0 + min_margin_bps_ / 10000.0);
        ask = std::max(ask, min_ask);
    }

    // -------------------------------------------------------------------
    // Step 7: Safety floors.
    // -------------------------------------------------------------------
    bid = std::max(bid, 0.0);
    ask = std::max(ask, bid + 1e-12);

    // -------------------------------------------------------------------
    // Step 8: Compute position sizes.
    //
    // Same inventory-aware sizing as A-S: reduce the overweight side,
    // increase the underweight side, linearly in q / q_max.
    //
    //   bid_size = q_max * max(0, 1 - q / q_max)
    //   ask_size = q_max * max(0, 1 + q / q_max)
    //
    // When long (q > 0):  bid shrinks, ask grows.
    // When short (q < 0): bid grows, ask shrinks.
    // When flat (q = 0):  both sides equal q_max.
    // -------------------------------------------------------------------
    const double q_ratio = q / cfg_.q_max;
    const double bid_size = cfg_.q_max * std::max(0.0, 1.0 - q_ratio);
    const double ask_size = cfg_.q_max * std::max(0.0, 1.0 + q_ratio);

    // -------------------------------------------------------------------
    // Step 9: Spread in basis points.
    // -------------------------------------------------------------------
    const double spread_bps = (mid > 0.0)
        ? 10000.0 * (ask - bid) / mid
        : 0.0;

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

// ===========================================================================
// Market data feed
// ===========================================================================

void GlftStrategy::update_price(double mid, BlockHeight block_height)
{
    // MEDIUM-1: Exclusive lock -- update_price mutates price_buffer_,
    // last_mid_, and regime_ via active_detector().update() and
    // update_regime().  ISO/IEC 27001:2022: protect shared mutable state.
    std::unique_lock lock(mtx_);

    // Append to the rolling buffer (retained for backward compatibility
    // and potential diagnostic use).
    price_buffer_.push_back(PriceObs{block_height, mid});

    // Trim to the regime detection window.
    while (price_buffer_.size() > cfg_.regime_window_blocks) {
        price_buffer_.pop_front();
    }

    // T3-01: feed the single-block log return to the active RegimeDetector.
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

RegimeInfo GlftStrategy::current_regime() const
{
    // MEDIUM-1: Shared lock -- read-only access to regime_.
    std::shared_lock lock(mtx_);
    return regime_;
}

// [MEDIUM-2] Return by value so the caller does not hold a reference through
// an expiring shared_lock (ISO/IEC 5055 -- CWE-362).
std::string GlftStrategy::name() const
{
    // MEDIUM-1: Shared lock -- read-only access to name_.
    std::shared_lock lock(mtx_);
    return name_;
}

void GlftStrategy::set_cost_basis(double cost_basis,
                                  double min_margin_bps)
{
    // MEDIUM-1: Exclusive lock -- set_cost_basis mutates cost_basis_
    // and min_margin_bps_.  ISO/IEC 27001:2022: protect shared mutable state.
    std::unique_lock lock(mtx_);
    cost_basis_     = cost_basis;
    min_margin_bps_ = min_margin_bps;
}

// ===========================================================================
// GLFT specific computations
// ===========================================================================

double GlftStrategy::base_half_spread(double sigma, double tau) const
{
    // MEDIUM-1: No lock needed -- reads only cfg_ which is immutable after
    // construction.  Called internally from compute_quotes() which already
    // holds the exclusive lock; acquiring here would deadlock.

    // half_spread = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
    //
    // Term 1: pure market-making component from the fill-intensity model.
    //         Independent of volatility and time.  Captures the fundamental
    //         trade-off between wider spread (more revenue per fill) and
    //         fewer fills (exponential decay in fill rate).
    //
    // Term 2: inventory-risk compensation.  The market maker demands
    //         gamma/2 * sigma^2 * tau additional spread to cover the
    //         expected variance of holding a position for tau seconds.
    const double term1 = (1.0 / cfg_.kappa) * std::log(1.0 + cfg_.kappa / cfg_.gamma);
    const double term2 = 0.5 * cfg_.gamma * sigma * sigma * tau;
    return term1 + term2;
}

double GlftStrategy::inventory_skew(double q) const
{
    // MEDIUM-1: No lock needed -- reads only cfg_ which is immutable after
    // construction.  Called internally from compute_quotes() which already
    // holds the exclusive lock; acquiring here would deadlock.

    // skew = phi * q / q_max
    //
    // This is a linear function of inventory:
    //   - Centered at zero when flat.
    //   - Magnitude grows proportionally to |q|.
    //   - Sign matches the sign of q.
    //   - At q = q_max, skew = phi (maximum skew).
    //
    // phi controls the aggressiveness of inventory rebalancing:
    //   phi = 0   => no skew, pure symmetric quotes (ignores inventory).
    //   phi = 0.5 => moderate skew (default).
    //   phi = 2.0 => very aggressive skew (fast inventory turnover).
    //
    // The units of skew are the same as the price units (quote asset per
    // base asset) because it enters the quote formulas as a price offset.
    return cfg_.phi * q / cfg_.q_max;
}

double GlftStrategy::compute_tau(BlockHeight block_height) const
{
    // MEDIUM-1: No lock needed -- reads only cfg_ which is immutable after
    // construction.  Called internally from compute_quotes() which already
    // holds the exclusive lock; acquiring here would deadlock.

    // Same rolling-horizon tau as A-S.
    //   tau = (N - n) * block_time
    //   n = block_height mod N
    //
    // Even though GLFT does not have a terminal penalty, tau is still
    // needed in the half-spread formula to quantify the per-block risk.
    // The rolling horizon prevents tau from collapsing to zero.
    const uint32_t n = block_height % cfg_.horizon_blocks;
    const uint32_t remaining = cfg_.horizon_blocks - n;
    return static_cast<double>(remaining) * cfg_.block_time_seconds;
}

/* static */
double GlftStrategy::per_block_volatility(double sigma_annual,
                                          double block_time_seconds)
{
    // sigma_block = sigma_annual * sqrt(block_time / seconds_per_year)
    //
    // Derivation: under a geometric Brownian motion,
    //   dS/S = mu*dt + sigma*dW
    // the standard deviation of returns over interval dt is sigma*sqrt(dt).
    //
    // For one CHIA block (dt = 52 / 31,536,000 years):
    //   sigma_block = sigma_annual * sqrt(52 / 31,536,000)
    //               = sigma_annual * sqrt(1.6487e-6)
    //               = sigma_annual * 0.001284
    return sigma_annual * std::sqrt(block_time_seconds / kSecondsPerYear);
}

double GlftStrategy::fill_intensity(double delta) const
{
    // MEDIUM-1: No lock needed -- reads only cfg_ which is immutable after
    // construction.  Safe for concurrent callers.

    // lambda(delta) = A * exp(-kappa * delta)
    //
    // Poisson arrival rate of fills at distance delta from mid.
    //
    // COUNTER-RESEARCH NOTE (CR-8, Fodra & Pham 2015):
    //   This continuous-time exponential intensity was calibrated for
    //   dense electronic markets.  On CHIA (~1 fill/hour/pair), the
    //   discrete sparse-block structure means the optimal inventory
    //   skew coefficient should be larger than what the GLFT formula
    //   produces — the trader should shed inventory more aggressively
    //   per fill because opportunities are rarer.
    //   Also: Laruelle, Lehalle & Pagès (2011) show empirically that
    //   fill intensity has a two-regime structure (plateau near mid,
    //   sharp fall-off outside spread) rather than pure exponential.
    //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, §3.1.
    return cfg_.A * std::exp(-cfg_.kappa * delta);
}

// ===========================================================================
// Regime detection (T3-01: delegated to shared canonical RegimeDetector)
// ===========================================================================

void GlftStrategy::update_regime()
{
    // T3-01: delegate regime classification to the active RegimeDetector
    // (shared if injected via set_regime_detector(), else the internal
    // detector created in the constructor).  The canonical detector provides
    // dual-horizon VR, Z-statistic significance testing (Lo-MacKinlay 1988),
    // hysteresis to prevent regime whipsawing, and optional HMM.
    regime_ = to_regime_info(active_detector());
}

}  // namespace xop
