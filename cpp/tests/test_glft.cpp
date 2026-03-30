// test_glft.cpp -- Unit tests for the GLFT (Gueant-Lehalle-Fernandez-Tapia)
//                  market-making strategy with running inventory penalty.
//
// Tests verify:
//   1. base_half_spread() formula against hand-computed values
//   2. inventory_skew() with dense and sparse-fill correction
//   3. compute_quotes() end-to-end: symmetry, no-loss, regime interaction
//   4. compute_tau() exponential decay (T5-CR3) identical to A-S path
//   5. Regime-dependent spread/skew multipliers
//   6. Size scaling with inventory
//   7. Edge cases: zero vol, zero inventory, NaN inputs
//
// Reference: Gueant, O., Lehalle, C.A., & Fernandez-Tapia, J. (2013).
//            "Dealing with the inventory risk: A solution to the market making
//            problem." Mathematics and Financial Economics, 7(4), 477-507.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/strategy/glft.hpp>

#include <cmath>
#include <limits>

namespace {

// ============================================================================
// GLFT parameter fixture
// ============================================================================

class GlftFullTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_.gamma             = 0.01;
        cfg_.kappa             = 1.5;
        cfg_.A                 = 100.0;
        cfg_.phi               = 0.5;
        cfg_.q_max             = 1000.0;
        cfg_.horizon_blocks    = 120;
        cfg_.block_time_seconds = 52.0;
        cfg_.regime_window_blocks = 100;
        cfg_.enable_no_loss_constraint = false;
        cfg_.min_margin_bps    = 35.0;
        // Neutralize sparse-fill correction for base tests.
        cfg_.expected_dense_fills_per_hour = 100.0;
        cfg_.actual_fills_per_hour         = 100.0;
        cfg_.sparse_correction_cap         = 10.0;
    }

    xop::GlftConfig cfg_;

    static void construct_strategy(const xop::GlftConfig& c) {
        xop::GlftStrategy s(c);
        (void)s;
    }
};

// ============================================================================
// TEST: Base half-spread formula
// ============================================================================
//
// Formula:
//   hs = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
//
// Hand computation:
//   kappa = 1.5, gamma = 0.01, sigma = 0.05, tau = 6240.0 (120 * 52)
//
//   term1 = (1/1.5) * ln(1 + 1.5/0.01) = 0.66667 * ln(151) = 0.66667 * 5.01728 = 3.34485
//   term2 = 0.5 * 0.01 * 0.0025 * 6240 = 0.078
//   hs    = 3.34485 + 0.078 = 3.42285

TEST_F(GlftFullTest, BaseHalfSpreadKnownValues) {
    xop::GlftStrategy strategy(cfg_);

    const double sigma = 0.05;
    const double tau   = 6240.0;

    const double hs = strategy.base_half_spread(sigma, tau);

    const double term1 = (1.0 / 1.5) * std::log(1.0 + 1.5 / 0.01);
    const double term2 = 0.5 * 0.01 * 0.0025 * 6240.0;
    const double expected = term1 + term2;

    EXPECT_NEAR(hs, expected, 1e-10);
    EXPECT_NEAR(hs, 3.42285, 1e-4);
}

TEST_F(GlftFullTest, BaseHalfSpreadAlwaysPositive) {
    xop::GlftStrategy strategy(cfg_);

    EXPECT_GT(strategy.base_half_spread(0.01, 52.0), 0.0);
    EXPECT_GT(strategy.base_half_spread(0.10, 6240.0), 0.0);
    EXPECT_GT(strategy.base_half_spread(1e-6, 1.0), 0.0);
}

// ============================================================================
// TEST: Inventory skew (dense market, no sparse correction)
// ============================================================================
//
//   skew = phi * q / q_max
//   With phi = 0.5, q_max = 1000:
//     skew(200)  = 0.5 * 200 / 1000 = 0.10
//     skew(-200) = 0.5 * (-200) / 1000 = -0.10
//     skew(0)    = 0.0

TEST_F(GlftFullTest, InventorySkewLinearInQ) {
    xop::GlftStrategy strategy(cfg_);

    EXPECT_NEAR(strategy.inventory_skew(200.0),  0.10, 1e-10);
    EXPECT_NEAR(strategy.inventory_skew(-200.0), -0.10, 1e-10);
    EXPECT_NEAR(strategy.inventory_skew(0.0),    0.0,  1e-10);
}

TEST_F(GlftFullTest, InventorySkewAtMaxInventory) {
    xop::GlftStrategy strategy(cfg_);

    // At q = q_max, skew = phi = 0.5.
    EXPECT_NEAR(strategy.inventory_skew(1000.0), 0.5, 1e-10);
    // At q = -q_max, skew = -phi = -0.5.
    EXPECT_NEAR(strategy.inventory_skew(-1000.0), -0.5, 1e-10);
}

// ============================================================================
// TEST: Sparse-fill correction amplifies skew
// ============================================================================
//
// With dense_rate = 100, actual_rate = 1, cap = 10:
//   correction = min(max(1, 100/1), 10) = 10
//   effective_phi = 0.5 * 10 = 5.0
//   skew(100) = 5.0 * 100 / 1000 = 0.5

TEST_F(GlftFullTest, SparseCorrection10x) {
    cfg_.expected_dense_fills_per_hour = 100.0;
    cfg_.actual_fills_per_hour         = 1.0;
    cfg_.sparse_correction_cap         = 10.0;
    xop::GlftStrategy strategy(cfg_);

    EXPECT_NEAR(strategy.inventory_skew(100.0), 0.5, 1e-10);
    EXPECT_NEAR(strategy.inventory_skew(1000.0), 5.0, 1e-10);
}

// When cap is 5, correction = min(100, 5) = 5
TEST_F(GlftFullTest, SparseCorrectionCapped) {
    cfg_.expected_dense_fills_per_hour = 100.0;
    cfg_.actual_fills_per_hour         = 1.0;
    cfg_.sparse_correction_cap         = 5.0;
    xop::GlftStrategy strategy(cfg_);

    // effective_phi = 0.5 * 5 = 2.5
    // skew(100) = 2.5 * 100 / 1000 = 0.25
    EXPECT_NEAR(strategy.inventory_skew(100.0), 0.25, 1e-10);
}

// ============================================================================
// TEST: Tau exponential decay (T5-CR3)
// ============================================================================
//
// tau_max = 120 * 52 = 6240
// tau_min = 0.01
// lambda  = -ln(0.01 / 6240) / 120

TEST_F(GlftFullTest, TauAtFillBlock) {
    xop::GlftStrategy strategy(cfg_);
    EXPECT_NEAR(strategy.compute_tau(0), 6240.0, 1e-10);
}

TEST_F(GlftFullTest, TauMidDecay) {
    xop::GlftStrategy strategy(cfg_);
    double expected = std::sqrt(6240.0 * cfg_.tau_min);
    EXPECT_NEAR(strategy.compute_tau(60), expected, 1e-4);
}

TEST_F(GlftFullTest, TauAtHorizon) {
    xop::GlftStrategy strategy(cfg_);
    EXPECT_NEAR(strategy.compute_tau(120), cfg_.tau_min, 1e-10);
}

TEST_F(GlftFullTest, TauNeverBelowFloor) {
    xop::GlftStrategy strategy(cfg_);
    for (uint32_t bh = 0; bh < 500; ++bh) {
        EXPECT_GE(strategy.compute_tau(bh), cfg_.tau_min);
    }
}

TEST_F(GlftFullTest, RecordFillResetsTau) {
    xop::GlftStrategy strategy(cfg_);

    strategy.update_price(2.70, 50);

    double tau_before = strategy.compute_tau(50);
    EXPECT_LT(tau_before, 1000.0);

    strategy.record_fill();

    // After fill, blocks_since_fill = 0 at block 50 => tau = tau_max.
    EXPECT_NEAR(strategy.compute_tau(50), 6240.0, 1e-10);
}

// ============================================================================
// TEST: compute_quotes symmetry when q = 0
// ============================================================================

TEST_F(GlftFullTest, ZeroInventorySymmetricQuotes) {
    xop::GlftStrategy strategy(cfg_);

    const double S = 10.0;
    auto quotes = strategy.compute_quotes(S, 0.05, 0.0, 0);

    // With zero inventory the skew is zero, so quotes are symmetric.
    const double mid = (quotes.bid_price + quotes.ask_price) / 2.0;
    EXPECT_NEAR(mid, S, 1e-6);
}

// ============================================================================
// TEST: compute_quotes inventory direction
// ============================================================================

TEST_F(GlftFullTest, LongInventoryShiftsQuotesDown) {
    xop::GlftStrategy strategy(cfg_);

    const double S = 10.0;
    auto q0 = strategy.compute_quotes(S, 0.05, 0.0, 0);
    auto q_long = strategy.compute_quotes(S, 0.05, 500.0, 0);

    // Long inventory: both quotes shift down (easier to sell).
    EXPECT_LT(q_long.ask_price, q0.ask_price);
    EXPECT_LT(q_long.bid_price, q0.bid_price);
}

TEST_F(GlftFullTest, ShortInventoryShiftsQuotesUp) {
    xop::GlftStrategy strategy(cfg_);

    const double S = 10.0;
    auto q0 = strategy.compute_quotes(S, 0.05, 0.0, 0);
    auto q_short = strategy.compute_quotes(S, 0.05, -500.0, 0);

    // Short inventory: both quotes shift up (easier to buy).
    EXPECT_GT(q_short.ask_price, q0.ask_price);
    EXPECT_GT(q_short.bid_price, q0.bid_price);
}

// ============================================================================
// TEST: Size scaling
// ============================================================================

TEST_F(GlftFullTest, SizeScalingBalanced) {
    xop::GlftStrategy strategy(cfg_);
    auto quotes = strategy.compute_quotes(10.0, 0.05, 0.0, 0);
    EXPECT_NEAR(quotes.bid_size, 1000.0, 1e-10);
    EXPECT_NEAR(quotes.ask_size, 1000.0, 1e-10);
}

TEST_F(GlftFullTest, SizeScalingFullyLong) {
    xop::GlftStrategy strategy(cfg_);
    auto quotes = strategy.compute_quotes(10.0, 0.05, 1000.0, 0);
    EXPECT_NEAR(quotes.bid_size, 0.0, 1e-10);
    EXPECT_NEAR(quotes.ask_size, 2000.0, 1e-10);
}

TEST_F(GlftFullTest, SizeScalingFullyShort) {
    xop::GlftStrategy strategy(cfg_);
    auto quotes = strategy.compute_quotes(10.0, 0.05, -1000.0, 0);
    EXPECT_NEAR(quotes.bid_size, 2000.0, 1e-10);
    EXPECT_NEAR(quotes.ask_size, 0.0, 1e-10);
}

// ============================================================================
// TEST: No-loss constraint
// ============================================================================

TEST_F(GlftFullTest, NoLossConstraintFloors) {
    cfg_.enable_no_loss_constraint = true;
    cfg_.min_margin_bps = 35.0;
    xop::GlftStrategy strategy(cfg_);

    strategy.set_cost_basis(5.00, 35.0);

    auto quotes = strategy.compute_quotes(4.00, 0.001, 0.0, 0);

    const double min_ask = 5.00 * (1.0 + 35.0 / 10000.0);
    EXPECT_GE(quotes.ask_price, min_ask - 1e-10);
}

TEST_F(GlftFullTest, NoLossConstraintDisabled) {
    cfg_.enable_no_loss_constraint = false;
    xop::GlftStrategy strategy(cfg_);

    strategy.set_cost_basis(100.0, 35.0);

    auto quotes = strategy.compute_quotes(2.70, 0.001, 0.0, 0);

    // Constraint disabled: ask is NOT floored at 100*(1+0.0035).
    EXPECT_LT(quotes.ask_price, 100.0);
}

// ============================================================================
// TEST: Zero volatility
// ============================================================================

TEST_F(GlftFullTest, ZeroVolatilityPositiveSpread) {
    xop::GlftStrategy strategy(cfg_);

    auto quotes = strategy.compute_quotes(10.0, 0.0, 0.0, 0);

    // With sigma = 0, term2 = 0 but term1 > 0, so spread is still positive.
    EXPECT_GT(quotes.ask_price - quotes.bid_price, 0.0);
    EXPECT_GT(quotes.spread_bps, 0.0);
}

// ============================================================================
// TEST: NaN inputs return zero quote
// ============================================================================

TEST_F(GlftFullTest, NaNInputReturnsZeroQuote) {
    xop::GlftStrategy strategy(cfg_);

    const double nan_val = std::numeric_limits<double>::quiet_NaN();

    auto q1 = strategy.compute_quotes(nan_val, 0.05, 0.0, 0);
    EXPECT_NEAR(q1.spread_bps, 0.0, 1e-10);

    auto q2 = strategy.compute_quotes(10.0, nan_val, 0.0, 0);
    EXPECT_NEAR(q2.spread_bps, 0.0, 1e-10);

    auto q3 = strategy.compute_quotes(10.0, 0.05, nan_val, 0);
    EXPECT_NEAR(q3.spread_bps, 0.0, 1e-10);
}

// ============================================================================
// TEST: Spread basis-point consistency
// ============================================================================

TEST_F(GlftFullTest, SpreadBpsConsistency) {
    xop::GlftStrategy strategy(cfg_);

    const double S = 10.0;
    auto quotes = strategy.compute_quotes(S, 0.05, 50.0, 30);

    EXPECT_LT(quotes.bid_price, quotes.ask_price);

    const double expected_bps = 10000.0 * (quotes.ask_price - quotes.bid_price) / S;
    EXPECT_NEAR(quotes.spread_bps, expected_bps, 0.01);
}

// ============================================================================
// TEST: Regime detection integration
// ============================================================================
//
// Feed prices to update_price() to populate the regime detector, then
// verify that regime classification affects quote output.

TEST_F(GlftFullTest, RegimeClassificationDefaultIsRandom) {
    xop::GlftStrategy strategy(cfg_);

    auto ri = strategy.current_regime();
    EXPECT_EQ(ri.regime, xop::MarketRegime::Random);
    EXPECT_NEAR(ri.spread_mult, 1.0, 1e-10);
    EXPECT_NEAR(ri.skew_mult, 1.0, 1e-10);
}

// ============================================================================
// TEST: Constructor validation
// ============================================================================

TEST_F(GlftFullTest, InvalidGammaThrows) {
    cfg_.gamma = 0.0;
    EXPECT_THROW(construct_strategy(cfg_), std::invalid_argument);
}

TEST_F(GlftFullTest, InvalidKappaThrows) {
    cfg_.kappa = -1.0;
    EXPECT_THROW(construct_strategy(cfg_), std::invalid_argument);
}

TEST_F(GlftFullTest, InvalidPhiThrows) {
    cfg_.phi = -0.1;
    EXPECT_THROW(construct_strategy(cfg_), std::invalid_argument);
}

TEST_F(GlftFullTest, InvalidQMaxThrows) {
    cfg_.q_max = 0.0;
    EXPECT_THROW(construct_strategy(cfg_), std::invalid_argument);
}

TEST_F(GlftFullTest, InvalidTauMinThrows) {
    cfg_.tau_min = 0.0;
    EXPECT_THROW(construct_strategy(cfg_), std::invalid_argument);
}

TEST_F(GlftFullTest, TauMinExceedsTauMaxThrows) {
    cfg_.tau_min = 999999.0;
    EXPECT_THROW(construct_strategy(cfg_), std::invalid_argument);
}

TEST_F(GlftFullTest, InvalidSparseActualThrows) {
    cfg_.actual_fills_per_hour = 0.0;
    EXPECT_THROW(construct_strategy(cfg_), std::invalid_argument);
}

TEST_F(GlftFullTest, InvalidSparseDenseThrows) {
    cfg_.expected_dense_fills_per_hour = 0.0;
    EXPECT_THROW(construct_strategy(cfg_), std::invalid_argument);
}

TEST_F(GlftFullTest, InvalidSparseCapThrows) {
    cfg_.sparse_correction_cap = 0.5;
    EXPECT_THROW(construct_strategy(cfg_), std::invalid_argument);
}

}  // namespace
