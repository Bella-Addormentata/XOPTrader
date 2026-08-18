// test_pnl_units.cpp -- Regression tests for the canonical
// xop::quote_mojos_for() helper and the PnL unit convention it enforces.
//
// Background: v0.7.45 fixed a 1e9-inflation bug in realized- and inventory-PnL
// for XCH/wUSDC.b (and any pair where base_mojos_per_unit !=
// quote_mojos_per_unit).  The fix introduced a single source of truth in
// types.hpp.  These tests lock the formula in so that future refactors cannot
// regress to the broken (quote_denom / base_denom)-missing version.
//
// ISO/IEC 27001:2022 -- pure numerical verification, no secrets.
// ISO/IEC 5055       -- deterministic tests, no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/types.hpp>

#include <cmath>

namespace {

constexpr xop::Mojo kBaseXch  = xop::kMojosPerXch;          // 1e12
constexpr xop::Mojo kCatDenom = 1'000LL;                    // wUSDC.b / BYC

// --------------------------------------------------------------------------
// quote_mojos_for: XCH/wUSDC.b round-trip
//
// Sell 10 XCH at $2.50/XCH should yield 25 wUSDC.b = 25 * 1000 = 25,000
// quote-asset mojos.  Pre-0.7.45 the missing factor would have produced
// 25,000 * 1e9 = 2.5e13 mojos (the "billions of dollars" symptom).
// --------------------------------------------------------------------------
TEST(QuoteMojosFor, XchUsdcRoundTripMatchesExpectedQuoteMojos) {
    const double size_base   = 10.0 * static_cast<double>(kBaseXch);  // 10 XCH
    const double price_real  = 2.50;
    const double price_pseudo = price_real * static_cast<double>(kBaseXch);

    const double quote_mojos = xop::quote_mojos_for(
        size_base,
        price_pseudo,
        static_cast<double>(kBaseXch),
        static_cast<double>(kCatDenom));

    EXPECT_NEAR(quote_mojos, 25'000.0, 1e-6);
}

// --------------------------------------------------------------------------
// Realized PnL: sell at price > basis must produce positive PnL in
// QUOTE-asset mojos (not 1e9x inflated).  Sell 10 XCH at $2.50 with cost
// basis $2.00 should produce (2.50 - 2.00) * 10 = $5.00 = 5,000 wUSDC.b
// quote mojos.
// --------------------------------------------------------------------------
TEST(QuoteMojosFor, RealizedPnlMatchesHandComputation) {
    const xop::Mojo fill_price  = static_cast<xop::Mojo>(2.50 * kBaseXch);
    const xop::Mojo cost_basis  = static_cast<xop::Mojo>(2.00 * kBaseXch);
    const xop::Mojo fill_size   = 10 * kBaseXch;

    const double pnl = xop::quote_mojos_for(
        static_cast<double>(fill_size),
        static_cast<double>(fill_price - cost_basis),
        static_cast<double>(kBaseXch),
        static_cast<double>(kCatDenom));

    EXPECT_NEAR(pnl, 5'000.0, 1.0);
}

// --------------------------------------------------------------------------
// XCH/XCH-style pair (base_denom == quote_denom == 1e12) collapses to the
// legacy formula: pnl = (price - basis) * size / kMojosPerXch.
// --------------------------------------------------------------------------
TEST(QuoteMojosFor, SymmetricDenomCollapsesToLegacyFormula) {
    const double size  = 5.0 * static_cast<double>(kBaseXch);
    const double price = 1.50 * static_cast<double>(kBaseXch);
    const double pnl_helper = xop::quote_mojos_for(
        size, price,
        static_cast<double>(kBaseXch),
        static_cast<double>(kBaseXch));
    const double pnl_legacy = size * price / static_cast<double>(kBaseXch);
    EXPECT_NEAR(pnl_helper, pnl_legacy, 1.0);
}

// --------------------------------------------------------------------------
// Defensive: any non-positive denominator must return 0 instead of NaN/inf.
// --------------------------------------------------------------------------
TEST(QuoteMojosFor, NonPositiveDenomReturnsZero) {
    EXPECT_EQ(0.0,
        xop::quote_mojos_for(1.0, 1.0, 0.0, 1.0));
    EXPECT_EQ(0.0,
        xop::quote_mojos_for(1.0, 1.0, 1.0, 0.0));
    EXPECT_EQ(0.0,
        xop::quote_mojos_for(1.0, 1.0, -1.0, 1.0));
}

// --------------------------------------------------------------------------
// CAT/CAT pair (BYC/wUSDC.b): both denoms are 1e3.  Selling 100 BYC at
// $1.001/BYC should yield ~100.1 wUSDC.b = 100,100 quote-asset mojos.
// --------------------------------------------------------------------------
TEST(QuoteMojosFor, CatCatPairProducesExpectedQuoteMojos) {
    const double size_base   = 100.0 * static_cast<double>(kCatDenom);  // 100 BYC
    const double price_real  = 1.001;
    const double price_pseudo = price_real * static_cast<double>(kBaseXch);

    const double quote_mojos = xop::quote_mojos_for(
        size_base,
        price_pseudo,
        static_cast<double>(kCatDenom),
        static_cast<double>(kCatDenom));

    EXPECT_NEAR(quote_mojos, 100'100.0, 1.0);
}


// --------------------------------------------------------------------------
// affordable_base_mojos: the inverse-direction conversion (quote -> base).
//
// Background [S3, TODO.md, fixed 2026-08-18]: Step 7 subtracted the XCH fee
// reserve (1e12-scale XCH mojos) RAW from bid pools denominated in BASE
// mojos (1e3/unit on CAT-base pairs).  On wmilliETH.b/XCH a 0.5 XCH reserve
// (5e11 "mojos") annihilated any realistic pool every heartbeat, so the
// Avellaneda/GLFT sizing model was inert and only floor mechanisms quoted.
// The v0.7.38 XCH wallet cap on the same branch compared the same
// mismatched units and could never fire.  These tests lock the conversion
// in with the live pair's real numbers.
// --------------------------------------------------------------------------

TEST(AffordableBaseMojos, WmilliEthFeeReserveConvertsSanely) {
    // 0.5 XCH reserve, mid 1.39 XCH per wmilliETH.b, CAT base (1e3/unit):
    // (5e11 / 1e12) / 1.39 * 1e3 = 359.7 -> 360 base mojos (~0.36 units).
    const auto reserve_base = xop::reserve_base_mojos(
        5e11, static_cast<double>(kBaseXch), 1.39,
        static_cast<double>(kCatDenom));
    EXPECT_EQ(reserve_base, 360);  // ceil(359.71): never under-reserve

    // The affordability direction FLOORS the same quantity: a cap must
    // never admit a mojo the wallet cannot back.
    EXPECT_EQ(xop::affordable_base_mojos(
                  5e11, static_cast<double>(kBaseXch), 1.39,
                  static_cast<double>(kCatDenom)),
              359);

    // The pre-fix arithmetic subtracted 5e11 -- nine orders of magnitude
    // larger than the correct figure, and larger than any realistic pool.
    EXPECT_LT(reserve_base, 1'000);
}

TEST(AffordableBaseMojos, XchWalletCapNowReachable) {
    // 83.499 XCH confirmed at mid 1.39: the wallet can back
    // (83.499 / 1.39) * 1e3 = 60,071 base mojos (~60 units).  Pre-fix the
    // cap compared a 1e3-scale pool against 8.35e13 and never fired.
    const auto cap = xop::affordable_base_mojos(
        83.499 * static_cast<double>(kBaseXch),
        static_cast<double>(kBaseXch), 1.39,
        static_cast<double>(kCatDenom));
    EXPECT_NEAR(static_cast<double>(cap), 60'071.0, 1.0);
}

TEST(AffordableBaseMojos, MatchesTheCatQuoteCapFormula) {
    // The CAT-quote bid cap used quote_units / mid * base_denom inline;
    // the helper must reproduce it exactly.  38.32 wUSDC.b (1e3/unit) at
    // mid 2.50 wUSDC per XCH, XCH base (1e12/unit):
    // (38'320 / 1000) / 2.5 * 1e12 = 15.328 XCH in mojos.
    const auto cap = xop::affordable_base_mojos(
        38'320.0, static_cast<double>(kCatDenom), 2.5,
        static_cast<double>(kBaseXch));
    // Affordability FLOORS, and the double intermediate may land a hair
    // under the exact 15.328e12 -- one mojo low is the safe direction, one
    // mojo HIGH would claim funds the wallet does not have.
    EXPECT_GE(cap, 15'327'999'999'999LL);
    EXPECT_LE(cap, 15'328'000'000'000LL);
}

TEST(AffordableBaseMojos, OverflowReadsAsUnavailable) {
    // llround is UB outside int64; a dust-priced mid on a large balance
    // can push the quotient past 9.2e18.  Out-of-range must read as 0
    // ("conversion unavailable") so call sites skip -- a cap or reserve
    // must never be produced by undefined behaviour.
    const auto cap = xop::affordable_base_mojos(
        9e18, static_cast<double>(kBaseXch), 1e-9,
        static_cast<double>(kBaseXch));
    EXPECT_EQ(cap, 0);
    EXPECT_EQ(xop::reserve_base_mojos(
                  9e18, static_cast<double>(kBaseXch), 1e-9,
                  static_cast<double>(kBaseXch)),
              0);
}

TEST(AffordableBaseMojos, UnavailableConversionReturnsZero) {
    // A missing/invalid mid or denomination must return 0, which every
    // call site treats as "skip" -- a cap of 0 must never zero a pool,
    // and a reserve of 0 must never be silently enormous.
    const double q = 5e11, qd = 1e12, bd = 1e3;
    EXPECT_EQ(xop::affordable_base_mojos(q, qd, 0.0,  bd), 0);
    EXPECT_EQ(xop::affordable_base_mojos(q, qd, -1.0, bd), 0);
    EXPECT_EQ(xop::affordable_base_mojos(q, 0.0, 1.39, bd), 0);
    EXPECT_EQ(xop::affordable_base_mojos(q, qd, 1.39, 0.0), 0);
    EXPECT_EQ(xop::reserve_base_mojos(q, qd, 0.0, bd), 0);
    EXPECT_EQ(xop::reserve_base_mojos(q, 0.0, 1.39, bd), 0);
}

TEST(AffordableBaseMojos, NoOverflowAtRealisticExtremes) {
    // 9,000 XCH balance, a dust-priced CAT (mid 1e-6 XCH/unit), CAT base:
    // (9e15/1e12) / 1e-6 * 1e3 = 9e12 base mojos -- large but exact in
    // double and well inside int64.
    const auto cap = xop::affordable_base_mojos(
        9e15, static_cast<double>(kBaseXch), 1e-6,
        static_cast<double>(kCatDenom));
    EXPECT_EQ(cap, static_cast<xop::Mojo>(9e12));
}

}  // namespace
