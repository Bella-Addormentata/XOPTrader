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

}  // namespace
