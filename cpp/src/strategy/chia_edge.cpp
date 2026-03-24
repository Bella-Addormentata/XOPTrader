// chia_edge.cpp -- Implementation of ChiaEdgeOptimizer for XOPTrader.
//
// Augments the Avellaneda-Stoikov base model with five multiplicative spread-
// tightening factors derived from structural advantages of the Chia DEX
// environment.  See chia_edge.hpp for the full mathematical derivations and
// scholarly references.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no credentials; pure algorithmic computation.
//   ISO/IEC 5055       -- no raw pointers; bounds-checked; no UB.
//   ISO/IEC 25000      -- documented formulas; single-responsibility.
//   ISO/IEC JTC 1/SC 22 -- standard C++20.

#include <xop/strategy/chia_edge.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace xop {

// ===========================================================================
// Helpers (file-local)
// ===========================================================================

namespace {

/// Variance-ratio VR(2) from a price series.  Returns 1.0 (random walk) if
/// insufficient data.  Shared logic with other strategy implementations.
double compute_variance_ratio(const std::vector<double>& prices) {
    if (prices.size() < 4) {
        return 1.0;
    }

    const auto n = prices.size();

    // One-period log returns.
    std::vector<double> r1;
    r1.reserve(n - 1);
    for (std::size_t i = 1; i < n; ++i) {
        if (prices[i - 1] > 0.0 && prices[i] > 0.0) {
            r1.push_back(std::log(prices[i] / prices[i - 1]));
        }
    }
    if (r1.size() < 3) {
        return 1.0;
    }

    // Two-period log returns.
    std::vector<double> r2;
    r2.reserve(n - 2);
    for (std::size_t i = 2; i < n; ++i) {
        if (prices[i - 2] > 0.0 && prices[i] > 0.0) {
            r2.push_back(std::log(prices[i] / prices[i - 2]));
        }
    }
    if (r2.empty()) {
        return 1.0;
    }

    // Variance of 1-period returns (sample variance).
    const double mean1 = std::accumulate(r1.begin(), r1.end(), 0.0)
                         / static_cast<double>(r1.size());
    double var1 = 0.0;
    for (const auto& r : r1) {
        const double d = r - mean1;
        var1 += d * d;
    }
    var1 /= static_cast<double>(r1.size() - 1);

    // Variance of 2-period returns.
    const double mean2 = std::accumulate(r2.begin(), r2.end(), 0.0)
                         / static_cast<double>(r2.size());
    double var2 = 0.0;
    for (const auto& r : r2) {
        const double d = r - mean2;
        var2 += d * d;
    }
    var2 /= static_cast<double>(r2.size() - 1);

    if (var1 < 1e-18) {
        return 1.0;
    }

    return var2 / (2.0 * var1);
}

}  // anonymous namespace

// ===========================================================================
// Construction
// ===========================================================================

ChiaEdgeOptimizer::ChiaEdgeOptimizer(const ChiaEdgeConfig& cfg)
    : cfg_{cfg}
    , min_margin_bps_{cfg.min_margin_bps}
{
    // Validate critical parameters; reset to safe defaults on invalid input.
    if (cfg_.gamma <= 0.0) cfg_.gamma = 0.01;
    if (cfg_.kappa <= 0.0) cfg_.kappa = 1.5;
    if (cfg_.q_max <= 0.0) cfg_.q_max = 1000.0;
    if (cfg_.block_time_seconds <= 0.0) cfg_.block_time_seconds = 52.0;
    if (cfg_.reference_spread_bps <= 0.0) cfg_.reference_spread_bps = 200.0;
    if (cfg_.spread_floor_bps < 0.0) cfg_.spread_floor_bps = 40.0;
}

// ===========================================================================
// StrategyBase — compute_quotes()
// ===========================================================================

QuoteResult ChiaEdgeOptimizer::compute_quotes(
    double mid, double sigma, double q, BlockHeight block_height)
{
    // -----------------------------------------------------------------------
    // Step 1: Convert volatility to per-second units for A-S formulas.
    //   sigma_ps = sigma_annual / sqrt(seconds_per_year)
    // -----------------------------------------------------------------------
    const double sigma_ps = sigma / std::sqrt(kSecondsPerYear);

    // -----------------------------------------------------------------------
    // Step 2: Compute remaining time in the rolling horizon (seconds).
    //   tau = remaining_blocks * block_time
    // -----------------------------------------------------------------------
    const double tau = compute_tau(block_height);

    // -----------------------------------------------------------------------
    // Step 3: A-S reservation price (inventory-adjusted mid).
    //   r = S - q * gamma * sigma² * tau
    // -----------------------------------------------------------------------
    const double r = reservation_price(mid, sigma, q, tau);

    // -----------------------------------------------------------------------
    // Step 4: A-S optimal half-spread (before any multipliers).
    //   delta = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma² * tau
    // -----------------------------------------------------------------------
    const double delta_base = optimal_half_spread(sigma, tau);

    // -----------------------------------------------------------------------
    // Step 5: Update reference spread for edge multiplier calculations.
    //   The multiplier formulas use bps savings / reference spread.
    // -----------------------------------------------------------------------
    const double delta_bps = (mid > 0.0) ? (delta_base / mid * 10000.0) : cfg_.reference_spread_bps;
    if (delta_bps > 10.0) {
        // Only update if we have a meaningful spread; avoids polluting the
        // reference with degenerate near-zero values.
        cfg_.reference_spread_bps = delta_bps;
    }

    // -----------------------------------------------------------------------
    // Step 6: Apply regime multiplier.
    // -----------------------------------------------------------------------
    const double delta_regime = delta_base * regime_.spread_mult;

    // -----------------------------------------------------------------------
    // Step 7: Apply composite CHIA-edge multiplier.
    //   m_composite = m_atomic * m_cancel * m_utxo * m_block_time * m_mempool
    //   All factors are in (0, 1], so the product tightens the spread.
    // -----------------------------------------------------------------------
    const double m_edge = composite_edge_multiplier();
    const double delta_final = delta_regime * m_edge;

    // -----------------------------------------------------------------------
    // Step 8: Construct bid and ask prices.
    // -----------------------------------------------------------------------
    double ask = r + delta_final;
    double bid = r - delta_final;

    // -----------------------------------------------------------------------
    // Step 9: Enforce the 40 bps spread floor.
    //   No combination of multipliers should push below this level.
    // -----------------------------------------------------------------------
    const double floor_half = mid * cfg_.spread_floor_bps / 20000.0;
    if (delta_final < floor_half && mid > 0.0) {
        ask = r + floor_half;
        bid = r - floor_half;
    }

    // -----------------------------------------------------------------------
    // Step 10: Apply the never-sell-at-loss constraint.
    //   ask >= cost_basis * (1 + min_margin_bps / 10000)
    // -----------------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint && cost_basis_ > 0.0) {
        const double ask_floor = cost_basis_ * (1.0 + min_margin_bps_ / 10000.0);
        ask = std::max(ask, ask_floor);
    }

    // -----------------------------------------------------------------------
    // Step 11: Compute bid/ask sizes with inventory-aware scaling.
    //   Reduce size on the overweight side.
    // -----------------------------------------------------------------------
    const double q_norm   = (cfg_.q_max > 0.0) ? (q / cfg_.q_max) : 0.0;
    const double bid_size = cfg_.q_max * std::max(0.1, 1.0 - q_norm);
    const double ask_size = cfg_.q_max * std::max(0.1, 1.0 + q_norm);

    // -----------------------------------------------------------------------
    // Step 12: Compute diagnostic spread in basis points.
    // -----------------------------------------------------------------------
    const double mid_price = (ask + bid) / 2.0;
    const double spread_bps = (mid_price > 0.0)
        ? 10000.0 * (ask - bid) / mid_price
        : 0.0;

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

// ===========================================================================
// StrategyBase — update_price()
// ===========================================================================

void ChiaEdgeOptimizer::update_price(double mid, BlockHeight block_height)
{
    price_buffer_.push_back({block_height, mid});

    while (price_buffer_.size() > cfg_.regime_window_blocks) {
        price_buffer_.pop_front();
    }

    update_regime();
}

// ===========================================================================
// StrategyBase — accessors
// ===========================================================================

RegimeInfo ChiaEdgeOptimizer::current_regime() const
{
    return regime_;
}

const std::string& ChiaEdgeOptimizer::name() const
{
    return name_;
}

void ChiaEdgeOptimizer::set_cost_basis(double cost_basis, double min_margin_bps)
{
    cost_basis_     = cost_basis;
    min_margin_bps_ = min_margin_bps;
}

// ===========================================================================
// Individual edge multipliers
// ===========================================================================

double ChiaEdgeOptimizer::atomic_offer_multiplier() const
{
    // m_atomic = 1.0 - atomic_tightening_bps / reference_spread_bps
    // Clamped to [floor, 1.0].
    if (cfg_.reference_spread_bps <= 0.0) return 1.0;

    const double m = 1.0 - cfg_.atomic_tightening_bps / cfg_.reference_spread_bps;
    return std::clamp(m, cfg_.atomic_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::free_cancel_multiplier() const
{
    // m_cancel = 1.0 - cancel_savings_bps / reference_spread_bps
    if (cfg_.reference_spread_bps <= 0.0) return 1.0;

    const double m = 1.0 - cfg_.cancel_savings_bps / cfg_.reference_spread_bps;
    return std::clamp(m, cfg_.cancel_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::utxo_parallel_multiplier() const
{
    // Fill probability scales with active tiers:
    //   m_utxo_fill = 1.0 + bonus * (tiers - 1)
    //   m_utxo_spread = 1.0 / m_utxo_fill  (more fills => can afford tighter)
    const double fill_mult = 1.0 + cfg_.utxo_fill_bonus_pct
                             * static_cast<double>(std::max(1u, cfg_.active_tiers) - 1u);

    if (fill_mult <= 0.0) return 1.0;

    const double m = 1.0 / fill_mult;
    return std::clamp(m, cfg_.utxo_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::block_time_multiplier() const
{
    // m_block_time = 1.0 - latency_savings_bps / reference_spread_bps
    if (cfg_.reference_spread_bps <= 0.0) return 1.0;

    const double m = 1.0 - cfg_.latency_savings_bps / cfg_.reference_spread_bps;
    return std::clamp(m, cfg_.block_time_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::mempool_info_multiplier() const
{
    // mempool_info_ratio = observation_window / block_time
    // m_mempool = 1.0 - info_bps * ratio / reference_spread_bps
    if (cfg_.reference_spread_bps <= 0.0 || cfg_.block_time_seconds <= 0.0) {
        return 1.0;
    }

    const double ratio = cfg_.mempool_window_seconds / cfg_.block_time_seconds;
    const double m = 1.0 - cfg_.mempool_info_bps * ratio / cfg_.reference_spread_bps;
    return std::clamp(m, cfg_.mempool_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::composite_edge_multiplier() const
{
    // Multiplicative combination of all five structural edges.
    return atomic_offer_multiplier()
         * free_cancel_multiplier()
         * utxo_parallel_multiplier()
         * block_time_multiplier()
         * mempool_info_multiplier();
}

// ===========================================================================
// A-S model helpers
// ===========================================================================

double ChiaEdgeOptimizer::reservation_price(
    double mid, double sigma, double q, double tau) const
{
    // r = S - q * gamma * sigma_ps² * tau
    const double sigma_ps = sigma / std::sqrt(kSecondsPerYear);
    return mid - q * cfg_.gamma * sigma_ps * sigma_ps * tau;
}

double ChiaEdgeOptimizer::optimal_half_spread(double sigma, double tau) const
{
    // delta = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma_ps² * tau
    const double sigma_ps = sigma / std::sqrt(kSecondsPerYear);
    return (1.0 / cfg_.kappa) * std::log(1.0 + cfg_.kappa / cfg_.gamma)
           + 0.5 * cfg_.gamma * sigma_ps * sigma_ps * tau;
}

double ChiaEdgeOptimizer::compute_tau(BlockHeight block_height) const
{
    // Remaining time in the rolling horizon, in seconds.
    // tau = remaining_blocks * block_time
    // Minimum 1 block to prevent tau = 0 (which collapses the A-S model).
    const uint32_t pos = block_height % cfg_.horizon_blocks;
    const uint32_t remaining = (pos == 0) ? cfg_.horizon_blocks
                                          : (cfg_.horizon_blocks - pos);
    return static_cast<double>(remaining) * cfg_.block_time_seconds;
}

double ChiaEdgeOptimizer::per_block_volatility(
    double sigma_annual, double block_time_seconds)
{
    // sigma_block = sigma_annual * sqrt(block_time / seconds_per_year)
    static constexpr double kSecPerYear = 365.0 * 24.0 * 3600.0;
    return sigma_annual * std::sqrt(block_time_seconds / kSecPerYear);
}

// ===========================================================================
// Regime detection
// ===========================================================================

double ChiaEdgeOptimizer::variance_ratio_test() const
{
    std::vector<double> prices;
    prices.reserve(price_buffer_.size());
    for (const auto& obs : price_buffer_) {
        prices.push_back(obs.mid);
    }
    return compute_variance_ratio(prices);
}

void ChiaEdgeOptimizer::update_regime()
{
    const double vr = variance_ratio_test();

    regime_.variance_ratio = vr;

    if (vr < cfg_.vr_mean_revert_threshold) {
        regime_.regime     = MarketRegime::MeanReverting;
        regime_.spread_mult = cfg_.regime_mr_spread_mult;
        regime_.skew_mult   = cfg_.regime_mr_skew_mult;
    } else if (vr > cfg_.vr_momentum_threshold) {
        regime_.regime     = MarketRegime::Momentum;
        regime_.spread_mult = cfg_.regime_mo_spread_mult;
        regime_.skew_mult   = cfg_.regime_mo_skew_mult;
    } else {
        regime_.regime     = MarketRegime::Random;
        regime_.spread_mult = 1.0;
        regime_.skew_mult   = 1.0;
    }
}

}  // namespace xop
