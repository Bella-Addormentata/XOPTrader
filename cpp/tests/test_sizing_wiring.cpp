// test_sizing_wiring.cpp -- source scans of Step 7's minimum-size and reserve
// glue ([PAIRMIN 2026-10-26]), and of Step 4's target-gap bid floor
// ([TARGET-GAP 2026-10-26]).
//
// wmilliETH.b/XCH posted no offers on 2026-10-26: the deploy-idle floor sized
// a pool to the GLOBAL strategy.min_offer_size_units whatever the pair's
// override said, and Step 7 sized a CAT with no asset_reserve_units entry to
// its whole balance while Step 8's reserve gates held strategy.min_reserve_units
// back -- so the only ask breached the reserve and its whole side was
// suppressed every cycle.  The arithmetic is unit-tested in test_config.cpp
// (effective_min_offer_units) and test_asset_reserve.cpp
// (sizing_reserve_units); these scans pin that engine.cpp calls it, and where.
// Like test_pace_wiring.cpp, each scan is mutation-checked: it fails against
// the engine.cpp this change replaced.

#include <gtest/gtest.h>

#include "engine_source_scan.hpp"

#include <cstddef>
#include <string>

namespace {

using xop_tests::engine_scan::count_of;
using xop_tests::engine_scan::function_body;
using xop_tests::engine_scan::position_of;
using xop_tests::engine_scan::read_engine_cpp;
using xop_tests::engine_scan::squeeze;

const char* const kLadderSignature = "void Engine::step_generate_ladder(BlockHeight block_height)";
const char* const kReviveSignature =
    "asio::awaitable<void> Engine::step_revive_maintenance(BlockHeight block_height)";

const char* const kPairMinCall =
    "effective_min_offer_units(*pair_cfg, config_.strategy.min_offer_size_units)";
const char* const kSizingReserveCall =
    "execution::sizing_reserve_units(config_.strategy.asset_reserve_units, "
    "config_.strategy.min_reserve_units, config_.strategy.wallet_balance_caps_enabled, asset_id)";

}  // namespace

TEST(SizingWiring, DeployIdleFloorUsesThePairsMinimum) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(body, "const double floor_min_units = pair_cfg ? "
                                "effective_min_offer_units(*pair_cfg, config_.strategy.min_offer_size_units) : 0.0;"),
              std::string::npos);
    EXPECT_NE(position_of(body, "&& floor_min_units > 0.0)"), std::string::npos);
    EXPECT_NE(position_of(body, "const Mojo min_pool = static_cast<Mojo>(std::llround("
                                "floor_min_units * static_cast<double>(pair_cfg->base_mojos_per_unit)));"),
              std::string::npos);
    EXPECT_NE(position_of(body, "const double required_quote_units = floor_min_units * market_mid;"),
              std::string::npos);
    // The global minimum reaches Step 7 only through the helper: the floor
    // and the up-scale, nowhere else.
    EXPECT_EQ(count_of(body, kPairMinCall), 2u);
    EXPECT_EQ(count_of(body, "config_.strategy.min_offer_size_units"), count_of(body, kPairMinCall));
}

TEST(SizingWiring, UpScaleAndDustFilterUseThePairsMinimum) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t floor_at = position_of(body, "const double floor_min_units =");
    const std::size_t upscale_at =
        position_of(body, std::string("const double eff_min_units = ") + kPairMinCall + ";");
    ASSERT_NE(floor_at, std::string::npos);
    ASSERT_NE(upscale_at, std::string::npos);
    EXPECT_LT(floor_at, upscale_at);
    EXPECT_EQ(position_of(body, "min_offer_size_units_override.value_or"), std::string::npos);
}

TEST(SizingWiring, Step7SizesEveryCatToLeaveTheStep8Reserve) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    // The final caps and the post-up-scale budget filter, in that order,
    // after the floor and around the up-scale.
    EXPECT_EQ(count_of(body, kSizingReserveCall), 2u);
    const std::size_t floor_at = position_of(body, "const double floor_min_units =");
    const std::size_t caps_at = position_of(body, kSizingReserveCall);
    const std::size_t upscale_at = position_of(body, "const double eff_min_units =");
    const std::size_t funding_at = position_of(body, "[S3 rounds 5+7] Re-enforce EVERY side's funding budget");
    ASSERT_NE(floor_at, std::string::npos);
    ASSERT_NE(caps_at, std::string::npos);
    ASSERT_NE(upscale_at, std::string::npos);
    ASSERT_NE(funding_at, std::string::npos);
    EXPECT_LT(floor_at, caps_at);
    EXPECT_LT(caps_at, upscale_at);
    const std::size_t budget_at = body.find(squeeze(kSizingReserveCall), caps_at + 1);
    ASSERT_NE(budget_at, std::string::npos);
    EXPECT_LT(funding_at, budget_at);
    // Neither site is limited to assets with an asset_reserve_units entry.
    EXPECT_EQ(position_of(body, "if (pair_cfg && !config_.strategy.asset_reserve_units.empty())"),
              std::string::npos);
    EXPECT_EQ(position_of(body, "config_.strategy.asset_reserve_units.find("), std::string::npos);
}

TEST(SizingWiring, ReviveMaintenanceUsesThePairsMinimum) {
    const std::string body = function_body(read_engine_cpp(), kReviveSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(body, "const double min_units = "
                                "effective_min_offer_units(pc, config_.strategy.min_offer_size_units);"),
              std::string::npos);
    EXPECT_EQ(position_of(body, "min_offer_size_units_override"), std::string::npos);
}

// -- [TARGET-GAP 2026-10-26] Step 4's target-gap bid floor --------------------
// XCH/wUSDC.b posted nothing on 2026-10-26: the strategy's bid was q_max 20 -
// 34.76 XCH held = 0 while XCH sat at 13.8% against a 50% target.  The floor's
// arithmetic is unit-tested in test_target_gap.cpp; these scans pin that Step 4
// values the portfolio only when the switch is on and only from validated
// balances, applies the floor after the strategy and before anything reads the
// size, and only ever raises the bid.

namespace {

const char* const kComputeQuotesSignature = "void Engine::step_compute_quotes(BlockHeight block_height)";
const char* const kFloorCall =
    "strategy::target_gap::bid_floor_units(gap_valuation, gap_base_key, gap_quote_key, gap_base_xch_per_unit)";

}  // namespace

TEST(SizingWiring, TargetGapValuationIsGatedAndValidated) {
    const std::string body = function_body(read_engine_cpp(), kComputeQuotesSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t gate_at = position_of(body, "if (config_.strategy.target_gap_sizing_enabled) {");
    const std::size_t value_at =
        position_of(body, "gap_valuation = strategy::target_gap::value_targeted(gap_holdings);");
    const std::size_t loop_at = position_of(body, "for (auto& [pair_name, pcs] : cycle_) {");
    ASSERT_NE(gate_at, std::string::npos);
    ASSERT_NE(value_at, std::string::npos);
    ASSERT_NE(loop_at, std::string::npos);
    EXPECT_LT(gate_at, value_at);
    EXPECT_LT(value_at, loop_at) << "valued once per heartbeat, not per pair";
    // Every targeted coin, from the balances Step 8 validated, at the marks
    // the drift guard reads; a balance with no mark is unvalued.
    EXPECT_NE(position_of(body, "for (const auto& [gap_key, gap_target] : config_.strategy.asset_target_allocations)"),
              std::string::npos);
    EXPECT_NE(position_of(body, "gap_cached != cached_wallet_balances_.end() && gap_cached->second.fields_validated"),
              std::string::npos);
    EXPECT_NE(position_of(body, "const Mojo gap_confirmed = gap_cached->second.confirmed;"), std::string::npos);
    EXPECT_NE(position_of(body, "gap_holding.valued = gap_confirmed == 0 || gap_rate > 0.0;"), std::string::npos);
    EXPECT_EQ(count_of(body, "value_targeted("), 1u);
}

TEST(SizingWiring, TargetGapFloorFollowsTheStrategyAndOnlyRaisesTheBid) {
    const std::string body = function_body(read_engine_cpp(), kComputeQuotesSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t strategy_at =
        position_of(body, "pcs.raw_quote = strategy.compute_quotes(mid, sigma, q, block_height);");
    const std::size_t gate_at =
        position_of(body, "if (config_.strategy.target_gap_sizing_enabled && gap_valuation.ok) {");
    const std::size_t floor_at = position_of(body, kFloorCall);
    const std::size_t raise_at = position_of(body, "if (gap_floor > 0.0 && gap_floor > pcs.raw_quote.bid_size) {");
    const std::size_t assign_at = position_of(body, "pcs.raw_quote.bid_size = gap_floor;");
    const std::size_t kept_at = position_of(body, "pcs.strategy_q = q;");
    ASSERT_NE(strategy_at, std::string::npos);
    ASSERT_NE(gate_at, std::string::npos);
    ASSERT_NE(floor_at, std::string::npos);
    ASSERT_NE(raise_at, std::string::npos);
    ASSERT_NE(assign_at, std::string::npos);
    ASSERT_NE(kept_at, std::string::npos);
    EXPECT_LT(strategy_at, gate_at);
    EXPECT_LT(gate_at, floor_at);
    EXPECT_LT(floor_at, raise_at);
    EXPECT_LT(raise_at, assign_at);
    EXPECT_LT(assign_at, kept_at) << "before the Step 4 log and every later step";
    // The base coin's XCH per unit at the mark the valuation used.
    EXPECT_NE(position_of(body, "state_->get_asset_xch_rate(AssetId{pair_cfg->base_asset_id}) "
                                "* static_cast<double>(pair_cfg->base_mojos_per_unit) "
                                "/ static_cast<double>(kMojosPerXch);"),
              std::string::npos);
    // The floor is the only write to the sizes in Step 4, and the ask is
    // never touched: the strategy sizes it q_max + q.
    EXPECT_EQ(count_of(body, kFloorCall), 1u);
    EXPECT_EQ(count_of(body, "pcs.raw_quote.bid_size ="), 1u);
    EXPECT_EQ(count_of(body, "pcs.raw_quote.ask_size ="), 0u);
}
