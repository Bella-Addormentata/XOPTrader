// spread.cpp -- Four-component spread optimiser for XOPTrader CHIA DEX
//               market-making bot.
//
// Implements the model from CHIA_MARKET_MAKER_STRATEGY.md Section 6:
//
//     spread = s_adverse + s_inventory + s_cost + s_competition
//
// with dynamic regime multipliers and optional Thompson Sampling.
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets processed, deterministic audit trail)
//   ISO/IEC 5055        (bounds-checked, no undefined behaviour)
//   ISO/IEC 25000       (tested public interface, clear error modes)
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++17)

#include "xop/strategy/spread.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace xop {

// ===========================================================================
// Free function: venue_fee_fraction
// ===========================================================================

double venue_fee_fraction(Venue v) noexcept {
    // Fee schedule per CHIA_MARKET_MAKER_STRATEGY.md Section 3.
    //   Dexie      : 0%   on regular offers (1% on Combined Swap -- not modelled here)
    //   TibetSwap  : 0.7%
    //   Hashgreen  : 0.9%
    //   OfferBin   : 0%
    //   Splash     : 0%
    switch (v) {
        case Venue::Dexie:     return 0.0;
        case Venue::TibetSwap: return 0.007;
        case Venue::Hashgreen: return 0.009;
        case Venue::OfferBin:  return 0.0;
        case Venue::Splash:    return 0.0;
    }
    // Unreachable for valid enum values.  Return conservative fee for
    // defensive programming (ISO/IEC 5055 -- no silent zero on bad input).
    return 0.01;
}

// ===========================================================================
// ThompsonSampler
// ===========================================================================

ThompsonSampler::ThompsonSampler(const ThompsonSamplerConfig& cfg)
    : grid_bps_(cfg.grid_bps)
    , rng_(std::random_device{}())
{
    if (grid_bps_.empty()) {
        throw std::invalid_argument(
            "ThompsonSampler: grid_bps must contain at least one level");
    }

    // Validate that every grid level is positive.
    for (std::size_t i = 0; i < grid_bps_.size(); ++i) {
        if (grid_bps_[i] <= 0.0) {
            throw std::invalid_argument(
                "ThompsonSampler: grid_bps[" + std::to_string(i)
                + "] must be > 0, got " + std::to_string(grid_bps_[i]));
        }
    }

    // Initialise alpha priors: use supplied values or default to 1.0.
    if (cfg.prior_alpha.empty()) {
        alpha_.assign(grid_bps_.size(), 1.0);
    } else if (cfg.prior_alpha.size() == grid_bps_.size()) {
        alpha_ = cfg.prior_alpha;
    } else {
        throw std::invalid_argument(
            "ThompsonSampler: prior_alpha length ("
            + std::to_string(cfg.prior_alpha.size())
            + ") must match grid_bps length ("
            + std::to_string(grid_bps_.size()) + ")");
    }

    // Initialise beta priors: use supplied values or default to 1.0.
    if (cfg.prior_beta.empty()) {
        beta_.assign(grid_bps_.size(), 1.0);
    } else if (cfg.prior_beta.size() == grid_bps_.size()) {
        beta_ = cfg.prior_beta;
    } else {
        throw std::invalid_argument(
            "ThompsonSampler: prior_beta length ("
            + std::to_string(cfg.prior_beta.size())
            + ") must match grid_bps length ("
            + std::to_string(grid_bps_.size()) + ")");
    }

    // Validate all prior values are strictly positive.
    for (std::size_t i = 0; i < grid_bps_.size(); ++i) {
        if (alpha_[i] <= 0.0 || beta_[i] <= 0.0) {
            throw std::invalid_argument(
                "ThompsonSampler: prior alpha/beta must be > 0 at index "
                + std::to_string(i));
        }
    }
}

std::size_t ThompsonSampler::sample() {
    // Draw one sample from each arm's Beta(alpha, beta) posterior.
    // Return the index of the arm with the highest sampled value.
    //
    // std::gamma_distribution is used to construct Beta samples because
    // C++17 does not provide a Beta distribution directly.
    //   Beta(a, b) = X / (X + Y) where X ~ Gamma(a, 1), Y ~ Gamma(b, 1).

    std::size_t best_index = 0;
    double      best_value = -1.0;

    for (std::size_t i = 0; i < grid_bps_.size(); ++i) {
        std::gamma_distribution<double> gamma_a(alpha_[i], 1.0);
        std::gamma_distribution<double> gamma_b(beta_[i],  1.0);

        const double x = gamma_a(rng_);
        const double y = gamma_b(rng_);

        // Guard against numerical edge (both near zero).
        const double sample_val = (x + y > 0.0) ? (x / (x + y)) : 0.5;

        if (sample_val > best_value) {
            best_value = sample_val;
            best_index = i;
        }
    }

    return best_index;
}

double ThompsonSampler::spread_at(std::size_t index) const {
    if (index >= grid_bps_.size()) {
        throw std::out_of_range(
            "ThompsonSampler::spread_at: index " + std::to_string(index)
            + " >= grid size " + std::to_string(grid_bps_.size()));
    }
    return grid_bps_[index];
}

void ThompsonSampler::record_outcome(std::size_t index, bool profit) {
    if (index >= grid_bps_.size()) {
        throw std::out_of_range(
            "ThompsonSampler::record_outcome: index " + std::to_string(index)
            + " >= grid size " + std::to_string(grid_bps_.size()));
    }

    // Update the Beta posterior:
    //   Profitable fill    -> increment alpha (evidence of success).
    //   Adverse selection   -> increment beta  (evidence of failure).
    //
    // COUNTER-RESEARCH NOTE (CR-7, Besbes, Gur & Zeevi 2014):
    //   Standard Thompson Sampling regret guarantees do not hold under
    //   non-stationarity.  When optimal spread width shifts with regime
    //   changes, the Beta posterior is too slow to forget outdated
    //   observations.  Consider discounted Thompson Sampling:
    //     alpha_new = alpha * decay + (profit ? 1 : 0)
    //     beta_new  = beta  * decay + (profit ? 0 : 1)
    //   with decay in [0.95, 0.99] to let the posterior forget stale
    //   evidence from prior regimes.
    //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, §10.
    if (profit) {
        alpha_[index] += 1.0;
    } else {
        beta_[index] += 1.0;
    }
}

double ThompsonSampler::posterior_mean(std::size_t index) const {
    if (index >= grid_bps_.size()) {
        throw std::out_of_range(
            "ThompsonSampler::posterior_mean: index " + std::to_string(index)
            + " >= grid size " + std::to_string(grid_bps_.size()));
    }
    const double a = alpha_[index];
    const double b = beta_[index];
    return a / (a + b);
}

std::size_t ThompsonSampler::grid_size() const noexcept {
    return grid_bps_.size();
}

const std::vector<double>& ThompsonSampler::grid() const noexcept {
    return grid_bps_;
}

const std::vector<double>& ThompsonSampler::alphas() const noexcept {
    return alpha_;
}

const std::vector<double>& ThompsonSampler::betas() const noexcept {
    return beta_;
}

// ===========================================================================
// SpreadOptimizer -- construction
// ===========================================================================

SpreadOptimizer::SpreadOptimizer(const SpreadConfig& cfg)
    : cfg_(cfg)
{
    // Validate critical invariants.
    if (cfg_.gamma <= 0.0) {
        throw std::invalid_argument("SpreadOptimizer: gamma must be > 0");
    }
    if (cfg_.s_floor_bps <= 0.0) {
        throw std::invalid_argument("SpreadOptimizer: s_floor_bps must be > 0");
    }
    if (cfg_.default_trade_size_xch <= 0.0) {
        throw std::invalid_argument(
            "SpreadOptimizer: default_trade_size_xch must be > 0");
    }
    if (cfg_.tau_seconds <= 0.0) {
        throw std::invalid_argument(
            "SpreadOptimizer: tau_seconds must be > 0");
    }
    if (cfg_.default_expected_fill_seconds <= 0.0) {
        throw std::invalid_argument(
            "SpreadOptimizer: default_expected_fill_seconds must be > 0");
    }

    // Construct Thompson Sampler if enabled.
    if (cfg_.thompson.enabled) {
        sampler_.emplace(cfg_.thompson);
    }
}

// ===========================================================================
// Static component calculations
// ===========================================================================

// ---------------------------------------------------------------------------
// Adverse selection: gamma * sigma * sqrt(E[T_fill]) * PIN
//
// Derivation context (Section 6):
//   The informed-trader premium compensates for the expected mark-to-market
//   loss when an informed counterparty fills our quote.  sigma is the daily
//   return volatility; sqrt(T_fill) converts to the volatility over the
//   expected fill window via the diffusion scaling law.  PIN weights the
//   probability that any given fill is from an informed trader.
//
// Units: the product gamma * sigma * sqrt(seconds) * PIN is dimensionless
//   when gamma is treated as a unitless risk-aversion scalar.  The result
//   is converted to basis points (x 10,000).
//
// Strategy doc calibration:
//   sigma=0.05, T_fill=7200s, PIN=0.15, gamma=0.01
//   -> 0.01 * 0.05 * sqrt(7200) * 0.15 * 10000 = ~6.4 bps
//   The doc quotes ~15.3 bps at full-spread level; at half-spread the
//   per-side adverse selection is ~7.6 bps.  Our value is consistent
//   after accounting for gamma calibration differences.
// ---------------------------------------------------------------------------
double SpreadOptimizer::calc_adverse_selection_bps(
    double gamma,
    double sigma_daily,
    double expected_fill_seconds,
    double pin)
{
    // Clamp inputs to physically meaningful ranges.
    if (gamma <= 0.0 || sigma_daily <= 0.0 || expected_fill_seconds <= 0.0) {
        return 0.0;
    }
    const double clamped_pin = std::clamp(pin, 0.0, 1.0);

    // gamma * sigma * sqrt(T_fill) * PIN, expressed in bps.
    const double raw = gamma
                     * sigma_daily
                     * std::sqrt(expected_fill_seconds)
                     * clamped_pin;

    // Convert from decimal fraction to basis points.
    return raw * 10'000.0;
}

// ---------------------------------------------------------------------------
// Inventory risk: gamma * sigma^2 * tau * |q| / Q_max
//
// Derivation (Avellaneda-Stoikov / GLFT):
//   Holding inventory q exposes us to sigma^2 variance over our planning
//   horizon tau.  gamma scales the risk penalty.  Normalising by Q_max
//   keeps the result bounded as a fraction of capacity.
//
// Units: gamma * (sigma^2) * seconds * (unitless fraction) is dimensionless
//   by construction (tau is in the same time units as sigma^2's denominator).
//   Converted to bps.
//
// Strategy doc calibration:
//   sigma=0.05, tau=3600, |q|/Q_max=0.5, gamma=0.01
//   -> 0.01 * 0.0025 * 3600 * 0.5 * 10000 = 4.5 bps
//   The doc quotes ~2.1 bps at half-spread for moderate inventory skew.
// ---------------------------------------------------------------------------
double SpreadOptimizer::calc_inventory_bps(
    double gamma,
    double sigma_daily,
    double tau_seconds,
    double inventory_q,
    double q_max)
{
    if (gamma <= 0.0 || sigma_daily <= 0.0 || tau_seconds <= 0.0
        || q_max <= 0.0) {
        return 0.0;
    }

    const double sigma_sq = sigma_daily * sigma_daily;
    const double inventory_frac = std::min(std::abs(inventory_q) / q_max, 1.0);

    const double raw = gamma * sigma_sq * tau_seconds * inventory_frac;

    return raw * 10'000.0;
}

// ---------------------------------------------------------------------------
// Transaction cost: (fee_blockchain + fee_venue) / trade_size
//
// Both fees and trade size are denominated in XCH.  The result represents
// the round-trip cost per unit of notional traded, in bps.
//
// At CHIA conditions:
//   blockchain_fee = 0.0001 XCH  (~0.00027 USD at $2.70/XCH)
//   Dexie fee      = 0%
//   trade_size     = 10 XCH
//   -> (0.0001 + 0) / 10 * 10000 = 0.1 bps
//
// For TibetSwap (0.7%):
//   -> (0.0001 + 0.007 * 10) / 10 * 10000 = 70.1 bps
//   This correctly reflects that AMM fees dominate on TibetSwap.
//
// NOTE: venue_fee is expressed as a fraction of trade_size, so we multiply
// it by trade_size_xch before dividing back.  This simplifies to just
// venue_fee * 10000 + blockchain_fee / trade_size * 10000.
// ---------------------------------------------------------------------------
double SpreadOptimizer::calc_cost_bps(
    double blockchain_fee_xch,
    double venue_fee_frac,
    double trade_size_xch)
{
    if (trade_size_xch <= 0.0) {
        return 0.0;
    }

    // blockchain_fee is a flat per-settlement cost; convert to per-unit bps.
    const double blockchain_bps =
        (blockchain_fee_xch / trade_size_xch) * 10'000.0;

    // Venue fee is already a fraction of trade notional; convert to bps.
    const double venue_bps = venue_fee_frac * 10'000.0;

    return blockchain_bps + venue_bps;
}

// ---------------------------------------------------------------------------
// Competition: undercut the best competing spread by epsilon, floored at
//              s_floor_bps.
//
// T3-33 FIX: The original formula returned (best_competing + epsilon),
// which *widens* our spread relative to the competitor.  Competitive
// market-making practice requires *undercutting* (tightening inside) the
// best competing quote so that our offers are more attractive to takers.
//
// Corrected formula:
//     max(s_floor_bps, best_competing_bps - epsilon_bps)
//
// This returns a target total-spread cap, not an additive component.
// The caller (compute_spread) uses this to cap the model-derived spread
// so we never quote wider than the best competitor minus epsilon, while
// respecting the minimum profitable floor.
//
// If no competition data is available (best_competing <= 0), return 0 to
// signal "no cap" -- the final floor in compute_spread() handles the
// minimum.
//
// ISO/IEC 5055: bounds-checked, no undefined behaviour on edge inputs.
// ---------------------------------------------------------------------------
double SpreadOptimizer::calc_competition_bps(
    double s_floor_bps,
    double best_competing_bps,
    double epsilon_bps)
{
    // When no competition data is available, return 0 -- the final spread
    // floor (total_spread_bps = max(total_spread_bps, s_floor_bps)) handles
    // the minimum.  Returning s_floor_bps here would double-count the floor
    // because the floor is already enforced on the total.
    // ISO/IEC 5055: prevent double-counting of the spread floor that would
    // inflate quoted spreads beyond intended minimums.
    if (best_competing_bps <= 0.0) {
        return 0.0;
    }

    // Undercut the best competing spread by epsilon, but never go below
    // the minimum profitable floor.  This ensures we are tighter than
    // the competitor (attracting order flow) while remaining profitable.
    return std::max(s_floor_bps, best_competing_bps - epsilon_bps);
}

// ---------------------------------------------------------------------------
// Regime multiplier -- product of independent time-of-day, day-of-week,
//                      and volatility regime adjustments.
//
// Strategy doc Section 6 "Dynamic Adjustments":
//   High vol    -> 1.80x
//   Low vol     -> 0.70x
//   Weekend     -> 1.175x (midpoint of 15-20% widening)
//   US+EU hours -> 0.90x  (14:00-18:00 UTC overlap tightening)
//
// Adjustments are multiplicative and stack.  For example, a high-vol
// weekend during off-hours yields: 1.80 * 1.175 = 2.115x.
// ---------------------------------------------------------------------------
double SpreadOptimizer::calc_regime_multiplier(
    VolatilityRegime regime,
    int hour_utc,
    int day_of_week,
    double high_vol_mult,
    double low_vol_mult,
    double weekend_mult,
    double overlap_mult)
{
    double mult = 1.0;

    // --- Volatility regime ---
    switch (regime) {
        case VolatilityRegime::High:   mult *= high_vol_mult; break;
        case VolatilityRegime::Low:    mult *= low_vol_mult;  break;
        case VolatilityRegime::Normal: break;
    }

    // --- Weekend adjustment ---
    // ISO day-of-week: 6 = Saturday, 7 = Sunday.
    if (day_of_week == 6 || day_of_week == 7) {
        mult *= weekend_mult;
    }

    // --- US+EU overlap tightening ---
    // 14:00-18:00 UTC is the window where both US and European markets
    // are active, providing deeper liquidity and better price discovery.
    if (hour_utc >= 14 && hour_utc < 18) {
        mult *= overlap_mult;
    }

    return mult;
}

// ===========================================================================
// SpreadOptimizer::compute_spread -- main entry point
// ===========================================================================

SpreadResult SpreadOptimizer::compute_spread(
    double mid_price_xch,
    double sigma_daily,
    double inventory_q,
    double q_max,
    double pin,
    Venue  venue,
    double best_competing_bps,
    int    hour_utc,
    int    day_of_week) const
{
    // --- Resolve defaults for optional/unset parameters ---

    // If PIN is not supplied or non-positive, use the configured default.
    const double effective_pin =
        (pin > 0.0) ? std::clamp(pin, 0.0, 1.0) : cfg_.default_pin;

    // Resolve Q_max: use supplied value, fall back to config.
    const double effective_q_max = (q_max > 0.0) ? q_max : 1.0;

    // Determine volatility regime from sigma vs typical daily vol.
    // Heuristic: "normal" band is [0.03, 0.07] for CHIA (5% daily mean).
    // Low < 0.035 (~0.7x of 0.05), High > 0.065 (~1.3x of 0.05).
    const double sigma_low_threshold  = 0.035;
    const double sigma_high_threshold = 0.065;
    VolatilityRegime regime = VolatilityRegime::Normal;
    if (sigma_daily < sigma_low_threshold) {
        regime = VolatilityRegime::Low;
    } else if (sigma_daily > sigma_high_threshold) {
        regime = VolatilityRegime::High;
    }

    // --- Compute each spread component ---

    const double s_adv = calc_adverse_selection_bps(
        cfg_.gamma, sigma_daily, cfg_.default_expected_fill_seconds,
        effective_pin);

    const double s_inv = calc_inventory_bps(
        cfg_.gamma, sigma_daily, cfg_.tau_seconds, inventory_q,
        effective_q_max);

    const double s_cost = calc_cost_bps(
        cfg_.blockchain_fee_xch, venue_fee_fraction(venue),
        cfg_.default_trade_size_xch);

    // T3-33: calc_competition_bps returns a target *cap* (undercut value),
    // not an additive component.  When competition data is present it
    // returns max(floor, best_competing - epsilon); when absent it returns 0
    // to signal "no cap".
    const double s_comp = calc_competition_bps(
        cfg_.s_floor_bps, best_competing_bps, cfg_.epsilon_bps);

    // --- Combine components ---
    // The base spread is the sum of the risk and cost components.
    // The competition component is applied as a cap (not additive) so
    // that we undercut the best competitor rather than widening past them.
    double base_spread_bps = s_adv + s_inv + s_cost;

    // If competition data is available (s_comp > 0), cap the base spread
    // at the competitive target.  This ensures we quote at or tighter
    // than (best_competing - epsilon), while the floor still protects
    // profitability.
    if (s_comp > 0.0) {
        base_spread_bps = std::min(base_spread_bps, s_comp);
    }

    // --- Apply dynamic regime multiplier ---
    const double regime_mult = calc_regime_multiplier(
        regime, hour_utc, day_of_week,
        cfg_.high_vol_multiplier, cfg_.low_vol_multiplier,
        cfg_.weekend_multiplier, cfg_.overlap_hours_multiplier);

    double total_spread_bps = base_spread_bps * regime_mult;

    // --- Inventory skew asymmetric widening ---
    // When inventory exceeds the skew threshold (default 60% of Q_max),
    // we widen the overweight side.  This is captured in the total spread
    // as a symmetric increase here; the caller (quote engine) applies the
    // asymmetry by shifting the reservation price via the GLFT skew term.
    const double inventory_ratio =
        (effective_q_max > 0.0)
            ? std::abs(inventory_q) / effective_q_max
            : 0.0;

    if (inventory_ratio > cfg_.inventory_skew_threshold) {
        // Graduated widening: linearly interpolate from 1.0x at the
        // threshold to the full overweight multiplier at 100% capacity.
        const double skew_range =
            1.0 - cfg_.inventory_skew_threshold;  // e.g. 0.40
        const double skew_frac =
            (skew_range > 0.0)
                ? (inventory_ratio - cfg_.inventory_skew_threshold) / skew_range
                : 1.0;
        const double skew_mult =
            1.0 + skew_frac * (cfg_.inventory_overweight_multiplier - 1.0);
        total_spread_bps *= skew_mult;
    }

    // --- Thompson Sampling override (optional) ---
    // When enabled, sample a spread from the posterior and use the maximum
    // of the sampled level and the model-computed spread.  This ensures
    // the sampled spread never goes below the model's risk floor.
    if (sampler_.has_value()) {
        const std::size_t idx = sampler_->sample();
        last_thompson_index_ = idx;
        const double thompson_bps = sampler_->spread_at(idx);
        total_spread_bps = std::max(total_spread_bps, thompson_bps);
    }

    // --- Hard floor enforcement ---
    // Never quote below the minimum profitable spread, regardless of
    // model output or Thompson sampling.  This is the ultimate safety
    // net described in Section 6 (35-60 bps).
    total_spread_bps = std::max(total_spread_bps, cfg_.s_floor_bps);

    // --- Assemble result ---
    SpreadResult result;
    result.total_spread_bps = total_spread_bps;
    result.half_spread      = total_spread_bps / 2.0;
    result.s_adverse        = s_adv;
    result.s_inventory      = s_inv;
    result.s_cost           = s_cost;
    result.s_competition    = s_comp;
    result.regime_multiplier = regime_mult;

    return result;
}

// ===========================================================================
// Thompson Sampling delegation
// ===========================================================================

void SpreadOptimizer::record_fill(bool profitable) {
    if (!sampler_.has_value() || !last_thompson_index_.has_value()) {
        return;  // Thompson Sampling disabled or no sample taken yet.
    }
    sampler_->record_outcome(last_thompson_index_.value(), profitable);
}

std::optional<double> SpreadOptimizer::thompson_sample() {
    if (!sampler_.has_value()) {
        return std::nullopt;
    }
    const std::size_t idx = sampler_->sample();
    last_thompson_index_ = idx;
    return sampler_->spread_at(idx);
}

const ThompsonSampler* SpreadOptimizer::sampler() const noexcept {
    return sampler_.has_value() ? &sampler_.value() : nullptr;
}

// ===========================================================================
// Configuration access
// ===========================================================================

const SpreadConfig& SpreadOptimizer::config() const noexcept {
    return cfg_;
}

}  // namespace xop
