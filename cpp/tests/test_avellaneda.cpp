// test_avellaneda.cpp -- Unit tests for the Avellaneda-Stoikov optimal
//                        market-making strategy (CHIA adaptation).
//
// Tests verify every formula from CHIA_MARKET_MAKER_STRATEGY.md Section 5
// against hand-computed expected values.  Floating-point comparisons use
// EXPECT_NEAR with tolerances chosen to exceed IEEE-754 double rounding
// but remain tight enough to catch formula errors.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/strategy/avellaneda.hpp>
#include <xop/strategy/glft.hpp>

#include <cmath>
#include <limits>

namespace {

// ============================================================================
// Avellaneda-Stoikov parameter fixture
// ============================================================================
//
// Default CHIA parameters from the strategy document:
//   S     = 2.70  (XCH price in wUSDC)
//   q     = 100   (net inventory, base-asset units)
//   gamma = 0.01  (risk aversion)
//   kappa = 1.5   (fill-intensity decay)
//   sigma = 0.05  (annualised or daily volatility, treated as a raw number)
//   tau   = 3120  seconds (60 blocks * 52 s)

class AvellanedaTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_.gamma             = 0.01;
        cfg_.kappa             = 1.5;
        cfg_.A                 = 100.0;
        cfg_.q_max             = 1000.0;
        cfg_.horizon_blocks    = 60;
        cfg_.block_time_seconds = 52.0;
        cfg_.regime_window_blocks = 100;
        cfg_.enable_no_loss_constraint = false;
        cfg_.min_margin_bps    = 35.0;
    }

    xop::AvellanedaConfig cfg_;
};

// ============================================================================
// TEST: Reservation price
// ============================================================================
//
// Formula (strategy doc S5, Key Formulas table):
//   r = S - q * gamma * sigma^2 * tau
//
// Hand computation with known values:
//   S     = 2.70
//   q     = 100
//   gamma = 0.01
//   sigma = 0.05
//   tau   = 3120.0  (60 blocks * 52 s)
//
//   sigma^2 = 0.0025
//   q * gamma * sigma^2 * tau = 100 * 0.01 * 0.0025 * 3120
//                             = 100 * 0.01 * 7.80
//                             = 100 * 0.078
//                             = 7.80
//   r = 2.70 - 7.80 = -5.10
//
// Note: a large negative reservation price is expected with these parameters
// because the inventory penalty (q=100 at gamma=0.01) is very large.  In
// production, sigma would be the per-block volatility (~0.001284), not 0.05,
// yielding a much milder adjustment.  We use sigma=0.05 here because the
// strategy doc uses that value in its example calibrations.

TEST_F(AvellanedaTest, ReservationPriceKnownValues) {
    xop::AvellanedaStoikov strategy(cfg_);

    const double S     = 2.70;
    const double sigma = 0.05;
    const double q     = 100.0;
    const double tau   = 3120.0;  // 60 blocks * 52 s

    const double r = strategy.reservation_price(S, sigma, q, tau);

    // Expected: 2.70 - 100 * 0.01 * 0.0025 * 3120 = 2.70 - 7.80 = -5.10
    EXPECT_NEAR(r, -5.10, 1e-10);
}

// Verify that positive inventory pushes reservation price below mid.
TEST_F(AvellanedaTest, ReservationPricePositiveInventory) {
    xop::AvellanedaStoikov strategy(cfg_);

    const double S     = 2.70;
    const double sigma = 0.05;
    const double tau   = 3120.0;

    // q > 0 (long) => r < S.
    const double r_long = strategy.reservation_price(S, sigma, 50.0, tau);
    EXPECT_LT(r_long, S);
}

// Verify that negative inventory pushes reservation price above mid.
TEST_F(AvellanedaTest, ReservationPriceNegativeInventory) {
    xop::AvellanedaStoikov strategy(cfg_);

    const double S     = 2.70;
    const double sigma = 0.05;
    const double tau   = 3120.0;

    // q < 0 (short) => r > S.
    const double r_short = strategy.reservation_price(S, sigma, -50.0, tau);
    EXPECT_GT(r_short, S);
}

// ============================================================================
// TEST: Optimal half-spread
// ============================================================================
//
// Formula (strategy doc S5):
//   delta = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
//
// Hand computation:
//   kappa = 1.5, gamma = 0.01, sigma = 0.05, tau = 3120.0
//
//   Term 1 = (1/1.5) * ln(1 + 1.5 / 0.01)
//          = 0.66667 * ln(151.0)
//          = 0.66667 * 5.01728
//          = 3.34485
//
//   Term 2 = 0.5 * 0.01 * 0.0025 * 3120
//          = 0.5 * 0.078
//          = 0.039
//
//   delta  = 3.34485 + 0.039 = 3.38385

TEST_F(AvellanedaTest, OptimalHalfSpreadKnownValues) {
    xop::AvellanedaStoikov strategy(cfg_);

    const double sigma = 0.05;
    const double tau   = 3120.0;

    const double delta = strategy.optimal_half_spread(sigma, tau);

    const double term1 = (1.0 / 1.5) * std::log(1.0 + 1.5 / 0.01);
    const double term2 = 0.5 * 0.01 * 0.0025 * 3120.0;
    const double expected = term1 + term2;

    EXPECT_NEAR(delta, expected, 1e-10);
    // Verify against the hand-computed value (rounded).
    EXPECT_NEAR(delta, 3.38385, 1e-4);
}

// Verify that the half-spread is always positive.
TEST_F(AvellanedaTest, OptimalHalfSpreadAlwaysPositive) {
    xop::AvellanedaStoikov strategy(cfg_);

    // Test with various sigma and tau values.
    EXPECT_GT(strategy.optimal_half_spread(0.01, 52.0), 0.0);
    EXPECT_GT(strategy.optimal_half_spread(0.10, 6240.0), 0.0);
    EXPECT_GT(strategy.optimal_half_spread(1e-6, 1.0), 0.0);
}

// ============================================================================
// TEST: Compute tau (remaining horizon in seconds)
// ============================================================================
//
// Formula:
//   tau = (N - (block_height mod N)) * block_time
//
//   N = 60 blocks, block_time = 52 s
//
//   block_height = 0 => n = 0, remaining = 60, tau = 60*52 = 3120
//   block_height = 30 => n = 30, remaining = 30, tau = 30*52 = 1560
//   block_height = 59 => n = 59, remaining = 1, tau = 1*52 = 52
//   block_height = 60 => n = 0 (rollover), remaining = 60, tau = 3120

TEST_F(AvellanedaTest, ComputeTauStartOfWindow) {
    xop::AvellanedaStoikov strategy(cfg_);
    EXPECT_NEAR(strategy.compute_tau(0), 3120.0, 1e-10);
}

TEST_F(AvellanedaTest, ComputeTauMiddle) {
    xop::AvellanedaStoikov strategy(cfg_);
    EXPECT_NEAR(strategy.compute_tau(30), 1560.0, 1e-10);
}

TEST_F(AvellanedaTest, ComputeTauEndOfWindow) {
    xop::AvellanedaStoikov strategy(cfg_);
    // block 59 => remaining = 1, tau = 52.
    EXPECT_NEAR(strategy.compute_tau(59), 52.0, 1e-10);
}

TEST_F(AvellanedaTest, ComputeTauRollover) {
    xop::AvellanedaStoikov strategy(cfg_);
    // block 60 => 60 mod 60 = 0, remaining = 60, tau = 3120.
    EXPECT_NEAR(strategy.compute_tau(60), 3120.0, 1e-10);
}

// Tau must never be zero (minimum is 1 block = 52 s).
TEST_F(AvellanedaTest, ComputeTauNeverZero) {
    xop::AvellanedaStoikov strategy(cfg_);

    for (uint32_t bh = 0; bh < 1000; ++bh) {
        EXPECT_GE(strategy.compute_tau(bh), 52.0);
    }
}

// ============================================================================
// TEST: Per-block volatility conversion
// ============================================================================
//
// Formula (strategy doc Key Formulas table, corrected in avellaneda.hpp):
//   sigma_block = sigma_annual * sqrt(block_time / seconds_per_year)
//
//   With block_time = 52 s, seconds_per_year = 31,536,000 (365 * 24 * 3600):
//     sqrt(52 / 31536000) = sqrt(1.64867e-6) = 0.001284...
//
//   sigma_annual = 0.50 => sigma_block = 0.50 * 0.001284 = 0.000642
//
// Note: the strategy doc quotes 0.000963 as the per-block factor; the header
// correctly identifies this as erroneous.  The mathematically exact value is
// sqrt(52 / 31,536,000) = 0.001284.

TEST_F(AvellanedaTest, PerBlockVolatilityConversion) {
    const double sigma_annual = 0.50;
    const double sigma_block = xop::AvellanedaStoikov::per_block_volatility(
        sigma_annual, 52.0);

    // Expected: 0.50 * sqrt(52 / 31536000)
    const double expected = 0.50 * std::sqrt(52.0 / (365.0 * 24.0 * 3600.0));
    EXPECT_NEAR(sigma_block, expected, 1e-10);
    EXPECT_NEAR(sigma_block, 0.000642, 1e-5);
}

// Identity: per-block vol with block_time = seconds_per_year should equal
// sigma_annual (the entire year is one "block").
TEST_F(AvellanedaTest, PerBlockVolatilityIdentity) {
    const double secs_per_year = 365.0 * 24.0 * 3600.0;
    const double sigma = xop::AvellanedaStoikov::per_block_volatility(
        1.0, secs_per_year);
    EXPECT_NEAR(sigma, 1.0, 1e-10);
}

// ============================================================================
// TEST: Fill intensity
// ============================================================================
//
// Formula (strategy doc Key Formulas table):
//   lambda(delta) = A * exp(-kappa * delta)
//
//   A = 100, kappa = 1.5
//
//   delta = 0   => lambda = 100 * exp(0)     = 100.0
//   delta = 1.0 => lambda = 100 * exp(-1.5)  = 100 * 0.22313 = 22.313

TEST_F(AvellanedaTest, FillIntensityAtZeroDelta) {
    xop::AvellanedaStoikov strategy(cfg_);
    EXPECT_NEAR(strategy.fill_intensity(0.0), 100.0, 1e-10);
}

TEST_F(AvellanedaTest, FillIntensityDecaysExponentially) {
    xop::AvellanedaStoikov strategy(cfg_);

    const double lambda = strategy.fill_intensity(1.0);
    const double expected = 100.0 * std::exp(-1.5);
    EXPECT_NEAR(lambda, expected, 1e-10);
    EXPECT_NEAR(lambda, 22.3130, 1e-3);
}

// Fill intensity is always positive and monotonically decreasing.
TEST_F(AvellanedaTest, FillIntensityMonotonicallyDecreasing) {
    xop::AvellanedaStoikov strategy(cfg_);

    double prev = strategy.fill_intensity(0.0);
    for (double delta = 0.1; delta <= 5.0; delta += 0.1) {
        double curr = strategy.fill_intensity(delta);
        EXPECT_GT(curr, 0.0);
        EXPECT_LT(curr, prev);
        prev = curr;
    }
}

// ============================================================================
// TEST: GLFT skew (Section 5)
// ============================================================================
//
// Formula:
//   skew = phi * q / q_max
//
//   phi = 0.5, q = 100, q_max = 1000
//   skew = 0.5 * 100 / 1000 = 0.05

TEST(GlftTest, InventorySkewKnownValues) {
    xop::GlftConfig gcfg;
    gcfg.phi   = 0.5;
    gcfg.q_max = 1000.0;

    xop::GlftStrategy glft(gcfg);

    EXPECT_NEAR(glft.inventory_skew(100.0),  0.05, 1e-10);
    EXPECT_NEAR(glft.inventory_skew(-100.0), -0.05, 1e-10);
    EXPECT_NEAR(glft.inventory_skew(0.0),    0.0, 1e-10);
}

// Skew should be linear in q and bounded by [-phi, phi].
TEST(GlftTest, InventorySkewBounded) {
    xop::GlftConfig gcfg;
    gcfg.phi   = 0.5;
    gcfg.q_max = 1000.0;

    xop::GlftStrategy glft(gcfg);

    // At q = q_max, skew = phi.
    EXPECT_NEAR(glft.inventory_skew(1000.0), 0.5, 1e-10);
    // At q = -q_max, skew = -phi.
    EXPECT_NEAR(glft.inventory_skew(-1000.0), -0.5, 1e-10);
}

// ============================================================================
// TEST: Never-sell-at-loss constraint (Section 8)
// ============================================================================
//
// Formula:
//   ask = max(optimal_ask, cost_basis * (1 + min_margin_bps / 10000))
//
// If cost_basis = 2.80, min_margin_bps = 35:
//   min_ask = 2.80 * (1 + 35/10000) = 2.80 * 1.0035 = 2.8098
//
// If optimal_ask = 2.75 (below cost), ask must be floored at 2.8098.
// If optimal_ask = 2.90 (above cost), ask remains 2.90.

TEST_F(AvellanedaTest, NoLossConstraintFloorsBelowCost) {
    cfg_.enable_no_loss_constraint = true;
    cfg_.min_margin_bps = 35.0;
    xop::AvellanedaStoikov strategy(cfg_);

    // Set cost basis so the constraint is active.
    strategy.set_cost_basis(2.80, 35.0);

    const double S     = 2.70;  // mid below cost => ask would be low
    const double sigma = 0.001; // small vol => tight spread
    const double q     = 0.0;   // no inventory penalty
    // tau at block 0 = 3120 s
    auto quotes = strategy.compute_quotes(S, sigma, q, 0);

    // The unconstrained ask would be approximately S + delta, which at
    // small sigma is close to S + (1/kappa)*ln(1+kappa/gamma).
    // With the no-loss constraint, ask must be >= 2.80 * 1.0035 = 2.8098.
    const double min_ask = 2.80 * (1.0 + 35.0 / 10000.0);
    EXPECT_GE(quotes.ask_price, min_ask - 1e-10);
}

TEST_F(AvellanedaTest, NoLossConstraintDoesNotAffectAboveCost) {
    cfg_.enable_no_loss_constraint = true;
    cfg_.min_margin_bps = 35.0;
    xop::AvellanedaStoikov strategy(cfg_);

    // Set cost basis well below the mid price.
    strategy.set_cost_basis(1.00, 35.0);

    const double S     = 2.70;
    const double sigma = 0.05;
    const double q     = 0.0;
    auto quotes = strategy.compute_quotes(S, sigma, q, 0);

    // The ask should be determined by the model (well above cost_basis).
    const double min_ask = 1.00 * (1.0 + 35.0 / 10000.0);
    EXPECT_GT(quotes.ask_price, min_ask);
}

TEST_F(AvellanedaTest, NoLossConstraintDisabledAllowsBelowCost) {
    cfg_.enable_no_loss_constraint = false;
    xop::AvellanedaStoikov strategy(cfg_);

    strategy.set_cost_basis(5.00, 35.0);  // cost basis above any likely ask

    const double S     = 2.70;
    const double sigma = 0.001;
    const double q     = 0.0;
    auto quotes = strategy.compute_quotes(S, sigma, q, 0);

    // With constraint disabled, ask can be below cost basis.
    // The unconstrained ask is approximately S + delta ~ 2.70 + 3.345 ~ 6.04
    // which happens to exceed 5.00 with these params.  Use extreme cost to test.
    strategy.set_cost_basis(100.0, 35.0);
    auto quotes2 = strategy.compute_quotes(S, sigma, q, 0);

    // Constraint disabled: ask is NOT floored at 100*(1+0.0035).
    EXPECT_LT(quotes2.ask_price, 100.0);
}

// ============================================================================
// EDGE CASE: Zero inventory (q = 0)
// ============================================================================
//
// When q = 0, the reservation price should equal the mid-price exactly
// (no inventory penalty), and the spread should be symmetric.

TEST_F(AvellanedaTest, ZeroInventorySymmetricQuotes) {
    xop::AvellanedaStoikov strategy(cfg_);

    const double S     = 2.70;
    const double sigma = 0.05;
    const double q     = 0.0;
    const double tau   = 3120.0;

    // r = S when q = 0.
    EXPECT_NEAR(strategy.reservation_price(S, sigma, q, tau), S, 1e-10);

    // Quotes should be symmetric around S.
    auto quotes = strategy.compute_quotes(S, sigma, q, 0);
    const double mid = (quotes.bid_price + quotes.ask_price) / 2.0;
    EXPECT_NEAR(mid, S, 1e-6);
}

// ============================================================================
// EDGE CASE: Zero volatility (sigma = 0)
// ============================================================================
//
// With sigma = 0:
//   - Reservation price r = S (no risk penalty).
//   - Half-spread term2 = 0, but term1 remains positive.
//   - Fill intensity is unchanged.

TEST_F(AvellanedaTest, ZeroVolatility) {
    xop::AvellanedaStoikov strategy(cfg_);

    const double S   = 2.70;
    const double tau = 3120.0;

    // r = S - q * gamma * 0 * tau = S, regardless of q.
    EXPECT_NEAR(strategy.reservation_price(S, 0.0, 500.0, tau), S, 1e-10);

    // delta = term1 + 0 = (1/kappa)*ln(1+kappa/gamma).
    const double term1 = (1.0 / 1.5) * std::log(1.0 + 1.5 / 0.01);
    EXPECT_NEAR(strategy.optimal_half_spread(0.0, tau), term1, 1e-10);
}

// ============================================================================
// EDGE CASE: Tau approaching minimum (end of rolling window)
// ============================================================================
//
// At block_height = 59 with horizon_blocks = 60:
//   tau = 1 * 52 = 52 s (minimum)
//
// The spread's volatility component (term2) should be minimal but nonzero.

TEST_F(AvellanedaTest, MinimumTauSmallSpread) {
    xop::AvellanedaStoikov strategy(cfg_);

    const double sigma = 0.05;
    const double tau_min = 52.0;

    const double delta_min = strategy.optimal_half_spread(sigma, tau_min);
    const double delta_max = strategy.optimal_half_spread(sigma, 3120.0);

    // Spread at minimum tau should be smaller than at maximum tau.
    EXPECT_LT(delta_min, delta_max);

    // Term2 at minimum: 0.5 * 0.01 * 0.0025 * 52 = 0.00065
    const double term2_min = 0.5 * 0.01 * 0.0025 * 52.0;
    EXPECT_NEAR(term2_min, 0.00065, 1e-10);
}

// ============================================================================
// TEST: compute_quotes end-to-end consistency
// ============================================================================
//
// Verify that bid < ask and spread_bps = 10000 * (ask - bid) / mid.

TEST_F(AvellanedaTest, QuotesEndToEndConsistency) {
    xop::AvellanedaStoikov strategy(cfg_);

    const double S     = 2.70;
    const double sigma = 0.05;
    const double q     = 50.0;
    auto quotes = strategy.compute_quotes(S, sigma, q, 30);

    EXPECT_LT(quotes.bid_price, quotes.ask_price);

    const double expected_spread_bps =
        10000.0 * (quotes.ask_price - quotes.bid_price) / S;
    EXPECT_NEAR(quotes.spread_bps, expected_spread_bps, 0.01);
}

// ============================================================================
// TEST: Size scaling with inventory
// ============================================================================
//
// bid_size = q_max * max(0, 1 - q/q_max)
// ask_size = q_max * max(0, 1 + q/q_max)
//
// q = q_max (fully long) => bid_size = 0, ask_size = 2*q_max.
// q = 0 (balanced)       => bid_size = q_max, ask_size = q_max.

TEST_F(AvellanedaTest, SizeScalingFullyLong) {
    xop::AvellanedaStoikov strategy(cfg_);

    auto quotes = strategy.compute_quotes(2.70, 0.05, 1000.0, 0);
    EXPECT_NEAR(quotes.bid_size, 0.0, 1e-10);
    EXPECT_NEAR(quotes.ask_size, 2000.0, 1e-10);
}

TEST_F(AvellanedaTest, SizeScalingBalanced) {
    xop::AvellanedaStoikov strategy(cfg_);

    auto quotes = strategy.compute_quotes(2.70, 0.05, 0.0, 0);
    EXPECT_NEAR(quotes.bid_size, 1000.0, 1e-10);
    EXPECT_NEAR(quotes.ask_size, 1000.0, 1e-10);
}

}  // namespace
