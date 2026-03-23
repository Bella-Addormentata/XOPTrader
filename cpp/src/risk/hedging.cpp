// hedging.cpp -- Hedging framework implementation (Layers 1-4).
//
// See hedging.hpp for interface documentation, formulas, and safety rationale.
//
// Key invariant: no method in this file ever produces a SuggestedTrade that
// would violate the NEVER-SELL-AT-A-LOSS constraint.  Every sell suggestion
// is pre-checked against cost basis before inclusion.
//
// Compliant with:
//   ISO/IEC 27001:2022  (auditable hedge logic, no hidden state mutation)
//   ISO/IEC 5055        (bounds-checked, no UB paths)
//   ISO/IEC 25000       (single-responsibility functions, documented formulas)

#include "xop/risk/hedging.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <sstream>
#include <shared_mutex>

namespace xop {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HedgingManager::HedgingManager(const StrategyConfig& strat_cfg,
                               const RiskConfig&     risk_cfg) noexcept
    : strat_cfg_(strat_cfg)
    , risk_cfg_(risk_cfg)
{
}

// ===========================================================================
// Layer 1 -- Inventory-based self-hedging (quote skewing)
// ===========================================================================

// Formula from GLFT model (Section 5):
//     skew = phi * inventory_q / q_max
//
// Properties:
//   - Linear in inventory: small positions produce small skew.
//   - Bounded by phi when |inventory_q| == q_max.
//   - At q_max, the full skew phi is applied; beyond q_max the skew
//     is clamped to prevent runaway quote displacement.
//
// The sign convention follows the strategy document:
//   positive inventory_q (long base) -> positive skew -> quotes shift DOWN
//   negative inventory_q (short base) -> negative skew -> quotes shift UP

double HedgingManager::compute_skew_adjustment(double inventory_q,
                                               double q_max,
                                               double phi) noexcept
{
    // Guard: q_max must be positive.
    // In debug builds this is a hard assertion; in release builds we clamp
    // to avoid division by zero.
    assert(q_max > 0.0 && "q_max must be strictly positive");
    if (q_max <= 0.0) {
        return 0.0;
    }

    // Clamp inventory to [-q_max, +q_max] so that the skew never exceeds
    // the configured strength phi.  Positions beyond q_max should have been
    // caught by the hard limit in PreTradeCheck, but defense in depth.
    const double clamped_q = std::clamp(inventory_q, -q_max, q_max);

    return phi * clamped_q / q_max;
}

// ===========================================================================
// Layer 2 -- Natural two-sided balancing (NHE computation)
// ===========================================================================

// Formula from Section 9:
//     NHE = 1 - (|net_inventory_change| / total_volume)
//     Target: NHE > 0.70
//
// Interpretation:
//   total_volume is the sum of the absolute value of all fills (buys + sells).
//   net_inventory_change is the signed sum (buys positive, sells negative).
//
//   If every buy is matched by a sell: net = 0, NHE = 1.0 (perfect).
//   If all fills are buys: net = total, NHE = 0.0 (worst case).
//
// Edge case: total_volume == 0 means no fills occurred.  Returning 0.0
// rather than 1.0 is intentional: zero volume means zero hedging is
// happening, which is the alarm the caller needs.

double HedgingManager::compute_nhe(double net_inventory_change,
                                   double total_volume) noexcept
{
    if (total_volume <= 0.0) {
        return 0.0;  // no volume -- no natural hedging is occurring
    }

    const double ratio = std::abs(net_inventory_change) / total_volume;

    // ratio should be in [0, 1] by construction, but clamp defensively
    // (e.g. floating-point accumulation could nudge it slightly above 1).
    const double clamped_ratio = std::clamp(ratio, 0.0, 1.0);

    return 1.0 - clamped_ratio;
}

// ===========================================================================
// Layer 3 -- Portfolio-level netting across pairs
// ===========================================================================

// Simply aggregates balance per asset across all positions.  The caller
// uses this to see which assets have outsized exposure that could be
// offset by adjusting activity on a different pair containing the same asset.

std::unordered_map<AssetId, double>
HedgingManager::compute_portfolio_net_exposure(
    const std::vector<Position>& positions)
{
    std::unordered_map<AssetId, double> exposure;
    exposure.reserve(positions.size());

    for (const auto& pos : positions) {
        // Multiple positions in the same asset (theoretically possible if
        // the same asset appears in multiple pairs) are summed.
        exposure[pos.asset_id] += static_cast<double>(pos.balance);
    }

    return exposure;
}

// ===========================================================================
// Layer 4 -- Statistical pairs hedging (correlation management)
// ===========================================================================

void HedgingManager::set_correlations(
    std::vector<CorrelationEntry> correlations)
{
    std::unique_lock lock(mtx_correlations_);
    correlations_ = std::move(correlations);
}

std::vector<CorrelationEntry>
HedgingManager::get_correlations() const
{
    // Return a copy under a shared lock to avoid data races with
    // set_correlations().  Correlations change rarely (~every 1000 blocks)
    // so the copy cost is negligible compared to the safety guarantee.
    std::shared_lock lock(mtx_correlations_);
    return correlations_;
}

// ===========================================================================
// Rebalancing suggestions (combines Layers 1-4)
// ===========================================================================

// The algorithm:
//   1. Compute current allocation fractions from positions.
//   2. Compute deviation = target_fraction - current_fraction for each asset.
//   3. Sort assets by absolute deviation, descending.
//   4. For each overweight asset (negative deviation):
//        a. Check the never-sell-at-loss constraint.
//        b. If selling is safe, find the most underweight asset and emit
//           a sell-overweight/buy-underweight suggestion.
//        c. If selling would be at a loss, emit a "hold" diagnostic with
//           urgency 0 (informational only).
//   5. Return suggestions ordered by urgency.

std::vector<SuggestedTrade> HedgingManager::suggest_rebalancing_trades(
    const std::vector<Position>&               positions,
    const std::unordered_map<AssetId, double>&  targets,
    const std::unordered_map<AssetId, Mojo>&    current_prices) const
{
    std::vector<SuggestedTrade> suggestions;

    const double total_val = total_portfolio_value(positions);
    if (total_val <= 0.0) {
        return suggestions;  // empty portfolio -- nothing to rebalance
    }

    // Step 1-2: compute deviations per asset.
    //   deviation > 0  -> underweight (need to buy)
    //   deviation < 0  -> overweight  (need to sell, if not at a loss)
    std::unordered_map<AssetId, double> deviations;
    std::unordered_map<AssetId, double> current_fractions;

    for (const auto& pos : positions) {
        const double frac = static_cast<double>(pos.balance) / total_val;
        current_fractions[pos.asset_id] = frac;

        auto tgt_it = targets.find(pos.asset_id);
        const double target_frac = (tgt_it != targets.end()) ? tgt_it->second : 0.0;
        deviations[pos.asset_id] = target_frac - frac;
    }

    // Also record assets that have a target but no current position (underweight).
    for (const auto& [asset_id, target_frac] : targets) {
        if (deviations.find(asset_id) == deviations.end()) {
            deviations[asset_id] = target_frac;  // fully underweight
        }
    }

    // Step 3-4: generate suggestions.
    // Collect overweight assets (negative deviation, magnitude > tolerance).
    struct OverweightEntry {
        AssetId id;
        double  deviation;  // negative
    };
    std::vector<OverweightEntry> overweight;

    for (const auto& [asset_id, dev] : deviations) {
        if (dev < -kRebalanceTolerance) {
            overweight.push_back({asset_id, dev});
        }
    }

    // Sort by deviation ascending (most overweight first, i.e. most negative).
    std::sort(overweight.begin(), overweight.end(),
              [](const OverweightEntry& a, const OverweightEntry& b) {
                  return a.deviation < b.deviation;
              });

    for (const auto& ow : overweight) {
        // Look up the position for cost-basis check.
        const auto pos_it = std::find_if(
            positions.begin(), positions.end(),
            [&](const Position& p) { return p.asset_id == ow.id; });

        if (pos_it == positions.end()) {
            continue;  // should not happen, but defensive
        }

        // Look up current market price for this asset.
        auto price_it = current_prices.find(ow.id);
        const Mojo cur_price = (price_it != current_prices.end())
                             ? price_it->second
                             : 0;

        // NEVER-SELL-AT-A-LOSS gate.
        if (would_sell_at_loss(*pos_it, cur_price)) {
            // Emit a hold diagnostic -- do NOT suggest a trade.
            SuggestedTrade hold{};
            hold.sell_asset = ow.id;
            hold.buy_asset  = "";  // n/a
            hold.quantity   = 0;
            hold.reason     = "HOLD: asset " + ow.id
                            + " is overweight but underwater (cost_basis > market)."
                              " Patience is the ultimate hedge.";
            hold.urgency    = 0.0;
            suggestions.push_back(std::move(hold));
            continue;
        }

        // Find the best underweight asset to buy.
        const AssetId buy_id = find_best_buy_candidate(deviations);
        if (buy_id.empty()) {
            continue;  // nothing to buy -- portfolio is at target
        }

        // Compute the trade quantity: the smaller of the overweight excess
        // and the underweight deficit, expressed in mojos of the sell asset.
        const double excess_frac = std::abs(ow.deviation);
        const double deficit_frac = deviations[buy_id];  // positive
        const double trade_frac = std::min(excess_frac, deficit_frac);
        const auto trade_qty = static_cast<Mojo>(
            std::llround(trade_frac * total_val));

        if (trade_qty <= 0) {
            continue;  // rounding eliminated the trade
        }

        // Compute urgency based on deviation magnitude.
        // Scale linearly: 0% deviation -> 0.0 urgency, 20%+ deviation -> 1.0.
        const double urgency = std::clamp(excess_frac / 0.20, 0.0, 1.0);

        SuggestedTrade st{};
        st.sell_asset = ow.id;
        st.buy_asset  = buy_id;
        st.quantity   = trade_qty;
        st.reason     = "Rebalance: " + ow.id + " overweight by "
                      + std::to_string(static_cast<int>(excess_frac * 100))
                      + "%, " + buy_id + " underweight by "
                      + std::to_string(static_cast<int>(deficit_frac * 100))
                      + "%.";
        st.urgency    = urgency;
        suggestions.push_back(std::move(st));

        // Reduce the deficit for the buy candidate so subsequent overweight
        // assets pick a different target if this one is now satisfied.
        deviations[buy_id] -= trade_frac;
    }

    // Sort by urgency descending (most urgent first).
    std::sort(suggestions.begin(), suggestions.end(),
              [](const SuggestedTrade& a, const SuggestedTrade& b) {
                  return a.urgency > b.urgency;
              });

    return suggestions;
}

// ===========================================================================
// Diagnostic -- hedge status summary
// ===========================================================================

std::string HedgingManager::hedge_summary(
    const std::vector<Position>& positions,
    double                       net_inv_change,
    double                       total_volume)
{
    std::ostringstream oss;

    // NHE.
    const double nhe = compute_nhe(net_inv_change, total_volume);
    oss << "NHE: " << (nhe * 100.0) << "% (target >" << (nhe_target() * 100.0)
        << "%)";

    if (nhe < nhe_target()) {
        oss << " [BELOW TARGET]";
    }

    // Net exposure per asset.
    oss << " | Exposure:";
    const auto exposure = compute_portfolio_net_exposure(positions);
    for (const auto& [id, bal] : exposure) {
        oss << " " << id << "=" << static_cast<int64_t>(bal);
    }

    return oss.str();
}

// ===========================================================================
// Private helpers
// ===========================================================================

double HedgingManager::total_portfolio_value(
    const std::vector<Position>& positions) noexcept
{
    double total = 0.0;
    for (const auto& p : positions) {
        total += static_cast<double>(p.balance);
    }
    return total;
}

// ---------------------------------------------------------------------------
// would_sell_at_loss -- check the NEVER-SELL-AT-A-LOSS constraint.
//
// A sell is "at a loss" if the current market price is strictly below the
// position's weighted-average cost basis.  When current_price is zero
// (unknown), we conservatively assume a loss to prevent accidental sells
// in the absence of price data.
// ---------------------------------------------------------------------------

bool HedgingManager::would_sell_at_loss(const Position& pos,
                                        Mojo current_price) noexcept
{
    if (pos.balance <= 0) {
        return false;  // nothing to sell
    }

    if (current_price <= 0) {
        return true;  // no price data -- assume loss (conservative)
    }

    // Cost basis is mojos-of-quote per mojo-of-base.  current_price is in
    // the same units.  Selling at current_price < cost_basis is a loss.
    return current_price < pos.cost_basis;
}

// ---------------------------------------------------------------------------
// find_best_sell_candidate -- most overweight asset that is not underwater.
// ---------------------------------------------------------------------------

AssetId HedgingManager::find_best_sell_candidate(
    const std::vector<Position>&               positions,
    const std::unordered_map<AssetId, double>&  deviations,
    const std::unordered_map<AssetId, Mojo>&    current_prices)
{
    AssetId best_id;
    double  best_dev = 0.0;  // looking for most negative

    for (const auto& pos : positions) {
        auto dev_it = deviations.find(pos.asset_id);
        if (dev_it == deviations.end()) continue;

        // Only overweight assets (negative deviation).
        if (dev_it->second >= 0.0) continue;

        // Never-sell-at-loss check.
        auto price_it = current_prices.find(pos.asset_id);
        const Mojo price = (price_it != current_prices.end())
                         ? price_it->second
                         : 0;
        if (would_sell_at_loss(pos, price)) continue;

        if (dev_it->second < best_dev) {
            best_dev = dev_it->second;
            best_id  = pos.asset_id;
        }
    }

    return best_id;
}

// ---------------------------------------------------------------------------
// find_best_buy_candidate -- most underweight asset.
// ---------------------------------------------------------------------------

AssetId HedgingManager::find_best_buy_candidate(
    const std::unordered_map<AssetId, double>& deviations)
{
    AssetId best_id;
    double  best_dev = 0.0;  // looking for most positive

    for (const auto& [id, dev] : deviations) {
        if (dev > best_dev) {
            best_dev = dev;
            best_id  = id;
        }
    }

    return best_id;
}

}  // namespace xop
