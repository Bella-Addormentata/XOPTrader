// ---------------------------------------------------------------------------
// bbo_sanity.hpp -- side-aware BBO proximity routing for Step 8 tiers.
//
// [ALWAYSOFFER 2026-08-30] The original check used one symmetric constant
// (0.10) for both directions, which conflated two very different risks:
//
//  * AGGRESSIVE deviation (ask below best ask / bid above best bid, past
//    the BBO midpoint): the quote would EXECUTE at a dislocated price --
//    the 2026-08-01 microprice-sweep incident class. Cap stays tight.
//
//  * PASSIVE deviation (ask above the book / bid at-or-below the BBO
//    midpoint): the quote merely RESTS far from a thin book. A
//    cost-basis-floored ask 20% above a crashed book cannot fill below
//    basis+margin -- suppressing it guarantees zero participation for
//    zero protection. Live case: XCH/BYC basis ~11.68 BYC/XCH vs a book
//    at 9.75 during the 2026-08-30 XCH repricing; the symmetric check
//    kept the pair permanently empty. Cap is wide and configurable.
//
// The bid passive rule is deliberately "at or below the BBO midpoint",
// not "below best bid": with extreme sigma the width floor prices bids
// under the (dust) best bid, and as vol decays the bid walks up toward
// mid -- both are resting positions the book can trade INTO, mirroring
// the competitive anchor's own bid cap at bbo_ref.
// ---------------------------------------------------------------------------

#ifndef XOP_STRATEGY_BBO_SANITY_HPP
#define XOP_STRATEGY_BBO_SANITY_HPP

#include <algorithm>   // std::min, in the passive safe-harbor branch below.
                       // Was missing: the header compiled only because every
                       // existing includer happened to pull <algorithm> in
                       // first, which is the include-order-dependent break
                       // that shows up on one toolchain and not the other.
#include <cmath>

namespace xop::strategy {

enum class BboVerdict {
    Pass,                 ///< within bounds for its direction.
    SuppressAggressive,   ///< would execute at a dislocated price.
    SuppressPassive,      ///< rests absurdly far even for a thin book.
};

/// Route one tier. `is_ask` names the side; prices must share units and
/// be positive; `bbo_same_side` is best ask for asks, best bid for bids;
/// `bbo_mid` is (best_bid+best_ask)/2. Non-finite or non-positive inputs
/// pass (the caller's upstream validity gates own those).
[[nodiscard]] inline BboVerdict classify_tier(
    bool is_ask,
    double tier_price,
    double bbo_same_side,
    double bbo_mid,
    double max_aggressive_dev,
    double max_passive_dev) noexcept
{
    if (!(tier_price > 0.0) || !(bbo_same_side > 0.0)
        || !std::isfinite(tier_price) || !std::isfinite(bbo_same_side)) {
        return BboVerdict::Pass;
    }
    const double dev =
        std::fabs(tier_price - bbo_same_side) / bbo_same_side;

    const bool passive =
        is_ask ? (tier_price >= bbo_same_side)
               : (std::isfinite(bbo_mid) && bbo_mid > 0.0
                      ? tier_price <= bbo_mid
                      : tier_price <= bbo_same_side);

    if (passive) {
        // A passive tier has TWO safe harbors: near its own side's BBO,
        // or (bids) near the midpoint from below. Measuring a
        // mid-adjacent bid against a DUST best bid reads every sane
        // resting price as hundreds of percent off -- the reference that
        // matters is whichever it is actually anchored to.
        double best_dev = dev;
        if (!is_ask && std::isfinite(bbo_mid) && bbo_mid > 0.0
            && tier_price <= bbo_mid) {
            best_dev = std::min(
                best_dev, (bbo_mid - tier_price) / bbo_mid);
        }
        return best_dev > max_passive_dev ? BboVerdict::SuppressPassive
                                          : BboVerdict::Pass;
    }
    return dev > max_aggressive_dev ? BboVerdict::SuppressAggressive
                                    : BboVerdict::Pass;
}

}  // namespace xop::strategy

#endif  // XOP_STRATEGY_BBO_SANITY_HPP
