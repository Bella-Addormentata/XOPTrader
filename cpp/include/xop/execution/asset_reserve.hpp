// asset_reserve.hpp -- Per-asset reserve arithmetic for Steps 7 and 8.
//
// [GAS-RESERVE 2026-10-24] strategy.min_reserve_units is one figure for
// every asset: 1 unit of BYC, 1 unit of wUSDC.b, 1 unit of wmilliETH.b.
// wmilliETH.b now has a job besides trading -- the warp rebalancer unwraps
// it into ETH to pay Base gas -- so the operator needs to keep MORE of it
// than of anything else, and to keep it whatever the ladder wants.
// strategy.asset_reserve_units carries that per-asset figure; these helpers
// are the one definition of how it is applied, shared by Step 7's final
// pool cap, Step 8's reserve gates and Step 7's inventory throttle, and
// unit-tested in isolation (tests/test_asset_reserve.cpp).
//
// Compliant with:
//   ISO/IEC 5055 -- no UB: NaN/negative inputs clamp, conversions saturate
//   ISO/IEC 25000 -- single responsibility, documented interface

#ifndef XOP_EXECUTION_ASSET_RESERVE_HPP
#define XOP_EXECUTION_ASSET_RESERVE_HPP

#include "xop/types.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace xop::execution {

/// The reserve, in display units, Steps 7 and 8 hold back for `asset_id`:
/// its strategy.asset_reserve_units entry when it has one (an entry
/// REPLACES the global figure for that asset, in either direction), else
/// strategy.min_reserve_units.
[[nodiscard]] inline double effective_reserve_units(
    const std::unordered_map<std::string, double>& per_asset,
    double global_units, const std::string& asset_id)
{
    const auto it = per_asset.find(asset_id);
    return it != per_asset.end() ? it->second : global_units;
}

/// [PAIRMIN 2026-10-26] The reserve, in display units, Step 7 sizes a CAT's
/// pools to leave unsold (its final caps and the post-up-scale budget
/// filter), or nullopt when Step 7 leaves that CAT uncapped.  An
/// asset_reserve_units entry always binds, whatever the CAT-cap switch says;
/// otherwise strategy.min_reserve_units binds while
/// strategy.wallet_balance_caps_enabled is on.  The figure is
/// effective_reserve_units, which Step 8's reserve gates hold back for every
/// asset: Step 7 used to size a CAT with no entry to its whole balance, so a
/// small holding (1.0 wmilliETH.b against a 0.1 reserve) had its whole side
/// suppressed in Step 8 every cycle where a smaller ladder fits.  XCH's
/// reserve is fee_reserve_xch, applied by its own caps.
[[nodiscard]] inline std::optional<double> sizing_reserve_units(
    const std::unordered_map<std::string, double>& per_asset,
    double global_units, bool wallet_caps_enabled,
    const std::string& asset_id)
{
    const auto it = per_asset.find(asset_id);
    if (it != per_asset.end()) return it->second;
    if (!wallet_caps_enabled) return std::nullopt;
    return global_units;
}

/// Display units to mojos, rounded to nearest.  A NaN, negative or
/// non-positive-scale input is no reserve (0); a product past the Mojo
/// range saturates to Mojo max -- "keep everything", never a wrapped
/// negative that would read as no reserve.
[[nodiscard]] inline Mojo reserve_units_to_mojos(double units,
                                                 Mojo mojos_per_unit) noexcept
{
    if (!(units > 0.0) || mojos_per_unit <= 0) return 0;
    const double m = units * static_cast<double>(mojos_per_unit);
    if (!(m < static_cast<double>(std::numeric_limits<Mojo>::max()))) {
        return std::numeric_limits<Mojo>::max();
    }
    return static_cast<Mojo>(std::llround(m));
}

/// The most of a `balance` a pool may commit while `reserve` stays
/// unspent: min(pool, max(0, balance - reserve)).  Never raises a pool.
[[nodiscard]] inline Mojo cap_pool_above_reserve(Mojo pool, Mojo balance,
                                                 Mojo reserve) noexcept
{
    const Mojo budget = (reserve <= 0)        ? std::max(Mojo{0}, balance)
                      : (balance <= reserve)  ? Mojo{0}
                                              : balance - reserve;
    return std::max(Mojo{0}, std::min(pool, budget));
}

// ---------------------------------------------------------------------------
// [GAS-RESERVE review] The reserve kept SPENDABLE, not just unsold.
//
// Amount caps keep the ladder from selling the reserve, but the wallet funds
// an offer with whole coins: with one 5-unit wmilliETH.b coin and a 3-unit
// reserve, a 2-unit ask locks the whole coin and leaves nothing the warp
// rebalancer can unwrap until that offer turns over.  So each cycle Step 8
// picks spendable coins covering the reserve and every offer that spends the
// asset sends them as excluded_coin_ids (chia 2.7.4 coin-selection config,
// read from the top level of create_offer_for_ids): the wallet cannot lock
// them.  An offer the remaining coins cannot fund is refused by the wallet,
// which is the reserve doing its job.
// ---------------------------------------------------------------------------

struct ReserveCoin {
    std::string id;       ///< Coin name, lowercase hex, no 0x.
    Mojo        amount{0};
};

/// Spendable coins to hold back so `reserve` mojos stay spendable: the
/// smallest single coin covering it when there is one (fewest coins kept
/// out of trading), else largest-first until covered; every coin when even
/// all of them fall short.  None for no reserve.  Coins with a
/// non-positive amount or an empty id are ignored.
[[nodiscard]] inline std::vector<std::string> choose_reserve_coins(
    std::vector<ReserveCoin> coins, Mojo reserve)
{
    std::vector<std::string> out;
    if (reserve <= 0) return out;
    coins.erase(std::remove_if(coins.begin(), coins.end(),
                               [](const ReserveCoin& c) {
                                   return c.amount <= 0 || c.id.empty();
                               }),
                coins.end());
    // Ascending, ties by id, so the choice is deterministic.
    std::sort(coins.begin(), coins.end(),
              [](const ReserveCoin& a, const ReserveCoin& b) {
                  return a.amount != b.amount ? a.amount < b.amount
                                              : a.id < b.id;
              });
    for (const auto& c : coins) {
        if (c.amount >= reserve) {
            out.push_back(c.id);
            return out;
        }
    }
    Mojo kept = 0;
    for (auto it = coins.rbegin(); it != coins.rend() && kept < reserve; ++it) {
        out.push_back(it->id);
        kept = (it->amount > std::numeric_limits<Mojo>::max() - kept)
                   ? std::numeric_limits<Mojo>::max()
                   : kept + it->amount;
    }
    return out;
}

/// The reserve coins an offer must exclude: those held back for every wallet
/// the offer SPENDS from (a negative amount in the create_offer_for_ids
/// dict, keyed by wallet id).  Receive legs and unknown wallets add nothing.
[[nodiscard]] inline std::vector<std::string> reserved_coins_for_offer(
    const nlohmann::json& offer_dict,
    const std::unordered_map<std::string, std::vector<std::string>>& by_wallet)
{
    std::vector<std::string> out;
    if (by_wallet.empty() || !offer_dict.is_object()) return out;
    for (const auto& [wallet_id, amount] : offer_dict.items()) {
        if (!amount.is_number_integer() || amount.is_number_unsigned()) continue;
        if (amount.get<std::int64_t>() >= 0) continue;
        const auto it = by_wallet.find(wallet_id);
        if (it == by_wallet.end()) continue;
        out.insert(out.end(), it->second.begin(), it->second.end());
    }
    return out;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_ASSET_RESERVE_HPP
