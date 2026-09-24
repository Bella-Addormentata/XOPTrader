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
//     atomic spend bundle.  But that is not enough to prove a take: a cancel,
//     or a stray spend of a one-coin offer, does it too.  So all spent at one
//     height is only SpentTogether (prove_fill), and books nothing until the
//     spend block also shows the TAKE'S OWN MARK:
//       - from the node: a settlement coin, created from a maker coin at the
//         offered asset's settlement puzzle, for exactly the amount offered,
//         and spent in that same block (prove_take_from_children);
//       - from the wallet, which cannot see settlement coins: a payment to us
//         of exactly a requested amount in that block, from a coin that is
//         not ours (prove_take_from_payments).
//     With the mark it is Settled.  If the node shows the block without one
//     -- or shows the maker coins created no children at all -- it is Dead.
//     The wallet's silence proves nothing, so there it stays Unknown.
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
// NOT DETECTED:
//   - From the node: another of our offers, built on the same coins, taken for
//     exactly the same offered asset and amount while this one is reported
//     CONFIRMED.  Its settlement coin is indistinguishable here.  Only its
//     requested payment differs, and the node cannot search for that.
//   - From the wallet: an unrelated coin of ours confirmed in the same block
//     for exactly a requested amount -- another fill's payment of the same
//     size, say.  The wallet cannot see a payment's parent, so it cannot tell.
//     This path runs only while the node is not trusted.
//
// Pure header: no I/O, no RPC, no logging.
// ---------------------------------------------------------------------------

#include "xop/execution/cancel_escalation.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xop::execution {

/// What the chain says about a trade the wallet reports CONFIRMED.
enum class FillProof : std::uint8_t {
    Unknown = 0,    ///< no usable answer: book nothing, keep tracking, retry
    Settled,        ///< a take: every maker coin spent in one block, and the
                    ///< block shows the take's own mark (prove_take_from_*)
    Dead,           ///< consumed, but not by a take: never taken, never takeable
    Live,           ///< every maker coin unspent: still takeable
    SpentTogether,  ///< prove_fill() only: every maker coin spent in one block.
                    ///< A take does that, and so does a cancel or a stray spend
                    ///< of a one-coin offer.  Never booked as it stands.
};

/// Outcome of prove_fill() and prove_take_from_*().
struct FillProofResult {
    FillProof     verdict{FillProof::Unknown};
    std::uint64_t height{0};   ///< Settled/SpentTogether: the spend height; Dead: the EARLIEST spend
    std::size_t   coins{0};    ///< maker coins the answer covered
    std::size_t   unspent{0};  ///< of them, still unspent
    bool          spent_together{false};  ///< every maker coin spent at one height
};

/// [review #171, round 3] The highest height a record may carry: the engine
/// narrows every proven height to BlockHeight (std::uint32_t), and a larger
/// value would wrap to an old block -- a fill booked at a height long past
/// its confirmation depth.  cancel_escalation's confirmed_height_from_record()
/// bounds its heights the same way.
inline constexpr std::uint64_t kMaxRecordHeight = std::numeric_limits<std::uint32_t>::max();

/// The height a node or wallet coin record was spent at: 0 when unspent,
/// std::nullopt when neither `spent_block_index` nor `spent` is readable, when
/// the height is past kMaxRecordHeight, or when the two disagree (a spent flag
/// with no height, or a height with a flag that says unspent).
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
        if (height && *height > kMaxRecordHeight) {
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

/// The height a coin record was created at: the node's
/// `confirmed_block_index` or the wallet's `confirmed_height`.  std::nullopt
/// when neither is a readable number from 0 to kMaxRecordHeight.
[[nodiscard]] inline std::optional<std::uint64_t> coin_record_confirmed_height(
    const nlohmann::json& record)
{
    if (!record.is_object()) {
        return std::nullopt;
    }
    for (const char* key : {"confirmed_block_index", "confirmed_height"}) {
        const auto it = record.find(key);
        if (it == record.end()) {
            continue;
        }
        std::uint64_t value = 0;
        if (it->is_number_unsigned()) {
            value = it->get<std::uint64_t>();
        } else if (it->is_number_integer()) {
            const auto signed_value = it->get<std::int64_t>();
            if (signed_value < 0) {
                return std::nullopt;
            }
            value = static_cast<std::uint64_t>(signed_value);
        } else {
            return std::nullopt;
        }
        if (value > kMaxRecordHeight) {
            return std::nullopt;
        }
        return value;
    }
    return std::nullopt;
}

/// The assets and amounts, in mojos, that a trade record's summary lists on
/// one side: `side` is "offered" or "requested".  Each asset is the summary's
/// own key: "xch", or a CAT's asset id (its TAIL hash, hex).  The wallet writes
/// amounts as decimal strings; numbers are read too.  Empty when the summary
/// or the side is missing, or when ANY amount is unreadable or zero -- no
/// amounts, no evidence.
[[nodiscard]] inline std::vector<std::pair<std::string, std::uint64_t>> summary_assets(
    const nlohmann::json& trade_record, const char* side)
{
    if (!trade_record.is_object()) {
        return {};
    }
    const auto summary = trade_record.find("summary");
    if (summary == trade_record.end() || !summary->is_object()) {
        return {};
    }
    const auto listed = summary->find(side);
    if (listed == summary->end() || !listed->is_object() || listed->empty()) {
        return {};
    }
    std::vector<std::pair<std::string, std::uint64_t>> assets;
    for (const auto& [asset, value] : listed->items()) {
        std::uint64_t amount = 0;
        if (value.is_number_unsigned()) {
            amount = value.get<std::uint64_t>();
        } else if (value.is_number_integer()) {
            const auto signed_amount = value.get<std::int64_t>();
            if (signed_amount <= 0) {
                return {};
            }
            amount = static_cast<std::uint64_t>(signed_amount);
        } else if (value.is_string()) {
            const std::string& text = value.get_ref<const std::string&>();
            const char* const end = text.data() + text.size();
            const auto [ptr, ec] = std::from_chars(text.data(), end, amount);
            if (text.empty() || ec != std::errc{} || ptr != end) {
                return {};
            }
        } else {
            return {};
        }
        if (amount == 0U) {
            return {};
        }
        assets.emplace_back(asset, amount);
    }
    return assets;
}

/// summary_assets() without the assets: what the wallet's payment check asks
/// for, since the wallet cannot see a payment's asset from its amount filter.
[[nodiscard]] inline std::vector<std::uint64_t> summary_amounts(
    const nlohmann::json& trade_record, const char* side)
{
    std::vector<std::uint64_t> amounts;
    for (const auto& asset : summary_assets(trade_record, side)) {
        amounts.push_back(asset.second);
    }
    return amounts;
}

/// [review #171, round 5] A settlement coin a take creates for one offered
/// asset: the asset's settlement puzzle hash (lowercase hex, no 0x) and the
/// offered amount.  Both must match -- an amount alone could be any child.
struct SettlementCoin {
    std::string   puzzle_hash_hex{};
    std::uint64_t amount{0};
};

/// The settlement coins a trade record's offered side predicts.  Each asset's
/// settlement puzzle hash comes from `puzzle_of`
/// (CoinManager::settlement_puzzle_hash behind a catch in the caller; an empty
/// string means "cannot name it").  Empty when the summary cannot be read, or
/// when ANY offered asset's settlement puzzle cannot be named -- an offer we
/// cannot fully describe proves nothing.
template <class PuzzleOf>
[[nodiscard]] std::vector<SettlementCoin> offered_settlements(
    const nlohmann::json& trade_record, PuzzleOf puzzle_of)
{
    std::vector<SettlementCoin> settlements;
    for (const auto& [asset, amount] : summary_assets(trade_record, "offered")) {
        std::string puzzle_hash{puzzle_of(asset)};
        if (puzzle_hash.empty()) {
            return {};
        }
        settlements.push_back(SettlementCoin{std::move(puzzle_hash), amount});
    }
    return settlements;
}

/// Stage 1: what a coin-records answer proves about the offer whose maker
/// coins are `requested_names`.  NEVER Settled: every maker coin spent at one
/// height is SpentTogether, for prove_take_from_children() or
/// prove_take_from_payments() to decide.  Each record is matched back to a
/// requested name by hashing its own coin with `name_of`
/// (CoinManager::compute_coin_name behind a catch in the caller; an empty
/// string means "cannot hash"), exactly as classify_coin_records() does --
/// the answer omits coins it does not find.
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
        result.verdict = FillProof::SpentTogether;
        result.height = earliest;
        result.spent_together = true;
    } else {
        result.verdict = FillProof::Dead;
        result.height = earliest;
    }
    return result;
}

/// Stage 2, from the full node: does the spend block show the take's own mark?
/// A take creates a settlement coin from one of the maker coins: at the
/// offered asset's settlement puzzle, for exactly the amount offered.  The
/// taker's half of the bundle spends it in the same block.  A cancel, or a
/// stray spend of a one-coin offer, sends its children to our own addresses,
/// where they outlive the block -- or creates none at all.  Observed
/// 2026-09-23: four real takes (one ask, three bids) each show exactly one
/// such child: 1 XCH at OFFER_MOD's puzzle, or DBX at its CAT wrapping.  The
/// coin consumed from the phantom 0xdb63709cb9 and four confirmed cancels
/// show none.
///
/// `children` is the node's get_coin_records_by_parent_ids over the maker
/// coins, spent coins included -- the node's complete answer, since that
/// wrapper refuses one without a coin_records list.  `settlements` is
/// offered_settlements().  Only a SpentTogether result is examined; every
/// other verdict is returned as it came:
///   - a child created AND spent at the spend height, at a settlement coin's
///     puzzle hash and for its amount                           -> Settled
///   - no settlements, a record that cannot be read, a child of a coin that
///     was not asked about, or one created at another height    -> Unknown
///   - otherwise, NO CHILDREN AT ALL included [review #171, round 5]: spent
///     together, but not by a take                               -> Dead
[[nodiscard]] inline FillProofResult prove_take_from_children(
    const FillProofResult&             spent,
    const std::vector<std::string>&    maker_names,
    const std::vector<nlohmann::json>& children,
    const std::vector<SettlementCoin>& settlements)
{
    if (spent.verdict != FillProof::SpentTogether) {
        return spent;
    }
    const FillProofResult unknown{};
    if (settlements.empty() || spent.height == 0U) {
        return unknown;
    }
    const std::unordered_set<std::string> makers(maker_names.begin(), maker_names.end());
    bool marked = false;
    for (const auto& record : children) {
        const auto coin_it = record.find("coin");
        if (coin_it == record.end()) {
            return unknown;
        }
        const auto ref = parse_coin_ref(*coin_it);
        const auto created = coin_record_confirmed_height(record);
        const auto spent_at = coin_record_spent_height(record);
        if (!ref || !created || !spent_at || makers.count(ref->parent_hex) == 0U
            || *created != spent.height) {
            return unknown;   // not an answer about these coins' spend
        }
        if (*spent_at != spent.height) {
            continue;         // outlived the block: ours, not a settlement
        }
        for (const auto& settlement : settlements) {
            if (ref->puzzle_hash_hex == settlement.puzzle_hash_hex
                && ref->amount == settlement.amount) {
                marked = true;
            }
        }
    }
    FillProofResult result = spent;
    result.verdict = marked ? FillProof::Settled : FillProof::Dead;
    return result;
}

/// Stage 2, from the wallet, which cannot see a settlement coin (it is not
/// ours): did we receive what the offer requested?  A take pays each requested
/// amount to us in the take block, from the taker's settlement coin, never
/// from one of our own maker coins.  Observed 2026-09-23: the real ask
/// 0x202ff7d2d8 shows its 85,094-mojo DBX payment at 9,297,025; the phantom
/// 0xdb63709cb9 shows no payment at 9,325,694.
///
/// `payments` is the wallet's get_coin_records for our coins confirmed at the
/// spend height with a requested amount.  Only a SpentTogether result is
/// examined; every other verdict is returned as it came:
///   - a coin confirmed at the spend height, for a requested amount, whose
///     parent is not a maker coin                              -> Settled
///   - anything else                                            -> Unknown,
///     NEVER Dead: the wallet's store is what was incomplete on 2026-09-22,
///     so its silence proves nothing, and closing an offer is one-way.
[[nodiscard]] inline FillProofResult prove_take_from_payments(
    const FillProofResult&             spent,
    const std::vector<std::string>&    maker_names,
    const std::vector<nlohmann::json>& payments,
    const std::vector<std::uint64_t>&  requested)
{
    if (spent.verdict != FillProof::SpentTogether) {
        return spent;
    }
    const FillProofResult unknown{};
    if (requested.empty() || spent.height == 0U) {
        return unknown;
    }
    const std::unordered_set<std::string> makers(maker_names.begin(), maker_names.end());
    const std::unordered_set<std::uint64_t> amounts(requested.begin(), requested.end());
    for (const auto& record : payments) {
        const auto ref = parse_coin_ref(record);   // wallet records are flat
        const auto created = coin_record_confirmed_height(record);
        if (!ref || !created) {
            return unknown;
        }
        if (*created == spent.height && amounts.count(ref->amount) > 0U
            && makers.count(ref->parent_hex) == 0U) {
            FillProofResult result = spent;
            result.verdict = FillProof::Settled;
            return result;
        }
    }
    return unknown;
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

/// [review #171, round 2] May an offer the wallet reports CONFIRMED be
/// cancelled?  Chia 2.7.4's cancel sets PENDING_CANCEL (secure) or CANCELLED
/// (insecure) over any status, so a cancel erases the CONFIRMED a real take
/// would be booked from.  And Chia sets CONFIRMED only when a spend of one of
/// the offer's coins triggers it: a Live verdict against it means the node
/// lags the wallet -- or a reorganisation undid that spend, and the offer can
/// be taken again.  Only the second may be cancelled, and they are told apart
/// by depth.  So a cancel is allowed only when:
///   - the verdict is Live (every maker coin unspent);
///   - it came from the full node, not the wallet, whose store is what
///     mislabelled these offers;
///   - it was made by `current_call`, the latest detect_fills call.  [round 4]
///     A call, not a block: detect_fills can run more than once at one
///     height, and a proof an earlier call made is not this call's evidence.
///     Call 0 is no call -- an offer held before its first proof;
///   - `proved_block`, the height it was made at, is at least
///     `confirmation_depth` past `claimed_height`, the wallet's
///     confirmed_at_index.  When the node is trusted, the engine's height is
///     the node's own, so the node has processed the claimed block and
///     `confirmation_depth` more without seeing a spend.
/// Anything else stays withheld until the proof settles or closes the offer.
[[nodiscard]] constexpr bool live_offer_cancellable(FillProof     verdict,
                                                    bool          from_node,
                                                    std::uint64_t claimed_height,
                                                    std::uint64_t proved_block,
                                                    std::uint64_t proof_call,
                                                    std::uint64_t current_call,
                                                    std::uint64_t confirmation_depth) noexcept
{
    return verdict == FillProof::Live && from_node && claimed_height > 0U
        && proof_call != 0U && proof_call == current_call
        && proved_block >= claimed_height
        && proved_block - claimed_height >= confirmation_depth;
}

[[nodiscard]] constexpr const char* fill_proof_name(FillProof proof) noexcept
{
    switch (proof) {
        case FillProof::Unknown:       return "unknown";
        case FillProof::Settled:       return "settled";
        case FillProof::Dead:          return "dead";
        case FillProof::Live:          return "live";
        case FillProof::SpentTogether: return "spent together";
    }
    return "unknown";
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_FILL_PROOF_HPP
