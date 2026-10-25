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
#include "xop/execution/market_data.hpp"
#include "xop/data/volatility.hpp"
#include "xop/data/adverse_selection.hpp"
#include "xop/state.hpp"

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

TEST(ConfigLiveApply, startup_analysis_length_waits_for_the_next_start)
{
    // The observe-before-quoting phase ran once, before the first heartbeat.
    const auto running = running_config();
    auto saved = running;
    saved.strategy.startup_analysis_blocks = running.strategy.startup_analysis_blocks + 5;
    const auto plan = xop::plan_live_config(running, saved);
    EXPECT_TRUE(lists(plan.restart_required, "strategy.startup_analysis_blocks"));
    EXPECT_EQ(plan.merged.strategy.startup_analysis_blocks,
              running.strategy.startup_analysis_blocks);
}

TEST(ConfigLiveApply, the_merged_config_is_held_to_the_cross_section_rules)
{
    // The saved file is valid on its own and so is the running config, but
    // their MERGE (a pair the file removed keeps running with its own cap,
    // under the file's raised global limits) breaks soft < hard for it.
    auto running = running_config();
    running.risk.soft_limit_pct = 0.60;
    running.risk.hard_limit_pct = 0.70;
    running.pairs[0].hard_limit_pct_override = 0.80;
    ASSERT_NO_THROW(xop::validate_config_relations(running));

    auto saved = running;
    saved.pairs.erase(saved.pairs.begin());   // XCH/A dropped from the file
    saved.risk.soft_limit_pct = 0.85;
    saved.risk.hard_limit_pct = 0.95;
    ASSERT_NO_THROW(xop::validate_config_relations(saved));

    const auto plan = xop::plan_live_config(running, saved);
    EXPECT_THROW(xop::validate_config_relations(plan.merged), xop::ConfigError);
}

TEST(ConfigLiveApply, turning_the_ledger_on_waits_turning_it_off_does_not)
{
    auto running = running_config();
    running.accounting.ledger_enabled        = false;
    running.accounting.bridge_ingest_enabled = false;
    auto saved = running;
    saved.accounting.ledger_enabled        = true;
    saved.accounting.bridge_ingest_enabled = true;
    auto plan = xop::plan_live_config(running, saved);
    EXPECT_TRUE(lists(plan.restart_required, "accounting.ledger_enabled"));
    EXPECT_TRUE(lists(plan.restart_required, "accounting.bridge_ingest_enabled"));
    EXPECT_FALSE(plan.merged.accounting.ledger_enabled)
        << "no opening balances exist in this process";
    EXPECT_FALSE(plan.merged.accounting.bridge_ingest_enabled);

    plan = xop::plan_live_config(saved, running);   // on -> off
    EXPECT_TRUE(plan.restart_required.empty());
    EXPECT_FALSE(plan.merged.accounting.ledger_enabled);
}

TEST(ConfigLiveApply, a_retained_revive_pair_still_refuses_batching)
{
    // The file drops a Revive pair and turns batching on: fine on its own,
    // but the running engine keeps that pair, and batching would merge its
    // capped tiers into one offer above revive_max_offer_size_units.
    auto running = running_config();
    running.strategy.revive_max_offer_size_units = 1.0;
    running.strategy.batch_offers_enabled        = false;
    running.pairs[1].revive_market               = true;
    auto saved = running;
    saved.pairs.erase(saved.pairs.begin() + 1);
    saved.strategy.batch_offers_enabled = true;

    const auto plan = xop::plan_live_config(running, saved);
    ASSERT_TRUE(plan.merged.pairs[1].revive_market);
    try {
        xop::validate_config_relations(plan.merged);
        FAIL() << "the merged config must be refused";
    } catch (const xop::ConfigError& e) {
        EXPECT_NE(std::string(e.what()).find("batch_offers_enabled"),
                  std::string::npos) << e.what();
    }
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

TEST(ConfigLiveApplySetters, depeg_switched_off_releases_automatic_bails_only)
{
    xop::PairConfig a;
    a.name                   = "BYC/wUSDC.b";
    a.is_stablecoin          = true;
    a.peg_target             = 1.0;
    a.depeg_warn_pct         = 2.0;
    a.depeg_bail_pct         = 10.0;
    a.depeg_sustained_blocks = 1;
    xop::PairConfig b = a;
    b.name = "XYZ/wUSDC.b";

    xop::DepegConfig on;
    on.enabled = true;
    xop::DepegDetector d{on};
    d.register_pair(a);
    d.register_pair(b);
    ASSERT_EQ(d.update(a.name, 0.50, 1u), xop::DepegStatus::Bailed);
    d.flag_suspected_failure(b.name);
    ASSERT_TRUE(d.should_bail(a.name));

    auto off = on;
    off.enabled = false;
    d.set_global_config(off);
    EXPECT_FALSE(d.should_bail(a.name))
        << "nothing would ever clear an automatic bail while switched off";
    EXPECT_TRUE(d.should_bail(b.name)) << "an operator's flag is kept";
}

TEST(ConfigLiveApplySetters, volatility_restarts_its_window_on_a_new_candle_size)
{
    xop::VolatilityEstimatorConfig vc;
    vc.candle_aggregation_blocks = 1;
    vc.lookback_blocks           = 100;
    xop::VolatilityEstimator est{vc};
    for (int i = 0; i < 40; ++i) est.update_tick(1.0 + 0.01 * (i % 3));
    ASSERT_GT(est.candle_count(), 0u);

    est.set_tunables(100, vc.yz_alpha, 1);
    EXPECT_GT(est.candle_count(), 0u) << "same candle size: history kept";
    est.set_tunables(20, vc.yz_alpha, 1);
    EXPECT_LE(est.candle_count(), 20u) << "a shorter lookback trims";

    est.set_tunables(20, vc.yz_alpha, 10);
    EXPECT_EQ(est.candle_count(), 0u)
        << "candles of another period would mis-state sigma";
}

TEST(ConfigLiveApplySetters, vpin_starts_over_on_a_new_bucket_size)
{
    xop::MarketDataConfig mc;
    mc.vpin_bucket_size    = 10.0;
    mc.vpin_window_buckets = 50;
    xop::State state;
    xop::MarketDataFeed feed{mc, state};
    const std::string pair = "XCH/wUSDC";
    for (int i = 0; i < 5; ++i) feed.ingest_trade_for_vpin(pair, xop::Side::Bid, 10.0);
    ASSERT_TRUE(feed.get_vpin_metrics(pair).has_value());

    feed.set_config(mc);
    EXPECT_TRUE(feed.get_vpin_metrics(pair).has_value()) << "same size: kept";

    mc.vpin_bucket_size = 100.0;
    feed.set_config(mc);
    EXPECT_FALSE(feed.get_vpin_metrics(pair).has_value())
        << "buckets of the old size would read one-sided flow as ~0.1";
}

TEST(ConfigLiveApplySetters, a_withdrawn_amm_observation_is_gone)
{
    xop::MarketDataConfig mc;
    xop::State state;
    xop::MarketDataFeed feed{mc, state};
    const std::string pair = "XCH/BYC";
    feed.ingest_amm_mid(pair, 1.49, 250'000.0);
    ASSERT_GT(feed.get_fair_value_inputs(pair).amm_mid, 0.0);

    feed.clear_amm_observation(pair);
    const auto obs = feed.get_fair_value_inputs(pair);
    EXPECT_DOUBLE_EQ(obs.amm_mid, 0.0)
        << "a TibetSwap feed switched off must stop feeding the solve";
    EXPECT_DOUBLE_EQ(obs.amm_pool_usd, 0.0);
    feed.clear_amm_observation("XCH/NONE");   // unknown pair: a no-op
}

TEST(ConfigLiveApplySetters, adverse_selection_priors_apply_without_a_new_fill)
{
    xop::AdverseSelectionConfig ac;
    ac.prior_alpha = 1.0;
    ac.prior_beta  = 9.0;
    xop::AdverseSelectionEstimator est{ac};
    const double before = est.get_pin();

    ac.prior_alpha = 5.0;
    ac.prior_beta  = 5.0;
    est.set_config(ac);
    EXPECT_GT(est.get_pin(), before)
        << "a quiet pair must not wait for its next fill to see new priors";
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
