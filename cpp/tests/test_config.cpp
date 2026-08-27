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

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
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
- asset_id: aabb
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
    const auto* a = cfg.pegged_assets.find("aabb");
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

TEST(ConfigParserTest, PeggedAssets_EnforceFalseSurvivesTheParser) {
    // The switch that did not exist when an issuer was compromised.  If the
    // parser dropped it, an operator would set it and nothing would change.
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: dead
  symbol: GONE
  peg_currency: USD
  peg_target: 1.0
  enforce: false
)"));
    auto cfg = xop::load_config(tmp.path());
    ASSERT_NE(cfg.pegged_assets.find("dead"), nullptr) << "declaration retained";
    EXPECT_FALSE(cfg.pegged_assets.is_pegged("dead"));
    EXPECT_FALSE(cfg.pegged_assets.usd_par_value("dead").has_value())
        << "an unenforced peg must not value anything";
}

TEST(ConfigParserTest, PeggedAssets_PreferMarketCrossSurvivesTheParser) {
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: cdp
  symbol: CDP
  peg_currency: USD
  peg_target: 1.0
  prefer_market_cross: true
)"));
    auto cfg = xop::load_config(tmp.path());
    const auto* a = cfg.pegged_assets.find("cdp");
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->prefer_market_cross)
        << "wrapper-vs-CDP is what selects the valuation path";
}

TEST(ConfigParserTest, PeggedAssets_NonUsdDeclarationParsesAndYieldsNoUsdValue) {
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: euro
  symbol: wEURC
  peg_currency: EUR
  peg_target: 1.0
)"));
    auto cfg = xop::load_config(tmp.path());
    const auto* a = cfg.pegged_assets.find("euro");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->peg_currency, "EUR");
    EXPECT_FALSE(cfg.pegged_assets.usd_par_value("euro").has_value())
        << "no FX rate supplied, so no USD value -- never a silent 1:1";
    EXPECT_TRUE(cfg.pegged_assets.usd_par_value("euro", 1.09).has_value());
}

TEST(ConfigParserTest, PeggedAssets_IncoherentEntryThrows) {
    // Dropped silently, this is an asset everyone assumes is watched.
    TempYaml tmp(with_pegs(R"(
pegged_assets:
- asset_id: bad
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
- asset_id: inf
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
