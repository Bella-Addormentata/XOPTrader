// chia_edge.cpp -- ChiaEdgeOptimizer implementation: Avellaneda-Stoikov base
//                  model augmented with five CHIA-specific structural edge
//                  multipliers for tighter, safer quoting.
//
// See chia_edge.hpp for the full mathematical derivation, scholarly references,
// and detailed documentation of each edge factor.
//
// ISO/IEC 27001:2022 -- no secrets handled.
// ISO/IEC 5055       -- no unchecked arithmetic; all edge cases guarded.
// ISO/IEC 25000      -- comprehensive inline documentation.
// ISO/IEC JTC 1/SC 22 -- standard-conforming C++20.

#include <xop/strategy/chia_edge.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <numeric>

namespace xop {

// ===========================================================================
// Construction
// ===========================================================================

ChiaEdgeOptimizer::ChiaEdgeOptimizer(const ChiaEdgeConfig& cfg)
    : cfg_(cfg)
    , min_margin_bps_(cfg.min_margin_bps)
{
    // Validate critical parameters.  Config values may come from user files or
    // command-line flags, so throw on invalid input rather than assert (which
    // is stripped in Release builds).
    // ISO/IEC 5055: fail-fast on invalid configuration.
    if (!(cfg_.gamma > 0.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: gamma must be strictly positive");
    }
    if (!(cfg_.kappa > 0.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: kappa must be strictly positive");
    }
    if (!(cfg_.q_max > 0.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: q_max must be strictly positive");
    }
    if (!(cfg_.horizon_blocks > 0)) {
        throw std::invalid_argument("ChiaEdgeConfig: horizon_blocks must be at least 1");
    }
    if (!(cfg_.block_time_seconds > 0.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: block_time_seconds must be positive");
    }
    if (!(cfg_.reference_spread_bps > 0.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: reference_spread_bps must be positive");
    }
    if (!(cfg_.spread_floor_bps >= 0.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: spread_floor_bps must be non-negative");
    }

    // Validate edge factor floor bounds: each must be in (0.0, 1.0].
    if (!(cfg_.atomic_mult_floor > 0.0 && cfg_.atomic_mult_floor <= 1.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: atomic_mult_floor must be in (0, 1]");
    }
    if (!(cfg_.cancel_mult_floor > 0.0 && cfg_.cancel_mult_floor <= 1.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: cancel_mult_floor must be in (0, 1]");
    }
    if (!(cfg_.utxo_mult_floor > 0.0 && cfg_.utxo_mult_floor <= 1.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: utxo_mult_floor must be in (0, 1]");
    }
    if (!(cfg_.block_time_mult_floor > 0.0 && cfg_.block_time_mult_floor <= 1.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: block_time_mult_floor must be in (0, 1]");
    }
    if (!(cfg_.mempool_mult_floor > 0.0 && cfg_.mempool_mult_floor <= 1.0)) {
        throw std::invalid_argument("ChiaEdgeConfig: mempool_mult_floor must be in (0, 1]");
    }

    // Initialize regime to sane defaults so the first compute_quotes() call
    // before any update_price() produces reasonable spreads (not zero).
    // ISO/IEC 5055: defensive initialization of derived state.
    regime_ = RegimeInfo{MarketRegime::Random, 1.0, 1.0, 1.0};

    spdlog::info("[ChiaEdgeOptimizer] Initialised with composite edge = {:.4f} "
                 "(atomic={:.3f}, cancel={:.3f}, utxo={:.3f}, "
                 "block_time={:.3f}, mempool={:.3f})",
                 composite_edge_multiplier(),
                 atomic_offer_multiplier(),
                 free_cancel_multiplier(),
                 utxo_parallel_multiplier(),
                 block_time_multiplier(),
                 mempool_info_multiplier());
}

// ===========================================================================
// Individual edge multiplier accessors
// ===========================================================================

double ChiaEdgeOptimizer::atomic_offer_multiplier() const
{
    // Edge 1: Atomic Offers (no partial fills).
    //
    //   m_atomic = 1.0 - atomic_tightening_bps / reference_spread_bps
    //
    // Atomic execution eliminates partial-fill adverse selection.  An informed
    // counterparty cannot cherry-pick a profitable fraction of our resting
    // offer; they must take all or nothing.  This reduces the Glosten-Milgrom
    // adverse-selection component of the spread by atomic_tightening_bps.
    //
    // Clamped to [atomic_mult_floor, 1.0] to prevent over-tightening when the
    // base spread is narrow.
    const double raw = 1.0 - cfg_.atomic_tightening_bps / cfg_.reference_spread_bps;
    return std::clamp(raw, cfg_.atomic_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::free_cancel_multiplier() const
{
    // Edge 2: Free Cancellation (zero gas cost to cancel/refresh).
    //
    //   m_cancel = 1.0 - cancel_savings_bps / reference_spread_bps
    //
    // On Ethereum, cancelling a resting order costs gas ($0.50-$50+), forcing
    // makers to leave stale quotes longer than optimal.  On CHIA, cancellation
    // is free (the offer is a local file; spending its backing coins invalidates
    // it).  This allows aggressive per-block refresh with zero downside, saving
    // cancel_savings_bps of effective spread.
    //
    // Clamped to [cancel_mult_floor, 1.0].
    const double raw = 1.0 - cfg_.cancel_savings_bps / cfg_.reference_spread_bps;
    return std::clamp(raw, cfg_.cancel_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::utxo_parallel_multiplier() const
{
    // Edge 3: Coin-Set (UTXO) Parallel Offers.
    //
    //   fill_boost = 1.0 + utxo_fill_bonus_pct * (active_tiers - 1)
    //   m_utxo     = 1.0 / fill_boost
    //
    // Pre-split coins enable N simultaneously valid, non-competing offers at
    // different price levels.  Each additional tier increases fill probability
    // by utxo_fill_bonus_pct.  Higher fill probability justifies tighter
    // spreads: the maker earns the spread more frequently, so each individual
    // spread can be narrower while maintaining the same expected revenue.
    //
    // Clamped to [utxo_mult_floor, 1.0].
    const double tiers = static_cast<double>(std::max(cfg_.active_tiers, 1u));
    const double fill_boost = 1.0 + cfg_.utxo_fill_bonus_pct * (tiers - 1.0);

    // Guard against fill_boost near zero (should not happen with valid config).
    const double raw = (fill_boost > 1e-9) ? (1.0 / fill_boost) : 1.0;
    return std::clamp(raw, cfg_.utxo_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::block_time_multiplier() const
{
    // Edge 4: 52-Second Block Time (latency advantage neutralisation).
    //
    //   m_block_time = 1.0 - latency_savings_bps / reference_spread_bps
    //
    // CHIA's 52-second block time functions as a natural batch auction
    // (Budish, Cramton & Shim 2015).  Sub-second latency confers zero
    // advantage.  Our computational sophistication (A-S model, VR test, VPIN)
    // is NOT competed away by faster hardware, unlike on Ethereum or Solana.
    // This eliminates the HFT latency-based adverse-selection component of
    // the spread.
    //
    // Clamped to [block_time_mult_floor, 1.0].
    const double raw = 1.0 - cfg_.latency_savings_bps / cfg_.reference_spread_bps;
    return std::clamp(raw, cfg_.block_time_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::mempool_info_multiplier() const
{
    // Edge 5: Transparent Mempool (~40 seconds advance information).
    //
    //   info_ratio = mempool_window_seconds / block_time_seconds
    //   m_mempool  = 1.0 - mempool_info_bps * info_ratio / reference_spread_bps
    //
    // The CHIA full-node RPC exposes all pending transactions ~40 seconds
    // before block settlement.  This provides advance knowledge of trade
    // direction, size, and whether our own offers are being taken.  The
    // information ratio (mempool window / block time) quantifies the fraction
    // of the inter-block period during which we have advance knowledge.
    //
    // Higher info_ratio => more time to react => stronger edge.
    // At default values: info_ratio = 40/52 ~ 0.77.
    //
    // Clamped to [mempool_mult_floor, 1.0].
    const double info_ratio = (cfg_.block_time_seconds > 1e-9)
        ? cfg_.mempool_window_seconds / cfg_.block_time_seconds
        : 0.0;
    const double raw = 1.0 - cfg_.mempool_info_bps * info_ratio
                            / cfg_.reference_spread_bps;
    return std::clamp(raw, cfg_.mempool_mult_floor, 1.0);
}

double ChiaEdgeOptimizer::composite_edge_multiplier() const
{
    // Composite: multiplicative combination of all five edge factors.
    //
    //   m_composite = m_atomic * m_cancel * m_utxo * m_block_time * m_mempool
    //
    // Each factor is in (0, 1], so the composite is also in (0, 1].
    // A composite of 0.73 means we can quote ~27% tighter than an equivalent
    // strategy on a chain without these structural advantages.
    return atomic_offer_multiplier()
         * free_cancel_multiplier()
         * utxo_parallel_multiplier()
         * block_time_multiplier()
         * mempool_info_multiplier();
}

// ===========================================================================
// Core interface -- compute_quotes
// ===========================================================================

QuoteResult ChiaEdgeOptimizer::compute_quotes(double mid,
                                              double sigma,
                                              double q,
                                              BlockHeight block_height)
{
    // -------------------------------------------------------------------
    // Step 1: Compute remaining time in the rolling horizon (seconds).
    //
    //   tau = (N - n) * block_time
    //
    // where N = horizon_blocks and n = block_height mod N.  The modular
    // rollover prevents tau from reaching zero.
    // -------------------------------------------------------------------
    const double tau = compute_tau(block_height);

    // -------------------------------------------------------------------
    // Step 2: Compute the Avellaneda-Stoikov reservation price.
    //
    //   r = S - q * gamma * sigma^2 * tau
    //
    // The reservation price is the mid-price adjusted for the cost of
    // carrying inventory q over the remaining horizon at volatility sigma.
    // -------------------------------------------------------------------
    const double r_raw = reservation_price(mid, sigma, q, tau);

    // -------------------------------------------------------------------
    // Step 3: Apply regime-dependent skew multiplier to the inventory
    //         component of the reservation price.
    //
    //   r_adjusted = S + skew_mult * (r_raw - S)
    //
    // Mean-reverting: reduce skew (0.5x) -- reversion will help.
    // Momentum: amplify skew (2.0x) -- shed inventory with the trend.
    // Random: no change (1.0x).
    // -------------------------------------------------------------------
    const double r = mid + regime_.skew_mult * (r_raw - mid);

    // -------------------------------------------------------------------
    // Step 4: Compute the A-S optimal symmetric half-spread (base model).
    //
    //   delta_base = (1/kappa) * ln(1 + kappa/gamma)
    //              + 0.5 * gamma * sigma^2 * tau
    //
    // This is the standard Avellaneda-Stoikov half-spread BEFORE any
    // CHIA-specific or regime adjustments.
    // -------------------------------------------------------------------
    double delta = optimal_half_spread(sigma, tau);

    // -------------------------------------------------------------------
    // Step 5: Apply regime-dependent spread multiplier.
    //
    // Mean-reverting (0.8x): tighten -- lower adverse selection risk.
    // Momentum (1.5x): widen -- higher adverse selection risk.
    // Random (1.0x): no change.
    // -------------------------------------------------------------------
    delta *= regime_.spread_mult;

    // -------------------------------------------------------------------
    // Step 6: Apply the composite CHIA-edge multiplier.
    //
    //   delta_chia = delta * m_composite
    //
    // where m_composite = product of all five structural edge factors.
    // Each factor is in (0, 1], so this can only tighten the spread.
    //
    // The edge reflects structural cost savings and informational
    // advantages unique to the CHIA DEX environment:
    //   1. Atomic offers: no partial-fill adverse selection.
    //   2. Free cancellation: zero-cost per-block refresh.
    //   3. UTXO parallel offers: higher fill probability.
    //   4. 52s block time: no HFT latency arms race.
    //   5. Transparent mempool: ~40s advance information.
    // -------------------------------------------------------------------
    const double edge_mult = composite_edge_multiplier();
    delta *= edge_mult;

    // -------------------------------------------------------------------
    // Step 7: Enforce the spread floor.
    //
    // The minimum spread (spread_floor_bps) ensures profitability net of
    // operational costs, estimation error, and the risk of being wrong
    // about one or more edge factors.  No combination of multipliers may
    // push the full spread below this floor.
    //
    //   delta_min = mid * spread_floor_bps / (2 * 10000)
    //
    // The factor of 2 converts full-spread bps to half-spread.
    // -------------------------------------------------------------------
    if (mid > 0.0) {
        const double delta_min = mid * cfg_.spread_floor_bps / 20000.0;
        delta = std::max(delta, delta_min);
    }

    // -------------------------------------------------------------------
    // Step 8: Compute raw bid and ask.
    //
    //   bid = r - delta
    //   ask = r + delta
    //
    // The reservation price r is already skewed by inventory, so the
    // final quotes are asymmetric when q != 0.
    // -------------------------------------------------------------------
    double bid = r - delta;
    double ask = r + delta;

    // -------------------------------------------------------------------
    // Step 9: Apply the never-sell-at-loss constraint (optional).
    //
    //   ask = max(ask, cost_basis * (1 + min_margin_bps / 10000))
    //
    // When enabled, the ask is floored at cost basis plus minimum margin.
    // Underwater inventory is held rather than liquidated at a loss.
    // The bid side is unaffected (we always want to buy cheap).
    // -------------------------------------------------------------------
    if (cfg_.enable_no_loss_constraint && cost_basis_ > 0.0) {
        const double min_ask = cost_basis_ * (1.0 + min_margin_bps_ / 10000.0);
        if (ask < min_ask) {
            spdlog::debug("[ChiaEdgeOptimizer] Cost-basis floor activated: "
                          "ask {:.6f} -> {:.6f} (cost_basis={:.6f}, "
                          "margin={:.1f} bps)",
                          ask, min_ask, cost_basis_, min_margin_bps_);
            ask = min_ask;
        }
    }

    // -------------------------------------------------------------------
    // Step 10: Floor prices at zero (defensive -- should never trigger
    //          with sane parameters).
    // -------------------------------------------------------------------
    bid = std::max(bid, 0.0);
    ask = std::max(ask, bid + 1e-12);  // Ask must always exceed bid.

    // -------------------------------------------------------------------
    // Step 11: Compute sizes.
    //
    // Size scaling is proportional to remaining inventory capacity:
    //   bid_size = q_max * max(0, 1 - q / q_max)
    //   ask_size = q_max * max(0, 1 + q / q_max)
    //
    // When q = +q_max: bid_size=0 (stop buying), ask_size=2*q_max (sell).
    // When q = -q_max: bid_size=2*q_max (buy), ask_size=0 (stop selling).
    // -------------------------------------------------------------------
    const double q_ratio = q / cfg_.q_max;
    const double bid_size = cfg_.q_max * std::max(0.0, 1.0 - q_ratio);
    const double ask_size = cfg_.q_max * std::max(0.0, 1.0 + q_ratio);

    // -------------------------------------------------------------------
    // Step 12: Compute spread in basis points for reporting.
    //
    //   spread_bps = 10000 * (ask - bid) / mid
    // -------------------------------------------------------------------
    const double spread_bps = (mid > 0.0)
        ? 10000.0 * (ask - bid) / mid
        : 0.0;

    // Log at debug level for diagnostics.
    spdlog::debug("[ChiaEdgeOptimizer] block={} mid={:.6f} sigma={:.6f} q={:.2f} "
                  "tau={:.1f}s r={:.6f} delta={:.6f} edge_mult={:.4f} "
                  "spread={:.1f}bps regime={}",
                  block_height, mid, sigma, q, tau, r, delta, edge_mult,
                  spread_bps, to_string(regime_.regime));

    return QuoteResult{bid, ask, bid_size, ask_size, spread_bps};
}

// ===========================================================================
// Market data feed
// ===========================================================================

void ChiaEdgeOptimizer::update_price(double mid, BlockHeight block_height)
{
    // Append the new observation to the rolling price buffer.
    price_buffer_.push_back(PriceObs{block_height, mid});

    // Trim to the regime detection window size.
    while (price_buffer_.size() > cfg_.regime_window_blocks) {
        price_buffer_.pop_front();
    }

    // Recompute regime classification from updated price history.
    update_regime();
}

// ===========================================================================
// Accessors
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
// A-S model helper computations
// ===========================================================================

double ChiaEdgeOptimizer::reservation_price(double mid, double sigma,
                                            double q, double tau) const
{
    // r = S - q * gamma * sigma^2 * tau
    //
    // The inventory penalty q * gamma * sigma^2 * tau represents the
    // expected cost of carrying inventory q over the remaining horizon
    // tau at volatility sigma, scaled by risk aversion gamma.
    return mid - q * cfg_.gamma * sigma * sigma * tau;
}

double ChiaEdgeOptimizer::optimal_half_spread(double sigma, double tau) const
{
    // delta = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
    //
    // Term 1: Optimal spread from the Poisson fill-intensity model.
    //   Balances fill probability against spread revenue per fill.
    //   Derived by maximising E[spread * fill_probability].
    //
    // Term 2: Inventory-risk compensation.
    //   Holding any position for tau more seconds at volatility sigma
    //   carries variance sigma^2 * tau; the maker demands gamma/2 times
    //   that variance as additional compensation.
    const double term1 = (1.0 / cfg_.kappa)
                       * std::log(1.0 + cfg_.kappa / cfg_.gamma);
    const double term2 = 0.5 * cfg_.gamma * sigma * sigma * tau;
    return term1 + term2;
}

double ChiaEdgeOptimizer::compute_tau(BlockHeight block_height) const
{
    // tau = (N - n) * block_time
    //
    // n = block_height mod N.  The sawtooth pattern gives tau in
    // [1*block_time, N*block_time], never reaching zero.
    const uint32_t n = block_height % cfg_.horizon_blocks;
    const uint32_t remaining = cfg_.horizon_blocks - n;
    return static_cast<double>(remaining) * cfg_.block_time_seconds;
}

/* static */
double ChiaEdgeOptimizer::per_block_volatility(double sigma_annual,
                                               double block_time_seconds)
{
    // sigma_block = sigma_annual * sqrt(block_time / seconds_per_year)
    //
    // For CHIA with block_time = 52 s:
    //   sigma_block = sigma_annual * sqrt(52 / 31,536,000)
    //               = sigma_annual * 0.001284
    return sigma_annual * std::sqrt(block_time_seconds / kSecondsPerYear);
}

// ===========================================================================
// Regime detection -- variance ratio test
// ===========================================================================

double ChiaEdgeOptimizer::variance_ratio_test() const
{
    // -----------------------------------------------------------------------
    // Variance Ratio Test (VR)
    //
    // Tests whether the price process is a random walk.  Under H0 (random
    // walk), the variance of k-period log-returns equals k times the
    // variance of 1-period log-returns:
    //
    //   VR(k) = Var(r_k) / (k * Var(r_1))
    //
    // We use k = 5 (five blocks) and compute overlapping log-returns.
    //
    //   VR ~ 1.0  => random walk (no exploitable pattern).
    //   VR < 0.85 => mean-reverting (prices bounce back -- tighten spreads).
    //   VR > 1.15 => momentum (prices trend -- widen spreads).
    //
    // Reference: Lo, A.W. & MacKinlay, A.C. (1988). "Stock market prices do
    //            not follow random walks."
    // -----------------------------------------------------------------------

    const std::size_t n = price_buffer_.size();

    // Need at least k+2 observations for meaningful k-period returns.
    constexpr std::size_t k = 5;
    if (n < k + 2) {
        return 1.0;  // Insufficient data; assume random walk.
    }

    // Compute 1-period log-returns: r1[i] = ln(P[i+1] / P[i]).
    std::vector<double> r1;
    r1.reserve(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (price_buffer_[i].mid > 0.0 && price_buffer_[i + 1].mid > 0.0) {
            r1.push_back(
                std::log(price_buffer_[i + 1].mid / price_buffer_[i].mid));
        }
    }

    if (r1.size() < k + 1) {
        return 1.0;  // Insufficient valid returns.
    }

    // Compute k-period log-returns (overlapping): rk[i] = sum(r1[i..i+k-1]).
    std::vector<double> rk;
    rk.reserve(r1.size() - k + 1);
    for (std::size_t i = 0; i + k <= r1.size(); ++i) {
        double sum = 0.0;
        for (std::size_t j = 0; j < k; ++j) {
            sum += r1[i + j];
        }
        rk.push_back(sum);
    }

    if (rk.empty()) {
        return 1.0;
    }

    // Variance of 1-period returns (unbiased estimator).
    const double mean_r1 = std::accumulate(r1.begin(), r1.end(), 0.0)
                         / static_cast<double>(r1.size());
    double var_r1 = 0.0;
    for (const double x : r1) {
        const double d = x - mean_r1;
        var_r1 += d * d;
    }
    var_r1 /= static_cast<double>(r1.size() - 1);

    // Variance of k-period returns (unbiased estimator).
    const double mean_rk = std::accumulate(rk.begin(), rk.end(), 0.0)
                         / static_cast<double>(rk.size());
    double var_rk = 0.0;
    for (const double x : rk) {
        const double d = x - mean_rk;
        var_rk += d * d;
    }
    var_rk /= static_cast<double>(rk.size() - 1);

    // Guard against division by zero when prices are flat.
    if (var_r1 < 1e-20) {
        return 1.0;
    }

    // VR = Var(r_k) / (k * Var(r_1)).
    return var_rk / (static_cast<double>(k) * var_r1);
}

void ChiaEdgeOptimizer::update_regime()
{
    const double vr = variance_ratio_test();

    MarketRegime new_regime = MarketRegime::Random;
    double spread_mult = 1.0;
    double skew_mult   = 1.0;

    if (vr < cfg_.vr_mean_revert_threshold) {
        // Mean-reverting regime: prices tend to bounce back.
        //   - Tighten spreads (0.8x): lower adverse selection risk.
        //   - Reduce inventory shedding (0.5x): mean-reversion will
        //     naturally correct inventory imbalance.
        new_regime  = MarketRegime::MeanReverting;
        spread_mult = cfg_.regime_mr_spread_mult;
        skew_mult   = cfg_.regime_mr_skew_mult;
    } else if (vr > cfg_.vr_momentum_threshold) {
        // Momentum regime: prices trend persistently.
        //   - Widen spreads (1.5x): compensate for higher adverse selection.
        //   - Aggressive inventory shedding (2.0x): holding directional
        //     inventory against a trend is costly.
        new_regime  = MarketRegime::Momentum;
        spread_mult = cfg_.regime_mo_spread_mult;
        skew_mult   = cfg_.regime_mo_skew_mult;
    }
    // Else: Random regime, multipliers stay at 1.0.

    regime_ = RegimeInfo{new_regime, vr, spread_mult, skew_mult};
}

}  // namespace xop
