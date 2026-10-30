// coin_budget.hpp -- [COINBUDGET 2026-10-30] Step 7's split of a coin that
// several pair sides spend.
//
// Why it exists.  XCH/wUSDC.b, XCH/BYC and XCH/DBX asks all sell the same XCH,
// and the wmilliETH.b/XCH bids spend it too; wUSDC.b funds the XCH/wUSDC.b
// and BYC/wUSDC.b bids, and BYC the XCH/BYC bids and the BYC/wUSDC.b asks.
// Step 7 sized every one of those sides as if it held the whole coin (each
// capped at the coin's confirmed balance minus its reserve), and Step 8,
// which never promises a coin twice, posted the pairs one at a time in the
// engine's map order: first come, first served, so a later pair got only
// what the earlier ones left.  The operator asked (2026-10-30) for the coins
// to be shared so the pairs stop stepping on each other.
//
// What it does.  Each Step 7 records, per side, how much of the coin it
// spends its sizing wanted (after the whole-coin caps, paired the way the
// ladder pairs its sides).  The next Step 7 compares those claims with the
// coin's balance minus its reserve.  When they fit, nothing is capped.  When
// they do not, the balance is split by weight -- the claim itself, so every
// side gets the same fraction of what it asked for -- never giving a side
// more than it asked for, and passing what a side leaves to the others.  The
// claims and the balance both count coins locked in our own resting offers,
// so a side's share covers its resting offers too.  The split is one
// heartbeat behind the sizing it caps; a side that claimed nothing last
// heartbeat gets no share while its coin is short, and claims again this one.
//
// Unit-tested in tests/test_coin_budget.cpp; the engine wiring is pinned by
// tests/test_sizing_wiring.cpp.
//
// Compliant with:
//   ISO/IEC 5055 -- no UB: NaN/negative inputs claim nothing; shares floor
//                   to whole mojos and never sum past the budget
//   ISO/IEC 25000 -- single responsibility, documented interface

#ifndef XOP_STRATEGY_COIN_BUDGET_HPP
#define XOP_STRATEGY_COIN_BUDGET_HPP

#include "xop/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace xop::strategy::coin_budget {

/// The key a coin is split under: "xch" for XCH however a pair spells it,
/// else the CAT asset id.
[[nodiscard]] inline std::string coin_key(const std::string& asset_id)
{
    return (asset_id == "XCH") ? std::string{"xch"} : asset_id;
}

/// One pair side that spends the coin.
struct Claim {
    double demand{0.0};  ///< what its sizing wanted, in the coin's mojos; <= 0 or NaN claims nothing
    double weight{0.0};  ///< its weight in the split; <= 0 or NaN gets nothing while the coin is short
};

/// Each claim's share of `budget` (the coin's mojos), in claim order, when
/// the claims together want more than the budget; nullopt when they fit (or
/// the budget is negative), and then nothing is capped.  Weighted water
/// filling: the budget is split by weight, no claim gets more than its
/// demand, and what a claim leaves passes to the rest by weight.  Shares
/// floor to whole mojos, taken from an integer remainder, so they never sum
/// past the budget at any magnitude.
[[nodiscard]] inline std::optional<std::vector<Mojo>> split(
    Mojo budget, const std::vector<Claim>& claims)
{
    if (budget < 0) return std::nullopt;
    auto valid = [](double x) { return std::isfinite(x) && x > 0.0; };
    double total = 0.0;
    for (const auto& c : claims) {
        if (valid(c.demand)) total += c.demand;
    }
    const double budget_d = static_cast<double>(budget);
    if (!(total > budget_d)) return std::nullopt;

    const std::size_t n = claims.size();
    std::vector<double> share(n, 0.0);
    std::vector<bool> open(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        open[i] = valid(claims[i].demand) && valid(claims[i].weight);
    }
    double fixed_sum = 0.0;
    // Each pass either settles every open claim or fixes at least one at its
    // demand, so n + 1 passes always finish.
    for (std::size_t pass = 0; pass <= n; ++pass) {
        double weights = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (open[i]) weights += claims[i].weight;
        }
        if (!(weights > 0.0)) break;
        const double remaining = std::max(0.0, budget_d - fixed_sum);
        bool fixed_any = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!open[i]) continue;
            if (remaining * (claims[i].weight / weights) >= claims[i].demand) {
                share[i] = claims[i].demand;
                fixed_sum += claims[i].demand;
                open[i] = false;
                fixed_any = true;
            }
        }
        if (fixed_any) continue;
        for (std::size_t i = 0; i < n; ++i) {
            if (open[i]) share[i] = remaining * (claims[i].weight / weights);
        }
        break;
    }

    // Whole mojos out of an integer remainder: a budget past 2^53 rounds in
    // double (two halves of 9007199254740995 both round up), and one at Mojo
    // max rounds to 2^63, which no Mojo holds.  A share at or past what is
    // left takes exactly what is left, so the shares never sum past the
    // budget and no out-of-range double is cast.
    std::vector<Mojo> out(n, 0);
    Mojo left = budget;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = std::floor(share[i]);
        const Mojo m = !(s > 0.0)                         ? Mojo{0}
                     : (s >= static_cast<double>(left))   ? left
                                                          : static_cast<Mojo>(s);
        out[i] = m;
        left -= m;
    }
    return out;
}

}  // namespace xop::strategy::coin_budget

#endif  // XOP_STRATEGY_COIN_BUDGET_HPP
