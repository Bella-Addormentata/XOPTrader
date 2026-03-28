// config.cpp -- YAML configuration loader for XOPTrader.
//
// Parses config.yaml via yaml-cpp, maps every node into the plain-data
// structs declared in <xop/config.hpp>, and validates all value domains
// before returning an AppConfig to the caller.
//
// Security policy (ISO/IEC 27001:2022):
//   * SSL certificate paths   -- NEVER logged.
//   * Wallet fingerprint       -- NEVER logged.
//   * Telegram bot token       -- NEVER logged.
//   * Telegram chat ID         -- NEVER logged.
//   Logged summary includes only non-secret operational parameters.
//
// ISO/IEC 5055  -- no raw pointers, deterministic resource handling.
// ISO/IEC 25000 -- clear error messages citing the offending field.

#include "xop/config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>

namespace xop {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Expand a leading '~' to the value of HOME (or USERPROFILE on Windows).
// Returns the input unchanged when it does not start with '~'.
std::string expand_tilde(const std::string& path)
{
    if (path.empty() || path[0] != '~') {
        return path;
    }

    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) {
        home = std::getenv("USERPROFILE");
    }
#endif
    if (!home) {
        throw ConfigError("Cannot expand '~': HOME environment variable is not set");
    }

    // Replace the leading '~' with the home directory.
    return std::string(home) + path.substr(1);
}

// Require that a YAML node exists and is a scalar; throw ConfigError otherwise.
void require_scalar(const YAML::Node& parent,
                    const std::string& key,
                    const std::string& section)
{
    if (!parent[key] || !parent[key].IsDefined()) {
        throw ConfigError(section + "." + key + " is required but missing");
    }
    if (parent[key].IsNull()) {
        throw ConfigError(section + "." + key + " must not be null");
    }
}

// Require that a YAML node is a non-empty sequence.
void require_sequence(const YAML::Node& parent,
                      const std::string& key,
                      const std::string& section)
{
    if (!parent[key] || !parent[key].IsSequence()) {
        throw ConfigError(section + "." + key + " must be a non-empty sequence");
    }
    if (parent[key].size() == 0) {
        throw ConfigError(section + "." + key + " must contain at least one element");
    }
}

// Read a required string field, trim whitespace from both ends.
std::string read_string(const YAML::Node& parent,
                        const std::string& key,
                        const std::string& section)
{
    require_scalar(parent, key, section);
    std::string value = parent[key].as<std::string>();
    // Trim leading and trailing whitespace.
    auto start = value.find_first_not_of(" \t\r\n");
    auto end   = value.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) {
        throw ConfigError(section + "." + key + " must not be empty or whitespace-only");
    }
    return value.substr(start, end - start + 1);
}

// Read an optional string field; return fallback when absent.
std::string read_string_opt(const YAML::Node& parent,
                            const std::string& key,
                            const std::string& fallback)
{
    if (!parent[key] || parent[key].IsNull()) {
        return fallback;
    }
    return parent[key].as<std::string>();
}

// Read a required unsigned 16-bit port in [1, 65535].
uint16_t read_port(const YAML::Node& parent,
                   const std::string& key,
                   const std::string& section)
{
    require_scalar(parent, key, section);
    int value = parent[key].as<int>();
    if (value < 1 || value > 65535) {
        throw ConfigError(section + "." + key + " must be in [1, 65535]; got "
                          + std::to_string(value));
    }
    return static_cast<uint16_t>(value);
}

// Read a required unsigned 32-bit integer >= 1.
// ISO/IEC 5055 -- CWE-681: parse as int64_t to prevent truncation of values
// exceeding INT_MAX when targeting uint32_t.
uint32_t read_uint32_positive(const YAML::Node& parent,
                              const std::string& key,
                              const std::string& section)
{
    require_scalar(parent, key, section);
    int64_t value = parent[key].as<int64_t>();
    if (value < 1 || value > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
        throw ConfigError(section + "." + key + " must be in [1, "
                          + std::to_string(std::numeric_limits<uint32_t>::max())
                          + "]; got " + std::to_string(value));
    }
    return static_cast<uint32_t>(value);
}

// Read a required unsigned 32-bit integer >= 0.
// ISO/IEC 5055 -- CWE-681: parse as int64_t to prevent truncation of values
// exceeding INT_MAX when targeting uint32_t.
uint32_t read_uint32(const YAML::Node& parent,
                     const std::string& key,
                     const std::string& section)
{
    require_scalar(parent, key, section);
    int64_t value = parent[key].as<int64_t>();
    if (value < 0 || value > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
        throw ConfigError(section + "." + key + " must be in [0, "
                          + std::to_string(std::numeric_limits<uint32_t>::max())
                          + "]; got " + std::to_string(value));
    }
    return static_cast<uint32_t>(value);
}

// Read a required double that must be strictly positive (> 0).
double read_positive_double(const YAML::Node& parent,
                            const std::string& key,
                            const std::string& section)
{
    require_scalar(parent, key, section);
    double value = parent[key].as<double>();
    if (!(value > 0.0)) {  // Also catches NaN.
        throw ConfigError(section + "." + key + " must be > 0; got "
                          + std::to_string(value));
    }
    return value;
}

// Read a required double clamped to (0, 1].
double read_fraction(const YAML::Node& parent,
                     const std::string& key,
                     const std::string& section)
{
    require_scalar(parent, key, section);
    double value = parent[key].as<double>();
    if (!(value > 0.0 && value <= 1.0)) {
        throw ConfigError(section + "." + key + " must be in (0, 1]; got "
                          + std::to_string(value));
    }
    return value;
}

// Read a required double clamped to the open interval (0, 1).
double read_fraction_open(const YAML::Node& parent,
                          const std::string& key,
                          const std::string& section)
{
    require_scalar(parent, key, section);
    double value = parent[key].as<double>();
    if (!(value > 0.0 && value < 1.0)) {
        throw ConfigError(section + "." + key + " must be in (0, 1); got "
                          + std::to_string(value));
    }
    return value;
}

// Read a sequence of doubles from a YAML node; each element must be > 0.
std::vector<double> read_positive_double_seq(const YAML::Node& parent,
                                             const std::string& key,
                                             const std::string& section)
{
    require_sequence(parent, key, section);
    std::vector<double> result;
    result.reserve(parent[key].size());
    for (std::size_t i = 0; i < parent[key].size(); ++i) {
        double v = parent[key][i].as<double>();
        if (!(v > 0.0)) {
            throw ConfigError(section + "." + key + "[" + std::to_string(i)
                              + "] must be > 0; got " + std::to_string(v));
        }
        result.push_back(v);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Section parsers
// ---------------------------------------------------------------------------

ChiaConfig parse_chia(const YAML::Node& root)
{
    const std::string sec = "chia";
    if (!root[sec] || !root[sec].IsMap()) {
        throw ConfigError("'" + sec + "' section is required and must be a map");
    }
    const YAML::Node& node = root[sec];

    ChiaConfig cfg;
    cfg.full_node_host   = read_string(node, "full_node_host", sec);
    cfg.full_node_port   = read_port(node, "full_node_port", sec);
    cfg.wallet_host      = read_string(node, "wallet_host", sec);
    cfg.wallet_port      = read_port(node, "wallet_port", sec);

    // SSL paths are expanded and stored; they are never logged.
    cfg.ssl_cert_path    = expand_tilde(read_string(node, "ssl_cert_path", sec));
    cfg.ssl_key_path     = expand_tilde(read_string(node, "ssl_key_path", sec));
    cfg.wallet_cert_path = expand_tilde(read_string(node, "wallet_cert_path", sec));
    cfg.wallet_key_path  = expand_tilde(read_string(node, "wallet_key_path", sec));

    cfg.wallet_fingerprint = read_uint32(node, "wallet_fingerprint", sec);

    // T3-32: Reject sentinel value 0 for wallet_fingerprint.
    // A zero fingerprint means the wallet key is unconfigured; the daemon
    // would silently fall back to the first available key, violating the
    // principle of explicit configuration.
    // ISO/IEC 27001:2022 -- mandatory authentication parameter must be set.
    if (cfg.wallet_fingerprint == 0) {
        throw ConfigError(sec + ".wallet_fingerprint must not be 0; "
                          "configure the fingerprint of the wallet key to use");
    }

    return cfg;
}

DexieConfig parse_dexie(const YAML::Node& root)
{
    const std::string sec = "dexie";
    if (!root[sec] || !root[sec].IsMap()) {
        throw ConfigError("'" + sec + "' section is required and must be a map");
    }
    const YAML::Node& node = root[sec];

    DexieConfig cfg;
    cfg.api_base              = read_string(node, "api_base", sec);
    cfg.max_requests_per_10s  = read_uint32_positive(node, "max_requests_per_10s", sec);

    return cfg;
}

std::vector<PairConfig> parse_pairs(const YAML::Node& root)
{
    const std::string sec = "pairs";
    if (!root[sec] || !root[sec].IsSequence()) {
        throw ConfigError("'" + sec + "' must be a non-empty sequence of pair definitions");
    }
    if (root[sec].size() == 0) {
        throw ConfigError("'" + sec + "' must contain at least one trading pair");
    }

    std::vector<PairConfig> pairs;
    pairs.reserve(root[sec].size());

    for (std::size_t i = 0; i < root[sec].size(); ++i) {
        const YAML::Node& item = root[sec][i];
        const std::string idx  = sec + "[" + std::to_string(i) + "]";

        PairConfig p;
        p.base_asset_id  = read_string(item, "base_asset_id", idx);
        p.quote_asset_id = read_string(item, "quote_asset_id", idx);
        p.name           = read_string(item, "name", idx);

        // T3-29: Normalize asset IDs to lowercase before validation.
        // Chia tools may emit uppercase hex (e.g. "A1B2..."); coerce to
        // lowercase so all downstream code can rely on consistent casing.
        // ISO/IEC 25000 -- predictable data normalization at the boundary.
        auto to_lower = [](std::string& s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        };
        to_lower(p.base_asset_id);
        to_lower(p.quote_asset_id);

        // 'enabled' defaults to true when omitted.
        if (item["enabled"] && item["enabled"].IsDefined() && !item["enabled"].IsNull()) {
            p.enabled = item["enabled"].as<bool>();
        } else {
            p.enabled = true;
        }

        // Validate asset ID format: must be "xch" or a 64-character hex string.
        auto validate_asset_id = [&](const std::string& id, const std::string& field) {
            if (id == "xch") {
                return; // Native XCH -- valid.
            }
            if (id.size() != 64) {
                throw ConfigError(idx + "." + field
                                  + " must be 'xch' or a 64-character hex string; got length "
                                  + std::to_string(id.size()));
            }
            for (char c : id) {
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                    throw ConfigError(idx + "." + field
                                      + " contains invalid hex character '" + c + "'");
                }
            }
        };
        validate_asset_id(p.base_asset_id, "base_asset_id");
        validate_asset_id(p.quote_asset_id, "quote_asset_id");

        // -- Per-pair strategy overrides (all optional) ---------------------
        if (item["gamma_override"] && item["gamma_override"].IsDefined()
            && !item["gamma_override"].IsNull()) {
            double v = item["gamma_override"].as<double>();
            if (!(v > 0.0)) {
                throw ConfigError(idx + ".gamma_override must be > 0; got "
                                  + std::to_string(v));
            }
            p.gamma_override = v;
        }
        if (item["kappa_override"] && item["kappa_override"].IsDefined()
            && !item["kappa_override"].IsNull()) {
            double v = item["kappa_override"].as<double>();
            if (!(v > 0.0)) {
                throw ConfigError(idx + ".kappa_override must be > 0; got "
                                  + std::to_string(v));
            }
            p.kappa_override = v;
        }
        if (item["phi_override"] && item["phi_override"].IsDefined()
            && !item["phi_override"].IsNull()) {
            double v = item["phi_override"].as<double>();
            if (!(v > 0.0)) {
                throw ConfigError(idx + ".phi_override must be > 0; got "
                                  + std::to_string(v));
            }
            p.phi_override = v;
        }
        if (item["q_max_override"] && item["q_max_override"].IsDefined()
            && !item["q_max_override"].IsNull()) {
            double v = item["q_max_override"].as<double>();
            if (!(v > 0.0)) {
                throw ConfigError(idx + ".q_max_override must be > 0; got "
                                  + std::to_string(v));
            }
            p.q_max_override = v;
        }
        if (item["min_profit_margin_bps_override"]
            && item["min_profit_margin_bps_override"].IsDefined()
            && !item["min_profit_margin_bps_override"].IsNull()) {
            double v = item["min_profit_margin_bps_override"].as<double>();
            if (!(v > 0.0)) {
                throw ConfigError(idx + ".min_profit_margin_bps_override must be > 0; got "
                                  + std::to_string(v));
            }
            p.min_profit_margin_bps_override = v;
        }
        if (item["tier_spacing_bps_override"]
            && item["tier_spacing_bps_override"].IsSequence()
            && item["tier_spacing_bps_override"].size() > 0) {
            std::vector<double> ts;
            ts.reserve(item["tier_spacing_bps_override"].size());
            for (std::size_t j = 0; j < item["tier_spacing_bps_override"].size(); ++j) {
                double v = item["tier_spacing_bps_override"][j].as<double>();
                if (!(v > 0.0)) {
                    throw ConfigError(idx + ".tier_spacing_bps_override[" + std::to_string(j)
                                      + "] must be > 0; got " + std::to_string(v));
                }
                ts.push_back(v);
            }
            p.tier_spacing_bps_override = std::move(ts);
        }
        if (item["tier_size_pct_override"]
            && item["tier_size_pct_override"].IsSequence()
            && item["tier_size_pct_override"].size() > 0) {
            std::vector<double> tp;
            tp.reserve(item["tier_size_pct_override"].size());
            for (std::size_t j = 0; j < item["tier_size_pct_override"].size(); ++j) {
                double v = item["tier_size_pct_override"][j].as<double>();
                if (!(v > 0.0 && v <= 1.0)) {
                    throw ConfigError(idx + ".tier_size_pct_override[" + std::to_string(j)
                                      + "] must be in (0, 1]; got " + std::to_string(v));
                }
                tp.push_back(v);
            }
            double tp_sum = std::accumulate(tp.begin(), tp.end(), 0.0);
            if (std::fabs(tp_sum - 1.0) > 0.01) {
                throw ConfigError(idx + ".tier_size_pct_override must sum to ~1.0; got "
                                  + std::to_string(tp_sum));
            }
            p.tier_size_pct_override = std::move(tp);
        }

        // -- Stablecoin peg configuration (optional) ------------------------
        if (item["is_stablecoin"] && item["is_stablecoin"].IsDefined()
            && !item["is_stablecoin"].IsNull()) {
            p.is_stablecoin = item["is_stablecoin"].as<bool>();
        }
        if (item["peg_target"] && item["peg_target"].IsDefined()
            && !item["peg_target"].IsNull()) {
            double v = item["peg_target"].as<double>();
            if (!(v > 0.0)) {
                throw ConfigError(idx + ".peg_target must be > 0; got "
                                  + std::to_string(v));
            }
            p.peg_target = v;
        }
        if (item["depeg_warn_pct"] && item["depeg_warn_pct"].IsDefined()
            && !item["depeg_warn_pct"].IsNull()) {
            double v = item["depeg_warn_pct"].as<double>();
            if (!(v > 0.0)) {
                throw ConfigError(idx + ".depeg_warn_pct must be > 0; got "
                                  + std::to_string(v));
            }
            p.depeg_warn_pct = v;
        }
        if (item["depeg_bail_pct"] && item["depeg_bail_pct"].IsDefined()
            && !item["depeg_bail_pct"].IsNull()) {
            double v = item["depeg_bail_pct"].as<double>();
            if (!(v > 0.0)) {
                throw ConfigError(idx + ".depeg_bail_pct must be > 0; got "
                                  + std::to_string(v));
            }
            p.depeg_bail_pct = v;
        }
        if (item["depeg_sustained_blocks"] && item["depeg_sustained_blocks"].IsDefined()
            && !item["depeg_sustained_blocks"].IsNull()) {
            p.depeg_sustained_blocks = item["depeg_sustained_blocks"].as<uint32_t>();
        }

        // Validate: depeg_bail_pct must exceed depeg_warn_pct.
        if (p.is_stablecoin && p.depeg_bail_pct <= p.depeg_warn_pct) {
            throw ConfigError(idx + ".depeg_bail_pct ("
                              + std::to_string(p.depeg_bail_pct)
                              + ") must be greater than depeg_warn_pct ("
                              + std::to_string(p.depeg_warn_pct) + ")");
        }

        pairs.push_back(std::move(p));
    }

    return pairs;
}

StrategyConfig parse_strategy(const YAML::Node& root)
{
    const std::string sec = "strategy";
    if (!root[sec] || !root[sec].IsMap()) {
        throw ConfigError("'" + sec + "' section is required and must be a map");
    }
    const YAML::Node& node = root[sec];

    StrategyConfig cfg;
    cfg.gamma                = read_positive_double(node, "gamma", sec);
    cfg.kappa                = read_positive_double(node, "kappa", sec);
    cfg.phi                  = read_positive_double(node, "phi", sec);
    cfg.q_max                = read_positive_double(node, "q_max", sec);
    cfg.min_profit_margin_bps = read_positive_double(node, "min_profit_margin_bps", sec);
    cfg.offer_ttl_blocks     = read_uint32_positive(node, "offer_ttl_blocks", sec);
    cfg.num_tiers            = read_uint32_positive(node, "num_tiers", sec);

    cfg.tier_spacing_bps = read_positive_double_seq(node, "tier_spacing_bps", sec);
    cfg.tier_size_pct    = read_positive_double_seq(node, "tier_size_pct", sec);

    // Array lengths must match num_tiers.
    if (cfg.tier_spacing_bps.size() != cfg.num_tiers) {
        throw ConfigError(sec + ".tier_spacing_bps length ("
                          + std::to_string(cfg.tier_spacing_bps.size())
                          + ") must equal num_tiers ("
                          + std::to_string(cfg.num_tiers) + ")");
    }
    if (cfg.tier_size_pct.size() != cfg.num_tiers) {
        throw ConfigError(sec + ".tier_size_pct length ("
                          + std::to_string(cfg.tier_size_pct.size())
                          + ") must equal num_tiers ("
                          + std::to_string(cfg.num_tiers) + ")");
    }

    // tier_spacing_bps must be strictly ascending.
    for (std::size_t i = 1; i < cfg.tier_spacing_bps.size(); ++i) {
        if (cfg.tier_spacing_bps[i] <= cfg.tier_spacing_bps[i - 1]) {
            throw ConfigError(sec + ".tier_spacing_bps must be strictly ascending; "
                              "tier " + std::to_string(i) + " ("
                              + std::to_string(cfg.tier_spacing_bps[i])
                              + ") <= tier " + std::to_string(i - 1) + " ("
                              + std::to_string(cfg.tier_spacing_bps[i - 1]) + ")");
        }
    }

    // tier_size_pct elements must each be in (0, 1], and sum to ~1.0.
    for (std::size_t i = 0; i < cfg.tier_size_pct.size(); ++i) {
        if (cfg.tier_size_pct[i] <= 0.0 || cfg.tier_size_pct[i] > 1.0) {
            throw ConfigError(sec + ".tier_size_pct[" + std::to_string(i)
                              + "] must be in (0, 1]; got "
                              + std::to_string(cfg.tier_size_pct[i]));
        }
    }
    double tier_sum = std::accumulate(cfg.tier_size_pct.begin(),
                                      cfg.tier_size_pct.end(), 0.0);
    if (std::fabs(tier_sum - 1.0) > 0.01) {
        throw ConfigError(sec + ".tier_size_pct values must sum to ~1.0; got "
                          + std::to_string(tier_sum));
    }

    // Optional: global half-spread cap (bps).  Defaults to 250 bps if absent.
    // Prevents compounding multipliers from producing effective market withdrawal.
    if (node["max_half_spread_bps"] && node["max_half_spread_bps"].IsDefined()
        && !node["max_half_spread_bps"].IsNull()) {
        cfg.max_half_spread_bps = node["max_half_spread_bps"].as<double>();
        if (cfg.max_half_spread_bps <= 0.0) {
            throw ConfigError(sec + ".max_half_spread_bps must be > 0; got "
                              + std::to_string(cfg.max_half_spread_bps));
        }
    }
    // else: default from StrategyConfig{} is used (250.0 bps).

    // Optional: on-chain fee per offer/cancel (mojos).  Default 0.0001 XCH.
    if (node["offer_fee_mojos"] && node["offer_fee_mojos"].IsDefined()
        && !node["offer_fee_mojos"].IsNull()) {
        auto fee = node["offer_fee_mojos"].as<std::uint64_t>();
        if (fee == 0) {
            throw ConfigError(sec + ".offer_fee_mojos must be > 0");
        }
        cfg.offer_fee_mojos = fee;
    }

    return cfg;
}

RiskConfig parse_risk(const YAML::Node& root)
{
    const std::string sec = "risk";
    if (!root[sec] || !root[sec].IsMap()) {
        throw ConfigError("'" + sec + "' section is required and must be a map");
    }
    const YAML::Node& node = root[sec];

    RiskConfig cfg;
    cfg.soft_limit_pct          = read_fraction(node, "soft_limit_pct", sec);
    cfg.hard_limit_pct          = read_fraction(node, "hard_limit_pct", sec);
    cfg.single_cat_cap_pct      = read_fraction(node, "single_cat_cap_pct", sec);
    cfg.kelly_fraction          = read_fraction(node, "kelly_fraction", sec);
    cfg.max_capital_per_pair_pct = read_fraction(node, "max_capital_per_pair_pct", sec);

    // Hard limit must exceed soft limit to maintain a meaningful escalation.
    if (cfg.hard_limit_pct <= cfg.soft_limit_pct) {
        throw ConfigError(sec + ".hard_limit_pct ("
                          + std::to_string(cfg.hard_limit_pct)
                          + ") must be greater than soft_limit_pct ("
                          + std::to_string(cfg.soft_limit_pct) + ")");
    }

    return cfg;
}

VolatilityConfig parse_volatility(const YAML::Node& root)
{
    const std::string sec = "volatility";
    if (!root[sec] || !root[sec].IsMap()) {
        throw ConfigError("'" + sec + "' section is required and must be a map");
    }
    const YAML::Node& node = root[sec];

    VolatilityConfig cfg;
    cfg.lookback_blocks = read_uint32_positive(node, "lookback_blocks", sec);
    cfg.yz_alpha        = read_fraction_open(node, "yz_alpha", sec);

    return cfg;
}

MonitoringConfig parse_monitoring(const YAML::Node& root)
{
    const std::string sec = "monitoring";
    if (!root[sec] || !root[sec].IsMap()) {
        throw ConfigError("'" + sec + "' section is required and must be a map");
    }
    const YAML::Node& node = root[sec];

    MonitoringConfig cfg;
    cfg.prometheus_port   = read_port(node, "prometheus_port", sec);
    // Telegram fields are optional; empty means alerts are disabled.
    cfg.telegram_bot_token = read_string_opt(node, "telegram_bot_token", "");
    cfg.telegram_chat_id   = read_string_opt(node, "telegram_chat_id", "");

    // If one Telegram field is set, both must be set.
    bool has_token = !cfg.telegram_bot_token.empty();
    bool has_chat  = !cfg.telegram_chat_id.empty();
    if (has_token != has_chat) {
        throw ConfigError(sec + ": telegram_bot_token and telegram_chat_id "
                          "must both be set or both be empty");
    }

    return cfg;
}

DatabaseConfig parse_database(const YAML::Node& root)
{
    const std::string sec = "database";
    if (!root[sec] || !root[sec].IsMap()) {
        throw ConfigError("'" + sec + "' section is required and must be a map");
    }
    const YAML::Node& node = root[sec];

    DatabaseConfig cfg;
    cfg.path = expand_tilde(read_string(node, "path", sec));

    return cfg;
}

DepegConfig parse_depeg(const YAML::Node& root)
{
    const std::string sec = "depeg";
    DepegConfig cfg;  // All fields have sensible defaults.

    // Section is optional; return defaults if absent.
    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;
    }
    const YAML::Node& node = root[sec];

    if (node["enabled"] && node["enabled"].IsDefined()
        && !node["enabled"].IsNull()) {
        cfg.enabled = node["enabled"].as<bool>();
    }
    if (node["default_warn_pct"] && node["default_warn_pct"].IsDefined()
        && !node["default_warn_pct"].IsNull()) {
        cfg.default_warn_pct = node["default_warn_pct"].as<double>();
        if (!(cfg.default_warn_pct > 0.0)) {
            throw ConfigError(sec + ".default_warn_pct must be > 0");
        }
    }
    if (node["default_bail_pct"] && node["default_bail_pct"].IsDefined()
        && !node["default_bail_pct"].IsNull()) {
        cfg.default_bail_pct = node["default_bail_pct"].as<double>();
        if (!(cfg.default_bail_pct > 0.0)) {
            throw ConfigError(sec + ".default_bail_pct must be > 0");
        }
    }
    if (node["default_sustained_blocks"] && node["default_sustained_blocks"].IsDefined()
        && !node["default_sustained_blocks"].IsNull()) {
        cfg.default_sustained_blocks = node["default_sustained_blocks"].as<uint32_t>();
    }
    if (node["auto_disable_pair"] && node["auto_disable_pair"].IsDefined()
        && !node["auto_disable_pair"].IsNull()) {
        cfg.auto_disable_pair = node["auto_disable_pair"].as<bool>();
    }
    if (node["alert_on_warn"] && node["alert_on_warn"].IsDefined()
        && !node["alert_on_warn"].IsNull()) {
        cfg.alert_on_warn = node["alert_on_warn"].as<bool>();
    }
    if (node["alert_on_bail"] && node["alert_on_bail"].IsDefined()
        && !node["alert_on_bail"].IsNull()) {
        cfg.alert_on_bail = node["alert_on_bail"].as<bool>();
    }

    if (cfg.default_bail_pct <= cfg.default_warn_pct) {
        throw ConfigError(sec + ".default_bail_pct ("
                          + std::to_string(cfg.default_bail_pct)
                          + ") must be greater than default_warn_pct ("
                          + std::to_string(cfg.default_warn_pct) + ")");
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_arbitrage -- optional `arbitrage:` section.
// ---------------------------------------------------------------------------
ArbitrageSettings parse_arbitrage(const YAML::Node& root)
{
    const std::string sec = "arbitrage";
    ArbitrageSettings cfg;  // All fields have sensible defaults.

    // Section is optional; return defaults if absent.
    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;
    }
    const YAML::Node& node = root[sec];

    auto read_bool = [&](const char* key, bool& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<bool>();
    };
    auto read_dbl = [&](const char* key, double& out, double lo = 0.0) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull()) {
            out = node[key].as<double>();
            if (out < lo)
                throw ConfigError(sec + "." + key + " must be >= "
                                  + std::to_string(lo));
        }
    };
    auto read_u32 = [&](const char* key, uint32_t& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<uint32_t>();
    };

    read_bool("enabled",                        cfg.enabled);

    // Triangular
    read_dbl ("triangular_min_profit_bps",      cfg.triangular_min_profit_bps);
    read_dbl ("triangular_slippage_bps",        cfg.triangular_slippage_bps);
    read_dbl ("triangular_per_leg_fee_bps",     cfg.triangular_per_leg_fee_bps);
    read_u32 ("triangular_max_legs",            cfg.triangular_max_legs);

    // CEX-DEX
    read_dbl ("cex_dex_min_edge_bps",           cfg.cex_dex_min_edge_bps);
    read_dbl ("cex_dex_max_edge_bps",           cfg.cex_dex_max_edge_bps);
    read_dbl ("cex_fee_bps",                    cfg.cex_fee_bps);
    read_dbl ("bridge_fee_bps",                 cfg.bridge_fee_bps);

    // Cross-DEX
    read_dbl ("cross_dex_min_edge_bps",         cfg.cross_dex_min_edge_bps);
    read_dbl ("tibetswap_fee_bps",              cfg.tibetswap_fee_bps);
    read_dbl ("dexie_fee_bps",                  cfg.dexie_fee_bps);

    // Cross-Bridge
    read_dbl ("cross_bridge_min_edge_bps",      cfg.cross_bridge_min_edge_bps);
    read_dbl ("bridge_cost_bps",                cfg.bridge_cost_bps);

    // General
    read_dbl ("max_position_size",              cfg.max_position_size, 0.001);
    read_dbl ("default_confidence",             cfg.default_confidence, 0.01);
    read_dbl ("min_confidence_threshold",       cfg.min_confidence_threshold);
    read_u32 ("default_urgency_blocks",         cfg.default_urgency_blocks);

    return cfg;
}

// ---------------------------------------------------------------------------
// Redacted summary printer.  Emits every operationally useful field while
// suppressing all classified secrets (SSL paths, fingerprint, tokens).
// ---------------------------------------------------------------------------
void log_config_summary(const AppConfig& cfg)
{
    std::ostringstream out;
    out << "=== XOPTrader configuration loaded ===\n";

    // Chia -- hosts and ports only; SSL paths and fingerprint are secret.
    out << "[chia]\n"
        << "  full_node  = " << cfg.chia.full_node_host
        << ":" << cfg.chia.full_node_port << "\n"
        << "  wallet     = " << cfg.chia.wallet_host
        << ":" << cfg.chia.wallet_port << "\n"
        << "  ssl_cert   = <redacted>\n"
        << "  ssl_key    = <redacted>\n"
        << "  wal_cert   = <redacted>\n"
        << "  wal_key    = <redacted>\n"
        << "  fingerprint= <redacted>\n";

    // Dexie -- no secrets.
    out << "[dexie]\n"
        << "  api_base   = " << cfg.dexie.api_base << "\n"
        << "  rate_limit = " << cfg.dexie.max_requests_per_10s << " req/10s\n";

    // Pairs -- asset IDs and names are operational data, not secrets.
    out << "[pairs] (" << cfg.pairs.size() << " configured)\n";
    for (std::size_t i = 0; i < cfg.pairs.size(); ++i) {
        out << "  " << i << ": " << cfg.pairs[i].name
            << (cfg.pairs[i].enabled ? " [enabled]" : " [disabled]");
        if (cfg.pairs[i].is_stablecoin) {
            out << " [stablecoin peg=" << cfg.pairs[i].peg_target
                << " warn=" << cfg.pairs[i].depeg_warn_pct
                << "% bail=" << cfg.pairs[i].depeg_bail_pct << "%]";
        }
        if (cfg.pairs[i].gamma_override) {
            out << " [gamma=" << *cfg.pairs[i].gamma_override << "]";
        }
        if (cfg.pairs[i].phi_override) {
            out << " [phi=" << *cfg.pairs[i].phi_override << "]";
        }
        out << "\n";
    }

    // Strategy -- all fields are tuning parameters, no secrets.
    out << "[strategy]\n"
        << "  gamma      = " << cfg.strategy.gamma << "\n"
        << "  kappa      = " << cfg.strategy.kappa << "\n"
        << "  phi        = " << cfg.strategy.phi << "\n"
        << "  q_max      = " << cfg.strategy.q_max << "\n"
        << "  min_margin = " << cfg.strategy.min_profit_margin_bps << " bps\n"
        << "  offer_ttl  = " << cfg.strategy.offer_ttl_blocks << " blocks\n"
        << "  tiers      = " << cfg.strategy.num_tiers << "\n"
        << "  spacing    = [";
    for (std::size_t i = 0; i < cfg.strategy.tier_spacing_bps.size(); ++i) {
        if (i > 0) out << ", ";
        out << cfg.strategy.tier_spacing_bps[i];
    }
    out << "] bps\n"
        << "  sizes      = [";
    for (std::size_t i = 0; i < cfg.strategy.tier_size_pct.size(); ++i) {
        if (i > 0) out << ", ";
        out << cfg.strategy.tier_size_pct[i];
    }
    out << "]\n"
        << "  max_hs_cap = " << cfg.strategy.max_half_spread_bps << " bps\n";

    // Risk -- all fields are tuning parameters.
    out << "[risk]\n"
        << "  soft_limit = " << cfg.risk.soft_limit_pct << "\n"
        << "  hard_limit = " << cfg.risk.hard_limit_pct << "\n"
        << "  cat_cap    = " << cfg.risk.single_cat_cap_pct << "\n"
        << "  kelly      = " << cfg.risk.kelly_fraction << "\n"
        << "  max_pair   = " << cfg.risk.max_capital_per_pair_pct << "\n";

    // Volatility -- no secrets.
    out << "[volatility]\n"
        << "  lookback   = " << cfg.volatility.lookback_blocks << " blocks\n"
        << "  yz_alpha   = " << cfg.volatility.yz_alpha << "\n";

    // Monitoring -- port is public; Telegram credentials are secret.
    out << "[monitoring]\n"
        << "  prometheus = :" << cfg.monitoring.prometheus_port << "\n"
        << "  telegram   = "
        << (cfg.monitoring.telegram_bot_token.empty() ? "disabled" : "<configured>")
        << "\n";

    // Database -- path is operational data.
    out << "[database]\n"
        << "  path       = " << cfg.database.path << "\n";

    // Depeg detection -- operational settings.
    out << "[depeg]\n"
        << "  enabled    = " << (cfg.depeg.enabled ? "true" : "false") << "\n"
        << "  warn_pct   = " << cfg.depeg.default_warn_pct << "%\n"
        << "  bail_pct   = " << cfg.depeg.default_bail_pct << "%\n"
        << "  sustained  = " << cfg.depeg.default_sustained_blocks << " blocks\n"
        << "  auto_off   = " << (cfg.depeg.auto_disable_pair ? "true" : "false") << "\n";

    // Arbitrage detection -- operational settings.
    out << "[arbitrage]\n"
        << "  enabled              = " << (cfg.arbitrage.enabled ? "true" : "false") << "\n"
        << "  tri_min_profit_bps   = " << cfg.arbitrage.triangular_min_profit_bps << "\n"
        << "  tri_slippage_bps     = " << cfg.arbitrage.triangular_slippage_bps << "\n"
        << "  tri_fee_bps/leg      = " << cfg.arbitrage.triangular_per_leg_fee_bps << "\n"
        << "  max_position_size    = " << cfg.arbitrage.max_position_size << "\n"
        << "  min_confidence       = " << cfg.arbitrage.min_confidence_threshold << "\n";

    out << "======================================\n";

    std::cout << out.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AppConfig load_config(const std::string& path)
{
    // Attempt to load the YAML file.  yaml-cpp throws on I/O or parse errors;
    // wrap them in ConfigError for a uniform exception type.
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw ConfigError("Failed to load configuration from '" + path + "': "
                          + std::string(e.what()));
    }

    if (!root.IsMap()) {
        throw ConfigError("Top-level YAML structure must be a map in '" + path + "'");
    }

    // Parse each required section.  Individual parsers throw ConfigError on
    // missing or invalid fields.
    AppConfig cfg;
    cfg.chia       = parse_chia(root);
    cfg.dexie      = parse_dexie(root);
    cfg.pairs      = parse_pairs(root);
    cfg.strategy   = parse_strategy(root);
    cfg.risk       = parse_risk(root);
    cfg.volatility = parse_volatility(root);
    cfg.monitoring = parse_monitoring(root);
    cfg.database   = parse_database(root);
    cfg.depeg      = parse_depeg(root);
    cfg.arbitrage  = parse_arbitrage(root);

    // Emit a redacted summary so operators can verify the loaded parameters
    // without exposing secrets in log files.
    log_config_summary(cfg);

    return cfg;
}

} // namespace xop
