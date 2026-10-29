// tier_minimums.hpp -- [TIERMIN 2026-10-29] Step 7's per-tier minimum offer
// sizes (PairConfig::tight_tier_min_offer_units / wide_tier_min_offer_units)
// in base mojos, and how a side short of funds fits them.
//
// On 2026-10-29 every XCH/wUSDC.b bid sat at the flat 5 XCH cap (tier 0 would
// have been 16.9 XCH) and the operator asked for 10 XCH at the tight tier down
// to 1 XCH at the wide one, still growing with the balances above that.  The
// minimums only ever reshape what the side's risk-approved pool and its
// funding budget can pay for.  Short of funds, the side skips its tightest
// (largest) tiers and keeps its wider ones, as the operator asked; a side
// that cannot fund even its widest tier at its minimum sets the minimums
// aside and posts its plain ladder.

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

/// How many of one side's tiers, widest first, keep their tier-minimum sizes:
/// the longest run of its WIDEST tiers whose `sizes` (base mojos) fit within
/// `pool` -- the side's risk-approved pool, the sum of the sizes Step 7 had
/// before the cap and the minimums -- and whose `costs` (in the funding
/// asset) fit within `budget` when that is known.  Both vectors are in ladder
/// order, tightest first; the side skips the tiers in front of that run.  The
/// tight tiers carry the largest minimums, so skipping them first keeps the
/// most tiers on the book.  0 means even the widest tier does not fit, and
/// the side sets its minimums aside.
[[nodiscard]] inline std::size_t widest_tiers_within_pool(const std::vector<Mojo>& sizes,
                                                          const std::vector<Mojo>& costs,
                                                          Mojo pool,
                                                          std::optional<Mojo> budget)
{
    const std::vector<Mojo> wide_sizes(sizes.rbegin(), sizes.rend());
    std::size_t n = tiers_within_budget(wide_sizes, pool);
    if (budget.has_value()) {
        const std::vector<Mojo> wide_costs(costs.rbegin(), costs.rend());
        n = std::min(n, tiers_within_budget(wide_costs, *budget));
    }
    return n;
}

/// How many of the tiers in [min_tier, max_tier] Step 8's XCH limiter keeps
/// for a pair with tier minimums: the widest first while their summed cost
/// (`tier_costs[i]` is tier min_tier + i's) fits `budget`, and always at
/// least the widest tier -- the mirror of the tightest-first limiter that
/// always keeps tier 0.  Returns the lowest tier index kept.
[[nodiscard]] inline std::uint8_t lowest_affordable_tier_widest_first(
    const std::vector<Mojo>& tier_costs, std::uint8_t min_tier,
    std::uint8_t max_tier, Mojo budget) noexcept
{
    std::uint8_t lowest = max_tier;
    Mojo used = 0;
    for (int ti = max_tier; ti >= static_cast<int>(min_tier); --ti) {
        const std::size_t i = static_cast<std::size_t>(ti - min_tier);
        const Mojo c = (i < tier_costs.size()) ? tier_costs[i] : Mojo{0};
        if (c < 0 || used > budget - c) break;
        used += c;
        lowest = static_cast<std::uint8_t>(ti);
    }
    return lowest;
}

}  // namespace xop::strategy::tiermin

#endif  // XOP_STRATEGY_TIER_MINIMUMS_HPP
