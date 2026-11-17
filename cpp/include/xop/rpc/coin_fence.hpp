// coin_fence.hpp -- [OPTIONS-FENCE] Coins no engine spend may select.
//
// The options book (gui/services/options) can sign from the trading wallet
// (options.wallet.share_trading_wallet).  Its working funds sit at one
// address of that wallet, the options sub-account: collateral headroom, fees,
// a cancel's seed coin, premiums, returned collateral.  The wallet picks the
// coins for every engine spend from the WHOLE wallet, so on 2026-11-15 the
// engine's offers took the coins the book was counting on within minutes.
//
// With options.wallet.engine_fence the engine fences that sub-account off:
//
//   * which coins -- every unspent coin whose puzzle hash is the options
//     address's, or a traded CAT's wrapping of it (CoinManager::
//     cat_puzzle_hash).  Listed from the full node by puzzle hash, so coins
//     locked in an offer and coins of wallets the engine never opened are
//     covered alike;
//   * where -- ChiaWalletRPC, the one class every engine wallet call goes
//     through.  Each call that makes the wallet select coins (offers, takes,
//     cancels that pay a fee, sends, splits, select_coins) carries the list
//     as excluded_coin_ids, the top-level coin-selection config chia 2.7.4
//     reads on every tx endpoint (TXConfigLoader);
//   * when -- refreshed at the point of use once the list is older than
//     max_age, through the refresher the engine installs;
//   * fail-closed -- a fence that is on but whose list cannot be had (never
//     refreshed, refresh failed, stale) makes those calls throw
//     CoinFenceUnavailable instead of spending: an unknown fence is never
//     read as an empty one.
//
// A coin can reach the address between a refresh and a spend (a premium,
// change from the book).  ChiaWalletRPC::create_offer therefore also reads
// back the coins the new offer locked (trade_record.coins_of_interest) and
// cancels the offer locally -- before it is ever posted -- when one of them
// is fenced (fenced_coins_in_trade_record).
//
// Everything that decides is pure and in this header, unit-tested in
// tests/test_coin_fence.cpp.

#ifndef XOP_RPC_COIN_FENCE_HPP
#define XOP_RPC_COIN_FENCE_HPP

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <nlohmann/json.hpp>

namespace xop::rpc {

/// Lowercase hex without a 0x prefix; anything else unchanged in case.
[[nodiscard]] inline std::string fence_normalise_hex(std::string_view hex)
{
    if (hex.size() >= 2U && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex.remove_prefix(2);
    }
    std::string out(hex);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// ---------------------------------------------------------------------------
// bech32m (BIP-350) -- a Chia address to its puzzle hash
// ---------------------------------------------------------------------------

namespace detail {

inline std::uint32_t bech32_polymod(const std::vector<std::uint8_t>& values)
{
    static constexpr std::array<std::uint32_t, 5> kGen{
        0x3b6a57b2U, 0x26508e6dU, 0x1ea119faU, 0x3d4233ddU, 0x2a1462b3U};
    std::uint32_t chk = 1;
    for (const std::uint8_t v : values) {
        const std::uint32_t top = chk >> 25U;
        chk = ((chk & 0x1ffffffU) << 5U) ^ v;
        for (std::size_t i = 0; i < kGen.size(); ++i) {
            if (((top >> i) & 1U) != 0U) chk ^= kGen.at(i);
        }
    }
    return chk;
}

}  // namespace detail

/// The 32-byte puzzle hash an address encodes, as 64 lowercase hex digits, or
/// std::nullopt for anything that is not a valid bech32m address with the
/// prefix `hrp` ("xch" on mainnet, "txch" on testnet11) and 32 bytes of data.
[[nodiscard]] inline std::optional<std::string> decode_chia_address(
    std::string_view address, std::string_view hrp)
{
    static constexpr std::string_view kCharset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    static constexpr std::uint32_t    kBech32mConst = 0x2bc830a3U;

    std::string lower(address);
    bool has_upper = false;
    bool has_lower = false;
    for (char& c : lower) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 33U || uc > 126U) return std::nullopt;
        if (std::isupper(uc) != 0) has_upper = true;
        if (std::islower(uc) != 0) has_lower = true;
        c = static_cast<char>(std::tolower(uc));
    }
    if (has_upper && has_lower) return std::nullopt;
    const std::size_t sep = lower.rfind('1');
    if (sep == std::string::npos || sep == 0U || sep + 7U > lower.size()) return std::nullopt;
    if (std::string_view(lower).substr(0, sep) != hrp) return std::nullopt;

    std::vector<std::uint8_t> data;
    data.reserve(lower.size() - sep - 1U);
    for (std::size_t i = sep + 1U; i < lower.size(); ++i) {
        const std::size_t pos = kCharset.find(lower[i]);
        if (pos == std::string_view::npos) return std::nullopt;
        data.push_back(static_cast<std::uint8_t>(pos));
    }

    std::vector<std::uint8_t> check;
    check.reserve(hrp.size() * 2U + 1U + data.size());
    for (const char c : hrp) check.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(c) >> 5U));
    check.push_back(0);
    for (const char c : hrp) check.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(c) & 31U));
    check.insert(check.end(), data.begin(), data.end());
    if (detail::bech32_polymod(check) != kBech32mConst) return std::nullopt;

    // 5-bit groups (checksum dropped) to bytes, no padding allowed.
    data.resize(data.size() - 6U);
    std::vector<std::uint8_t> bytes;
    std::uint32_t acc = 0;
    unsigned bits = 0;
    for (const std::uint8_t v : data) {
        acc = (acc << 5U) | v;
        bits += 5U;
        while (bits >= 8U) {
            bits -= 8U;
            bytes.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xffU));
        }
    }
    if (bits >= 5U || ((acc << (8U - bits)) & 0xffU) != 0U) return std::nullopt;
    if (bytes.size() != 32U) return std::nullopt;

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4U]);
        out.push_back(kHex[b & 0x0fU]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Pure decisions
// ---------------------------------------------------------------------------

/// `base` with every id of `fence` appended that it does not already hold,
/// order kept: the exclusion list a fenced call sends.
[[nodiscard]] inline std::vector<std::string> merge_exclusions(
    std::vector<std::string> base, const std::vector<std::string>& fence)
{
    std::set<std::string> seen;
    for (const auto& id : base) seen.insert(fence_normalise_hex(id));
    for (const auto& id : fence) {
        if (seen.insert(fence_normalise_hex(id)).second) base.push_back(id);
    }
    return base;
}

/// The coins of a create_offer_for_ids answer's trade record that sit at a
/// fenced puzzle hash, as "0x<parent>/<amount>" descriptions; empty when
/// none.  std::nullopt when the record does not list its coins at all: the
/// caller cannot tell, and a fenced caller treats that as a hit.
[[nodiscard]] inline std::optional<std::vector<std::string>> fenced_coins_in_trade_record(
    const nlohmann::json& create_response, const std::set<std::string>& fenced_puzzle_hashes)
{
    if (!create_response.is_object()) return std::nullopt;
    const auto tr = create_response.find("trade_record");
    if (tr == create_response.end() || !tr->is_object()) return std::nullopt;
    const auto coins = tr->find("coins_of_interest");
    if (coins == tr->end() || !coins->is_array()) return std::nullopt;
    std::vector<std::string> hits;
    for (const auto& coin : *coins) {
        if (!coin.is_object() || !coin.contains("puzzle_hash") || !coin["puzzle_hash"].is_string()) {
            return std::nullopt;
        }
        if (fenced_puzzle_hashes.count(fence_normalise_hex(coin["puzzle_hash"].get<std::string>())) == 0U) {
            continue;
        }
        std::string what = coin.value("parent_coin_info", std::string{"?"});
        what += "/";
        what += coin.contains("amount") ? coin["amount"].dump() : std::string{"?"};
        hits.push_back(std::move(what));
    }
    return hits;
}

// ---------------------------------------------------------------------------
// CoinFence
// ---------------------------------------------------------------------------

/// One unspent coin of the sub-account.
struct FencedCoin {
    std::string   id;           ///< Coin name, hex.
    std::string   puzzle_hash;  ///< hex
    std::uint64_t amount{0};
};

class CoinFence {
public:
    using Clock = std::chrono::steady_clock;
    /// Lists the fenced coins afresh; throws on failure.
    using Refresher = std::function<boost::asio::awaitable<std::vector<FencedCoin>>()>;

    struct Snapshot {
        bool                     enabled{false};
        bool                     fresh{false};
        std::size_t              coin_count{0};
        double                   age_seconds{-1.0};  ///< -1 = never refreshed.
        std::string              last_error;
    };

    /// Off until configured: an engine without the switch fences nothing.
    CoinFence() = default;

    /// Turn the fence on for `puzzle_hashes` (hex, 0x optional).  An empty
    /// list is still ON -- a fence whose address could not be read fences
    /// everything by refusing to spend.
    void configure(const std::vector<std::string>& puzzle_hashes, std::chrono::seconds max_age)
    {
        std::lock_guard lock(mtx_);
        enabled_ = true;
        max_age_ = max_age;
        puzzle_hashes_.clear();
        asset_of_.clear();
        for (const auto& ph : puzzle_hashes) puzzle_hashes_.insert(fence_normalise_hex(ph));
        refreshed_at_.reset();
        coins_.clear();
    }

    /// Which asset each fenced puzzle hash holds ("xch", or a CAT's asset id),
    /// for held(). A puzzle hash with no asset counts toward none.
    void set_assets(const std::vector<std::pair<std::string, std::string>>& puzzle_hash_to_asset)
    {
        std::lock_guard lock(mtx_);
        for (const auto& [ph, asset] : puzzle_hash_to_asset) {
            asset_of_.emplace_back(fence_normalise_hex(ph), fence_normalise_hex(asset));
        }
    }

    void set_refresher(Refresher refresher)
    {
        std::lock_guard lock(mtx_);
        refresher_ = std::move(refresher);
    }

    [[nodiscard]] bool enabled() const
    {
        std::lock_guard lock(mtx_);
        return enabled_;
    }

    [[nodiscard]] std::set<std::string> puzzle_hashes() const
    {
        std::lock_guard lock(mtx_);
        return puzzle_hashes_;
    }

    void update(std::vector<FencedCoin> coins, Clock::time_point now = Clock::now())
    {
        std::lock_guard lock(mtx_);
        coins_ = std::move(coins);
        refreshed_at_ = now;
        last_error_.clear();
    }

    /// The list could not be refreshed: until the next successful update the
    /// fence is unknown, whatever it held before.
    void mark_failed(std::string error)
    {
        std::lock_guard lock(mtx_);
        refreshed_at_.reset();
        last_error_ = std::move(error);
    }

    /// The coins every coin-selecting call must exclude.  Empty for a fence
    /// that is off; std::nullopt for one that is on but unknown or stale --
    /// the call must not go out.
    [[nodiscard]] std::optional<std::vector<std::string>> exclusions(
        Clock::time_point now = Clock::now()) const
    {
        std::lock_guard lock(mtx_);
        if (!enabled_) return std::vector<std::string>{};
        if (!known(now)) return std::nullopt;
        std::vector<std::string> ids;
        ids.reserve(coins_.size());
        for (const auto& c : coins_) ids.push_back(c.id);
        return ids;
    }

    [[nodiscard]] bool is_fenced_coin(std::string_view coin_id) const
    {
        const std::string id = fence_normalise_hex(coin_id);
        std::lock_guard lock(mtx_);
        return std::any_of(coins_.begin(), coins_.end(),
                           [&id](const FencedCoin& c) { return fence_normalise_hex(c.id) == id; });
    }

    /// [Stage 2] The sub-account's holding of `asset` ("xch" or an asset id):
    /// its amount and coin count.  {0, 0} for a fence that is off;
    /// std::nullopt for one that is on but unknown -- a balance must then
    /// not be reported at all.
    [[nodiscard]] std::optional<std::pair<std::uint64_t, std::size_t>> held(
        std::string_view asset, Clock::time_point now = Clock::now()) const
    {
        const std::string want = fence_normalise_hex(asset);
        std::lock_guard lock(mtx_);
        if (!enabled_) return std::pair<std::uint64_t, std::size_t>{0, 0};
        if (!known(now)) return std::nullopt;
        std::uint64_t amount = 0;
        std::size_t   count  = 0;
        for (const auto& c : coins_) {
            const std::string ph = fence_normalise_hex(c.puzzle_hash);
            const bool match = std::any_of(asset_of_.begin(), asset_of_.end(),
                                           [&](const auto& pa) { return pa.first == ph && pa.second == want; });
            if (match) {
                amount += c.amount;
                ++count;
            }
        }
        return std::pair<std::uint64_t, std::size_t>{amount, count};
    }

    /// Refresh through the refresher when the list is missing or older than
    /// max_age.  Never throws: a failed refresh leaves the fence unknown.
    boost::asio::awaitable<void> ensure_fresh()
    {
        Refresher refresher;
        {
            std::lock_guard lock(mtx_);
            if (!enabled_) co_return;
            if (refreshed_at_ && Clock::now() - *refreshed_at_ <= max_age_) co_return;
            refresher = refresher_;
        }
        if (!refresher) {
            mark_failed("no refresher installed");
            co_return;
        }
        try {
            update(co_await refresher());
        } catch (const std::exception& e) {
            mark_failed(e.what());
        }
    }

    [[nodiscard]] Snapshot snapshot(Clock::time_point now = Clock::now()) const
    {
        std::lock_guard lock(mtx_);
        Snapshot s;
        s.enabled = enabled_;
        s.coin_count = coins_.size();
        s.last_error = last_error_;
        if (refreshed_at_) {
            s.age_seconds = std::chrono::duration<double>(now - *refreshed_at_).count();
            s.fresh = !puzzle_hashes_.empty() && now - *refreshed_at_ <= max_age_;
        }
        return s;
    }

private:
    /// Caller holds mtx_.
    [[nodiscard]] bool known(Clock::time_point now) const
    {
        return enabled_ && !puzzle_hashes_.empty() && refreshed_at_ && now - *refreshed_at_ <= max_age_;
    }

    mutable std::mutex                 mtx_;
    bool                               enabled_{false};
    std::chrono::seconds               max_age_{30};
    std::set<std::string>              puzzle_hashes_;
    std::vector<std::pair<std::string, std::string>> asset_of_;
    std::vector<FencedCoin>            coins_;
    std::optional<Clock::time_point>   refreshed_at_;
    std::string                        last_error_;
    Refresher                          refresher_;
};

}  // namespace xop::rpc

#endif  // XOP_RPC_COIN_FENCE_HPP
