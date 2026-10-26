// target_gap.hpp -- the bid floor that moves both coins of a pair toward
// their target allocations.
//
// [TARGET-GAP 2026-10-26] Why it exists.  Avellaneda-Stoikov sizes the bid as
// q_max x max(0, 1 - q/q_max) with q the WHOLE holding of the base coin
// (avellaneda.cpp, Step 9 of compute_quotes).  q_max is a fixed count of base
// units that never looks at strategy.asset_target_allocations, so once a coin
// is held past q_max no pair bids for it, however far below its target it
// sits.  Live on 2026-10-26: 34.76 XCH against q_max 20 left every XCH-base
// bid at 0 while XCH was 13.8% of the portfolio against a 50% target, and a
// 100 wUSDC.b bridge had taken wUSDC.b to 59% against 20%.  The single-CAT
// cap zeroed the ask (it would collect more wUSDC.b), so XCH/wUSDC.b posted
// nothing: the ratio rebalancer, drift guard and deploy-idle floor only scale
// or floor what the strategy produced, and Step 6 dropped the pair first.
//
// What it does.  A pair's bid spends its quote coin and buys its base coin.
// Step 4 raises the strategy's bid size -- never lowers it -- to
//
//     min(base coin's deficit below its target, quote coin's excess above its target)
//
// valued in XCH and converted to base units.  It is non-zero only when the
// bid moves BOTH coins toward their targets, and filling all of it brings one
// of them exactly to its target, so it overshoots neither.  Asks need no
// floor: the strategy sizes them q_max + q, never below the whole holding.
// The floor runs before the Step 6 risk limits, so every limit, the ratio
// scaling, the drift guard, the wallet caps and the per-tier maximum still
// apply, and prices are untouched.
//
// Valuation: wallet-confirmed balances at the XCH marks the drift guard
// reads, over the TARGETED coins only (the targets describe that portfolio).
// A targeted coin with no validated balance, or with a balance and no mark,
// leaves the valuation unusable and every floor 0: today's sizing, never a
// guess.  Unit-tested in tests/test_target_gap.cpp; the Step 4 wiring is
// pinned by tests/test_sizing_wiring.cpp.
//
// Compliant with:
//   ISO/IEC 5055 -- no UB: NaN/negative inputs fail closed to a floor of 0
//   ISO/IEC 25000 -- single responsibility, documented interface

#ifndef XOP_STRATEGY_TARGET_GAP_HPP
#define XOP_STRATEGY_TARGET_GAP_HPP

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop::strategy::target_gap {

/// One targeted coin, as Step 4 sees it.
struct Holding {
    std::string key{};          ///< upper-cased symbol, the asset_target_allocations key
    double      target{0.0};    ///< its asset_target_allocations entry
    bool        valued{false};  ///< a validated balance is cached and, when non-zero, has a mark
    double      xch_value{0.0}; ///< confirmed balance at its mark, in XCH
};

/// The targeted portfolio.  ok is false unless every holding was valued.
struct Valuation {
    bool   ok{false};
    double total_xch{0.0};
    std::unordered_map<std::string, double> xch_by_key{};
    std::unordered_map<std::string, double> target_by_key{};
};

/// Values the targeted portfolio.  Not ok when there is no holding, when any
/// holding is unvalued, has a non-finite or negative value, or a target that
/// is not finite in [0, 1], or when the total is not > 0.
[[nodiscard]] inline Valuation value_targeted(const std::vector<Holding>& holdings)
{
    Valuation out{};
    if (holdings.empty()) { return out; }
    double total = 0.0;
    for (const Holding& h : holdings) {
        if (!h.valued || !std::isfinite(h.xch_value) || h.xch_value < 0.0
            || !std::isfinite(h.target) || h.target < 0.0 || h.target > 1.0) {
            return Valuation{};
        }
        out.xch_by_key[h.key] += h.xch_value;
        out.target_by_key[h.key] = h.target;
        total += h.xch_value;
    }
    if (!(std::isfinite(total) && total > 0.0)) { return Valuation{}; }
    out.ok = true;
    out.total_xch = total;
    return out;
}

/// The bid floor, in base display units: min(base deficit, quote excess) in
/// XCH, divided by the base coin's XCH per unit.  0 when the valuation is not
/// ok, either coin has no target, the two keys are the same coin, the rate is
/// not finite and > 0, or the bid would not move both coins toward their
/// targets.
[[nodiscard]] inline double bid_floor_units(const Valuation& v, const std::string& base_key,
                                            const std::string& quote_key,
                                            double base_xch_per_unit) noexcept
{
    if (!v.ok || base_key == quote_key
        || !(std::isfinite(base_xch_per_unit) && base_xch_per_unit > 0.0)) {
        return 0.0;
    }
    const auto base_target = v.target_by_key.find(base_key);
    const auto quote_target = v.target_by_key.find(quote_key);
    const auto base_value = v.xch_by_key.find(base_key);
    const auto quote_value = v.xch_by_key.find(quote_key);
    if (base_target == v.target_by_key.end() || quote_target == v.target_by_key.end()
        || base_value == v.xch_by_key.end() || quote_value == v.xch_by_key.end()) {
        return 0.0;
    }
    const double deficit = base_target->second * v.total_xch - base_value->second;
    const double excess = quote_value->second - quote_target->second * v.total_xch;
    if (!(deficit > 0.0) || !(excess > 0.0)) { return 0.0; }
    const double units = std::min(deficit, excess) / base_xch_per_unit;
    return std::isfinite(units) ? units : 0.0;
}

}  // namespace xop::strategy::target_gap

#endif  // XOP_STRATEGY_TARGET_GAP_HPP
