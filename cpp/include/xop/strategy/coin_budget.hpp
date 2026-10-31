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
// [TIERFIRST 2026-10-31] Minimum tiers first.  The operator asked that every
// pair get its tiers filled before any pair's tiers grow past their
// minimums.  A side on a pair with tier minimums claims them too (its
// minimum offer sizes, widest tier first, in the coin's mojos), and a short
// coin is split in two passes: first the minimum tiers, one tier level at a
// time from the widest, so every side gets its widest tier before any side
// gets its next one; then what is left, by weight as above, up to what each
// side still wants.  A level that does not fit whole goes to its cheapest
// tiers first and ends the first pass, so the tight tiers are the ones left
// blank.
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
    double weight{0.0};  ///< its weight in the split; <= 0 or NaN gets no more than its minimum tiers while the coin is short
    /// [TIERFIRST] Its minimum offer sizes, widest tier first, in the coin's
    /// mojos: what the first pass funds.  Empty on a pair without tier
    /// minimums.  Only the run of tiers whose sum fits `demand` counts.
    std::vector<double> tiers{};
};

namespace detail {

/// Weighted water filling of `budget` over `caps`: split by weight, none
/// past its cap, what one leaves passed to the rest by weight.  A cap or
/// weight that is not a positive finite number gets nothing.
[[nodiscard]] inline std::vector<double> water_fill(double budget,
                                                    const std::vector<double>& caps,
                                                    const std::vector<double>& weights)
{
    auto valid = [](double x) { return std::isfinite(x) && x > 0.0; };
    const std::size_t n = caps.size();
    std::vector<double> share(n, 0.0);
    std::vector<bool> open(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        open[i] = valid(caps[i]) && valid(weights[i]);
    }
    double fixed_sum = 0.0;
    // Each pass either settles every open claim or fixes at least one at its
    // cap, so n + 1 passes always finish.
    for (std::size_t pass = 0; pass <= n; ++pass) {
        double total_weight = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (open[i]) total_weight += weights[i];
        }
        if (!(total_weight > 0.0)) break;
        const double remaining = std::max(0.0, budget - fixed_sum);
        bool fixed_any = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (!open[i]) continue;
            if (remaining * (weights[i] / total_weight) >= caps[i]) {
                share[i] = caps[i];
                fixed_sum += caps[i];
                open[i] = false;
                fixed_any = true;
            }
        }
        if (fixed_any) continue;
        for (std::size_t i = 0; i < n; ++i) {
            if (open[i]) share[i] = remaining * (weights[i] / total_weight);
        }
        break;
    }
    return share;
}

/// [TIERFIRST] The first pass: each claim's funded minimum tiers, one tier
/// level at a time from the widest.  A level that does not fit whole goes to
/// its cheapest tiers while they fit, and ends the pass.
[[nodiscard]] inline std::vector<double> minimum_tiers(double budget,
                                                       const std::vector<Claim>& claims)
{
    auto valid = [](double x) { return std::isfinite(x) && x > 0.0; };
    const std::size_t n = claims.size();
    std::vector<double> funded(n, 0.0);
    double left = budget;
    for (std::size_t level = 0;; ++level) {
        // The claims whose tier at this level is still within their demand.
        std::vector<std::size_t> wanting;
        double level_cost = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = claims[i];
            if (!valid(c.demand) || level >= c.tiers.size()) continue;
            const double t = c.tiers[level];
            if (!valid(t) || funded[i] + t > c.demand) continue;
            wanting.push_back(i);
            level_cost += t;
        }
        if (wanting.empty()) break;
        if (level_cost <= left) {
            for (const std::size_t i : wanting) funded[i] += claims[i].tiers[level];
            left -= level_cost;
            continue;
        }
        std::stable_sort(wanting.begin(), wanting.end(),
                         [&](std::size_t a, std::size_t b) {
                             return claims[a].tiers[level] < claims[b].tiers[level];
                         });
        for (const std::size_t i : wanting) {
            const double t = claims[i].tiers[level];
            if (t > left) break;
            funded[i] += t;
            left -= t;
        }
        break;
    }
    return funded;
}

}  // namespace detail

/// Each claim's share of `budget` (the coin's mojos), in claim order, when
/// the claims together want more than the budget; nullopt when they fit (or
/// the budget is negative), and then nothing is capped.  First each claim's
/// minimum tiers (`Claim::tiers`, see detail::minimum_tiers), then weighted
/// water filling of the rest: split by weight, no claim past its demand, and
/// what a claim leaves passed to the rest by weight.  Shares floor to whole
/// mojos, taken from an integer remainder, so they never sum past the budget
/// at any magnitude.
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
    std::vector<double> share = detail::minimum_tiers(budget_d, claims);
    double funded = 0.0;
    for (const double f : share) funded += f;
    std::vector<double> rest(n, 0.0);
    std::vector<double> weights(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        rest[i] = valid(claims[i].demand) ? claims[i].demand - share[i] : 0.0;
        weights[i] = claims[i].weight;
    }
    const std::vector<double> extra =
        detail::water_fill(std::max(0.0, budget_d - funded), rest, weights);
    for (std::size_t i = 0; i < n; ++i) share[i] += extra[i];

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
