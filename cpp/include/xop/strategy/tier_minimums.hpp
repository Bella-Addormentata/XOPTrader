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

/// [TIERFIRST 2026-10-31] One side's tier sizes fitted to its pool, minimums
/// first.  `sizes` are the tiers as Step 7 has them after the cap and the
/// up-scale (each at least its minimum), `mins` each tier's minimum, and
/// `weights` each tier's share of the pool before both (the growth it would
/// get), all in base mojos and ladder order, tightest first.  When the sizes
/// already fit `pool` they are returned unchanged.  Otherwise the most of the
/// side's WIDEST tiers whose minimums fit the pool keep their minimum, and
/// what the pool holds past those minimums is shared out among them by
/// weight -- each tier max(min, mu * weight), never above its current size,
/// with mu the largest that keeps their sum within the pool.  The tighter
/// tiers in front of them are left as they are, for the funding fit to skip.
/// So a pool that holds every minimum keeps every tier, and the growth
/// shrinks first: raising tiers to their minimums from their share of the
/// pool used to push the sum past it and cost the side its tight tier even
/// with the whole minimum ladder funded.  When even the widest minimum does
/// not fit, the sizes are returned unchanged and the fit sets them aside.
[[nodiscard]] inline std::vector<Mojo> fit_tier_sizes_to_pool(const std::vector<Mojo>& sizes,
                                                              const std::vector<Mojo>& mins,
                                                              const std::vector<Mojo>& weights,
                                                              Mojo pool)
{
    const std::size_t n = sizes.size();
    if (mins.size() != n || weights.size() != n || pool <= 0) return sizes;
    Mojo total = 0;
    for (const Mojo s : sizes) {
        if (s < 0 || total > pool - s) { total = pool + 1; break; }
        total += s;
    }
    if (total <= pool) return sizes;

    const std::vector<Mojo> wide_mins(mins.rbegin(), mins.rend());
    const std::size_t keep = tiers_within_budget(wide_mins, pool);
    if (keep == 0) return sizes;
    const std::size_t first = n - keep;  // the kept tiers are [first, n)
    for (std::size_t i = first; i < n; ++i) {
        if (mins[i] < 0 || sizes[i] < mins[i] || weights[i] < 0) return sizes;
    }

    // Sum of the kept tiers at a given mu, each clamped to [min, size].
    auto sized = [&](double mu, std::size_t i) -> Mojo {
        const double grown = std::floor(mu * static_cast<double>(weights[i]));
        const double upper = static_cast<double>(sizes[i]);
        const double lower = static_cast<double>(mins[i]);
        return static_cast<Mojo>(std::clamp(grown, lower, upper));
    };
    auto sum_at = [&](double mu) -> double {
        double sum = 0.0;
        for (std::size_t i = first; i < n; ++i) sum += static_cast<double>(sized(mu, i));
        return sum;
    };
    double hi = 1.0;
    for (std::size_t i = first; i < n; ++i) {
        if (weights[i] > 0) {
            hi = std::max(hi, static_cast<double>(sizes[i]) / static_cast<double>(weights[i]) + 1.0);
        }
    }
    double lo = 0.0;  // sum_at(0) is the kept minimums, which fit
    if (sum_at(hi) <= static_cast<double>(pool)) {
        lo = hi;
    } else {
        for (int iter = 0; iter < 200; ++iter) {
            const double mid = 0.5 * (lo + hi);
            if (sum_at(mid) <= static_cast<double>(pool)) lo = mid; else hi = mid;
        }
    }
    std::vector<Mojo> out = sizes;
    for (std::size_t i = first; i < n; ++i) out[i] = sized(lo, i);
    return out;
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

/// Whether Step 7 sizes a pair's two ladder sides from the smaller pool
/// (`LiquidityEngine::side_pools` pairing) or each from its own.  Pairing
/// stays the rule, except on a pair posting tier minimums where exactly one
/// side was both tapered by the drift guard (0 < scale < 1) and then lifted
/// to the pair minimum by the deploy-idle floor: that 1-unit side would
/// otherwise cut the other side to one unit too, so it could post only its
/// widest tier.  A side the strategy itself sized small (no drift taper)
/// keeps the pairing.
[[nodiscard]] constexpr bool pair_side_pools(bool tier_minimums_on,
                                             bool bid_guard_floored,
                                             bool ask_guard_floored) noexcept
{
    return !(tier_minimums_on && bid_guard_floored != ask_guard_floored);
}

}  // namespace xop::strategy::tiermin

#endif  // XOP_STRATEGY_TIER_MINIMUMS_HPP
