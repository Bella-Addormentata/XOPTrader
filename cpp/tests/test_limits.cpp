// test_limits.cpp -- Unit tests for PreTradeCheck (risk/limits.hpp).
//
// Tests cover:
//   - enforce_no_loss: ask price flooring, disabled constraint, zero cost basis
//   - check_flash_crash: rolling max-drawdown detection, edge cases
//   - is_stable_after_crash: recovery gate, insufficient history
//   - congestion_buffer_multiplier: normal vs congested
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/risk/limits.hpp>
#include <xop/config.hpp>
#include <xop/types.hpp>

#include <cmath>
#include <vector>

namespace {

// ============================================================================
// Helper: construct a PreTradeCheck with default configs
// ============================================================================

struct LimitsTestFixture : public ::testing::Test {
    xop::RiskConfig     risk_cfg;
    xop::StrategyConfig strat_cfg;

    void SetUp() override {
        risk_cfg.soft_limit_pct         = 0.60;
        risk_cfg.hard_limit_pct         = 0.80;
        risk_cfg.single_cat_cap_pct     = 0.12;
        risk_cfg.max_capital_per_pair_pct = 0.20;
        risk_cfg.max_drawdown_pct       = 0.10;

        strat_cfg.min_profit_margin_bps = 35.0;  // 0.35%
    }
};

// ============================================================================
// enforce_no_loss
// ============================================================================

TEST_F(LimitsTestFixture, EnforceNoLoss_AskBelowFloor_RaisedToFloor) {
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);

    xop::Quote q{};
    q.bid_price = 90'000;
    q.ask_price = 99'000;  // below cost_basis + margin
    q.bid_size  = 1000;
    q.ask_size  = 1000;
    q.spread_bps = 100.0;

    const xop::Mojo cost_basis = 100'000;
    // margin = round(100'000 * 0.0035) = 350
    // min_ask = 100'000 + 350 = 100'350

    auto result = ptc.enforce_no_loss(q, cost_basis, true);
    EXPECT_GE(result.ask_price, 100'350);
    EXPECT_EQ(result.bid_price, 90'000);  // bid unchanged
    EXPECT_EQ(result.bid_size, 1000);     // sizes unchanged
    EXPECT_EQ(result.ask_size, 1000);
}

TEST_F(LimitsTestFixture, EnforceNoLoss_AskAboveFloor_Unchanged) {
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);

    xop::Quote q{};
    q.bid_price = 90'000;
    q.ask_price = 110'000;  // well above floor
    q.bid_size  = 1000;
    q.ask_size  = 1000;
    q.spread_bps = 200.0;

    auto result = ptc.enforce_no_loss(q, 100'000, true);
    EXPECT_EQ(result.ask_price, 110'000);
}

TEST_F(LimitsTestFixture, EnforceNoLoss_Disabled_PassThrough) {
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);

    xop::Quote q{};
    q.bid_price = 90'000;
    q.ask_price = 50'000;  // far below cost basis
    q.bid_size  = 1000;
    q.ask_size  = 1000;

    auto result = ptc.enforce_no_loss(q, 100'000, false);
    EXPECT_EQ(result.ask_price, 50'000);  // unchanged when disabled
}

TEST_F(LimitsTestFixture, EnforceNoLoss_ZeroCostBasis_PassThrough) {
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);

    xop::Quote q{};
    q.bid_price = 90'000;
    q.ask_price = 50'000;
    q.bid_size  = 1000;
    q.ask_size  = 1000;

    auto result = ptc.enforce_no_loss(q, 0, true);
    EXPECT_EQ(result.ask_price, 50'000);  // no cost basis, constraint vacuous
}

TEST_F(LimitsTestFixture, EnforceNoLoss_NegativeCostBasis_PassThrough) {
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);

    xop::Quote q{};
    q.ask_price = 50'000;

    auto result = ptc.enforce_no_loss(q, -100, true);
    EXPECT_EQ(result.ask_price, 50'000);
}

// ============================================================================
// check_flash_crash (static)
// ============================================================================

TEST(FlashCrashTest, NoDataNocrash) {
    EXPECT_FALSE(xop::PreTradeCheck::check_flash_crash({}, 0.20));
    EXPECT_FALSE(xop::PreTradeCheck::check_flash_crash({100}, 0.20));
}

TEST(FlashCrashTest, DetectsLargeDrop) {
    // 100 -> 75: 25% drop, threshold 20%
    std::vector<xop::Mojo> prices = {100, 95, 90, 85, 75};
    EXPECT_TRUE(xop::PreTradeCheck::check_flash_crash(prices, 0.20));
}

TEST(FlashCrashTest, NoCrash_SmallDrop) {
    // 100 -> 85: 15% drop, threshold 20%
    std::vector<xop::Mojo> prices = {100, 95, 90, 85};
    EXPECT_FALSE(xop::PreTradeCheck::check_flash_crash(prices, 0.20));
}

TEST(FlashCrashTest, DetectsEarlyDropFollowedByRecovery) {
    // 100 -> 70 (30% drop) then recovery to 110
    std::vector<xop::Mojo> prices = {100, 90, 70, 80, 95, 110};
    EXPECT_TRUE(xop::PreTradeCheck::check_flash_crash(prices, 0.20));
}

TEST(FlashCrashTest, MonotonicallyRising_NoCrash) {
    std::vector<xop::Mojo> prices = {50, 60, 70, 80, 90, 100};
    EXPECT_FALSE(xop::PreTradeCheck::check_flash_crash(prices, 0.20));
}

TEST(FlashCrashTest, FlatMarket_NoCrash) {
    std::vector<xop::Mojo> prices = {100, 100, 100, 100};
    EXPECT_FALSE(xop::PreTradeCheck::check_flash_crash(prices, 0.20));
}

TEST(FlashCrashTest, ExactThreshold_Triggers) {
    // 100 -> 80: exactly 20% drop, threshold 20% (>= triggers)
    std::vector<xop::Mojo> prices = {100, 80};
    EXPECT_TRUE(xop::PreTradeCheck::check_flash_crash(prices, 0.20));
}

// ============================================================================
// is_stable_after_crash (static)
// ============================================================================

TEST(StabilityTest, InsufficientHistory_NotStable) {
    std::vector<xop::Mojo> prices = {100, 100, 100};
    EXPECT_FALSE(xop::PreTradeCheck::is_stable_after_crash(prices, 5, 0.05));
}

TEST(StabilityTest, AllPricesWithinBand_Stable) {
    // 10 prices all within 5% of 100
    std::vector<xop::Mojo> prices = {98, 99, 100, 101, 100, 99, 100, 101, 100, 100};
    EXPECT_TRUE(xop::PreTradeCheck::is_stable_after_crash(prices, 10, 0.05));
}

TEST(StabilityTest, OneOutlier_NotStable) {
    // One price 10% off in the tail
    std::vector<xop::Mojo> prices = {100, 100, 100, 100, 90, 100, 100, 100, 100, 100};
    // The outlier (90) is within the required_stable_blocks=10 tail
    // deviation = |90 - 100| / 100 = 0.10 > 0.05
    EXPECT_FALSE(xop::PreTradeCheck::is_stable_after_crash(prices, 10, 0.05));
}

TEST(StabilityTest, ZeroLatestPrice_NotStable) {
    std::vector<xop::Mojo> prices = {100, 100, 100, 0};
    EXPECT_FALSE(xop::PreTradeCheck::is_stable_after_crash(prices, 4, 0.05));
}

// ============================================================================
// congestion_buffer_multiplier
// ============================================================================

TEST_F(LimitsTestFixture, CongestionMultiplier_Normal) {
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    EXPECT_DOUBLE_EQ(ptc.congestion_buffer_multiplier(false), 1.0);
}

TEST_F(LimitsTestFixture, CongestionMultiplier_Congested) {
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    double mult = ptc.congestion_buffer_multiplier(true);
    EXPECT_GT(mult, 1.0);
    EXPECT_LE(mult, 1.5);
}

// ============================================================================
// apply_limits
// ============================================================================

TEST_F(LimitsTestFixture, ApplyLimitsHardBaseOverweightKeepsTinyBid) {
    risk_cfg.max_capital_per_pair_pct = 0.95;
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;

    state.record_buy("xch", 80, 1);
    state.record_buy("wusdc", 20, 1);
    state.record_buy("dbx", 100, 1);
    state.set_asset_xch_rate("wusdc", 1.0);
    state.set_asset_xch_rate("dbx", 1.0);

    xop::Quote q{};
    q.bid_size = 1000;
    q.ask_size = 1000;

    auto result = ptc.apply_limits(q, "XCH/wUSDC.b", "xch", "wusdc", state);
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(result->bid_size, 0);
    EXPECT_LT(result->bid_size, q.bid_size);
    EXPECT_EQ(result->ask_size, q.ask_size);
}

TEST_F(LimitsTestFixture, ApplyLimitsSingleCatCapKeepsTinyBid) {
    risk_cfg.max_capital_per_pair_pct = 0.95;
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;

    state.record_buy("cat", 50, 1);
    state.record_buy("xch", 50, 1);
    state.record_buy("wusdc", 150, 1);
    state.set_asset_xch_rate("cat", 1.0);
    state.set_asset_xch_rate("wusdc", 1.0);

    xop::Quote q{};
    q.bid_size = 1000;
    q.ask_size = 1000;

    auto result = ptc.apply_limits(q, "CAT/XCH", "cat", "xch", state);
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(result->bid_size, 0);
    EXPECT_LT(result->bid_size, q.bid_size);
    EXPECT_EQ(result->ask_size, q.ask_size);
}

TEST_F(LimitsTestFixture, ApplyLimitsExtremeBaseOverweightStillBlocksBid) {
    risk_cfg.max_capital_per_pair_pct = 0.95;
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;

    state.record_buy("xch", 100, 1);
    state.record_buy("dbx", 100, 1);
    state.set_asset_xch_rate("dbx", 1.0);

    xop::Quote q{};
    q.bid_size = 1000;
    q.ask_size = 1000;

    auto result = ptc.apply_limits(q, "XCH/wUSDC.b", "xch", "wusdc", state);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->bid_size, 0);
    EXPECT_EQ(result->ask_size, q.ask_size);
}

// ============================================================================
// Construction validation
// ============================================================================

TEST(PreTradeCheckConstruction, RejectsSoftGtHard) {
    xop::RiskConfig risk;
    risk.soft_limit_pct = 0.90;
    risk.hard_limit_pct = 0.80;
    xop::StrategyConfig strat;
    strat.min_profit_margin_bps = 35.0;

    EXPECT_THROW(xop::PreTradeCheck(risk, strat), std::invalid_argument);
}

TEST(PreTradeCheckConstruction, RejectsNegativeMargin) {
    xop::RiskConfig risk;
    xop::StrategyConfig strat;
    strat.min_profit_margin_bps = -100.0;

    EXPECT_THROW(xop::PreTradeCheck(risk, strat), std::invalid_argument);
}

}  // namespace
