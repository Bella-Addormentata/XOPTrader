// test_config.cpp -- Unit tests for xop::load_config() YAML parser.
//
// Tests verify that the config loader handles:
//   - Valid YAML with all sections
//   - Optional sections defaulting correctly
//   - Invalid / missing required fields
//   - Domain validation (negative values, out-of-range percentages)
//
// Tests that call load_config() use temporary YAML files written to disk.
//
// ISO/IEC 27001:2022 -- no real secrets in test fixtures.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/config.hpp>
#include <xop/execution/book_side_quality.hpp>
#include <xop/execution/cancel_escalation_config.hpp>

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

namespace {

// ============================================================================
// Helper: write a temporary YAML file, return its path.
// ============================================================================

class TempYaml {
public:
    explicit TempYaml(const std::string& content) {
        path_ = "test_config_tmp_" + std::to_string(counter_++) + ".yaml";
        std::ofstream ofs(path_);
        ofs << content;
        ofs.close();
    }
    ~TempYaml() { std::remove(path_.c_str()); }

    const std::string& path() const { return path_; }

private:
    std::string path_;
    static int counter_;
};

int TempYaml::counter_ = 0;

// Minimal valid YAML that satisfies all required sections.
const char* kMinimalValidYaml = R"(
chia:
  full_node_host: "localhost"
  full_node_port: 8555
  wallet_host: "localhost"
  wallet_port: 9256
  ssl_cert_path: "/tmp/cert.pem"
  ssl_key_path: "/tmp/key.pem"
  wallet_cert_path: "/tmp/wcert.pem"
  wallet_key_path: "/tmp/wkey.pem"
  ca_cert_path: "/tmp/ca.crt"
  wallet_fingerprint: 123456

dexie:
  api_base: "https://api.dexie.space/v1"
  max_requests_per_10s: 50

pairs:
  - base_asset_id: "xch"
    quote_asset_id: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    name: "XCH/TEST"
    enabled: true

strategy:
  gamma: 0.01
  kappa: 1.5
  phi: 0.5
  q_max: 1000.0
  min_profit_margin_bps: 35.0
  offer_ttl_blocks: 60
  num_tiers: 2
  tier_spacing_bps: [40, 80]
  tier_size_pct: [0.6, 0.4]

risk:
  soft_limit_pct: 0.60
  hard_limit_pct: 0.80
  single_cat_cap_pct: 0.12
  kelly_fraction: 0.50
  max_capital_per_pair_pct: 0.20

volatility:
  lookback_blocks: 200
  yz_alpha: 0.34

monitoring:
  prometheus_port: 9090
  telegram_bot_token: "test-token"
  telegram_chat_id: "test-chat"

database:
  path: "test.db"
)";

// ============================================================================
// Positive tests: valid YAML parses correctly
// ============================================================================

TEST(ConfigParserTest, MinimalValidYaml_Parses) {
    TempYaml tmp(kMinimalValidYaml);
    EXPECT_NO_THROW({
        auto cfg = xop::load_config(tmp.path());
        EXPECT_EQ(cfg.chia.full_node_port, 8555);
        EXPECT_EQ(cfg.pairs.size(), 1u);
        EXPECT_EQ(cfg.pairs[0].name, "XCH/TEST");
        EXPECT_DOUBLE_EQ(cfg.strategy.gamma, 0.01);
        EXPECT_DOUBLE_EQ(cfg.strategy.kappa, 1.5);
        EXPECT_DOUBLE_EQ(cfg.risk.soft_limit_pct, 0.60);
        EXPECT_EQ(cfg.volatility.lookback_blocks, 200u);
    });
}

TEST(ConfigParserTest, OptionalSections_DefaultCorrectly) {
    TempYaml tmp(kMinimalValidYaml);
    auto cfg = xop::load_config(tmp.path());

    // CoinGecko section is optional; should default to disabled.
    EXPECT_FALSE(cfg.coingecko.enabled);

    // Fee section is optional; should default to disabled.
    EXPECT_FALSE(cfg.fees.enabled);

    // Inventory aging is optional; should default to disabled.
    EXPECT_FALSE(cfg.inventory_aging.enabled);

    // Market allocator is optional; should default to disabled.
    EXPECT_FALSE(cfg.market_allocator.enabled);
    EXPECT_EQ(cfg.market_allocator.eval_interval_blocks, 50u);
    EXPECT_NEAR(cfg.market_allocator.min_alloc_pct, 0.10, 0.001);
    EXPECT_NEAR(cfg.market_allocator.max_alloc_pct, 0.50, 0.001);

    // Depeg detector enabled by default.
    EXPECT_TRUE(cfg.depeg.enabled);

    // Volatility candle aggregation defaults to 10.
    EXPECT_EQ(cfg.volatility.candle_aggregation_blocks, 10u);

    // Strategy confirmation depth defaults to 6.
    EXPECT_EQ(cfg.strategy.confirmation_depth_blocks, 6u);

    // [LEDGER 2026-07-30] The accounting section is optional.  An existing
    // deployment whose config.yaml predates it must still boot -- a throw
    // here would stop the engine from starting at all.
    EXPECT_TRUE(cfg.accounting.ledger_enabled);
    EXPECT_FALSE(cfg.accounting.pause_enabled)
        << "auto-pause must be opt-in, never a default";
    EXPECT_NEAR(cfg.accounting.alert_pct, 0.005, 1e-9);
    EXPECT_NEAR(cfg.accounting.pause_pct, 0.02, 1e-9);
    EXPECT_EQ(cfg.accounting.alert_observations, 2u);
    EXPECT_EQ(cfg.accounting.pause_observations, 3u);
}

// ============================================================================
// accounting: -- ledger / reconciliation control (LEDGER 2026-07-30)
// ============================================================================

TEST(ConfigParserTest, AccountingSection_Parses) {
    std::string yaml = std::string(kMinimalValidYaml) + R"(
accounting:
  ledger_enabled: true
  alert_pct: 0.01
  alert_observations: 3
  pause_pct: 0.05
  pause_observations: 4
  pause_enabled: true
  floor_xch_mojos: 2000000000
  floor_cat_mojos: 250
  fee_slack_mojos: 300000
  max_balance_age_blocks: 20
  bridge_ingest_enabled: false
  bridge_jobs_db_path: elsewhere/warp.db
  bridge_asset_id: aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899
)";
    TempYaml tmp(yaml.c_str());
    auto cfg = xop::load_config(tmp.path());

    EXPECT_TRUE(cfg.accounting.ledger_enabled);
    EXPECT_NEAR(cfg.accounting.alert_pct, 0.01, 1e-9);
    EXPECT_EQ(cfg.accounting.alert_observations, 3u);
    EXPECT_NEAR(cfg.accounting.pause_pct, 0.05, 1e-9);
    EXPECT_EQ(cfg.accounting.pause_observations, 4u);
    EXPECT_TRUE(cfg.accounting.pause_enabled);
    EXPECT_EQ(cfg.accounting.floor_xch_mojos, 2'000'000'000LL);
    EXPECT_EQ(cfg.accounting.floor_cat_mojos, 250LL);
    EXPECT_EQ(cfg.accounting.fee_slack_mojos, 300'000LL);
    EXPECT_EQ(cfg.accounting.max_balance_age_blocks, 20u);
    // [S19] Bridge-ingest keys (review round 5: non-default values so a
    // key rename or conversion regression is caught here, not live).
    EXPECT_FALSE(cfg.accounting.bridge_ingest_enabled);
    EXPECT_EQ(cfg.accounting.bridge_jobs_db_path, "elsewhere/warp.db");
    EXPECT_EQ(cfg.accounting.bridge_asset_id,
              "aabbccddeeff00112233445566778899"
              "aabbccddeeff00112233445566778899");
}

TEST(ConfigParserTest, BridgeAssetWithoutEnabledPair_AutoDisables) {
    // [S19 review round 14] Bridge ingestion needs the asset tracked by
    // an enabled pair (openings + balance snapshots come from pairs).
    // An untracked asset LOUDLY auto-disables the feature rather than
    // throwing: the flag defaults to true, so a throw would brick every
    // config that simply does not trade the bridge asset.
    std::string yaml = std::string(kMinimalValidYaml) + R"(
accounting:
  bridge_ingest_enabled: true
  bridge_asset_id: 00000000000000000000000000000000000000000000000000000000000000ff
)";
    TempYaml tmp(yaml.c_str());
    auto cfg = xop::load_config(tmp.path());
    EXPECT_FALSE(cfg.accounting.bridge_ingest_enabled);
}

TEST(ConfigParserTest, BridgeIngestDisabledSkipsPairCheck) {
    std::string yaml = std::string(kMinimalValidYaml) + R"(
accounting:
  bridge_ingest_enabled: false
  bridge_asset_id: 00000000000000000000000000000000000000000000000000000000000000ff
)";
    TempYaml tmp(yaml.c_str());
    auto cfg = xop::load_config(tmp.path());
    EXPECT_FALSE(cfg.accounting.bridge_ingest_enabled);
}

TEST(ConfigParserTest, AccountingPauseBelowAlert_Throws) {
    // A pause threshold tighter than the alert threshold would pause before
    // ever alerting -- reject it rather than silently mis-escalate.
    std::string yaml = std::string(kMinimalValidYaml) + R"(
accounting:
  alert_pct: 0.05
  pause_pct: 0.01
)";
    TempYaml tmp(yaml.c_str());
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, AccountingZeroObservations_Throws) {
    std::string yaml = std::string(kMinimalValidYaml) + R"(
accounting:
  alert_observations: 0
)";
    TempYaml tmp(yaml.c_str());
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, AccountingOutOfRangePct_Throws) {
    std::string yaml = std::string(kMinimalValidYaml) + R"(
accounting:
  alert_pct: 1.5
)";
    TempYaml tmp(yaml.c_str());
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

// ============================================================================
// Negative tests: missing / invalid inputs
// ============================================================================

TEST(ConfigParserTest, NonexistentFile_Throws) {
    EXPECT_THROW(
        xop::load_config("definitely_does_not_exist_12345.yaml"),
        xop::ConfigError);
}

TEST(ConfigParserTest, EmptyFile_Throws) {
    TempYaml tmp("");
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, InvalidYaml_Throws) {
    TempYaml tmp("{{{{invalid yaml");
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, MissingChiaSection_Throws) {
    // YAML without the required 'chia' section.
    TempYaml tmp(R"(
dexie:
  api_base: "https://api.dexie.space/v1"
pairs: []
strategy:
  gamma: 0.01
  kappa: 1.5
risk:
  soft_limit_pct: 0.60
  hard_limit_pct: 0.80
)");
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

// ============================================================================
// Domain validation
// ============================================================================

TEST(ConfigParserTest, StrategyDefaults_AreReasonable) {
    // Verify the AppConfig struct defaults without loading YAML.
    xop::StrategyConfig s;
    EXPECT_GT(s.gamma, 0.0);
    EXPECT_GT(s.kappa, 0.0);
    EXPECT_GT(s.q_max, 0.0);
    EXPECT_GE(s.min_profit_margin_bps, 0.0);
    EXPECT_EQ(s.confirmation_depth_blocks, 6u);
    EXPECT_EQ(s.reconciliation_interval_blocks, 20u);
    EXPECT_DOUBLE_EQ(s.taker_min_spendable_xch, 0.25);
    EXPECT_DOUBLE_EQ(s.block_time_seconds, 52.0);
}

TEST(ConfigParserTest, StrategyConfig_TakerFloorAndBlockTimeParsed) {
    std::string yaml = kMinimalValidYaml;
    const std::string marker = "  tier_size_pct: [0.6, 0.4]\n";
    const auto marker_pos = yaml.find(marker);
    ASSERT_NE(marker_pos, std::string::npos);
    yaml.insert(
        marker_pos + marker.size(),
        "  taker_min_spendable_xch: 0.30\n"
        "  block_time_seconds: 60.0\n");

    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());

    EXPECT_DOUBLE_EQ(cfg.strategy.taker_min_spendable_xch, 0.30);
    EXPECT_DOUBLE_EQ(cfg.strategy.block_time_seconds, 60.0);
}

TEST(ConfigParserTest, RiskDefaults_SoftLeHard) {
    xop::RiskConfig r;
    EXPECT_LE(r.soft_limit_pct, r.hard_limit_pct);
}

TEST(ConfigParserTest, FeeDefaults_MinLeMax) {
    xop::FeeConfig f;
    EXPECT_LE(f.min_fee_mojos, f.max_fee_mojos);
}

TEST(ConfigParserTest, InventoryAgingDefaults_Reasonable) {
    xop::InventoryAgingConfig ia;
    EXPECT_FALSE(ia.enabled);
    EXPECT_GT(ia.aging_start_blocks, 0u);
    EXPECT_GT(ia.max_loss_relax_bps, 0.0);
    EXPECT_GT(ia.relax_rate_bps_per_block, 0.0);
}

// ============================================================================
// Crossed-book arbitrage config
// ============================================================================

TEST(ConfigParserTest, ArbitrageDefaults_CrossedBook) {
    xop::ArbitrageSettings as;
    EXPECT_TRUE(as.crossed_book_enabled);
    EXPECT_DOUBLE_EQ(as.crossed_book_min_edge_bps, 10.0);
    EXPECT_DOUBLE_EQ(as.crossed_book_max_take_xch, 5.0);
}

TEST(ConfigParserTest, ArbitrageSettings_CrossedBookParsed) {
    std::string yaml = std::string(kMinimalValidYaml) + R"(
arbitrage:
  enabled: true
  crossed_book_enabled: true
  crossed_book_min_edge_bps: 25.0
  crossed_book_max_take_xch: 2.5
)";
    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_TRUE(cfg.arbitrage.crossed_book_enabled);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.crossed_book_min_edge_bps, 25.0);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.crossed_book_max_take_xch, 2.5);
}

TEST(ConfigParserTest, ArbitrageSettings_CrossedBookDisabled) {
    std::string yaml = std::string(kMinimalValidYaml) + R"(
arbitrage:
  enabled: true
  crossed_book_enabled: false
)";
    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_FALSE(cfg.arbitrage.crossed_book_enabled);
}

TEST(ConfigParserTest, ArbitrageSettings_MidpointRecyclingParsed) {
    std::string yaml = std::string(kMinimalValidYaml) + R"(
arbitrage:
  enabled: true
  crossed_book_enabled: true
  cex_reference_half_spread_bps: 7.5
  midpoint_recycling_enabled: true
  midpoint_recycling_pairs: ["XCH/TEST"]
  midpoint_recycling_band_bps: 18
  midpoint_recycling_min_take_xch: 0.10
  midpoint_recycling_max_take_xch: 0.20
  midpoint_recycling_cooldown_blocks: 6
  midpoint_recycling_max_takes_per_block: 2
  midpoint_recycling_daily_take_xch_cap: 1.5
  midpoint_recycling_epoch_blocks: 2304
  midpoint_recycling_min_expected_edge_bps: 4
  midpoint_recycling_fee_buffer_bps: 1.5
  midpoint_recycling_toxicity_buffer_bps: 5
  midpoint_recycling_slippage_buffer_bps: 1.0
  midpoint_recycling_inventory_ratio_cap: 0.55
  midpoint_recycling_require_cex_ref: false
  midpoint_recycling_max_cex_age_blocks: 8
  midpoint_recycling_vpin_max: 0.65
)";
    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());

    EXPECT_TRUE(cfg.arbitrage.midpoint_recycling_enabled);
    ASSERT_EQ(cfg.arbitrage.midpoint_recycling_pairs.size(), 1u);
    EXPECT_EQ(cfg.arbitrage.midpoint_recycling_pairs[0], "XCH/TEST");
    EXPECT_DOUBLE_EQ(cfg.arbitrage.cex_reference_half_spread_bps, 7.5);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_band_bps, 18.0);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_min_take_xch, 0.10);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_max_take_xch, 0.20);
    EXPECT_EQ(cfg.arbitrage.midpoint_recycling_cooldown_blocks, 6u);
    EXPECT_EQ(cfg.arbitrage.midpoint_recycling_max_takes_per_block, 2u);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_daily_take_xch_cap, 1.5);
    EXPECT_EQ(cfg.arbitrage.midpoint_recycling_epoch_blocks, 2304u);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_min_expected_edge_bps, 4.0);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_fee_buffer_bps, 1.5);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_toxicity_buffer_bps, 5.0);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_slippage_buffer_bps, 1.0);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_inventory_ratio_cap, 0.55);
    EXPECT_FALSE(cfg.arbitrage.midpoint_recycling_require_cex_ref);
    EXPECT_EQ(cfg.arbitrage.midpoint_recycling_max_cex_age_blocks, 8u);
    EXPECT_DOUBLE_EQ(cfg.arbitrage.midpoint_recycling_vpin_max, 0.65);
}

TEST(ConfigParserTest, ArbitrageSettings_MidpointRecyclingZeroSlackRejected) {
    std::string yaml = std::string(kMinimalValidYaml) + R"(
arbitrage:
  enabled: true
  midpoint_recycling_enabled: true
  midpoint_recycling_pairs: ["XCH/TEST"]
  midpoint_recycling_band_bps: 0
)";
    TempYaml tmp(yaml);
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, BuyerConfig_ExternalWrappedFileParses) {
    TempYaml buyer_tmp(R"(
buyer:
  enabled: true
  cooldown_blocks: 7
  max_takes_per_block: 2
  pair_rules:
    - pair_name: "XCH/TEST"
      enabled: true
      side: "ask"
      band_bps: 35
      min_edge_bps: 14
      min_take_units: 0.05
      max_take_units: 0.50
      daily_cap_units: 3.0
      max_premium_over_cex_bps: 40
      inventory_ratio_cap: 0.60
)");

    std::string yaml = std::string(kMinimalValidYaml) + "\n" + R"(
buyer:
  enabled: true
  config_path: ")" + buyer_tmp.path() + R"("
)";

    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());

    ASSERT_TRUE(cfg.buyer.enabled);
    EXPECT_EQ(cfg.buyer.cooldown_blocks, 7u);
    EXPECT_EQ(cfg.buyer.max_takes_per_block, 2u);
    ASSERT_EQ(cfg.buyer.pair_rules.size(), 1u);
    EXPECT_EQ(cfg.buyer.pair_rules[0].pair_name, "XCH/TEST");
    EXPECT_EQ(cfg.buyer.pair_rules[0].side, "ask");
}

TEST(ConfigParserTest, BuyerConfig_LegacyPairsFormatStillParses) {
    TempYaml buyer_tmp(R"(
enabled: true
pairs:
  - name: "XCH/TEST"
    enabled: true
    side: "bid"
    band_bps: 25
    min_edge_bps: 10
    min_take_units: 0.10
    max_take_units: 0.40
    daily_cap_units: 2.0
    max_premium_over_cex_bps: 30
    inventory_ratio_cap: 0.55
)");

    std::string yaml = std::string(kMinimalValidYaml) + "\n" + R"(
buyer:
  enabled: true
  config_path: ")" + buyer_tmp.path() + R"("
)";

    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());

    ASSERT_TRUE(cfg.buyer.enabled);
    ASSERT_EQ(cfg.buyer.pair_rules.size(), 1u);
    EXPECT_EQ(cfg.buyer.pair_rules[0].pair_name, "XCH/TEST");
    EXPECT_EQ(cfg.buyer.pair_rules[0].side, "bid");
    EXPECT_DOUBLE_EQ(cfg.buyer.pair_rules[0].inventory_ratio_cap, 0.55);
}

TEST(ConfigParserTest, BuyerConfig_ZeroSlackRejected) {
    TempYaml buyer_tmp(R"(
buyer:
  enabled: true
  pair_rules:
    - pair_name: "XCH/TEST"
      enabled: true
      side: "ask"
      band_bps: 0
      min_edge_bps: 10
      min_take_units: 0.05
      max_take_units: 0.25
      daily_cap_units: 1.0
      max_premium_over_cex_bps: 50
      inventory_ratio_cap: 0.60
    )" );

    std::string yaml = std::string(kMinimalValidYaml) + "\n" + R"(
buyer:
  enabled: true
  config_path: ")" + buyer_tmp.path() + R"("
)";

    TempYaml tmp(yaml);
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, RecoveryConfig_PairAllowlistParses) {
    std::string yaml = std::string(kMinimalValidYaml) +
        "\nrecovery:\n"
        "  enabled: true\n"
        "  xch_low_threshold: 0.10\n"
        "  xch_recovery_target: 0.75\n"
        "  max_take_per_block_xch: 0.25\n"
        "  max_premium_bps: 80\n"
        "  cancel_on_enter: false\n"
        "  zero_fee_below_xch: 0.002\n"
        "  pair_allowlist:\n"
        "    - \"XCH/wUSDC.b\"\n";

    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());

    ASSERT_EQ(cfg.recovery.pair_allowlist.size(), 1u);
    EXPECT_EQ(cfg.recovery.pair_allowlist[0], "XCH/wUSDC.b");
    EXPECT_FALSE(cfg.recovery.cancel_on_enter);
    EXPECT_DOUBLE_EQ(cfg.recovery.zero_fee_below_xch, 0.002);
}

// ============================================================================
// Micro-price blend schedule
//
// Both knobs are absent from the shipped config.yaml on purpose: the defaults
// have to protect an unconfigured deployment, because an unconfigured
// deployment is exactly what the BYC mispricing reached.
// ============================================================================

namespace {

/// kMinimalValidYaml with extra keys spliced into the [strategy] block.
std::string with_strategy_keys(const std::string& extra) {
    std::string y = kMinimalValidYaml;
    const std::string anchor = "  tier_size_pct: [0.6, 0.4]";
    const auto pos = y.find(anchor);
    if (pos == std::string::npos) return y;
    y.insert(pos + anchor.size(), extra);
    return y;
}

/// kMinimalValidYaml with one existing [strategy] line swapped out.
/// with_strategy_keys() appends, which would leave a DUPLICATE key for the
/// required scalars (gamma, q_max, ...) and make the test depend on yaml-cpp's
/// duplicate-key resolution. Replacing is deterministic.
std::string with_strategy_replaced(const std::string& from,
                                   const std::string& to) {
    std::string y(kMinimalValidYaml);
    const auto pos = y.find(from);
    if (pos == std::string::npos) return y;
    y.replace(pos, from.size(), to);
    return y;
}

std::string with_pair_extra(const std::string& line) {
    // Insert a key into the single pair in kMinimalValidYaml. The pair block
    // is indented four spaces, so the inserted line must match or YAML
    // silently reads it as a sibling of `pairs:` rather than a pair field --
    // which would make an override test pass while overriding nothing.
    std::string s(kMinimalValidYaml);
    const std::string anchor = "    enabled: true";
    const auto pos = s.find(anchor);
    s.insert(pos + anchor.size(), "\n    " + line);
    return s;
}

}  // namespace

TEST(ConfigParserTest, MicropriceBandDefaultsWithoutAnyConfig) {
    TempYaml tmp(kMinimalValidYaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.strategy.microprice_narrow_bps, 200.0);
    EXPECT_DOUBLE_EQ(cfg.strategy.microprice_wide_bps,   800.0);
}

TEST(ConfigParserTest, MicropriceBandIsOverridable) {
    TempYaml tmp(with_strategy_keys(
        "\n  microprice_narrow_bps: 150\n  microprice_wide_bps: 1200"));
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.strategy.microprice_narrow_bps,  150.0);
    EXPECT_DOUBLE_EQ(cfg.strategy.microprice_wide_bps,   1200.0);
}

TEST(ConfigParserTest, MicropriceBandRejectsAnInvertedBand) {
    // wide <= narrow leaves no interior to interpolate across, so the blend
    // would silently collapse into the discontinuous step this schedule
    // exists to replace.  Refuse it rather than quietly degrade.
    TempYaml tmp(with_strategy_keys(
        "\n  microprice_narrow_bps: 800\n  microprice_wide_bps: 200"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, MicropriceBandRejectsEqualEdges) {
    TempYaml tmp(with_strategy_keys(
        "\n  microprice_narrow_bps: 400\n  microprice_wide_bps: 400"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, MicropriceBandRejectsNegativeEdges) {
    TempYaml tmp(with_strategy_keys("\n  microprice_narrow_bps: -1"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

// ============================================================================
// Published-mid BBO band
//
// Like the micro-price schedule, both knobs are deliberately absent from the
// shipped config.yaml: the defaults must protect an unconfigured deployment.
// ============================================================================

TEST(ConfigParserTest, PublishedMidBandDefaultsWithoutAnyConfig) {
    TempYaml tmp(kMinimalValidYaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.strategy.published_mid_band_floor_bps,   150.0);
    EXPECT_DOUBLE_EQ(cfg.strategy.published_mid_band_spread_frac, 0.25);
}

TEST(ConfigParserTest, PublishedMidBandIsOverridable) {
    TempYaml tmp(with_strategy_keys(
        "\n  published_mid_band_floor_bps: 200"
        "\n  published_mid_band_spread_frac: 0.5"));
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.strategy.published_mid_band_floor_bps,   200.0);
    EXPECT_DOUBLE_EQ(cfg.strategy.published_mid_band_spread_frac, 0.5);
}

TEST(ConfigParserTest, PublishedMidBandRejectsNegativeValues) {
    TempYaml tmp(with_strategy_keys(
        "\n  published_mid_band_floor_bps: -10"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);

    TempYaml tmp2(with_strategy_keys(
        "\n  published_mid_band_spread_frac: -0.1"));
    EXPECT_THROW(xop::load_config(tmp2.path()), xop::ConfigError);
}


// ============================================================================
// revive_market -- the empty-book quoting opt-in.
//
// A pair whose third-party book is expected to be dead (wmilliETH.b/XCH was
// the motivating case: every live offer sat 20%+ from fair, so the outlier
// filter emptied the book and Step 7 cleared the ladder every heartbeat).
// The flag lets the ladder survive an empty FILTERED book, but only while a
// live external estimate anchors the centre -- the predicate below is the
// exact decision Step 7 executes, factored out so this file can pin it.
// ============================================================================

TEST(ConfigParserTest, ReviveMarket_DefaultsFalse) {
    TempYaml tmp(kMinimalValidYaml);
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    EXPECT_FALSE(cfg.pairs[0].revive_market);
}

TEST(ConfigParserTest, ReviveMarket_ParsesTrue) {
    std::string yaml(kMinimalValidYaml);
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";
    auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(), "    revive_market: true\n");
    // revive_market now demands a live CoinGecko feed at load time whose
    // ids cover BOTH legs of a mappable pair name.
    auto npos_ = yaml.find("name: \"XCH/TEST\"");
    ASSERT_NE(npos_, std::string::npos);
    yaml.replace(npos_, std::string("name: \"XCH/TEST\"").size(),
                 "name: \"XCH/wUSDC.b\"");
    yaml += "\ncoingecko:\n  enabled: true\n"
            "  coin_ids: [\"chia\", \"usd-coin\"]\n";
    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    EXPECT_TRUE(cfg.pairs[0].revive_market);
}

TEST(LadderSurvivesEmptyBook, RequiresFlagAndAnchorAndFreshFeed) {
    xop::PairConfig p;

    // Without the opt-in, no combination of anchor/freshness quotes: an
    // operator who did not ask for revival keeps the old behaviour.
    p.revive_market = false;
    EXPECT_FALSE(xop::ladder_survives_empty_book(&p, false, false));
    EXPECT_FALSE(xop::ladder_survives_empty_book(&p, false, true));
    EXPECT_FALSE(xop::ladder_survives_empty_book(&p, true,  false));
    EXPECT_FALSE(xop::ladder_survives_empty_book(&p, true,  true));

    p.revive_market = true;

    // Opt-in without an anchor: quoting blind -- the exact thing the
    // clear exists to stop.  The flag must NOT override it.
    EXPECT_FALSE(xop::ladder_survives_empty_book(&p, false, false));
    EXPECT_FALSE(xop::ladder_survives_empty_book(&p, false, true));

    // Opt-in with an anchor whose FEED is stale: the frozen-anchor trap.
    // The solve keeps a self-refreshing timestamp, so the estimate looks
    // alive long after the feed died; a revived ladder would stand at
    // yesterday's price while the market walks away.  Must not quote.
    EXPECT_FALSE(xop::ladder_survives_empty_book(&p, true, false));

    // Opt-in + live anchor + fresh feed: the one combination that quotes.
    EXPECT_TRUE(xop::ladder_survives_empty_book(&p, true, true));
}

TEST(LadderSurvivesEmptyBook, NullPairConfigNeverSurvives) {
    // A pair name that resolves to no PairConfig (defensive: find_pair_config
    // returned nullptr) must behave like the flag is off.
    EXPECT_FALSE(xop::ladder_survives_empty_book(nullptr, true,  true));
    EXPECT_FALSE(xop::ladder_survives_empty_book(nullptr, true,  false));
    EXPECT_FALSE(xop::ladder_survives_empty_book(nullptr, false, true));
    EXPECT_FALSE(xop::ladder_survives_empty_book(nullptr, false, false));
}


TEST(CoingeckoFeedFreshForRevival, PinsTheAgeArithmetic) {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const double threshold = 120.0;

    // Fresh: fetched 30s ago.
    EXPECT_TRUE(xop::coingecko_feed_fresh_for_revival(
        true, now - std::chrono::seconds(30), now, threshold));

    // Boundary: exactly at the threshold still counts as fresh (<=).
    EXPECT_TRUE(xop::coingecko_feed_fresh_for_revival(
        true, now - std::chrono::seconds(120), now, threshold));

    // Stale: one poll interval past the threshold.
    EXPECT_FALSE(xop::coingecko_feed_fresh_for_revival(
        true, now - std::chrono::seconds(150), now, threshold));

    // Never fetched successfully: a default-constructed time_point gives
    // an enormous age -- must read stale, not fresh.
    EXPECT_FALSE(xop::coingecko_feed_fresh_for_revival(
        true, clock::time_point{}, now, threshold));

    // No prices cached at all: stale regardless of timestamps.
    EXPECT_FALSE(xop::coingecko_feed_fresh_for_revival(
        false, now, now, threshold));
}

TEST(CoingeckoFeedFreshForRevival, DisabledThresholdReadsStaleNotFresh) {
    // cex_freshness_threshold_sec <= 0 legally disables freshness decay
    // for the published-mid blend.  For revival "no freshness check" would
    // mean a frozen feed quotes forever, so the helper must be
    // conservative -- and load_config refuses the combination anyway
    // (tested below).
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    EXPECT_FALSE(xop::coingecko_feed_fresh_for_revival(true, now, now, 0.0));
    EXPECT_FALSE(xop::coingecko_feed_fresh_for_revival(true, now, now, -1.0));
}

TEST(ConfigParserTest, ReviveMarketWithDisabledFreshnessThreshold_Throws) {
    std::string yaml(kMinimalValidYaml);
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";
    auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(), "    revive_market: true\n");
    yaml += "\nmarket_data:\n  cex_freshness_threshold_sec: 0\n";
    TempYaml tmp(yaml);
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, FeeReserveXch_RejectsNonFiniteAndOverflow) {
    // (review) .nan passed the bare < 0 check into llround -- a domain
    // error that on common implementations zeroes the reserve floor,
    // silently disabling the protection the value configures.
    const auto with_reserve = [](const std::string& value) {
        std::string yaml(kMinimalValidYaml);
        const std::string anchor = "strategy:\n";
        const auto at = yaml.find(anchor);
        EXPECT_NE(at, std::string::npos);
        yaml.insert(at + anchor.size(),
                    "  fee_reserve_xch: " + value + "\n");
        return yaml;
    };

    {
        TempYaml tmp(with_reserve("0.5"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.strategy.fee_reserve_xch, 0.5);
    }
    {
        TempYaml tmp(with_reserve(".nan"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        TempYaml tmp(with_reserve(".inf"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        TempYaml tmp(with_reserve("10000000"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
}

TEST(ConfigParserTest, XchCycleCommitFrac_ParsesDefaultsAndRejects) {
    // Injected into the existing strategy: section (duplicate root keys
    // resolve to whichever yaml-cpp meets first -- same hazard the
    // FlashCrashWindow test documents).
    const auto with_frac = [](const std::string& value) {
        std::string yaml(kMinimalValidYaml);
        const std::string anchor = "strategy:\n";
        const auto at = yaml.find(anchor);
        EXPECT_NE(at, std::string::npos);
        yaml.insert(at + anchor.size(),
                    "  xch_cycle_commit_frac: " + value + "\n");
        return yaml;
    };

    {
        TempYaml tmp(with_frac("0.25"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.strategy.xch_cycle_commit_frac, 0.25);
    }
    {
        // Omitted: the documented default.
        TempYaml tmp(kMinimalValidYaml);
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.strategy.xch_cycle_commit_frac, 0.5);
    }
    {
        // The documented bounds are inclusive; an operator sets exactly
        // these during an incident (0.0 = no SPEND-SIDE posting -- buy-XCH
        // offers stay cap-exempt -- and 1.0 = floor-only).
        TempYaml tmp(with_frac("0.0"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.strategy.xch_cycle_commit_frac, 0.0);
    }
    {
        TempYaml tmp(with_frac("1.0"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.strategy.xch_cycle_commit_frac, 1.0);
    }
    {
        TempYaml tmp(with_frac("1.5"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        TempYaml tmp(with_frac(".nan"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        TempYaml tmp(with_frac("-0.1"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
}

TEST(ConfigParserTest, FlashCrashWindow_ParsesDefaultsAndRejects) {
    // kMinimalValidYaml already carries a risk: section, so the knob is
    // INJECTED into it rather than appended as a duplicate root key --
    // yaml-cpp resolves duplicate keys to whichever it meets first, which
    // would silently test the wrong value.
    const auto with_window = [](const std::string& value) {
        std::string yaml(kMinimalValidYaml);
        const std::string anchor = "risk:\n";
        const auto at = yaml.find(anchor);
        EXPECT_NE(at, std::string::npos);
        yaml.insert(at + anchor.size(),
                    "  flash_crash_window_blocks: " + value + "\n");
        return yaml;
    };

    {
        TempYaml tmp(with_window("240"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_EQ(cfg.risk.flash_crash_window_blocks, 240u);
    }
    {
        // Omitted: the documented default.
        TempYaml tmp(kMinimalValidYaml);
        auto cfg = xop::load_config(tmp.path());
        EXPECT_EQ(cfg.risk.flash_crash_window_blocks, 100u);
    }
    {
        // 0 is the deliberate whole-history opt-out, not an error.
        TempYaml tmp(with_window("0"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_EQ(cfg.risk.flash_crash_window_blocks, 0u);
    }
    {
        TempYaml tmp(with_window("-5"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        // window=1 selects one sample, runs zero comparisons, and silently
        // disables the detector -- rejected rather than accepted as a trap.
        TempYaml tmp(with_window("1"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
}

TEST(ConfigParserTest, DexLastTradeMaxAge_ParsesAndDefaults) {
    // The propagation path itself, not just the gate: this knob was
    // advertised as configurable while nothing read it, so production sat
    // pinned to the default no matter what the YAML said.
    {
        std::string yaml(kMinimalValidYaml);
        yaml += R"(
market_data:
  dex_last_trade_max_age_sec: 42.5
)";
        TempYaml tmp(yaml);
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.market_data.dex_last_trade_max_age_sec, 42.5);
    }
    {
        // Omitted: the documented default, and <= 0 stays legal as a
        // deliberate disable (same convention as the other tapers).
        TempYaml tmp(kMinimalValidYaml);
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.market_data.dex_last_trade_max_age_sec, 1800.0);
    }
    {
        std::string yaml(kMinimalValidYaml);
        yaml += R"(
market_data:
  dex_last_trade_max_age_sec: 0
)";
        TempYaml tmp(yaml);
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.market_data.dex_last_trade_max_age_sec, 0.0);
    }
}

TEST(ConfigParserTest, DisabledFreshnessThresholdWithoutRevive_IsLegal) {
    // The 0-disables-decay setting predates revive_market and must keep
    // working for configs that never opted into revival.
    std::string yaml(kMinimalValidYaml);
    yaml += "\nmarket_data:\n  cex_freshness_threshold_sec: 0\n";
    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.market_data.cex_freshness_threshold_sec, 0.0);
}


TEST(ConfigParserTest, ReviveMarketWithDisabledAmmExpiry_Throws) {
    // fair_value_amm_max_age_sec: 0 legally admits AMM edges of any age
    // into the fair-value graph.  A revived pair quoting from that graph
    // could then stand on a frozen TibetSwap price while CoinGecko stays
    // fresh -- the one feed the runtime gate cannot see.  Refused at load.
    std::string yaml(kMinimalValidYaml);
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";
    auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(), "    revive_market: true\n");
    const std::string skey = "\n  min_profit_margin_bps: 35.0";
    auto spos = yaml.find(skey);
    ASSERT_NE(spos, std::string::npos);
    yaml.insert(spos + skey.size(), "\n  fair_value_amm_max_age_sec: 0");
    TempYaml tmp(yaml);
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, DisabledAmmExpiryWithoutRevive_IsLegal) {
    std::string yaml(kMinimalValidYaml);
    const std::string skey = "\n  min_profit_margin_bps: 35.0";
    auto spos = yaml.find(skey);
    ASSERT_NE(spos, std::string::npos);
    yaml.insert(spos + skey.size(), "\n  fair_value_amm_max_age_sec: 0");
    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.strategy.fair_value_amm_max_age_sec, 0.0);
}


TEST(ConfigParserTest, ReviveMarketWithInfiniteThresholds_Throws) {
    // YAML .inf parses to +infinity, and inf <= 0.0 is false -- so an
    // infinite "expiry" slid through the non-positive check while
    // disabling the freshness arithmetic entirely (age <= inf is always
    // true).  Both knobs must be refused when revival is on.
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";

    {
        std::string yaml(kMinimalValidYaml);
        auto pos = yaml.find(anchor);
        ASSERT_NE(pos, std::string::npos);
        yaml.insert(pos + anchor.size(), "    revive_market: true\n");
        yaml += "\nmarket_data:\n  cex_freshness_threshold_sec: .inf\n";
        TempYaml tmp(yaml);
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        std::string yaml(kMinimalValidYaml);
        auto pos = yaml.find(anchor);
        ASSERT_NE(pos, std::string::npos);
        yaml.insert(pos + anchor.size(), "    revive_market: true\n");
        const std::string skey = "\n  min_profit_margin_bps: 35.0";
        auto spos = yaml.find(skey);
        ASSERT_NE(spos, std::string::npos);
        yaml.insert(spos + skey.size(),
                    "\n  fair_value_amm_max_age_sec: .inf");
        TempYaml tmp(yaml);
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
}

TEST(CoingeckoFeedFreshForRevival, NonFiniteThresholdReadsStale) {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(xop::coingecko_feed_fresh_for_revival(true, now, now, inf));
    EXPECT_FALSE(xop::coingecko_feed_fresh_for_revival(true, now, now, nan));
}


TEST(ConfigParserTest, ReviveMarketWithDisabledWidthSigma_Throws) {
    // quote_width_sigma_mult: 0 removes the sigma term from the width
    // floor -- the one bound the untrusted-solve branch relies on in
    // place of the fair-value clamp.  A revived ladder would quote an
    // uncertain estimate tighter than its own error bar.
    std::string yaml(kMinimalValidYaml);
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";
    auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(), "    revive_market: true\n");
    const std::string skey = "\n  min_profit_margin_bps: 35.0";
    auto spos = yaml.find(skey);
    ASSERT_NE(spos, std::string::npos);
    yaml.insert(spos + skey.size(), "\n  quote_width_sigma_mult: 0");
    TempYaml tmp(yaml);
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, ReviveMarketWithDisabledStaleDemotion_Throws) {
    // fair_value_stale_sigma_bps_per_print: 0 disables the term that
    // demotes a frozen book edge -- a stale transitive edge would keep
    // fixed weight in the solve forever.
    std::string yaml(kMinimalValidYaml);
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";
    auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(), "    revive_market: true\n");
    const std::string skey = "\n  min_profit_margin_bps: 35.0";
    auto spos = yaml.find(skey);
    ASSERT_NE(spos, std::string::npos);
    yaml.insert(spos + skey.size(),
                "\n  fair_value_stale_sigma_bps_per_print: 0");
    TempYaml tmp(yaml);
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, DisabledWidthSigmaWithoutRevive_IsLegal) {
    // Both knobs keep their legal 0 settings for configs that never
    // opted into revival.
    std::string yaml(kMinimalValidYaml);
    const std::string skey = "\n  min_profit_margin_bps: 35.0";
    auto spos = yaml.find(skey);
    ASSERT_NE(spos, std::string::npos);
    yaml.insert(spos + skey.size(),
                "\n  quote_width_sigma_mult: 0"
                "\n  fair_value_stale_sigma_bps_per_print: 0");
    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.strategy.quote_width_sigma_mult, 0.0);
    EXPECT_DOUBLE_EQ(cfg.strategy.fair_value_stale_sigma_bps_per_print, 0.0);
}


TEST(ApplyDeployIdleFloor, HardZeroStaysStopped_TaperStillFloors) {
    // The wallet-bleed regression: a drift-guard hard zero (scale 0.0)
    // means STOPPED, and the floor must not re-inflate it -- on either
    // side (the helper is side-agnostic; bid and ask both route through
    // it).  A merely tapered side is still acquiring and may be floored.
    const std::int64_t pool = 0, min_pool = 1000;

    // Hard-stopped: pool stays zero even with the floor armed and the
    // wallet able to back it.
    EXPECT_EQ(xop::apply_deploy_idle_floor(pool, min_pool, true, 0.0, true),
              0);

    // Tapered but nonzero: the guard is slowing acquisition, not stopping
    // it -- the floor may still raise the pool to one minimum offer.
    EXPECT_EQ(xop::apply_deploy_idle_floor(pool, min_pool, true, 0.35, true),
              min_pool);
    EXPECT_EQ(xop::apply_deploy_idle_floor(pool, min_pool, true, 1.0, true),
              min_pool);
}

TEST(ApplyDeployIdleFloor, RespectsArmingWalletAndExistingPool) {
    const std::int64_t min_pool = 1000;

    // Floor disarmed by ratio-rebalance mode: nothing happens.
    EXPECT_EQ(xop::apply_deploy_idle_floor(0, min_pool, false, 1.0, true), 0);

    // Wallet cannot back one minimum offer: nothing happens.
    EXPECT_EQ(xop::apply_deploy_idle_floor(0, min_pool, true, 1.0, false), 0);

    // Pool already at/above the minimum: left alone (never scaled DOWN).
    EXPECT_EQ(xop::apply_deploy_idle_floor(5000, min_pool, true, 1.0, true),
              5000);
    EXPECT_EQ(xop::apply_deploy_idle_floor(min_pool, min_pool, true, 0.0,
                                           true),
              min_pool);

    // Degenerate min_pool: no-op.
    EXPECT_EQ(xop::apply_deploy_idle_floor(0, 0, true, 1.0, true), 0);
}

TEST(EffectiveQMax, PairOverrideWinsElseStrategyDefault) {
    // [STEP6-CAUSE 2026-09-13] The one copy of the q_max resolution: the
    // Engine constructor builds each AvellanedaStoikov with it, and Step 4
    // stores it for Step 6's no-quote line.
    xop::PairConfig pair_cfg{};
    xop::StrategyConfig strategy_cfg{};
    strategy_cfg.q_max = 20.0;
    EXPECT_DOUBLE_EQ(xop::effective_q_max(pair_cfg, strategy_cfg), 20.0);

    pair_cfg.q_max_override = 6.0;
    EXPECT_DOUBLE_EQ(xop::effective_q_max(pair_cfg, strategy_cfg), 6.0);
}


TEST(ConfigParserTest, ReviveMarketWindowNarrowerThanPolling_Throws) {
    // A 300s poll with the default 120s freshness window means no fetch
    // is even scheduled between 120s and 300s: a HEALTHY feed reads as
    // stale for ~180s of every cycle and the revived ladder oscillates.
    std::string yaml(kMinimalValidYaml);
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";
    auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(), "    revive_market: true\n");
    auto npos_ = yaml.find("name: \"XCH/TEST\"");
    ASSERT_NE(npos_, std::string::npos);
    yaml.replace(npos_, std::string("name: \"XCH/TEST\"").size(),
                 "name: \"XCH/wUSDC.b\"");
    yaml += "\ncoingecko:\n  enabled: true\n"
            "  coin_ids: [\"chia\", \"usd-coin\"]\n"
            "  polling_interval_ms: 300000\n";
    TempYaml tmp(yaml);
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);

    // The same cadence is legal once the window covers it.
    std::string ok(yaml);
    ok += "\nmarket_data:\n  cex_freshness_threshold_sec: 600\n";
    TempYaml tmp2(ok);
    EXPECT_NO_THROW(xop::load_config(tmp2.path()));
}

TEST(ConfigParserTest, ReviveMarketWithCoingeckoDisabled_Throws) {
    // The revive freshness gate is anchored to the CoinGecko feed; with
    // the feed off, a revived pair would sit silent forever.  Loud, not
    // silent.
    std::string yaml(kMinimalValidYaml);
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";
    auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(), "    revive_market: true\n");
    yaml += "\ncoingecko:\n  enabled: false\n";
    TempYaml tmp(yaml);
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, SlowPollingWithoutRevive_IsLegal) {
    std::string yaml(kMinimalValidYaml);
    yaml += "\ncoingecko:\n  enabled: true\n  coin_ids: [\"chia\"]\n"
            "  polling_interval_ms: 300000\n";
    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_EQ(cfg.coingecko.polling_interval_ms, 300000u);
}


TEST(ConfigParserTest, ReviveMarketNeverWorksCombos_Throw) {
    // Three legal settings each make revival structurally impossible --
    // the engine would start cleanly and clear the ladder forever,
    // exactly the silent failure the cross-check exists to prevent:
    // the blend switch off (quote_has_external_est permanently false),
    // an empty coin id list (every fetch returns an empty map), and a
    // zero feed sigma (the solver discards anchors with sigma <= 0).
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";

    auto with_revive = [&](const std::string& extra) {
        std::string yaml(kMinimalValidYaml);
        auto pos = yaml.find(anchor);
        EXPECT_NE(pos, std::string::npos);
        yaml.insert(pos + anchor.size(), "    revive_market: true\n");
        auto npos_ = yaml.find("name: \"XCH/TEST\"");
        EXPECT_NE(npos_, std::string::npos);
        yaml.replace(npos_, std::string("name: \"XCH/TEST\"").size(),
                     "name: \"XCH/wUSDC.b\"");
        yaml += "\ncoingecko:\n  enabled: true\n"
                "  coin_ids: [\"chia\", \"usd-coin\"]\n";
        yaml += extra;
        return yaml;
    };

    {
        std::string yaml = with_revive("");
        const std::string skey = "\n  min_profit_margin_bps: 35.0";
        auto spos = yaml.find(skey);
        ASSERT_NE(spos, std::string::npos);
        yaml.insert(spos + skey.size(),
                    "\n  quote_center_blend_enabled: false");
        TempYaml tmp(yaml);
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        // Empty coin id list: build without the helper's non-empty list.
        std::string yaml(kMinimalValidYaml);
        auto pos = yaml.find(anchor);
        ASSERT_NE(pos, std::string::npos);
        yaml.insert(pos + anchor.size(), "    revive_market: true\n");
        yaml += "\ncoingecko:\n  enabled: true\n  coin_ids: []\n";
        TempYaml tmp(yaml);
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        std::string yaml = with_revive("");
        const std::string skey = "\n  min_profit_margin_bps: 35.0";
        auto spos = yaml.find(skey);
        ASSERT_NE(spos, std::string::npos);
        yaml.insert(spos + skey.size(),
                    "\n  fair_value_feed_sigma_bps: 0");
        TempYaml tmp(yaml);
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
}

TEST(ConfigParserTest, NeverWorksCombosWithoutRevive_AreLegal) {
    std::string yaml(kMinimalValidYaml);
    const std::string skey = "\n  min_profit_margin_bps: 35.0";
    auto spos = yaml.find(skey);
    ASSERT_NE(spos, std::string::npos);
    yaml.insert(spos + skey.size(),
                "\n  quote_center_blend_enabled: false"
                "\n  fair_value_feed_sigma_bps: 0");
    yaml += "\ncoingecko:\n  enabled: true\n  coin_ids: []\n";
    TempYaml tmp(yaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_FALSE(cfg.strategy.quote_center_blend_enabled);
    EXPECT_DOUBLE_EQ(cfg.strategy.fair_value_feed_sigma_bps, 0.0);
    EXPECT_TRUE(cfg.coingecko.coin_ids.empty());
}


TEST(ConfigParserTest, ReviveMarketCoinIdsMustCoverTheLegs) {
    // A non-empty-but-unrelated id list previously loaded: with
    // coin_ids: [bitcoin] no anchor is ever created for the pair's legs
    // and the revived pair sits silent forever.  The legs resolve
    // through the same table the engine uses (xop/feed_listings.hpp).
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";

    auto revive_pair = [&](const char* ids) {
        std::string yaml(kMinimalValidYaml);
        auto pos = yaml.find(anchor);
        EXPECT_NE(pos, std::string::npos);
        yaml.insert(pos + anchor.size(), "    revive_market: true\n");
        auto npos_ = yaml.find("name: \"XCH/TEST\"");
        EXPECT_NE(npos_, std::string::npos);
        yaml.replace(npos_, std::string("name: \"XCH/TEST\"").size(),
                     "name: \"XCH/wUSDC.b\"");
        yaml += std::string("\ncoingecko:\n  enabled: true\n  coin_ids: ")
              + ids + "\n";
        return yaml;
    };

    // Unrelated ids: fetches succeed, anchors never exist.
    {
        TempYaml tmp(revive_pair("[\"bitcoin\"]"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    // One leg covered, the other missing: still refused.
    {
        TempYaml tmp(revive_pair("[\"chia\"]"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    // Both legs covered: loads.
    {
        TempYaml tmp(revive_pair("[\"chia\", \"usd-coin\"]"));
        EXPECT_NO_THROW(xop::load_config(tmp.path()));
    }
}

TEST(ConfigParserTest, ReviveMarketUnmappableLeg_Throws) {
    // "TEST" has no CoinGecko feed mapping at all: no id list can anchor
    // it, so revival is refused with a message naming the leg.
    std::string yaml(kMinimalValidYaml);
    const std::string anchor =
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n";
    auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(), "    revive_market: true\n");
    yaml += "\ncoingecko:\n  enabled: true\n"
            "  coin_ids: [\"chia\", \"usd-coin\", \"ethereum\"]\n";
    TempYaml tmp(yaml);
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

}  // namespace

// ============================================================================
// [S20 2026-08-24] Gate-knob validation through the REAL YAML parser.
//
// The gate tests construct MarketDataConfig / RiskConfig structs directly,
// so nothing exercised these validators.  That matters more than usual
// here: yaml-cpp accepts `.nan` and `.inf`, and a non-finite value slips
// through ordinary range comparisons -- `NaN > 1.0` is false, so a
// `.nan` band would silently skip the anchor test while the config still
// read as though the gate were armed.
// ============================================================================

namespace {

std::string with_market_data(const std::string& body) {
    return std::string(kMinimalValidYaml) + "\nmarket_data:\n" + body;
}

std::string with_risk_extra(const std::string& line) {
    // Append into the existing risk: block by re-declaring the key under a
    // fresh document is not possible, so build the section inline.
    std::string s(kMinimalValidYaml);
    const std::string anchor = "risk:\n";
    const auto pos = s.find(anchor);
    s.insert(pos + anchor.size(), "  " + line + "\n");
    return s;
}

}  // namespace

TEST(ConfigParserTest, S20GateKnobs_NonFiniteRejected) {
    for (const char* bad : {".nan", ".inf", "-.inf"}) {
        {
            TempYaml tmp(with_market_data(
                std::string("  mid_anchor_band_ratio: ") + bad));
            EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
                << "mid_anchor_band_ratio accepted " << bad;
        }
        {
            TempYaml tmp(with_market_data(
                std::string("  mid_gate_book_confirm_max_spread_bps: ") + bad));
            EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
                << "mid_gate_book_confirm_max_spread_bps accepted " << bad;
        }
        {
            TempYaml tmp(with_market_data(
                std::string("  mid_gate_max_step_frac: ") + bad));
            EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
                << "mid_gate_max_step_frac accepted " << bad;
        }
        {
            TempYaml tmp(with_market_data(
                std::string("  implied_cross_max_leg_spread_bps: ") + bad));
            EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
                << "implied_cross_max_leg_spread_bps accepted " << bad;
        }
    }
}

TEST(ConfigParserTest, S20GateKnobs_LegalDisabledValuesAccepted) {
    // 0 / <=1 are documented as "disabled" for the band and step gates and
    // must keep parsing -- a validator that rejected them would make the
    // documented escape hatch unusable.
    TempYaml tmp(with_market_data(
        "  mid_gate_enabled: false\n"
        "  mid_anchor_band_ratio: 0.0\n"
        "  mid_gate_max_step_frac: 0.0\n"
        "  mid_gate_book_confirm_max_spread_bps: 0.0\n"
        "  implied_cross_max_leg_spread_bps: 1500.0\n"));
    auto cfg = xop::load_config(tmp.path());
    EXPECT_FALSE(cfg.market_data.mid_gate_enabled);
    EXPECT_DOUBLE_EQ(cfg.market_data.mid_anchor_band_ratio, 0.0);
    EXPECT_DOUBLE_EQ(cfg.market_data.mid_gate_max_step_frac, 0.0);
}

TEST(ConfigParserTest, S20GateKnobs_NegativeAndZeroLegCapRejected) {
    {
        TempYaml tmp(with_market_data("  mid_anchor_band_ratio: -1.0\n"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        // The leg cap is a strict positive: at 0 no triangle can ever form.
        TempYaml tmp(with_market_data("  implied_cross_max_leg_spread_bps: 0.0\n"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
}

// ============================================================================
// [SIDEQUALITY 2026-09-01] Per-side anchor-agreement knob validation through
// the REAL YAML parser.
//
// Same hazard as the S20 knobs above: the side-quality tests build
// MarketDataConfig directly, so nothing exercised these two validators, and
// yaml-cpp happily hands back `.nan` / `.inf`.  A `.nan` side band would make
// every band comparison false -- the per-side test silently skipped while the
// config read as though it were armed -- and an `.inf` agreement cap would
// trust ANY two-sided book whole, which is exactly the absent-ask-side book
// this feature exists to distrust.
//
// The disabled-value case below is the one that matters most: <= 1.0 is
// DOCUMENTED as "disables the per-side test", not as an error.  A validator
// that rejected it would take the escape hatch away from the operator.
// ============================================================================

// [BBOPERPAIR 2026-09-01] Per-pair BBO proximity caps.
//
// FRACTIONS (0.10 == 10%), bounded (0, 1]. The bound is the point: the
// strategy-level fields these override are fractions too, and an operator
// reaching for a "percentage from the market" knob is one keystroke from
// entering 10 (meaning 10%) or 1000 (meaning bps). Either sails through an
// unbounded parse and yields a cap that can never bind -- a suppression
// control silently switched off. Rejecting at load is the whole value.

TEST(ConfigParserTest, BboSanityPerPairOverrides_ParseAndReachTheConfig) {
    TempYaml tmp(with_pair_extra(
        "bbo_sanity_max_aggressive_dev_override: 0.05\n"
        "    bbo_sanity_max_passive_dev_override: 0.45"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    ASSERT_TRUE(cfg.pairs[0].bbo_sanity_max_aggressive_dev_override.has_value());
    ASSERT_TRUE(cfg.pairs[0].bbo_sanity_max_passive_dev_override.has_value());
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].bbo_sanity_max_aggressive_dev_override, 0.05);
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].bbo_sanity_max_passive_dev_override, 0.45);
}

TEST(ConfigParserTest, BboSanityPerPairOverrides_AbsentMeansUnset) {
    // Absent must be nullopt, NOT a defaulted number: the engine tests
    // has_value() to decide whether to fall back to the strategy-level
    // value, so a silently defaulted override would pin every pair to it and
    // make the strategy-level setting unreachable.
    TempYaml tmp(kMinimalValidYaml);
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    EXPECT_FALSE(cfg.pairs[0].bbo_sanity_max_aggressive_dev_override.has_value());
    EXPECT_FALSE(cfg.pairs[0].bbo_sanity_max_passive_dev_override.has_value());
}

TEST(ConfigParserTest, BboSanityPerPairOverrides_RejectBpsEnteredByMistake) {
    // THE REGRESSION THE BOUND EXISTS FOR. 10 ("10%") and 1000 ("1000 bps")
    // are both plausible operator entries and both would produce a cap that
    // can never bind.
    for (const char* v : {"10", "100", "1000", "1.5"}) {
        TempYaml tmp(with_pair_extra(
            std::string("bbo_sanity_max_aggressive_dev_override: ") + v));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "aggressive override " << v << " must be rejected";
    }
    for (const char* v : {"10", "1000"}) {
        TempYaml tmp(with_pair_extra(
            std::string("bbo_sanity_max_passive_dev_override: ") + v));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "passive override " << v << " must be rejected";
    }
}

TEST(ConfigParserTest, BboSanityPerPairOverrides_RejectZeroNegativeNonFinite) {
    // Zero would suppress EVERY tier on that side. A config value that
    // silently stops a pair quoting is worse than one that fails to load.
    for (const char* v : {"0", "0.0", "-0.1", ".nan", ".inf", "-.inf"}) {
        TempYaml a(with_pair_extra(
            std::string("bbo_sanity_max_aggressive_dev_override: ") + v));
        EXPECT_THROW(xop::load_config(a.path()), xop::ConfigError)
            << "aggressive override " << v;
        TempYaml b(with_pair_extra(
            std::string("bbo_sanity_max_passive_dev_override: ") + v));
        EXPECT_THROW(xop::load_config(b.path()), xop::ConfigError)
            << "passive override " << v;
    }
}

TEST(ConfigParserTest, BboSanityPerPairOverrides_BoundaryOneIsAccepted) {
    // 1.0 == "100% from the touch" is the documented upper edge and must
    // LOAD. Pinning the boundary in the ACCEPTING direction matters: a
    // stricter-than-documented parser is what an operator discovers at 3am.
    TempYaml tmp(with_pair_extra(
        "bbo_sanity_max_aggressive_dev_override: 1.0\n"
        "    bbo_sanity_max_passive_dev_override: 1.0"));
    ASSERT_NO_THROW(xop::load_config(tmp.path()));
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].bbo_sanity_max_aggressive_dev_override, 1.0);
}

// ============================================================================
// [S33 2026-09-12] Per-pair activity / margin / spacing override PARSING.
//
// The behavioural tests for this family (test_activity_interpolation.cpp)
// build the interpolation inputs directly and never open a YAML file, so
// until now NOTHING drove these keys through load_config.  A key misspelled
// in parse_pairs, an optional bound to the wrong field, or a dropped bound
// would leave every one of those tests green while the deployed pair silently
// fell back to the global strategy value -- on XCH/BYC that is the difference
// between the Section I controller running and not existing at all.
//
// The bounds are NOT uniform, so each is pinned to what config.cpp actually
// enforces rather than to a house rule:
//   max_half_spread_bps_override             > 0
//   min_profit_margin_max_bps_override       > 0
//   tier_spacing_max_bps_override[i]         > 0   (per element)
//   activity_target_fills_24h_override       > 0   (int)
//   activity_book_weight_override            >= 0  (0 is legal)
//   fair_value_residual_widen_ratio_override >= 0  (0 is legal AND engaged)
//   activity_adaptive_spacing_override       bool  -- no range to violate
//   competitive_anchor_enabled_override      bool  -- no range to violate
// ============================================================================

TEST(ConfigParserTest, S33ActivityOverrides_ExplicitValuesRoundTrip) {
    TempYaml tmp(with_pair_extra(
        "max_half_spread_bps_override: 5000.0\n"
        "    min_profit_margin_max_bps_override: 800.0\n"
        "    tier_spacing_max_bps_override: [600, 1200, 1900, 2700, 3600, 4800]\n"
        "    activity_adaptive_spacing_override: true\n"
        "    activity_target_fills_24h_override: 24\n"
        "    activity_book_weight_override: 0.5\n"
        "    competitive_anchor_enabled_override: false\n"
        "    fair_value_residual_widen_ratio_override: 0.25"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    const auto& pc = cfg.pairs[0];

    ASSERT_TRUE(pc.max_half_spread_bps_override.has_value());
    EXPECT_DOUBLE_EQ(*pc.max_half_spread_bps_override, 5000.0);
    ASSERT_TRUE(pc.min_profit_margin_max_bps_override.has_value());
    EXPECT_DOUBLE_EQ(*pc.min_profit_margin_max_bps_override, 800.0);

    // The whole vector, not merely "a vector": a parser that read only the
    // first element would still satisfy has_value() while the outer tiers of
    // S_max silently reverted to the base ladder.
    ASSERT_TRUE(pc.tier_spacing_max_bps_override.has_value());
    ASSERT_EQ(pc.tier_spacing_max_bps_override->size(), 6u);
    EXPECT_DOUBLE_EQ((*pc.tier_spacing_max_bps_override)[0], 600.0);
    EXPECT_DOUBLE_EQ((*pc.tier_spacing_max_bps_override)[2], 1900.0);
    EXPECT_DOUBLE_EQ((*pc.tier_spacing_max_bps_override)[5], 4800.0);

    ASSERT_TRUE(pc.activity_adaptive_spacing_override.has_value());
    EXPECT_TRUE(*pc.activity_adaptive_spacing_override);
    ASSERT_TRUE(pc.activity_target_fills_24h_override.has_value());
    EXPECT_EQ(*pc.activity_target_fills_24h_override, 24);
    ASSERT_TRUE(pc.activity_book_weight_override.has_value());
    EXPECT_DOUBLE_EQ(*pc.activity_book_weight_override, 0.5);

    // Engaged-FALSE, not "unset".  This is the Section F knob: if a dropped
    // binding left it nullopt the pair would fall back to the global
    // competitive_anchor_enabled: true and the wide ladder would collapse
    // back into a 45 bps staircase -- with every behavioural test passing.
    ASSERT_TRUE(pc.competitive_anchor_enabled_override.has_value());
    EXPECT_FALSE(*pc.competitive_anchor_enabled_override);

    ASSERT_TRUE(pc.fair_value_residual_widen_ratio_override.has_value());
    EXPECT_DOUBLE_EQ(*pc.fair_value_residual_widen_ratio_override, 0.25);
}

TEST(ConfigParserTest, S33ActivityOverrides_ReversedBoolPolarityRoundTrips) {
    // The mirror of the test above: a parser that hard-coded either bool
    // would pass one polarity and fail the other.
    TempYaml tmp(with_pair_extra(
        "activity_adaptive_spacing_override: false\n"
        "    competitive_anchor_enabled_override: true"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    ASSERT_TRUE(cfg.pairs[0].activity_adaptive_spacing_override.has_value());
    EXPECT_FALSE(*cfg.pairs[0].activity_adaptive_spacing_override);
    ASSERT_TRUE(cfg.pairs[0].competitive_anchor_enabled_override.has_value());
    EXPECT_TRUE(*cfg.pairs[0].competitive_anchor_enabled_override);
}

TEST(ConfigParserTest, S33ActivityOverrides_AbsentMeansUnsetNotDefaulted) {
    // Absence must be nullopt, NOT a default-constructed value: every
    // consumption site in Step 7 is `override.value_or(strategy_value)`, so a
    // defaulted optional would pin every pair to 0 / false and make the
    // strategy-level setting unreachable.
    TempYaml tmp(kMinimalValidYaml);
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    const auto& pc = cfg.pairs[0];
    EXPECT_FALSE(pc.max_half_spread_bps_override.has_value());
    EXPECT_FALSE(pc.min_profit_margin_max_bps_override.has_value());
    EXPECT_FALSE(pc.tier_spacing_max_bps_override.has_value());
    EXPECT_FALSE(pc.activity_adaptive_spacing_override.has_value());
    EXPECT_FALSE(pc.activity_target_fills_24h_override.has_value());
    EXPECT_FALSE(pc.activity_book_weight_override.has_value());
    EXPECT_FALSE(pc.competitive_anchor_enabled_override.has_value());
    EXPECT_FALSE(pc.fair_value_residual_widen_ratio_override.has_value());
}

TEST(ConfigParserTest, S33ActivityOverrides_OutOfRangeValuesRejected) {
    // Strictly positive knobs.  `.nan` was once asserted only where the guard
    // was written `!(v > 0.0)`, because the `v < 0.0` guards accepted it.
    // That hole is now closed for this whole family -- see
    // S33ActivityOverrides_NonFiniteRejected below, which owns the non-finite
    // cases; the ordinary out-of-range values stay here.
    for (const char* v : {"0", "0.0", "-1.0", ".nan"}) {
        TempYaml tmp(with_pair_extra(
            std::string("max_half_spread_bps_override: ") + v));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "max_half_spread_bps_override " << v;
    }
    for (const char* v : {"0", "0.0", "-800.0"}) {
        TempYaml tmp(with_pair_extra(
            std::string("min_profit_margin_max_bps_override: ") + v));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "min_profit_margin_max_bps_override " << v;
    }
    for (const char* v : {"0", "-24"}) {
        TempYaml tmp(with_pair_extra(
            std::string("activity_target_fills_24h_override: ") + v));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "activity_target_fills_24h_override " << v;
    }
    // These two are >= 0, so only the negative side is out of range.
    for (const char* v : {"-0.5", "-1.0"}) {
        TempYaml tmp(with_pair_extra(
            std::string("activity_book_weight_override: ") + v));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "activity_book_weight_override " << v;
    }
    for (const char* v : {"-0.25", "-1.0"}) {
        TempYaml tmp(with_pair_extra(
            std::string("fair_value_residual_widen_ratio_override: ") + v));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "fair_value_residual_widen_ratio_override " << v;
    }
    // Per ELEMENT, not just the first: a bad maximum anywhere in S_max would
    // run that tier's interpolation backwards.
    // [S33 2026-09-12] `.inf` at BOTH positions. The old `!(v > 0.0)` guard
    // rejected NaN but ADMITTED +inf, and the two positions fail
    // differently: at index 0 shift_schedule_to_floor spreads the resulting
    // NaN across EVERY tier of BOTH side schedules, so the pair stops
    // quoting outright; at a later index only that tier is lost.
    for (const char* seq : {"[0, 1200, 1900]", "[600, -1200, 1900]",
                            "[600, 1200, .nan]",
                            "[.inf, 1200, 1900]", "[600, 1200, .inf]"}) {
        TempYaml tmp(with_pair_extra(
            std::string("tier_spacing_max_bps_override: ") + seq));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "tier_spacing_max_bps_override " << seq;
    }
}

TEST(ConfigParserTest, S33ActivityOverrides_LegalZeroIsAcceptedAndEngaged) {
    // 0.0 is legal for both of these AND must arrive ENGAGED, not read as a
    // fallback to the global.  Section C turns on exactly this:
    // fair_value_residual_widen_ratio_override: 0.0 is how XCH/BYC disables
    // the Step 7 symmetric widener, and activity_book_weight_override: 0.0 is
    // how it drives the activity score from realized fills alone.  A parser
    // that treated 0.0 as "absent" would re-arm both globals.
    TempYaml tmp(with_pair_extra(
        "activity_book_weight_override: 0.0\n"
        "    fair_value_residual_widen_ratio_override: 0.0"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    ASSERT_TRUE(cfg.pairs[0].activity_book_weight_override.has_value());
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].activity_book_weight_override, 0.0);
    ASSERT_TRUE(cfg.pairs[0].fair_value_residual_widen_ratio_override.has_value());
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].fair_value_residual_widen_ratio_override, 0.0);
}

TEST(ConfigParserTest, S33ActivityOverrides_EmptyMaxSpacingSequenceIsNotAnOverride) {
    // Observed parser behaviour, pinned so it cannot drift unnoticed: the
    // max-spacing block is gated on IsSequence() && size() > 0, so `[]` loads
    // and leaves the optional UNSET (the pair keeps its base ladder) rather
    // than installing an empty S_max for the Step 7 interpolation to index.
    TempYaml tmp(with_pair_extra("tier_spacing_max_bps_override: []"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    EXPECT_FALSE(cfg.pairs[0].tier_spacing_max_bps_override.has_value());
}

// ============================================================================
// [OFFER-EXPIRY 2026-09-12] The per-pair expiry override, through the PARSER.
//
// test_offer_expiry.cpp drives effective_offer_expiry_secs() with an optional
// it builds itself and never opens a YAML file -- this file contained ZERO
// mentions of offer_expiry before these tests.  The gap is not cosmetic: the
// bind is also the GATE on the startup floor check in OfferManager, so a
// parser that quietly stopped binding would remove the pair from the one
// validation standing between a short expiry and offers the chain retires
// while the engine still tracks them as live.  Deleting the bind reddened
// nothing before this.
// ============================================================================

TEST(ConfigParserTest, OfferExpiryOverride_ExplicitValueRoundTrips) {
    // The load-bearing direction.  If this value never reaches the config the
    // pair falls back to strategy.offer_expiry_secs -- 0 by default, i.e. NO
    // timelock on precisely the pair configured to carry one, silently.
    TempYaml tmp(with_pair_extra("offer_expiry_secs_override: 172800"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    ASSERT_TRUE(cfg.pairs[0].offer_expiry_secs_override.has_value());
    EXPECT_EQ(*cfg.pairs[0].offer_expiry_secs_override, 172800u);
}

TEST(ConfigParserTest, OfferExpiryOverride_ZeroBindsRatherThanInheriting) {
    // 0 is a REAL setting -- "never expire this pair's offers" -- and must
    // BIND, not read as absence.  The pure test that pins the value_or side
    // builds its optional directly, so it stays green under a parser that
    // drops an explicit 0.
    TempYaml tmp(with_pair_extra("offer_expiry_secs_override: 0"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    ASSERT_TRUE(cfg.pairs[0].offer_expiry_secs_override.has_value());
    EXPECT_EQ(*cfg.pairs[0].offer_expiry_secs_override, 0u);
}

TEST(ConfigParserTest, OfferExpiryOverride_AbsentLeavesTheOptionalUnset) {
    // Absence must be nullopt, NOT a defaulted 0: the consumer is
    // effective_offer_expiry_secs(override, global), so a defaulted optional
    // would pin every pair to "no expiry" and make the global unreachable.
    TempYaml tmp(kMinimalValidYaml);
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    EXPECT_FALSE(cfg.pairs[0].offer_expiry_secs_override.has_value());
}

TEST(ConfigParserTest, OfferExpiryOverride_NegativeAndAboveUint32Rejected) {
    // CWE-681: straight to uint32 a YAML -1 wraps to 4294967295s (~136
    // years), which reads as "configured" and behaves as "never expires" --
    // and being enormous it also sails through the OfferManager floor check,
    // so nothing downstream catches it.
    //
    // 4294967296 is UINT32_MAX + 1, the smallest value the upper bound owns,
    // and deliberately not a larger literal: as<std::int64_t>() throws
    // YAML::TypedBadConversion before this code runs, and parse_pairs is not
    // inside a YAML::Exception handler, so a bigger number would not surface
    // as a ConfigError at all.
    for (const char* v : {"-1", "-86400", "4294967296"}) {
        TempYaml tmp(with_pair_extra(
            std::string("offer_expiry_secs_override: ") + v));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "offer_expiry_secs_override " << v;
    }
}

// ============================================================================
// [S33 2026-09-12] Non-finite values in the numeric knobs.
//
// yaml-cpp accepts `.nan` and `.inf`, and NaN makes EVERY comparison false --
// so `v < 0.0` and `v <= 0.0` were not bounds at all against it.  What gets
// through is not a visibly wrong number in a log line:
//
//   * a NaN fair_value_residual_widen_ratio_override then fails the Step 7
//     consumer's own `widen_ratio > 0.0` gate, SILENTLY DISABLING the
//     residual widener on precisely the pair configured to use it;
//   * a non-finite activity_book_weight_override rides eff_bids/eff_asks
//     through the interpolated side spacings into the ladder prices, as a
//     double, right up to the integer conversion;
//   * a non-finite min_profit_margin_max_bps_override collapses the activity
//     interpolation that reads it as the wide end.
//
// The house already rejects non-finite values explicitly at peg_target,
// strategy.xch_cycle_commit_frac and the market_data bounds, so these sites
// were an inconsistency rather than a deliberate permissiveness.
// ============================================================================

namespace {

/// Assert load_config rejects `yaml` AND that the message names `key` and
/// cites finiteness.  A bare EXPECT_THROW would also be satisfied by a throw
/// for an unrelated reason -- a mis-spliced YAML line, say -- and would then
/// pin nothing at all while reading as a passing guard test.
void expect_non_finite_rejected(const std::string& yaml,
                                const char* key,
                                const char* bad) {
    TempYaml tmp(yaml);
    try {
        xop::load_config(tmp.path());
        ADD_FAILURE() << key << " accepted " << bad;
    } catch (const xop::ConfigError& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find(key), std::string::npos)
            << key << " = " << bad << ": threw, but not about that key: "
            << msg;
        EXPECT_NE(msg.find("finite"), std::string::npos)
            << key << " = " << bad << ": threw, but not the finiteness "
            << "guard: " << msg;
    }
}

}  // namespace

TEST(ConfigParserTest, S33ActivityOverrides_NonFiniteRejected) {
    // The three per-pair keys whose guards were written `v < 0.0` / `v <= 0.0`
    // and so admitted NaN; `.inf` is the other half of the same hole.
    for (const char* key : {"fair_value_residual_widen_ratio_override",
                            "activity_book_weight_override",
                            "min_profit_margin_max_bps_override"}) {
        for (const char* bad : {".nan", ".inf", "-.inf"}) {
            expect_non_finite_rejected(
                with_pair_extra(std::string(key) + ": " + bad), key, bad);
        }
    }
}

TEST(ConfigParserTest, PairPositiveOverrides_NonFiniteRejected) {
    // [INFGUARD] The OTHER half of the same hole. These keys were guarded
    // `!(v > 0.0)`, which rejects NaN (every NaN comparison is false) but
    // ACCEPTS +infinity. The two that matter most for safety:
    // depeg_bail_pct at +inf permanently disables the depeg bail, and
    // min_offer_size_units_override at +inf silently stops the pair quoting.
    for (const char* key : {"gamma_override",
                            "kappa_override",
                            "phi_override",
                            "q_max_override",
                            "min_profit_margin_bps_override",
                            "depeg_warn_pct",
                            "depeg_bail_pct",
                            "competitive_anchor_max_distance_bps_override",
                            "competitive_anchor_stride_bps_override",
                            // [review #151] Its own loop in
                            // S33ActivityOverrides_OutOfRangeValuesRejected
                            // carries .nan but NOT .inf, and this guard is a
                            // SEPARATE parser copy: removing its isfinite
                            // left the suite green.
                            "max_half_spread_bps_override"}) {
        for (const char* bad : {".inf", "-.inf", ".nan"}) {
            expect_non_finite_rejected(
                with_pair_extra(std::string(key) + ": " + bad), key, bad);
        }
    }
    // >= 0 variant: 0.0 must STILL parse (it is the documented disable), so
    // only the non-finite legs are asserted here.
    for (const char* bad : {".inf", "-.inf", ".nan"}) {
        expect_non_finite_rejected(
            with_pair_extra(std::string("min_offer_size_units_override: ")
                            + bad),
            "min_offer_size_units_override", bad);
    }

    // [review #151] The NARROW-end sequence had NO coverage of any kind --
    // zero mentions in this file -- while carrying its own copy of the
    // guard. Both positions, because a first-element failure and a later one
    // propagate differently through the ladder.
    //
    // Two elements, not three: kMinimalValidYaml declares num_tiers: 2, so a
    // longer list risks throwing on a LENGTH check instead of the finiteness
    // guard -- which would look like coverage while proving nothing.
    for (const char* seq : {"[.inf, 80]", "[40, .inf]",
                            "[-.inf, 80]", "[40, .nan]"}) {
        expect_non_finite_rejected(
            with_pair_extra(std::string("tier_spacing_bps_override: ") + seq),
            "tier_spacing_bps_override", seq);
    }
}

TEST(ConfigParserTest, PairPositiveOverrides_ZeroDisableStillParses) {
    // The acceptance direction. A guard that over-rejected would remove the
    // operator's documented escape hatch at load time.
    TempYaml tmp(with_pair_extra("min_offer_size_units_override: 0.0"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    ASSERT_TRUE(cfg.pairs[0].min_offer_size_units_override.has_value());
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].min_offer_size_units_override, 0.0);
}

TEST(ConfigParserTest, GlobalStrategyScalars_NonFiniteRejected) {
    // [INFGUARD] The REQUIRED global keys, which share read_positive_double.
    // This is the larger half of the hole: q_max at +inf yields an infinite
    // order size that becomes INT64_MIN downstream, and the pair posts
    // nothing from the first heartbeat while the config loads cleanly.
    struct Sub { const char* line; const char* prefix; const char* key; };
    const Sub subs[] = {
        {"  gamma: 0.01",                  "  gamma: ",                 "gamma"},
        {"  kappa: 1.5",                   "  kappa: ",                 "kappa"},
        {"  phi: 0.5",                     "  phi: ",                   "phi"},
        {"  q_max: 1000.0",                "  q_max: ",                 "q_max"},
        {"  min_profit_margin_bps: 35.0",  "  min_profit_margin_bps: ",
         "min_profit_margin_bps"},
    };
    for (const auto& s : subs) {
        for (const char* bad : {".inf", "-.inf", ".nan"}) {
            expect_non_finite_rejected(
                with_strategy_replaced(s.line, std::string(s.prefix) + bad),
                s.key, bad);
        }
    }
}

TEST(ConfigParserTest, GlobalTierSpacingSeq_NonFiniteRejected) {
    // read_positive_double_seq: an infinite spacing reaches
    // `mid * (1.0 - v/10000.0)` and then a static_cast<int64_t> of -inf.
    for (const char* bad : {".inf", "-.inf", ".nan"}) {
        expect_non_finite_rejected(
            with_strategy_replaced("  tier_spacing_bps: [40, 80]",
                                   std::string("  tier_spacing_bps: [")
                                   + bad + ", 80]"),
            "tier_spacing_bps", bad);
    }
}

TEST(ConfigParserTest, GlobalMaxHalfSpread_NonFiniteRejected) {
    // The `<= 0.0` twin, which admitted BOTH NaN and +inf.
    for (const char* bad : {".inf", "-.inf", ".nan"}) {
        expect_non_finite_rejected(
            with_strategy_keys(std::string("\n  max_half_spread_bps: ") + bad),
            "max_half_spread_bps", bad);
    }
}

TEST(ConfigParserTest, GlobalBlockTimeSeconds_NonFiniteRejected) {
    // [INFGUARD] Found independently by review on #150. Same `<= 0.0` shape
    // as the max_half_spread_bps twin, so it admitted NaN and +inf alike.
    // Downstream this value is used to convert block counts into wall-clock
    // bounds, and a non-finite result reaches static_cast<std::int64_t>,
    // which is undefined behaviour -- not a clamped number.
    for (const char* bad : {".inf", "-.inf", ".nan"}) {
        expect_non_finite_rejected(
            with_strategy_keys(std::string(R"(
  block_time_seconds: )") + bad),
            "block_time_seconds", bad);
    }
}

TEST(ConfigParserTest, S33ActivityOverrides_FiniteValuesStillParseAfterGuard) {
    // The acceptance direction, which matters as much as the rejection: 0.0 is
    // the DOCUMENTED disable for both >= 0 knobs (Section C drives XCH/BYC
    // with exactly these), and a guard that over-rejected would take the
    // operator's escape hatch away at load time.
    TempYaml tmp(with_pair_extra(
        "min_profit_margin_max_bps_override: 800.0\n"
        "    activity_book_weight_override: 0.0\n"
        "    fair_value_residual_widen_ratio_override: 0.0"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_FALSE(cfg.pairs.empty());
    const auto& pc = cfg.pairs[0];
    ASSERT_TRUE(pc.min_profit_margin_max_bps_override.has_value());
    EXPECT_DOUBLE_EQ(*pc.min_profit_margin_max_bps_override, 800.0);
    ASSERT_TRUE(pc.activity_book_weight_override.has_value());
    EXPECT_DOUBLE_EQ(*pc.activity_book_weight_override, 0.0);
    ASSERT_TRUE(pc.fair_value_residual_widen_ratio_override.has_value());
    EXPECT_DOUBLE_EQ(*pc.fair_value_residual_widen_ratio_override, 0.0);
}

TEST(ConfigParserTest, S33BookSideAgreeOverride_RoundTripsAbsentAndLegalZero) {
    // [S33 2026-09-12] The per-pair two-sides-agree ceiling had no PARSER
    // test.  The behavioural tests build a MarketDataConfig by hand and call
    // set_agree_max_spread_bps_for() directly, so a misspelled key or a lost
    // `p.` assignment in the pairs loop would leave this optional empty, drop
    // the pair back to the bot-wide 5000 bps default, and break no test.
    // The fallback is silent and strictly MORE permissive -- it re-arms the
    // two-sides-agree bypass on a dislocated book, which is the phantom mark
    // this branch exists to stop.  Parser only: engine.cpp populating the
    // MarketDataConfig map from these optionals is not reachable from here.

    // Round-trip: pins the key SPELLING and the optional binding.
    TempYaml tmp_set(with_pair_extra(
        "book_side_agree_max_spread_bps_override: 1500.0"));
    auto cfg_set = xop::load_config(tmp_set.path());
    ASSERT_FALSE(cfg_set.pairs.empty());
    const auto& pc_set = cfg_set.pairs[0];
    ASSERT_TRUE(pc_set.book_side_agree_max_spread_bps_override.has_value());
    EXPECT_DOUBLE_EQ(*pc_set.book_side_agree_max_spread_bps_override, 1500.0);

    // Absent: nullopt, NOT a defaulted number.  The engine tests the optional
    // to decide whether to record a map entry at all, so a defaulted 0 would
    // pin every pair to a DISABLED bypass instead of the global value.
    TempYaml tmp_absent(kMinimalValidYaml);
    auto cfg_absent = xop::load_config(tmp_absent.path());
    ASSERT_FALSE(cfg_absent.pairs.empty());
    EXPECT_FALSE(cfg_absent.pairs[0]
                     .book_side_agree_max_spread_bps_override.has_value());

    // Legal zero, ENGAGED.  0 is the documented "bypass off" SETTING for the
    // pair, not absence; a presence check rewritten as a truthiness check
    // (`v > 0.0`) breaks only this case and leaves 1500.0 working.
    TempYaml tmp_zero(with_pair_extra(
        "book_side_agree_max_spread_bps_override: 0.0"));
    auto cfg_zero = xop::load_config(tmp_zero.path());
    ASSERT_FALSE(cfg_zero.pairs.empty());
    const auto& pc_zero = cfg_zero.pairs[0];
    ASSERT_TRUE(pc_zero.book_side_agree_max_spread_bps_override.has_value());
    EXPECT_DOUBLE_EQ(*pc_zero.book_side_agree_max_spread_bps_override, 0.0);
}

TEST(ConfigParserTest, S33BookSideAgreeOverride_NonFiniteRejected) {
    // [review] The fourth case the review asked for, and the rejection twin
    // of the round-trip test above.  yaml-cpp hands back `.nan` / `.inf`
    // happily; the guard is `!std::isfinite(v) || v < 0.0`, and THIS test
    // pins only the isfinite conjunct -- the negative half is pinned
    // separately below so a mutation can tell the two apart.
    //
    // An admitted `.inf` here would be the permissive direction: an infinite
    // ceiling re-arms the two-sides-agree bypass on any book, which is the
    // phantom mark this branch exists to stop.
    const char* const kKey = "book_side_agree_max_spread_bps_override";
    for (const char* bad : {".nan", ".inf", "-.inf"}) {
        expect_non_finite_rejected(
            with_pair_extra(std::string(kKey) + ": " + bad), kKey, bad);
    }
}

TEST(ConfigParserTest, S33BookSideAgreeOverride_NegativeRejected) {
    // The OTHER conjunct.  A finite negative is not "non-finite", so it does
    // not belong in the helper above despite throwing from the same site --
    // and keeping it separate is what lets a mutation that drops `v < 0.0`
    // go red HERE while the non-finite test stays green.
    //
    // Not a bare EXPECT_THROW: load_config throws for many reasons, and a
    // mis-spliced YAML line would satisfy one while pinning nothing.  The
    // message must name the key.
    const char* const kKey = "book_side_agree_max_spread_bps_override";
    for (const char* bad : {"-1.0", "-0.1"}) {
        TempYaml tmp(with_pair_extra(std::string(kKey) + ": " + bad));
        try {
            xop::load_config(tmp.path());
            ADD_FAILURE() << kKey << " accepted negative " << bad;
        } catch (const xop::ConfigError& e) {
            const std::string msg = e.what();
            EXPECT_NE(msg.find(kKey), std::string::npos)
                << kKey << " = " << bad
                << ": threw, but not about that key: " << msg;
        }
    }
}

TEST(ConfigParserTest, StrategyNonNegativeKnobs_NonFiniteRejected) {
    // The shared opt_non_negative helper feeds all 23 keys below, so the hole
    // was 23 keys wide -- including activity_book_weight, newly exposed as a
    // global by this branch.  Every one of them is a magnitude whose
    // documented "disabled" setting is 0 (config.hpp), so none wants +inf.
    for (const char* key : {"max_fair_value_deviation_bps",
                            "blind_quote_widen_pct",
                            "fair_value_clamp_tier_step_bps",
                            "quote_width_sigma_mult",
                            "as_reservation_gamma",
                            "as_reservation_max_offset_bps",
                            "fair_value_feed_sigma_bps",
                            "fair_value_amm_sigma_bps",
                            "fair_value_amm_depth_k_bps",
                            "fair_value_amm_max_age_sec",
                            "fair_value_min_book_sigma_bps",
                            "fair_value_stale_sigma_bps_per_print",
                            "fair_value_depth_ref_bps",
                            "fair_value_max_sigma_bps",
                            "fair_value_tight_sigma_bps",
                            "fair_value_sigma_band_mult",
                            "fair_value_residual_widen_ratio",
                            "fair_value_residual_widen_floor_bps",
                            "microprice_narrow_bps",
                            "microprice_wide_bps",
                            "published_mid_band_floor_bps",
                            "published_mid_band_spread_frac",
                            "activity_book_weight"}) {
        for (const char* bad : {".nan", ".inf", "-.inf"}) {
            expect_non_finite_rejected(
                with_strategy_keys(std::string("\n  ") + key + ": " + bad),
                key, bad);
        }
    }
}

TEST(ConfigParserTest, StrategyNonNegativeKnobs_FiniteValuesStillParse) {
    // Valid values must survive the guard and reach the config.  The pairs
    // chosen here also satisfy the two cross-checks that run AFTER the
    // helper (wide > narrow, tight <= max), so a failure here is the guard
    // and not a coherence rule firing.
    TempYaml tmp(with_strategy_keys(
        "\n  activity_book_weight: 0.75"
        "\n  fair_value_residual_widen_ratio: 0.3"
        "\n  microprice_narrow_bps: 150.0"
        "\n  microprice_wide_bps: 900.0"
        "\n  fair_value_max_sigma_bps: 250.0"
        "\n  fair_value_tight_sigma_bps: 120.0"
        "\n  quote_width_sigma_mult: 0.0"));
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.strategy.activity_book_weight, 0.75);
    EXPECT_DOUBLE_EQ(cfg.strategy.fair_value_residual_widen_ratio, 0.3);
    EXPECT_DOUBLE_EQ(cfg.strategy.microprice_narrow_bps, 150.0);
    EXPECT_DOUBLE_EQ(cfg.strategy.microprice_wide_bps, 900.0);
    EXPECT_DOUBLE_EQ(cfg.strategy.fair_value_max_sigma_bps, 250.0);
    EXPECT_DOUBLE_EQ(cfg.strategy.fair_value_tight_sigma_bps, 120.0);
    // 0 is the documented "disabled" setting for the sigma term and must
    // still load -- the finiteness guard must not narrow the legal domain.
    EXPECT_DOUBLE_EQ(cfg.strategy.quote_width_sigma_mult, 0.0);
}

TEST(ConfigParserTest, SideQualityKnobs_ExplicitValuesParse) {
    TempYaml tmp(with_market_data(
        "  book_side_anchor_band_ratio: 2.5\n"
        "  book_side_agree_max_spread_bps: 750.0\n"));
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.market_data.book_side_anchor_band_ratio, 2.5);
    EXPECT_DOUBLE_EQ(cfg.market_data.book_side_agree_max_spread_bps, 750.0);
}

TEST(ConfigParserTest, SideQualityKnobs_DefaultsWhenAbsent) {
    {
        // No market_data section at all.
        TempYaml tmp(kMinimalValidYaml);
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.market_data.book_side_anchor_band_ratio, 3.0);
        EXPECT_DOUBLE_EQ(cfg.market_data.book_side_agree_max_spread_bps,
                         5000.0);
    }
    {
        // Section present, both keys absent: the neighbouring key must not
        // drag either default off its documented value.
        TempYaml tmp(with_market_data(
            "  cex_freshness_threshold_sec: 600\n"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.market_data.book_side_anchor_band_ratio, 3.0);
        EXPECT_DOUBLE_EQ(cfg.market_data.book_side_agree_max_spread_bps,
                         5000.0);
    }
}

TEST(ConfigParserTest, SideQualityKnobs_NonFiniteRejected) {
    for (const char* bad : {".nan", ".inf", "-.inf"}) {
        {
            TempYaml tmp(with_market_data(
                std::string("  book_side_anchor_band_ratio: ") + bad));
            EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
                << "book_side_anchor_band_ratio accepted " << bad;
        }
        {
            TempYaml tmp(with_market_data(
                std::string("  book_side_agree_max_spread_bps: ") + bad));
            EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
                << "book_side_agree_max_spread_bps accepted " << bad;
        }
    }
}

TEST(ConfigParserTest, SideQualityKnobs_NegativeRejected) {
    {
        TempYaml tmp(with_market_data(
            "  book_side_anchor_band_ratio: -1.0\n"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    {
        TempYaml tmp(with_market_data(
            "  book_side_agree_max_spread_bps: -0.5\n"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
}

TEST(ConfigParserTest, SideQualityKnobs_DisabledBandAccepted) {
    // <= 1.0 DISABLES the per-side anchor test; it is a legal setting, not a
    // malformed one.  Rejecting it here is the regression this case exists to
    // catch -- assert acceptance AND that the value survives to the config.
    const struct { const char* written; double expected; } kDisabling[] = {
        {"0.0", 0.0}, {"0.5", 0.5}, {"1.0", 1.0},
    };
    for (const auto& d : kDisabling) {
        TempYaml tmp(with_market_data(
            std::string("  book_side_anchor_band_ratio: ") + d.written + "\n"));
        ASSERT_NO_THROW(xop::load_config(tmp.path()))
            << "disabling value rejected: " << d.written;
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.market_data.book_side_anchor_band_ratio,
                         d.expected)
            << "disabling value not carried through: " << d.written;
    }
    {
        // 0 on the agreement cap is likewise legal: it means no two-sided
        // book is ever narrow enough to be trusted whole.
        //
        // [review round 6, PR #134] THAT SENTENCE USED TO BE FALSE.  The
        // effective threshold was max(configured, mid-gate confirm), so 0
        // here returned the gate's 5000 and the bypass carried on firing.
        // The comment promised operators an escape hatch that did not
        // exist, and the test asserted only that the number survived the
        // parser -- which it did, on its way to being discarded.
        //
        // The helper now returns min(), so the sentence is true, and the
        // load-level assertion is followed by the RUNTIME assertion that
        // makes it a contract instead of a claim.
        TempYaml tmp(with_market_data(
            "  book_side_agree_max_spread_bps: 0.0\n"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_DOUBLE_EQ(cfg.market_data.book_side_agree_max_spread_bps, 0.0);

        // The value the classifier ACTUALLY uses, against the gate's
        // shipped default rather than a hand-written constant.
        const double effective =
            xop::bookside::effective_agree_max_spread_bps(
                cfg.market_data.book_side_agree_max_spread_bps,
                cfg.market_data.mid_gate_book_confirm_max_spread_bps);
        ASSERT_GT(cfg.market_data.mid_gate_book_confirm_max_spread_bps, 0.0)
            << "sanity: the gate's own threshold must be non-zero, or this "
               "case cannot distinguish min() from max()";
        EXPECT_DOUBLE_EQ(effective, 0.0)
            << "0 must BIND, not be treated as 'unset' and replaced by the "
               "mid gate's threshold";

        // And the behaviour the operator was promised: a book so tight it
        // would otherwise be trusted whole (10 bps) gets no bypass, and the
        // per-side band is what decides.
        const auto q = xop::bookside::classify_sides(
            /*best_bid=*/5.60, /*best_ask=*/5.6056,
            /*anchor=*/1.41022765,
            cfg.market_data.book_side_anchor_band_ratio, effective);
        EXPECT_FALSE(q.bypassed)
            << "with the cap at 0 no two-sided book may be trusted whole";
        EXPECT_FALSE(q.ask_ok)
            << "3.97x the anchor, and nothing bypassed the band";
    }
}

namespace {

/// Capturing sink for the advisory warnings config.cpp emits through the
/// DEFAULT logger.  Same technique as cpp/tests/test_client_logger.cpp: a
/// ringbuffer sink installed as the default logger for the duration of one
/// test, then removed.
using LogProbe = spdlog::sinks::ringbuffer_sink_mt;

/// Installs a capturing default logger and restores the previous one.
/// spdlog's default logger and registry are PROCESS state, so this must be
/// exception-safe and must not leave the probe installed for later tests.
class CapturedLog {
public:
    /// Capacity is generous on purpose: a ringbuffer keeps only the LAST N
    /// records, and load_config emits other advisories.  A tight buffer would
    /// silently evict the line under test and fail for the wrong reason.
    explicit CapturedLog(std::size_t capacity = 256)
        : probe_(std::make_shared<LogProbe>(capacity)),
          saved_(spdlog::default_logger())
    {
        auto logger = std::make_shared<spdlog::logger>("config_test_probe",
                                                       probe_);
        // Level set on THIS logger only.  spdlog::set_level() would stamp
        // every registered logger in the process, which test_client_logger.cpp
        // asserts on -- a global side effect is not worth one advisory line.
        logger->set_level(spdlog::level::trace);
        spdlog::set_default_logger(logger);
    }

    ~CapturedLog() {
        spdlog::set_default_logger(saved_);
        spdlog::drop("config_test_probe");
    }

    CapturedLog(const CapturedLog&)            = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    /// Every captured payload joined by newlines, for substring matching.
    [[nodiscard]] std::string text() const {
        std::string out;
        for (const auto& rec : probe_->last_raw()) {
            out.append(rec.payload.data(), rec.payload.size());
            out.push_back('\n');
        }
        return out;
    }

    /// True if any captured record at >= warn level contains `needle`.
    [[nodiscard]] bool warned_containing(const std::string& needle) const {
        for (const auto& rec : probe_->last_raw()) {
            if (rec.level < spdlog::level::warn) continue;
            const std::string payload(rec.payload.data(), rec.payload.size());
            if (payload.find(needle) != std::string::npos) return true;
        }
        return false;
    }

private:
    std::shared_ptr<LogProbe>       probe_;
    std::shared_ptr<spdlog::logger> saved_;
};

}  // namespace

TEST(ConfigParserTest, SideQualityKnobs_WiderThanMidBandWarnsAndLoads) {
    // A side band wider than mid_anchor_band_ratio lets a side stay a trusted
    // reference while the mid it implies is refused.  config.cpp treats that
    // as a coherence smell: it emits an advisory spdlog WARNING and loads the
    // values anyway.  BOTH halves are checked here.
    //
    // [review round 6, PR #134] The previous version of this test asserted
    // only the load and said so in a comment that argued sink capture was not
    // worth the trouble.  The consequence was that deleting the warning
    // outright left the case green -- the exact vacuity this repo's
    // mutation-check rule exists to catch.  test_client_logger.cpp already
    // owned the capture technique, so the cost was a fixture, not machinery.
    CapturedLog log;

    TempYaml tmp(with_market_data(
        "  mid_anchor_band_ratio: 2.0\n"
        "  book_side_anchor_band_ratio: 6.0\n"));
    ASSERT_NO_THROW(xop::load_config(tmp.path()));
    auto cfg = xop::load_config(tmp.path());
    EXPECT_DOUBLE_EQ(cfg.market_data.mid_anchor_band_ratio, 2.0);
    EXPECT_DOUBLE_EQ(cfg.market_data.book_side_anchor_band_ratio, 6.0);

    // Matched on the KEY NAMES and the numbers, not on the prose: the
    // advisory's wording may legitimately be reworded, but a warning that no
    // longer names which two knobs disagree is not the same warning.
    EXPECT_TRUE(log.warned_containing("book_side_anchor_band_ratio"))
        << "captured log was:\n" << log.text();
    EXPECT_TRUE(log.warned_containing("mid_anchor_band_ratio"))
        << "captured log was:\n" << log.text();
    EXPECT_TRUE(log.warned_containing("6.00"))
        << "the advisory must report the offending value; captured log was:\n"
        << log.text();
    EXPECT_TRUE(log.warned_containing("2.00"))
        << "captured log was:\n" << log.text();
}

TEST(ConfigParserTest, SideQualityKnobs_CoherentBandsDoNotWarn) {
    // The negative half, without which the assertions above would also pass
    // against a warning emitted unconditionally on every load.
    CapturedLog log;

    TempYaml tmp(with_market_data(
        "  mid_anchor_band_ratio: 6.0\n"
        "  book_side_anchor_band_ratio: 2.0\n"));
    ASSERT_NO_THROW(xop::load_config(tmp.path()));

    EXPECT_FALSE(log.warned_containing("book_side_anchor_band_ratio"))
        << "a side band NARROWER than the mid band is coherent and must be "
           "silent; captured log was:\n" << log.text();
}

TEST(ConfigParserTest, S20CarryTtl_ParsesAndRejectsNegative) {
    {
        TempYaml tmp(with_risk_extra("valuation_carry_ttl_blocks: 720"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_EQ(cfg.risk.valuation_carry_ttl_blocks, 720u);
    }
    {
        // 0 is the documented "expiry disabled" value and must parse.
        TempYaml tmp(with_risk_extra("valuation_carry_ttl_blocks: 0"));
        auto cfg = xop::load_config(tmp.path());
        EXPECT_EQ(cfg.risk.valuation_carry_ttl_blocks, 0u);
    }
    {
        TempYaml tmp(with_risk_extra("valuation_carry_ttl_blocks: -1"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
}

// [S20 2026-08-24] A non-finite peg_target must fail startup.
//
// The peg is the FIRST-CYCLE anchor for a stablecoin pair -- the one thing
// between a freshly restarted process and the junk-book poisoning this
// release exists to stop.  The historical `!(v > 0)` test catches NaN but
// passes +inf, and select_anchor then discards the infinity as unusable,
// leaving the pair silently anchorless on exactly that path.
TEST(ConfigParserTest, S20NonFinitePegTargetRejected) {
    auto with_peg = [](const char* v) {
        std::string s(kMinimalValidYaml);
        const std::string anchor = "    name: \"XCH/TEST\"\n";
        const auto pos = s.find(anchor);
        EXPECT_NE(pos, std::string::npos);
        s.insert(pos + anchor.size(),
                 std::string("    is_stablecoin: true\n    peg_target: ")
                 + v + "\n");
        return s;
    };

    for (const char* bad : {".inf", "-.inf", ".nan", "0", "-1.0"}) {
        TempYaml tmp(with_peg(bad));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "peg_target accepted " << bad;
    }

    TempYaml ok(with_peg("1.0"));
    EXPECT_NO_THROW({
        auto cfg = xop::load_config(ok.path());
        EXPECT_DOUBLE_EQ(cfg.pairs[0].peg_target, 1.0);
    });
}

TEST(ConfigParserTest, S31_NegativeWatchdogThresholdIsRejectedNotWrapped) {
    // Parsing straight to uint32 let -1 wrap to UINT32_MAX: a ~136-year
    // threshold that reads as "configured" and behaves as "disabled" -- the
    // worst outcome for a safety switch.
    TempYaml tmp(with_risk_extra("watchdog_stall_seconds: -1"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, S31_ZeroWatchdogThresholdStaysLegal) {
    // 0 is the documented way to disable it and must survive the range check.
    TempYaml tmp(with_risk_extra("watchdog_stall_seconds: 0"));
    auto cfg = xop::load_config(tmp.path());
    EXPECT_EQ(cfg.risk.watchdog_stall_seconds, 0u);
}

// ============================================================================
// [PEG 2026-08-27] pegged_assets parser
//
// The registry's own tests build PeggedAsset directly, so none of them would
// notice the PARSER ignoring `enforce: false`, dropping
// `prefer_market_cross`, or accepting the wrong YAML shape.  These close
// that gap -- a peg silently mis-parsed is an asset everyone believes is
// monitored and valued correctly when it is neither.
// ============================================================================

namespace {

std::string with_pegs(const std::string& pegs) {
    return std::string(kMinimalValidYaml) + pegs;
}

}  // namespace

TEST(ConfigParserTest, PeggedAssets_AbsentSectionIsLegalAndEmpty) {
    TempYaml tmp(kMinimalValidYaml);
    auto cfg = xop::load_config(tmp.path());
    EXPECT_TRUE(cfg.pegged_assets.empty())
        << "no declaration means nothing is pegged -- not a default of $1";
}

TEST(ConfigParserTest, PeggedAssets_AllFieldsRoundTrip) {
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: aabb000000000000000000000000000000000000000000000000000000000000
  symbol: wTEST
  peg_currency: USD
  peg_target: 1.0
  warn_pct: 3.0
  bail_pct: 12.0
  sustained_observations: 7
  prefer_market_cross: false
  enforce: true
)"));
    auto cfg = xop::load_config(tmp.path());
    const auto* a = cfg.pegged_assets.find("aabb000000000000000000000000000000000000000000000000000000000000");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->symbol, "wTEST");
    EXPECT_EQ(a->peg_currency, "USD");
    EXPECT_DOUBLE_EQ(a->peg_target, 1.0);
    EXPECT_DOUBLE_EQ(a->warn_pct, 3.0);
    EXPECT_DOUBLE_EQ(a->bail_pct, 12.0);
    EXPECT_EQ(a->sustained_observations, 7u);
    EXPECT_FALSE(a->prefer_market_cross);
    EXPECT_TRUE(a->enforce);
}

TEST(ConfigParserTest, PeggedAssets_ThresholdsAreNotAdvertisedAsUnwired) {
    // The parser used to warn at startup that warn_pct / bail_pct /
    // sustained_observations were "NOT YET WIRED to a detector". The wiring
    // landed one day after that comment was written and the warning has been
    // false ever since -- it told operators to treat live suspension
    // thresholds as dead config.
    //
    // CAVEAT: CapturedLog is a ringbuffer holding the LAST 256 records. These
    // are negative assertions, which is the direction eviction breaks -- if
    // load_config ever emits more than 256 advisories the needle is evicted
    // and this passes for the wrong reason. The (A) block below is what stops
    // the test being vacuous today.
    CapturedLog log;
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: aabb000000000000000000000000000000000000000000000000000000000000
  symbol: wTEST
  peg_currency: USD
  peg_target: 1.0
  warn_pct: 3.0
  bail_pct: 12.0
  sustained_observations: 7
)"));
    auto cfg = xop::load_config(tmp.path());

    // (A) ANTI-VACUITY: prove the load actually reached the threshold keys.
    // Without this, a YAML that failed to parse or a silently skipped section
    // would satisfy (B) trivially.
    const auto* a = cfg.pegged_assets.find(
        "aabb000000000000000000000000000000000000000000000000000000000000");
    ASSERT_NE(a, nullptr);
    EXPECT_DOUBLE_EQ(a->warn_pct, 3.0);
    EXPECT_DOUBLE_EQ(a->bail_pct, 12.0);
    EXPECT_EQ(a->sustained_observations, 7u);

    // (B) THE PIN: two independent substrings from the two false sentences,
    // so restoring either half alone still fails.
    EXPECT_FALSE(log.warned_containing("NOT YET WIRED"))
        << "startup must not advertise live suspension thresholds as dead";
    EXPECT_FALSE(log.warned_containing("still comes only from pairs marked"))
        << "the asset-level watcher is a second watcher, not absent";
}

TEST(ConfigParserTest, PeggedAssets_EnforcedButUnobservableAssetWarns) {
    // [review #151] The partner of ThresholdsAreNotAdvertisedAsUnwired: that
    // test pins the FALSE blanket warning staying gone; this pins the TRUE
    // targeted one appearing. Deleting a wrong warning should not leave the
    // real case silent.
    //
    // kMinimalValidYaml's single pair is XCH/DBX, so an asset declared here
    // is crossed by no enabled pair and is genuinely unobservable.
    CapturedLog log;
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: aabb000000000000000000000000000000000000000000000000000000000000
  symbol: wLONELY
  peg_currency: USD
  peg_target: 1.0
  enforce: true
)"));
    auto cfg = xop::load_config(tmp.path());

    // Anti-vacuity: the declaration really did load and really is enforced.
    const auto* a = cfg.pegged_assets.find(
        "aabb000000000000000000000000000000000000000000000000000000000000");
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->enforce);

    EXPECT_TRUE(log.warned_containing("NO ENABLED pair crosses it against XCH"))
        << "an enforced peg nothing observes must not be silent";
    EXPECT_TRUE(log.warned_containing("wLONELY"))
        << "the warning must name the asset";
    // And the deleted blanket warning must STAY deleted.
    EXPECT_FALSE(log.warned_containing("NOT YET WIRED"));
}

TEST(ConfigParserTest, PeggedAssets_ObservedAssetIsSilent) {
    // [review #151] THE HALF THAT WAS UNPINNED. A mutation removing the
    // `observed` early-continue reddened NOTHING, because neither other test
    // declares an asset that IS observed -- so the guard rested on
    // inspection. This closes that.
    //
    // kMinimalValidYaml's single ENABLED pair is XCH/TEST, base xch, quote
    // 0123...cdef. Declaring THAT asset id means an enabled pair really does
    // cross it against XCH, so the warning must stay silent. The asset id
    // must match exactly: a typo would make this pass for the wrong reason
    // (nothing matched) rather than the right one (it is observed).
    CapturedLog log;
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  symbol: wSEEN
  peg_currency: USD
  peg_target: 1.0
  enforce: true
)"));
    auto cfg = xop::load_config(tmp.path());

    // Anti-vacuity: the asset loaded, is enforced, AND an enabled pair
    // really does cross it against XCH.
    const auto* a = cfg.pegged_assets.find(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->enforce);
    ASSERT_FALSE(cfg.pairs.empty());
    EXPECT_TRUE(cfg.pairs[0].enabled);
    EXPECT_EQ(cfg.pairs[0].base_asset_id, "xch");
    EXPECT_EQ(cfg.pairs[0].quote_asset_id, a->asset_id);

    EXPECT_FALSE(log.warned_containing("NO ENABLED pair crosses it against XCH"))
        << "this asset IS observed -- warning here would be a false alarm on "
           "a correctly configured deployment";
}

TEST(ConfigParserTest, PeggedAssets_UnenforcedAssetIsSilent) {
    // enforce:false is an explicit operator instruction meaning "do not
    // enforce this peg". Warning about it would re-create the noise the
    // blanket warning was deleted for.
    CapturedLog log;
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: dead000000000000000000000000000000000000000000000000000000000000
  symbol: wQUIET
  peg_currency: USD
  peg_target: 1.0
  enforce: false
)"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_NE(cfg.pegged_assets.find(
        "dead000000000000000000000000000000000000000000000000000000000000"),
        nullptr);
    EXPECT_FALSE(log.warned_containing("NO ENABLED pair crosses it against XCH"))
        << "enforce:false is deliberate, not a watch gap";
}

TEST(ConfigParserTest, PeggedAssets_AHalfDeclarationIsRefused) {
    // [review round 11] PeggedAsset defaults peg_currency to "USD" and
    // peg_target to 1.0, and the parser only overwrote PRESENT keys -- so an
    // entry carrying nothing but an asset id was accepted and silently
    // recreated the implicit $1 par this registry exists to remove. One
    // case per omitted key.
    {
        TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: aabb000000000000000000000000000000000000000000000000000000000000
  symbol: HALF
  peg_target: 1.0
)"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "omitted peg_currency defaulted to USD";
    }
    {
        TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: aabb000000000000000000000000000000000000000000000000000000000000
  symbol: HALF
  peg_currency: USD
)"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "omitted peg_target defaulted to 1.0";
    }
    {
        TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: aabb000000000000000000000000000000000000000000000000000000000000
  symbol: BARE
)"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
            << "a bare asset id recreated the full implicit par";
    }
}

TEST(ConfigParserTest, PeggedAssets_EnforceFalseSurvivesTheParser) {
    // The switch that did not exist when an issuer was compromised.  If the
    // parser dropped it, an operator would set it and nothing would change.
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: dead000000000000000000000000000000000000000000000000000000000000
  symbol: GONE
  peg_currency: USD
  peg_target: 1.0
  enforce: false
)"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_NE(cfg.pegged_assets.find("dead000000000000000000000000000000000000000000000000000000000000"), nullptr) << "declaration retained";
    EXPECT_FALSE(cfg.pegged_assets.is_pegged("dead000000000000000000000000000000000000000000000000000000000000"));
    EXPECT_FALSE(cfg.pegged_assets.usd_par_value("dead000000000000000000000000000000000000000000000000000000000000").has_value())
        << "an unenforced peg must not value anything";
}

TEST(ConfigParserTest, PeggedAssets_PreferMarketCrossSurvivesTheParser) {
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: cd90000000000000000000000000000000000000000000000000000000000000
  symbol: CDP
  peg_currency: USD
  peg_target: 1.0
  prefer_market_cross: true
)"));
    auto cfg = xop::load_config(tmp.path());
    const auto* a = cfg.pegged_assets.find("cd90000000000000000000000000000000000000000000000000000000000000");
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->prefer_market_cross)
        << "wrapper-vs-CDP is what selects the valuation path";
}

TEST(ConfigParserTest, PeggedAssets_NonUsdDeclarationParsesAndYieldsNoUsdValue) {
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: e040000000000000000000000000000000000000000000000000000000000000
  symbol: wEURC
  peg_currency: EUR
  peg_target: 1.0
)"));
    auto cfg = xop::load_config(tmp.path());
    const auto* a = cfg.pegged_assets.find("e040000000000000000000000000000000000000000000000000000000000000");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->peg_currency, "EUR");
    EXPECT_FALSE(cfg.pegged_assets.usd_par_value("e040000000000000000000000000000000000000000000000000000000000000").has_value())
        << "no FX rate supplied, so no USD value -- never a silent 1:1";
    EXPECT_TRUE(cfg.pegged_assets.usd_par_value("e040000000000000000000000000000000000000000000000000000000000000", 1.09).has_value());
}

TEST(ConfigParserTest, PeggedAssets_IncoherentEntryThrows) {
    // Dropped silently, this is an asset everyone assumes is watched.
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: bad0000000000000000000000000000000000000000000000000000000000000
  symbol: BAD
  peg_currency: USD
  peg_target: 1.0
  warn_pct: 10.0
  bail_pct: 2.0
)"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
        << "bail_pct must exceed warn_pct or the warning can never fire first";
}

TEST(ConfigParserTest, PeggedAssets_MissingAssetIdThrows) {
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- symbol: NOID
  peg_currency: USD
  peg_target: 1.0
)"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, PeggedAssets_NonFiniteTargetThrows) {
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: 1f00000000000000000000000000000000000000000000000000000000000000
  symbol: INF
  peg_currency: USD
  peg_target: .inf
)"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError)
        << "+inf satisfies `> 0` and would reach llround as an infinite factor";
}

TEST(ConfigParserTest, PeggedAssets_MalformedSectionThrowsRatherThanDisablingEveryPeg) {
    // A mapping instead of a sequence -- an indentation slip.  Treating it
    // like absence would silently zero all USD valuation on a typo.
    TempYaml tmp(with_pegs(R"(
pegged_assets:
  asset_id: oops
  symbol: OOPS
)"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, PeggedAssets_UppercaseAssetIdIsLowercased) {
    // Chia tools emit uppercase hex.  Without canonicalization the
    // declaration could never match a pair's lowercased asset id, so the
    // peg would silently do nothing while looking configured.
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899
  symbol: UPPER
  peg_currency: USD
  peg_target: 1.0
)"));
    auto cfg = xop::load_config(tmp.path());
    EXPECT_NE(cfg.pegged_assets.find(
        "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899"),
        nullptr);
}

TEST(ConfigParserTest, PeggedAssets_MalformedAssetIdThrows) {
    // A placeholder or typo must fail at startup, not key on a string no
    // asset can ever match.
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: REPLACE_WITH_REAL_TAIL
  symbol: PLACEHOLDER
  peg_currency: USD
  peg_target: 1.0
)"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, PeggedAssets_XchIsAValidAssetId) {
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: xch
  symbol: XCH
  peg_currency: USD
  peg_target: 1.0
)"));
    EXPECT_NO_THROW(xop::load_config(tmp.path()));
}

TEST(ConfigParserTest, PeggedAssets_DuplicateAssetIdThrows) {
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: aa11000000000000000000000000000000000000000000000000000000000000
  symbol: ONE
  peg_currency: USD
  peg_target: 1.0
- asset_id: aa11000000000000000000000000000000000000000000000000000000000000
  symbol: TWO
  peg_currency: USD
  peg_target: 1.0
)"));
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

TEST(ConfigParserTest, PeggedAssets_PegCurrencyIsCanonicalised) {
    // usd_par_value() matches "USD" exactly. The parser accepted any
    // non-empty string, so `peg_currency: usd` was accepted and then behaved
    // like an unsupported foreign currency -- silently removing the par the
    // operator believed they had declared.
    for (const char* written : {"usd", " USD ", "Usd", "\tuSd\n"}) {
        TempYaml tmp(with_pegs(std::string(R"(
pegged_assets:
- asset_id: "aa11bb22cc33dd44ee55ff6600112233445566778899aabbccddeeff00112233"
  symbol: TEST
  peg_currency: ")") + written + R"("
  peg_target: 1.0
  warn_pct: 2.0
  bail_pct: 10.0
)"));
        auto cfg = xop::load_config(tmp.path());
        const auto* a = cfg.pegged_assets.find(
            "aa11bb22cc33dd44ee55ff6600112233445566778899aabbccddeeff00112233");
        ASSERT_NE(a, nullptr) << "written as: " << written;
        EXPECT_EQ(a->peg_currency, "USD") << "written as: " << written;
        EXPECT_TRUE(cfg.pegged_assets.usd_par_value(
            "aa11bb22cc33dd44ee55ff6600112233445566778899aabbccddeeff00112233")
                        .has_value())
            << "a canonicalised USD peg must still yield a par: " << written;
    }
}


// [RELOAD] Duplicate pair names would make a live disable flip one entry,
// cancel the book, alert success, and leave the twin quoting. Rejected at
// parse time so neither boot nor reload can ever see them.
TEST(ConfigParserTest, DuplicatePairNames_Throw) {
    std::string yaml = kMinimalValidYaml;
    const std::string anchor = "    enabled: true\n";
    auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(),
        "  - base_asset_id: \"xch\"\n"
        "    quote_asset_id: \"fedcba9876543210fedcba9876543210"
        "fedcba9876543210fedcba9876543210\"\n"
        "    name: \"XCH/TEST\"\n"
        "    enabled: true\n");
    TempYaml tmp(yaml.c_str());
    EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
}

// ============================================================================
// [S14 2026-09-13] strategy.cancel_escalation_*
//
// Keys are spliced in with this file's with_strategy_keys() helper (above),
// which inserts directly after "  tier_size_pct: [0.6, 0.4]" and returns the
// YAML unchanged if that anchor ever moves -- hence the ASSERT_NE guards.
// ============================================================================

TEST(CancelEscalationConfig, DefaultsMatchTheHeaderConstants)
{
    TempYaml tmp(kMinimalValidYaml);
    const auto cfg = xop::load_config(tmp.path());
    EXPECT_TRUE(cfg.strategy.cancel_escalation_enabled);

    const auto params = xop::execution::cancel_escalation_params_from(cfg.strategy);
    const xop::execution::CancelEscalationParams header{};
    EXPECT_EQ(params.window_blocks, header.window_blocks);
    EXPECT_EQ(params.max_escalations, header.max_escalations);
    EXPECT_EQ(params.fee_step_mojos, header.fee_step_mojos);
    EXPECT_EQ(params.max_fee_mojos, header.max_fee_mojos);
    EXPECT_EQ(params.retry_blocks, header.retry_blocks);
    EXPECT_EQ(params.max_probes_per_sweep, header.max_probes_per_sweep);
}

TEST(CancelEscalationConfig, KeysParseIntoTheParams)
{
    const std::string yaml = with_strategy_keys(
        "\n  cancel_escalation_enabled: false"
        "\n  cancel_escalation_window_blocks: 120"
        "\n  cancel_escalation_max_attempts: 5"
        "\n  cancel_escalation_fee_step_mojos: 15000000"
        "\n  cancel_escalation_max_fee_mojos: 250000000"
        "\n  cancel_escalation_retry_blocks: 12"
        "\n  cancel_escalation_max_probes: 7");
    ASSERT_NE(yaml, std::string(kMinimalValidYaml)) << "the strategy anchor moved";
    TempYaml tmp(yaml.c_str());
    const auto cfg = xop::load_config(tmp.path());

    EXPECT_FALSE(cfg.strategy.cancel_escalation_enabled);
    const auto params = xop::execution::cancel_escalation_params_from(cfg.strategy);
    EXPECT_EQ(params.window_blocks, 120u);
    EXPECT_EQ(params.max_escalations, 5u);
    EXPECT_EQ(params.fee_step_mojos, 15'000'000u);
    EXPECT_EQ(params.max_fee_mojos, 250'000'000u);
    EXPECT_EQ(params.retry_blocks, 12u);
    EXPECT_EQ(params.max_probes_per_sweep, 7u);
}

TEST(CancelEscalationConfig, UnsafeValuesAreRejected)
{
    const char* const unsafe[] = {
        // Below chia's replacement increment: cannot replace a conflict.
        "\n  cancel_escalation_fee_step_mojos: 9999999",
        "\n  cancel_escalation_window_blocks: 0",
        "\n  cancel_escalation_retry_blocks: 0",
        "\n  cancel_escalation_window_blocks: 8\n  cancel_escalation_retry_blocks: 9",
        "\n  cancel_escalation_max_fee_mojos: 10000000",
        "\n  cancel_escalation_max_probes: 0",
    };
    for (const char* keys : unsafe) {
        const std::string yaml = with_strategy_keys(keys);
        ASSERT_NE(yaml, std::string(kMinimalValidYaml)) << "the strategy anchor moved";
        TempYaml tmp(yaml.c_str());
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError) << keys;
    }
}

// ============================================================================
// [PACE D1 2026-09-13] Per-pair concentration overrides
// (pairs[i].soft_limit_pct_override / pairs[i].hard_limit_pct_override)
// ============================================================================

namespace {

/// A second 64-hex CAT id, for configs that need two pairs.
const char* const kTest2 =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdee";

/// load_config must reject `yaml` with a ConfigError whose message contains
/// `needle`: an error raised for a different reason must not pass the test.
void expect_config_error_containing(const std::string& yaml, const std::string& needle)
{
    TempYaml tmp(yaml);
    try {
        const auto cfg = xop::load_config(tmp.path());
        static_cast<void>(cfg);
        ADD_FAILURE() << "loaded; expected a ConfigError containing \"" << needle << "\"";
    } catch (const xop::ConfigError& e) {
        EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
            << "ConfigError: " << e.what() << "\nexpected it to contain \"" << needle << "\"";
    }
}

/// load_config must accept `yaml`.
void expect_loads(const std::string& yaml)
{
    TempYaml tmp(yaml);
    EXPECT_NO_THROW({
        const auto cfg = xop::load_config(tmp.path());
        static_cast<void>(cfg);
    }) << yaml;
}

}  // namespace

TEST(PairConcentrationOverride, AbsentByDefault) {
    TempYaml tmp(kMinimalValidYaml);
    const auto cfg = xop::load_config(tmp.path());
    ASSERT_EQ(cfg.pairs.size(), 1u);
    EXPECT_FALSE(cfg.pairs[0].soft_limit_pct_override.has_value());
    EXPECT_FALSE(cfg.pairs[0].hard_limit_pct_override.has_value());
}

TEST(PairConcentrationOverride, ParsesBoth) {
    TempYaml tmp(with_pair_extra(
        "soft_limit_pct_override: 0.9\n    hard_limit_pct_override: 0.97"));
    const auto cfg = xop::load_config(tmp.path());
    ASSERT_EQ(cfg.pairs.size(), 1u);
    ASSERT_TRUE(cfg.pairs[0].soft_limit_pct_override.has_value());
    ASSERT_TRUE(cfg.pairs[0].hard_limit_pct_override.has_value());
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].soft_limit_pct_override, 0.9);
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].hard_limit_pct_override, 0.97);
}

// NonFiniteThrows and OutOfRangeThrows require the PARSE-stage message, not
// just the key.  The effective soft < hard error names both override keys too,
// and on its own it rejects a NaN override and a soft override above 1, so a
// key-only needle would pass with the parse check deleted.
TEST(PairConcentrationOverride, NonFiniteThrows) {
    for (const char* key : {"soft_limit_pct_override", "hard_limit_pct_override"}) {
        for (const char* value : {".nan", ".inf"}) {
            SCOPED_TRACE(std::string(key) + ": " + value);
            expect_config_error_containing(
                with_pair_extra(std::string(key) + ": " + value),
                std::string(key) + " must be a finite fraction in (0, 1]");
        }
    }
}

TEST(PairConcentrationOverride, OutOfRangeThrows) {
    for (const char* key : {"soft_limit_pct_override", "hard_limit_pct_override"}) {
        for (const char* value : {"0", "-0.1", "1.0001"}) {
            SCOPED_TRACE(std::string(key) + ": " + value);
            expect_config_error_containing(
                with_pair_extra(std::string(key) + ": " + value),
                std::string(key) + " must be a finite fraction in (0, 1]");
        }
    }
}

TEST(PairConcentrationOverride, EffectiveSoftNotBelowEffectiveHardThrows) {
    // Both present, inverted.
    expect_config_error_containing(
        with_pair_extra("soft_limit_pct_override: 0.97\n    hard_limit_pct_override: 0.90"),
        "effective soft limit");
    // Soft-only above the global hard (0.80).
    {
        TempYaml tmp(with_pair_extra("soft_limit_pct_override: 0.85"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    // Hard-only below the global soft (0.60).
    {
        TempYaml tmp(with_pair_extra("hard_limit_pct_override: 0.55"));
        EXPECT_THROW(xop::load_config(tmp.path()), xop::ConfigError);
    }
    // Soft-only below the global hard, and hard-only above the global soft.
    expect_loads(with_pair_extra("soft_limit_pct_override: 0.70"));
    expect_loads(with_pair_extra("hard_limit_pct_override: 0.97"));
}

TEST(PairConcentrationOverride, OtherPairsUnaffected) {
    std::string yaml = kMinimalValidYaml;
    const std::string anchor = "    enabled: true\n";
    const auto pos = yaml.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    yaml.insert(pos + anchor.size(),
                std::string("    soft_limit_pct_override: 0.9\n"
                            "    hard_limit_pct_override: 0.97\n"
                            "  - base_asset_id: \"xch\"\n"
                            "    quote_asset_id: \"") + kTest2 + "\"\n"
                "    name: \"XCH/TEST2\"\n"
                "    enabled: true\n");
    TempYaml tmp(yaml);
    const auto cfg = xop::load_config(tmp.path());
    ASSERT_EQ(cfg.pairs.size(), 2u);
    EXPECT_FALSE(cfg.pairs[1].soft_limit_pct_override.has_value());
    EXPECT_FALSE(cfg.pairs[1].hard_limit_pct_override.has_value());
    ASSERT_TRUE(cfg.pairs[0].soft_limit_pct_override.has_value());
    ASSERT_TRUE(cfg.pairs[0].hard_limit_pct_override.has_value());
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].soft_limit_pct_override, 0.9);
    EXPECT_DOUBLE_EQ(*cfg.pairs[0].hard_limit_pct_override, 0.97);
}

TEST(PairConcentrationOverride, OverrideLoggedAtLoad) {
    CapturedLog log;
    TempYaml tmp(with_pair_extra(
        "soft_limit_pct_override: 0.9\n    hard_limit_pct_override: 0.97"));
    EXPECT_NO_THROW({
        const auto cfg = xop::load_config(tmp.path());
        static_cast<void>(cfg);
    });
    EXPECT_TRUE(log.warned_containing("concentration limits overridden")) << log.text();
}

// ============================================================================
// [PACE 2026-09-13] Pace controller keys (strategy.pace_*)
// ============================================================================

namespace {

/// kMinimalValidYaml with `extra` (whole lines) spliced into the [strategy]
/// block, after tier_size_pct.
std::string pace_with_strategy(const std::string& extra)
{
    std::string y = kMinimalValidYaml;
    const std::string anchor = "  tier_size_pct: [0.6, 0.4]\n";
    const auto pos = y.find(anchor);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "strategy anchor not found";
        return y;
    }
    y.insert(pos + anchor.size(), extra);
    return y;
}

/// `yaml` with `extra` (whole lines) spliced in after the first pair's
/// `    enabled: true` line: more keys for that pair, or a second pair.
std::string pace_after_first_pair(std::string yaml, const std::string& extra)
{
    const std::string anchor = "    enabled: true\n";
    const auto pos = yaml.find(anchor);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "pair anchor not found";
        return yaml;
    }
    yaml.insert(pos + anchor.size(), extra);
    return yaml;
}

const std::string kPaceTargets =
    "  asset_target_allocations:\n    XCH: 0.5\n    TEST: 0.05\n"
    "  asset_target_tolerances:\n    XCH: 0.4\n    TEST: 0.02\n";

/// The topology checks iterate pace_assets, so a pair row without [TEST]
/// would load for the wrong reason.
const std::string kPaceOn = "  pace_enabled: true\n  pace_assets: [TEST]\n";

/// kMinimalValidYaml's quote asset id: the TEST symbol.
const std::string kTestId = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

}  // namespace

TEST(PaceConfig, DefaultsKeepFeatureOff) {
    TempYaml tmp(kMinimalValidYaml);
    const auto cfg = xop::load_config(tmp.path());
    const xop::StrategyConfig& strategy = cfg.strategy;
    EXPECT_FALSE(strategy.pace_enabled);
    EXPECT_TRUE(strategy.pace_assets.empty());
    EXPECT_EQ(strategy.pace_horizon_blocks, 64512u);
    EXPECT_DOUBLE_EQ(strategy.pace_enter_tol_mult, 1.5);
    EXPECT_DOUBLE_EQ(strategy.pace_exit_tol_mult, 1.0);
    EXPECT_DOUBLE_EQ(strategy.pace_max_resting_frac, 0.5);
    EXPECT_DOUBLE_EQ(strategy.pace_min_tier_units, 1.0);
    EXPECT_DOUBLE_EQ(strategy.pace_max_tier_units, 5.0);
    EXPECT_EQ(strategy.pace_max_tiers, 3u);
    EXPECT_DOUBLE_EQ(strategy.pace_tighten_step_bps, 25.0);
    EXPECT_DOUBLE_EQ(strategy.pace_tighten_max_bps, 300.0);
    EXPECT_DOUBLE_EQ(strategy.pace_min_edge_bps, 50.0);
    EXPECT_DOUBLE_EQ(strategy.pace_edge_sigma_mult, 1.0);
    EXPECT_DOUBLE_EQ(strategy.pace_max_fair_value_sigma_bps, 200.0);
    EXPECT_EQ(strategy.pace_max_balance_age_blocks, 20u);
    EXPECT_DOUBLE_EQ(strategy.pace_reprice_min_bps, 50.0);
    EXPECT_EQ(strategy.pace_reprice_min_age_blocks, 96u);
    ASSERT_EQ(cfg.pairs.size(), 1u);
    EXPECT_FALSE(cfg.pairs[0].soft_limit_pct_override.has_value());
    EXPECT_FALSE(cfg.pairs[0].hard_limit_pct_override.has_value());
}

TEST(PaceConfig, ParsesAllKeysAndUppercasesAssets) {
    TempYaml tmp(pace_with_strategy(
        "  pace_enabled: true\n"
        "  pace_assets: [test]\n" + kPaceTargets +
        "  pace_horizon_blocks: 32256\n"
        "  pace_enter_tol_mult: 2.0\n"
        "  pace_exit_tol_mult: 0.5\n"
        "  pace_max_resting_frac: 0.25\n"
        "  pace_min_tier_units: 1.5\n"
        "  pace_max_tier_units: 4.0\n"
        "  pace_max_tiers: 2\n"
        "  pace_tighten_step_bps: 10\n"
        "  pace_tighten_max_bps: 150\n"
        "  pace_min_edge_bps: 60\n"
        "  pace_edge_sigma_mult: 1.5\n"
        "  pace_max_fair_value_sigma_bps: 150\n"
        "  pace_max_balance_age_blocks: 30\n"
        "  pace_reprice_min_bps: 40\n"
        "  pace_reprice_min_age_blocks: 120\n"));
    const auto cfg = xop::load_config(tmp.path());
    const xop::StrategyConfig& strategy = cfg.strategy;
    EXPECT_TRUE(strategy.pace_enabled);
    EXPECT_EQ(strategy.pace_assets, std::vector<std::string>{"TEST"});
    EXPECT_EQ(strategy.pace_horizon_blocks, 32256u);
    EXPECT_DOUBLE_EQ(strategy.pace_enter_tol_mult, 2.0);
    EXPECT_DOUBLE_EQ(strategy.pace_exit_tol_mult, 0.5);
    EXPECT_DOUBLE_EQ(strategy.pace_max_resting_frac, 0.25);
    EXPECT_DOUBLE_EQ(strategy.pace_min_tier_units, 1.5);
    EXPECT_DOUBLE_EQ(strategy.pace_max_tier_units, 4.0);
    EXPECT_EQ(strategy.pace_max_tiers, 2u);
    EXPECT_DOUBLE_EQ(strategy.pace_tighten_step_bps, 10.0);
    EXPECT_DOUBLE_EQ(strategy.pace_tighten_max_bps, 150.0);
    EXPECT_DOUBLE_EQ(strategy.pace_min_edge_bps, 60.0);
    EXPECT_DOUBLE_EQ(strategy.pace_edge_sigma_mult, 1.5);
    EXPECT_DOUBLE_EQ(strategy.pace_max_fair_value_sigma_bps, 150.0);
    EXPECT_EQ(strategy.pace_max_balance_age_blocks, 30u);
    EXPECT_DOUBLE_EQ(strategy.pace_reprice_min_bps, 40.0);
    EXPECT_EQ(strategy.pace_reprice_min_age_blocks, 120u);
}

// ===========================================================================
// [S70-S72 2026-09-20] The three cancel-reduction switches, through the PARSER.
//
// Each switch changes what the bot CANCELS, so the properties that matter
// are: the default is the rule that existed before the key did; a mode is a
// closed vocabulary (a typo must not silently keep the old rule while the
// operator believes the new one is on); and `expire` cannot be selected with
// nothing to expire.
//
// (Placed mid-file on purpose: two open branches add their config tests at the
// top of the PaceConfig suite and at the end of this file, and these tests need
// the helpers defined above both.)
// ===========================================================================

TEST(CancelReductionConfig, DefaultsAreTheRulesThatExistedBeforeTheKeys) {
    TempYaml tmp(kMinimalValidYaml);
    const auto cfg = xop::load_config(tmp.path());
    const xop::StrategyConfig& s = cfg.strategy;
    EXPECT_EQ(s.ttl_cancel_mode, xop::TtlCancelMode::Cancel);
    EXPECT_EQ(s.exposure_rule, xop::ExposureRule::Legacy);
    EXPECT_EQ(s.price_cancel_mode, xop::PriceCancelMode::Deviation);
    EXPECT_DOUBLE_EQ(s.exposure_cancel_hysteresis_pct, 0.25);
    EXPECT_EQ(s.exposure_cancel_min_age_blocks, 32u);
    EXPECT_DOUBLE_EQ(s.price_cancel_edge_retain, 0.5);
    // A default-constructed StrategyConfig (what the tests that never load a
    // file get) agrees with the parser.
    const xop::StrategyConfig fresh{};
    EXPECT_EQ(fresh.ttl_cancel_mode, xop::TtlCancelMode::Cancel);
    EXPECT_EQ(fresh.exposure_rule, xop::ExposureRule::Legacy);
    EXPECT_EQ(fresh.price_cancel_mode, xop::PriceCancelMode::Deviation);
}

TEST(CancelReductionConfig, ParsesEveryKey) {
    TempYaml tmp(pace_with_strategy(
        "  offer_expiry_secs: 86400\n"
        "  ttl_cancel_mode: expire\n"
        "  exposure_rule: unified\n"
        "  exposure_cancel_hysteresis_pct: 0.4\n"
        "  exposure_cancel_min_age_blocks: 96\n"
        "  price_cancel_mode: margin\n"
        "  price_cancel_edge_retain: 0.75\n"));
    const auto cfg = xop::load_config(tmp.path());
    const xop::StrategyConfig& s = cfg.strategy;
    EXPECT_EQ(s.ttl_cancel_mode, xop::TtlCancelMode::Expire);
    EXPECT_EQ(s.exposure_rule, xop::ExposureRule::Unified);
    EXPECT_EQ(s.price_cancel_mode, xop::PriceCancelMode::Margin);
    EXPECT_DOUBLE_EQ(s.exposure_cancel_hysteresis_pct, 0.4);
    EXPECT_EQ(s.exposure_cancel_min_age_blocks, 96u);
    EXPECT_DOUBLE_EQ(s.price_cancel_edge_retain, 0.75);
}

TEST(CancelReductionConfig, TheRollbackValuesParseExplicitly) {
    // The rollback for each switch is to WRITE the old mode, not only to
    // delete the key -- so the old names must be accepted, not just implied.
    TempYaml tmp(pace_with_strategy(
        "  ttl_cancel_mode: cancel\n"
        "  exposure_rule: legacy\n"
        "  price_cancel_mode: deviation\n"));
    const auto cfg = xop::load_config(tmp.path());
    EXPECT_EQ(cfg.strategy.ttl_cancel_mode, xop::TtlCancelMode::Cancel);
    EXPECT_EQ(cfg.strategy.exposure_rule, xop::ExposureRule::Legacy);
    EXPECT_EQ(cfg.strategy.price_cancel_mode, xop::PriceCancelMode::Deviation);
}

TEST(CancelReductionConfig, NullKeepsTheDefault) {
    expect_loads(pace_with_strategy(
        "  ttl_cancel_mode: ~\n"
        "  exposure_rule: ~\n"
        "  price_cancel_mode: ~\n"
        "  exposure_cancel_hysteresis_pct: ~\n"
        "  exposure_cancel_min_age_blocks: ~\n"
        "  price_cancel_edge_retain: ~\n"));
}

TEST(CancelReductionConfig, AnUnknownModeThrowsRatherThanFallingBack) {
    struct Row {
        const char* key{nullptr};
        const char* value{nullptr};
    };
    const Row rows[] = {
        {"ttl_cancel_mode", "expired"},     {"ttl_cancel_mode", "Expire"},
        {"ttl_cancel_mode", "true"},        {"ttl_cancel_mode", "1"},
        {"ttl_cancel_mode", "[expire]"},
        {"exposure_rule", "unifed"},        {"exposure_rule", "UNIFIED"},
        {"exposure_rule", "{a: b}"},
        {"price_cancel_mode", "margins"},   {"price_cancel_mode", "edge"},
        {"price_cancel_mode", "[margin]"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(std::string(row.key) + ": " + row.value);
        expect_config_error_containing(
            pace_with_strategy("  offer_expiry_secs: 86400\n"
                               + std::string("  ") + row.key + ": " + row.value + "\n"),
            row.key);
    }
}

TEST(CancelReductionConfig, OutOfRangeAndNonFiniteThrow) {
    struct Row {
        const char* key{nullptr};
        const char* value{nullptr};
    };
    const Row rows[] = {
        {"exposure_cancel_hysteresis_pct", "-0.0001"},
        {"exposure_cancel_hysteresis_pct", "1.0001"},
        {"exposure_cancel_hysteresis_pct", ".nan"},
        {"exposure_cancel_hysteresis_pct", ".inf"},
        {"exposure_cancel_min_age_blocks", "-1"},
        {"exposure_cancel_min_age_blocks", "4609"},
        // (0, 1]: 0 would mean "never cancel for price".
        {"price_cancel_edge_retain", "0"},
        {"price_cancel_edge_retain", "-0.5"},
        {"price_cancel_edge_retain", "1.0001"},
        {"price_cancel_edge_retain", ".nan"},
        {"price_cancel_edge_retain", ".inf"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(std::string(row.key) + ": " + row.value);
        expect_config_error_containing(
            pace_with_strategy(std::string("  ") + row.key + ": " + row.value + "\n"),
            row.key);
    }
}

TEST(CancelReductionConfig, TheRangeEndsAreLegal) {
    // hysteresis 0 (cancel at the reserve) and 1 (suppress only) are both
    // documented operator levers; min age 0 spares nothing; retain 1.0 is the
    // literal rule.
    expect_loads(pace_with_strategy(
        "  exposure_cancel_hysteresis_pct: 0\n"
        "  exposure_cancel_min_age_blocks: 0\n"
        "  price_cancel_edge_retain: 1.0\n"));
    expect_loads(pace_with_strategy(
        "  exposure_cancel_hysteresis_pct: 1\n"
        "  exposure_cancel_min_age_blocks: 4608\n"));
}

TEST(CancelReductionConfig, ExpireWithNothingToExpireIsRefused) {
    // With no expiry on the strategy or any pair, `expire` spares no offer --
    // yet the config would read as "age cancels are off".
    expect_config_error_containing(
        pace_with_strategy("  ttl_cancel_mode: expire\n"), "ttl_cancel_mode");
    expect_config_error_containing(
        pace_with_strategy("  ttl_cancel_mode: expire\n  offer_expiry_secs: 0\n"),
        "offer_expiry_secs");
    // A pair that explicitly opts OUT does not count as an expiry either.
    expect_config_error_containing(
        pace_after_first_pair(pace_with_strategy("  ttl_cancel_mode: expire\n"),
                              "    offer_expiry_secs_override: 0\n"),
        "ttl_cancel_mode");
}

TEST(CancelReductionConfig, ExpireCountsEachPairsEffectiveExpiry) {
    // [review #164] A present 0 override BINDS (effective_offer_expiry_secs),
    // so a global expiry that the only pair opts out of attaches no timelock
    // to anything.  The first revision counted the global regardless.
    expect_config_error_containing(
        pace_after_first_pair(
            pace_with_strategy("  ttl_cancel_mode: expire\n  offer_expiry_secs: 86400\n"),
            "    offer_expiry_secs_override: 0\n"),
        "ttl_cancel_mode");
    // Two enabled pairs, one opted out, one inheriting the global: satisfied.
    expect_loads(pace_after_first_pair(
        pace_with_strategy("  ttl_cancel_mode: expire\n  offer_expiry_secs: 86400\n"),
        "    offer_expiry_secs_override: 0\n"
        "  - base_asset_id: \"xch\"\n"
        "    quote_asset_id: \"" + std::string(kTest2) + "\"\n"
        "    name: \"XCH/OTHER\"\n"
        "    enabled: true\n"));
}

TEST(CancelReductionConfig, ExpireIgnoresPairsThatPostNothing) {
    // The enabled pair opts out; the only pair with an effective expiry is
    // DISABLED, so no offer this engine posts carries one.
    expect_config_error_containing(
        pace_after_first_pair(
            pace_with_strategy("  ttl_cancel_mode: expire\n  offer_expiry_secs: 86400\n"),
            "    offer_expiry_secs_override: 0\n"
            "  - base_asset_id: \"xch\"\n"
            "    quote_asset_id: \"" + std::string(kTest2) + "\"\n"
            "    name: \"XCH/OTHER\"\n"
            "    enabled: false\n"),
        "ENABLED pair");
    // No enabled pair at all: nothing is posted, so there is nothing for the
    // mode to mislead about -- an operator who has parked every pair must
    // still be able to start the engine.
    std::string parked = pace_with_strategy("  ttl_cancel_mode: expire\n");
    const std::string on = "    enabled: true\n";
    const auto at = parked.find(on);
    ASSERT_NE(at, std::string::npos);
    parked.replace(at, on.size(), "    enabled: false\n");
    expect_loads(parked);
}

TEST(CancelReductionConfig, ExpireIsSatisfiedByTheGlobalOrByOnePair) {
    expect_loads(pace_with_strategy(
        "  ttl_cancel_mode: expire\n  offer_expiry_secs: 86400\n"));
    expect_loads(pace_after_first_pair(
        pace_with_strategy("  ttl_cancel_mode: expire\n"),
        "    offer_expiry_secs_override: 86400\n"));
    // The default mode asks for nothing.
    expect_loads(pace_with_strategy("  ttl_cancel_mode: cancel\n"));
}

TEST(CancelReductionConfig, ModeNamesRoundTripThroughToString) {
    // The startup summary and the log print these; the names are the ones
    // the parser accepts, so what is printed can be pasted back.
    EXPECT_STREQ(xop::to_string(xop::TtlCancelMode::Cancel), "cancel");
    EXPECT_STREQ(xop::to_string(xop::TtlCancelMode::Expire), "expire");
    EXPECT_STREQ(xop::to_string(xop::ExposureRule::Legacy), "legacy");
    EXPECT_STREQ(xop::to_string(xop::ExposureRule::Unified), "unified");
    EXPECT_STREQ(xop::to_string(xop::PriceCancelMode::Deviation), "deviation");
    EXPECT_STREQ(xop::to_string(xop::PriceCancelMode::Margin), "margin");
}

TEST(PaceConfig, NonFiniteDoubleKeysThrow) {
    // Every pace double key has a finite upper bound, so the .inf rows also
    // trip the range check; only the .nan rows need the finiteness test.
    const char* const keys[] = {
        "pace_enter_tol_mult", "pace_exit_tol_mult", "pace_max_resting_frac", "pace_min_tier_units",
        "pace_max_tier_units", "pace_tighten_step_bps", "pace_tighten_max_bps", "pace_min_edge_bps",
        "pace_edge_sigma_mult", "pace_max_fair_value_sigma_bps", "pace_reprice_min_bps",
    };
    for (const char* key : keys) {
        for (const char* value : {".nan", ".inf"}) {
            SCOPED_TRACE(std::string(key) + ": " + value);
            expect_config_error_containing(
                pace_with_strategy(std::string("  ") + key + ": " + value + "\n"), key);
        }
    }
}

TEST(PaceConfig, OutOfRangeThrows) {
    struct Row {
        const char* key{nullptr};
        const char* value{nullptr};
    };
    const Row rows[] = {
        {"pace_horizon_blocks", "4607"},           {"pace_horizon_blocks", "414721"},
        {"pace_enter_tol_mult", "0"},              {"pace_enter_tol_mult", "10.0001"},
        {"pace_exit_tol_mult", "-0.0001"},         {"pace_exit_tol_mult", "10"},
        {"pace_max_resting_frac", "0"},            {"pace_max_resting_frac", "1.0001"},
        {"pace_min_tier_units", "0"},
        {"pace_max_tier_units", "0"},              {"pace_max_tier_units", "1000000.1"},
        {"pace_max_tiers", "0"},                   {"pace_max_tiers", "17"},
        {"pace_tighten_step_bps", "0.9999"},       {"pace_tighten_step_bps", "1000.0001"},
        {"pace_tighten_max_bps", "-1"},            {"pace_tighten_max_bps", "5000.0001"},
        {"pace_min_edge_bps", "0"},                {"pace_min_edge_bps", "2000.0001"},
        {"pace_edge_sigma_mult", "-0.0001"},       {"pace_edge_sigma_mult", "5.0001"},
        {"pace_max_fair_value_sigma_bps", "0"},    {"pace_max_fair_value_sigma_bps", "2000.0001"},
        {"pace_max_balance_age_blocks", "0"},      {"pace_max_balance_age_blocks", "4609"},
        {"pace_reprice_min_bps", "0"},             {"pace_reprice_min_bps", "1000.0001"},
        {"pace_reprice_min_age_blocks", "11"},     {"pace_reprice_min_age_blocks", "4609"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(std::string(row.key) + ": " + row.value);
        expect_config_error_containing(
            pace_with_strategy(std::string("  ") + row.key + ": " + row.value + "\n"), row.key);
    }
}

TEST(PaceConfig, ExitNotBelowEnterThrows) {
    expect_config_error_containing(
        pace_with_strategy("  pace_enter_tol_mult: 1.5\n  pace_exit_tol_mult: 1.5\n"), "pace_exit_tol_mult");
}

TEST(PaceConfig, MinTierAboveMaxTierThrows) {
    expect_config_error_containing(
        pace_with_strategy("  pace_min_tier_units: 6\n  pace_max_tier_units: 5\n"), "pace_min_tier_units");
}

TEST(PaceConfig, XchInAssetsThrowsWhenEnabled) {
    expect_config_error_containing(
        pace_with_strategy("  pace_enabled: true\n  pace_assets: [XCH]\n" + kPaceTargets),
        "must not contain XCH");
}

TEST(PaceConfig, AssetWithoutTargetThrowsWhenEnabled) {
    expect_config_error_containing(pace_with_strategy(kPaceOn), "asset_target_allocations");
}

TEST(PaceConfig, ZeroToleranceThrowsWhenEnabled) {
    expect_config_error_containing(
        pace_with_strategy(kPaceOn
                           + "  asset_target_allocations:\n    XCH: 0.5\n    TEST: 0.05\n"
                             "  asset_target_tolerances:\n    XCH: 0.4\n    TEST: 0.0\n"),
        "asset_target_tolerances");
}

TEST(PaceConfig, SigmaCeilingAboveFairValueCeilingThrowsWhenEnabled) {
    // 150 is the lowest fair_value_max_sigma_bps the repo's tight-sigma check
    // accepts at the default fair_value_tight_sigma_bps of 150.
    expect_config_error_containing(
        pace_with_strategy(kPaceOn + kPaceTargets + "  fair_value_max_sigma_bps: 150\n"),
        "pace_max_fair_value_sigma_bps");
}

TEST(PaceConfig, ChecksSkippedWhenDisabled) {
    expect_loads(pace_with_strategy("  pace_assets: [XCH]\n  fair_value_max_sigma_bps: 150\n"));
}

TEST(PaceConfig, BaseManagedPairThrowsWhenEnabled) {
    expect_config_error_containing(
        pace_after_first_pair(pace_with_strategy(kPaceOn + kPaceTargets),
                              "  - base_asset_id: \"" + kTestId + "\"\n"
                              "    quote_asset_id: \"xch\"\n"
                              "    name: \"TEST/XCH\"\n"
                              "    enabled: true\n"),
        "must be XCH/TEST");
}

TEST(PaceConfig, NonXchPairTouchingAssetThrowsWhenEnabled) {
    expect_config_error_containing(
        pace_after_first_pair(pace_with_strategy(kPaceOn + kPaceTargets),
                              "  - base_asset_id: \"" + kTestId + "\"\n"
                              "    quote_asset_id: \"" + std::string(kTest2) + "\"\n"
                              "    name: \"TEST/OTHER\"\n"
                              "    enabled: true\n"),
        "must be XCH/TEST");
}

TEST(PaceConfig, DisabledPairTouchingAssetIgnored) {
    expect_loads(
        pace_after_first_pair(pace_with_strategy(kPaceOn + kPaceTargets),
                              "  - base_asset_id: \"" + kTestId + "\"\n"
                              "    quote_asset_id: \"" + std::string(kTest2) + "\"\n"
                              "    name: \"TEST/OTHER\"\n"
                              "    enabled: false\n"));
}

TEST(PaceConfig, StablecoinPacePairThrowsWhenEnabled) {
    // The same is_stablecoin / peg_target shape loads without pace
    // (ConfigParserTest.S20NonFinitePegTargetRejected).
    expect_config_error_containing(
        pace_after_first_pair(pace_with_strategy(kPaceOn + kPaceTargets),
                              "    is_stablecoin: true\n    peg_target: 1.0\n"),
        "must not set is_stablecoin");
}

TEST(PaceConfig, StaleFeedThresholdDisabledThrowsWhenEnabled) {
    std::string yaml = pace_with_strategy(kPaceOn + kPaceTargets);
    yaml += "\nmarket_data:\n  cex_freshness_threshold_sec: 0\n";
    expect_config_error_containing(yaml, "cex_freshness_threshold_sec");
}

TEST(PaceConfig, EnabledWithEmptyAssetsWarns) {
    CapturedLog log;
    expect_loads(pace_with_strategy("  pace_enabled: true\n"));
    EXPECT_TRUE(log.warned_containing("pace_assets is empty")) << log.text();
}
