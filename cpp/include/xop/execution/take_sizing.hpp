#ifndef XOP_EXECUTION_TAKE_SIZING_HPP
#define XOP_EXECUTION_TAKE_SIZING_HPP
// ---------------------------------------------------------------------------
// take_sizing.hpp -- unit conversion for taker paths, extracted so it can be
//                    tested and so there is ONE copy of it.
//
// [S40 follow-up 2026-09-01] WHY THIS EXISTS
// ------------------------------------------
// Two facts about a competing offer, both easy to forget at a call site:
//
//   1. take_offer(offer_text, fee) has NO SIZE PARAMETER (chia_rpc.hpp:499).
//      A Chia offer is an atomic swap of exactly the coins the maker
//      committed; there is no partial fill. Any per-take size limit is
//      therefore a FILTER -- "does the whole offer fit?" -- and never a
//      clamp. std::min() on a take size bounds a log field and nothing else.
//      That was S40.
//
//   2. The two sides are denominated in DIFFERENT assets. The ingest says so
//      (market_data.cpp:1933-1937): "Bid-side offers are denominated in the
//      QUOTE asset ...; ask-side offers are in the BASE asset". So an ASK's
//      `size` is already base mojos, while a BID's `size` is quote mojos and
//      must be converted before it is compared against a base-denominated
//      cap, charged against a balance, or handed to record_taker_fill() --
//      which books its `base_mojos` argument as base.
//
// Step 9f got both right inline. Step 9e got both wrong: it clamped with
// std::min and treated a bid's quote size as base mojos, in the cap
// comparison, in the pre-balance cost estimate and in the ledger. On
// BYC/wUSDC.b the second error is invisible because base and quote
// denominations happen to be equal (1000 each); on any pair where they
// differ the cap is applied in the wrong unit.
//
// Rounding is UP, always, and deliberately. These numbers are what we must
// be able to pay and what we must fit under a cap; rounding a cost down
// invents affordability, and rounding a size down under-reports what we are
// about to lift. Both errors point at spending more than intended, which is
// the direction this file exists to close.
//
// Pure header: plain integers plus xop::types. No engine types, no asio, no
// spdlog. Driven directly by cpp/tests/test_take_sizing.cpp.
//
// WHAT THESE TESTS DO NOT COVER -- read this before treating a green suite as
// proof. test_take_sizing.cpp pins the ARITHMETIC in this file. It cannot
// reach the CALL SITES: nothing in cpp/tests constructs an Engine (TODO S36),
// so the Step 9e filter, the Step 9f filter and the Step 9 recovery taker's
// per-block budget are all UNGUARDED LINES. Delete any of those three filters
// and the suite stays green. The reason to move the maths here was to make
// the part that CAN be pinned actually pinned, and to stop 9e and 9f keeping
// two copies of it -- not to claim the call sites are covered.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdint>
#include <limits>

#include "xop/types.hpp"

namespace xop::execution {

// ---------------------------------------------------------------------------
// ceil_to_mojo -- round a positive real up to a Mojo, saturating.
//
// Returns 0 for anything that is not a positive finite number, so a NaN or a
// negative intermediate becomes "no size" rather than an arbitrary integer.
// Saturates at Mojo max rather than narrowing an out-of-range value, because
// narrowing a floating value that does not fit the target integer type is
// UNDEFINED -- and a saturated maximum is safe here: every caller compares
// the result against a cap it cannot exceed, so the saturated value declines.
// ---------------------------------------------------------------------------
[[nodiscard]] inline Mojo ceil_to_mojo(long double value) noexcept
{
    if (!std::isfinite(static_cast<double>(value)) || value <= 0.0L) {
        return 0;
    }
    const long double cap =
        static_cast<long double>(std::numeric_limits<Mojo>::max());
    if (value >= cap) {
        return std::numeric_limits<Mojo>::max();
    }
    return static_cast<Mojo>(std::ceil(value));
}

// ---------------------------------------------------------------------------
// Denomination normaliser. A non-positive mojos-per-unit is a broken pair
// config; treating it as 1 keeps the arithmetic defined and is what Step 9f
// has always done. It never widens a take: the cap is normalised the same way
// at the call site.
// ---------------------------------------------------------------------------
[[nodiscard]] inline long double mpu_or_one(std::int64_t mojos_per_unit) noexcept
{
    return static_cast<long double>(mojos_per_unit > 0 ? mojos_per_unit : 1);
}

// ---------------------------------------------------------------------------
// quote_cost_for_ask -- what lifting an ASK costs us, in QUOTE mojos.
//
// We buy `base_size` base mojos at `price`. Prices are carried scaled by
// kMojosPerXch regardless of the pair's denominations, so the conversion is
//
//     quote = base_size * price * quote_mpu / (base_mpu * kMojosPerXch)
//
// @return 0 when the inputs cannot produce a usable cost. Zero means DO NOT
//         TAKE; it is not "free".
// ---------------------------------------------------------------------------
[[nodiscard]] inline Mojo quote_cost_for_ask(Mojo         base_size,
                                             Mojo         price,
                                             std::int64_t base_mojos_per_unit,
                                             std::int64_t quote_mojos_per_unit) noexcept
{
    if (base_size <= 0 || price <= 0) return 0;
    return ceil_to_mojo(static_cast<long double>(base_size)
                        * static_cast<long double>(price)
                        * mpu_or_one(quote_mojos_per_unit)
                        / (mpu_or_one(base_mojos_per_unit)
                           * static_cast<long double>(kMojosPerXch)));
}

// ---------------------------------------------------------------------------
// base_size_for_bid -- how much BASE hitting a BID delivers, in base mojos.
//
// The bid's advertised `size` is QUOTE mojos (the ingest convention above),
// so this is the inverse of quote_cost_for_ask:
//
//     base = quote_size * kMojosPerXch * base_mpu / (price * quote_mpu)
//
// This is the number that must be compared against a base-denominated cap,
// charged against the base wallet, and recorded as record_taker_fill()'s
// base_mojos. Using the raw `size` there is a unit error, not a rounding one.
//
// @return 0 when the inputs cannot produce a usable size. Zero means DO NOT
//         TAKE.
// ---------------------------------------------------------------------------
[[nodiscard]] inline Mojo base_size_for_bid(Mojo         quote_size,
                                            Mojo         price,
                                            std::int64_t base_mojos_per_unit,
                                            std::int64_t quote_mojos_per_unit) noexcept
{
    if (quote_size <= 0 || price <= 0) return 0;
    return ceil_to_mojo(static_cast<long double>(quote_size)
                        * static_cast<long double>(kMojosPerXch)
                        * mpu_or_one(base_mojos_per_unit)
                        / (static_cast<long double>(price)
                           * mpu_or_one(quote_mojos_per_unit)));
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_TAKE_SIZING_HPP
