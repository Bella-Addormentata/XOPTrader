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
#include <string>
#include <unordered_map>

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

}  // namespace xop::execution

#endif  // XOP_EXECUTION_ASSET_RESERVE_HPP
