// feed_listings.hpp -- the symbol -> CoinGecko listing table, shared.
//
// This table, and the leg canonicalisation that keys into it, used to live
// as function-locals inside Engine::update_fair_values().  config.cpp's
// revive_market validation needs the same answers at load time -- "does
// this pair's leg map to a feed, and is that feed id configured?" -- and a
// duplicated table would drift.  One definition, two consumers.
//
// ISO/IEC 5055: pure functions of their inputs; no I/O, no globals mutated.

#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace xop {

/// Symbol -> external listing.  units_per_coin is how many units of the
/// pair's asset make one unit of the listed coin: wmilliETH is 1/1000 of
/// an ETH, so 1000 pair-units per coin.
struct FeedListing {
    const char* id;             // CoinGecko coin id ("chia", "ethereum"...)
    double      units_per_coin; // pair-units per listed coin
};

/// The known symbol -> listing mappings.  Keys are canonical leg symbols
/// (see canonical_feed_symbol below): lower-case, ".b" bridge suffix
/// stripped.
inline const std::unordered_map<std::string, FeedListing>& feed_listings() {
    static const std::unordered_map<std::string, FeedListing> kFeedListings = {
        {"xch",       {"chia",           1.0}},
        {"usdc",      {"usd-coin",       1.0}},
        {"wusdc",     {"usd-coin",       1.0}},
        {"dbx",       {"dexie-bucks",    1.0}},
        {"eth",       {"ethereum",       1.0}},
        {"weth",      {"ethereum",       1.0}},
        {"millieth",  {"ethereum",    1000.0}},
        {"wmillieth", {"ethereum",    1000.0}},
    };
    return kFeedListings;
}

/// Canonical leg key: lower-cased, with the ".b" bridge suffix removed so
/// that "wUSDC.b" and "wUSDC" resolve to the same underlying asset.
inline std::string canonical_feed_symbol(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".b") == 0) {
        s.erase(s.size() - 2);
    }
    return s;
}

/// Split "BASE/QUOTE" into its two canonical leg keys.
inline std::optional<std::pair<std::string, std::string>>
split_pair_legs(const std::string& pair_name) {
    const auto pos = pair_name.find('/');
    if (pos == std::string::npos || pos == 0
        || pos + 1 >= pair_name.size()) {
        return std::nullopt;
    }
    auto base  = canonical_feed_symbol(pair_name.substr(0, pos));
    auto quote = canonical_feed_symbol(pair_name.substr(pos + 1));
    if (base.empty() || quote.empty() || base == quote) {
        return std::nullopt;
    }
    return std::make_pair(std::move(base), std::move(quote));
}

}  // namespace xop
