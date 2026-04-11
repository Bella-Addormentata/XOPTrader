// glft.cpp -- GLFT running-inventory-penalty market-making implementation.
//
// See glft.hpp for the full mathematical derivation, skew direction proof,
// and references (Gueant, Lehalle, Fernandez-Tapia 2013).
//
// ISO/IEC 27001:2022 -- no secrets handled.
// ISO/IEC 5055       -- no unchecked arithmetic; all edge cases guarded.
// ISO/IEC 25000      -- comprehensive inline documentation.

#include <xop/strategy/glft.hpp>

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
    // T5-CR3: tau_min must be strictly positive to prevent log(0) in lambda
    // computation and to guarantee tau never collapses to zero.
    // ISO/IEC 5055: fail-fast on invalid configuration.
    if (!(cfg_.tau_min > 0.0)) {
        throw std::invalid_argument("GlftConfig: tau_min must be strictly positive");
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
                "GlftConfig: tau_min must be < horizon_blocks * block_time_seconds");
        }
    }

    // T5-CR8: validate sparse-fill correction parameters.
    // ISO/IEC 5055: fail-fast on invalid configuration.
    if (!(cfg_.expected_dense_fills_per_hour > 0.0)) {
        throw std::invalid_argument(
            "GlftConfig: expected_dense_fills_per_hour must be strictly positive");
    }
    if (!(cfg_.actual_fills_per_hour > 0.0)) {
        throw std::invalid_argument(
            "GlftConfig: actual_fills_per_hour must be strictly positive");
    }
    if (!(cfg_.sparse_correction_cap >= 1.0)) {
        throw std::invalid_argument(
            "GlftConfig: sparse_correction_cap must be >= 1.0");
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
    // NaN/Inf guard at the public API boundary.  If any input is non-finite,
    // return a zero-spread quote that the engine will skip (spread_bps = 0).
    // This prevents NaN from propagating through all downstream arithmetic.
    // ISO/IEC 5055: CWE-754 -- check for exceptional conditions.
    if (!std::isfinite(mid) || !std::isfinite(sigma) || !std::isfinite(q)) {
        spdlog::warn("[GlftStrategy] compute_quotes: non-finite input "
                     "(mid={}, sigma={}, q={}) -- returning zero quote",
                     mid, sigma, q);
        return QuoteResult{0.0, 0.0, 0.0, 0.0, 0.0};
    }

    // MEDIUM-1: Exclusive lock -- compute_quotes reads cost_basis_,
    // min_margin_bps_, regime_, and cfg_ (mutable strategy state).
    // ISO/IEC 27001:2022: protect shared mutable state.
    std::unique_lock lock(mtx_);

    // -------------------------------------------------------------------
    // Step 1: Compute tau via exponential decay for the risk term.
    //
    // T5-CR3: tau decays exponentially from tau_max after each fill:
    //   tau = tau_max * exp(-lambda * blocks_since_last_fill)
    //
    // Even though GLFT removes the terminal penalty, tau is still
    // needed in the half-spread formula to quantify the per-block
    // inventory risk.  The exponential decay replaces the deterministic
    // sawtooth that adversaries could exploit.  See compute_tau().
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
    // -------------------------------------------------------------------
    // Step 2b: Cap half-spread relative to mid price.
    //
    // The A-S formula (1/kappa)*ln(1+kappa/gamma) produces an ABSOLUTE
    // spread that is independent of the price level.  With gamma=0.005,
    // kappa=1.5, term1=3.806 price units — wider than the mid for any
    // pair priced below ~$7.60.  When hs > mid, bid = mid - hs < 0 and
    // is floored to zero, which:
    //   (a) destroys the inventory-skew signal (reservation_mid shifts
    //       far above market mid);
    //   (b) makes spread_bps = 10000*(ask-0)/mid absurdly large;
    //   (c) results in pathological reservation_mid in Step 6.
    //
    // Cap hs to max_half_spread_pct of mid (default 49%) so that bid
    // always stays positive and the inventory skew remains effective.
    // Applied BEFORE the regime multiplier so that regime-dependent
    // spread scaling (e.g., 0.8× in mean-revert) still differentiates.
    // The engine's Step 5 spread cap (max_half_spread_bps) provides the
    // final refinement; this cap prevents the upstream pathology.
    // -------------------------------------------------------------------
    if (mid > 0.0) {
        const double max_hs = mid * cfg_.max_half_spread_pct;
        if (hs > max_hs) {
            hs = max_hs;
        }
    }

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
// Fill tracking (T5-CR3)
// ===========================================================================

void GlftStrategy::record_fill()
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

    // -----------------------------------------------------------------
    // T5-CR8: Sparse-fill correction (Fodra & Pham 2015; Laruelle et al. 2011).
    //
    // In dense electronic markets (e.g., equity CLOB at ~100 fills/hour),
    // GLFT's continuous-time fill intensity accurately captures the rate
    // at which the market maker can rebalance inventory.  On sparse
    // discrete venues such as CHIA (~1 fill/hour/pair), each fill is far
    // more valuable for inventory management.  The optimal strategy is to
    // skew quotes more aggressively per fill to compensate for the lower
    // rebalancing frequency.
    //
    // Correction factor:
    //   sparse_correction = clamp(dense_rate / actual_rate, 1.0, cap)
    //
    // The effective skew coefficient becomes:
    //   effective_phi = phi * sparse_correction
    //
    // Example (default config):
    //   dense_rate = 100, actual_rate = 1  =>  correction = min(100, 10) = 10
    //   effective_phi = 0.5 * 10 = 5.0
    //
    // ISO/IEC 5055: division-by-zero is precluded by construction
    //               (actual_fills_per_hour validated > 0 in constructor).
    // ISO/IEC 25000: derivation and rationale documented inline.
    // -----------------------------------------------------------------

    // Compute the sparse-fill amplification factor, capped to prevent
    // extreme skew in pathological configurations.
    const double raw_correction = cfg_.expected_dense_fills_per_hour
                                / cfg_.actual_fills_per_hour;
    const double sparse_correction = std::min(
        std::max(1.0, raw_correction),
        cfg_.sparse_correction_cap);

    // Apply the amplified skew coefficient.
    const double effective_phi = cfg_.phi * sparse_correction;

    // skew = effective_phi * q / q_max
    //
    // This is a linear function of inventory:
    //   - Centered at zero when flat.
    //   - Magnitude grows proportionally to |q|.
    //   - Sign matches the sign of q.
    //   - At q = q_max, skew = effective_phi (maximum skew).
    //
    // phi controls the base aggressiveness of inventory rebalancing:
    //   phi = 0   => no skew, pure symmetric quotes (ignores inventory).
    //   phi = 0.5 => moderate skew (default).
    //   phi = 2.0 => very aggressive skew (fast inventory turnover).
    //
    // sparse_correction amplifies phi to account for rare fill events:
    //   sparse_correction = 1.0  => dense market, no amplification.
    //   sparse_correction = 10.0 => CHIA-like venue, 10x skew per fill.
    //
    // The units of skew are the same as the price units (quote asset per
    // base asset) because it enters the quote formulas as a price offset.
    return effective_phi * q / cfg_.q_max;
}

double GlftStrategy::compute_tau(BlockHeight block_height) const
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
    // Even though GLFT does not have a terminal penalty, tau is still
    // needed in the half-spread formula to quantify per-block inventory
    // risk.  The exponential decay keyed to fills replaces the
    // deterministic sawtooth cycle that adversaries could exploit.
    //
    // Reference: Stoikov (2018) "The micro-price";
    //            Cartea, Jaimungal & Penalva (2015) S10.3.
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
    //   produces -- the trader should shed inventory more aggressively
    //   per fill because opportunities are rarer.
    //   Also: Laruelle, Lehalle & Pages (2011) show empirically that
    //   fill intensity has a two-regime structure (plateau near mid,
    //   sharp fall-off outside spread) rather than pure exponential.
    //   See: docs/CODE REVIEWS/COUNTERRESEARCH-20260325-1, S3.1.
    //
    //   FIX (T5-CR8): The sparse-fill miscalibration is now corrected
    //   in inventory_skew() by amplifying phi with a sparse-fill
    //   correction factor: effective_phi = phi * min(max(1, dense/actual), cap).
    //   The fill_intensity() function itself is left unchanged because it
    //   is the correct Poisson model; only the *downstream skew decision*
    //   needed adjustment for the sparse regime.
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
