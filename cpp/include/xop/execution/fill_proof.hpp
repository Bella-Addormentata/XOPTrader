#ifndef XOP_EXECUTION_FILL_PROOF_HPP
#define XOP_EXECUTION_FILL_PROOF_HPP
// ---------------------------------------------------------------------------
// fill_proof.hpp -- is a trade the wallet reports CONFIRMED really a fill?
//
// [FILL-PROOF 2026-09-23] detect_fills booked every offer the wallet reported
// CONFIRMED.  On 2026-09-22 it booked three that were never taken:
//
//   XCH/DBX ask 0xd6a8325c15   XCH/DBX ask 0x83eef9df80   XCH/BYC bid 0xdb63709cb9
//
// Each had lost exactly ONE XCH input to another of the bot's own
// transactions, which spent it paying a 15,000,000-mojo fee (blocks
// 9,324,680, 9,325,004 and 9,325,694; two of them before Dexie had even
// listed the offer).  Every other maker coin of all three is still unspent
// today, and Dexie reports all three cancelled.  After the 2026-09-22 wallet
// resyncs the wallet nevertheless reported them CONFIRMED at exactly those
// heights, and they entered trade_log, the ledger, the inventory tracker and
// State as fills.
//
// Why the wallet can say so (the likely mechanism, read from Chia 2.7.4's
// code): its trade_manager marks a trade CONFIRMED when every coin that its
// OWN inputs -- those it finds in its coin store at that moment -- would
// create exists on-chain.  Mid-resync the store can be missing the untouched
// inputs, and a check over what is left can pass for an offer nobody took
// (over nothing at all, trivially).  The wallet's status is its bookkeeping,
// not evidence.
//
// The rules -- the chain decides, from the trade record's coins_of_interest:
//
//   * A TAKE SPENDS EVERY MAKER COIN, IN ONE BLOCK.  An offer settles as one
//     atomic spend bundle.  All spent at one height is Settled.
//   * ANY MAKER COIN UNSPENT WHILE ANOTHER IS SPENT, or maker coins spent at
//     different heights, is Dead: something else consumed an input and the
//     offer can never be taken.  (A later reuse of the surviving coins spends
//     them at other heights, so "all spent" alone would not do.)
//   * EVERY MAKER COIN UNSPENT is Live: the offer can still be taken, whatever
//     the wallet says.
//   * NO PROOF, NO FILL.  A coin the answer does not cover, a record that
//     cannot be read or matched, or a contradictory one is Unknown -- nothing
//     is booked and the offer stays tracked for the next heartbeat.
//   * A DEAD VERDICT IS ACTED ON ONLY AT CONFIRMATION DEPTH.  Closing an offer
//     is one-way (update_offer_status never reopens a 'cancelled' row), so
//     the earliest spend must be as deep as a fill must be before it books.
//
// NOT DECIDED HERE: where the records come from (the node when the engine
// trusts it, else the wallet, which refuses to answer until synced), and what
// happens to a Dead offer (OfferManager::detect_fills).
//
// Pure header: no I/O, no RPC, no logging.
// ---------------------------------------------------------------------------

#include "xop/execution/cancel_escalation.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xop::execution {

/// What the chain says about a trade the wallet reports CONFIRMED.
enum class FillProof : std::uint8_t {
    Unknown = 0,  ///< no usable answer: book nothing, keep tracking, retry
    Settled,      ///< every maker coin spent, all at one height: a take
    Dead,         ///< an input consumed elsewhere: never taken, never takeable
    Live,         ///< every maker coin unspent: still takeable
};

/// Outcome of prove_fill().
struct FillProofResult {
    FillProof     verdict{FillProof::Unknown};
    std::uint64_t height{0};   ///< Settled: the height; Dead: the EARLIEST spend
    std::size_t   coins{0};    ///< maker coins the answer covered
    std::size_t   unspent{0};  ///< of them, still unspent
};

/// The height a node or wallet coin record was spent at: 0 when unspent,
/// std::nullopt when neither `spent_block_index` nor `spent` is readable or
/// the two disagree (a spent flag with no height, or a height with a flag
/// that says unspent).
[[nodiscard]] inline std::optional<std::uint64_t> coin_record_spent_height(
    const nlohmann::json& record)
{
    if (!record.is_object()) {
        return std::nullopt;
    }
    std::optional<std::uint64_t> height;
    const auto index_it = record.find("spent_block_index");
    if (index_it != record.end()) {
        if (index_it->is_number_unsigned()) {
            height = index_it->get<std::uint64_t>();
        } else if (index_it->is_number_integer()) {
            const auto value = index_it->get<std::int64_t>();
            if (value < 0) {
                return std::nullopt;
            }
            height = static_cast<std::uint64_t>(value);
        } else {
            return std::nullopt;
        }
    }
    const auto flag_it = record.find("spent");
    if (flag_it != record.end()) {
        if (!flag_it->is_boolean()) {
            return std::nullopt;
        }
        const bool spent = flag_it->get<bool>();
        if (!height.has_value()) {
            // A flag without a height: fine only when it says unspent.
            return spent ? std::nullopt : std::optional<std::uint64_t>{0};
        }
        if (spent != (*height > 0U)) {
            return std::nullopt;   // the two fields disagree
        }
    }
    return height;
}

/// What a coin-records answer proves about the offer whose maker coins are
/// `requested_names`.  Each record is matched back to a requested name by
/// hashing its own coin with `name_of` (CoinManager::compute_coin_name behind
/// a catch in the caller; an empty string means "cannot hash"), exactly as
/// classify_coin_records() does -- the answer omits coins it does not find.
template <class NameOf>
[[nodiscard]] FillProofResult prove_fill(
    const std::vector<std::string>&    requested_names,
    const std::vector<nlohmann::json>& records,
    NameOf                             name_of)
{
    FillProofResult result;
    if (requested_names.empty()) {
        return result;
    }
    const std::unordered_set<std::string> wanted(requested_names.begin(),
                                                 requested_names.end());
    std::unordered_map<std::string, std::uint64_t> spent_at;
    for (const auto& record : records) {
        const auto coin_it = record.find("coin");
        if (coin_it == record.end() || !coin_it->is_object()) {
            return result;
        }
        const auto ref = parse_coin_ref(*coin_it);
        const std::string name = ref ? std::string{name_of(*ref)} : std::string{};
        if (name.empty() || wanted.count(name) == 0U) {
            return result;
        }
        const auto height = coin_record_spent_height(record);
        if (!height.has_value()) {
            return result;
        }
        const auto [it, inserted] = spent_at.emplace(name, *height);
        if (!inserted && it->second != *height) {
            return result;   // two records for one coin, and they disagree
        }
    }
    if (spent_at.size() != wanted.size()) {
        return result;       // a maker coin the answer does not cover
    }

    std::uint64_t earliest = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t latest = 0;
    std::size_t unspent = 0;
    for (const auto& [name, height] : spent_at) {
        if (height == 0U) {
            ++unspent;
            continue;
        }
        earliest = height < earliest ? height : earliest;
        latest = height > latest ? height : latest;
    }
    result.coins = spent_at.size();
    result.unspent = unspent;
    if (unspent == spent_at.size()) {
        result.verdict = FillProof::Live;
    } else if (unspent == 0U && earliest == latest) {
        result.verdict = FillProof::Settled;
        result.height = earliest;
    } else {
        result.verdict = FillProof::Dead;
        result.height = earliest;
    }
    return result;
}

/// A Dead verdict is final once its earliest spend is `confirmation_depth`
/// blocks deep -- the depth a fill waits before it books.  Anything else is
/// not a closable Dead verdict.
[[nodiscard]] constexpr bool dead_offer_closable(const FillProofResult& proof,
                                                 std::uint64_t current_block,
                                                 std::uint64_t confirmation_depth) noexcept
{
    return proof.verdict == FillProof::Dead && proof.height > 0U
        && current_block >= proof.height
        && current_block - proof.height >= confirmation_depth;
}

[[nodiscard]] constexpr const char* fill_proof_name(FillProof proof) noexcept
{
    switch (proof) {
        case FillProof::Unknown: return "unknown";
        case FillProof::Settled: return "settled";
        case FillProof::Dead:    return "dead";
        case FillProof::Live:    return "live";
    }
    return "unknown";
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_FILL_PROOF_HPP
