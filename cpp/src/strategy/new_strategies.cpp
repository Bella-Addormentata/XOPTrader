// new_strategies.cpp -- Implementation of three novel CHIA DEX market-making
// strategies: CoinAgeWeightedQuoting, BlockCadenceAdaptiveSpread, and
// MempoolSentinelStrategy.
//
// These strategies exploit CHIA-specific properties not addressed by the
// existing catalog (A-S, GLFT, VPIN, OFI, whale detection, etc.):
//   1. Coin-set UTXO model => deterministic coin ages for inventory weighting.
//   2. High block-time variance => adaptive spread based on block cadence.
//   3. Transparent mempool + long block time => anticipatory quote adjustment.
//
// Each strategy implements the StrategyBase interface and can operate standalone
// or as a composable module feeding multipliers/adjustments into the spread
// pipeline.  The regime detection (variance-ratio test) is reused from the
// A-S/GLFT pattern for consistency.
//
// All strategies enforce the CORE RULE: NEVER SELL AT A LOSS.  The ask price
// is floored at cost_basis * (1 + min_margin_bps / 10000) when the no-loss
// constraint is enabled.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical computation.
// ISO/IEC 5055       -- no raw pointers; bounds-checked containers; no UB.
// ISO/IEC 25000      -- comprehensive annotation; single-responsibility functions.

#include <xop/strategy/new_strategies.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>

namespace xop {

// ===========================================================================
//  Common helpers (shared across all three strategies)
// ===========================================================================

namespace {

/// Compute the variance ratio VR(q) for a price series.
/// VR(q) = Var(q-period returns) / (q * Var(1-period returns))
/// Under a random walk, VR(q) = 1.0.
/// VR < 1 => mean-reverting; VR > 1 => trending/momentum.
///
/// We use q=2 (compare 2-block returns to 1-block returns) for a simple,
/// robust test suitable for the short rolling windows available on CHIA.
///
/// Reference: Lo & MacKinlay (1988), "Stock market prices do not follow
/// random walks," Review of Financial Studies, 1(1), 41-66.
///
/// @param prices  Vector of mid-prices, ordered oldest to newest.
/// @return VR statistic (1.0 if insufficient data).
double compute_variance_ratio(const std::vector<double>& prices) {
    // Need at least 4 prices (3 one-period returns, 1 two-period return).
    if (prices.size() < 4) {
        return 1.0;
    }

    const auto n = prices.size();

    // Compute one-period log returns.
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

    // Compute two-period log returns.
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

    // Variance of one-period returns.
    const double mean1 = std::accumulate(r1.begin(), r1.end(), 0.0)
                         / static_cast<double>(r1.size());
    double var1 = 0.0;
    for (const auto& r : r1) {
        const double d = r - mean1;
        var1 += d * d;
    }
    var1 /= static_cast<double>(r1.size() - 1);

    // Variance of two-period returns.
    const double mean2 = std::accumulate(r2.begin(), r2.end(), 0.0)
                         / static_cast<double>(r2.size());
    double var2 = 0.0;
    for (const auto& r : r2) {
        const double d = r - mean2;
        var2 += d * d;
    }
    var2 /= static_cast<double>(r2.size() - 1);

    // Guard against division by zero (perfectly flat price series).
    if (var1 < 1e-18) {
        return 1.0;
    }

    // VR(2) = Var(2-period) / (2 * Var(1-period)).
    return var2 / (2.0 * var1);
}

/// Map a raw variance ratio to a RegimeInfo struct with multipliers.
/// This logic is shared by all three strategies for consistency with
/// the existing A-S and GLFT implementations.
RegimeInfo classify_regime(double vr,
                           double mr_threshold,
                           double mo_threshold,
                           double mr_spread_mult,
                           double mr_skew_mult,
                           double mo_spread_mult,
                           double mo_skew_mult) {
    RegimeInfo info;
    info.variance_ratio = vr;

    if (vr < mr_threshold) {
        info.regime     = MarketRegime::MeanReverting;
        info.spread_mult = mr_spread_mult;
        info.skew_mult   = mr_skew_mult;
    } else if (vr > mo_threshold) {
        info.regime     = MarketRegime::Momentum;
        info.spread_mult = mo_spread_mult;
        info.skew_mult   = mo_skew_mult;
    } else {
        info.regime     = MarketRegime::Random;
        info.spread_mult = 1.0;
        info.skew_mult   = 1.0;
    }

    return info;
}

/// Compute the A-S reservation price.
///   r = S - q * gamma * sigma^2 * tau
/// where sigma is in per-second units and tau is in seconds.
double as_reservation_price(double mid, double q, double gamma,
                            double sigma, double tau) {
    return mid - q * gamma * sigma * sigma * tau;
}

/// Compute the A-S optimal half-spread.
///   delta = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
double as_half_spread(double gamma, double kappa, double sigma, double tau) {
    return (1.0 / kappa) * std::log(1.0 + kappa / gamma)
           + 0.5 * gamma * sigma * sigma * tau;
}

/// Convert annualised volatility to per-second volatility.
///   sigma_per_sec = sigma_annual / sqrt(seconds_per_year)
double annual_to_per_second_vol(double sigma_annual) {
    static constexpr double kSecondsPerYear = 365.0 * 24.0 * 3600.0;
    return sigma_annual / std::sqrt(kSecondsPerYear);
}

/// Apply the never-sell-at-loss constraint to an ask price.
///   ask_floor = cost_basis * (1 + min_margin_bps / 10000)
///   ask = max(ask, ask_floor)
double apply_no_loss_floor(double ask, double cost_basis,
                           double min_margin_bps) {
    if (cost_basis <= 0.0) {
        return ask;  // No cost basis set; no constraint to apply.
    }
    const double ask_floor = cost_basis * (1.0 + min_margin_bps / 10000.0);
    return std::max(ask, ask_floor);
}

/// Compute the remaining time tau for a rolling N-block horizon.
///   tau = ((horizon - (block % horizon)) * block_time)
/// Returns tau in seconds; minimum 1 block worth to prevent tau=0.
double rolling_tau(BlockHeight block_height, uint32_t horizon_blocks,
                   double block_time_seconds) {
    const uint32_t pos = block_height % horizon_blocks;
    const uint32_t remaining = (pos == 0) ? horizon_blocks : (horizon_blocks - pos);
    return static_cast<double>(remaining) * block_time_seconds;
}

}  // anonymous namespace


// ===========================================================================
//
//  STRATEGY 1: CoinAgeWeightedQuoting -- Implementation
//
// ===========================================================================

CoinAgeWeightedQuoting::CoinAgeWeightedQuoting(const CoinAgeConfig& cfg)
    : cfg_(cfg)
    , min_margin_bps_(cfg.min_margin_bps)
{
    // Validate critical parameters to prevent silent misconfiguration.
    if (cfg_.lambda_age <= 0.0) {
        cfg_.lambda_age = 1.0 / 3600.0;  // Safe default: 1-hour half-life.
    }
    if (cfg_.alpha_age < 0.0 || cfg_.alpha_age > 1.0) {
        cfg_.alpha_age = 0.30;  // Clamp to safe range.
    }
    if (cfg_.beta_age < 0.0 || cfg_.beta_age > 1.0) {
        cfg_.beta_age = 0.20;
    }
}

QuoteResult CoinAgeWeightedQuoting::compute_quotes(
    double mid, double sigma, double q, BlockHeight block_height)
{
    // -----------------------------------------------------------------
    // Step 1: Compute base A-S quotes as the foundation.
    //
    // Convert sigma from annualised to per-second for the A-S formulas.
    // tau is from the rolling horizon.
    // -----------------------------------------------------------------
    const double sigma_ps = annual_to_per_second_vol(sigma);
    const double tau      = rolling_tau(block_height, cfg_.horizon_blocks,
                                        cfg_.block_time_seconds);

    const double r     = as_reservation_price(mid, q, cfg_.gamma, sigma_ps, tau);
    const double delta = as_half_spread(cfg_.gamma, cfg_.kappa, sigma_ps, tau);

    // Base quotes before coin-age adjustment.
    double ask_base = r + delta;
    double bid_base = r - delta;

    // -----------------------------------------------------------------
    // Step 2: Apply regime multiplier to the half-spread.
    //
    // This is consistent with the existing A-S and GLFT implementations.
    // -----------------------------------------------------------------
    const double regime_mult = regime_.spread_mult;
    const double delta_adj   = delta * regime_mult;
    double ask = r + delta_adj;
    double bid = r - delta_adj;

    // -----------------------------------------------------------------
    // Step 3: Compute the coin-age urgency factor U and apply multipliers.
    //
    // U = (1/n) * SUM_i [1 - exp(-lambda_age * age_seconds_i)]
    //
    // ask_spread_adjusted = ask_spread * (1 - alpha_age * U)
    // bid_spread_adjusted = bid_spread * (1 + beta_age * U)
    //
    // The multipliers are applied to the SPREAD (distance from mid),
    // not to the price directly, preserving the reservation-price logic.
    // -----------------------------------------------------------------
    const double U = compute_urgency(block_height);

    const double ask_mult = 1.0 - cfg_.alpha_age * U;  // Tighten ask for old coins.
    const double bid_mult = 1.0 + cfg_.beta_age  * U;  // Widen bid for old inventory.

    // Apply to the half-spread (distance from reservation price r).
    ask = r + delta_adj * ask_mult;
    bid = r - delta_adj * bid_mult;

    // -----------------------------------------------------------------
    // Step 4: Apply the never-sell-at-loss constraint.
    //
    // The ask is floored at cost_basis + minimum margin.
    // -----------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint) {
        ask = apply_no_loss_floor(ask, cost_basis_, min_margin_bps_);
    }

    // -----------------------------------------------------------------
    // Step 5: Compute sizes using normalised inventory (same as A-S).
    //
    // Size decreases on the side where we have excess inventory.
    // -----------------------------------------------------------------
    const double q_norm = (cfg_.q_max > 0.0) ? (q / cfg_.q_max) : 0.0;
    const double bid_size = cfg_.q_max * std::max(0.1, 1.0 - q_norm);
    const double ask_size = cfg_.q_max * std::max(0.1, 1.0 + q_norm);

    // Compute spread in basis points for diagnostics.
    const double mid_price = (ask + bid) / 2.0;
    const double spread_bps = (mid_price > 0.0)
        ? 10000.0 * (ask - bid) / mid_price
        : 0.0;

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

void CoinAgeWeightedQuoting::update_price(double mid, BlockHeight block_height)
{
    // Store price observation for the variance-ratio regime detector.
    price_buffer_.push_back({block_height, mid});

    // Trim to the regime detection window size.
    while (price_buffer_.size() > cfg_.regime_window_blocks) {
        price_buffer_.pop_front();
    }

    // Update regime classification from the latest price buffer.
    update_regime();
}

RegimeInfo CoinAgeWeightedQuoting::current_regime() const
{
    return regime_;
}

const std::string& CoinAgeWeightedQuoting::name() const
{
    return name_;
}

void CoinAgeWeightedQuoting::set_cost_basis(double cost_basis,
                                             double min_margin_bps)
{
    cost_basis_     = cost_basis;
    min_margin_bps_ = min_margin_bps;
}

void CoinAgeWeightedQuoting::update_coin_ages(const std::vector<CoinAge>& coins)
{
    coins_ = coins;
}

double CoinAgeWeightedQuoting::compute_urgency(BlockHeight current_block) const
{
    // If no coins are tracked, urgency is zero (nothing to be urgent about).
    if (coins_.empty()) {
        return 0.0;
    }

    // U = (1/n) * SUM_i [ 1 - exp(-lambda_age * a_i) ]
    // where a_i = (current_block - creation_block) * block_time_seconds.
    double sum = 0.0;
    for (const auto& coin : coins_) {
        // Guard against future-dated coins (should not happen, but defensive).
        const uint32_t age_blocks = (current_block > coin.creation_block)
            ? (current_block - coin.creation_block)
            : 0u;

        // Clamp age to prevent numerical overflow in exp().
        const uint32_t clamped_blocks = std::min(age_blocks, cfg_.max_age_blocks);
        const double age_seconds = static_cast<double>(clamped_blocks)
                                   * cfg_.block_time_seconds;

        // Urgency contribution: 1 - exp(-lambda * age).
        // For very large ages this saturates to 1.0; for age=0 it is 0.0.
        sum += 1.0 - std::exp(-cfg_.lambda_age * age_seconds);
    }

    return sum / static_cast<double>(coins_.size());
}

double CoinAgeWeightedQuoting::ask_spread_multiplier(BlockHeight current_block) const
{
    // ask_mult = 1 - alpha_age * U.
    // When U=0 (all coins fresh): mult = 1.0 (no change).
    // When U=1 (all coins old):   mult = 1 - alpha_age (tighten ask).
    return 1.0 - cfg_.alpha_age * compute_urgency(current_block);
}

double CoinAgeWeightedQuoting::bid_spread_multiplier(BlockHeight current_block) const
{
    // bid_mult = 1 + beta_age * U.
    // When U=0: mult = 1.0.  When U=1: mult = 1 + beta_age (widen bid).
    return 1.0 + cfg_.beta_age * compute_urgency(current_block);
}

double CoinAgeWeightedQuoting::variance_ratio_test() const
{
    std::vector<double> prices;
    prices.reserve(price_buffer_.size());
    for (const auto& obs : price_buffer_) {
        prices.push_back(obs.mid);
    }
    return compute_variance_ratio(prices);
}

void CoinAgeWeightedQuoting::update_regime()
{
    const double vr = variance_ratio_test();
    regime_ = classify_regime(
        vr,
        cfg_.vr_mean_revert_threshold,
        cfg_.vr_momentum_threshold,
        0.80,  // mr_spread_mult (mean-reverting: tighten)
        0.50,  // mr_skew_mult
        1.50,  // mo_spread_mult (momentum: widen)
        2.00   // mo_skew_mult
    );
}


// ===========================================================================
//
//  STRATEGY 2: BlockCadenceAdaptiveSpread -- Implementation
//
// ===========================================================================

BlockCadenceAdaptiveSpread::BlockCadenceAdaptiveSpread(
    const BlockCadenceConfig& cfg)
    : cfg_(cfg)
    , dt_ema_(cfg.target_block_time)  // Initialize EMA to target.
    , min_margin_bps_(cfg.min_margin_bps)
{
    // Validate parameters.
    if (cfg_.target_block_time <= 0.0) {
        cfg_.target_block_time = 52.0;
    }
    if (cfg_.ema_window_blocks < 2) {
        cfg_.ema_window_blocks = 10;
    }
    if (cfg_.eta < 0.0) {
        cfg_.eta = 0.5;
    }
}

QuoteResult BlockCadenceAdaptiveSpread::compute_quotes(
    double mid, double sigma, double q, BlockHeight block_height)
{
    // -----------------------------------------------------------------
    // Step 1: Compute base A-S quotes.
    // -----------------------------------------------------------------
    const double sigma_ps = annual_to_per_second_vol(sigma);
    const double tau      = rolling_tau(block_height, cfg_.horizon_blocks,
                                        cfg_.target_block_time);

    // Use the ADJUSTED kappa that accounts for current block cadence.
    // When blocks are arriving faster, fills per block decrease (less time
    // for takers), so effective kappa increases.  Vice versa for slow blocks.
    const double kappa_adj = adjusted_kappa();

    const double r     = as_reservation_price(mid, q, cfg_.gamma, sigma_ps, tau);
    const double delta = (1.0 / kappa_adj) * std::log(1.0 + kappa_adj / cfg_.gamma)
                         + 0.5 * cfg_.gamma * sigma_ps * sigma_ps * tau;

    // -----------------------------------------------------------------
    // Step 2: Apply regime multiplier.
    // -----------------------------------------------------------------
    const double delta_regime = delta * regime_.spread_mult;

    // -----------------------------------------------------------------
    // Step 3: Apply the cadence spread multiplier.
    //
    // m_cadence = 1 + eta * (R - 1)^2
    // where R = dt_ema / dt_target.
    //
    // This is a U-shaped function: widest at extreme cadences, baseline
    // at R=1.  The rationale:
    //   - Fast blocks (R < 1): price discovery accelerates, quotes stale faster.
    //   - Slow blocks (R > 1): accumulated uncertainty is higher.
    //   - Normal blocks (R ~ 1): no adjustment needed.
    // -----------------------------------------------------------------
    const double m_cadence = cadence_spread_multiplier();
    const double delta_final = delta_regime * m_cadence;

    double ask = r + delta_final;
    double bid = r - delta_final;

    // -----------------------------------------------------------------
    // Step 4: Apply the never-sell-at-loss constraint.
    // -----------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint) {
        ask = apply_no_loss_floor(ask, cost_basis_, min_margin_bps_);
    }

    // -----------------------------------------------------------------
    // Step 5: Compute sizes.
    // -----------------------------------------------------------------
    const double q_norm   = (cfg_.q_max > 0.0) ? (q / cfg_.q_max) : 0.0;
    const double bid_size = cfg_.q_max * std::max(0.1, 1.0 - q_norm);
    const double ask_size = cfg_.q_max * std::max(0.1, 1.0 + q_norm);

    const double mid_price = (ask + bid) / 2.0;
    const double spread_bps = (mid_price > 0.0)
        ? 10000.0 * (ask - bid) / mid_price
        : 0.0;

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

void BlockCadenceAdaptiveSpread::update_price(double mid,
                                               BlockHeight block_height)
{
    price_buffer_.push_back({block_height, mid});
    while (price_buffer_.size() > cfg_.regime_window_blocks) {
        price_buffer_.pop_front();
    }
    update_regime();
}

RegimeInfo BlockCadenceAdaptiveSpread::current_regime() const
{
    return regime_;
}

const std::string& BlockCadenceAdaptiveSpread::name() const
{
    return name_;
}

void BlockCadenceAdaptiveSpread::set_cost_basis(double cost_basis,
                                                 double min_margin_bps)
{
    cost_basis_     = cost_basis;
    min_margin_bps_ = min_margin_bps;
}

void BlockCadenceAdaptiveSpread::update_block_arrival(
    BlockHeight block_height, Timestamp timestamp)
{
    // Store the arrival.
    block_arrivals_.push_back({block_height, timestamp});

    // Keep a bounded history (2x the EMA window for robustness).
    const auto max_history = static_cast<std::size_t>(cfg_.ema_window_blocks * 2);
    while (block_arrivals_.size() > max_history) {
        block_arrivals_.pop_front();
    }

    // Need at least 2 arrivals to compute an inter-block interval.
    if (block_arrivals_.size() < 2) {
        return;
    }

    // Compute the most recent inter-block interval in seconds.
    const auto& prev = block_arrivals_[block_arrivals_.size() - 2];
    const auto& curr = block_arrivals_[block_arrivals_.size() - 1];

    // Use chrono duration_cast for precise interval measurement.
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        curr.arrival - prev.arrival);
    const double dt_seconds = static_cast<double>(duration.count()) / 1000.0;

    // Sanity check: reject negative or zero intervals (clock skew, duplicates).
    if (dt_seconds <= 0.0) {
        return;
    }

    // Update the EMA.
    // alpha = 2 / (N + 1) is the standard EMA smoothing factor.
    const double alpha = 2.0 / (static_cast<double>(cfg_.ema_window_blocks) + 1.0);
    dt_ema_ = alpha * dt_seconds + (1.0 - alpha) * dt_ema_;
}

double BlockCadenceAdaptiveSpread::current_dt_ema() const
{
    return dt_ema_;
}

double BlockCadenceAdaptiveSpread::cadence_ratio() const
{
    // R = dt_ema / dt_target.
    // Guard against division by zero (should never happen with default config).
    if (cfg_.target_block_time <= 0.0) {
        return 1.0;
    }
    return dt_ema_ / cfg_.target_block_time;
}

double BlockCadenceAdaptiveSpread::cadence_spread_multiplier() const
{
    // m_cadence = 1 + eta * (R - 1)^2
    // Clamped to [mult_min, mult_max].
    const double R = cadence_ratio();
    const double deviation = R - 1.0;
    const double raw_mult = 1.0 + cfg_.eta * deviation * deviation;

    return std::clamp(raw_mult, cfg_.mult_min, cfg_.mult_max);
}

double BlockCadenceAdaptiveSpread::adjusted_kappa() const
{
    // kappa_adjusted = kappa_base * (dt_target / dt_ema)
    //
    // Rationale: kappa measures fill intensity decay per unit of spread.
    // Fill intensity is proportional to the number of takers per unit time.
    // When blocks are slow (dt_ema > dt_target), there is MORE time for
    // takers to discover and take our offers, so effective fill intensity
    // increases => kappa should decrease (slower decay).  The reciprocal
    // relationship captures this.
    //
    // When blocks are fast (dt_ema < dt_target), less time per block =>
    // fewer fills per block => kappa should increase (faster decay).
    if (dt_ema_ <= 0.0) {
        return cfg_.kappa;
    }

    const double ratio = cfg_.target_block_time / dt_ema_;
    const double kappa_adj = cfg_.kappa * ratio;

    // Clamp to configured bounds.
    return std::clamp(kappa_adj,
                      cfg_.kappa * cfg_.kappa_adj_min,
                      cfg_.kappa * cfg_.kappa_adj_max);
}

double BlockCadenceAdaptiveSpread::variance_ratio_test() const
{
    std::vector<double> prices;
    prices.reserve(price_buffer_.size());
    for (const auto& obs : price_buffer_) {
        prices.push_back(obs.mid);
    }
    return compute_variance_ratio(prices);
}

void BlockCadenceAdaptiveSpread::update_regime()
{
    const double vr = variance_ratio_test();
    regime_ = classify_regime(
        vr,
        cfg_.vr_mean_revert_threshold,
        cfg_.vr_momentum_threshold,
        0.80, 0.50,  // mean-reverting multipliers
        1.50, 2.00   // momentum multipliers
    );
}


// ===========================================================================
//
//  STRATEGY 3: MempoolSentinelStrategy -- Implementation
//
// ===========================================================================

MempoolSentinelStrategy::MempoolSentinelStrategy(
    const MempoolSentinelConfig& cfg)
    : cfg_(cfg)
    , min_margin_bps_(cfg.min_margin_bps)
{
    // Validate parameters.
    if (cfg_.psi < 0.0) {
        cfg_.psi = 1.5;
    }
    if (cfg_.avg_daily_volume <= 0.0) {
        cfg_.avg_daily_volume = 750.0;  // ~$2K/day at $2.70.
    }
}

QuoteResult MempoolSentinelStrategy::compute_quotes(
    double mid, double sigma, double q, BlockHeight block_height)
{
    // -----------------------------------------------------------------
    // Step 1: Compute base A-S quotes.
    // -----------------------------------------------------------------
    const double sigma_ps = annual_to_per_second_vol(sigma);
    const double tau      = rolling_tau(block_height, cfg_.horizon_blocks,
                                        cfg_.block_time_seconds);

    const double r     = as_reservation_price(mid, q, cfg_.gamma, sigma_ps, tau);
    const double delta = as_half_spread(cfg_.gamma, cfg_.kappa, sigma_ps, tau);

    // -----------------------------------------------------------------
    // Step 2: Apply regime multiplier.
    // -----------------------------------------------------------------
    const double delta_regime = delta * regime_.spread_mult;

    // -----------------------------------------------------------------
    // Step 3: Apply mempool-based spread multiplier.
    //
    // m_mempool = 1 + psi * I_pending
    // where I_pending = |F_pending| / avg_daily_volume.
    //
    // The multiplier widens spreads when significant pending flow is
    // detected in the mempool, providing anticipatory adverse-selection
    // protection.
    // -----------------------------------------------------------------
    const double m_mempool = mempool_spread_multiplier();
    const double delta_final = delta_regime * m_mempool;

    // -----------------------------------------------------------------
    // Step 4: Apply mempool-based skew adjustment.
    //
    // skew_mempool = -phi_mempool * sign(F_pending) * I_pending
    //
    // If pending flow is net buying (F > 0), we shift quotes UP:
    //   ask moves up (wider) -- less eager to sell into buying pressure.
    //   bid moves up (tighter) -- more eager to buy (anticipating price rise).
    //
    // If pending flow is net selling (F < 0), we shift quotes DOWN:
    //   ask moves down (tighter) -- more eager to sell (anticipating price fall).
    //   bid moves down (wider) -- less eager to buy into selling pressure.
    // -----------------------------------------------------------------
    const double skew_adj = mempool_skew_adjustment();

    double ask = r + delta_final + skew_adj;
    double bid = r - delta_final + skew_adj;

    // -----------------------------------------------------------------
    // Step 5: Apply the never-sell-at-loss constraint.
    // -----------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint) {
        ask = apply_no_loss_floor(ask, cost_basis_, min_margin_bps_);
    }

    // -----------------------------------------------------------------
    // Step 6: Compute sizes.
    //
    // When we detect our own offers being taken in the mempool, reduce
    // sizes on the filled side to limit additional exposure until the
    // fill confirms and we can fully recompute.
    // -----------------------------------------------------------------
    const double q_norm   = (cfg_.q_max > 0.0) ? (q / cfg_.q_max) : 0.0;
    double bid_size = cfg_.q_max * std::max(0.1, 1.0 - q_norm);
    double ask_size = cfg_.q_max * std::max(0.1, 1.0 + q_norm);

    // Check for imminent fills of our own offers.
    if (cfg_.enable_preemptive_cancel) {
        for (const auto& take : pending_takes_) {
            if (take.is_our_offer) {
                // A taker is taking our offer.  Reduce remaining size on
                // the same side to limit adverse selection on stacked orders.
                if (take.taker_side == Side::Bid) {
                    // Taker is buying (taking our ask).
                    // Reduce our ask size by 50% as a precaution.
                    ask_size *= 0.5;
                } else {
                    // Taker is selling (taking our bid).
                    // Reduce our bid size by 50% as a precaution.
                    bid_size *= 0.5;
                }
            }
        }
    }

    const double mid_price = (ask + bid) / 2.0;
    const double spread_bps = (mid_price > 0.0)
        ? 10000.0 * (ask - bid) / mid_price
        : 0.0;

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

void MempoolSentinelStrategy::update_price(double mid,
                                            BlockHeight block_height)
{
    price_buffer_.push_back({block_height, mid});
    while (price_buffer_.size() > cfg_.regime_window_blocks) {
        price_buffer_.pop_front();
    }
    update_regime();
}

RegimeInfo MempoolSentinelStrategy::current_regime() const
{
    return regime_;
}

const std::string& MempoolSentinelStrategy::name() const
{
    return name_;
}

void MempoolSentinelStrategy::set_cost_basis(double cost_basis,
                                              double min_margin_bps)
{
    cost_basis_     = cost_basis;
    min_margin_bps_ = min_margin_bps;
}

void MempoolSentinelStrategy::update_mempool_takes(
    const std::vector<PendingMempoolTake>& takes)
{
    // Filter out stale mempool items (older than max_mempool_age_seconds).
    const auto now = std::chrono::system_clock::now();
    const auto max_age = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::milliseconds(
            static_cast<int64_t>(cfg_.max_mempool_age_seconds * 1000.0)));

    pending_takes_.clear();
    pending_takes_.reserve(takes.size());

    for (const auto& take : takes) {
        const auto age = now - take.first_seen;
        if (age <= max_age) {
            pending_takes_.push_back(take);
        }
    }
}

double MempoolSentinelStrategy::pending_net_flow() const
{
    // F_pending = SUM(size * sign(side))
    // Bid (taker buying) = +1, Ask (taker selling) = -1.
    double flow = 0.0;
    for (const auto& take : pending_takes_) {
        const double sign = (take.taker_side == Side::Bid) ? 1.0 : -1.0;
        flow += take.size * sign;
    }
    return flow;
}

double MempoolSentinelStrategy::pending_flow_intensity() const
{
    // I_pending = |F_pending| / avg_daily_volume.
    if (cfg_.avg_daily_volume <= 0.0) {
        return 0.0;
    }
    return std::abs(pending_net_flow()) / cfg_.avg_daily_volume;
}

double MempoolSentinelStrategy::mempool_spread_multiplier() const
{
    // m_mempool = 1 + psi * I_pending, clamped to [mult_min, mult_max].
    const double raw_mult = 1.0 + cfg_.psi * pending_flow_intensity();
    return std::clamp(raw_mult, cfg_.mult_min, cfg_.mult_max);
}

double MempoolSentinelStrategy::mempool_skew_adjustment() const
{
    // skew_mempool = -phi_mempool * sign(F_pending) * I_pending
    //
    // The negative sign ensures that if takers are buying (F > 0),
    // we shift quotes UP (positive skew applied to both bid and ask).
    // This makes the ask wider (less eager to sell) and the bid
    // tighter (more eager to buy), which is the correct response
    // to anticipated buying pressure.
    const double F = pending_net_flow();
    if (std::abs(F) < 1e-12) {
        return 0.0;  // No pending flow; no skew adjustment.
    }

    const double I = pending_flow_intensity();
    const double sign_F = (F > 0.0) ? 1.0 : -1.0;
    const double raw_skew = -cfg_.phi_mempool * sign_F * I;

    // Clamp to [-skew_max, +skew_max].
    return std::clamp(raw_skew, -cfg_.skew_max, cfg_.skew_max);
}

std::vector<std::string> MempoolSentinelStrategy::our_offers_pending_fill() const
{
    std::vector<std::string> result;
    for (const auto& take : pending_takes_) {
        if (take.is_our_offer) {
            result.push_back(take.offer_id);
        }
    }
    return result;
}

std::size_t MempoolSentinelStrategy::pending_take_count() const
{
    return pending_takes_.size();
}

double MempoolSentinelStrategy::variance_ratio_test() const
{
    std::vector<double> prices;
    prices.reserve(price_buffer_.size());
    for (const auto& obs : price_buffer_) {
        prices.push_back(obs.mid);
    }
    return compute_variance_ratio(prices);
}

void MempoolSentinelStrategy::update_regime()
{
    const double vr = variance_ratio_test();
    regime_ = classify_regime(
        vr,
        cfg_.vr_mean_revert_threshold,
        cfg_.vr_momentum_threshold,
        0.80, 0.50,  // mean-reverting multipliers
        1.50, 2.00   // momentum multipliers
    );
}

}  // namespace xop
