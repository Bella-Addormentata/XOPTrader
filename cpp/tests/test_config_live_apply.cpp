// ---------------------------------------------------------------------------
// [LIVECFG] What a saved config does to a running engine.
//
// The contract under test: every setting applies live EXCEPT the
// restart-only list in config_live_apply.hpp, and each of those is named in
// restart_required and held at its running value -- never silently applied,
// never silently dropped. Plus the subsystem setters the reload drives: each
// must adopt the new values while keeping the state it has learned.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/config_live_apply.hpp"
#include "xop/strategy/competitiveness_pid.hpp"
#include "xop/strategy/depeg_detector.hpp"
#include "xop/strategy/fee_tracker.hpp"
#include "xop/strategy/kappa_calibrator.hpp"
#include "xop/strategy/liquidity.hpp"
#include "xop/execution/offer_manager.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

xop::PairConfig pair(const std::string& name, bool enabled = true)
{
    xop::PairConfig p;
    p.name           = name;
    p.base_asset_id  = "xch";
    p.quote_asset_id = name + "-cat";
    p.enabled        = enabled;
    return p;
}

xop::AppConfig running_config()
{
    xop::AppConfig c;
    c.chia.wallet_host  = "localhost";
    c.chia.wallet_port  = 9256;
    c.database.path     = "data/xop_trader.db";
    c.monitoring.prometheus_port = 9090;
    c.risk.watchdog_stall_seconds = 600;
    c.pairs = {pair("XCH/A"), pair("XCH/B"), pair("XCH/C", false)};
    return c;
}

bool lists(const std::vector<std::string>& v, const std::string& what)
{
    return std::find(v.begin(), v.end(), what) != v.end();
}

}  // namespace

TEST(ConfigLiveApply, an_unchanged_save_needs_no_restart)
{
    const auto running = running_config();
    const auto plan = xop::plan_live_config(running, running);
    EXPECT_TRUE(plan.restart_required.empty());
    ASSERT_EQ(plan.merged.pairs.size(), running.pairs.size());
}

TEST(ConfigLiveApply, ordinary_settings_are_adopted_without_a_restart)
{
    const auto running = running_config();
    auto saved = running;
    saved.strategy.gamma               = running.strategy.gamma * 2.0;
    saved.strategy.min_profit_margin_bps = 77.0;
    saved.risk.max_drawdown_frac       = 0.07;
    saved.fees.max_fee_mojos           = running.fees.max_fee_mojos + 1;
    saved.monitoring.telegram_chat_id  = "123";
    saved.dexie.claim_rewards          = !running.dexie.claim_rewards;
    saved.engine.shutdown_offers       = xop::util::StopOffersPolicy::Keep;
    saved.pairs[0].gamma_override      = 0.5;

    const auto plan = xop::plan_live_config(running, saved);
    EXPECT_TRUE(plan.restart_required.empty());
    EXPECT_DOUBLE_EQ(plan.merged.strategy.gamma, saved.strategy.gamma);
    EXPECT_DOUBLE_EQ(plan.merged.strategy.min_profit_margin_bps, 77.0);
    EXPECT_DOUBLE_EQ(plan.merged.risk.max_drawdown_frac, 0.07);
    EXPECT_EQ(plan.merged.fees.max_fee_mojos, saved.fees.max_fee_mojos);
    EXPECT_EQ(plan.merged.monitoring.telegram_chat_id, "123");
    EXPECT_EQ(plan.merged.dexie.claim_rewards, saved.dexie.claim_rewards);
    EXPECT_EQ(plan.merged.engine.shutdown_offers,
              xop::util::StopOffersPolicy::Keep);
    ASSERT_TRUE(plan.merged.pairs[0].gamma_override.has_value());
    EXPECT_DOUBLE_EQ(*plan.merged.pairs[0].gamma_override, 0.5);
}

TEST(ConfigLiveApply, connections_keep_their_running_values_and_are_named)
{
    const auto running = running_config();
    auto saved = running;
    saved.chia.wallet_port            = 9999;
    saved.dexie.api_base              = "https://example.invalid/v1";
    saved.database.path               = "elsewhere.db";
    saved.monitoring.prometheus_port  = 9191;
    saved.risk.watchdog_stall_seconds = 60;
    saved.strategy.gamma              = running.strategy.gamma * 3.0;

    const auto plan = xop::plan_live_config(running, saved);
    EXPECT_TRUE(lists(plan.restart_required, "chia wallet host/port"));
    EXPECT_TRUE(lists(plan.restart_required, "dexie.api_base"));
    EXPECT_TRUE(lists(plan.restart_required, "database.path"));
    EXPECT_TRUE(lists(plan.restart_required, "monitoring.prometheus_port"));
    EXPECT_TRUE(lists(plan.restart_required, "risk.watchdog_stall_seconds"));

    EXPECT_EQ(plan.merged.chia.wallet_port, running.chia.wallet_port);
    EXPECT_EQ(plan.merged.dexie.api_base, running.dexie.api_base);
    EXPECT_EQ(plan.merged.database.path, running.database.path);
    EXPECT_EQ(plan.merged.monitoring.prometheus_port, running.monitoring.prometheus_port);
    EXPECT_EQ(plan.merged.risk.watchdog_stall_seconds,
              running.risk.watchdog_stall_seconds);
    // ...and the rest of the same save still applies.
    EXPECT_DOUBLE_EQ(plan.merged.strategy.gamma, saved.strategy.gamma);
}

TEST(ConfigLiveApply, a_disable_applies_an_enable_waits)
{
    const auto running = running_config();
    auto saved = running;
    saved.pairs[1].enabled = false;   // XCH/B off: live
    saved.pairs[2].enabled = true;    // XCH/C on: restart

    const auto plan = xop::plan_live_config(running, saved);
    ASSERT_EQ(plan.pairs.to_disable.size(), 1u);
    EXPECT_EQ(plan.pairs.to_disable[0], "XCH/B");
    EXPECT_FALSE(plan.merged.pairs[1].enabled);
    EXPECT_FALSE(plan.merged.pairs[2].enabled)
        << "an enable must never reach the running config: the pair has no "
           "strategy state in this process";
    EXPECT_TRUE(lists(plan.restart_required, "pair XCH/C enabled"));
}

TEST(ConfigLiveApply, the_pair_list_keeps_the_running_shape)
{
    // Other structures hold pointers into config_.pairs, so the merged list
    // must be the running list, in the running order, whatever the file did.
    const auto running = running_config();
    auto saved = running;
    std::reverse(saved.pairs.begin(), saved.pairs.end());
    saved.pairs.erase(saved.pairs.begin());          // drop XCH/C
    saved.pairs.push_back(pair("XCH/NEW"));
    saved.pairs.back().gamma_override = 0.9;

    const auto plan = xop::plan_live_config(running, saved);
    ASSERT_EQ(plan.merged.pairs.size(), running.pairs.size());
    for (std::size_t i = 0; i < running.pairs.size(); ++i) {
        EXPECT_EQ(plan.merged.pairs[i].name, running.pairs[i].name);
    }
    EXPECT_TRUE(lists(plan.restart_required, "pair XCH/C removed"));
    EXPECT_TRUE(lists(plan.restart_required, "pair XCH/NEW added"));
}

TEST(ConfigLiveApply, a_pairs_market_identity_is_restart_only)
{
    const auto running = running_config();
    auto saved = running;
    saved.pairs[0].quote_asset_id      = "a-different-cat";
    saved.pairs[0].quote_mojos_per_unit = 1;
    saved.pairs[0].min_profit_margin_bps_override = 12.0;

    const auto plan = xop::plan_live_config(running, saved);
    EXPECT_TRUE(lists(plan.restart_required, "pair XCH/A asset ids/units"));
    EXPECT_EQ(plan.merged.pairs[0].quote_asset_id, running.pairs[0].quote_asset_id);
    EXPECT_EQ(plan.merged.pairs[0].quote_mojos_per_unit,
              running.pairs[0].quote_mojos_per_unit);
    ASSERT_TRUE(plan.merged.pairs[0].min_profit_margin_bps_override.has_value());
    EXPECT_DOUBLE_EQ(*plan.merged.pairs[0].min_profit_margin_bps_override, 12.0);
}

// -- Subsystem setters ------------------------------------------------------

TEST(ConfigLiveApplySetters, comp_pid_keeps_its_memory_and_reclamps)
{
    xop::strategy::CompetitivenessPidConfig cfg;
    cfg.enabled       = true;
    cfg.warmup_blocks = 0;
    cfg.min_offset    = -3;
    cfg.max_offset    = 3;
    xop::strategy::CompetitivenessPid pid{cfg};
    for (int i = 0; i < 200; ++i) pid.observe_block(false);   // underfilling
    ASSERT_LT(pid.current_offset(), -1);
    const auto blocks = pid.blocks_active();

    cfg.min_offset = -1;
    pid.set_config(cfg);
    EXPECT_EQ(pid.current_offset(), -1) << "re-clamped into the new bounds";
    EXPECT_EQ(pid.blocks_active(), blocks) << "learned state kept";
}

TEST(ConfigLiveApplySetters, depeg_sync_keeps_a_streak_and_adopts_thresholds)
{
    xop::PairConfig pc;
    pc.name                   = "BYC/wUSDC.b";
    pc.is_stablecoin          = true;
    pc.peg_target             = 1.0;
    pc.depeg_warn_pct         = 2.0;
    pc.depeg_bail_pct         = 10.0;
    pc.depeg_sustained_blocks = 3;
    xop::DepegDetector d{xop::DepegConfig{}};
    d.register_pair(pc);
    d.update(pc.name, 0.50, 1u);
    d.update(pc.name, 0.50, 2u);

    pc.depeg_sustained_blocks = 3;
    pc.depeg_bail_pct         = 20.0;
    d.sync_pair(pc);
    // The streak survived the sync: the third deep print still bails.
    EXPECT_EQ(d.update(pc.name, 0.50, 3u), xop::DepegStatus::Bailed);

    pc.is_stablecoin = false;
    d.sync_pair(pc);
    EXPECT_EQ(d.update(pc.name, 0.50, 4u), xop::DepegStatus::Normal)
        << "a pair no longer marked stablecoin is no longer monitored";
}

TEST(ConfigLiveApplySetters, kappa_default_moves_until_fills_take_over)
{
    xop::KappaCalibratorConfig kc;
    kc.default_kappa = 1.5;
    xop::KappaCalibrator cal{kc};
    cal.set_default_kappa(2.5);
    EXPECT_DOUBLE_EQ(cal.current_kappa(), 2.5);
    EXPECT_DOUBLE_EQ(cal.config().default_kappa, 2.5);
}

TEST(ConfigLiveApplySetters, fee_controller_restarts_only_when_its_inputs_change)
{
    xop::FeeConfig f;
    xop::FeeTracker t{f};
    auto same_inputs = f;
    same_inputs.daily_budget_mojos = f.daily_budget_mojos + 1;
    EXPECT_FALSE(t.set_config(same_inputs));
    auto new_bounds = same_inputs;
    new_bounds.max_fee_mojos = same_inputs.max_fee_mojos + 1;
    EXPECT_TRUE(t.set_config(new_bounds));
}

TEST(ConfigLiveApplySetters, liquidity_refuses_a_bad_config_and_keeps_the_old)
{
    xop::LiquidityConfig good;
    good.num_tiers        = 2;
    good.tier_spacing_bps = {10.0, 20.0};
    good.tier_size_pct    = {0.5, 0.5};
    xop::LiquidityEngine le{"XCH/A", good};

    auto bad = good;
    bad.tier_spacing_bps = {10.0};   // length no longer matches num_tiers
    EXPECT_THROW(le.set_config(bad), std::invalid_argument);
    EXPECT_EQ(le.config().tier_spacing_bps.size(), 2u);
}

TEST(ConfigLiveApplySetters, offer_manager_validation_matches_its_constructor)
{
    xop::AppConfig cfg;
    cfg.strategy.num_tiers        = 2;
    cfg.strategy.tier_spacing_bps = {10.0, 20.0};
    cfg.strategy.tier_size_pct    = {0.5};
    EXPECT_THROW(xop::execution::OfferManager::validate_config(cfg),
                 std::invalid_argument);
}
