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
// Volatility convention:
//   sigma_block = sigma_annual * sqrt(52.0 / kSecondsPerYear)
//   tau         = remaining_blocks * 52.0 / kSecondsPerYear  (in years)
//
// Spread floor: 40 bps minimum (0.004 * mid) enforced on all three strategies.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical computation.
// ISO/IEC 5055       -- no raw pointers; bounds-checked containers; no UB.
// ISO/IEC 25000      -- comprehensive annotation; single-responsibility functions.

#include <xop/strategy/new_strategies.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace xop {

// ===========================================================================
//  Common helpers (shared across all three strategies)
// ===========================================================================

namespace {

/// Minimum spread expressed as a fraction of mid-price.
/// 40 bps = 0.004.  Applied as a floor on the full spread (ask - bid)
/// across all three strategies to prevent sub-economic quoting.
static constexpr double kMinSpreadFraction = 0.004;

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

    // Compute one-period log returns (non-overlapping at lag 1).
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

    // Compute two-period log returns (overlapping at lag 2).
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

    // Variance of one-period returns (unbiased, N-1 denominator).
    const double mean1 = std::accumulate(r1.begin(), r1.end(), 0.0)
                         / static_cast<double>(r1.size());
    double var1 = 0.0;
    for (const auto& r : r1) {
        const double d = r - mean1;
        var1 += d * d;
    }
    var1 /= static_cast<double>(r1.size() - 1);

    // Variance of two-period returns (unbiased, N-1 denominator).
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
        // Mean-reverting: tighten spreads, reduce inventory shedding.
        info.regime      = MarketRegime::MeanReverting;
        info.spread_mult = mr_spread_mult;
        info.skew_mult   = mr_skew_mult;
    } else if (vr > mo_threshold) {
        // Momentum: widen spreads, aggressive inventory shedding.
        info.regime      = MarketRegime::Momentum;
        info.spread_mult = mo_spread_mult;
        info.skew_mult   = mo_skew_mult;
    } else {
        // Random walk: no adjustment.
        info.regime      = MarketRegime::Random;
        info.spread_mult = 1.0;
        info.skew_mult   = 1.0;
    }

    return info;
}

/// Convert annualised volatility to per-block volatility.
///   sigma_block = sigma_annual * sqrt(block_time_seconds / kSecondsPerYear)
///
/// For CHIA with block_time = 52 s:
///   sigma_block = sigma_annual * sqrt(52.0 / 31536000.0)
///               = sigma_annual * 1.2843e-3
///
/// @param sigma_annual  Annualised volatility (dimensionless fraction).
/// @param block_time_seconds  Block interval in seconds (default 52.0).
/// @param seconds_per_year  Calendar seconds in a year.
/// @return Per-block standard deviation.
double annual_to_per_block_vol(double sigma_annual,
                               double block_time_seconds,
                               double seconds_per_year) {
    return sigma_annual * std::sqrt(block_time_seconds / seconds_per_year);
}

/// Compute the remaining time tau as a fraction of one year.
///   tau = remaining_blocks * block_time_seconds / kSecondsPerYear
///
/// The rolling horizon ensures tau never reaches zero: when the block
/// height modulo horizon_blocks equals zero the window resets to full.
///
/// @param block_height  Current block height.
/// @param horizon_blocks  Total horizon length in blocks.
/// @param block_time_seconds  Block interval in seconds.
/// @param seconds_per_year  Calendar seconds in a year.
/// @return Remaining time in years (always > 0).
double rolling_tau_years(BlockHeight block_height,
                         uint32_t horizon_blocks,
                         double block_time_seconds,
                         double seconds_per_year) {
    const uint32_t pos       = block_height % horizon_blocks;
    const uint32_t remaining = (pos == 0) ? horizon_blocks
                                          : (horizon_blocks - pos);
    return static_cast<double>(remaining) * block_time_seconds
           / seconds_per_year;
}

/// Compute the A-S reservation price using per-block sigma and tau in years.
///   r = S - q * gamma * sigma_block^2 * tau_years
///
/// @param mid    Current mid-price (quote per base).
/// @param q      Current net inventory (positive = long).
/// @param gamma  Risk aversion coefficient.
/// @param sigma  Per-block volatility.
/// @param tau    Remaining time in years.
/// @return Reservation price.
double as_reservation_price(double mid, double q, double gamma,
                            double sigma, double tau) {
    return mid - q * gamma * sigma * sigma * tau;
}

/// Compute the A-S optimal half-spread using per-block sigma and tau in years.
///   delta = (1/kappa) * ln(1 + kappa/gamma)
///         + 0.5 * gamma * sigma_block^2 * tau_years
///
/// @param gamma  Risk aversion coefficient.
/// @param kappa  Fill intensity decay parameter.
/// @param sigma  Per-block volatility.
/// @param tau    Remaining time in years.
/// @return Optimal half-spread (distance from reservation price to each side).
double as_half_spread(double gamma, double kappa,
                      double sigma, double tau) {
    return (1.0 / kappa) * std::log(1.0 + kappa / gamma)
           + 0.5 * gamma * sigma * sigma * tau;
}

/// Apply the never-sell-at-loss constraint to an ask price.
///   ask_floor = cost_basis * (1 + min_margin_bps / 10000)
///   ask = max(ask, ask_floor)
///
/// @param ask             Raw ask price from the model.
/// @param cost_basis      Weighted-average acquisition cost per unit.
/// @param min_margin_bps  Minimum margin above cost basis in basis points.
/// @return Constrained ask price (>= ask_floor when cost_basis > 0).
double apply_no_loss_floor(double ask, double cost_basis,
                           double min_margin_bps) {
    if (cost_basis <= 0.0) {
        return ask;  // No cost basis set; no constraint to apply.
    }
    const double ask_floor = cost_basis * (1.0 + min_margin_bps / 10000.0);
    return std::max(ask, ask_floor);
}

/// Enforce the 40 bps minimum spread floor on a bid/ask pair.
/// If the current spread (ask - bid) is less than kMinSpreadFraction * mid,
/// symmetrically widen both sides around their midpoint.
///
/// @param[in,out] bid  Bid price (may be raised or lowered).
/// @param[in,out] ask  Ask price (may be raised or lowered).
/// @param mid          Reference mid-price for the floor computation.
void enforce_spread_floor(double& bid, double& ask, double mid) {
    const double min_spread = kMinSpreadFraction * mid;
    const double current_spread = ask - bid;
    if (current_spread < min_spread && mid > 0.0) {
        // Symmetrically widen around the midpoint of the current quotes.
        const double quote_mid  = (ask + bid) * 0.5;
        const double half_floor = min_spread * 0.5;
        bid = quote_mid - half_floor;
        ask = quote_mid + half_floor;
        spdlog::debug("Spread floor enforced: bid={:.6f} ask={:.6f} "
                       "floor={:.1f} bps", bid, ask,
                       kMinSpreadFraction * 10000.0);
    }
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
        spdlog::warn("CoinAgeWeightedQuoting: lambda_age <= 0, "
                      "resetting to default 1/3600");
        cfg_.lambda_age = 1.0 / 3600.0;
    }
    if (cfg_.alpha_age < 0.0 || cfg_.alpha_age > 1.0) {
        spdlog::warn("CoinAgeWeightedQuoting: alpha_age out of [0,1], "
                      "resetting to 0.30");
        cfg_.alpha_age = 0.30;
    }
    if (cfg_.beta_age < 0.0 || cfg_.beta_age > 1.0) {
        spdlog::warn("CoinAgeWeightedQuoting: beta_age out of [0,1], "
                      "resetting to 0.20");
        cfg_.beta_age = 0.20;
    }
}

QuoteResult CoinAgeWeightedQuoting::compute_quotes(
    double mid, double sigma, double q, BlockHeight block_height)
{
    // -----------------------------------------------------------------
    // Step 1: Compute tau in years for the A-S model.
    //
    //   tau = remaining_blocks * block_time_seconds / kSecondsPerYear
    //
    // sigma is already annualized; tau is in years from rolling_tau_years().
    // sigma_annual^2 * tau_years = variance in years (dimensionally correct).
    // ISO/IEC 5055: correct dimensional analysis -- no per-block conversion
    // that would produce sigma_block^2 * tau_years ~6 orders too small.
    // -----------------------------------------------------------------
    const double tau = rolling_tau_years(
        block_height, cfg_.horizon_blocks,
        cfg_.block_time_seconds, kSecondsPerYear);

    // -----------------------------------------------------------------
    // Step 2: Compute A-S reservation price and half-spread.
    //
    //   r     = mid - q * gamma * sigma_annual^2 * tau_years
    //   delta = (1/kappa) * ln(1 + kappa/gamma)
    //         + 0.5 * gamma * sigma_annual^2 * tau_years
    // -----------------------------------------------------------------
    const double r     = as_reservation_price(mid, q, cfg_.gamma,
                                              sigma, tau);
    const double delta = as_half_spread(cfg_.gamma, cfg_.kappa,
                                        sigma, tau);

    // -----------------------------------------------------------------
    // Step 3: Apply regime multiplier to the half-spread.
    //
    // Consistent with the existing A-S and GLFT implementations.
    // -----------------------------------------------------------------
    const double delta_adj = delta * regime_.spread_mult;

    // -----------------------------------------------------------------
    // Step 4: Compute coin-age urgency U and apply spread multipliers.
    //
    //   U = (1/n) * SUM_i [1 - exp(-lambda_age * a_i)]
    //   ask_mult = (1 - alpha_age * U), clamped to [1-alpha_age, 1.0]
    //   bid_mult = (1 + beta_age  * U), clamped to [1.0, 1+beta_age]
    //
    // Multipliers are applied to the half-spread (distance from the
    // reservation price r), preserving the A-S inventory-skew logic.
    // -----------------------------------------------------------------
    const double ask_mult = ask_spread_multiplier(block_height);
    const double bid_mult = bid_spread_multiplier(block_height);

    double ask = r + delta_adj * ask_mult;
    double bid = r - delta_adj * bid_mult;

    // -----------------------------------------------------------------
    // Step 5: Apply the never-sell-at-loss constraint.
    //
    // If enabled, clamp ask >= cost_basis * (1 + min_margin_bps/10000).
    // -----------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint) {
        ask = apply_no_loss_floor(ask, cost_basis_, min_margin_bps_);
    }

    // -----------------------------------------------------------------
    // Step 6: Enforce the 40 bps minimum spread floor.
    // -----------------------------------------------------------------
    enforce_spread_floor(bid, ask, mid);

    // -----------------------------------------------------------------
    // Step 7: Compute sizes using normalised inventory.
    //
    // Size decreases on the side where we have excess inventory.
    // Minimum 10% of q_max to always maintain market presence.
    // -----------------------------------------------------------------
    const double q_norm   = (cfg_.q_max > 0.0) ? (q / cfg_.q_max) : 0.0;
    const double bid_size = cfg_.q_max * std::max(0.1, 1.0 - q_norm);
    const double ask_size = cfg_.q_max * std::max(0.1, 1.0 + q_norm);

    // Compute spread in basis points for diagnostics.
    const double mid_price = (ask + bid) * 0.5;
    const double spread_bps = (mid_price > 0.0)
        ? 10000.0 * (ask - bid) / mid_price
        : 0.0;

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

void CoinAgeWeightedQuoting::update_price(double mid,
                                           BlockHeight block_height)
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

void CoinAgeWeightedQuoting::update_coin_ages(
    const std::vector<CoinAge>& coins)
{
    coins_ = coins;
}

double CoinAgeWeightedQuoting::compute_urgency(
    BlockHeight current_block) const
{
    // If no coins are tracked, urgency is zero (nothing to be urgent about).
    if (coins_.empty()) {
        return 0.0;
    }

    // U = (1/n) * SUM_i [ 1 - exp(-lambda_age * a_i) ]
    // where a_i = (current_block - creation_block) * 52.0 seconds.
    double sum = 0.0;
    for (const auto& coin : coins_) {
        // Guard against future-dated coins (should not happen, but defensive).
        const uint32_t age_blocks = (current_block > coin.creation_block)
            ? (current_block - coin.creation_block)
            : 0u;

        // Clamp age to prevent numerical overflow in exp().
        const uint32_t clamped_blocks = std::min(age_blocks,
                                                  cfg_.max_age_blocks);
        const double age_seconds = static_cast<double>(clamped_blocks)
                                   * cfg_.block_time_seconds;

        // Urgency contribution: 1 - exp(-lambda * age).
        // For very large ages this saturates to 1.0; for age=0 it is 0.0.
        sum += 1.0 - std::exp(-cfg_.lambda_age * age_seconds);
    }

    return sum / static_cast<double>(coins_.size());
}

double CoinAgeWeightedQuoting::ask_spread_multiplier(
    BlockHeight current_block) const
{
    // ask_mult = (1 - alpha_age * U), clamped to [1 - alpha_age, 1.0].
    //
    // When U=0 (all coins fresh): mult = 1.0 (no change).
    // When U~1 (all coins old):   mult = 1 - alpha_age (max tightening).
    const double U = compute_urgency(current_block);
    const double raw = 1.0 - cfg_.alpha_age * U;
    return std::clamp(raw, 1.0 - cfg_.alpha_age, 1.0);
}

double CoinAgeWeightedQuoting::bid_spread_multiplier(
    BlockHeight current_block) const
{
    // bid_mult = (1 + beta_age * U), clamped to [1.0, 1 + beta_age].
    //
    // When U=0: mult = 1.0.  When U~1: mult = 1 + beta_age (max widening).
    const double U = compute_urgency(current_block);
    const double raw = 1.0 + cfg_.beta_age * U;
    return std::clamp(raw, 1.0, 1.0 + cfg_.beta_age);
}

double CoinAgeWeightedQuoting::variance_ratio_test() const
{
    // Extract the price series from the rolling buffer for the VR test.
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
        0.80,  // mr_spread_mult (mean-reverting: tighten to 80%)
        0.50,  // mr_skew_mult   (halve inventory shedding)
        1.50,  // mo_spread_mult (momentum: widen to 150%)
        2.00   // mo_skew_mult   (double inventory shedding)
    );
    spdlog::trace("CoinAge regime: VR={:.4f} regime={}",
                   vr, to_string(regime_.regime));
}


// ===========================================================================
//
//  STRATEGY 2: BlockCadenceAdaptiveSpread -- Implementation
//
// ===========================================================================

BlockCadenceAdaptiveSpread::BlockCadenceAdaptiveSpread(
    const BlockCadenceConfig& cfg)
    : cfg_(cfg)
    , dt_ema_(cfg.target_block_time)  // Initialize EMA to target cadence.
    , min_margin_bps_(cfg.min_margin_bps)
{
    // Validate parameters with safe fallbacks.
    if (cfg_.target_block_time <= 0.0) {
        spdlog::warn("BlockCadence: target_block_time <= 0, "
                      "resetting to 52.0");
        cfg_.target_block_time = 52.0;
    }
    if (cfg_.ema_window_blocks < 2) {
        spdlog::warn("BlockCadence: ema_window_blocks < 2, "
                      "resetting to 10");
        cfg_.ema_window_blocks = 10;
    }
    if (cfg_.eta < 0.0) {
        spdlog::warn("BlockCadence: eta < 0, resetting to 0.5");
        cfg_.eta = 0.5;
    }
}

QuoteResult BlockCadenceAdaptiveSpread::compute_quotes(
    double mid, double sigma, double q, BlockHeight block_height)
{
    // -----------------------------------------------------------------
    // Step 1: Compute tau in years.
    //
    // sigma is already annualized; tau is in years from rolling_tau_years().
    // sigma_annual^2 * tau_years = variance in years (dimensionally correct).
    // ISO/IEC 5055: correct dimensional analysis -- no per-block conversion
    // that would produce sigma_block^2 * tau_years ~6 orders too small.
    // -----------------------------------------------------------------
    const double tau = rolling_tau_years(
        block_height, cfg_.horizon_blocks,
        cfg_.target_block_time, kSecondsPerYear);

    // -----------------------------------------------------------------
    // Step 2: Compute A-S reservation price and half-spread.
    //
    // Use the ADJUSTED kappa that accounts for current block cadence.
    // When blocks arrive faster than target, fill intensity per block
    // decreases (less time for takers), so kappa scales up.
    // -----------------------------------------------------------------
    const double kappa_adj = adjusted_kappa();

    const double r = as_reservation_price(mid, q, cfg_.gamma,
                                          sigma, tau);

    // Half-spread uses the cadence-adjusted kappa, not the base kappa.
    const double delta = (1.0 / kappa_adj)
                         * std::log(1.0 + kappa_adj / cfg_.gamma)
                         + 0.5 * cfg_.gamma * sigma * sigma * tau;

    // -----------------------------------------------------------------
    // Step 3: Apply regime multiplier.
    // -----------------------------------------------------------------
    const double delta_regime = delta * regime_.spread_mult;

    // -----------------------------------------------------------------
    // Step 4: Apply the cadence spread multiplier.
    //
    //   m_cadence = 1 + eta * (R - 1)^2
    //   where R = dt_ema / dt_target.
    //
    // U-shaped function: widest at extreme cadences (fast or slow),
    // baseline at R=1 (normal cadence).
    // -----------------------------------------------------------------
    const double m_cadence    = cadence_spread_multiplier();
    const double delta_final  = delta_regime * m_cadence;

    double ask = r + delta_final;
    double bid = r - delta_final;

    // -----------------------------------------------------------------
    // Step 5: Apply the never-sell-at-loss constraint.
    // -----------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint) {
        ask = apply_no_loss_floor(ask, cost_basis_, min_margin_bps_);
    }

    // -----------------------------------------------------------------
    // Step 6: Enforce the 40 bps minimum spread floor.
    // -----------------------------------------------------------------
    enforce_spread_floor(bid, ask, mid);

    // -----------------------------------------------------------------
    // Step 7: Compute sizes.
    // -----------------------------------------------------------------
    const double q_norm   = (cfg_.q_max > 0.0) ? (q / cfg_.q_max) : 0.0;
    const double bid_size = cfg_.q_max * std::max(0.1, 1.0 - q_norm);
    const double ask_size = cfg_.q_max * std::max(0.1, 1.0 + q_norm);

    const double mid_price = (ask + bid) * 0.5;
    const double spread_bps = (mid_price > 0.0)
        ? 10000.0 * (ask - bid) / mid_price
        : 0.0;

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

void BlockCadenceAdaptiveSpread::update_price(double mid,
                                               BlockHeight block_height)
{
    // Store observation and trim to regime detection window.
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
    const auto max_history =
        static_cast<std::size_t>(cfg_.ema_window_blocks * 2);
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
    const auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            curr.arrival - prev.arrival);
    const double dt_seconds =
        static_cast<double>(duration.count()) / 1000.0;

    // Sanity check: reject non-positive intervals (clock skew, duplicates).
    if (dt_seconds <= 0.0) {
        spdlog::warn("BlockCadence: non-positive dt={:.3f}s between "
                      "blocks {} and {}, skipping",
                      dt_seconds, prev.block, curr.block);
        return;
    }

    // Update the EMA: alpha = 2 / (N + 1) is the standard smoothing factor.
    const double alpha = 2.0
        / (static_cast<double>(cfg_.ema_window_blocks) + 1.0);
    dt_ema_ = alpha * dt_seconds + (1.0 - alpha) * dt_ema_;

    spdlog::trace("BlockCadence: dt={:.1f}s dt_ema={:.1f}s R={:.3f}",
                   dt_seconds, dt_ema_, cadence_ratio());
}

double BlockCadenceAdaptiveSpread::current_dt_ema() const
{
    return dt_ema_;
}

double BlockCadenceAdaptiveSpread::cadence_ratio() const
{
    // R = dt_ema / target_block_time.
    // Guard against division by zero (should never happen with validated config).
    if (cfg_.target_block_time <= 0.0) {
        return 1.0;
    }
    return dt_ema_ / cfg_.target_block_time;
}

double BlockCadenceAdaptiveSpread::cadence_spread_multiplier() const
{
    // m_cadence = 1 + eta * (R - 1)^2, clamped to [mult_min, mult_max].
    //
    // This is a U-shaped function centered at R=1:
    //   R = 1.0 (normal):  m = 1.0      (no adjustment)
    //   R = 0.5 (fast):    m = 1 + 0.25*eta
    //   R = 2.0 (slow):    m = 1 + eta
    const double R         = cadence_ratio();
    const double deviation = R - 1.0;
    const double raw_mult  = 1.0 + cfg_.eta * deviation * deviation;

    return std::clamp(raw_mult, cfg_.mult_min, cfg_.mult_max);
}

double BlockCadenceAdaptiveSpread::adjusted_kappa() const
{
    // kappa_adjusted = kappa * (target / dt_ema)
    //
    // Rationale: fill intensity scales inversely with block time.
    // Slow blocks (dt_ema > target) => more time for takers => higher
    // effective fill rate => kappa decreases.
    // Fast blocks (dt_ema < target) => less time => kappa increases.
    //
    // Clamped to [kappa_adj_min * kappa, kappa_adj_max * kappa].
    if (dt_ema_ <= 0.0) {
        return cfg_.kappa;
    }

    const double ratio    = cfg_.target_block_time / dt_ema_;
    const double kappa_adj = cfg_.kappa * ratio;

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
        0.80, 0.50,  // mean-reverting: tighten spread, halve skew
        1.50, 2.00   // momentum: widen spread, double skew
    );
    spdlog::trace("BlockCadence regime: VR={:.4f} regime={}",
                   vr, to_string(regime_.regime));
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
    // Validate parameters with safe fallbacks.
    if (cfg_.psi < 0.0) {
        spdlog::warn("MempoolSentinel: psi < 0, resetting to 1.5");
        cfg_.psi = 1.5;
    }
    if (cfg_.avg_daily_volume <= 0.0) {
        spdlog::warn("MempoolSentinel: avg_daily_volume <= 0, "
                      "resetting to 750.0");
        cfg_.avg_daily_volume = 750.0;
    }
}

QuoteResult MempoolSentinelStrategy::compute_quotes(
    double mid, double sigma, double q, BlockHeight block_height)
{
    // -----------------------------------------------------------------
    // Step 1: Compute tau in years.
    //
    // sigma is already annualized; tau is in years from rolling_tau_years().
    // sigma_annual^2 * tau_years = variance in years (dimensionally correct).
    // ISO/IEC 5055: correct dimensional analysis -- no per-block conversion
    // that would produce sigma_block^2 * tau_years ~6 orders too small.
    // -----------------------------------------------------------------
    const double tau = rolling_tau_years(
        block_height, cfg_.horizon_blocks,
        cfg_.block_time_seconds, kSecondsPerYear);

    // -----------------------------------------------------------------
    // Step 2: Compute A-S reservation price and half-spread.
    // -----------------------------------------------------------------
    const double r     = as_reservation_price(mid, q, cfg_.gamma,
                                              sigma, tau);
    const double delta = as_half_spread(cfg_.gamma, cfg_.kappa,
                                        sigma, tau);

    // -----------------------------------------------------------------
    // Step 3: Apply regime multiplier.
    // -----------------------------------------------------------------
    const double delta_regime = delta * regime_.spread_mult;

    // -----------------------------------------------------------------
    // Step 4: Apply mempool spread multiplier.
    //
    //   m_mempool = 1 + psi * I_pending
    //   where I_pending = |F_pending| / avg_daily_volume.
    //
    // Widens spreads when significant pending flow is detected,
    // providing anticipatory adverse-selection protection.
    // -----------------------------------------------------------------
    const double m_mempool    = mempool_spread_multiplier();
    const double delta_final  = delta_regime * m_mempool;

    // -----------------------------------------------------------------
    // Step 5: Apply mempool skew adjustment.
    //
    //   skew = -phi_mempool * sign(F_pending) * I_pending
    //
    // Net buying in mempool (F > 0) => shift quotes UP (positive skew):
    //   ask widens  (less eager to sell into buying pressure)
    //   bid tightens (more eager to buy, anticipating price rise)
    //
    // Net selling in mempool (F < 0) => shift quotes DOWN (negative skew):
    //   ask tightens (more eager to sell, anticipating price fall)
    //   bid widens   (less eager to buy into selling pressure)
    // -----------------------------------------------------------------
    const double skew_adj = mempool_skew_adjustment();

    double ask = r + delta_final + skew_adj;
    double bid = r - delta_final + skew_adj;

    // -----------------------------------------------------------------
    // Step 6: Apply the never-sell-at-loss constraint.
    // -----------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint) {
        ask = apply_no_loss_floor(ask, cost_basis_, min_margin_bps_);
    }

    // -----------------------------------------------------------------
    // Step 7: Enforce the 40 bps minimum spread floor.
    // -----------------------------------------------------------------
    enforce_spread_floor(bid, ask, mid);

    // -----------------------------------------------------------------
    // Step 8: Compute sizes.
    //
    // When our own offers are detected as pending fill in the mempool,
    // reduce size on the filled side to limit additional exposure until
    // the fill confirms and we can fully recompute.
    // -----------------------------------------------------------------
    const double q_norm = (cfg_.q_max > 0.0) ? (q / cfg_.q_max) : 0.0;
    double bid_size = cfg_.q_max * std::max(0.1, 1.0 - q_norm);
    double ask_size = cfg_.q_max * std::max(0.1, 1.0 + q_norm);

    // Preemptive size reduction for imminent fills of our offers.
    if (cfg_.enable_preemptive_cancel) {
        for (const auto& take : pending_takes_) {
            if (take.is_our_offer) {
                if (take.taker_side == Side::Bid) {
                    // Taker is buying (taking our ask).  Reduce our
                    // remaining ask size by 50% as a precaution.
                    ask_size *= 0.5;
                    spdlog::info("MempoolSentinel: our ask offer {} "
                                  "being taken, reducing ask size",
                                  take.offer_id);
                } else {
                    // Taker is selling (taking our bid).  Reduce our
                    // remaining bid size by 50% as a precaution.
                    bid_size *= 0.5;
                    spdlog::info("MempoolSentinel: our bid offer {} "
                                  "being taken, reducing bid size",
                                  take.offer_id);
                }
            }
        }
    }

    const double mid_price = (ask + bid) * 0.5;
    const double spread_bps = (mid_price > 0.0)
        ? 10000.0 * (ask - bid) / mid_price
        : 0.0;

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

void MempoolSentinelStrategy::update_price(double mid,
                                            BlockHeight block_height)
{
    // Store observation and trim to regime detection window.
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
    // Items that have been pending too long are likely stuck or invalid
    // and should not influence quoting decisions.
    const auto now = std::chrono::system_clock::now();
    const auto max_age =
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds(
                static_cast<int64_t>(
                    cfg_.max_mempool_age_seconds * 1000.0)));

    pending_takes_.clear();
    pending_takes_.reserve(takes.size());

    for (const auto& take : takes) {
        const auto age = now - take.first_seen;
        if (age <= max_age) {
            pending_takes_.push_back(take);
        }
    }

    spdlog::debug("MempoolSentinel: accepted {}/{} takes "
                   "(stale filter={:.0f}s)",
                   pending_takes_.size(), takes.size(),
                   cfg_.max_mempool_age_seconds);
}

double MempoolSentinelStrategy::pending_net_flow() const
{
    // F_pending = SUM(size * sign(side))
    // where Bid (taker buying base) = +1, Ask (taker selling base) = -1.
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
    // Returns 0.0 if avg_daily_volume is non-positive (defensive).
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
    // skew = phi_mempool * sign(F_pending) * I_pending
    // Clamped to [-skew_max, +skew_max].
    //
    // Positive sign ensures correct directional response:
    //   F > 0 (buying pressure) => positive skew => quotes shift UP
    //     (ask widens / less eager to sell, bid tightens / more eager to buy)
    //   F < 0 (selling pressure) => negative skew => quotes shift DOWN
    //     (ask tightens / more eager to sell, bid widens / less eager to buy)
    const double F = pending_net_flow();
    if (std::abs(F) < 1e-12) {
        return 0.0;  // No pending flow; no skew adjustment.
    }

    const double I      = pending_flow_intensity();
    const double sign_F = (F > 0.0) ? 1.0 : -1.0;
    const double raw_skew = cfg_.phi_mempool * sign_F * I;

    return std::clamp(raw_skew, -cfg_.skew_max, cfg_.skew_max);
}

std::vector<std::string>
MempoolSentinelStrategy::our_offers_pending_fill() const
{
    // Filter the pending takes to find those targeting our own offers.
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
        0.80, 0.50,  // mean-reverting: tighten spread, halve skew
        1.50, 2.00   // momentum: widen spread, double skew
    );
    spdlog::trace("Mempool regime: VR={:.4f} regime={}",
                   vr, to_string(regime_.regime));
}

}  // namespace xop
