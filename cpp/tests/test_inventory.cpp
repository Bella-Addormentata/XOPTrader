// test_inventory.cpp -- Unit tests for inventory tracking, cost basis
//                       accounting, and risk management.
//
// Tests verify the weighted-average cost basis arithmetic, never-sell-at-loss
// constraint, soft/hard limit thresholds, and Kelly sizing from
// CHIA_MARKET_MAKER_STRATEGY.md Sections 7 and 8.
//
// Monetary values use int64_t mojos (1 XCH = 10^12 mojos).  For readability,
// the tests use small integer mojos (e.g. price=2700000 represents 2.70 XCH
// at a precision where 1 unit = 10^-6 XCH).  This keeps the hand-computed
// expectations tractable without losing test validity.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/risk/inventory.hpp>
#include <xop/config.hpp>
#include <xop/types.hpp>

#include <chrono>
#include <unordered_map>

namespace {

// Convenience alias for timestamps.
using Clock = std::chrono::system_clock;

// ============================================================================
// Cost Basis Fixture
// ============================================================================

class CostBasisTest : public ::testing::Test {
protected:
    void SetUp() override {
        risk_cfg_.soft_limit_pct          = 0.60;
        risk_cfg_.hard_limit_pct          = 0.80;
        risk_cfg_.single_cat_cap_pct      = 0.12;
        risk_cfg_.kelly_fraction          = 0.50;
        risk_cfg_.max_capital_per_pair_pct = 0.20;
    }

    xop::RiskConfig risk_cfg_;
    xop::Timestamp  now_ = Clock::now();
};

// ============================================================================
// TEST: Weighted-average cost basis calculation
// ============================================================================
//
// Scenario:
//   Buy 100 units @ price 2700000  => total_cost = 270,000,000
//   Buy  50 units @ price 2800000  => total_cost = 270,000,000 + 140,000,000
//                                                 = 410,000,000
//   total_quantity = 150
//   cost_basis = 410,000,000 / 150 = 2,733,333  (truncated integer division)

TEST_F(CostBasisTest, WeightedAverageTwoBuys) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL);

    tracker.record_buy("xch", 100, 2'700'000, 1, now_);
    tracker.record_buy("xch",  50, 2'800'000, 2, now_);

    auto rec = tracker.get_record("xch");
    EXPECT_EQ(rec.total_quantity, 150);
    EXPECT_EQ(rec.total_cost, 410'000'000LL);

    // Weighted average: 410000000 / 150 = 2733333 (truncated).
    EXPECT_EQ(rec.weighted_avg_cost_basis, 2'733'333);
}

// ============================================================================
// TEST: Sell reduces quantity but preserves cost basis
// ============================================================================
//
// After the two buys (basis = 2733333):
//   Sell 75 units @ 2800000 (above cost => allowed).
//
//   Cost removed proportionally:
//     new_total_cost = old_total_cost * (old_qty - sell_qty) / old_qty
//                    = 410,000,000 * (150 - 75) / 150
//                    = 410,000,000 * 75 / 150
//                    = 205,000,000
//     new_qty = 75
//     new_basis = 205,000,000 / 75 = 2,733,333
//
//   The basis remains unchanged under weighted-average drawdown.

TEST_F(CostBasisTest, SellPreservesCostBasis) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL);

    tracker.record_buy("xch", 100, 2'700'000, 1, now_);
    tracker.record_buy("xch",  50, 2'800'000, 2, now_);

    bool ok = tracker.record_sell("xch", 75, 2'800'000, 3, now_);
    EXPECT_TRUE(ok);

    auto rec = tracker.get_record("xch");
    EXPECT_EQ(rec.total_quantity, 75);

    // Basis should be preserved at 2733333.
    EXPECT_EQ(rec.weighted_avg_cost_basis, 2'733'333);
}

// ============================================================================
// TEST: Never-sell-at-loss enforcement
// ============================================================================
//
// With no_loss_constraint enabled, a sell at a price below the cost basis
// must be rejected.

TEST_F(CostBasisTest, SellAtLossRejected) {
    // Constructor with no_loss_constraint = true (the default).
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, true);

    tracker.record_buy("xch", 100, 2'700'000, 1, now_);

    // Attempt to sell below cost basis (2700000).
    bool ok = tracker.record_sell("xch", 50, 2'500'000, 2, now_);
    EXPECT_FALSE(ok) << "Sell below cost basis must be rejected";

    // Inventory must remain unchanged.
    auto rec = tracker.get_record("xch");
    EXPECT_EQ(rec.total_quantity, 100);
    EXPECT_EQ(rec.weighted_avg_cost_basis, 2'700'000);
}

// Sell at exactly cost basis is permitted (not a loss -- strict less-than check).
TEST_F(CostBasisTest, SellAtExactCostAllowed) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, true);
    tracker.record_buy("xch", 100, 2'700'000, 1, now_);

    // sell_price < cost_basis check: 2700000 < 2700000 is false, so this
    // should succeed (equal price is NOT a loss).
    bool ok = tracker.record_sell("xch", 50, 2'700'000, 2, now_);
    EXPECT_TRUE(ok) << "Sell at exact cost basis is not a loss";
}

// With constraint disabled, sell below cost is allowed.
TEST_F(CostBasisTest, SellAtLossAllowedWhenDisabled) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    tracker.record_buy("xch", 100, 2'700'000, 1, now_);

    bool ok = tracker.record_sell("xch", 50, 2'500'000, 2, now_);
    EXPECT_TRUE(ok);

    auto rec = tracker.get_record("xch");
    EXPECT_EQ(rec.total_quantity, 50);
}

// ============================================================================
// TEST: Full liquidation resets cost basis to zero
// ============================================================================

TEST_F(CostBasisTest, FullLiquidationResetsBasis) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    tracker.record_buy("xch", 100, 2'700'000, 1, now_);
    bool ok = tracker.record_sell("xch", 100, 2'800'000, 2, now_);
    EXPECT_TRUE(ok);

    auto rec = tracker.get_record("xch");
    EXPECT_EQ(rec.total_quantity, 0);
    EXPECT_EQ(rec.total_cost, 0);
    EXPECT_EQ(rec.weighted_avg_cost_basis, 0);
}

// ============================================================================
// TEST: Zero balance then buy again
// ============================================================================
//
// After full liquidation, buying again should produce a fresh cost basis.

TEST_F(CostBasisTest, BuyAfterFullLiquidation) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    tracker.record_buy("xch", 100, 2'700'000, 1, now_);
    EXPECT_TRUE(tracker.record_sell("xch", 100, 2'800'000, 2, now_));

    // Fresh buy at a new price.
    tracker.record_buy("xch", 200, 3'000'000, 3, now_);

    auto rec = tracker.get_record("xch");
    EXPECT_EQ(rec.total_quantity, 200);
    EXPECT_EQ(rec.weighted_avg_cost_basis, 3'000'000);
}

// ============================================================================
// TEST: Cannot sell more than holdings
// ============================================================================

TEST_F(CostBasisTest, SellExceedingHoldingsRejected) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);
    tracker.record_buy("xch", 100, 2'700'000, 1, now_);

    bool ok = tracker.record_sell("xch", 150, 2'800'000, 2, now_);
    EXPECT_FALSE(ok) << "Cannot sell more than current holdings";

    auto rec = tracker.get_record("xch");
    EXPECT_EQ(rec.total_quantity, 100);
}

// ============================================================================
// TEST: Invalid inputs rejected
// ============================================================================

TEST_F(CostBasisTest, InvalidBuyInputs) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL);

    // Zero quantity.
    tracker.record_buy("xch", 0, 2'700'000, 1, now_);
    EXPECT_EQ(tracker.net_inventory("xch"), 0);

    // Negative quantity.
    tracker.record_buy("xch", -10, 2'700'000, 1, now_);
    EXPECT_EQ(tracker.net_inventory("xch"), 0);

    // Zero price.
    tracker.record_buy("xch", 100, 0, 1, now_);
    EXPECT_EQ(tracker.net_inventory("xch"), 0);
}

// ============================================================================
// TEST: Soft / Hard limit thresholds
// ============================================================================
//
// Strategy doc S8:
//   Soft limit:  60% in one asset => begin aggressive skewing
//   Hard limit:  80% in one asset => pull quotes on overweight side

TEST_F(CostBasisTest, SoftLimitExactThreshold) {
    // Create a tracker where a single asset constitutes exactly 60% of value.
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    // Buy 60 units of "base" and 40 units of "quote" at equal price.
    tracker.record_buy("base",  60, 1'000'000, 1, now_);
    tracker.record_buy("quote", 40, 1'000'000, 1, now_);

    // Price map: both assets at the same price (1000000).
    std::unordered_map<xop::AssetId, xop::Mojo> prices = {
        {"base",  1'000'000},
        {"quote", 1'000'000}
    };

    // "base" concentration: 60 / (60+40) = 0.60 => exactly at soft limit.
    auto status = tracker.get_risk_status("base", 1'000'000, prices);
    EXPECT_EQ(status, xop::RiskStatus::SoftLimit);
}

TEST_F(CostBasisTest, HardLimitExactThreshold) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    tracker.record_buy("base",  80, 1'000'000, 1, now_);
    tracker.record_buy("quote", 20, 1'000'000, 1, now_);

    std::unordered_map<xop::AssetId, xop::Mojo> prices = {
        {"base",  1'000'000},
        {"quote", 1'000'000}
    };

    // "base" concentration: 80 / 100 = 0.80 => exactly at hard limit.
    auto status = tracker.get_risk_status("base", 1'000'000, prices);
    EXPECT_EQ(status, xop::RiskStatus::HardLimit);
}

TEST_F(CostBasisTest, NormalBelowSoftLimit) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    tracker.record_buy("base",  50, 1'000'000, 1, now_);
    tracker.record_buy("quote", 50, 1'000'000, 1, now_);

    std::unordered_map<xop::AssetId, xop::Mojo> prices = {
        {"base",  1'000'000},
        {"quote", 1'000'000}
    };

    // 50% concentration => Normal.
    auto status = tracker.get_risk_status("base", 1'000'000, prices);
    EXPECT_EQ(status, xop::RiskStatus::Normal);
}

// ============================================================================
// TEST: Underwater detection
// ============================================================================

TEST_F(CostBasisTest, UnderwaterDetection) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    tracker.record_buy("xch", 100, 2'700'000, 1, now_);

    // Market price below cost basis => underwater.
    EXPECT_TRUE(tracker.is_underwater("xch", 2'500'000));
    // Market price at cost basis => NOT underwater.
    EXPECT_FALSE(tracker.is_underwater("xch", 2'700'000));
    // Market price above cost basis => NOT underwater.
    EXPECT_FALSE(tracker.is_underwater("xch", 3'000'000));
}

// Underwater trumps soft/hard limit in risk status (highest severity).
TEST_F(CostBasisTest, UnderwaterTrumpsOtherStatuses) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    tracker.record_buy("base",  80, 2'700'000, 1, now_);
    tracker.record_buy("quote", 20, 1'000'000, 1, now_);

    std::unordered_map<xop::AssetId, xop::Mojo> prices = {
        {"base",  2'000'000},  // below cost basis of 2700000
        {"quote", 1'000'000}
    };

    // "base" is both at hard limit (concentration ~89%) and underwater.
    // Underwater should win (highest severity).
    auto status = tracker.get_risk_status("base", 2'000'000, prices);
    EXPECT_EQ(status, xop::RiskStatus::Underwater);
}

// ============================================================================
// TEST: inventory_ratio normalizes mismatched denominations
// ============================================================================

TEST_F(CostBasisTest, InventoryRatioNormalizesPairDenominations) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    // Use scaled-down synthetic denominations so the test exercises the
    // normalization math without hitting unrelated cost-basis overflow limits.
    constexpr xop::Mojo kBaseMojosPerUnit  = 100;
    constexpr xop::Mojo kQuoteMojosPerUnit = 10;

    tracker.record_buy("base", 5'000, 400'000'000'000LL, 1, now_); // 50 base units
    tracker.record_buy("quote", 200, 10, 1, now_);                 // 20 quote units

    const double ratio = tracker.inventory_ratio(
        "base",
        "quote",
        400'000'000'000LL,
        kBaseMojosPerUnit,
        kQuoteMojosPerUnit);

    EXPECT_NEAR(ratio, 0.5, 1e-12);
}

TEST_F(CostBasisTest, InventoryRatioReturnsBaseShareInQuoteUnits) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);

    constexpr xop::Mojo kBaseMojosPerUnit  = 100;
    constexpr xop::Mojo kQuoteMojosPerUnit = 10;

    tracker.record_buy("base", 5'000, 400'000'000'000LL, 1, now_); // 50 base units
    tracker.record_buy("quote", 600, 10, 1, now_);                 // 60 quote units

    const double ratio = tracker.inventory_ratio(
        "base",
        "quote",
        400'000'000'000LL,
        kBaseMojosPerUnit,
        kQuoteMojosPerUnit);

    EXPECT_NEAR(ratio, 0.25, 1e-12);
}

TEST_F(CostBasisTest, InventoryRatioInfersXchAndCatDenominations) {
    xop::InventoryTracker tracker(risk_cfg_, 10'000'000'000'000LL, false);

    tracker.record_buy("xch", xop::kMojosPerXch, 1, 1, now_); // 1 XCH
    tracker.record_buy("cat", 2'000, 1, 1, now_);             // 2 CAT units

    const double ratio = tracker.inventory_ratio(
        "xch",
        "cat",
        2 * xop::kMojosPerXch);

    EXPECT_NEAR(ratio, 0.5, 1e-12);
}

// ============================================================================
// TEST: Kelly sizing
// ============================================================================
//
// Formula (strategy doc S7):
//   f* = kelly_fraction * (spread_bps/10000 - sigma * sqrt(tau))
//                         / (sigma^2 * tau)
//
// Hand computation:
//   spread_bps = 100, sigma = 0.05, tau = 0.5 (half a year)
//   kelly_fraction = 0.50
//
//   spread_frac = 100 / 10000 = 0.01
//   edge = 0.01 - 0.05 * sqrt(0.5)
//        = 0.01 - 0.05 * 0.7071
//        = 0.01 - 0.03536
//        = -0.02536  (NEGATIVE => no bet)
//
// Use parameters that yield a positive edge:
//   spread_bps = 500, sigma = 0.05, tau = 0.01 (very short horizon)
//
//   edge = 0.05 - 0.05 * sqrt(0.01) = 0.05 - 0.005 = 0.045
//   full_kelly = 0.045 / (0.0025 * 0.01) = 0.045 / 0.000025 = 1800.0
//   half_kelly = 1800.0 * 0.50 = 900.0
//   clamped to max(max_capital_per_pair_pct=0.20, kPracticalCap=0.02) = 0.02

TEST_F(CostBasisTest, KellySizingPositiveEdge) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL);

    // Parameters that yield a large positive edge (clamped to practical cap).
    double sized = tracker.compute_kelly_size(500.0, 0.05, 0.01);
    EXPECT_NEAR(sized, 0.02, 1e-10);  // clamped to practical cap
}

TEST_F(CostBasisTest, KellySizingNegativeEdge) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL);

    // Edge is negative when spread does not compensate for vol.
    double sized = tracker.compute_kelly_size(100.0, 0.05, 0.5);
    EXPECT_NEAR(sized, 0.0, 1e-10);
}

TEST_F(CostBasisTest, KellySizingZeroSigmaReturnsZero) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL);
    EXPECT_NEAR(tracker.compute_kelly_size(100.0, 0.0, 1.0), 0.0, 1e-10);
}

TEST_F(CostBasisTest, KellySizingZeroTauReturnsZero) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL);
    EXPECT_NEAR(tracker.compute_kelly_size(100.0, 0.05, 0.0), 0.0, 1e-10);
}

// ============================================================================
// TEST: Capital allocation categories
// ============================================================================
//
// Strategy doc S7:
//   ActiveOffers    : 35-45% of capital
//   RebalancingBuffer: 15-20%
//   etc.

TEST_F(CostBasisTest, CapitalAllocationWithinLimits) {
    const xop::Mojo total = 10'000'000'000LL;  // 10B mojos
    xop::InventoryTracker tracker(risk_cfg_, total);

    // ActiveOffers max = 45% of 10B = 4.5B.
    bool ok = tracker.allocate_capital(
        xop::CapitalCategory::ActiveOffers, 4'000'000'000LL);
    EXPECT_TRUE(ok);

    EXPECT_EQ(tracker.allocated_capital(
        xop::CapitalCategory::ActiveOffers), 4'000'000'000LL);
}

TEST_F(CostBasisTest, CapitalAllocationExceedsLimit) {
    const xop::Mojo total = 10'000'000'000LL;
    xop::InventoryTracker tracker(risk_cfg_, total);

    // ActiveOffers max = 45% of 10B = 4.5B.  Trying to allocate 5B should fail.
    bool ok = tracker.allocate_capital(
        xop::CapitalCategory::ActiveOffers, 5'000'000'000LL);
    EXPECT_FALSE(ok);

    // Nothing should have been allocated.
    EXPECT_EQ(tracker.allocated_capital(
        xop::CapitalCategory::ActiveOffers), 0);
}

TEST_F(CostBasisTest, CapitalFreeAndReallocate) {
    const xop::Mojo total = 10'000'000'000LL;
    xop::InventoryTracker tracker(risk_cfg_, total);

    EXPECT_TRUE(tracker.allocate_capital(
        xop::CapitalCategory::ActiveOffers, 3'000'000'000LL));
    tracker.free_capital(
        xop::CapitalCategory::ActiveOffers, 1'000'000'000LL);

    EXPECT_EQ(tracker.allocated_capital(
        xop::CapitalCategory::ActiveOffers), 2'000'000'000LL);

    // Free capacity: max(4.5B) - used(2B) = 2.5B.
    EXPECT_EQ(tracker.free_capacity(
        xop::CapitalCategory::ActiveOffers), 2'500'000'000LL);
}

// ============================================================================
// TEST: min_ask_price respects no-loss constraint
// ============================================================================

TEST_F(CostBasisTest, MinAskPriceWithConstraint) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, true);
    tracker.record_buy("xch", 100, 2'700'000, 1, now_);

    // min_ask_price returns cost_basis when constraint is enabled.
    EXPECT_EQ(tracker.min_ask_price("xch"), 2'700'000);
}

TEST_F(CostBasisTest, MinAskPriceWithoutConstraint) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, false);
    tracker.record_buy("xch", 100, 2'700'000, 1, now_);

    // Constraint disabled: min_ask_price returns 0.
    EXPECT_EQ(tracker.min_ask_price("xch"), 0);
}

// ============================================================================
// TEST: Position age tracking
// ============================================================================

TEST_F(CostBasisTest, PositionAgeUnknownAsset) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL);
    EXPECT_EQ(tracker.position_age_blocks("unknown_asset", 1000), -1);
}

TEST_F(CostBasisTest, PositionAgeAfterFill) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL);
    tracker.record_buy("xch", 100, 2'700'000, 1000, now_);

    EXPECT_EQ(tracker.position_age_blocks("xch", 1500), 500);
    EXPECT_EQ(tracker.position_age_blocks("xch", 1000), 0);
}

// ============================================================================
// TEST: no_loss_constraint toggle at runtime
// ============================================================================

TEST_F(CostBasisTest, RuntimeConstraintToggle) {
    xop::InventoryTracker tracker(risk_cfg_, 1'000'000'000LL, true);
    EXPECT_TRUE(tracker.no_loss_constraint_enabled());

    tracker.set_no_loss_constraint(false);
    EXPECT_FALSE(tracker.no_loss_constraint_enabled());

    tracker.record_buy("xch", 100, 2'700'000, 1, now_);

    // With constraint disabled, sell below cost is allowed.
    bool ok = tracker.record_sell("xch", 50, 2'500'000, 2, now_);
    EXPECT_TRUE(ok);

    // Re-enable constraint.
    tracker.set_no_loss_constraint(true);
    EXPECT_TRUE(tracker.no_loss_constraint_enabled());

    // Now selling below cost should be blocked again.
    ok = tracker.record_sell("xch", 25, 2'500'000, 3, now_);
    EXPECT_FALSE(ok);
}

}  // namespace
