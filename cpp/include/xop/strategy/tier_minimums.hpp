// tier_minimums.hpp -- [TIERMIN 2026-10-29] Step 7's per-tier minimum offer
// sizes (PairConfig::tight_tier_min_offer_units / wide_tier_min_offer_units)
// and per-pair cap (max_offer_size_units_override), in base mojos.
//
// On 2026-10-29 every XCH/wUSDC.b bid sat at the flat 5 XCH cap (tier 0 would
// have been 16.9 XCH) and the operator asked for 10 XCH at the tight tier down
// to 1 XCH at the wide one, still growing with the balances above that.  The
// minimums only ever reshape what the side's risk-approved pool and its
// funding budget can pay for: a side that cannot fund its tightest tier at
// its minimum sets the minimums aside and posts its plain ladder.

#ifndef XOP_STRATEGY_TIER_MINIMUMS_HPP
#define XOP_STRATEGY_TIER_MINIMUMS_HPP

#include "xop/peg_registry.hpp"
#include "xop/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace xop::strategy::tiermin {

/// A tier minimum in base mojos, rounded UP so an offer never falls short of
/// the configured figure (1.0001 units at 1,000 mojos per unit is 1,001
/// mojos).  A product within 0.05 mojo of a whole mojo is that mojo, so a
/// figure like 4.6 whose double product lands a hair above 4,600 stays 4,600.
/// nullopt when the product is not a representable mojo amount (config
/// bounds the units to 1e6).
[[nodiscard]] inline std::optional<Mojo> min_units_to_mojos(double units,
                                                           std::int64_t mpu) noexcept
{
    if (!std::isfinite(units) || units < 0.0 || mpu <= 0) return std::nullopt;
    const double x = units * static_cast<double>(mpu);
    const double nearest = std::round(x);
    return (std::fabs(x - nearest) <= 0.05) ? to_mojo_checked(nearest)
                                            : ceil_mojo_checked(x);
}

/// A cap in base mojos, rounded DOWN so an offer never exceeds it.  A
/// positive cap is at least 1 mojo, because 0 means "no cap"; 0 or less is
/// no cap.  nullopt when the product is not a representable mojo amount.
[[nodiscard]] inline std::optional<Mojo> cap_units_to_mojos(double units,
                                                           std::int64_t mpu) noexcept
{
    if (!std::isfinite(units) || mpu <= 0) return std::nullopt;
    if (!(units > 0.0)) return Mojo{0};
    const std::optional<Mojo> m = floor_mojo_checked(units * static_cast<double>(mpu));
    if (!m.has_value()) return std::nullopt;
    return std::max<Mojo>(1, *m);
}

/// How many of one side's tiers, tightest first, keep their tier-minimum
/// sizes: the longest prefix whose `sizes` (base mojos) fit within `pool` --
/// the side's risk-approved pool, the sum of the sizes Step 7 had before the
/// cap and the minimums -- and whose `costs` (in the funding asset) fit
/// within `budget` when that is known.  0 means even the tightest tier does
/// not fit, and the side sets its minimums aside.
[[nodiscard]] inline std::size_t tiers_within_pool(const std::vector<Mojo>& sizes,
                                                   const std::vector<Mojo>& costs,
                                                   Mojo pool,
                                                   std::optional<Mojo> budget) noexcept
{
    std::size_t n = tiers_within_budget(sizes, pool);
    if (budget.has_value()) {
        n = std::min(n, tiers_within_budget(costs, *budget));
    }
    return n;
}

}  // namespace xop::strategy::tiermin

#endif  // XOP_STRATEGY_TIER_MINIMUMS_HPP
