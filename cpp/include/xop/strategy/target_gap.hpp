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
// Once every pair is quoted, Step 4 raises each strategy bid size -- never
// lowers it -- to the pair's share of
//
//     min(base coin's deficit below its target, quote coin's excess above its target)
//
// valued in XCH and converted to base units.  It is non-zero only when the
// bid moves BOTH coins toward their targets.  The pairs share each coin's
// gap: where the pairs buying one coin together want more than its deficit,
// or the pairs spending one coin together want more than its excess, each of
// their floors is scaled down by the same factor, so filling every floor of
// a heartbeat takes no coin past its target.  (An offer already resting keeps
// the size it was posted with until it is repriced or expires.)  Asks need no
// floor: the strategy sizes them q_max + q, never below the whole holding.
// The floor runs before the Step 6 risk limits, so every limit, the ratio
// scaling, the drift guard, the wallet caps and the per-tier maximum still
// apply, and prices are untouched.  A pair whose strategy rejected its inputs
// (the zero-spread quote) gets no floor and claims no gap.
//
// Valuation: wallet-confirmed balances at the XCH marks the drift guard
// reads, over the TARGETED coins only (the targets describe that portfolio).
// A targeted coin with no validated balance, a balance older than
// strategy.pace_max_balance_age_blocks, or a balance and no mark leaves the
// valuation unusable and every floor 0: today's sizing, never a guess.  The
// pace balance refresh keeps every targeted balance fresh while this is on,
// a coin whose pairs are all disabled included.  Unit-tested in
// tests/test_target_gap.cpp; the engine wiring is pinned by
// tests/test_sizing_wiring.cpp.
//
// Compliant with:
//   ISO/IEC 5055 -- no UB: NaN/negative inputs fail closed to a floor of 0
//   ISO/IEC 25000 -- single responsibility, documented interface

#ifndef XOP_STRATEGY_TARGET_GAP_HPP
#define XOP_STRATEGY_TARGET_GAP_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop::strategy::target_gap {

/// One targeted coin, as Step 4 sees it.
struct Holding {
    std::string   key{};                  ///< upper-cased symbol, the asset_target_allocations key
    double        target{0.0};            ///< its asset_target_allocations entry
    bool          balance_known{false};   ///< a validated wallet balance is cached for it
    std::uint64_t balance_age_blocks{0};  ///< heartbeat height minus that balance's block
    bool          priced{false};          ///< the balance is zero, or the coin has an XCH mark
    double        xch_value{0.0};         ///< the confirmed balance at its mark, in XCH
};

/// The targeted portfolio.  ok is false unless every holding was valued;
/// unusable_reason then names the first coin that was not, and why (a fixed
/// text per coin and cause, so a caller can log it once per change).
struct Valuation {
    bool        ok{false};
    double      total_xch{0.0};
    std::unordered_map<std::string, double> xch_by_key{};
    std::unordered_map<std::string, double> target_by_key{};
    std::string unusable_reason{};
};

/// Values the targeted portfolio.  Not ok when there is no holding, when any
/// holding has no validated balance, a balance more than
/// `max_balance_age_blocks` old, no mark for a non-zero balance, a non-finite
/// or negative value, or a target that is not finite in [0, 1], or when the
/// total is not > 0.
[[nodiscard]] inline Valuation value_targeted(const std::vector<Holding>& holdings,
                                              std::uint64_t max_balance_age_blocks)
{
    Valuation out{};
    if (holdings.empty()) {
        out.unusable_reason = "no asset_target_allocations";
        return out;
    }
    const auto unusable = [](const std::string& reason) {
        Valuation v{};
        v.unusable_reason = reason;
        return v;
    };
    double total = 0.0;
    for (const Holding& h : holdings) {
        if (!h.balance_known) {
            return unusable(h.key + ": no validated wallet balance");
        }
        if (h.balance_age_blocks > max_balance_age_blocks) {
            return unusable(h.key + ": wallet balance more than "
                            + std::to_string(max_balance_age_blocks) + " blocks old");
        }
        if (!h.priced) {
            return unusable(h.key + ": no XCH price");
        }
        if (!std::isfinite(h.xch_value) || h.xch_value < 0.0) {
            return unusable(h.key + ": unusable XCH value");
        }
        if (!std::isfinite(h.target) || h.target < 0.0 || h.target > 1.0) {
            return unusable(h.key + ": target outside [0, 1]");
        }
        out.xch_by_key[h.key] += h.xch_value;
        out.target_by_key[h.key] = h.target;
        total += h.xch_value;
    }
    if (!(std::isfinite(total) && total > 0.0)) {
        return unusable("the targeted coins are worth nothing");
    }
    out.ok = true;
    out.total_xch = total;
    return out;
}

/// How far `key` sits below its target, in XCH; 0 when it is at or above it,
/// untargeted, or the valuation is not ok.
[[nodiscard]] inline double deficit_xch(const Valuation& v, const std::string& key) noexcept
{
    const auto target = v.target_by_key.find(key);
    const auto value = v.xch_by_key.find(key);
    if (!v.ok || target == v.target_by_key.end() || value == v.xch_by_key.end()) {
        return 0.0;
    }
    const double d = target->second * v.total_xch - value->second;
    return d > 0.0 ? d : 0.0;
}

/// How far `key` sits above its target, in XCH; 0 when it is at or below it,
/// untargeted, or the valuation is not ok.
[[nodiscard]] inline double excess_xch(const Valuation& v, const std::string& key) noexcept
{
    const auto target = v.target_by_key.find(key);
    const auto value = v.xch_by_key.find(key);
    if (!v.ok || target == v.target_by_key.end() || value == v.xch_by_key.end()) {
        return 0.0;
    }
    const double e = value->second - target->second * v.total_xch;
    return e > 0.0 ? e : 0.0;
}

/// One pair's floor before sharing, in XCH: min(base deficit, quote excess).
/// 0 when the valuation is not ok, either coin has no target, the two keys
/// are the same coin, or the bid would not move both coins toward their
/// targets.
[[nodiscard]] inline double unshared_bid_floor_xch(const Valuation& v, const std::string& base_key,
                                                   const std::string& quote_key) noexcept
{
    if (!v.ok || base_key == quote_key) {
        return 0.0;
    }
    return std::min(deficit_xch(v, base_key), excess_xch(v, quote_key));
}

/// A pair's bid, as the floor sees it: the coin it buys and the coin it spends.
struct PairCoins {
    std::string base_key{};   ///< upper-cased base symbol
    std::string quote_key{};  ///< upper-cased quote symbol
};

/// Every pair's floor in XCH, one per entry of `pairs`, the pairs sharing
/// each coin's gap.  A pair's unshared floor is scaled by the smaller of two
/// factors: its base coin's deficit over what every pair buying that coin
/// wants, and its quote coin's excess over what every pair spending that coin
/// wants (each capped at 1).  So the floors buying one coin never add up to
/// more than its deficit, nor the floors spending one coin to more than its
/// excess, whatever the order of `pairs`.  All 0 when the valuation is not ok.
[[nodiscard]] inline std::vector<double> shared_bid_floors_xch(const Valuation& v,
                                                               const std::vector<PairCoins>& pairs)
{
    std::vector<double> floors(pairs.size(), 0.0);
    if (!v.ok) {
        return floors;
    }
    std::unordered_map<std::string, double> wanted_buying{};
    std::unordered_map<std::string, double> wanted_spending{};
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        floors[i] = unshared_bid_floor_xch(v, pairs[i].base_key, pairs[i].quote_key);
        if (floors[i] > 0.0) {
            wanted_buying[pairs[i].base_key] += floors[i];
            wanted_spending[pairs[i].quote_key] += floors[i];
        }
    }
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (!(floors[i] > 0.0)) {
            continue;
        }
        // Both sums hold floors[i] > 0, and a positive floor means a positive
        // deficit and excess, so neither division is by zero.
        const double buy_scale = std::min(1.0, deficit_xch(v, pairs[i].base_key)
                                                   / wanted_buying[pairs[i].base_key]);
        const double spend_scale = std::min(1.0, excess_xch(v, pairs[i].quote_key)
                                                     / wanted_spending[pairs[i].quote_key]);
        floors[i] *= std::min(buy_scale, spend_scale);
    }
    return floors;
}

/// A floor in XCH as base display units, at `base_xch_per_unit` XCH per
/// unit.  0 unless the floor is > 0 and the rate is finite and > 0.
[[nodiscard]] inline double floor_units(double floor_xch, double base_xch_per_unit) noexcept
{
    if (!(floor_xch > 0.0) || !std::isfinite(floor_xch)
        || !(std::isfinite(base_xch_per_unit) && base_xch_per_unit > 0.0)) {
        return 0.0;
    }
    const double units = floor_xch / base_xch_per_unit;
    return std::isfinite(units) ? units : 0.0;
}

}  // namespace xop::strategy::target_gap

#endif  // XOP_STRATEGY_TARGET_GAP_HPP
