// ---------------------------------------------------------------------------
// coingecko_parse.hpp -- pure parsing for /simple/price, plus the
// derivation of which vs_currencies a config needs.
//
// [PARANCHOR 2026-08-30] Split out of coingecko_client.cpp so the FX
// extraction that non-USD pegs depend on is testable without a network.
// A EUR- or JPY-pegged asset's par anchor needs USD-per-EUR / USD-per-JPY,
// and CoinGecko quotes every listed coin in any fiat it supports -- so the
// cross falls out of the SAME request that already fetches USD prices:
//
//     usd_per_CUR = coin_usd / coin_cur
//
// (USD per coin, divided by CUR per coin.)  CoinGecko derives all fiat
// quotes from one internal USD price, so every coin yields the same cross;
// taking the first coin in coin_ids order makes the choice deterministic.
// ---------------------------------------------------------------------------

#ifndef XOP_RPC_COINGECKO_PARSE_HPP
#define XOP_RPC_COINGECKO_PARSE_HPP

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "xop/peg_registry.hpp"

namespace xop::rpc {

[[nodiscard]] inline std::string ascii_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

[[nodiscard]] inline std::string ascii_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

/// One /simple/price response, digested.
struct ParsedPrices {
    /// coin id -> USD price.  Only finite, positive values survive.
    std::map<std::string, double> usd;

    /// Peg currency (UPPERCASE, e.g. "EUR") -> USD per one unit of it.
    /// Absent when no requested coin carried a usable quote in that
    /// currency -- callers must treat absence as "no FX rate", never 1.0.
    std::map<std::string, double> fx_usd_per;
};

/// The vs_currencies the fetch must request: "usd" first, then the
/// lowercase peg currency of every declared non-USD peg -- sorted and
/// deduplicated so the request URL is stable across runs.  Derived from
/// the registry rather than operator-configured: declaring a EUR peg IS
/// the request for a EURUSD rate.
[[nodiscard]] inline std::vector<std::string> vs_currencies_for(
    const PegRegistry& registry)
{
    std::vector<std::string> extra;
    for (const PeggedAsset* a : registry.all()) {
        std::string cur = ascii_lower(a->peg_currency);
        if (cur.empty() || cur == "usd") continue;
        extra.push_back(std::move(cur));
    }
    std::sort(extra.begin(), extra.end());
    extra.erase(std::unique(extra.begin(), extra.end()), extra.end());

    std::vector<std::string> out{"usd"};
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}

/// Digest a /simple/price response.
///
/// Expected shape: { "chia": {"usd": 1.43, "eur": 1.31}, ... }.  Junk is
/// skipped, never propagated: a missing coin, a non-numeric field, or a
/// non-finite / non-positive price simply leaves its entry absent.
[[nodiscard]] inline ParsedPrices parse_simple_price(
    const nlohmann::json& result,
    const std::vector<std::string>& coin_ids,
    const std::vector<std::string>& vs_currencies)
{
    ParsedPrices out;
    for (const auto& coin_id : coin_ids) {
        const auto coin_it = result.find(coin_id);
        if (coin_it == result.end() || !coin_it->is_object()) continue;

        const auto usd_it = coin_it->find("usd");
        if (usd_it == coin_it->end() || !usd_it->is_number()) continue;
        const double usd = usd_it->get<double>();
        if (!std::isfinite(usd) || !(usd > 0.0)) continue;
        out.usd[coin_id] = usd;

        for (const auto& cur : vs_currencies) {
            if (cur == "usd") continue;
            const std::string key = ascii_upper(cur);
            if (out.fx_usd_per.count(key)) continue;  // first coin wins
            const auto cit = coin_it->find(cur);
            if (cit == coin_it->end() || !cit->is_number()) continue;
            const double in_cur = cit->get<double>();
            if (!std::isfinite(in_cur) || !(in_cur > 0.0)) continue;
            const double fx = usd / in_cur;
            if (std::isfinite(fx) && fx > 0.0) {
                out.fx_usd_per[key] = fx;
            }
        }
    }
    return out;
}

}  // namespace xop::rpc

#endif  // XOP_RPC_COINGECKO_PARSE_HPP
