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
// values the portfolio only when the switch is on and only from validated,
// fresh balances, applies the floor once every pair is quoted and before
// anything reads the size, shares each coin's gap across the pairs, skips a
// quote the strategy rejected, only ever raises the bid, and that the pace
// balance refresh keeps the targeted balances fresh while the floor is on.

namespace {

const char* const kComputeQuotesSignature = "void Engine::step_compute_quotes(BlockHeight block_height)";
const char* const kHeartbeatSignature = "asio::awaitable<void> Engine::on_new_block_coro(BlockHeight block_height)";
const char* const kStep4Log = "spdlog::debug(\"[Engine] Step 4: {} bid={:.6f} ask={:.6f} spread={:.1f}bps \"";
const char* const kValuationCall =
    "strategy::target_gap::value_targeted(gap_holdings, config_.strategy.pace_max_balance_age_blocks);";
const char* const kSharedCall = "strategy::target_gap::shared_bid_floors_xch(gap_valuation, gap_pairs);";

}  // namespace

TEST(SizingWiring, TargetGapValuationIsGatedValidatedAndFresh) {
    const std::string body = function_body(read_engine_cpp(), kComputeQuotesSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t log_at = position_of(body, kStep4Log);
    const std::size_t gate_at = position_of(body, "if (!config_.strategy.target_gap_sizing_enabled) {");
    const std::size_t value_at = position_of(body, kValuationCall);
    ASSERT_NE(log_at, std::string::npos);
    ASSERT_NE(gate_at, std::string::npos);
    ASSERT_NE(value_at, std::string::npos);
    EXPECT_LT(log_at, gate_at) << "after the pair loop: once per heartbeat";
    EXPECT_LT(gate_at, value_at);
    // Every targeted coin, from the balances Step 8 and the pace refresh
    // validated, with their age, at the marks the drift guard reads; a
    // balance with no mark is unpriced.
    EXPECT_NE(position_of(body, "for (const auto& [gap_key, gap_target] : config_.strategy.asset_target_allocations)"),
              std::string::npos);
    EXPECT_NE(position_of(body, "gap_cached != cached_wallet_balances_.end() && gap_cached->second.fields_validated"),
              std::string::npos);
    EXPECT_NE(position_of(body, "const Mojo gap_confirmed = gap_cached->second.confirmed;"), std::string::npos);
    EXPECT_NE(position_of(body, "gap_holding.balance_known = true;"), std::string::npos);
    EXPECT_NE(position_of(body, "gap_holding.balance_age_blocks = block_height > gap_cached->second.as_of_block "
                                "? block_height - gap_cached->second.as_of_block : 0u;"),
              std::string::npos);
    EXPECT_NE(position_of(body, "gap_holding.priced = gap_confirmed == 0 || gap_rate > 0.0;"), std::string::npos);
    EXPECT_EQ(count_of(body, "value_targeted("), 1u);
    // An unusable valuation posts no floor.
    const std::size_t off_at = position_of(body, "if (!gap_valuation.ok) { return; }");
    ASSERT_NE(off_at, std::string::npos);
    EXPECT_LT(value_at, off_at);
}

TEST(SizingWiring, TargetGapFloorIsSharedAndOnlyRaisesAUsableBid) {
    const std::string body = function_body(read_engine_cpp(), kComputeQuotesSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t strategy_at =
        position_of(body, "pcs.raw_quote = strategy.compute_quotes(mid, sigma, q, block_height);");
    const std::size_t log_at = position_of(body, kStep4Log);
    const std::size_t usable_at = position_of(body, "if (!pcs.quote_valid || !std::isfinite(pcs.raw_quote.spread_bps) "
                                                    "|| !(pcs.raw_quote.spread_bps > 0.0)) { continue; }");
    const std::size_t claim_at = position_of(body, "gap_pairs.push_back(std::move(gap_coins));");
    const std::size_t shared_at = position_of(body, kSharedCall);
    const std::size_t units_at = position_of(body,
        "strategy::target_gap::floor_units(gap_floors_xch[gap_i], gap_base_xch_per_unit);");
    const std::size_t raise_at = position_of(body, "if (gap_floor > 0.0 && gap_floor > gap_pcs.raw_quote.bid_size) {");
    const std::size_t assign_at = position_of(body, "gap_pcs.raw_quote.bid_size = gap_floor;");
    ASSERT_NE(strategy_at, std::string::npos);
    ASSERT_NE(log_at, std::string::npos);
    ASSERT_NE(usable_at, std::string::npos);
    ASSERT_NE(claim_at, std::string::npos);
    ASSERT_NE(shared_at, std::string::npos);
    ASSERT_NE(units_at, std::string::npos);
    ASSERT_NE(raise_at, std::string::npos);
    ASSERT_NE(assign_at, std::string::npos);
    EXPECT_LT(strategy_at, log_at);
    EXPECT_LT(log_at, usable_at) << "every pair is quoted before any floor";
    EXPECT_LT(usable_at, claim_at) << "a rejected quote claims no share";
    EXPECT_LT(claim_at, shared_at);
    EXPECT_LT(shared_at, units_at);
    EXPECT_LT(units_at, raise_at);
    EXPECT_LT(raise_at, assign_at);
    // The base coin's XCH per unit at the mark the valuation used.
    EXPECT_NE(position_of(body, "state_->get_asset_xch_rate(AssetId{gap_pc->base_asset_id}) "
                                "* static_cast<double>(gap_pc->base_mojos_per_unit) "
                                "/ static_cast<double>(kMojosPerXch);"),
              std::string::npos);
    // The shared floors are the only ones: no pair takes an unshared floor.
    EXPECT_EQ(count_of(body, "shared_bid_floors_xch("), 1u);
    EXPECT_EQ(position_of(body, "unshared_bid_floor_xch("), std::string::npos);
    // The floor is the only write to the sizes in Step 4, and the ask is
    // never touched: the strategy sizes it q_max + q.
    EXPECT_EQ(count_of(body, "raw_quote.bid_size ="), 1u);
    EXPECT_EQ(count_of(body, "raw_quote.ask_size ="), 0u);
}

TEST(SizingWiring, TheBalanceRefreshRunsWhileTheTargetGapFloorIsOn) {
    const std::string body = function_body(read_engine_cpp(), kHeartbeatSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(body, "if (config_.strategy.pace_enabled || config_.strategy.target_gap_sizing_enabled) { "
                                "try { co_await refresh_pace_balances(block_height); }"),
              std::string::npos);
    // The refresh's own gate is pinned with its other gates in
    // test_pace_wiring.cpp (PaceWiring.BalanceRefreshClosedByEveryPostingGate).
}
