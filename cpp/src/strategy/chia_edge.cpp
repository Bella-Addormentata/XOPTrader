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
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

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
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

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
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

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
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

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
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

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
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

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
    // T2-02: Shared lock -- read-only access to cfg_.
    // NOTE: Individual multiplier computations are inlined here to avoid
    // re-entrant shared lock acquisition (std::shared_mutex is non-recursive
    // on Windows SRWLOCK).
    std::shared_lock lock(mtx_);

    // Composite: multiplicative combination of all five edge factors.
    //
    //   m_composite = m_atomic * m_cancel * m_utxo * m_block_time * m_mempool
    //
    // Each factor is in (0, 1], so the composite is also in (0, 1].
    // A composite of 0.73 means we can quote ~27% tighter than an equivalent
    // strategy on a chain without these structural advantages.

    // Edge 1: Atomic offers.
    const double m_atomic = std::clamp(
        1.0 - cfg_.atomic_tightening_bps / cfg_.reference_spread_bps,
        cfg_.atomic_mult_floor, 1.0);

    // Edge 2: Free cancellation.
    const double m_cancel = std::clamp(
        1.0 - cfg_.cancel_savings_bps / cfg_.reference_spread_bps,
        cfg_.cancel_mult_floor, 1.0);

    // Edge 3: UTXO parallel offers.
    const double tiers = static_cast<double>(std::max(cfg_.active_tiers, 1u));
    const double fill_boost = 1.0 + cfg_.utxo_fill_bonus_pct * (tiers - 1.0);
    const double m_utxo = std::clamp(
        (fill_boost > 1e-9) ? (1.0 / fill_boost) : 1.0,
        cfg_.utxo_mult_floor, 1.0);

    // Edge 4: Block time.
    const double m_block_time = std::clamp(
        1.0 - cfg_.latency_savings_bps / cfg_.reference_spread_bps,
        cfg_.block_time_mult_floor, 1.0);

    // Edge 5: Mempool info.
    const double info_ratio = (cfg_.block_time_seconds > 1e-9)
        ? cfg_.mempool_window_seconds / cfg_.block_time_seconds
        : 0.0;
    const double m_mempool = std::clamp(
        1.0 - cfg_.mempool_info_bps * info_ratio / cfg_.reference_spread_bps,
        cfg_.mempool_mult_floor, 1.0);

    return m_atomic * m_cancel * m_utxo * m_block_time * m_mempool;
}

// ===========================================================================
// Core interface -- compute_quotes
// ===========================================================================

QuoteResult ChiaEdgeOptimizer::compute_quotes(double mid,
                                              double sigma,
                                              double q,
                                              BlockHeight block_height)
{
    // T2-02: Shared lock -- compute_quotes reads cfg_, regime_, cost_basis_,
    // min_margin_bps_ but does not mutate member state.  All sub-computations
    // (compute_tau, reservation_price, optimal_half_spread,
    // composite_edge_multiplier) are inlined to avoid re-entrant lock
    // acquisition (std::shared_mutex is non-recursive on Windows SRWLOCK).
    std::shared_lock lock(mtx_);

    // -------------------------------------------------------------------
    // Step 1: Compute remaining time in the rolling horizon (seconds).
    //
    //   tau = (N - n) * block_time
    //
    // where N = horizon_blocks and n = block_height mod N.  The modular
    // rollover prevents tau from reaching zero.
    // -------------------------------------------------------------------
    // Inlined from compute_tau():
    const uint32_t n_mod = block_height % cfg_.horizon_blocks;
    const uint32_t remaining_blocks = cfg_.horizon_blocks - n_mod;
    const double tau = static_cast<double>(remaining_blocks) * cfg_.block_time_seconds;

    // -------------------------------------------------------------------
    // Step 2: Compute the Avellaneda-Stoikov reservation price.
    //
    //   r = S - q * gamma * sigma^2 * tau
    //
    // The reservation price is the mid-price adjusted for the cost of
    // carrying inventory q over the remaining horizon at volatility sigma.
    // -------------------------------------------------------------------
    // Inlined from reservation_price():
    const double r_raw = mid - q * cfg_.gamma * sigma * sigma * tau;

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
    // Inlined from optimal_half_spread():
    const double term1 = (1.0 / cfg_.kappa)
                       * std::log(1.0 + cfg_.kappa / cfg_.gamma);
    const double term2 = 0.5 * cfg_.gamma * sigma * sigma * tau;
    double delta = term1 + term2;

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
    // Inlined from composite_edge_multiplier():
    const double m_atomic = std::clamp(
        1.0 - cfg_.atomic_tightening_bps / cfg_.reference_spread_bps,
        cfg_.atomic_mult_floor, 1.0);
    const double m_cancel = std::clamp(
        1.0 - cfg_.cancel_savings_bps / cfg_.reference_spread_bps,
        cfg_.cancel_mult_floor, 1.0);
    const double utxo_tiers = static_cast<double>(std::max(cfg_.active_tiers, 1u));
    const double utxo_boost = 1.0 + cfg_.utxo_fill_bonus_pct * (utxo_tiers - 1.0);
    const double m_utxo = std::clamp(
        (utxo_boost > 1e-9) ? (1.0 / utxo_boost) : 1.0,
        cfg_.utxo_mult_floor, 1.0);
    const double m_block_time = std::clamp(
        1.0 - cfg_.latency_savings_bps / cfg_.reference_spread_bps,
        cfg_.block_time_mult_floor, 1.0);
    const double mp_info_ratio = (cfg_.block_time_seconds > 1e-9)
        ? cfg_.mempool_window_seconds / cfg_.block_time_seconds
        : 0.0;
    const double m_mempool = std::clamp(
        1.0 - cfg_.mempool_info_bps * mp_info_ratio / cfg_.reference_spread_bps,
        cfg_.mempool_mult_floor, 1.0);
    const double edge_mult = m_atomic * m_cancel * m_utxo * m_block_time * m_mempool;
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
    // T2-02: Exclusive lock -- update_price mutates price_buffer_, regime_,
    // last_mid_, and feeds data to the active RegimeDetector.
    std::unique_lock lock(mtx_);

    // Append the new observation to the rolling price buffer (retained for
    // backward compatibility and potential diagnostic use).
    price_buffer_.push_back(PriceObs{block_height, mid});

    // Trim to the regime detection window size.
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

RegimeInfo ChiaEdgeOptimizer::current_regime() const
{
    // T2-02: Shared lock -- read-only access to regime_.
    std::shared_lock lock(mtx_);
    return regime_;
}

std::string ChiaEdgeOptimizer::name() const
{
    // T2-02: Shared lock -- read-only access to name_.
    std::shared_lock lock(mtx_);
    return name_;
}

void ChiaEdgeOptimizer::set_cost_basis(double cost_basis, double min_margin_bps)
{
    // T2-02: Exclusive lock -- set_cost_basis mutates cost_basis_ and
    // min_margin_bps_.
    std::unique_lock lock(mtx_);
    cost_basis_     = cost_basis;
    min_margin_bps_ = min_margin_bps;
}

// ===========================================================================
// A-S model helper computations
// ===========================================================================

double ChiaEdgeOptimizer::reservation_price(double mid, double sigma,
                                            double q, double tau) const
{
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

    // r = S - q * gamma * sigma^2 * tau
    //
    // The inventory penalty q * gamma * sigma^2 * tau represents the
    // expected cost of carrying inventory q over the remaining horizon
    // tau at volatility sigma, scaled by risk aversion gamma.
    return mid - q * cfg_.gamma * sigma * sigma * tau;
}

double ChiaEdgeOptimizer::optimal_half_spread(double sigma, double tau) const
{
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

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
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

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
// Regime detection (T3-01: delegated to shared canonical RegimeDetector)
// ===========================================================================

void ChiaEdgeOptimizer::update_regime()
{
    // T3-01: delegate regime classification to the active RegimeDetector
    // (shared if injected via set_regime_detector(), else the internal
    // detector created in the constructor).  The canonical detector provides
    // dual-horizon VR, Z-statistic significance testing (Lo-MacKinlay 1988),
    // hysteresis to prevent regime whipsawing, and optional HMM.
    regime_ = to_regime_info(active_detector());
}

}  // namespace xop
