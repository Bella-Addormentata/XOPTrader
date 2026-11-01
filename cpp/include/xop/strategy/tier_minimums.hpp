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
#include <limits>
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

    // A kept tier at a given mu, clamped in whole mojos to [min, size], so
    // its minimum and its size stay exact at any magnitude.
    auto sized = [&](double mu, std::size_t i) -> Mojo {
        const double grown = std::floor(mu * static_cast<double>(weights[i]));
        if (!(grown > 0.0)) return mins[i];
        if (grown >= static_cast<double>(sizes[i])) return sizes[i];
        return std::clamp(static_cast<Mojo>(grown), mins[i], sizes[i]);
    };
    // Integer sum of the kept tiers, saturating past the pool.
    auto fits_at = [&](double mu) {
        Mojo sum = 0;
        for (std::size_t i = first; i < n; ++i) {
            const Mojo t = sized(mu, i);
            if (sum > pool - t) return false;
            sum += t;
        }
        return true;
    };
    double hi = 1.0;
    for (std::size_t i = first; i < n; ++i) {
        if (weights[i] > 0) {
            hi = std::max(hi, static_cast<double>(sizes[i]) / static_cast<double>(weights[i]) + 1.0);
        }
    }
    double lo = 0.0;  // at 0 every kept tier is at its minimum, and they fit
    if (fits_at(hi)) {
        lo = hi;
    } else {
        for (int iter = 0; iter < 200; ++iter) {
            const double mid = 0.5 * (lo + hi);
            if (fits_at(mid)) lo = mid; else hi = mid;
        }
    }
    std::vector<Mojo> out = sizes;
    for (std::size_t i = first; i < n; ++i) out[i] = sized(lo, i);
    return out;
}

/// [TIERFIRST 2026-10-31] fit_tier_sizes_to_pool, and then the kept tiers'
/// growth shrinks further while their funding cost passes `budget` (when
/// known).  `cost(i, size)` is what tier i would cost the funding asset at
/// `size` (an ask its base mojos, a bid its quote at its own price, rounded
/// up), nondecreasing in size.  The pool the sizes are fitted to is lowered,
/// never below the kept tiers' minimums, to the largest whose sizes the
/// budget pays for.  When the kept minimums alone cost more than the budget
/// the pool fit is returned and the funding fit skips tiers as before.
template <class CostFn>
[[nodiscard]] std::vector<Mojo> fit_tier_sizes_to_pool_and_budget(
    const std::vector<Mojo>& sizes, const std::vector<Mojo>& mins,
    const std::vector<Mojo>& weights, Mojo pool, std::optional<Mojo> budget,
    CostFn cost)
{
    std::vector<Mojo> out = fit_tier_sizes_to_pool(sizes, mins, weights, pool);
    const std::size_t n = sizes.size();
    if (!budget.has_value() || *budget < 0 || mins.size() != n || pool <= 0) return out;
    const std::vector<Mojo> wide_mins(mins.rbegin(), mins.rend());
    const std::size_t keep = tiers_within_budget(wide_mins, pool);
    if (keep == 0) return out;
    const std::size_t first = n - keep;
    auto kept_cost_fits = [&](const std::vector<Mojo>& v) {
        Mojo sum = 0;
        for (std::size_t i = first; i < n; ++i) {
            const Mojo c = cost(i, v[i]);
            if (c < 0 || sum > *budget - c) return false;
            sum += c;
        }
        return true;
    };
    if (kept_cost_fits(out)) return out;
    Mojo lo = 0;  // the kept minimums
    for (std::size_t i = first; i < n; ++i) lo += mins[i];
    if (!kept_cost_fits(fit_tier_sizes_to_pool(sizes, mins, weights, lo))) return out;
    Mojo hi = pool;  // does not fit
    while (hi - lo > 1) {
        const Mojo mid = lo + (hi - lo) / 2;
        if (kept_cost_fits(fit_tier_sizes_to_pool(sizes, mins, weights, mid))) lo = mid;
        else hi = mid;
    }
    return fit_tier_sizes_to_pool(sizes, mins, weights, lo);
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

/// [TIERMIN 2026-10-31] Whether a side the deploy-idle floor has just lifted
/// to the pair minimum counts as "floored" for `pair_side_pools`: a side the
/// drift guard tapered (0 < scale < 1), or a side with no pool at all before
/// the floor.  The strategy turned that second one off -- on 2026-10-31
/// XCH/DBX held 104.8 XCH against q_max 100, so Step 6 sized its bid 0 --
/// and the floor only puts back a token offer.  Left at 0, side_pools would
/// already size the other side from its own pool (its one-sided recovery);
/// the token offer must not take that away.  It did: the 1 XCH bid cut the
/// asks' 104 XCH pool to 1 XCH, one ask tier.  A side the strategy sized
/// small but not 0, with no drift taper, keeps the pairing.
[[nodiscard]] constexpr bool floored_side_unpairs(Mojo pool_before_floor,
                                                  double drift_scale) noexcept
{
    return drift_scale < 1.0 || pool_before_floor <= 0;
}

/// Whether Step 7 sizes a pair's two ladder sides from the smaller pool
/// (`LiquidityEngine::side_pools` pairing) or each from its own.  Pairing
/// stays the rule, except on a pair posting tier minimums where exactly one
/// side was lifted to the pair minimum by the deploy-idle floor and
/// `floored_side_unpairs` holds for it: that 1-unit side would otherwise cut
/// the other side to one unit too, so it could post only its widest tier.
[[nodiscard]] constexpr bool pair_side_pools(bool tier_minimums_on,
                                             bool bid_guard_floored,
                                             bool ask_guard_floored) noexcept
{
    return !(tier_minimums_on && bid_guard_floored != ask_guard_floored);
}

// ---------------------------------------------------------------------------
// [WAITGROW 2026-11-01] Wait to grow.
//
// Overnight on 2026-10-31/11-01 the tiers on the XCH/DBX asks and the
// XCH/wUSDC.b bids grew past their minimums out of each side's share of the
// coin's CONFIRMED balance.  That balance counts the coin locked in our own
// resting offers, and the change locked with them (an offer locks whole
// coins).  The wider tiers posted first and took the coin, and the tight
// tiers -- the ones most likely to fill -- then found too little free coin
// to post at all: the XCH/DBX asks' tiers 2-5 held 33.8 XCH, 19 of it
// growth, while tier 1 at 17.6 XCH was refused.  The operator approved
// (2026-11-01): every tier stays at its minimum until the side's whole
// minimum ladder fits the coin it can actually lock, and only what is left
// grows them; and a resting tier grown past its plan gives its coin back
// when a planned tier cannot be funded.
// ---------------------------------------------------------------------------

/// [WAITGROW] The coin a posting cycle must leave free beyond an XCH-funded
/// offer of `largest` mojos for the offer to post at all.  The coin-lock
/// ledger lets one cycle lock at most `commit_frac` of the free XCH above
/// the fee reserve (strategy.xch_cycle_commit_frac), so the offer needs
/// largest / commit_frac free: largest * (1 / commit_frac - 1) beyond
/// itself, rounded up.  0 when the fraction does not cap (1 or more), when
/// it stops spend-side posting outright (0 or less), or with no offer.
[[nodiscard]] inline Mojo cycle_cap_headroom(Mojo largest, double commit_frac) noexcept
{
    if (largest <= 0 || !std::isfinite(commit_frac) || commit_frac <= 0.0
        || commit_frac >= 1.0) {
        return 0;
    }
    const long double h = static_cast<long double>(largest)
                        * (1.0L / static_cast<long double>(commit_frac) - 1.0L);
    const long double top = static_cast<long double>(std::numeric_limits<Mojo>::max());
    if (!(h < top)) return std::numeric_limits<Mojo>::max();
    return static_cast<Mojo>(std::ceil(h));
}

/// [WAITGROW] One side's tier sizes held to `lockable`, the coin (in the
/// funding asset's mojos) the side can lock for its ladder, minimums first.
/// `sizes` are the tiers after fit_tier_sizes_to_pool_and_budget, `mins`,
/// `weights` and `pool` as there, and `cost(i, size)` what tier i would lock
/// at `size`, nondecreasing in size.  The tiers the pool keeps (the most of
/// the side's widest whose minimums fit it) are held to `lockable`:
///   * when their costs fit, the sizes are returned unchanged;
///   * when their minimums fit but their sizes do not, their growth shrinks,
///     shared out by weight as in fit_tier_sizes_to_pool, to the most whose
///     costs fit;
///   * when even their minimums do not fit, every one of them is held at its
///     minimum, so no tier grows before the whole minimum ladder fits.
/// The tighter tiers in front of them are left as they are, for the funding
/// fit to skip.  `held_at_minimums`, when given, says whether the last case
/// applied.
template <class CostFn>
[[nodiscard]] std::vector<Mojo> hold_growth_to_lockable(
    const std::vector<Mojo>& sizes, const std::vector<Mojo>& mins,
    const std::vector<Mojo>& weights, Mojo pool, Mojo lockable, CostFn cost,
    bool* held_at_minimums = nullptr)
{
    if (held_at_minimums != nullptr) *held_at_minimums = false;
    const std::size_t n = sizes.size();
    if (mins.size() != n || weights.size() != n || pool <= 0 || lockable < 0) return sizes;
    const std::vector<Mojo> wide_mins(mins.rbegin(), mins.rend());
    const std::size_t keep = tiers_within_budget(wide_mins, pool);
    if (keep == 0) return sizes;
    const std::size_t first = n - keep;  // the kept tiers are [first, n)
    for (std::size_t i = first; i < n; ++i) {
        if (mins[i] < 0 || sizes[i] < mins[i] || weights[i] < 0) return sizes;
    }
    auto kept_cost_fits = [&](const std::vector<Mojo>& v) {
        Mojo sum = 0;
        for (std::size_t i = first; i < n; ++i) {
            const Mojo c = cost(i, v[i]);
            if (c < 0 || sum > lockable - c) return false;
            sum += c;
        }
        return true;
    };
    if (kept_cost_fits(sizes)) return sizes;

    std::vector<Mojo> at_mins = sizes;
    for (std::size_t i = first; i < n; ++i) at_mins[i] = mins[i];
    if (!kept_cost_fits(at_mins)) {
        if (held_at_minimums != nullptr) *held_at_minimums = true;
        return at_mins;
    }

    // The kept tiers' total, saturating.  At their minimums they fit and at
    // their sizes they do not, so the most that fits lies between.  Any pool
    // in that range keeps the same tiers in fit_tier_sizes_to_pool (it holds
    // their minimums, and the sizes already fit the side's pool), whose
    // sizes rise with the pool, and so do their costs.
    auto kept_total = [&](const std::vector<Mojo>& v) {
        Mojo sum = 0;
        for (std::size_t i = first; i < n; ++i) {
            sum = (sum > std::numeric_limits<Mojo>::max() - v[i])
                ? std::numeric_limits<Mojo>::max() : sum + v[i];
        }
        return sum;
    };
    Mojo lo = kept_total(at_mins);  // fits
    Mojo hi = kept_total(sizes);    // does not
    while (hi - lo > 1) {
        const Mojo mid = lo + (hi - lo) / 2;
        if (kept_cost_fits(fit_tier_sizes_to_pool(sizes, mins, weights, mid))) lo = mid;
        else hi = mid;
    }
    std::vector<Mojo> out = fit_tier_sizes_to_pool(sizes, mins, weights, lo);
    return kept_cost_fits(out) ? out : at_mins;
}

/// [WAITGROW] Whether the wallet locks more than twice what our offers on a
/// coin hold (`offers`, their amounts; `locked`, confirmed less spendable
/// less pending change): the coin is in a few large coins, each offer locks
/// one whole, and a smaller repost would lock the same coin again, so a
/// reclaim would only trade which tiers rest.  Never with nothing locked.
[[nodiscard]] constexpr bool whole_coins_lock_the_coin(Mojo offers, Mojo locked) noexcept
{
    if (locked <= 0) return false;
    if (offers <= 0) return true;
    return locked - offers > offers;
}

/// [WAITGROW] One resting offer a reclaim may retire, in the mojos of the
/// coin it spends: what it locks now, and what its tier would lock at this
/// cycle's planned size (0 when the tier is not planned).
struct RestingTier {
    Mojo locked{0};
    Mojo planned{0};
};

/// [WAITGROW] Which resting offers to retire so that a coin's planned tiers
/// with no offer can be funded.  `missing` holds what each of those tiers
/// would lock at its planned size, and `free_coin` what the coin has free.
/// For XCH, `commit_frac` is strategy.xch_cycle_commit_frac: one posting
/// cycle may lock only that fraction of what is free, so the largest offer
/// still to post needs its cycle_cap_headroom free besides; pass 1 for a CAT.
/// A retired offer gives back what it locks and must be posted again at its
/// planned size, so it counts as one more tier to fund (and may be the
/// largest).  Candidates are the resting offers that lock more than
/// `tolerance` (a fraction) past their planned size -- their growth --
/// largest excess first.  The largest replacement sets the cycle-cap room,
/// so a pick can widen the gap on its own yet close it with others: for each
/// cap on the largest replacement (each candidate's planned size), the
/// candidates under it are taken largest excess first until the gap closes,
/// and the fewest picks win (on a tie, the set that leaves the most coin
/// over).  The most missing
/// tiers that can be funded, widest (smallest) first, are the target.
/// Nothing is picked when the free coin already funds every missing tier a
/// reclaim could, when no reclaim funds even the widest one (a reclaim that funds nothing
/// would only shrink the book), or when `commit_frac` is not above 0 (the
/// lock ledger then posts nothing spent from the coin, a replacement
/// included).  An offer on a tier with no plan is left to the caller.
/// Returns indices into `resting`, largest excess first.
[[nodiscard]] inline std::vector<std::size_t> tiers_to_reclaim(
    const std::vector<Mojo>& missing, Mojo free_coin, double commit_frac,
    const std::vector<RestingTier>& resting, double tolerance)
{
    std::vector<Mojo> wanted;
    for (const Mojo m : missing) {
        if (m > 0) wanted.push_back(m);
    }
    if (wanted.empty()) return {};
    // A fraction of 0 (or NaN, which the ledger reads as 0) posts nothing
    // spent from the coin: neither the missing tiers nor a replacement.
    if (!(commit_frac > 0.0)) return {};
    std::stable_sort(wanted.begin(), wanted.end());  // widest (smallest) first
    const long double free_now = static_cast<long double>(std::max<Mojo>(free_coin, 0));

    const long double tol = (std::isfinite(tolerance) && tolerance > 0.0)
        ? static_cast<long double>(tolerance) : 0.0L;
    std::vector<std::size_t> grown;
    for (std::size_t i = 0; i < resting.size(); ++i) {
        const auto& r = resting[i];
        if (r.planned <= 0 || r.locked <= r.planned) continue;
        const long double excess = static_cast<long double>(r.locked - r.planned);
        if (excess > tol * static_cast<long double>(r.planned)) grown.push_back(i);
    }
    std::stable_sort(grown.begin(), grown.end(), [&](std::size_t a, std::size_t b) {
        return resting[a].locked - resting[a].planned
             > resting[b].locked - resting[b].planned;
    });

    // What the first `count` wanted tiers, plus the picked replacements, need
    // beyond what is free and what the picks give back.
    auto shortfall = [&](std::size_t count, long double picked_planned,
                         long double picked_locked, Mojo largest) {
        long double need = picked_planned;
        for (std::size_t k = 0; k < count; ++k) {
            need += static_cast<long double>(wanted[k]);
            largest = std::max(largest, wanted[k]);
        }
        need += static_cast<long double>(cycle_cap_headroom(largest, commit_frac));
        return need - free_now - picked_locked;
    };
    if (!(shortfall(wanted.size(), 0.0L, 0.0L, 0) > 0.0L)) return {};

    // Each cap on the largest replacement, smallest first.  Every candidate
    // under a cap gives back more than it costs once the cap's room is paid,
    // so if any set whose largest replacement is the cap closes the gap, all
    // the candidates under it do: the sweep finds a set whenever one exists,
    // and taking them largest excess first keeps it short.
    std::vector<Mojo> caps;
    for (const std::size_t i : grown) caps.push_back(resting[i].planned);
    std::sort(caps.begin(), caps.end());
    caps.erase(std::unique(caps.begin(), caps.end()), caps.end());

    for (std::size_t count = wanted.size(); count > 0; --count) {
        // No reclaim funds more tiers than these, and the free coin funds
        // these already: retiring anything would buy nothing.
        if (!(shortfall(count, 0.0L, 0.0L, 0) > 0.0L)) return {};
        std::vector<std::size_t> best;
        long double best_gap = 0.0L;
        bool found = false;
        for (const Mojo cap : caps) {
            std::vector<std::size_t> out;
            long double planned_sum = 0.0L;
            long double locked_sum = 0.0L;
            Mojo largest = 0;
            bool closed = false;
            long double gap = 0.0L;
            for (const std::size_t i : grown) {
                const auto& r = resting[i];
                if (r.planned > cap) continue;
                out.push_back(i);
                planned_sum += static_cast<long double>(r.planned);
                locked_sum += static_cast<long double>(r.locked);
                largest = std::max(largest, r.planned);
                gap = shortfall(count, planned_sum, locked_sum, largest);
                if (!(gap > 0.0L)) {
                    closed = true;
                    break;
                }
            }
            if (closed
                && (!found || out.size() < best.size()
                    || (out.size() == best.size() && gap < best_gap))) {
                best = std::move(out);
                best_gap = gap;
                found = true;
            }
        }
        if (found) return best;
    }
    return {};
}

/// [WAITGROW-MIN 2026-11-01] How many blocks a tier's last resting offer
/// still sizes its replacement after that offer left the book.  A refresh
/// whose replacement could not post while the old offer still held its coin
/// posts it once the cancel lands, a block or two later, at the size the
/// tier had; a tier missing for longer than this starts at its minimum.
inline constexpr std::uint32_t kReplacementSizeBlocks = 3;

/// [WAITGROW-MIN 2026-11-01] The size a planned tier with no resting offer
/// starts at under wait to grow: its minimum, so a missing tier never grows
/// out of the coin the side's resting offers hold.  On 2026-11-01 the
/// XCH/DBX 10 XCH tight ask, refused by the XCH lock cap at 10.5 XCH, kept
/// growing (to 12.1 XCH) while it waited.  A tier whose own offer left the
/// book within kReplacementSizeBlocks keeps that offer's size instead (never
/// above the plan, never below the minimum): a refresh replacement deferred
/// until its cancel lands is not a first placement, and resetting it to the
/// minimum would keep every deferred refresh from ever growing.
[[nodiscard]] inline Mojo start_size_without_offer(Mojo planned, Mojo min,
                                                   std::optional<Mojo> recent_offer)
{
    if (planned <= min) return planned;
    if (!recent_offer.has_value()) return min;
    return std::clamp(*recent_offer, min, planned);
}

}  // namespace xop::strategy::tiermin

#endif  // XOP_STRATEGY_TIER_MINIMUMS_HPP
