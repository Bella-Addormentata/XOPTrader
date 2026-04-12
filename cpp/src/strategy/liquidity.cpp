// liquidity.cpp -- Multi-tier liquidity provision engine for XOPTrader CHIA
//                  DEX market-making bot.
//
// Implements the tier ladder, inventory skew, and rebalance logic described
// in CHIA_MARKET_MAKER_STRATEGY.md Section 11 (Liquidity Provision Strategies)
// and Section 7 (Capital Allocation).
//
// Coin-set model awareness:
//   Every tier's size is computed as a fraction of the available capital or
//   inventory.  The execution layer must pre-split coins to match these sizes
//   before creating on-chain offers.  Rebalancing always cancels-then-recreates
//   (never amends), because CHIA offers are immutable once broadcast -- the
//   only way to update is to spend the locked coin (cancellation) and post a
//   new offer referencing different coins.
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets processed, deterministic audit trail)
//   ISO/IEC 5055        (bounds-checked, no undefined behaviour on edge cases)
//   ISO/IEC 25000       (tested public interface, clear error modes)
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++17)

#include "xop/strategy/liquidity.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <stdexcept>

namespace xop {

// ===========================================================================
// Config validation
// ===========================================================================

void LiquidityEngine::validate_config(const LiquidityConfig& cfg) {
    // Tier count must be at least 1.
    if (cfg.num_tiers == 0) {
        throw std::invalid_argument(
            "LiquidityEngine: num_tiers must be >= 1");
    }

    // Vector lengths must match num_tiers.
    if (cfg.tier_spacing_bps.size() != cfg.num_tiers) {
        throw std::invalid_argument(
            "LiquidityEngine: tier_spacing_bps length ("
            + std::to_string(cfg.tier_spacing_bps.size())
            + ") must equal num_tiers ("
            + std::to_string(cfg.num_tiers) + ")");
    }
    if (cfg.tier_size_pct.size() != cfg.num_tiers) {
        throw std::invalid_argument(
            "LiquidityEngine: tier_size_pct length ("
            + std::to_string(cfg.tier_size_pct.size())
            + ") must equal num_tiers ("
            + std::to_string(cfg.num_tiers) + ")");
    }

    // All tier spacings must be positive and monotonically non-decreasing.
    // Tier 0 is the tightest (closest to mid), tier N-1 is the widest.
    for (std::uint32_t i = 0; i < cfg.num_tiers; ++i) {
        if (cfg.tier_spacing_bps[i] <= 0.0) {
            throw std::invalid_argument(
                "LiquidityEngine: tier_spacing_bps[" + std::to_string(i)
                + "] must be > 0, got "
                + std::to_string(cfg.tier_spacing_bps[i]));
        }
        if (i > 0 && cfg.tier_spacing_bps[i] < cfg.tier_spacing_bps[i - 1]) {
            throw std::invalid_argument(
                "LiquidityEngine: tier_spacing_bps must be non-decreasing; "
                "tier " + std::to_string(i) + " ("
                + std::to_string(cfg.tier_spacing_bps[i])
                + ") < tier " + std::to_string(i - 1) + " ("
                + std::to_string(cfg.tier_spacing_bps[i - 1]) + ")");
        }
    }

    // All size fractions must be positive.
    for (std::uint32_t i = 0; i < cfg.num_tiers; ++i) {
        if (cfg.tier_size_pct[i] <= 0.0) {
            throw std::invalid_argument(
                "LiquidityEngine: tier_size_pct[" + std::to_string(i)
                + "] must be > 0, got "
                + std::to_string(cfg.tier_size_pct[i]));
        }
    }

    // Size fractions should sum to approximately 1.0 (allow 5% tolerance).
    const double pct_sum = std::accumulate(
        cfg.tier_size_pct.begin(), cfg.tier_size_pct.end(), 0.0);
    if (std::abs(pct_sum - 1.0) > 0.05) {
        throw std::invalid_argument(
            "LiquidityEngine: tier_size_pct must sum to ~1.0, got "
            + std::to_string(pct_sum));
    }

    // Phi must be non-negative.
    if (cfg.phi < 0.0) {
        throw std::invalid_argument(
            "LiquidityEngine: phi must be >= 0, got "
            + std::to_string(cfg.phi));
    }

    // Rebalance thresholds must be positive where applicable.
    if (cfg.price_deviation_threshold <= 0.0) {
        throw std::invalid_argument(
            "LiquidityEngine: price_deviation_threshold must be > 0");
    }
    if (cfg.volume_spike_factor <= 0.0) {
        throw std::invalid_argument(
            "LiquidityEngine: volume_spike_factor must be > 0");
    }
    if (cfg.volatility_spike_factor <= 0.0) {
        throw std::invalid_argument(
            "LiquidityEngine: volatility_spike_factor must be > 0");
    }
}

// ===========================================================================
// Construction
// ===========================================================================

LiquidityEngine::LiquidityEngine(const std::string& pair_name,
                                 const LiquidityConfig& cfg)
    : pair_name_(pair_name)
    , cfg_(cfg)
{
    if (pair_name_.empty()) {
        throw std::invalid_argument(
            "LiquidityEngine: pair_name must not be empty");
    }
    validate_config(cfg_);
}

// ===========================================================================
// Raw ladder construction (no inventory skew)
// ===========================================================================

std::vector<TierQuote> LiquidityEngine::build_raw_ladder(
    std::int64_t          mid,
    std::int64_t          available_capital,
    std::int64_t          available_inventory,
    const LiquidityConfig& cfg)
{
    // Pre-allocate: num_tiers bids + num_tiers asks.
    std::vector<TierQuote> ladder;
    ladder.reserve(static_cast<std::size_t>(cfg.num_tiers) * 2);

    // Guard against non-positive mid-price.  In this case the ladder is
    // empty because we cannot compute meaningful tier prices.
    if (mid <= 0) {
        return ladder;
    }

    // Clamp capital/inventory to non-negative values.  Negative values
    // would indicate a data error upstream; defensive clamping prevents
    // nonsensical negative offer sizes.
    const std::int64_t cap = std::max(available_capital,    static_cast<std::int64_t>(0));
    const std::int64_t inv = std::max(available_inventory,  static_cast<std::int64_t>(0));

    for (std::uint32_t i = 0; i < cfg.num_tiers; ++i) {
        const double spread_bps = cfg.tier_spacing_bps[i];
        const double size_frac  = cfg.tier_size_pct[i];

        // -- Bid tier --
        // bid_price = mid * (1 - spread_bps / 10000)
        // Computed in floating point then rounded to int64 (mojos).
        // Rounding DOWN for bids is conservative (we pay less).
        const double bid_price_f =
            static_cast<double>(mid) * (1.0 - spread_bps / 10000.0);
        const auto   bid_price   =
            static_cast<std::int64_t>(std::floor(bid_price_f));

        // bid_size = available_capital * size_fraction
        // Capital is in quote-asset mojos.  Each bid tier gets a fraction.
        const auto bid_size =
            static_cast<std::int64_t>(
                std::floor(static_cast<double>(cap) * size_frac));

        if (bid_price > 0 && bid_size > 0) {
            TierQuote tq;
            tq.tier_index = static_cast<std::uint8_t>(i);
            tq.side       = Side::Bid;
            tq.price      = bid_price;
            tq.size       = bid_size;
            tq.spread_bps = spread_bps;
            ladder.push_back(tq);
        }

        // -- Ask tier --
        // ask_price = mid * (1 + spread_bps / 10000)
        // Rounding UP for asks is conservative (we receive more).
        const double ask_price_f =
            static_cast<double>(mid) * (1.0 + spread_bps / 10000.0);
        const auto   ask_price   =
            static_cast<std::int64_t>(std::ceil(ask_price_f));

        // ask_size = available_inventory * size_fraction
        // Inventory is in base-asset mojos.  Each ask tier gets a fraction.
        const auto ask_size =
            static_cast<std::int64_t>(
                std::floor(static_cast<double>(inv) * size_frac));

        if (ask_price > 0 && ask_size > 0) {
            TierQuote tq;
            tq.tier_index = static_cast<std::uint8_t>(i);
            tq.side       = Side::Ask;
            tq.price      = ask_price;
            tq.size       = ask_size;
            tq.spread_bps = spread_bps;
            ladder.push_back(tq);
        }
    }

    // Sort: all bids first (by tier ascending), then all asks (by tier
    // ascending).  This ordering simplifies downstream coin allocation
    // since the execution layer processes one side at a time.
    std::sort(ladder.begin(), ladder.end(),
        [](const TierQuote& a, const TierQuote& b) {
            if (a.side != b.side) {
                // Bid (0) before Ask (1).
                return static_cast<int>(a.side) < static_cast<int>(b.side);
            }
            return a.tier_index < b.tier_index;
        });

    return ladder;
}

// ===========================================================================
// Inventory skew adjustment
// ===========================================================================

std::vector<TierQuote> LiquidityEngine::apply_inventory_skew(
    const std::vector<TierQuote>& ladder,
    double inventory_ratio,
    double phi)
{
    // Clamp inventory_ratio to [0, 1] for safety.
    const double ratio = std::clamp(inventory_ratio, 0.0, 1.0);

    // Deviation from the balanced midpoint (0.5).
    // Positive when long-biased, negative when short-biased.
    const double deviation = (ratio - 0.5) / 0.5;  // normalised to [-1, +1]

    // Bid multiplier: > 1 when long (widen bids to discourage buying),
    //                 < 1 when short (tighten bids to encourage buying).
    //
    // Ask multiplier: < 1 when long (tighten asks to encourage selling),
    //                 > 1 when short (widen asks to discourage selling).
    //
    // The skew direction is verified:
    //   Long (ratio=0.8): deviation = +0.6
    //     bid_mult = 1 + 1.5*0.6 = 1.9  (wider bids -- discourage buying)
    //     ask_mult = 1 - 1.5*0.6 = 0.1  (tighter asks -- encourage selling)
    //   Short (ratio=0.2): deviation = -0.6
    //     bid_mult = 1 + 1.5*(-0.6) = 0.1  (tighter bids -- encourage buying)
    //     ask_mult = 1 - 1.5*(-0.6) = 1.9  (wider asks -- discourage selling)
    //   Balanced (ratio=0.5): deviation = 0
    //     bid_mult = 1.0, ask_mult = 1.0  (no shift)
    //
    // Multipliers are clamped to [0.1, 3.0] to prevent degenerate pricing
    // (zero or negative spreads, or excessively wide spreads).
    const double bid_mult = std::clamp(1.0 + phi * deviation, 0.1, 3.0);
    const double ask_mult = std::clamp(1.0 - phi * deviation, 0.1, 3.0);

    std::vector<TierQuote> adjusted;
    adjusted.reserve(ladder.size());

    for (const auto& tq : ladder) {
        TierQuote adj = tq;

        if (tq.side == Side::Bid) {
            // Adjust bid spread: multiply the distance from mid by bid_mult.
            // Reconstruct from spread_bps (the authoritative distance).
            //
            // The price is recalculated rather than scaled directly, because
            // direct scaling of the mojo price would introduce rounding errors
            // that accumulate across tiers.  Using the bps distance and the
            // original mid avoids this.
            //
            // However, we do not have mid stored in the TierQuote.  So we
            // infer it from the original price and spread:
            //   original: bid = mid * (1 - spread_bps / 10000)
            //   => mid = bid / (1 - spread_bps / 10000)
            //
            // Then recompute with adjusted spread:
            //   adjusted_spread = spread_bps * bid_mult
            //   adjusted_bid = mid * (1 - adjusted_spread / 10000)
            const double orig_spread_frac = tq.spread_bps / 10000.0;
            const double denom = 1.0 - orig_spread_frac;

            // Guard against degenerate spread (>= 100%, which would mean
            // bid at or below zero).
            if (denom > 0.0) {
                const double inferred_mid =
                    static_cast<double>(tq.price) / denom;
                const double adj_spread_bps = tq.spread_bps * bid_mult;
                const double adj_price_f =
                    inferred_mid * (1.0 - adj_spread_bps / 10000.0);

                adj.price      = std::max(
                    static_cast<std::int64_t>(std::floor(adj_price_f)),
                    static_cast<std::int64_t>(1));  // floor at 1 mojo
                adj.spread_bps = adj_spread_bps;
            }
        } else {
            // Adjust ask spread similarly.
            //   original: ask = mid * (1 + spread_bps / 10000)
            //   => mid = ask / (1 + spread_bps / 10000)
            const double orig_spread_frac = tq.spread_bps / 10000.0;
            const double denom = 1.0 + orig_spread_frac;

            if (denom > 0.0) {
                const double inferred_mid =
                    static_cast<double>(tq.price) / denom;
                const double adj_spread_bps = tq.spread_bps * ask_mult;
                const double adj_price_f =
                    inferred_mid * (1.0 + adj_spread_bps / 10000.0);

                adj.price      = std::max(
                    static_cast<std::int64_t>(std::ceil(adj_price_f)),
                    static_cast<std::int64_t>(1));
                adj.spread_bps = adj_spread_bps;
            }
        }

        adjusted.push_back(adj);
    }

    return adjusted;
}

// ===========================================================================
// compute_ladder -- public interface (instance config)
// ===========================================================================

std::vector<TierQuote> LiquidityEngine::compute_ladder(
    std::int64_t mid,
    double       sigma,
    double       inventory_ratio,
    std::int64_t available_capital,
    std::int64_t available_inventory) const
{
    return compute_ladder(mid, sigma, inventory_ratio,
                          available_capital, available_inventory, cfg_);
}

// ===========================================================================
// compute_ladder -- public interface (explicit config, for backtests)
// ===========================================================================

std::vector<TierQuote> LiquidityEngine::compute_ladder(
    std::int64_t          mid,
    double                sigma,
    double                inventory_ratio,
    std::int64_t          available_capital,
    std::int64_t          available_inventory,
    const LiquidityConfig& cfg) const
{
    // sigma is accepted for future use (volatility-adaptive tier spacing)
    // but is not consumed in the current implementation.  Suppressing the
    // unused-parameter warning explicitly.
    (void)sigma;

    // Build the raw (symmetric) ladder from config and market parameters.
    std::vector<TierQuote> ladder =
        build_raw_ladder(mid, available_capital, available_inventory, cfg);

    if (ladder.empty()) {
        return ladder;
    }

    // Apply inventory-aware skew adjustment.
    ladder = apply_inventory_skew(ladder, inventory_ratio, cfg.phi);

    return ladder;
}

// ===========================================================================
// should_rebalance -- evaluate all trigger conditions
// ===========================================================================

bool LiquidityEngine::should_rebalance(
    BlockHeight   current_block,
    std::int64_t  current_price,
    std::int64_t  current_vol,
    std::int64_t  avg_vol,
    double        current_sigma,
    double        avg_sigma,
    double        inventory_ratio) const
{
    // Reset the reason before evaluation.
    last_reason_ = RebalanceReason::None;

    // If we have never rebalanced, always trigger (bootstrap).
    if (last_rebalance_block_ == 0) {
        last_reason_ = RebalanceReason::PriceMove;
        return true;
    }

    // --- Trigger 1: Price deviation ---
    // Rebalance if mid-price moved by more than the configured threshold
    // (default 2%) since the last rebalance.
    if (last_rebalance_price_ > 0 && current_price > 0) {
        const double price_change =
            std::abs(static_cast<double>(current_price)
                     - static_cast<double>(last_rebalance_price_))
            / static_cast<double>(last_rebalance_price_);

        if (price_change > cfg_.price_deviation_threshold) {
            last_reason_ = RebalanceReason::PriceMove;
            return true;
        }
    }

    // --- Trigger 2: Inventory skew threshold crossing ---
    // Rebalance if inventory_ratio crossed the 0.6 boundary (in either
    // direction) since the last rebalance.  This detects both transitions
    // into and out of a skewed state.
    const double skew_hi = cfg_.rebalance_skew_threshold;
    const double skew_lo = 1.0 - skew_hi;  // symmetric: 0.40 by default

    const bool was_skewed =
        (last_inventory_ratio_ > skew_hi) || (last_inventory_ratio_ < skew_lo);
    const bool is_skewed  =
        (inventory_ratio > skew_hi) || (inventory_ratio < skew_lo);

    if (was_skewed != is_skewed) {
        last_reason_ = RebalanceReason::InventorySkew;
        return true;
    }

    // --- Trigger 3: Offer staleness ---
    // Rebalance if more than offer_ttl_blocks have elapsed since the last
    // rebalance.  This ensures offers are refreshed at least once per TTL
    // window, preventing stale pricing in a quiet market.
    if (current_block > last_rebalance_block_) {
        const auto blocks_elapsed =
            current_block - last_rebalance_block_;
        if (blocks_elapsed >= cfg_.offer_ttl_blocks) {
            last_reason_ = RebalanceReason::TTLExpired;
            return true;
        }
    }

    // --- Trigger 4: Volume spike ---
    // Rebalance if current volume exceeds the rolling average by the
    // configured factor (default 3x).  A sudden volume surge indicates
    // a regime change that warrants tighter spreads (Section 11).
    if (avg_vol > 0 && current_vol > 0) {
        const double vol_ratio =
            static_cast<double>(current_vol) / static_cast<double>(avg_vol);
        if (vol_ratio > cfg_.volume_spike_factor) {
            last_reason_ = RebalanceReason::RegimeChange;
            return true;
        }
    }

    // --- Trigger 5: Volatility spike ---
    // Rebalance if current sigma exceeds the 7-day average by the
    // configured factor (default 2x).  Elevated volatility requires
    // wider spreads for risk management (Section 11).
    if (avg_sigma > 0.0 && current_sigma > 0.0) {
        const double sigma_ratio = current_sigma / avg_sigma;
        if (sigma_ratio > cfg_.volatility_spike_factor) {
            last_reason_ = RebalanceReason::ForcedRefresh;
            return true;
        }
    }

    return false;
}

// ===========================================================================
// Rebalance reason accessors
// ===========================================================================

RebalanceReason LiquidityEngine::get_rebalance_reason() const noexcept {
    return last_reason_;
}

const char* LiquidityEngine::get_rebalance_reason_str() const noexcept {
    return to_string(last_reason_);
}

// ===========================================================================
// Record a completed rebalance cycle
// ===========================================================================

void LiquidityEngine::record_rebalance(BlockHeight  block,
                                       std::int64_t mid_price,
                                       double       inventory_ratio) {
    last_rebalance_block_ = block;
    last_rebalance_price_ = mid_price;
    last_inventory_ratio_ = inventory_ratio;
    ++rebalance_count_;
}

// ===========================================================================
// Accessors
// ===========================================================================

const std::string& LiquidityEngine::pair_name() const noexcept {
    return pair_name_;
}

const LiquidityConfig& LiquidityEngine::config() const noexcept {
    return cfg_;
}

std::uint64_t LiquidityEngine::rebalance_count() const noexcept {
    return rebalance_count_;
}

BlockHeight LiquidityEngine::last_rebalance_block() const noexcept {
    return last_rebalance_block_;
}

std::int64_t LiquidityEngine::last_rebalance_price() const noexcept {
    return last_rebalance_price_;
}

// ===========================================================================
// Order book gap analysis
// ===========================================================================

std::vector<OrderBookGap> analyse_order_book_gaps(
    const std::vector<CompetingOffer>& offers,
    std::int64_t                       mid,
    double                             min_gap_bps,
    double                             max_scan_bps)
{
    std::vector<OrderBookGap> gaps;

    if (mid <= 0 || offers.empty()) {
        return gaps;
    }

    const double mid_f = static_cast<double>(mid);

    // Analyse each side independently.
    for (const auto side : {Side::Bid, Side::Ask}) {
        // Collect distance-from-mid (in bps) for each offer on this side.
        std::vector<double> levels_bps;
        levels_bps.reserve(offers.size());

        for (const auto& o : offers) {
            if (o.side != side || o.price <= 0) continue;

            const double price_f = static_cast<double>(o.price);
            double dist_bps = 0.0;
            if (side == Side::Bid) {
                dist_bps = (mid_f - price_f) / mid_f * 10000.0;
            } else {
                dist_bps = (price_f - mid_f) / mid_f * 10000.0;
            }

            // Only consider offers within the scan range and on the
            // correct side of mid.
            if (dist_bps >= 0.0 && dist_bps <= max_scan_bps) {
                levels_bps.push_back(dist_bps);
            }
        }

        // Sort levels by distance from mid (ascending).
        std::sort(levels_bps.begin(), levels_bps.end());

        // Remove duplicate levels (within 1 bps tolerance).
        auto it = std::unique(levels_bps.begin(), levels_bps.end(),
            [](double a, double b) { return std::abs(a - b) < 1.0; });
        levels_bps.erase(it, levels_bps.end());

        // Check for gap from 0 (mid) to first offer.
        double prev_bps = 0.0;
        for (const double lvl : levels_bps) {
            const double gap_width = lvl - prev_bps;
            if (gap_width >= min_gap_bps) {
                OrderBookGap g;
                g.side       = side;
                g.low_bps    = prev_bps;
                g.high_bps   = lvl;
                g.center_bps = (prev_bps + lvl) / 2.0;
                g.width_bps  = gap_width;
                gaps.push_back(g);
            }
            prev_bps = lvl;
        }

        // Check for gap from last offer to max_scan_bps.
        if (max_scan_bps - prev_bps >= min_gap_bps) {
            OrderBookGap g;
            g.side       = side;
            g.low_bps    = prev_bps;
            g.high_bps   = max_scan_bps;
            g.center_bps = (prev_bps + max_scan_bps) / 2.0;
            g.width_bps  = max_scan_bps - prev_bps;
            gaps.push_back(g);
        }
    }

    // Sort by width descending (widest gaps first).
    std::sort(gaps.begin(), gaps.end(),
        [](const OrderBookGap& a, const OrderBookGap& b) {
            return a.width_bps > b.width_bps;
        });

    return gaps;
}

// ===========================================================================
// compute_ladder -- gap-aware + adverse-selection-aware overload
// ===========================================================================

std::vector<TierQuote> LiquidityEngine::compute_ladder(
    std::int64_t                       mid,
    double                             sigma,
    double                             inventory_ratio,
    std::int64_t                       available_capital,
    std::int64_t                       available_inventory,
    const std::vector<CompetingOffer>& competing_offers,
    const LiquidityConfig&             cfg) const
{
    // -- Step 1: Make a mutable copy of the config for dynamic adjustments --
    LiquidityConfig adj_cfg = cfg;

    // -- Step 2: Adverse-selection-aware tier sizing -------------------------
    // On slow blockchains (Chia: ~52 s/block), tier 0 (tightest spread) is
    // the most vulnerable to informed traders.  We reduce its size and
    // redistribute capital to outer tiers where adverse selection risk is
    // lower.
    if (adj_cfg.adverse_selection_sizing && adj_cfg.num_tiers > 1) {
        double decay = adj_cfg.adverse_selection_decay;

        // Under high volatility, apply extra conservative sizing.
        if (adj_cfg.adverse_selection_sigma_threshold > 0.0
            && sigma > adj_cfg.adverse_selection_sigma_threshold)
        {
            decay *= 0.5;  // More aggressive reallocation to outer tiers.
            decay = std::max(decay, 0.1);  // Floor to prevent degenerate weights.
        }

        // Compute inverse-decay weights: tier i gets weight = 1/decay^i.
        // This gives less weight to tier 0 (inner) and more to outer tiers.
        std::vector<double> weights(adj_cfg.num_tiers);
        double w_sum = 0.0;
        double factor = 1.0;
        for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
            weights[i] = 1.0 / factor;
            w_sum += weights[i];
            factor *= decay;
        }

        // Normalise weights so they sum to 1.0, preserving the original
        // total allocation.
        if (w_sum > 0.0) {
            for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                adj_cfg.tier_size_pct[i] = weights[i] / w_sum;
            }
        }

        // Validate: ensure no tier ended up with NaN/Inf/negative size.
        // If any tier is degenerate, fall back to the original cfg sizes.
        bool sizing_valid = true;
        for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
            if (!std::isfinite(adj_cfg.tier_size_pct[i])
                || adj_cfg.tier_size_pct[i] < 0.0) {
                sizing_valid = false;
                break;
            }
        }
        if (!sizing_valid) {
            spdlog::warn("[Liquidity] {} adverse-selection sizing produced "
                         "degenerate values -- falling back to baseline",
                         pair_name_);
            adj_cfg.tier_size_pct = cfg.tier_size_pct;
        }

        spdlog::debug("[Liquidity] {} adverse-selection sizing: "
                      "decay={:.2f} sigma={:.4f} sizes=[{}]",
                      pair_name_, decay, sigma,
                      [&]() {
                          std::string s;
                          for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                              if (i > 0) s += ", ";
                              char buf[16];
                              std::snprintf(buf, sizeof(buf), "%.3f",
                                            adj_cfg.tier_size_pct[i]);
                              s += buf;
                          }
                          return s;
                      }());
    }

    // -- Step 2b: Fill-rate-weighted adaptive tier sizing --------------------
    // Blend historical per-tier fill rates with the current tier_size_pct
    // to allocate more capital to tiers that get taken more frequently.
    // This adapts the sizing to observed market demand without disrupting
    // the ladder structure (min floor prevents any tier from being starved).
    if (adj_cfg.fill_rate_sizing
        && adj_cfg.num_tiers > 1
        && adj_cfg.fill_rate_blend > 0.0
        && !adj_cfg.tier_fill_rates.empty()
        && adj_cfg.tier_fill_rates.size() >= adj_cfg.num_tiers)
    {
        // Check that we have meaningful fill data (at least one tier filled).
        double total_rate = 0.0;
        for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
            total_rate += adj_cfg.tier_fill_rates[i];
        }

        if (total_rate > 0.0) {
            // Build fill-rate weights with minimum floor.
            std::vector<double> fr_weights(adj_cfg.num_tiers);
            double fr_sum = 0.0;
            for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                fr_weights[i] = std::max(adj_cfg.tier_fill_rates[i],
                                         adj_cfg.fill_rate_min_pct);
                fr_sum += fr_weights[i];
            }

            // Normalise fill-rate weights to sum to 1.0.
            if (fr_sum > 0.0) {
                for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                    fr_weights[i] /= fr_sum;
                }
            }

            // Blend: new_pct = (1 - blend) * current + blend * fill_rate_weight
            const double b = adj_cfg.fill_rate_blend;
            for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                adj_cfg.tier_size_pct[i] =
                    (1.0 - b) * adj_cfg.tier_size_pct[i] + b * fr_weights[i];
            }

            // Re-normalise to ensure sum = 1.0 after blending.
            double pct_sum = 0.0;
            for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                pct_sum += adj_cfg.tier_size_pct[i];
            }
            if (pct_sum > 0.0) {
                for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                    adj_cfg.tier_size_pct[i] /= pct_sum;
                }
            }

            // Validate blended sizes.
            bool blend_valid = true;
            for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                if (!std::isfinite(adj_cfg.tier_size_pct[i])
                    || adj_cfg.tier_size_pct[i] < 0.0) {
                    blend_valid = false;
                    break;
                }
            }
            if (!blend_valid) {
                spdlog::warn("[Liquidity] {} fill-rate sizing produced "
                             "degenerate values -- falling back",
                             pair_name_);
                adj_cfg.tier_size_pct = cfg.tier_size_pct;
            }

            spdlog::debug("[Liquidity] {} fill-rate sizing: "
                          "blend={:.2f} rates=[{}] sizes=[{}]",
                          pair_name_, b,
                          [&]() {
                              std::string s;
                              for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                                  if (i > 0) s += ", ";
                                  char buf[16];
                                  std::snprintf(buf, sizeof(buf), "%.3f",
                                                adj_cfg.tier_fill_rates[i]);
                                  s += buf;
                              }
                              return s;
                          }(),
                          [&]() {
                              std::string s;
                              for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                                  if (i > 0) s += ", ";
                                  char buf[16];
                                  std::snprintf(buf, sizeof(buf), "%.3f",
                                                adj_cfg.tier_size_pct[i]);
                                  s += buf;
                              }
                              return s;
                          }());
        }
    }

    // -- Step 3: Gap-aware dynamic tier spacing ------------------------------
    // Analyse the competing order book for gaps and shift tier spacing
    // toward underserved price ranges.
    if (adj_cfg.gap_aware_spacing && !competing_offers.empty() && mid > 0) {
        auto gaps = analyse_order_book_gaps(
            competing_offers, mid,
            adj_cfg.min_gap_bps, adj_cfg.max_gap_scan_bps);

        if (!gaps.empty()) {
            // Collect gap centers from BOTH sides (bid + ask) together.
            // Since build_raw_ladder uses one tier_spacing_bps for both
            // sides, we merge all gaps and target the widest ones
            // regardless of side.
            std::vector<double> gap_centers;
            for (const auto& g : gaps) {
                gap_centers.push_back(g.center_bps);
                if (gap_centers.size() >= adj_cfg.num_tiers) break;
            }
            // gaps are already sorted by width descending, so we get
            // the widest first.

            if (!gap_centers.empty()) {
                // Sort gap centers ascending so we can blend with the
                // ascending baseline tier_spacing_bps.
                std::sort(gap_centers.begin(), gap_centers.end());

                // Scale the blend factor by order book depth.  With very
                // few competing offers (e.g. 2) the gaps are statistical
                // noise; with 10+ offers the analysis is meaningful.
                // depth_scale ramps linearly from 0 at 0 offers to 1.0
                // at 10 offers.
                const double depth_scale = std::min(
                    1.0,
                    static_cast<double>(competing_offers.size()) / 10.0);

                // Blend each tier's spacing toward the nearest gap center.
                for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                    // Find the closest gap center for this tier.
                    double closest_gap = adj_cfg.tier_spacing_bps[i];
                    double min_dist = 1e9;
                    for (double gc : gap_centers) {
                        const double dist = std::abs(gc - adj_cfg.tier_spacing_bps[i]);
                        if (dist < min_dist) {
                            min_dist = dist;
                            closest_gap = gc;
                        }
                    }

                    // Blend: adjusted = (1-f)*baseline + f*gap_center,
                    // where f is depth-scaled to avoid over-reacting to
                    // thin books.
                    const double blend =
                        adj_cfg.gap_blend_factor * depth_scale;
                    double adjusted = (1.0 - blend) * adj_cfg.tier_spacing_bps[i]
                                    + blend * closest_gap;

                    // Enforce strictly ascending constraint:
                    // each tier must be at least prev + 10 bps.
                    if (i > 0) {
                        adjusted = std::max(adjusted,
                                            adj_cfg.tier_spacing_bps[i - 1] + 10.0);
                    }
                    // Floor at 10 bps to prevent degenerate spreads.
                    adjusted = std::max(adjusted, 10.0);

                    adj_cfg.tier_spacing_bps[i] = adjusted;
                }

                spdlog::debug("[Liquidity] {} gap-aware spacing: "
                              "gaps={} depth={} blend={:.2f} adjusted spacing=[{}]",
                              pair_name_, gap_centers.size(),
                              competing_offers.size(),
                              adj_cfg.gap_blend_factor * depth_scale,
                              [&]() {
                                  std::string s;
                                  for (std::uint32_t i = 0; i < adj_cfg.num_tiers; ++i) {
                                      if (i > 0) s += ", ";
                                      s += std::to_string(
                                          static_cast<int>(adj_cfg.tier_spacing_bps[i]));
                                  }
                                  return s;
                              }());
            }
        }
    }

    // -- Step 4: Build ladder with adjusted config --------------------------
    // Use the base compute_ladder (which does build_raw_ladder + skew) but
    // with the adjusted config containing dynamic spacing and sizing.
    return compute_ladder(mid, sigma, inventory_ratio,
                          available_capital, available_inventory, adj_cfg);
}

}  // namespace xop
