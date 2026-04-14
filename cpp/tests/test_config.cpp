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

}  // namespace
