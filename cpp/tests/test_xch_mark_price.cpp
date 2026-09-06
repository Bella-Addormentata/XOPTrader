// ---------------------------------------------------------------------------
// [S33 2026-09-05] The XCH mark handed to PnLTracker::mark_to_market.
//
// [XCH-MTM-ISOLATION 2026-09-04] XCH is the wallet's reserve currency; its USD
// value is known authoritatively from CEX/anchors, so the mark must not be
// derived from an individual CAT book's local DEX spread (wide BYC vs tight
// DBX moved the operator-visible figure by -$9 to -$12 and tripped Step 13's
// rolling-window breaker).
//
// [S33] The remaining bug was the DIVISOR.  mark_to_market converts both this
// price and the cost basis back to USD with the factor step_update_pnl
// REGISTERED for the pair; dividing here by the LIVE factor therefore yielded
// xch_usd * registered/live instead of canonical XCH USD, and the mark hopped
// between pairs exactly as before the isolation.  The registered factor is the
// carried last-trusted one whenever the live factor is ungraded.
//
// MUTATION CHECK, and its reach.  Inside this function the defect is the
// division itself; restore
//
//     return static_cast<Mojo>(std::llround(
//         static_cast<double>(xch_usd_mojos) * registered_factor));
//
// or drop the conversion and return xch_usd_mojos, and
// TheFactorIsADivisorNotAMultiplier / CarriedAndLiveFactorsGiveDifferentMarks
// go red.  WHICH factor the caller passes is a call-site property: reinstating
// `quote_usd_factor(*pc)` in step_update_pnl is not killable from here (no test
// constructs an Engine).  CarriedAndLiveFactorsGiveDifferentMarks pins the
// thing that makes it matter -- that the two factors are NOT interchangeable,
// so the choice is load-bearing rather than cosmetic -- with the live numbers
// from the incident.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <limits>

#include "xop/engine.hpp"
#include "xop/types.hpp"

using xop::Mojo;
using xop::xch_mark_price_mojos;

namespace {

// 1 XCH = $1.50, expressed the way asset_usd_pseudo_price returns it.
constexpr Mojo kXchUsdMojos = 1'500'000'000'000;   // 1.5 * kMojosPerXch

TEST(XchMarkPrice, TheFactorIsADivisorNotAMultiplier)
{
    // A quote unit worth $0.50: one XCH is 3 quote units.
    EXPECT_EQ(xch_mark_price_mojos(kXchUsdMojos, 0.5),
              Mojo{3'000'000'000'000});
    // A quote unit worth $1.00 (a par stable): the mark IS the USD price.
    EXPECT_EQ(xch_mark_price_mojos(kXchUsdMojos, 1.0), kXchUsdMojos);
}

// The reason the carried/live distinction is load-bearing: on the live pair
// the two factors differ, so passing the wrong one moves the operator-visible
// figure.  XCH/BYC is par (carried == live == 1.0); XCH/DBX derives its factor
// from its own mid, and an ungraded tick leaves the carried value behind.
TEST(XchMarkPrice, CarriedAndLiveFactorsGiveDifferentMarks)
{
    constexpr double carried_factor = 0.0137;   // last TRUSTED DBX/USD
    constexpr double live_factor    = 0.0125;   // this tick's ungraded mid

    const Mojo with_carried = xch_mark_price_mojos(kXchUsdMojos,
                                                   carried_factor);
    const Mojo with_live    = xch_mark_price_mojos(kXchUsdMojos, live_factor);

    EXPECT_NE(with_carried, with_live)
        << "if these were interchangeable the S33 fix would be cosmetic";
    // mark_to_market divides the basis by the REGISTERED (carried) factor and
    // converts the result back with it, so only the carried mark round-trips
    // to canonical XCH USD.  The live one is off by carried/live -- ~9.6% on
    // these numbers, the scale of the -$9 to -$12 hopping that was observed.
    EXPECT_NEAR(static_cast<double>(with_live)
                    / static_cast<double>(with_carried),
                carried_factor / live_factor, 1e-6);
}

// A pair registered UNPRICEABLE has no entry in the carry map, which the
// caller passes as 0.  Returning 0 here means "do not mark", instead of
// falling through to that CAT book's own mid -- the valuation the isolation
// exists to keep out of the XCH mark.  See the PnLTracker-level test in
// test_pnl_tracker.cpp for why this is also INERT: the same missing factor
// zeroes that pair's basis, so it contributes nothing either way.
TEST(XchMarkPrice, AnUnpriceablePairMarksNothing)
{
    EXPECT_EQ(xch_mark_price_mojos(kXchUsdMojos, 0.0), Mojo{0});
}

// Defensive, and NaN-safe by construction: `> 0.0` is false for NaN, so a
// poisoned factor cannot produce an inf/NaN mark for llround to trap on.
TEST(XchMarkPrice, ANegativeOrNanFactorMarksNothing)
{
    EXPECT_EQ(xch_mark_price_mojos(kXchUsdMojos, -1.0), Mojo{0});
    EXPECT_EQ(xch_mark_price_mojos(
                  kXchUsdMojos,
                  std::numeric_limits<double>::quiet_NaN()),
              Mojo{0});
}

// No USD anchor for XCH yet (pre-first-fetch, or every anchor down): mark
// nothing rather than marking at zero, whatever the factor says.
TEST(XchMarkPrice, NoXchUsdAnchorMarksNothing)
{
    EXPECT_EQ(xch_mark_price_mojos(0, 1.0), Mojo{0});
    EXPECT_EQ(xch_mark_price_mojos(0, 0.0), Mojo{0});
}

}  // namespace
