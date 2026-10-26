// test_sizing_wiring.cpp -- source scans of Step 7's minimum-size and reserve
// glue ([PAIRMIN 2026-10-26]).
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
