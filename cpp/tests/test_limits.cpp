// test_limits.cpp -- Unit tests for PreTradeCheck (risk/limits.hpp).
//
// Tests cover:
//   - enforce_no_loss: ask price flooring, disabled constraint, zero cost basis
//   - check_flash_crash: rolling max-drawdown detection, edge cases
//   - is_stable_after_crash: recovery gate, insufficient history
//   - congestion_buffer_multiplier: normal vs congested
//   - apply_limits / evaluate_limits: exact sizes and per-side attribution
//     (which rule, if any, lowered or zeroed each side) [STEP6-CAUSE]
//   - describe_side_limits / format_step6_no_quote: the operator-facing text
//   - LimitBlockWarnGate: the Step 6 no-quote warn and its recovery line
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/risk/limits.hpp>
#include <xop/config.hpp>
#include <xop/types.hpp>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
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
        risk_cfg.max_drawdown_frac      = 0.10;

        strat_cfg.min_profit_margin_bps = 35.0;  // 0.35%
    }
};

// [STEP6-CAUSE] Seed positions whose XCH marks equal their raw quantities,
// so portfolio fractions are plain ratios of the seeded numbers.
void seed_unit_rate_portfolio(
    xop::State& state,
    std::initializer_list<std::pair<const char*, xop::Mojo>> holdings)
{
    for (const auto& [asset, qty] : holdings) {
        state.record_buy(asset, qty, 1);
        if (std::string_view{asset} != "xch") {
            state.set_asset_xch_rate(asset, 1.0);
        }
    }
}

std::uint8_t rule_mask(xop::LimitRule a, xop::LimitRule b)
{
    return static_cast<std::uint8_t>(xop::limit_rule_bit(a) | xop::limit_rule_bit(b));
}

// The signature the live XCH/BYC heartbeat produces: a strategy-zero bid and
// an ask zeroed by the single-CAT cap.
xop::LimitBlockSignature xch_byc_cat_block()
{
    xop::LimitBlockSignature sig{};
    sig.bid_cause     = xop::SideZeroCause::ZeroBeforeLimits;
    sig.ask_cause     = xop::SideZeroCause::LimitZeroed;
    sig.ask_zeroed_by = xop::LimitRule::SingleCatCap;
    return sig;
}

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
    // 100 -> 70 (30% drop) then recovery to 110.  With NO window (the
    // default), the whole history is scanned and the old drop still trips.
    std::vector<xop::Mojo> prices = {100, 90, 70, 80, 95, 110};
    EXPECT_TRUE(xop::PreTradeCheck::check_flash_crash(prices, 0.20));
}

// ---------------------------------------------------------------------------
// [S12] The window: a "flash" crash is a RECENT event.
//
// Observed 2026-08-22: one junk dexie print (a crossed ticker; mid 2.3419 in
// a ~1.57 market, retraced within minutes) pinned the whole-history running
// maximum, so the detector reported a crash until the ~1000-entry buffer
// evicted the print -- and because the Crash state consults this signal
// before the stability checks, posting stayed halted globally for hours.
// ---------------------------------------------------------------------------

TEST(FlashCrashTest, Window_AnAgedSpikeStopsCounting) {
    // The S12 shape: one spike far in the past, market flat since.
    std::vector<xop::Mojo> prices;
    prices.push_back(157);
    prices.push_back(234);                       // the junk print
    for (int i = 0; i < 200; ++i) prices.push_back(163);

    // Whole history: still "crashing" (234 -> 163 is a 30% drawdown).
    EXPECT_TRUE(xop::PreTradeCheck::check_flash_crash(prices, 0.20));
    // Inside a 100-block window the spike is gone and the market is flat.
    EXPECT_FALSE(xop::PreTradeCheck::check_flash_crash(prices, 0.20, 100));
}

TEST(FlashCrashTest, Window_ARecentCollapseStillTrips) {
    // Flat, then a genuine 30% drop within the window.
    std::vector<xop::Mojo> prices(150, 100);
    for (int i = 0; i < 10; ++i) prices.push_back(70);
    EXPECT_TRUE(xop::PreTradeCheck::check_flash_crash(prices, 0.20, 100));
}

TEST(FlashCrashTest, Window_ThePeakOutsideTheWindowDoesNotCount) {
    // 100-era peak, then a long 79 plateau: 21% below the old peak, but the
    // peak is only visible if the scan reaches back past the window.  The
    // SAME data must trip or not trip purely on how far the window reaches.
    std::vector<xop::Mojo> prices;
    for (int i = 0; i < 50; ++i) prices.push_back(100);
    for (int i = 0; i < 100; ++i) prices.push_back(79);
    EXPECT_TRUE(xop::PreTradeCheck::check_flash_crash(prices, 0.20, 150));
    EXPECT_FALSE(xop::PreTradeCheck::check_flash_crash(prices, 0.20, 100));
}

TEST(FlashCrashTest, Window_ZeroMeansWholeHistory) {
    std::vector<xop::Mojo> prices = {100, 90, 70, 80, 95, 110};
    EXPECT_TRUE(xop::PreTradeCheck::check_flash_crash(prices, 0.20, 0));
}

TEST(FlashCrashTest, Window_LargerThanHistoryIsHarmless) {
    std::vector<xop::Mojo> prices = {100, 75};
    EXPECT_TRUE(xop::PreTradeCheck::check_flash_crash(prices, 0.20, 5000));
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
// evaluate_limits -- per-side attribution [STEP6-CAUSE]
//
// 2026-09-13: Step 6 logged "XCH/BYC -- both sides blocked by risk limits" on
// every heartbeat, but only the ask was limit-blocked; the bid arrived at 0
// from the strategy.  These tests pin which rule, if any, each side's cut or
// zero is attributed to, the exact sizes (the refactor must not move a mojo;
// every value was first confirmed against apply_limits on origin/main
// 209b36c), and the text the operator reads.
// ============================================================================

TEST_F(LimitsTestFixture, EvaluateLimits_XchBycLive_BidZeroFromStrategyAskZeroFromCatCap) {
    // Live risk config (config.yaml:294-298): soft 0.60 and hard 0.80 as in
    // the fixture, single-CAT cap 0.25, max capital per pair 0.85.
    risk_cfg.single_cat_cap_pct       = 0.25;
    risk_cfg.max_capital_per_pair_pct = 0.85;
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;
    // base_conc 268/774 = 0.346, BYC 506/1000 = 0.506, pair 774/1000 = 0.774.
    seed_unit_rate_portfolio(state, {{"xch", 268}, {"byc", 506}, {"dbx", 226}});

    xop::Quote q{};
    q.bid_size = 0;                    // strategy bid at q = 24.569719 >= q_max = 20
    q.ask_size = 44'569'719'126'041;   // strategy ask q_max + q = 44.569719 XCH

    const xop::LimitsDecision d = ptc.evaluate_limits(q, "xch", "byc", state);
    EXPECT_FALSE(d.has_quote);
    EXPECT_FALSE(ptc.apply_limits(q, "XCH/BYC", "xch", "byc", state).has_value());

    // Bid: arrived at 0 and no rule touched it.
    EXPECT_EQ(d.trace.bid.pre_size, 0);
    EXPECT_EQ(d.trace.bid.post_size, 0);
    EXPECT_EQ(d.trace.bid.reduced_by, 0);
    EXPECT_EQ(d.trace.bid.zeroed_by, xop::LimitRule::None);
    EXPECT_EQ(xop::side_zero_cause(d.trace.bid), xop::SideZeroCause::ZeroBeforeLimits);

    // Ask: scaled by the soft band (quote_conc 0.654), then zeroed by the
    // CAT cap, whose taper reaches 0 at 2 x 0.25 = 0.500 < 0.506.
    EXPECT_EQ(d.trace.ask.pre_size, 44'569'719'126'041);
    EXPECT_EQ(d.trace.ask.post_size, 0);
    EXPECT_EQ(d.trace.ask.zeroed_by, xop::LimitRule::SingleCatCap);
    EXPECT_EQ(d.trace.ask.reduced_by,
              rule_mask(xop::LimitRule::SoftConcentration, xop::LimitRule::SingleCatCap));
    EXPECT_EQ(xop::side_zero_cause(d.trace.ask), xop::SideZeroCause::LimitZeroed);

    EXPECT_NEAR(d.trace.base_concentration, 268.0 / 774.0, 1e-12);
    EXPECT_FALSE(d.trace.base_is_cat);
    EXPECT_TRUE(d.trace.quote_is_cat);
    EXPECT_NEAR(d.trace.quote_cat_fraction, 0.506, 1e-12);
    EXPECT_NEAR(d.trace.cat_full_block_pct, 0.50, 1e-12);
    EXPECT_NEAR(d.trace.pair_capital_fraction, 0.774, 1e-12);

    const xop::LimitBlockSignature sig = xop::limit_block_signature(d.trace);
    EXPECT_EQ(sig.bid_cause, xop::SideZeroCause::ZeroBeforeLimits);
    EXPECT_EQ(sig.bid_zeroed_by, xop::LimitRule::None);
    EXPECT_EQ(sig.ask_cause, xop::SideZeroCause::LimitZeroed);
    EXPECT_EQ(sig.ask_zeroed_by, xop::LimitRule::SingleCatCap);

    // The whole line the operator reads (the engine prepends only
    // "[Engine] ").  Exact on purpose: the wording is the fix.
    EXPECT_EQ(xop::format_step6_no_quote("XCH/BYC", d.trace, 1'000'000'000'000,
                                         24.569719126041, 20.0, risk_cfg,
                                         xop::kLimitBlockWarnReminderBlocks),
              "Step 6: XCH/BYC -- no quote this block: "
              "bid 0.000000 -> 0.000000 (zero before limits: the strategy's size "
              "converted to 0 mojos; no risk limit acted on it) | "
              "ask 44.569719 -> 0.000000 (zeroed by single_cat_cap; also reduced by "
              "soft_concentration) | "
              "base_conc=0.346 quote_conc=0.654 quote_cat_pct=0.506 (full block at 0.500) "
              "pair_pct=0.774 | strategy q=24.569719 q_max=20.000000 | "
              "cfg_soft=0.600 cfg_hard=0.800 cfg_cat=0.250 cfg_pair=0.850 "
              "(same-cause repeats log at debug for 192 blocks)");
}

TEST_F(LimitsTestFixture, EvaluateLimits_ZeroBidUnderFiringPairCap_NotAttributedToPairCap) {
    risk_cfg.single_cat_cap_pct = 0.95;   // keep the CAT rule out; the pair cap stays 0.20
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;
    seed_unit_rate_portfolio(state, {{"xch", 50}, {"wusdc", 50}, {"dbx", 100}});   // pair 0.5

    xop::Quote q{};
    q.bid_size = 0;
    q.ask_size = 1000;

    const xop::LimitsDecision d = ptc.evaluate_limits(q, "xch", "wusdc", state);
    ASSERT_TRUE(d.has_quote);
    EXPECT_EQ(d.quote.bid_size, 0);
    EXPECT_EQ(d.quote.ask_size, 644);   // the pair cap fired: keep 1 - 0.375 * 0.95
    // ...but the bid was already 0, so the pair cap did nothing to it.
    EXPECT_EQ(d.trace.bid.reduced_by, 0);
    EXPECT_EQ(d.trace.bid.zeroed_by, xop::LimitRule::None);
    EXPECT_EQ(xop::side_zero_cause(d.trace.bid), xop::SideZeroCause::ZeroBeforeLimits);
}

TEST_F(LimitsTestFixture, EvaluateLimits_LimitZeroedAskNotReattributedToLaterPairCap) {
    // The live XCH/BYC state with the pair cap lowered to 0.70, so the pair
    // cap fires AFTER the CAT cap has already zeroed the ask.  The zero stays
    // with the rule that caused it.
    risk_cfg.single_cat_cap_pct       = 0.25;
    risk_cfg.max_capital_per_pair_pct = 0.70;
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;
    seed_unit_rate_portfolio(state, {{"xch", 268}, {"byc", 506}, {"dbx", 226}});

    xop::Quote q{};
    q.bid_size = 0;
    q.ask_size = 44'569'719'126'041;

    const xop::LimitsDecision d = ptc.evaluate_limits(q, "xch", "byc", state);
    EXPECT_FALSE(d.has_quote);
    // The later rule's condition held (0.774 >= 0.70), so it did run.
    EXPECT_GE(d.trace.pair_capital_fraction, risk_cfg.max_capital_per_pair_pct);
    EXPECT_EQ(d.trace.ask.zeroed_by, xop::LimitRule::SingleCatCap);
    EXPECT_TRUE(xop::has_limit_rule(d.trace.ask.reduced_by, xop::LimitRule::SingleCatCap));
    EXPECT_FALSE(xop::has_limit_rule(d.trace.ask.reduced_by, xop::LimitRule::PairCapitalCap));
    EXPECT_EQ(d.trace.bid.reduced_by, 0);
    EXPECT_EQ(d.trace.bid.zeroed_by, xop::LimitRule::None);
}

TEST_F(LimitsTestFixture, EvaluateLimits_FullBaseConcentration_BidZeroedByHardLimit) {
    risk_cfg.max_capital_per_pair_pct = 0.95;
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;
    seed_unit_rate_portfolio(state, {{"xch", 100}, {"dbx", 100}});   // no wusdc: base_conc 1.0

    xop::Quote q{};
    q.bid_size = 1000;
    q.ask_size = 1000;

    const xop::LimitsDecision d = ptc.evaluate_limits(q, "xch", "wusdc", state);
    ASSERT_TRUE(d.has_quote);
    EXPECT_EQ(d.quote.bid_size, 0);
    EXPECT_EQ(d.quote.ask_size, 1000);
    EXPECT_EQ(d.trace.bid.pre_size, 1000);
    EXPECT_EQ(d.trace.bid.post_size, 0);
    EXPECT_EQ(d.trace.bid.zeroed_by, xop::LimitRule::HardConcentration);
    EXPECT_EQ(d.trace.bid.reduced_by, xop::limit_rule_bit(xop::LimitRule::HardConcentration));
    EXPECT_EQ(xop::side_zero_cause(d.trace.bid), xop::SideZeroCause::LimitZeroed);
    EXPECT_EQ(d.trace.ask.reduced_by, 0);
    EXPECT_EQ(d.trace.ask.post_size, 1000);
}

TEST_F(LimitsTestFixture, EvaluateLimits_SoftBaseConcentration_RecordedOnBid) {
    risk_cfg.max_capital_per_pair_pct = 0.95;
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;
    // base_conc 0.70 (soft band); wusdc 3% of the portfolio (< 0.12); pair 10%.
    seed_unit_rate_portfolio(state, {{"xch", 70}, {"wusdc", 30}, {"dbx", 900}});

    xop::Quote q{};
    q.bid_size = 1000;
    q.ask_size = 1000;

    const xop::LimitsDecision d = ptc.evaluate_limits(q, "xch", "wusdc", state);
    ASSERT_TRUE(d.has_quote);
    EXPECT_EQ(d.quote.bid_size, 525);   // keep 1 - 0.5 * 0.95 = 0.525
    EXPECT_EQ(d.quote.ask_size, 1000);
    EXPECT_EQ(d.trace.bid.reduced_by, xop::limit_rule_bit(xop::LimitRule::SoftConcentration));
    EXPECT_EQ(d.trace.bid.zeroed_by, xop::LimitRule::None);
    EXPECT_EQ(d.trace.bid.post_size, d.quote.bid_size);
    EXPECT_EQ(d.trace.ask.reduced_by, 0);
}

TEST_F(LimitsTestFixture, EvaluateLimits_HardQuoteConcentration_RecordedOnAsk) {
    risk_cfg.max_capital_per_pair_pct = 0.95;
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;
    // quote_conc 0.90 (past hard); wusdc 9% of the portfolio (< 0.12); pair 10%.
    seed_unit_rate_portfolio(state, {{"xch", 10}, {"wusdc", 90}, {"dbx", 900}});

    xop::Quote q{};
    q.bid_size = 1000;
    q.ask_size = 1000;

    const xop::LimitsDecision d = ptc.evaluate_limits(q, "xch", "wusdc", state);
    ASSERT_TRUE(d.has_quote);
    EXPECT_EQ(d.quote.ask_size, 25);   // 0.05 continuity floor x taper 0.5
    EXPECT_EQ(d.quote.bid_size, 1000);
    EXPECT_EQ(d.trace.ask.reduced_by, xop::limit_rule_bit(xop::LimitRule::HardConcentration));
    EXPECT_EQ(d.trace.ask.zeroed_by, xop::LimitRule::None);
    EXPECT_EQ(d.trace.ask.post_size, d.quote.ask_size);
    EXPECT_EQ(d.trace.bid.reduced_by, 0);
}

TEST_F(LimitsTestFixture, EvaluateLimits_BaseCatCap_RecordedOnBid) {
    risk_cfg.max_capital_per_pair_pct = 0.95;
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;
    // cat is 20% of the portfolio against the fixture's 12% cap; base_conc 0.5.
    seed_unit_rate_portfolio(state, {{"cat", 50}, {"xch", 50}, {"wusdc", 150}});

    xop::Quote q{};
    q.bid_size = 1000;
    q.ask_size = 1000;

    const xop::LimitsDecision d = ptc.evaluate_limits(q, "cat", "xch", state);
    ASSERT_TRUE(d.has_quote);
    EXPECT_TRUE(d.trace.base_is_cat);
    EXPECT_NEAR(d.trace.base_cat_fraction, 0.2, 1e-12);
    EXPECT_FALSE(d.trace.quote_is_cat);
    EXPECT_EQ(d.quote.bid_size, 17);   // 0.05 continuity floor x taper 1/3
    EXPECT_EQ(d.quote.ask_size, 1000);
    EXPECT_EQ(d.trace.bid.reduced_by, xop::limit_rule_bit(xop::LimitRule::SingleCatCap));
    EXPECT_EQ(d.trace.bid.zeroed_by, xop::LimitRule::None);
    EXPECT_EQ(d.trace.bid.post_size, d.quote.bid_size);
    EXPECT_EQ(d.trace.ask.reduced_by, 0);
}

TEST_F(LimitsTestFixture, EvaluateLimits_PairCapitalCap_RecordedOnBothSides) {
    risk_cfg.single_cat_cap_pct = 0.95;   // the pair cap stays at the fixture's 0.20
    xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
    xop::State state;
    seed_unit_rate_portfolio(state, {{"xch", 50}, {"wusdc", 50}, {"dbx", 100}});

    xop::Quote q{};
    q.bid_size = 1000;
    q.ask_size = 1000;

    const xop::LimitsDecision d = ptc.evaluate_limits(q, "xch", "wusdc", state);
    ASSERT_TRUE(d.has_quote);
    EXPECT_EQ(d.quote.bid_size, 644);   // keep 1 - 0.375 * 0.95 = 0.64375
    EXPECT_EQ(d.quote.ask_size, 644);
    EXPECT_EQ(d.trace.bid.reduced_by, xop::limit_rule_bit(xop::LimitRule::PairCapitalCap));
    EXPECT_EQ(d.trace.ask.reduced_by, xop::limit_rule_bit(xop::LimitRule::PairCapitalCap));
    EXPECT_EQ(d.trace.bid.zeroed_by, xop::LimitRule::None);
    EXPECT_EQ(d.trace.ask.zeroed_by, xop::LimitRule::None);
    EXPECT_EQ(d.trace.bid.post_size, d.quote.bid_size);
    EXPECT_EQ(d.trace.ask.post_size, d.quote.ask_size);
    EXPECT_NEAR(d.trace.pair_capital_fraction, 0.5, 1e-12);

    // apply_limits delegates: the legacy interface returns the same quote,
    // which ties the older ApplyLimits* tests to the path the engine calls.
    const auto legacy = ptc.apply_limits(q, "XCH/wUSDC.b", "xch", "wusdc", state);
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(legacy->bid_size, d.quote.bid_size);
    EXPECT_EQ(legacy->ask_size, d.quote.ask_size);
}

TEST(DescribeSideLimits, StrategyZeroAndLimitZeroReadDifferently) {
    const std::int64_t kXch = 1'000'000'000'000;

    // (1) Arrived at 0: nothing a limit did.
    const xop::SideLimitTrace strategy_zero{};
    EXPECT_EQ(xop::describe_side_limits(strategy_zero, kXch),
              "0.000000 -> 0.000000 (zero before limits: the strategy's size "
              "converted to 0 mojos; no risk limit acted on it)");

    // (2) Cut from 44.57 XCH to 0 by the CAT cap after the soft band.
    xop::SideLimitTrace cat_zeroed{};
    cat_zeroed.pre_size   = 44'569'719'126'041;
    cat_zeroed.post_size  = 0;
    cat_zeroed.reduced_by = rule_mask(xop::LimitRule::SoftConcentration,
                                      xop::LimitRule::SingleCatCap);
    cat_zeroed.zeroed_by  = xop::LimitRule::SingleCatCap;
    EXPECT_EQ(xop::describe_side_limits(cat_zeroed, kXch),
              "44.569719 -> 0.000000 (zeroed by single_cat_cap; also reduced by "
              "soft_concentration)");

    // (3) Reduced by two rules and still positive.
    xop::SideLimitTrace reduced{};
    reduced.pre_size   = 2'000'000'000'000;
    reduced.post_size  = 1'000'000'000'000;
    reduced.reduced_by = rule_mask(xop::LimitRule::SoftConcentration,
                                   xop::LimitRule::PairCapitalCap);
    EXPECT_EQ(xop::describe_side_limits(reduced, kXch),
              "2.000000 -> 1.000000 (reduced by soft_concentration+pair_capital_cap)");

    // (4) Untouched.
    xop::SideLimitTrace untouched{};
    untouched.pre_size  = 1'000'000'000'000;
    untouched.post_size = 1'000'000'000'000;
    EXPECT_EQ(xop::describe_side_limits(untouched, kXch),
              "1.000000 -> 1.000000 (no limit applied)");
}

TEST(LimitBlockWarnGate, WarnsOnFirstBlockThenOnlyOnReminder) {
    // Heartbeats where the pair quotes never reach should_warn(), so a pair
    // flapping between a quote and a same-cause block also warns at most
    // once per window.
    const xop::BlockHeight reminder = xop::kLimitBlockWarnReminderBlocks;
    const xop::BlockHeight h0 = 9'300'000;
    const xop::LimitBlockSignature cat_block = xch_byc_cat_block();

    xop::LimitBlockWarnGate gate;
    EXPECT_TRUE(gate.should_warn(cat_block, h0, reminder));                   // first block
    EXPECT_FALSE(gate.should_warn(cat_block, h0 + 1, reminder));              // same cause, next block
    EXPECT_FALSE(gate.should_warn(cat_block, h0 + reminder - 1, reminder));   // one short of the window
    EXPECT_TRUE(gate.should_warn(cat_block, h0 + reminder, reminder));        // the reminder
    EXPECT_FALSE(gate.should_warn(cat_block, h0 + reminder + 1, reminder));   // the window restarts there
}

TEST(LimitBlockWarnGate, CauseChangeWarnsImmediately) {
    const xop::BlockHeight reminder = xop::kLimitBlockWarnReminderBlocks;
    const xop::BlockHeight h0 = 9'300'000;
    const xop::LimitBlockSignature cat_block = xch_byc_cat_block();
    xop::LimitBlockSignature hard_block = cat_block;
    hard_block.ask_zeroed_by = xop::LimitRule::HardConcentration;

    xop::LimitBlockWarnGate gate;
    EXPECT_TRUE(gate.should_warn(cat_block, h0, reminder));
    EXPECT_TRUE(gate.should_warn(hard_block, h0 + 1, reminder));    // a new cause warns at once
    EXPECT_FALSE(gate.should_warn(hard_block, h0 + 2, reminder));   // ...then is gated as usual
    EXPECT_TRUE(gate.should_warn(cat_block, h0 + 3, reminder));     // changing back is a change too
}

TEST(LimitBlockWarnGate, RecoveryLineLogsOncePerWarn) {
    // With repeats rate-limited, the warn going quiet no longer means the
    // pair quotes again, so the first quote after a warn is announced --
    // once per warn, so a flapping pair cannot turn it into spam.
    const xop::BlockHeight reminder = xop::kLimitBlockWarnReminderBlocks;
    const xop::BlockHeight h0 = 9'300'000;
    const xop::LimitBlockSignature cat_block = xch_byc_cat_block();

    xop::LimitBlockWarnGate gate;
    EXPECT_FALSE(gate.should_log_recovery());                            // never warned: nothing to announce
    EXPECT_TRUE(gate.should_warn(cat_block, h0, reminder));
    EXPECT_TRUE(gate.should_log_recovery());                             // first quote after the warn
    EXPECT_EQ(xop::format_step6_quote_resumed("XCH/BYC", gate.last_warn_height(), reminder),
              "Step 6: XCH/BYC -- limits let a quote through again after the no-quote "
              "warn at height 9300000; a same-cause block before height 9300192 logs "
              "at debug only");
    EXPECT_FALSE(gate.should_log_recovery());                            // still quoting: said once
    EXPECT_FALSE(gate.should_warn(cat_block, h0 + 2, reminder));         // same-cause re-block in the window
    EXPECT_FALSE(gate.should_log_recovery());                            // nothing warned since: quiet
    EXPECT_TRUE(gate.should_warn(cat_block, h0 + reminder, reminder));   // the reminder warns...
    EXPECT_TRUE(gate.should_log_recovery());                             // ...and re-arms the line
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
