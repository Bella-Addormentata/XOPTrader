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

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

// [T4-04] Expand `${VAR}` environment-variable references in a string value.
//
// Syntax:
//   ${VAR}            — replaced with the value of environment variable VAR.
//                       Throws ConfigError if VAR is not set.
//   ${VAR:-default}   — replaced with VAR's value, or "default" if unset.
//
// Unmatched or malformed `${...}` sequences are left unchanged so that
// non-env-var uses of `$` (e.g., in asset IDs) are never silently altered.
//
// ISO/IEC 27001:2022 — enables externalization of secrets (SSL paths,
// Telegram tokens, API keys) from plaintext YAML into the OS environment.
std::string expand_env_vars(const std::string& input)
{
    std::string result;
    result.reserve(input.size());

    std::size_t i = 0;
    while (i < input.size()) {
        // Look for `${`
        if (i + 1 < input.size() && input[i] == '$' && input[i + 1] == '{') {
            const auto close = input.find('}', i + 2);
            if (close == std::string::npos) {
                // No closing brace — not an env-var reference; copy literally.
                result += input[i];
                ++i;
                continue;
            }

            // Extract the content between ${ and }.
            const std::string spec = input.substr(i + 2, close - (i + 2));

            // Check for default value syntax: VAR:-default
            std::string var_name;
            std::string default_val;
            bool has_default = false;

            const auto sep = spec.find(":-");
            if (sep != std::string::npos) {
                var_name    = spec.substr(0, sep);
                default_val = spec.substr(sep + 2);
                has_default = true;
            } else {
                var_name = spec;
            }

            // Validate variable name: must be non-empty and alphanumeric + underscores.
            if (var_name.empty()) {
                result += input.substr(i, close - i + 1);
                i = close + 1;
                continue;
            }

            const char* env_val = std::getenv(var_name.c_str());
            if (env_val) {
                result += env_val;
            } else if (has_default) {
                result += default_val;
            } else {
                throw ConfigError(
                    "Environment variable '" + var_name + "' referenced in "
                    "config via ${" + var_name + "} is not set. "
                    "Set the variable or use ${" + var_name + ":-default} syntax.");
            }

            i = close + 1;
        } else {
            result += input[i];
            ++i;
        }
    }

    return result;
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
// [T4-04] Environment variable references (${VAR}, ${VAR:-default}) are
// expanded before trimming, allowing secrets to be externalized.
std::string read_string(const YAML::Node& parent,
                        const std::string& key,
                        const std::string& section)
{
    require_scalar(parent, key, section);
    std::string value = expand_env_vars(parent[key].as<std::string>());
    // Trim leading and trailing whitespace.
    auto start = value.find_first_not_of(" \t\r\n");
    auto end   = value.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) {
        throw ConfigError(section + "." + key + " must not be empty or whitespace-only");
    }
    return value.substr(start, end - start + 1);
}

// Read an optional string field; return fallback when absent.
// [T4-04] Environment variable references are expanded in the value.
std::string read_string_opt(const YAML::Node& parent,
                            const std::string& key,
                            const std::string& fallback)
{
    if (!parent[key] || parent[key].IsNull()) {
        return fallback;
    }
    return expand_env_vars(parent[key].as<std::string>());
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

    // Parse mode (optional, defaults to "auto").
    if (node["mode"] && node["mode"].IsScalar()) {
        const std::string mode_str = node["mode"].as<std::string>();
        if (mode_str == "auto") {
            cfg.mode = ChiaMode::Auto;
        } else if (mode_str == "full_node") {
            cfg.mode = ChiaMode::FullNode;
        } else if (mode_str == "wallet_only") {
            cfg.mode = ChiaMode::WalletOnly;
        } else {
            throw ConfigError(sec + ".mode must be 'auto', 'full_node', "
                              "or 'wallet_only'; got '" + mode_str + "'");
        }
    }

    cfg.wallet_host      = read_string(node, "wallet_host", sec);
    cfg.wallet_port      = read_port(node, "wallet_port", sec);

    // Full node settings are optional in wallet_only mode.
    if (cfg.mode == ChiaMode::WalletOnly) {
        if (node["full_node_host"] && node["full_node_host"].IsScalar()) {
            cfg.full_node_host = node["full_node_host"].as<std::string>();
        }
        if (node["full_node_port"] && node["full_node_port"].IsScalar()) {
            cfg.full_node_port = node["full_node_port"].as<uint16_t>();
        }
    } else {
        cfg.full_node_host   = read_string(node, "full_node_host", sec);
        cfg.full_node_port   = read_port(node, "full_node_port", sec);
    }

    // SSL paths: wallet certs are always required; full-node certs are
    // optional when running in wallet_only mode.
    cfg.wallet_cert_path = expand_tilde(read_string(node, "wallet_cert_path", sec));
    cfg.wallet_key_path  = expand_tilde(read_string(node, "wallet_key_path", sec));

    // CA certificate — required; used for SSL peer verification.
    cfg.ca_cert_path = expand_tilde(read_string(node, "ca_cert_path", sec));

    // SSL verification defaults to true when omitted.
    if (node["verify_ssl"] && node["verify_ssl"].IsDefined() && !node["verify_ssl"].IsNull()) {
        cfg.verify_ssl = node["verify_ssl"].as<bool>();
    }

    if (cfg.mode == ChiaMode::WalletOnly) {
        // Full-node SSL paths optional -- read if present.
        if (node["ssl_cert_path"] && node["ssl_cert_path"].IsScalar()) {
            cfg.ssl_cert_path = expand_tilde(node["ssl_cert_path"].as<std::string>());
        }
        if (node["ssl_key_path"] && node["ssl_key_path"].IsScalar()) {
            cfg.ssl_key_path = expand_tilde(node["ssl_key_path"].as<std::string>());
        }
    } else {
        cfg.ssl_cert_path    = expand_tilde(read_string(node, "ssl_cert_path", sec));
        cfg.ssl_key_path     = expand_tilde(read_string(node, "ssl_key_path", sec));
    }

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

    if (node["claim_rewards"] && node["claim_rewards"].IsDefined()
        && !node["claim_rewards"].IsNull()) {
        cfg.claim_rewards = node["claim_rewards"].as<bool>();
    }

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

        // Auto-detect mojos-per-unit from asset type.
        // XCH = 1e12 mojos/unit, CAT tokens = 1e3 mojos/unit.
        p.base_mojos_per_unit  = (p.base_asset_id  == "xch") ? 1'000'000'000'000LL : 1'000LL;
        p.quote_mojos_per_unit = (p.quote_asset_id == "xch") ? 1'000'000'000'000LL : 1'000LL;

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

        // -- Stablecoin trading overrides (optional) --------------------
        if (item["max_half_spread_bps_override"]
            && item["max_half_spread_bps_override"].IsDefined()
            && !item["max_half_spread_bps_override"].IsNull()) {
            double v = item["max_half_spread_bps_override"].as<double>();
            if (!(v > 0.0)) {
                throw ConfigError(idx + ".max_half_spread_bps_override must be > 0; got "
                                  + std::to_string(v));
            }
            p.max_half_spread_bps_override = v;
        }
        if (item["min_offer_size_units_override"]
            && item["min_offer_size_units_override"].IsDefined()
            && !item["min_offer_size_units_override"].IsNull()) {
            double v = item["min_offer_size_units_override"].as<double>();
            if (!(v >= 0.0)) {
                throw ConfigError(idx + ".min_offer_size_units_override must be >= 0; got "
                                  + std::to_string(v));
            }
            p.min_offer_size_units_override = v;
        }
        if (item["peg_anchor_threshold_pct"]
            && item["peg_anchor_threshold_pct"].IsDefined()
            && !item["peg_anchor_threshold_pct"].IsNull()) {
            p.peg_anchor_threshold_pct = item["peg_anchor_threshold_pct"].as<double>();
        }
        if (item["peg_anchor_weight"]
            && item["peg_anchor_weight"].IsDefined()
            && !item["peg_anchor_weight"].IsNull()) {
            double v = item["peg_anchor_weight"].as<double>();
            if (v < 0.0 || v > 1.0) {
                throw ConfigError(idx + ".peg_anchor_weight must be in [0,1]; got "
                                  + std::to_string(v));
            }
            p.peg_anchor_weight = v;
        }
        if (item["stablecoin_exempt_buyonly"]
            && item["stablecoin_exempt_buyonly"].IsDefined()
            && !item["stablecoin_exempt_buyonly"].IsNull()) {
            p.stablecoin_exempt_buyonly = item["stablecoin_exempt_buyonly"].as<bool>();
        }
        if (item["stablecoin_undercut_all_tiers"]
            && item["stablecoin_undercut_all_tiers"].IsDefined()
            && !item["stablecoin_undercut_all_tiers"].IsNull()) {
            p.stablecoin_undercut_all_tiers = item["stablecoin_undercut_all_tiers"].as<bool>();
        }
        if (item["stablecoin_flat_sizing"]
            && item["stablecoin_flat_sizing"].IsDefined()
            && !item["stablecoin_flat_sizing"].IsNull()) {
            p.stablecoin_flat_sizing = item["stablecoin_flat_sizing"].as<bool>();
        }
        if (item["stablecoin_skip_gap_aware"]
            && item["stablecoin_skip_gap_aware"].IsDefined()
            && !item["stablecoin_skip_gap_aware"].IsNull()) {
            p.stablecoin_skip_gap_aware = item["stablecoin_skip_gap_aware"].as<bool>();
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

    // [v0.7.37] sigma_floor: minimum annualized sigma for GLFT formula.
    if (node["sigma_floor"] && node["sigma_floor"].IsDefined()
        && !node["sigma_floor"].IsNull()) {
        cfg.sigma_floor = node["sigma_floor"].as<double>();
        if (cfg.sigma_floor < 0.0) {
            throw ConfigError(sec + ".sigma_floor must be >= 0");
        }
    }
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

    // Optional: high-volatility regime multiplier.  Default 1.80.
    if (node["high_vol_multiplier"] && node["high_vol_multiplier"].IsDefined()
        && !node["high_vol_multiplier"].IsNull()) {
        cfg.high_vol_multiplier = node["high_vol_multiplier"].as<double>();
        if (cfg.high_vol_multiplier < 1.0) {
            throw ConfigError(sec + ".high_vol_multiplier must be >= 1.0; got "
                              + std::to_string(cfg.high_vol_multiplier));
        }
    }

    // Optional: on-chain fee per offer/cancel (mojos).  Default 0.0001 XCH.
    if (node["offer_fee_mojos"] && node["offer_fee_mojos"].IsDefined()
        && !node["offer_fee_mojos"].IsNull()) {
        auto fee = node["offer_fee_mojos"].as<std::uint64_t>();
        if (fee == 0) {
            throw ConfigError(sec + ".offer_fee_mojos must be > 0");
        }
        cfg.offer_fee_mojos = fee;
    }

    // Optional: startup market-analysis window (blocks).  0 = skip.
    if (node["startup_analysis_blocks"] && node["startup_analysis_blocks"].IsDefined()
        && !node["startup_analysis_blocks"].IsNull()) {
        auto blocks = node["startup_analysis_blocks"].as<uint32_t>();
        if (blocks > 1440) {
            throw ConfigError(sec + ".startup_analysis_blocks must be <= 1440");
        }
        cfg.startup_analysis_blocks = blocks;
    }

    // [T4-02] Optional: confirmation depth for reorg protection.
    if (node["confirmation_depth_blocks"] && node["confirmation_depth_blocks"].IsDefined()
        && !node["confirmation_depth_blocks"].IsNull()) {
        cfg.confirmation_depth_blocks = node["confirmation_depth_blocks"].as<uint32_t>();
    }

    // [T4-11] Optional: offer reconciliation interval.
    if (node["reconciliation_interval_blocks"] && node["reconciliation_interval_blocks"].IsDefined()
        && !node["reconciliation_interval_blocks"].IsNull()) {
        cfg.reconciliation_interval_blocks = node["reconciliation_interval_blocks"].as<uint32_t>();
    }

    // [T7-10] Optional: batch offer creation.
    if (node["batch_offers_enabled"] && node["batch_offers_enabled"].IsDefined()
        && !node["batch_offers_enabled"].IsNull()) {
        cfg.batch_offers_enabled = node["batch_offers_enabled"].as<bool>();
    }

    // Spendable reserve threshold.
    if (node["min_spendable_reserve_pct"] && node["min_spendable_reserve_pct"].IsDefined()
        && !node["min_spendable_reserve_pct"].IsNull()) {
        cfg.min_spendable_reserve_pct = node["min_spendable_reserve_pct"].as<double>();
        if (cfg.min_spendable_reserve_pct < 0.0 || cfg.min_spendable_reserve_pct > 1.0) {
            throw ConfigError("strategy.min_spendable_reserve_pct must be in [0, 1]");
        }
    }

    // Stuck offer age (extra blocks beyond TTL).
    if (node["stuck_offer_age_blocks"] && node["stuck_offer_age_blocks"].IsDefined()
        && !node["stuck_offer_age_blocks"].IsNull()) {
        cfg.stuck_offer_age_blocks = node["stuck_offer_age_blocks"].as<uint32_t>();
    }

    // -- Minimum balance management -----------------------------------------
    if (node["fee_reserve_xch"] && node["fee_reserve_xch"].IsDefined()
        && !node["fee_reserve_xch"].IsNull()) {
        cfg.fee_reserve_xch = node["fee_reserve_xch"].as<double>();
        if (cfg.fee_reserve_xch < 0.0) {
            throw ConfigError(sec + ".fee_reserve_xch must be >= 0");
        }
    }
    if (node["fee_min_spendable_xch"] && node["fee_min_spendable_xch"].IsDefined()
        && !node["fee_min_spendable_xch"].IsNull()) {
        cfg.fee_min_spendable_xch = node["fee_min_spendable_xch"].as<double>();
        if (cfg.fee_min_spendable_xch < 0.0) {
            throw ConfigError(sec + ".fee_min_spendable_xch must be >= 0");
        }
    }
    if (node["taker_min_spendable_xch"] && node["taker_min_spendable_xch"].IsDefined()
        && !node["taker_min_spendable_xch"].IsNull()) {
        cfg.taker_min_spendable_xch = node["taker_min_spendable_xch"].as<double>();
        if (cfg.taker_min_spendable_xch < 0.0) {
            throw ConfigError(sec + ".taker_min_spendable_xch must be >= 0");
        }
    }
    if (node["block_time_seconds"] && node["block_time_seconds"].IsDefined()
        && !node["block_time_seconds"].IsNull()) {
        cfg.block_time_seconds = node["block_time_seconds"].as<double>();
        if (cfg.block_time_seconds <= 0.0) {
            throw ConfigError(sec + ".block_time_seconds must be > 0");
        }
    }
    if (node["arb_reserve_coins"] && node["arb_reserve_coins"].IsDefined()
        && !node["arb_reserve_coins"].IsNull()) {
        cfg.arb_reserve_coins = node["arb_reserve_coins"].as<int>();
        if (cfg.arb_reserve_coins < 0) {
            throw ConfigError(sec + ".arb_reserve_coins must be >= 0");
        }
    }
    if (node["min_reserve_units"] && node["min_reserve_units"].IsDefined()
        && !node["min_reserve_units"].IsNull()) {
        cfg.min_reserve_units = node["min_reserve_units"].as<double>();
        if (cfg.min_reserve_units < 0.0) {
            throw ConfigError(sec + ".min_reserve_units must be >= 0");
        }
    }
    if (node["min_offer_size_units"] && node["min_offer_size_units"].IsDefined()
        && !node["min_offer_size_units"].IsNull()) {
        cfg.min_offer_size_units = node["min_offer_size_units"].as<double>();
        if (cfg.min_offer_size_units < 0.0) {
            throw ConfigError(sec + ".min_offer_size_units must be >= 0");
        }
    }
    if (node["min_trading_units"] && node["min_trading_units"].IsDefined()
        && !node["min_trading_units"].IsNull()) {
        cfg.min_trading_units = node["min_trading_units"].as<double>();
        if (cfg.min_trading_units < 0.0) {
            throw ConfigError(sec + ".min_trading_units must be >= 0");
        }
    }
    if (node["auto_rebalance_enabled"] && node["auto_rebalance_enabled"].IsDefined()
        && !node["auto_rebalance_enabled"].IsNull()) {
        cfg.auto_rebalance_enabled = node["auto_rebalance_enabled"].as<bool>();
    }
    if (node["xch_ask_throttle_enabled"] && node["xch_ask_throttle_enabled"].IsDefined()
        && !node["xch_ask_throttle_enabled"].IsNull()) {
        cfg.xch_ask_throttle_enabled = node["xch_ask_throttle_enabled"].as<bool>();
    }
    if (node["xch_ask_throttle_caution_xch"] && node["xch_ask_throttle_caution_xch"].IsDefined()
        && !node["xch_ask_throttle_caution_xch"].IsNull()) {
        cfg.xch_ask_throttle_caution_xch = node["xch_ask_throttle_caution_xch"].as<double>();
    }
    if (node["xch_ask_throttle_low_xch"] && node["xch_ask_throttle_low_xch"].IsDefined()
        && !node["xch_ask_throttle_low_xch"].IsNull()) {
        cfg.xch_ask_throttle_low_xch = node["xch_ask_throttle_low_xch"].as<double>();
    }
    if (node["xch_ask_throttle_critical_xch"] && node["xch_ask_throttle_critical_xch"].IsDefined()
        && !node["xch_ask_throttle_critical_xch"].IsNull()) {
        cfg.xch_ask_throttle_critical_xch = node["xch_ask_throttle_critical_xch"].as<double>();
    }
    if (node["xch_ask_throttle_aggressiveness"] && node["xch_ask_throttle_aggressiveness"].IsDefined()
        && !node["xch_ask_throttle_aggressiveness"].IsNull()) {
        cfg.xch_ask_throttle_aggressiveness = node["xch_ask_throttle_aggressiveness"].as<double>();
    }
    if (cfg.xch_ask_throttle_caution_xch <= 0.0) {
        throw ConfigError(sec + ".xch_ask_throttle_caution_xch must be > 0");
    }
    if (cfg.xch_ask_throttle_low_xch <= 0.0) {
        throw ConfigError(sec + ".xch_ask_throttle_low_xch must be > 0");
    }
    if (cfg.xch_ask_throttle_critical_xch < 0.0) {
        throw ConfigError(sec + ".xch_ask_throttle_critical_xch must be >= 0");
    }
    if (!(cfg.xch_ask_throttle_caution_xch > cfg.xch_ask_throttle_low_xch
          && cfg.xch_ask_throttle_low_xch > cfg.xch_ask_throttle_critical_xch)) {
        throw ConfigError(sec + ".xch_ask_throttle thresholds must satisfy caution > low > critical");
    }
    if (cfg.xch_ask_throttle_aggressiveness <= 0.0
        || cfg.xch_ask_throttle_aggressiveness > 3.0) {
        throw ConfigError(sec + ".xch_ask_throttle_aggressiveness must be in (0, 3]");
    }

    // -- Coin pool management -----------------------------------------------
    if (node["coin_pool_target_count"] && node["coin_pool_target_count"].IsDefined()
        && !node["coin_pool_target_count"].IsNull()) {
        cfg.coin_pool_target_count = node["coin_pool_target_count"].as<int>();
        if (cfg.coin_pool_target_count < 0) {
            throw ConfigError(sec + ".coin_pool_target_count must be >= 0");
        }
    }
    if (node["coin_pool_target_xch"] && node["coin_pool_target_xch"].IsDefined()
        && !node["coin_pool_target_xch"].IsNull()) {
        cfg.coin_pool_target_xch = node["coin_pool_target_xch"].as<double>();
        if (cfg.coin_pool_target_xch <= 0.0) {
            throw ConfigError(sec + ".coin_pool_target_xch must be > 0");
        }
    }
    if (node["coin_pool_interval_blocks"] && node["coin_pool_interval_blocks"].IsDefined()
        && !node["coin_pool_interval_blocks"].IsNull()) {
        cfg.coin_pool_interval_blocks = node["coin_pool_interval_blocks"].as<uint32_t>();
    }
    if (node["cat_coin_pool_target_count"] && node["cat_coin_pool_target_count"].IsDefined()
        && !node["cat_coin_pool_target_count"].IsNull()) {
        cfg.cat_coin_pool_target_count = node["cat_coin_pool_target_count"].as<int>();
        if (cfg.cat_coin_pool_target_count < 0) {
            throw ConfigError(sec + ".cat_coin_pool_target_count must be >= 0");
        }
    }
    if (node["cat_coin_pool_target_units"] && node["cat_coin_pool_target_units"].IsDefined()
        && !node["cat_coin_pool_target_units"].IsNull()) {
        cfg.cat_coin_pool_target_units = node["cat_coin_pool_target_units"].as<double>();
        if (cfg.cat_coin_pool_target_units <= 0.0) {
            throw ConfigError(sec + ".cat_coin_pool_target_units must be > 0");
        }
    }

    // -- Gap-aware dynamic tier spacing (optional, defaults in StrategyConfig) --
    if (node["gap_aware_spacing"] && node["gap_aware_spacing"].IsDefined()
        && !node["gap_aware_spacing"].IsNull()) {
        cfg.gap_aware_spacing = node["gap_aware_spacing"].as<bool>();
    }
    if (node["min_gap_bps"] && node["min_gap_bps"].IsDefined()
        && !node["min_gap_bps"].IsNull()) {
        cfg.min_gap_bps = node["min_gap_bps"].as<double>();
        if (cfg.min_gap_bps < 0.0) {
            throw ConfigError(sec + ".min_gap_bps must be >= 0");
        }
    }
    if (node["max_gap_scan_bps"] && node["max_gap_scan_bps"].IsDefined()
        && !node["max_gap_scan_bps"].IsNull()) {
        cfg.max_gap_scan_bps = node["max_gap_scan_bps"].as<double>();
        if (cfg.max_gap_scan_bps <= 0.0) {
            throw ConfigError(sec + ".max_gap_scan_bps must be > 0");
        }
    }
    if (node["gap_blend_factor"] && node["gap_blend_factor"].IsDefined()
        && !node["gap_blend_factor"].IsNull()) {
        cfg.gap_blend_factor = node["gap_blend_factor"].as<double>();
        if (cfg.gap_blend_factor < 0.0 || cfg.gap_blend_factor > 1.0) {
            throw ConfigError(sec + ".gap_blend_factor must be in [0, 1]");
        }
    }

    // -- Competitive anchor pricing (optional, defaults in StrategyConfig) --
    if (node["competitive_anchor_enabled"] && node["competitive_anchor_enabled"].IsDefined()
        && !node["competitive_anchor_enabled"].IsNull()) {
        cfg.competitive_anchor_enabled = node["competitive_anchor_enabled"].as<bool>();
    }
    if (node["competitive_anchor_max_distance_bps"] && node["competitive_anchor_max_distance_bps"].IsDefined()
        && !node["competitive_anchor_max_distance_bps"].IsNull()) {
        cfg.competitive_anchor_max_distance_bps = node["competitive_anchor_max_distance_bps"].as<double>();
        if (cfg.competitive_anchor_max_distance_bps <= 0.0) {
            throw ConfigError(sec + ".competitive_anchor_max_distance_bps must be > 0");
        }
    }
    if (node["competitive_anchor_stride_bps"] && node["competitive_anchor_stride_bps"].IsDefined()
        && !node["competitive_anchor_stride_bps"].IsNull()) {
        cfg.competitive_anchor_stride_bps = node["competitive_anchor_stride_bps"].as<double>();
        if (cfg.competitive_anchor_stride_bps <= 0.0) {
            throw ConfigError(sec + ".competitive_anchor_stride_bps must be > 0");
        }
    }

    // -- Adverse-selection-aware tier sizing (optional, defaults in StrategyConfig) --
    if (node["adverse_selection_sizing"] && node["adverse_selection_sizing"].IsDefined()
        && !node["adverse_selection_sizing"].IsNull()) {
        cfg.adverse_selection_sizing = node["adverse_selection_sizing"].as<bool>();
    }
    if (node["adverse_selection_decay"] && node["adverse_selection_decay"].IsDefined()
        && !node["adverse_selection_decay"].IsNull()) {
        cfg.adverse_selection_decay = node["adverse_selection_decay"].as<double>();
        if (cfg.adverse_selection_decay <= 0.0 || cfg.adverse_selection_decay >= 1.0) {
            throw ConfigError(sec + ".adverse_selection_decay must be in (0, 1)");
        }
    }
    if (node["adverse_selection_sigma_threshold"] && node["adverse_selection_sigma_threshold"].IsDefined()
        && !node["adverse_selection_sigma_threshold"].IsNull()) {
        cfg.adverse_selection_sigma_threshold = node["adverse_selection_sigma_threshold"].as<double>();
        if (cfg.adverse_selection_sigma_threshold < 0.0) {
            throw ConfigError(sec + ".adverse_selection_sigma_threshold must be >= 0");
        }
    }

    // -- Fill-rate-weighted adaptive tier sizing (optional, defaults in LiquidityConfig) --
    if (node["fill_rate_sizing"] && node["fill_rate_sizing"].IsDefined()
        && !node["fill_rate_sizing"].IsNull()) {
        cfg.fill_rate_sizing = node["fill_rate_sizing"].as<bool>();
    }
    if (node["fill_rate_blend"] && node["fill_rate_blend"].IsDefined()
        && !node["fill_rate_blend"].IsNull()) {
        cfg.fill_rate_blend = node["fill_rate_blend"].as<double>();
        if (cfg.fill_rate_blend < 0.0 || cfg.fill_rate_blend > 1.0) {
            throw ConfigError(sec + ".fill_rate_blend must be in [0, 1]");
        }
    }
    if (node["fill_rate_lookback_hours"] && node["fill_rate_lookback_hours"].IsDefined()
        && !node["fill_rate_lookback_hours"].IsNull()) {
        cfg.fill_rate_lookback_hours = node["fill_rate_lookback_hours"].as<int>();
        if (cfg.fill_rate_lookback_hours <= 0) {
            throw ConfigError(sec + ".fill_rate_lookback_hours must be > 0");
        }
    }
    if (node["fill_rate_min_pct"] && node["fill_rate_min_pct"].IsDefined()
        && !node["fill_rate_min_pct"].IsNull()) {
        cfg.fill_rate_min_pct = node["fill_rate_min_pct"].as<double>();
        if (cfg.fill_rate_min_pct < 0.0 || cfg.fill_rate_min_pct >= 1.0) {
            throw ConfigError(sec + ".fill_rate_min_pct must be in [0, 1)");
        }
    }

    // -- AMM blend weight for market data feed (optional) --
    if (node["amm_blend_weight"] && node["amm_blend_weight"].IsDefined()
        && !node["amm_blend_weight"].IsNull()) {
        cfg.amm_blend_weight = node["amm_blend_weight"].as<double>();
        if (cfg.amm_blend_weight < 0.0 || cfg.amm_blend_weight > 1.0) {
            throw ConfigError(sec + ".amm_blend_weight must be in [0, 1]");
        }
    }

    // -- Wall-aware retail niche pricing (optional) --
    if (node["wall_size_threshold_xch"] && node["wall_size_threshold_xch"].IsDefined()
        && !node["wall_size_threshold_xch"].IsNull()) {
        cfg.wall_size_threshold_xch = node["wall_size_threshold_xch"].as<double>();
        if (cfg.wall_size_threshold_xch < 0.0) {
            throw ConfigError(sec + ".wall_size_threshold_xch must be >= 0");
        }
    }
    if (node["wall_niche_premium_pct"] && node["wall_niche_premium_pct"].IsDefined()
        && !node["wall_niche_premium_pct"].IsNull()) {
        cfg.wall_niche_premium_pct = node["wall_niche_premium_pct"].as<double>();
        if (cfg.wall_niche_premium_pct < 0.0 || cfg.wall_niche_premium_pct > 1.0) {
            throw ConfigError(sec + ".wall_niche_premium_pct must be in [0, 1]");
        }
    }

    // -- Smart orphan management (optional) --
    // Guéant, Lehalle & Fernandez-Tapia (2013): cost-aware orphan evaluation.
    if (node["orphan_adopt_enabled"] && node["orphan_adopt_enabled"].IsDefined()
        && !node["orphan_adopt_enabled"].IsNull()) {
        cfg.orphan_adopt_enabled = node["orphan_adopt_enabled"].as<bool>();
    }
    if (node["orphan_adverse_threshold"] && node["orphan_adverse_threshold"].IsDefined()
        && !node["orphan_adverse_threshold"].IsNull()) {
        cfg.orphan_adverse_threshold = node["orphan_adverse_threshold"].as<double>();
        if (cfg.orphan_adverse_threshold < 0.0 || cfg.orphan_adverse_threshold > 1.0) {
            throw ConfigError(sec + ".orphan_adverse_threshold must be in [0, 1]");
        }
    }
    if (node["orphan_max_adopt_age_blocks"] && node["orphan_max_adopt_age_blocks"].IsDefined()
        && !node["orphan_max_adopt_age_blocks"].IsNull()) {
        cfg.orphan_max_adopt_age_blocks = node["orphan_max_adopt_age_blocks"].as<std::uint32_t>();
    }
    if (node["orphan_inventory_bonus"] && node["orphan_inventory_bonus"].IsDefined()
        && !node["orphan_inventory_bonus"].IsNull()) {
        cfg.orphan_inventory_bonus = node["orphan_inventory_bonus"].as<double>();
        if (cfg.orphan_inventory_bonus < 0.0 || cfg.orphan_inventory_bonus > 1.0) {
            throw ConfigError(sec + ".orphan_inventory_bonus must be in [0, 1]");
        }
    }

    // -- Cross-pair correlated inventory skewing (optional) --
    if (node["cross_pair_skew_enabled"] && node["cross_pair_skew_enabled"].IsDefined()
        && !node["cross_pair_skew_enabled"].IsNull()) {
        cfg.cross_pair_skew_enabled = node["cross_pair_skew_enabled"].as<bool>();
    }
    if (node["cross_pair_skew_phi"] && node["cross_pair_skew_phi"].IsDefined()
        && !node["cross_pair_skew_phi"].IsNull()) {
        cfg.cross_pair_skew_phi = node["cross_pair_skew_phi"].as<double>();
        if (cfg.cross_pair_skew_phi < 0.0 || cfg.cross_pair_skew_phi > 1.0) {
            throw ConfigError(sec + ".cross_pair_skew_phi must be in [0, 1]");
        }
    }

    // -- PID adaptive spread controller (optional, defaults in StrategyConfig) --
    if (node["pid_spread_enabled"] && node["pid_spread_enabled"].IsDefined()
        && !node["pid_spread_enabled"].IsNull()) {
        cfg.pid_spread_enabled = node["pid_spread_enabled"].as<bool>();
    }
    if (node["pid_target_fill_rate"] && node["pid_target_fill_rate"].IsDefined()
        && !node["pid_target_fill_rate"].IsNull()) {
        cfg.pid_target_fill_rate = node["pid_target_fill_rate"].as<double>();
        if (cfg.pid_target_fill_rate <= 0.0 || cfg.pid_target_fill_rate >= 1.0) {
            throw ConfigError(sec + ".pid_target_fill_rate must be in (0, 1)");
        }
    }
    if (node["pid_kp"] && node["pid_kp"].IsDefined()
        && !node["pid_kp"].IsNull()) {
        cfg.pid_kp = node["pid_kp"].as<double>();
        if (cfg.pid_kp < 0.0) {
            throw ConfigError(sec + ".pid_kp must be >= 0");
        }
    }
    if (node["pid_ki"] && node["pid_ki"].IsDefined()
        && !node["pid_ki"].IsNull()) {
        cfg.pid_ki = node["pid_ki"].as<double>();
        if (cfg.pid_ki < 0.0) {
            throw ConfigError(sec + ".pid_ki must be >= 0");
        }
    }
    if (node["pid_kd"] && node["pid_kd"].IsDefined()
        && !node["pid_kd"].IsNull()) {
        cfg.pid_kd = node["pid_kd"].as<double>();
        if (cfg.pid_kd < 0.0) {
            throw ConfigError(sec + ".pid_kd must be >= 0");
        }
    }
    if (node["pid_ema_alpha"] && node["pid_ema_alpha"].IsDefined()
        && !node["pid_ema_alpha"].IsNull()) {
        cfg.pid_ema_alpha = node["pid_ema_alpha"].as<double>();
        if (cfg.pid_ema_alpha <= 0.0 || cfg.pid_ema_alpha >= 1.0) {
            throw ConfigError(sec + ".pid_ema_alpha must be in (0, 1)");
        }
    }
    if (node["pid_min_mult"] && node["pid_min_mult"].IsDefined()
        && !node["pid_min_mult"].IsNull()) {
        cfg.pid_min_mult = node["pid_min_mult"].as<double>();
        if (cfg.pid_min_mult <= 0.0 || cfg.pid_min_mult > 1.0) {
            throw ConfigError(sec + ".pid_min_mult must be in (0, 1]");
        }
    }
    if (node["pid_max_mult"] && node["pid_max_mult"].IsDefined()
        && !node["pid_max_mult"].IsNull()) {
        cfg.pid_max_mult = node["pid_max_mult"].as<double>();
        if (cfg.pid_max_mult < 1.0 || cfg.pid_max_mult > 5.0) {
            throw ConfigError(sec + ".pid_max_mult must be in [1, 5]");
        }
    }
    if (node["pid_integral_max"] && node["pid_integral_max"].IsDefined()
        && !node["pid_integral_max"].IsNull()) {
        cfg.pid_integral_max = node["pid_integral_max"].as<double>();
        if (cfg.pid_integral_max <= 0.0) {
            throw ConfigError(sec + ".pid_integral_max must be > 0");
        }
    }
    if (node["pid_warmup_blocks"] && node["pid_warmup_blocks"].IsDefined()
        && !node["pid_warmup_blocks"].IsNull()) {
        cfg.pid_warmup_blocks = node["pid_warmup_blocks"].as<uint32_t>();
    }

    // -- Adaptive competitiveness-threshold PID (v0.7.48) ---------------------
    if (node["comp_pid_enabled"] && node["comp_pid_enabled"].IsDefined()
        && !node["comp_pid_enabled"].IsNull()) {
        cfg.comp_pid_enabled = node["comp_pid_enabled"].as<bool>();
    }
    if (node["comp_pid_target_fill_rate"] && node["comp_pid_target_fill_rate"].IsDefined()
        && !node["comp_pid_target_fill_rate"].IsNull()) {
        cfg.comp_pid_target_fill_rate = node["comp_pid_target_fill_rate"].as<double>();
        if (cfg.comp_pid_target_fill_rate <= 0.0
            || cfg.comp_pid_target_fill_rate >= 1.0) {
            throw ConfigError(sec + ".comp_pid_target_fill_rate must be in (0, 1)");
        }
    }
    if (node["comp_pid_kp"] && node["comp_pid_kp"].IsDefined()
        && !node["comp_pid_kp"].IsNull()) {
        cfg.comp_pid_kp = node["comp_pid_kp"].as<double>();
        if (cfg.comp_pid_kp < 0.0) {
            throw ConfigError(sec + ".comp_pid_kp must be >= 0");
        }
    }
    if (node["comp_pid_ki"] && node["comp_pid_ki"].IsDefined()
        && !node["comp_pid_ki"].IsNull()) {
        cfg.comp_pid_ki = node["comp_pid_ki"].as<double>();
        if (cfg.comp_pid_ki < 0.0) {
            throw ConfigError(sec + ".comp_pid_ki must be >= 0");
        }
    }
    if (node["comp_pid_kd"] && node["comp_pid_kd"].IsDefined()
        && !node["comp_pid_kd"].IsNull()) {
        cfg.comp_pid_kd = node["comp_pid_kd"].as<double>();
        if (cfg.comp_pid_kd < 0.0) {
            throw ConfigError(sec + ".comp_pid_kd must be >= 0");
        }
    }
    if (node["comp_pid_ema_alpha"] && node["comp_pid_ema_alpha"].IsDefined()
        && !node["comp_pid_ema_alpha"].IsNull()) {
        cfg.comp_pid_ema_alpha = node["comp_pid_ema_alpha"].as<double>();
        if (cfg.comp_pid_ema_alpha <= 0.0 || cfg.comp_pid_ema_alpha >= 1.0) {
            throw ConfigError(sec + ".comp_pid_ema_alpha must be in (0, 1)");
        }
    }
    if (node["comp_pid_integral_max"] && node["comp_pid_integral_max"].IsDefined()
        && !node["comp_pid_integral_max"].IsNull()) {
        cfg.comp_pid_integral_max = node["comp_pid_integral_max"].as<double>();
        if (cfg.comp_pid_integral_max <= 0.0) {
            throw ConfigError(sec + ".comp_pid_integral_max must be > 0");
        }
    }
    if (node["comp_pid_warmup_blocks"] && node["comp_pid_warmup_blocks"].IsDefined()
        && !node["comp_pid_warmup_blocks"].IsNull()) {
        cfg.comp_pid_warmup_blocks = node["comp_pid_warmup_blocks"].as<uint32_t>();
    }
    if (node["comp_pid_min_offset"] && node["comp_pid_min_offset"].IsDefined()
        && !node["comp_pid_min_offset"].IsNull()) {
        cfg.comp_pid_min_offset = node["comp_pid_min_offset"].as<int>();
        if (cfg.comp_pid_min_offset < -10 || cfg.comp_pid_min_offset > 0) {
            throw ConfigError(sec + ".comp_pid_min_offset must be in [-10, 0]");
        }
    }
    if (node["comp_pid_max_offset"] && node["comp_pid_max_offset"].IsDefined()
        && !node["comp_pid_max_offset"].IsNull()) {
        cfg.comp_pid_max_offset = node["comp_pid_max_offset"].as<int>();
        if (cfg.comp_pid_max_offset < 0 || cfg.comp_pid_max_offset > 10) {
            throw ConfigError(sec + ".comp_pid_max_offset must be in [0, 10]");
        }
    }
    if (cfg.comp_pid_min_offset > cfg.comp_pid_max_offset) {
        throw ConfigError(sec + ".comp_pid_min_offset must be <= comp_pid_max_offset");
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

    // -- Circuit breaker parameters (all optional with safe defaults) ---------

    // max_drawdown_pct: HWM drawdown threshold in (0, 1].  Default 0.10 (10%).
    if (node["max_drawdown_pct"] && node["max_drawdown_pct"].IsDefined()
            && !node["max_drawdown_pct"].IsNull()) {
        cfg.max_drawdown_pct = node["max_drawdown_pct"].as<double>();
        if (!(cfg.max_drawdown_pct > 0.0 && cfg.max_drawdown_pct <= 1.0)) {
            throw ConfigError(sec + ".max_drawdown_pct must be in (0, 1]; got "
                              + std::to_string(cfg.max_drawdown_pct));
        }
    }

    // drawdown_grace_blocks: blocks to skip drawdown check at startup [0, UINT32_MAX].
    // Default 100.  Allows the engine to absorb small initial losses before
    // the circuit breaker is armed (T8-03).
    if (node["drawdown_grace_blocks"] && node["drawdown_grace_blocks"].IsDefined()
            && !node["drawdown_grace_blocks"].IsNull()) {
        int64_t v = node["drawdown_grace_blocks"].as<int64_t>();
        if (v < 0 || v > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
            throw ConfigError(sec + ".drawdown_grace_blocks must be >= 0; got "
                              + std::to_string(v));
        }
        cfg.drawdown_grace_blocks = static_cast<uint32_t>(v);
    }

    // loss_window_blocks: rolling window in blocks in [1, UINT32_MAX].
    // Default 1152 (~10 h).  Parsed as int64 so we can detect negative /
    // out-of-range values before casting.
    if (node["loss_window_blocks"] && node["loss_window_blocks"].IsDefined()
            && !node["loss_window_blocks"].IsNull()) {
        int64_t wblocks = node["loss_window_blocks"].as<int64_t>();
        if (wblocks < 1
                || wblocks > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
            throw ConfigError(sec + ".loss_window_blocks must be in [1, "
                              + std::to_string(std::numeric_limits<uint32_t>::max())
                              + "]; got " + std::to_string(wblocks));
        }
        cfg.loss_window_blocks = static_cast<uint32_t>(wblocks);
    }

    // max_window_loss_bps: max loss in bps within the window, in [0, 10000].
    // A value of 0 disables the rolling-window circuit breaker.
    // Upper-bound 10000 (= 100%) prevents float→Mojo overflow at the use site.
    if (node["max_window_loss_bps"] && node["max_window_loss_bps"].IsDefined()
            && !node["max_window_loss_bps"].IsNull()) {
        cfg.max_window_loss_bps = node["max_window_loss_bps"].as<double>();
        if (!(cfg.max_window_loss_bps >= 0.0 && cfg.max_window_loss_bps <= 10000.0)) {
            throw ConfigError(sec + ".max_window_loss_bps must be in [0, 10000]; got "
                              + std::to_string(cfg.max_window_loss_bps));
        }
    }

    // flash_crash_threshold_pct: flash crash drop threshold in (0, 1].
    if (node["flash_crash_threshold_pct"] && node["flash_crash_threshold_pct"].IsDefined()
            && !node["flash_crash_threshold_pct"].IsNull()) {
        cfg.flash_crash_threshold_pct = node["flash_crash_threshold_pct"].as<double>();
        if (!(cfg.flash_crash_threshold_pct > 0.0 && cfg.flash_crash_threshold_pct <= 1.0)) {
            throw ConfigError(sec + ".flash_crash_threshold_pct must be in (0, 1]; got "
                              + std::to_string(cfg.flash_crash_threshold_pct));
        }
    }

    // recovery_stable_blocks_phase1: blocks that must be stable for Crash→Recovery.
    if (node["recovery_stable_blocks_phase1"] && node["recovery_stable_blocks_phase1"].IsDefined()
            && !node["recovery_stable_blocks_phase1"].IsNull()) {
        int64_t v = node["recovery_stable_blocks_phase1"].as<int64_t>();
        if (v < 1 || v > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
            throw ConfigError(sec + ".recovery_stable_blocks_phase1 must be >= 1; got "
                              + std::to_string(v));
        }
        cfg.recovery_stable_blocks_phase1 = static_cast<uint32_t>(v);
    }

    // recovery_stable_blocks_phase2: blocks that must be stable for Recovery→Normal.
    if (node["recovery_stable_blocks_phase2"] && node["recovery_stable_blocks_phase2"].IsDefined()
            && !node["recovery_stable_blocks_phase2"].IsNull()) {
        int64_t v = node["recovery_stable_blocks_phase2"].as<int64_t>();
        if (v < 1 || v > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
            throw ConfigError(sec + ".recovery_stable_blocks_phase2 must be >= 1; got "
                              + std::to_string(v));
        }
        cfg.recovery_stable_blocks_phase2 = static_cast<uint32_t>(v);
    }

    // recovery_stability_band_pct: max price deviation for stability check in (0, 1].
    if (node["recovery_stability_band_pct"] && node["recovery_stability_band_pct"].IsDefined()
            && !node["recovery_stability_band_pct"].IsNull()) {
        cfg.recovery_stability_band_pct = node["recovery_stability_band_pct"].as<double>();
        if (!(cfg.recovery_stability_band_pct > 0.0 && cfg.recovery_stability_band_pct <= 1.0)) {
            throw ConfigError(sec + ".recovery_stability_band_pct must be in (0, 1]; got "
                              + std::to_string(cfg.recovery_stability_band_pct));
        }
    }

    // -- Circuit-breaker rebalance (T7-09) -----------------------------------
    if (node["circuit_breaker_enabled"] && node["circuit_breaker_enabled"].IsDefined()
            && !node["circuit_breaker_enabled"].IsNull()) {
        cfg.circuit_breaker_enabled = node["circuit_breaker_enabled"].as<bool>();
    }

    if (node["circuit_breaker_hard_limit_ratio"] && node["circuit_breaker_hard_limit_ratio"].IsDefined()
            && !node["circuit_breaker_hard_limit_ratio"].IsNull()) {
        cfg.circuit_breaker_hard_limit_ratio = node["circuit_breaker_hard_limit_ratio"].as<double>();
        if (!(cfg.circuit_breaker_hard_limit_ratio > 0.0 && cfg.circuit_breaker_hard_limit_ratio <= 1.0)) {
            throw ConfigError(sec + ".circuit_breaker_hard_limit_ratio must be in (0, 1]; got "
                              + std::to_string(cfg.circuit_breaker_hard_limit_ratio));
        }
    }

    if (node["circuit_breaker_age_multiplier"] && node["circuit_breaker_age_multiplier"].IsDefined()
            && !node["circuit_breaker_age_multiplier"].IsNull()) {
        cfg.circuit_breaker_age_multiplier = node["circuit_breaker_age_multiplier"].as<double>();
        if (!(cfg.circuit_breaker_age_multiplier >= 1.0)) {
            throw ConfigError(sec + ".circuit_breaker_age_multiplier must be >= 1.0; got "
                              + std::to_string(cfg.circuit_breaker_age_multiplier));
        }
    }

    if (node["circuit_breaker_max_loss_bps"] && node["circuit_breaker_max_loss_bps"].IsDefined()
            && !node["circuit_breaker_max_loss_bps"].IsNull()) {
        cfg.circuit_breaker_max_loss_bps = node["circuit_breaker_max_loss_bps"].as<double>();
        if (!(cfg.circuit_breaker_max_loss_bps >= 0.0 && cfg.circuit_breaker_max_loss_bps <= 500.0)) {
            throw ConfigError(sec + ".circuit_breaker_max_loss_bps must be in [0, 500]; got "
                              + std::to_string(cfg.circuit_breaker_max_loss_bps));
        }
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

    // [T5-CR6] Optional: candle aggregation window.  Default 10.
    if (node["candle_aggregation_blocks"] && node["candle_aggregation_blocks"].IsDefined()
        && !node["candle_aggregation_blocks"].IsNull()) {
        auto agg = node["candle_aggregation_blocks"].as<uint32_t>();
        if (agg == 0) {
            throw ConfigError(sec + ".candle_aggregation_blocks must be >= 1");
        }
        cfg.candle_aggregation_blocks = agg;
    }

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
    read_dbl ("cex_dex_confidence_cap",         cfg.cex_dex_confidence_cap);
    read_dbl ("cex_reference_half_spread_bps",  cfg.cex_reference_half_spread_bps);

    // Cross-DEX
    read_dbl ("cross_dex_min_edge_bps",         cfg.cross_dex_min_edge_bps);
    read_dbl ("tibetswap_fee_bps",              cfg.tibetswap_fee_bps);
    read_dbl ("dexie_fee_bps",                  cfg.dexie_fee_bps);

    // Cross-Bridge
    read_dbl ("cross_bridge_min_edge_bps",      cfg.cross_bridge_min_edge_bps);
    read_dbl ("bridge_cost_bps",                cfg.bridge_cost_bps);

    // Crossed-book (intra-DEX)
    read_bool("crossed_book_enabled",           cfg.crossed_book_enabled);
    read_dbl ("crossed_book_min_edge_bps",      cfg.crossed_book_min_edge_bps);
    read_dbl ("crossed_book_max_take_xch",      cfg.crossed_book_max_take_xch, 0.001);
    read_bool("cancel_worst_to_free",           cfg.cancel_worst_to_free);

    // Midpoint recycling
    read_bool("midpoint_recycling_enabled",             cfg.midpoint_recycling_enabled);
    read_dbl ("midpoint_recycling_band_bps",            cfg.midpoint_recycling_band_bps);
    read_dbl ("midpoint_recycling_max_take_xch",        cfg.midpoint_recycling_max_take_xch, 0.001);
    read_dbl ("midpoint_recycling_min_take_xch",        cfg.midpoint_recycling_min_take_xch, 0.001);
    read_u32 ("midpoint_recycling_cooldown_blocks",     cfg.midpoint_recycling_cooldown_blocks);
    read_u32 ("midpoint_recycling_max_takes_per_block", cfg.midpoint_recycling_max_takes_per_block);
    read_dbl ("midpoint_recycling_daily_take_xch_cap",  cfg.midpoint_recycling_daily_take_xch_cap, 0.001);
    read_u32 ("midpoint_recycling_epoch_blocks",        cfg.midpoint_recycling_epoch_blocks);
    read_dbl ("midpoint_recycling_min_expected_edge_bps", cfg.midpoint_recycling_min_expected_edge_bps);
    read_dbl ("midpoint_recycling_fee_buffer_bps",      cfg.midpoint_recycling_fee_buffer_bps);
    read_dbl ("midpoint_recycling_toxicity_buffer_bps", cfg.midpoint_recycling_toxicity_buffer_bps);
    read_dbl ("midpoint_recycling_slippage_buffer_bps", cfg.midpoint_recycling_slippage_buffer_bps);
    read_dbl ("midpoint_recycling_inventory_ratio_cap", cfg.midpoint_recycling_inventory_ratio_cap, 0.01);
    read_bool("midpoint_recycling_require_cex_ref",     cfg.midpoint_recycling_require_cex_ref);
    read_u32 ("midpoint_recycling_max_cex_age_blocks",  cfg.midpoint_recycling_max_cex_age_blocks);
    read_dbl ("midpoint_recycling_vpin_max",            cfg.midpoint_recycling_vpin_max, 0.01);
    if (node["midpoint_recycling_pairs"] && node["midpoint_recycling_pairs"].IsSequence()) {
        cfg.midpoint_recycling_pairs.clear();
        for (const auto& item : node["midpoint_recycling_pairs"]) {
            cfg.midpoint_recycling_pairs.push_back(item.as<std::string>());
        }
    }

    // Cross-stablecoin arbitrage
    read_bool("cross_stable_arb_enabled",       cfg.cross_stable_arb_enabled);
    read_dbl ("cross_stable_min_edge_bps",      cfg.cross_stable_min_edge_bps);
    read_dbl ("cross_stable_max_take_xch",      cfg.cross_stable_max_take_xch, 0.001);

    // Peg-crossing offer taker
    read_bool("peg_arb_enabled",                cfg.peg_arb_enabled);
    read_dbl ("peg_arb_min_edge_bps",           cfg.peg_arb_min_edge_bps);
    read_dbl ("peg_arb_max_take_units",         cfg.peg_arb_max_take_units, 0.001);
    read_dbl ("peg_arb_max_inventory_ratio",    cfg.peg_arb_max_inventory_ratio, 0.01);

    // General
    read_dbl ("max_position_size",              cfg.max_position_size, 0.001);
    read_dbl ("default_confidence",             cfg.default_confidence, 0.01);
    read_dbl ("min_confidence_threshold",       cfg.min_confidence_threshold);
    read_u32 ("default_urgency_blocks",         cfg.default_urgency_blocks);

    if (cfg.cex_reference_half_spread_bps < 0.0) {
        throw ConfigError(sec + ".cex_reference_half_spread_bps must be >= 0");
    }
    if (cfg.midpoint_recycling_enabled && cfg.midpoint_recycling_pairs.empty()) {
        throw ConfigError(sec + ".midpoint_recycling_pairs must not be empty when midpoint_recycling_enabled=true");
    }
    if (cfg.midpoint_recycling_band_bps <= 0.0) {
        throw ConfigError(sec + ".midpoint_recycling_band_bps must be > 0");
    }
    if (cfg.midpoint_recycling_min_take_xch >= cfg.midpoint_recycling_max_take_xch) {
        throw ConfigError(sec + ".midpoint_recycling_min_take_xch must be < midpoint_recycling_max_take_xch");
    }
    if (cfg.midpoint_recycling_max_takes_per_block == 0) {
        throw ConfigError(sec + ".midpoint_recycling_max_takes_per_block must be >= 1");
    }
    if (cfg.midpoint_recycling_epoch_blocks != 0 && cfg.midpoint_recycling_epoch_blocks < 100) {
        throw ConfigError(sec + ".midpoint_recycling_epoch_blocks must be >= 100");
    }
    if (cfg.midpoint_recycling_inventory_ratio_cap <= 0.0
        || cfg.midpoint_recycling_inventory_ratio_cap > 1.0) {
        throw ConfigError(sec + ".midpoint_recycling_inventory_ratio_cap must be in (0, 1]");
    }
    if (cfg.midpoint_recycling_vpin_max <= 0.0 || cfg.midpoint_recycling_vpin_max > 1.0) {
        throw ConfigError(sec + ".midpoint_recycling_vpin_max must be in (0, 1]");
    }
    for (const auto& pair_name : cfg.midpoint_recycling_pairs) {
        if (pair_name.empty()) {
            throw ConfigError(sec + ".midpoint_recycling_pairs entries must be non-empty");
        }
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_coingecko -- optional `coingecko:` section.
// ---------------------------------------------------------------------------
CoinGeckoConfig parse_coingecko(const YAML::Node& root)
{
    const std::string sec = "coingecko";
    CoinGeckoConfig cfg;  // All fields have sensible defaults.

    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;
    }
    const YAML::Node& node = root[sec];

    auto read_bool = [&](const char* key, bool& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<bool>();
    };
    auto read_str = [&](const char* key, std::string& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<std::string>();
    };
    auto read_u32 = [&](const char* key, uint32_t& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<uint32_t>();
    };

    read_bool("enabled",                   cfg.enabled);
    read_str ("base_url",                  cfg.base_url);
    read_u32 ("polling_interval_ms",       cfg.polling_interval_ms);
    read_u32 ("request_timeout_ms",        cfg.request_timeout_ms);
    read_u32 ("connect_timeout_ms",        cfg.connect_timeout_ms);
    read_u32 ("max_retries",               cfg.max_retries);
    read_u32 ("retry_base_delay_ms",       cfg.retry_base_delay_ms);
    read_u32 ("rate_limit_max_requests",   cfg.rate_limit_max_requests);
    read_u32 ("rate_limit_window_ms",      cfg.rate_limit_window_ms);
    read_u32 ("curl_thread_pool_size",     cfg.curl_thread_pool_size);
    read_str ("api_key",                   cfg.api_key);
    read_str ("user_agent",                cfg.user_agent);

    // Parse coin_ids as a YAML sequence of strings.
    if (node["coin_ids"] && node["coin_ids"].IsSequence()) {
        for (const auto& item : node["coin_ids"]) {
            cfg.coin_ids.push_back(item.as<std::string>());
        }
    }

    // Validate polling interval isn't too aggressive for free tier.
    if (cfg.polling_interval_ms < 5'000) {
        throw ConfigError(sec + ".polling_interval_ms must be >= 5000 "
                          "(CoinGecko free tier rate limit)");
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_fees -- optional `fees:` section.
// ---------------------------------------------------------------------------
FeeConfig parse_fees(const YAML::Node& root)
{
    const std::string sec = "fees";
    FeeConfig cfg;  // All fields have sensible defaults.

    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;
    }
    const YAML::Node& node = root[sec];

    auto read_bool = [&](const char* key, bool& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<bool>();
    };
    auto read_u64 = [&](const char* key, std::uint64_t& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<std::uint64_t>();
    };
    auto read_dbl = [&](const char* key, double& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<double>();
    };
    auto read_u32 = [&](const char* key, uint32_t& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<uint32_t>();
    };

    read_bool("enabled",              cfg.enabled);
    read_u64 ("daily_budget_mojos",   cfg.daily_budget_mojos);
    read_dbl ("fee_to_gain_max_ratio", cfg.fee_to_gain_max_ratio);
    read_dbl ("cancel_cost_multiplier", cfg.cancel_cost_multiplier);
    read_u64 ("min_fee_mojos",        cfg.min_fee_mojos);
    read_u64 ("max_fee_mojos",        cfg.max_fee_mojos);
    read_bool("adaptive_enabled",     cfg.adaptive_enabled);
    read_u32 ("fee_window_blocks",    cfg.fee_window_blocks);
    read_u32 ("fee_estimate_target_seconds", cfg.fee_estimate_target_seconds);

    // Validate constraints.
    if (cfg.min_fee_mojos > cfg.max_fee_mojos) {
        throw ConfigError(sec + ".min_fee_mojos ("
                          + std::to_string(cfg.min_fee_mojos)
                          + ") must be <= max_fee_mojos ("
                          + std::to_string(cfg.max_fee_mojos) + ")");
    }
    if (cfg.fee_to_gain_max_ratio < 0.0 || cfg.fee_to_gain_max_ratio > 1.0) {
        throw ConfigError(sec + ".fee_to_gain_max_ratio must be in [0.0, 1.0]; got "
                          + std::to_string(cfg.fee_to_gain_max_ratio));
    }
    if (cfg.cancel_cost_multiplier < 1.0) {
        throw ConfigError(sec + ".cancel_cost_multiplier must be >= 1.0; got "
                          + std::to_string(cfg.cancel_cost_multiplier));
    }
    if (cfg.fee_window_blocks == 0) {
        throw ConfigError(sec + ".fee_window_blocks must be > 0");
    }
    if (cfg.daily_budget_mojos == 0) {
        throw ConfigError(sec + ".daily_budget_mojos must be > 0");
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_inventory_aging -- optional `inventory_aging:` section (T4-09).
// ---------------------------------------------------------------------------
InventoryAgingConfig parse_inventory_aging(const YAML::Node& root)
{
    const std::string sec = "inventory_aging";
    InventoryAgingConfig cfg;  // All fields have sensible defaults.

    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;
    }
    const YAML::Node& node = root[sec];

    auto read_bool = [&](const char* key, bool& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<bool>();
    };
    auto read_u32 = [&](const char* key, uint32_t& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<uint32_t>();
    };
    auto read_dbl = [&](const char* key, double& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<double>();
    };

    read_bool("enabled",                   cfg.enabled);
    read_u32 ("aging_start_blocks",        cfg.aging_start_blocks);
    read_dbl ("max_loss_relax_bps",        cfg.max_loss_relax_bps);
    read_dbl ("relax_rate_bps_per_block",  cfg.relax_rate_bps_per_block);

    // Validate constraints.
    if (cfg.max_loss_relax_bps < 0.0) {
        throw ConfigError(sec + ".max_loss_relax_bps must be >= 0; got "
                          + std::to_string(cfg.max_loss_relax_bps));
    }
    if (cfg.relax_rate_bps_per_block < 0.0) {
        throw ConfigError(sec + ".relax_rate_bps_per_block must be >= 0; got "
                          + std::to_string(cfg.relax_rate_bps_per_block));
    }

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

    // Chia -- mode, hosts and ports; SSL paths and fingerprint are secret.
    out << "[chia]\n"
        << "  mode       = " << to_string(cfg.chia.mode) << "\n"
        << "  verify_ssl = " << (cfg.chia.verify_ssl ? "true" : "false") << "\n"
        << "  full_node  = " << cfg.chia.full_node_host
        << ":" << cfg.chia.full_node_port
        << (cfg.chia.mode == ChiaMode::WalletOnly ? " (not used)" : "")
        << "\n"
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
        << "  rate_limit = " << cfg.dexie.max_requests_per_10s << " req/10s\n"
        << "  claim_rewards = " << (cfg.dexie.claim_rewards ? "ON" : "off") << "\n";

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
        << "  max_hs_cap = " << cfg.strategy.max_half_spread_bps << " bps\n"
        << "  sigma_floor = " << cfg.strategy.sigma_floor << "\n";

    // Risk -- all fields are tuning parameters.
    out << "[risk]\n"
        << "  soft_limit     = " << cfg.risk.soft_limit_pct << "\n"
        << "  hard_limit     = " << cfg.risk.hard_limit_pct << "\n"
        << "  cat_cap        = " << cfg.risk.single_cat_cap_pct << "\n"
        << "  kelly          = " << cfg.risk.kelly_fraction << "\n"
        << "  max_pair       = " << cfg.risk.max_capital_per_pair_pct << "\n"
        << "  max_drawdown   = " << cfg.risk.max_drawdown_pct * 100.0 << "%\n"
        << "  loss_window    = " << cfg.risk.loss_window_blocks << " blocks\n"
        << "  max_window_loss = " << cfg.risk.max_window_loss_bps << " bps"
        << (cfg.risk.max_window_loss_bps == 0.0 ? " (disabled)" : "") << "\n"
        << "  flash_crash    = " << cfg.risk.flash_crash_threshold_pct * 100.0 << "%\n"
        << "  recovery_ph1   = " << cfg.risk.recovery_stable_blocks_phase1 << " blocks\n"
        << "  recovery_ph2   = " << cfg.risk.recovery_stable_blocks_phase2 << " blocks\n"
        << "  recovery_band  = " << cfg.risk.recovery_stability_band_pct * 100.0 << "%\n"
        << "  circuit_break  = " << (cfg.risk.circuit_breaker_enabled ? "ON" : "off")
        << " (limit=" << cfg.risk.circuit_breaker_hard_limit_ratio
        << " age_mult=" << cfg.risk.circuit_breaker_age_multiplier
        << " max_loss=" << cfg.risk.circuit_breaker_max_loss_bps << "bps)\n";

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
        << "  cex_ref_half_spread  = " << cfg.arbitrage.cex_reference_half_spread_bps << "\n"
        << "  crossed_book         = " << (cfg.arbitrage.crossed_book_enabled ? "true" : "false") << "\n"
        << "  crossed_min_edge_bps = " << cfg.arbitrage.crossed_book_min_edge_bps << "\n"
        << "  crossed_max_take_xch = " << cfg.arbitrage.crossed_book_max_take_xch << "\n"
        << "  cancel_worst_to_free = " << (cfg.arbitrage.cancel_worst_to_free ? "true" : "false") << "\n"
        << "  midpoint_recycling   = " << (cfg.arbitrage.midpoint_recycling_enabled ? "true" : "false") << "\n";
    if (cfg.arbitrage.midpoint_recycling_enabled) {
        out << "  midpoint_slack_bps   = " << cfg.arbitrage.midpoint_recycling_band_bps << "\n"
            << "  midpoint_take_range  = ["
            << cfg.arbitrage.midpoint_recycling_min_take_xch << ", "
            << cfg.arbitrage.midpoint_recycling_max_take_xch << "] XCH\n"
            << "  midpoint_edge_bps    = " << cfg.arbitrage.midpoint_recycling_min_expected_edge_bps << "\n"
            << "  midpoint_buffers     = fee " << cfg.arbitrage.midpoint_recycling_fee_buffer_bps
            << ", tox " << cfg.arbitrage.midpoint_recycling_toxicity_buffer_bps
            << ", slip " << cfg.arbitrage.midpoint_recycling_slippage_buffer_bps << " bps\n"
            << "  midpoint_cooldown    = " << cfg.arbitrage.midpoint_recycling_cooldown_blocks << " blocks\n"
            << "  midpoint_caps        = " << cfg.arbitrage.midpoint_recycling_max_takes_per_block
            << "/block, " << cfg.arbitrage.midpoint_recycling_daily_take_xch_cap << " XCH/day\n"
            << "  midpoint_vpin_max    = " << cfg.arbitrage.midpoint_recycling_vpin_max << "\n"
            << "  midpoint_inv_cap     = " << cfg.arbitrage.midpoint_recycling_inventory_ratio_cap << "\n"
            << "  midpoint_cex_ref     = " << (cfg.arbitrage.midpoint_recycling_require_cex_ref ? "required" : "optional") << "\n";
        if (!cfg.arbitrage.midpoint_recycling_pairs.empty()) {
            out << "  midpoint_pairs       = ";
            for (std::size_t index = 0; index < cfg.arbitrage.midpoint_recycling_pairs.size(); ++index) {
                if (index > 0) out << ", ";
                out << cfg.arbitrage.midpoint_recycling_pairs[index];
            }
            out << "\n";
        }
    }
    out
        << "  cross_stable_arb     = " << (cfg.arbitrage.cross_stable_arb_enabled ? "true" : "false") << "\n"
        << "  cross_stable_edge_bps= " << cfg.arbitrage.cross_stable_min_edge_bps << "\n"
        << "  cross_stable_max_xch = " << cfg.arbitrage.cross_stable_max_take_xch << "\n"
        << "  peg_arb              = " << (cfg.arbitrage.peg_arb_enabled ? "true" : "false") << "\n"
        << "  peg_arb_min_edge_bps = " << cfg.arbitrage.peg_arb_min_edge_bps << "\n"
        << "  peg_arb_max_units    = " << cfg.arbitrage.peg_arb_max_take_units << "\n"
        << "  peg_arb_max_inv_ratio= " << cfg.arbitrage.peg_arb_max_inventory_ratio << "\n"
        << "  max_position_size    = " << cfg.arbitrage.max_position_size << "\n"
        << "  min_confidence       = " << cfg.arbitrage.min_confidence_threshold << "\n"
        << "  cex_dex_conf_cap     = " << cfg.arbitrage.cex_dex_confidence_cap << "\n";

    // CoinGecko external price reference -- api_key is secret.
    out << "[coingecko]\n"
        << "  enabled    = " << (cfg.coingecko.enabled ? "true" : "false") << "\n"
        << "  base_url   = " << cfg.coingecko.base_url << "\n"
        << "  coin_ids   = [";
    for (std::size_t i = 0; i < cfg.coingecko.coin_ids.size(); ++i) {
        if (i > 0) out << ", ";
        out << cfg.coingecko.coin_ids[i];
    }
    out << "]\n"
        << "  poll_ms    = " << cfg.coingecko.polling_interval_ms << "\n"
        << "  api_key    = "
        << (cfg.coingecko.api_key.empty() ? "none (free tier)" : "<configured>")
        << "\n";

    // Fees -- operational settings.
    out << "[fees]\n"
        << "  enabled    = " << (cfg.fees.enabled ? "true" : "false") << "\n"
        << "  budget/day = " << cfg.fees.daily_budget_mojos << " mojos\n"
        << "  gain_ratio = " << cfg.fees.fee_to_gain_max_ratio << "\n"
        << "  cancel_mul = " << cfg.fees.cancel_cost_multiplier << "\n"
        << "  min_fee    = " << cfg.fees.min_fee_mojos << " mojos\n"
        << "  max_fee    = " << cfg.fees.max_fee_mojos << " mojos\n"
        << "  adaptive   = " << (cfg.fees.adaptive_enabled ? "true" : "false") << "\n"
        << "  window     = " << cfg.fees.fee_window_blocks << " blocks\n"
        << "  estimate_target = " << cfg.fees.fee_estimate_target_seconds << "s\n";

    // Strategy: new fields.
    out << "  confirm    = " << cfg.strategy.confirmation_depth_blocks << " blocks\n"
        << "  reconcile  = " << cfg.strategy.reconciliation_interval_blocks << " blocks\n"
        << "  batch_offers = " << (cfg.strategy.batch_offers_enabled ? "ON" : "off") << "\n"
        << "  spendable_reserve = " << (cfg.strategy.min_spendable_reserve_pct * 100.0) << "%\n"
        << "  stuck_age  = " << cfg.strategy.stuck_offer_age_blocks << " blocks\n"
        << "  fee_reserve_xch = " << cfg.strategy.fee_reserve_xch << "\n"
        << "  fee_min_spendable = " << cfg.strategy.fee_min_spendable_xch << "\n"
        << "  taker_min_spendable = " << cfg.strategy.taker_min_spendable_xch << "\n"
        << "  block_time_seconds = " << cfg.strategy.block_time_seconds << "\n"
        << "  min_reserve_units = " << cfg.strategy.min_reserve_units << "\n"
        << "  min_offer_size_units = " << cfg.strategy.min_offer_size_units << "\n"
        << "  min_trading_units = " << cfg.strategy.min_trading_units << "\n"
        << "  auto_rebalance = " << (cfg.strategy.auto_rebalance_enabled ? "ON" : "off") << "\n"
        << "  xch_ask_throttle = " << (cfg.strategy.xch_ask_throttle_enabled ? "ON" : "off") << "\n"
        << "  xch_ask_throttle_levels = ["
        << cfg.strategy.xch_ask_throttle_caution_xch << ", "
        << cfg.strategy.xch_ask_throttle_low_xch << ", "
        << cfg.strategy.xch_ask_throttle_critical_xch << "] XCH\n"
        << "  xch_ask_throttle_aggr = " << cfg.strategy.xch_ask_throttle_aggressiveness << "\n"
        << "  arb_reserve_coins = " << cfg.strategy.arb_reserve_coins << "\n"
        << "  cross_pair_skew = " << (cfg.strategy.cross_pair_skew_enabled ? "ON" : "off") << "\n"
        << "  cross_pair_phi  = " << cfg.strategy.cross_pair_skew_phi << "\n";

    // Volatility: new fields.
    out << "  candle_agg = " << cfg.volatility.candle_aggregation_blocks << " blocks\n";

    // Inventory aging -- operational settings.
    out << "[inventory_aging]\n"
        << "  enabled    = " << (cfg.inventory_aging.enabled ? "true" : "false") << "\n"
        << "  start      = " << cfg.inventory_aging.aging_start_blocks << " blocks\n"
        << "  max_relax  = " << cfg.inventory_aging.max_loss_relax_bps << " bps\n"
        << "  rate       = " << cfg.inventory_aging.relax_rate_bps_per_block << " bps/block\n";

    // Market allocator -- dynamic capital allocation.
    out << "[market_allocator]\n"
        << "  enabled     = " << (cfg.market_allocator.enabled ? "ON" : "off") << "\n"
        << "  eval_interval = " << cfg.market_allocator.eval_interval_blocks << " blocks\n"
        << "  min_alloc   = " << (cfg.market_allocator.min_alloc_pct * 100.0) << "%\n"
        << "  max_alloc   = " << (cfg.market_allocator.max_alloc_pct * 100.0) << "%\n"
        << "  hysteresis  = " << cfg.market_allocator.hysteresis_bps << " bps\n"
        << "  smooth_alpha = " << cfg.market_allocator.smooth_alpha << "\n";

    // Recovery mode -- automatic XCH acquisition when balance low.
    out << "[recovery]\n"
        << "  enabled     = " << (cfg.recovery.enabled ? "ON" : "off") << "\n"
        << "  xch_low     = " << cfg.recovery.xch_low_threshold << " XCH\n"
        << "  xch_target  = " << cfg.recovery.xch_recovery_target << " XCH\n"
        << "  max_take    = " << cfg.recovery.max_take_per_block_xch << " XCH/block\n"
        << "  max_premium = " << cfg.recovery.max_premium_bps << " bps\n"
        << "  zero_fee_below = " << cfg.recovery.zero_fee_below_xch << " XCH\n"
        << "  cancel_on_enter = " << (cfg.recovery.cancel_on_enter ? "yes" : "no") << "\n";
    if (!cfg.recovery.pair_allowlist.empty()) {
        out << "  pair_allowlist = ";
        for (std::size_t index = 0; index < cfg.recovery.pair_allowlist.size(); ++index) {
            if (index > 0) out << ", ";
            out << cfg.recovery.pair_allowlist[index];
        }
        out << "\n";
    }

    // Buyer -- dedicated offer-taker flow.
    out << "[buyer]\n"
        << "  enabled     = " << (cfg.buyer.enabled ? "ON" : "off") << "\n";
    if (cfg.buyer.enabled) {
        if (!cfg.buyer.config_path.empty())
            out << "  config_path = " << cfg.buyer.config_path << "\n";
        out << "  fee_budget  = " << cfg.buyer.fee_budget_pct * 100.0 << "%\n"
            << "  cooldown    = " << cfg.buyer.cooldown_blocks << " blocks\n"
            << "  epoch       = " << cfg.buyer.epoch_blocks << " blocks\n"
            << "  vpin_max    = " << cfg.buyer.vpin_max << "\n"
            << "  max_takes   = " << cfg.buyer.max_takes_per_block << "/block\n"
            << "  relist_credit = " << (cfg.buyer.include_relist_credit ? "ON" : "off")
            << " (p_fill=" << cfg.buyer.relist_fill_probability << ")\n"
            << "  pairs       = " << cfg.buyer.pair_rules.size() << " rules\n";
        for (const auto& pr : cfg.buyer.pair_rules) {
            out << "    " << pr.pair_name
                << (pr.enabled ? " [enabled]" : " [disabled]")
                << " side=" << pr.side
                << " slack=" << pr.band_bps << "bps"
                << " edge>=" << pr.min_edge_bps << "bps"
                << " cap=" << pr.daily_cap_units << "/day\n";
        }
    }

    out << "======================================\n";

    // [T7-05] Use spdlog instead of std::cout to respect log-level filtering,
    // rotation, and structured output.
    spdlog::info("{}", out.str());
}

// ---------------------------------------------------------------------------
// parse_market_data -- optional `market_data:` section (T4-05).
//
// Exposes VPIN, OFI, whale detection, competitor detection params in YAML.
// ---------------------------------------------------------------------------
MarketDataSettings parse_market_data(const YAML::Node& root)
{
    const std::string sec = "market_data";
    MarketDataSettings cfg;

    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;
    }
    const YAML::Node& node = root[sec];

    auto read_bool = [&](const char* key, bool& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<bool>();
    };
    auto read_i64 = [&](const char* key, std::int64_t& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<std::int64_t>();
    };
    auto read_dbl = [&](const char* key, double& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<double>();
    };
    auto read_u32 = [&](const char* key, uint32_t& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<uint32_t>();
    };

    // Whale detection.
    read_i64 ("whale_trade_threshold",        cfg.whale_trade_threshold);
    read_dbl ("whale_volume_fraction",         cfg.whale_volume_fraction);
    read_u32 ("whale_window_blocks",           cfg.whale_window_blocks);
    read_dbl ("whale_max_spread_multiplier",   cfg.whale_max_spread_multiplier);

    // VPIN.
    read_dbl ("vpin_bucket_size",              cfg.vpin_bucket_size);
    read_u32 ("vpin_window_buckets",           cfg.vpin_window_buckets);

    // OFI.
    read_u32 ("ofi_window_size",               cfg.ofi_window_size);

    // Competitor detection.
    read_bool("enable_competitor_tracking",    cfg.enable_competitor_tracking);
    read_i64 ("min_competitor_offer_size",     cfg.min_competitor_offer_size);
    read_dbl ("competitor_alert_threshold_bps", cfg.competitor_alert_threshold_bps);

    // Asymmetric spread.
    read_dbl ("asymmetric_skew_factor",        cfg.asymmetric_skew_factor);

    // CEX freshness.
    read_dbl ("cex_freshness_threshold_sec",   cfg.cex_freshness_threshold_sec);

    // Order-book-derived mid-price.
    read_bool("orderbook_mid_enabled",         cfg.orderbook_mid_enabled);
    read_u32 ("orderbook_mid_depth",           cfg.orderbook_mid_depth);

    // Validate ranges.
    if (cfg.whale_volume_fraction < 0.0 || cfg.whale_volume_fraction > 1.0) {
        throw ConfigError(sec + ".whale_volume_fraction must be in [0, 1]");
    }
    if (cfg.whale_max_spread_multiplier < 1.0) {
        throw ConfigError(sec + ".whale_max_spread_multiplier must be >= 1.0");
    }
    if (cfg.vpin_bucket_size <= 0.0) {
        throw ConfigError(sec + ".vpin_bucket_size must be > 0");
    }
    if (cfg.asymmetric_skew_factor < 0.0 || cfg.asymmetric_skew_factor > 1.0) {
        throw ConfigError(sec + ".asymmetric_skew_factor must be in [0, 1]");
    }
    if (cfg.orderbook_mid_depth == 0) {
        throw ConfigError(sec + ".orderbook_mid_depth must be > 0");
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_adverse_selection -- optional `adverse_selection:` section (T4-05).
// ---------------------------------------------------------------------------
AdverseSelectionSettings parse_adverse_selection(const YAML::Node& root)
{
    const std::string sec = "adverse_selection";
    AdverseSelectionSettings cfg;

    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;
    }
    const YAML::Node& node = root[sec];

    auto read_dbl = [&](const char* key, double& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<double>();
    };
    auto read_u32 = [&](const char* key, uint32_t& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<uint32_t>();
    };

    read_dbl ("prior_alpha",        cfg.prior_alpha);
    read_dbl ("prior_beta",         cfg.prior_beta);
    read_u32 ("observation_blocks", cfg.observation_blocks);
    read_dbl ("adverse_threshold",  cfg.adverse_threshold);
    read_u32 ("max_history",        cfg.max_history);
    read_dbl ("decay_factor",       cfg.decay_factor);

    if (cfg.prior_alpha <= 0.0 || cfg.prior_beta <= 0.0) {
        throw ConfigError(sec + ".prior_alpha and .prior_beta must be > 0");
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_market_allocator -- optional `market_allocator:` section.
// ---------------------------------------------------------------------------
MarketAllocatorConfig parse_market_allocator(const YAML::Node& root)
{
    const std::string sec = "market_allocator";
    MarketAllocatorConfig cfg;

    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;
    }
    const YAML::Node& node = root[sec];

    auto read_bool = [&](const char* key, bool& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<bool>();
    };
    auto read_u32 = [&](const char* key, uint32_t& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<uint32_t>();
    };
    auto read_dbl = [&](const char* key, double& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<double>();
    };

    read_bool("enabled",              cfg.enabled);
    read_u32 ("eval_interval_blocks", cfg.eval_interval_blocks);
    read_dbl ("min_alloc_pct",        cfg.min_alloc_pct);
    read_dbl ("max_alloc_pct",        cfg.max_alloc_pct);
    read_dbl ("hysteresis_bps",       cfg.hysteresis_bps);
    read_dbl ("smooth_alpha",         cfg.smooth_alpha);
    read_dbl ("weight_spread",        cfg.weight_spread);
    read_dbl ("weight_volume",        cfg.weight_volume);
    read_dbl ("weight_competition",   cfg.weight_competition);
    read_dbl ("weight_fill_rate",     cfg.weight_fill_rate);
    read_dbl ("weight_tri_arb",       cfg.weight_tri_arb);
    read_dbl ("tri_arb_fee_bps",      cfg.tri_arb_fee_bps);
    read_dbl ("tri_arb_min_edge_bps", cfg.tri_arb_min_edge_bps);

    // Validate.
    if (cfg.min_alloc_pct < 0.0 || cfg.min_alloc_pct > 1.0)
        throw ConfigError(sec + ".min_alloc_pct must be in [0, 1]");
    if (cfg.max_alloc_pct < cfg.min_alloc_pct || cfg.max_alloc_pct > 1.0)
        throw ConfigError(sec + ".max_alloc_pct must be in [min_alloc_pct, 1]");
    if (cfg.smooth_alpha <= 0.0 || cfg.smooth_alpha > 1.0)
        throw ConfigError(sec + ".smooth_alpha must be in (0, 1]");

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_recovery -- optional `recovery:` section for XCH recovery mode.
// ---------------------------------------------------------------------------
RecoveryConfig parse_recovery(const YAML::Node& root)
{
    const std::string sec = "recovery";
    RecoveryConfig cfg;

    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;
    }
    const YAML::Node& node = root[sec];

    auto read_bool = [&](const char* key, bool& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<bool>();
    };
    auto read_dbl = [&](const char* key, double& out) {
        if (node[key] && node[key].IsDefined() && !node[key].IsNull())
            out = node[key].as<double>();
    };

    read_bool("enabled",                cfg.enabled);
    read_dbl ("xch_low_threshold",      cfg.xch_low_threshold);
    read_dbl ("xch_recovery_target",    cfg.xch_recovery_target);
    read_dbl ("max_take_per_block_xch", cfg.max_take_per_block_xch);
    read_dbl ("max_premium_bps",        cfg.max_premium_bps);
    read_bool("cancel_on_enter",        cfg.cancel_on_enter);
    read_dbl ("zero_fee_below_xch",     cfg.zero_fee_below_xch);
    if (node["pair_allowlist"] && node["pair_allowlist"].IsSequence()) {
        cfg.pair_allowlist.clear();
        for (const auto& item : node["pair_allowlist"]) {
            cfg.pair_allowlist.push_back(item.as<std::string>());
        }
    }

    // Validate.
    if (cfg.xch_low_threshold < 0.0)
        throw ConfigError(sec + ".xch_low_threshold must be >= 0");
    if (cfg.xch_recovery_target <= cfg.xch_low_threshold)
        throw ConfigError(sec + ".xch_recovery_target must be > xch_low_threshold");
    if (cfg.max_take_per_block_xch <= 0.0)
        throw ConfigError(sec + ".max_take_per_block_xch must be > 0");
    if (cfg.max_premium_bps < 0.0)
        throw ConfigError(sec + ".max_premium_bps must be >= 0");
    if (cfg.zero_fee_below_xch < 0.0)
        throw ConfigError(sec + ".zero_fee_below_xch must be >= 0");
    for (const auto& pair_name : cfg.pair_allowlist) {
        if (pair_name.empty())
            throw ConfigError(sec + ".pair_allowlist entries must be non-empty");
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_buyer -- optional `buyer:` section or external buyer.yaml.
//
// The buyer config can be:
//   1. Inline in config.yaml under `buyer:` key.
//   2. In a separate file specified by `buyer.config_path`.
//   3. Absent / disabled (returns defaults with enabled=false).
// ---------------------------------------------------------------------------
BuyerConfig parse_buyer(const YAML::Node& root)
{
    const std::string sec = "buyer";
    BuyerConfig cfg;

    if (!root[sec] || !root[sec].IsMap()) {
        return cfg;  // disabled by default
    }
    const YAML::Node& node = root[sec];

    // Helper lambdas matching the pattern used throughout config.cpp.
    auto read_bool = [&](const YAML::Node& n, const char* key, bool& out) {
        if (n[key] && n[key].IsDefined() && !n[key].IsNull())
            out = n[key].as<bool>();
    };
    auto read_dbl = [&](const YAML::Node& n, const char* key, double& out) {
        if (n[key] && n[key].IsDefined() && !n[key].IsNull())
            out = n[key].as<double>();
    };
    auto read_u32 = [&](const YAML::Node& n, const char* key, uint32_t& out) {
        if (n[key] && n[key].IsDefined() && !n[key].IsNull())
            out = n[key].as<uint32_t>();
    };
    auto read_str = [&](const YAML::Node& n, const char* key, std::string& out) {
        if (n[key] && n[key].IsDefined() && !n[key].IsNull())
            out = n[key].as<std::string>();
    };

    // Check if an external buyer.yaml is specified.
    read_str(node, "config_path", cfg.config_path);

    // If external file specified, load and deep-merge it over the inline
    // node. Accept either a bare buyer section body or a wrapped top-level
    // `buyer:` map for operator convenience.
    YAML::Node buyer_root = YAML::Clone(node);
    if (!cfg.config_path.empty()) {
        try {
            YAML::Node external = YAML::LoadFile(cfg.config_path);
            if (external.IsMap()) {
                const YAML::Node external_body =
                    (external[sec] && external[sec].IsMap())
                        ? external[sec]
                        : external;

                for (auto it = external_body.begin(); it != external_body.end(); ++it) {
                    const auto key = it->first.as<std::string>();
                    buyer_root[key] = YAML::Clone(it->second);
                }
                spdlog::info("Buyer config loaded from '{}'", cfg.config_path);
            }
        } catch (const YAML::BadFile&) {
            spdlog::warn("Buyer config_path '{}' not found -- using inline defaults",
                         cfg.config_path);
        } catch (const YAML::Exception& e) {
            throw ConfigError("Failed to load buyer config from '" + cfg.config_path
                              + "': " + std::string(e.what()));
        }
    }

    // Parse global settings from the (possibly merged) buyer_root.
    read_bool(buyer_root, "enabled",                cfg.enabled);
    read_dbl (buyer_root, "fee_budget_pct",         cfg.fee_budget_pct);
    read_u32 (buyer_root, "cooldown_blocks",        cfg.cooldown_blocks);
    read_u32 (buyer_root, "epoch_blocks",           cfg.epoch_blocks);
    read_dbl (buyer_root, "vpin_max",               cfg.vpin_max);
    read_dbl (buyer_root, "slippage_buffer_bps",    cfg.slippage_buffer_bps);
    read_dbl (buyer_root, "toxicity_buffer_bps",    cfg.toxicity_buffer_bps);
    read_bool(buyer_root, "require_cex_ref",        cfg.require_cex_ref);
    read_u32 (buyer_root, "max_cex_age_blocks",     cfg.max_cex_age_blocks);
    read_u32 (buyer_root, "max_takes_per_block",    cfg.max_takes_per_block);
    read_bool(buyer_root, "respect_recovery_mode",  cfg.respect_recovery_mode);
    read_bool(buyer_root, "respect_flash_crash",    cfg.respect_flash_crash);
    read_bool(buyer_root, "include_relist_credit",  cfg.include_relist_credit);
    read_dbl (buyer_root, "relist_fill_probability", cfg.relist_fill_probability);

    // Parse per-pair rules. Support both `pair_rules:` and the older `pairs:`
    // spelling, plus both `pair_name` and `name` for the pair key.
    YAML::Node pair_rules;
    if (buyer_root["pair_rules"] && buyer_root["pair_rules"].IsSequence()) {
        pair_rules = buyer_root["pair_rules"];
    } else if (buyer_root["pairs"] && buyer_root["pairs"].IsSequence()) {
        pair_rules = buyer_root["pairs"];
    }

    if (pair_rules && pair_rules.IsSequence()) {
        for (const auto& pnode : pair_rules) {
            BuyerPairRule rule;
            read_str (pnode, "name",                   rule.pair_name);
            if (rule.pair_name.empty()) {
                read_str(pnode, "pair_name",          rule.pair_name);
            }
            read_bool(pnode, "enabled",                rule.enabled);
            read_str (pnode, "side",                   rule.side);
            read_dbl (pnode, "band_bps",               rule.band_bps);
            read_dbl (pnode, "min_edge_bps",           rule.min_edge_bps);
            read_dbl (pnode, "max_take_units",         rule.max_take_units);
            read_dbl (pnode, "min_take_units",         rule.min_take_units);
            read_dbl (pnode, "daily_cap_units",        rule.daily_cap_units);
            read_dbl (pnode, "max_premium_over_cex_bps", rule.max_premium_over_cex_bps);
            read_dbl (pnode, "inventory_ratio_cap",    rule.inventory_ratio_cap);

            if (rule.pair_name.empty()) {
                throw ConfigError(sec + ".pair_rules[]: 'pair_name' (or legacy 'name') is required");
            }
            if (rule.side != "ask" && rule.side != "bid") {
                throw ConfigError(sec + ".pair_rules[" + rule.pair_name
                                  + "].side must be 'ask' or 'bid'");
            }
            if (rule.min_take_units >= rule.max_take_units) {
                throw ConfigError(sec + ".pair_rules[" + rule.pair_name
                                  + "]: min_take_units must be < max_take_units");
            }
            if (rule.inventory_ratio_cap <= 0.0 || rule.inventory_ratio_cap > 1.0) {
                throw ConfigError(sec + ".pair_rules[" + rule.pair_name
                                  + "]: inventory_ratio_cap must be in (0, 1]");
            }
            if (rule.band_bps <= 0.0 || rule.min_edge_bps < 0.0) {
                throw ConfigError(sec + ".pair_rules[" + rule.pair_name
                                  + "]: band_bps must be > 0 and min_edge_bps must be >= 0");
            }
            if (rule.daily_cap_units <= 0.0) {
                throw ConfigError(sec + ".pair_rules[" + rule.pair_name
                                  + "]: daily_cap_units must be > 0");
            }
            cfg.pair_rules.push_back(std::move(rule));
        }
    }

    // Validate global settings.
    if (cfg.enabled && cfg.pair_rules.empty()) {
        throw ConfigError(sec + ": enabled=true but no pair rules defined");
    }
    if (cfg.fee_budget_pct < 0.0 || cfg.fee_budget_pct > 1.0) {
        throw ConfigError(sec + ".fee_budget_pct must be in [0, 1]");
    }
    if (cfg.epoch_blocks < 100) {
        throw ConfigError(sec + ".epoch_blocks must be >= 100");
    }
    if (cfg.max_takes_per_block == 0) {
        throw ConfigError(sec + ".max_takes_per_block must be >= 1");
    }
    if (cfg.vpin_max <= 0.0 || cfg.vpin_max > 1.0) {
        throw ConfigError(sec + ".vpin_max must be in (0, 1]");
    }
    if (cfg.relist_fill_probability < 0.0 || cfg.relist_fill_probability > 1.0) {
        throw ConfigError(sec + ".relist_fill_probability must be in [0, 1]");
    }
    if (cfg.slippage_buffer_bps < 0.0 || cfg.toxicity_buffer_bps < 0.0) {
        throw ConfigError(sec + ".slippage_buffer_bps and .toxicity_buffer_bps must be >= 0");
    }

    return cfg;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// YAML deep-merge: recursively overlay `src` onto `dst` (maps merge, scalars
// and sequences in `src` replace `dst`).  Used to layer secrets.yaml onto
// the main config tree.
// ---------------------------------------------------------------------------
static void deep_merge(YAML::Node dst, const YAML::Node& src)
{
    if (!src.IsMap()) return;
    for (auto it = src.begin(); it != src.end(); ++it) {
        const auto key = it->first.as<std::string>();
        if (it->second.IsMap() && dst[key] && dst[key].IsMap()) {
            deep_merge(dst[key], it->second);
        } else {
            dst[key] = YAML::Clone(it->second);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AppConfig load_config(const std::string& path,
                      const std::string& secrets_path)
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

    // Layer secrets on top of the base config (deep merge).
    if (!secrets_path.empty()) {
        try {
            YAML::Node secrets = YAML::LoadFile(secrets_path);
            if (secrets.IsMap()) {
                deep_merge(root, secrets);
                spdlog::info("Merged secrets from '{}'", secrets_path);
            }
        } catch (const YAML::Exception& e) {
            throw ConfigError("Failed to load secrets from '" + secrets_path
                              + "': " + std::string(e.what()));
        }
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
    cfg.coingecko  = parse_coingecko(root);
    cfg.fees       = parse_fees(root);
    cfg.inventory_aging = parse_inventory_aging(root);
    cfg.market_data = parse_market_data(root);
    cfg.adverse_selection = parse_adverse_selection(root);
    cfg.market_allocator = parse_market_allocator(root);
    cfg.recovery   = parse_recovery(root);
    cfg.buyer      = parse_buyer(root);

    // Emit a redacted summary so operators can verify the loaded parameters
    // without exposing secrets in log files.
    log_config_summary(cfg);

    return cfg;
}

} // namespace xop
