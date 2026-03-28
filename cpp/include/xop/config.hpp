// config.hpp -- Configuration data structures for XOPTrader CHIA DEX market-maker.
//
// All configuration is loaded from a YAML file via yaml-cpp. Structures are
// plain-data aggregates with no heap indirection, facilitating value semantics
// and straightforward serialisation. Field names mirror the YAML schema
// defined in config.example.yaml so that the mapping is unsurprising.
//
// Security: SSL certificate paths, wallet fingerprints, and Telegram tokens
//           are classified as secrets and are excluded from any log output.
//
// ISO/IEC 27001:2022 -- secrets handling, least-privilege logging.
// ISO/IEC 5055       -- no raw pointers, bounds-checked containers.
// ISO/IEC 25000      -- clear naming, complete documentation.

#ifndef XOP_CONFIG_HPP
#define XOP_CONFIG_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace xop {

// ---------------------------------------------------------------------------
// Exception thrown when a configuration file is missing, malformed, or
// contains values outside their valid domain.
// ---------------------------------------------------------------------------
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Chia blockchain RPC connectivity and authentication.
// Covers both the full-node and wallet daemon endpoints.
// ---------------------------------------------------------------------------
struct ChiaConfig {
    std::string full_node_host;         // Full-node RPC hostname.
    uint16_t    full_node_port{8555};   // Full-node RPC port (default 8555).
    std::string wallet_host;            // Wallet RPC hostname.
    uint16_t    wallet_port{9256};      // Wallet RPC port (default 9256).

    // SSL mutual-auth certificate/key for the full-node endpoint.
    std::string ssl_cert_path;          // SECRET -- never log.
    std::string ssl_key_path;           // SECRET -- never log.

    // SSL mutual-auth certificate/key for the wallet endpoint.
    std::string wallet_cert_path;       // SECRET -- never log.
    std::string wallet_key_path;        // SECRET -- never log.

    // Wallet fingerprint that identifies the key to use.
    uint32_t    wallet_fingerprint{0};  // SECRET -- never log.
};

// ---------------------------------------------------------------------------
// Dexie aggregator API settings.
// Rate limiting is enforced per the published ceiling (50 req / 10 s).
// ---------------------------------------------------------------------------
struct DexieConfig {
    std::string api_base{"https://api.dexie.space/v1"};
    uint32_t    max_requests_per_10s{50};  // Must be >= 1.
};

// ---------------------------------------------------------------------------
// A single trading pair the bot may market-make.
// asset IDs are 64-character lower-hex strings except for native XCH which
// uses the literal "xch".
// ---------------------------------------------------------------------------
struct PairConfig {
    std::string base_asset_id;   // e.g. "xch".
    std::string quote_asset_id;  // e.g. 64-hex CAT asset ID.
    std::string name;            // Human-readable label, e.g. "XCH/wUSDC".
    bool        enabled{true};   // Inactive pairs are loaded but skipped.

    /// Mojos-per-displayable-unit for the base asset.
    /// XCH: 10^12 (1 XCH = 1 000 000 000 000 mojos).
    /// CAT: 10^3  (1 CAT unit = 1 000 mojos).
    /// ISO/IEC 5055: explicit denomination prevents silent truncation when
    /// a CAT amount is divided by the XCH constant (off by 10^9).
    std::int64_t base_mojos_per_unit{1'000'000'000'000LL};

    /// Mojos-per-displayable-unit for the quote asset.
    /// Same convention as base_mojos_per_unit.
    std::int64_t quote_mojos_per_unit{1'000LL};
};

// ---------------------------------------------------------------------------
// Core Avellaneda-Stoikov / GLFT market-making algorithm parameters.
//
// gamma  -- risk aversion coefficient (controls spread width).
// kappa  -- fill intensity decay (higher = less impact of spread on fills).
// phi    -- GLFT inventory skew strength (skew = phi * q / q_max).
// q_max  -- maximum tolerated inventory in base-asset units.
//
// Multi-tier offer ladder:
//   num_tiers         -- how many price levels per side.
//   tier_spacing_bps  -- spread from mid-price for each tier, in basis points.
//   tier_size_pct     -- fraction of allocated capital placed at each tier.
//                        Values must sum to approximately 1.0.
// ---------------------------------------------------------------------------
struct StrategyConfig {
    double   gamma{0.01};
    double   kappa{1.5};
    double   phi{0.5};
    double   q_max{1000.0};
    double   min_profit_margin_bps{35.0};   // Never ask below cost + this.
    uint32_t offer_ttl_blocks{60};          // Cancel stale offers after N blocks.
    uint32_t num_tiers{4};                  // Tier count per side.
    std::vector<double> tier_spacing_bps;   // Length == num_tiers.
    std::vector<double> tier_size_pct;      // Length == num_tiers, sum ~= 1.0.

    /// Global cap on half-spread (bps) after all compounding multipliers.
    /// Prevents the multiplicative chain (regime * whale * VPIN * OFI * tactic)
    /// from producing effective market withdrawal.
    /// Default 250 bps half-spread = 500 bps round-trip = 5% total.
    double   max_half_spread_bps{250.0};

    /// On-chain fee per offer/cancel (mojos).  Default 0.0001 XCH.
    std::uint64_t offer_fee_mojos{100'000'000ULL};
};

// ---------------------------------------------------------------------------
// Risk / inventory management thresholds.
//
// Percentages are expressed as fractions in [0, 1].
//   soft_limit_pct          -- begin aggressive quote skewing.
//   hard_limit_pct          -- pull quotes on overweight side.
//   single_cat_cap_pct      -- max portfolio fraction in any one CAT.
//   kelly_fraction          -- fraction of full Kelly to use (Half-Kelly = 0.5).
//   max_capital_per_pair_pct-- upper bound on capital allocated to one pair.
//
// Circuit breakers (ISO/IEC 27001:2022 §8.20 -- continuous risk monitoring):
//   max_drawdown_pct     -- peak-to-trough drawdown fraction that pauses the
//                           engine.  Default 10% (0.10).  Measures the drop
//                           from the all-time PnL high-water mark.
//   loss_window_blocks   -- rolling window size in blocks for the time-window
//                           loss circuit breaker.  Default 1152 blocks ≈ 10 h
//                           at the Chia mean block time of 52 s.
//   max_window_loss_bps  -- maximum loss (in basis points, i.e. 0.01 % per bp)
//                           permitted within the rolling window before the
//                           engine is paused.  Default 500 bps = 5 %.
//                           A value of 0 disables the window circuit breaker.
// ---------------------------------------------------------------------------
struct RiskConfig {
    double   soft_limit_pct{0.60};
    double   hard_limit_pct{0.80};
    double   single_cat_cap_pct{0.12};
    double   kelly_fraction{0.50};
    double   max_capital_per_pair_pct{0.20};

    // -- Circuit breakers ---------------------------------------------------
    double   max_drawdown_pct{0.10};        ///< HWM drawdown threshold (0,1].
    uint32_t loss_window_blocks{1152};      ///< Rolling window size in blocks.
    double   max_window_loss_bps{500.0};    ///< Max loss in window (bps; 0=disabled).
};

// ---------------------------------------------------------------------------
// Yang-Zhang hybrid volatility estimator settings.
//
// lookback_blocks -- rolling window length in blocks (~52 s each).
// yz_alpha        -- blending weight for the YZ estimator (0, 1).
// ---------------------------------------------------------------------------
struct VolatilityConfig {
    uint32_t lookback_blocks{200};
    double   yz_alpha{0.34};
};

// ---------------------------------------------------------------------------
// Observability: Prometheus metrics exporter and Telegram alert bot.
//
// telegram_bot_token and telegram_chat_id are SECRET and must not be logged.
// ---------------------------------------------------------------------------
struct MonitoringConfig {
    uint16_t    prometheus_port{9090};
    std::string telegram_bot_token;   // SECRET -- never log.
    std::string telegram_chat_id;     // SECRET -- never log.
};

// ---------------------------------------------------------------------------
// Persistent storage path (SQLite in Phase 1, PostgreSQL URI later).
// ---------------------------------------------------------------------------
struct DatabaseConfig {
    std::string path{"data/xop_trader.db"};
};

// ---------------------------------------------------------------------------
// Top-level application configuration aggregating every section.
// ---------------------------------------------------------------------------
struct AppConfig {
    ChiaConfig       chia;
    DexieConfig      dexie;
    std::vector<PairConfig> pairs;
    StrategyConfig   strategy;
    RiskConfig       risk;
    VolatilityConfig volatility;
    MonitoringConfig monitoring;
    DatabaseConfig   database;
};

// ---------------------------------------------------------------------------
// Load and fully validate a YAML configuration file, returning a populated
// AppConfig.  Throws xop::ConfigError on any structural or domain error.
//
// Tilde (~) prefixes in filesystem paths are expanded to the user's HOME.
// ---------------------------------------------------------------------------
AppConfig load_config(const std::string& path);

} // namespace xop

#endif // XOP_CONFIG_HPP
