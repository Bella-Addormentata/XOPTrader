#ifndef XOP_EXECUTION_TAKE_SETTLEMENT_HPP
#define XOP_EXECUTION_TAKE_SETTLEMENT_HPP
// ---------------------------------------------------------------------------
// take_settlement.hpp -- a take is booked when it SETTLES, not when the
//                        wallet accepts it.
//
// [TAKE-SETTLE 2026-11-24] WHAT WAS MEASURED.  A read-only trace of the live
// ledger's ~17.4 XCH gap, every wallet trade record and every coin record of
// the XCH, wUSDC.b, BYC and DBX wallets against ledger_entries, from the
// 2026-09-03 XCH re-tie (where the gap was exactly 0):
//
//   * 13 takes booked since then never settled.  The wallet holds each one
//     FAILED or CANCELLED, and no coin of ours was paid in for any of them:
//     +44.54 XCH in the ledger that never moved.  Seven were Step 9c's on
//     2026-11-20, which lifted the same Dexie offer two or three times
//     (AkA6GJ.. twice, 5Uoyw6.. twice, Dq3TqE.. three times).  Dexie keeps
//     listing an offer while our first take of it waits in the mempool, and
//     only one take of an offer can settle.  Step 9e's take #700 of
//     2026-11-24 repeated #699 on offer 2AHxocA7.. the same way.
//   * Five takes of 2026-11-20 DID settle -- each paid us exactly 5 XCH in
//     its block -- yet have no taker_fills row and no ledger legs: the engine
//     never learned their trade ids.  take_offer kept the transient retry,
//     so a request that timed out was sent again while the first copy was
//     still running in the wallet.  That is the likeliest source; the logs of
//     that day have rotated away.
//
// THE RULES
//
//   * record_taker_fill() -- ledger legs, the taker_fills row, revive flows --
//     runs for a take only once the wallet reports it CONFIRMED *and* its
//     payment to us is in our coin store at that height: one coin of exactly
//     each received amount (receipts_paid).  The maker side has proved its
//     fills the same way since #171 (fill_proof.hpp, prove_take_from_payments):
//     the wallet's status is its own bookkeeping, not evidence.
//   * FAILED or CANCELLED: the take never happened.  Nothing is booked.
//   * Everything else waits: pending, unreadable, or CONFIRMED with the
//     payment not in the store yet.  A take still waiting after
//     kTakeSettleGiveUpBlocks stops being tracked, loudly; if it settles
//     after that, the sweep below books it.
//   * While a take of a Dexie offer is unsettled, that offer is not taken
//     again.  Nor is an offer whose take request failed in a way that may
//     have reached the wallet (rpc::request_possibly_submitted), for
//     kUncertainTakeHoldBlocks: the wallet may still be taking it.
//   * THE SWEEP.  A settled take of ours in the wallet's recent trade records
//     that the ledger does not hold and the engine is not tracking -- one
//     whose request timed out, or one tracked by a process that has since
//     stopped -- is booked from the wallet's own amounts, after the same
//     payment proof.  Only takes between kTakeSweepGraceBlocks and
//     kTakeSweepLookbackBlocks old: the GUI's bridge top-up takes are booked
//     by their own ingest first, and older history is a repair's business,
//     not a heartbeat's.
//
// Pure header: no I/O, no RPC, no logging.
// ---------------------------------------------------------------------------

#include "xop/execution/cancel_escalation.hpp"
#include "xop/execution/fill_proof.hpp"
#include "xop/execution/summary_offer.hpp"
#include "xop/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xop::execution {

/// Heartbeats a take may stay unsettled before it is no longer tracked
/// (~5 h).  A take that settles later is the sweep's.
inline constexpr BlockHeight kTakeSettleGiveUpBlocks{960};

/// How long an offer whose take request may have reached the wallet without
/// an answer is left alone (~20 min): long enough for that take to settle or
/// fail, after which the offer is either gone or takeable again.
inline constexpr BlockHeight kUncertainTakeHoldBlocks{64};

/// Pending takes read per heartbeat, oldest first: one wallet get_offer each.
inline constexpr std::size_t kTakeSettlePerHeartbeat{4};

/// The sweep runs every this many blocks (~3 min) ...
inline constexpr BlockHeight kTakeSweepEveryBlocks{10};
/// ... over this many of the wallet's newest settled takes ...
inline constexpr std::int64_t kTakeSweepWindow{50};
/// ... confirmed at most this long ago (~1 day) ...
inline constexpr BlockHeight kTakeSweepLookbackBlocks{4'608};
/// ... and at least this long ago (~30 min), so a take booked by another
/// writer (the GUI's bridge top-up ingest) is already in the ledger.
inline constexpr BlockHeight kTakeSweepGraceBlocks{100};

/// One asset and an amount of it, in its own mojos.
using AssetAmount = std::pair<std::string, Mojo>;

/// A take the wallet accepted that is not booked yet.
struct PendingTake {
    std::string   strategy;               ///< taker_fills.strategy
    std::string   trade_id;               ///< the wallet's id, as take_offer returned it
    std::string   counterparty_offer_id;  ///< the Dexie offer taken
    std::string   pair_name;
    bool          we_bought_base{false};
    Mojo          base_mojos{0};
    Mojo          price_mojos{0};
    std::uint64_t fee_mojos{0};
    BlockHeight   submitted_block{0};
};

/// What to do with a tracked take this heartbeat.
enum class TakeVerdict : std::uint8_t {
    Wait = 0,  ///< keep tracking, book nothing (the zero value authorises nothing)
    Book,      ///< settled and paid: book it at the confirmation height
    Drop,      ///< failed or cancelled: it never happened, book nothing
    GiveUp,    ///< unsettled past kTakeSettleGiveUpBlocks: stop tracking it
};

[[nodiscard]] constexpr const char* to_string(TakeVerdict v) noexcept
{
    switch (v) {
        case TakeVerdict::Wait:   return "wait";
        case TakeVerdict::Book:   return "book";
        case TakeVerdict::Drop:   return "drop";
        case TakeVerdict::GiveUp: return "give_up";
    }
    return "wait";
}

/// The verdict for a tracked take: from the wallet's state of its trade, the
/// payment proof (receipts_paid), and how long it has been unsettled.
[[nodiscard]] constexpr TakeVerdict judge_take(WalletCancelState state,
                                               bool              payment_proven,
                                               BlockHeight       submitted_block,
                                               BlockHeight       now) noexcept
{
    if (state == WalletCancelState::Confirmed && payment_proven) {
        return TakeVerdict::Book;
    }
    if (state == WalletCancelState::Failed || state == WalletCancelState::Cancelled) {
        return TakeVerdict::Drop;
    }
    if (now > submitted_block && now - submitted_block > kTakeSettleGiveUpBlocks) {
        return TakeVerdict::GiveUp;
    }
    return TakeVerdict::Wait;
}

/// Lowercase, without a leading 0x: how asset ids and trade ids are compared.
[[nodiscard]] inline std::string normalize_id(std::string_view id)
{
    if (id.size() >= 2 && id[0] == '0' && (id[1] == 'x' || id[1] == 'X')) {
        id.remove_prefix(2);
    }
    std::string out(id);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    });
    return out;
}

/// One side of a summary as a sorted list of (normalised asset id, mojos).
/// nullopt when the side is missing, empty, or has an amount that cannot be
/// read or is not positive: a side that reads short must not book short.
[[nodiscard]] inline std::optional<std::vector<AssetAmount>> read_take_side(
    const nlohmann::json& summary, const char* key)
{
    if (!summary.is_object()) return std::nullopt;
    const auto it = summary.find(key);
    if (it == summary.end() || !it->is_object() || it->empty()) return std::nullopt;
    const auto side = summary_side(*it);
    if (side.size() != it->size()) return std::nullopt;   // an unreadable amount
    std::vector<AssetAmount> out;
    out.reserve(side.size());
    for (const auto& [asset, mojos] : side) {
        if (mojos <= 0) return std::nullopt;
        out.emplace_back(normalize_id(asset), mojos);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// A take of ours, as the wallet's trade record states it, from OUR side.
struct WalletTake {
    std::string              trade_id;   ///< as the record writes it
    BlockHeight              height{0};  ///< confirmed_at_index
    std::vector<AssetAmount> received;   ///< what the maker offered
    std::vector<AssetAmount> paid;       ///< what the maker requested
};

/// Read a wallet trade record that is a CONFIRMED take of ours.  For a take
/// (is_my_offer false) Chia summarises the MAKER's offer -- the record's
/// taken_offer -- so what the maker offered is what we received and what the
/// maker requested is what we paid.  nullopt for anything else: one of our
/// own offers, any other status, no confirmation height, an unreadable side.
[[nodiscard]] inline std::optional<WalletTake> read_wallet_take(const nlohmann::json& record)
{
    if (!record.is_object()) return std::nullopt;
    const auto mine = record.find("is_my_offer");
    if (mine == record.end() || !mine->is_boolean() || mine->get<bool>()) return std::nullopt;
    if (wallet_cancel_state_from_record(record) != WalletCancelState::Confirmed) return std::nullopt;
    const std::uint32_t height = confirmed_height_from_record(record, 0U);
    if (height == 0U) return std::nullopt;
    const auto id = record.find("trade_id");
    if (id == record.end() || !id->is_string() || id->get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    const auto summary = record.find("summary");
    if (summary == record.end()) return std::nullopt;
    auto received = read_take_side(*summary, "offered");
    auto paid     = read_take_side(*summary, "requested");
    if (!received || !paid) return std::nullopt;
    WalletTake take;
    take.trade_id = id->get<std::string>();
    take.height   = static_cast<BlockHeight>(height);
    take.received = std::move(*received);
    take.paid     = std::move(*paid);
    return take;
}

/// The amounts to look for in our coin store: one per received amount.
[[nodiscard]] inline std::vector<std::uint64_t> receipt_amounts(
    const std::vector<AssetAmount>& received)
{
    std::vector<std::uint64_t> out;
    out.reserve(received.size());
    for (const auto& r : received) {
        if (r.second > 0) out.push_back(static_cast<std::uint64_t>(r.second));
    }
    return out;
}

/// Did the take pay us?  Every received amount must be matched by its own
/// coin record -- the wallet's flat get_coin_records rows -- of exactly that
/// amount, created at `height`.  One record never vouches for two receipts.
/// Fail closed: no receipts, no height, or any record that cannot be read
/// proves nothing.
[[nodiscard]] inline bool receipts_paid(const std::vector<AssetAmount>&    received,
                                        const std::vector<nlohmann::json>& records,
                                        std::uint64_t                      height)
{
    if (received.empty() || height == 0U) return false;
    std::vector<std::uint64_t> coins;
    coins.reserve(records.size());
    for (const auto& record : records) {
        const auto ref     = parse_coin_ref(record);
        const auto created = coin_record_confirmed_height(record);
        if (!ref || !created) return false;
        if (*created == height) coins.push_back(ref->amount);
    }
    for (const auto& r : received) {
        if (r.second <= 0) return false;
        const auto want = static_cast<std::uint64_t>(r.second);
        const auto it   = std::find(coins.begin(), coins.end(), want);
        if (it == coins.end()) return false;
        coins.erase(it);
    }
    return true;
}

/// A settled take as the books want it on one pair.
struct TakeBooking {
    bool we_bought_base{false};
    Mojo base_mojos{0};
    Mojo quote_mojos{0};
};

/// The legs of a take on the pair (base_asset, quote_asset): exactly one asset
/// received and one paid, which are that pair's two assets.  nullopt for any
/// other take -- a multi-asset offer, or another pair's.
[[nodiscard]] inline std::optional<TakeBooking> take_booking_for_pair(
    const WalletTake& take, std::string_view base_asset, std::string_view quote_asset)
{
    if (take.received.size() != 1 || take.paid.size() != 1) return std::nullopt;
    const std::string base  = normalize_id(base_asset);
    const std::string quote = normalize_id(quote_asset);
    if (base.empty() || quote.empty() || base == quote) return std::nullopt;
    const auto& in  = take.received.front();
    const auto& out = take.paid.front();
    TakeBooking booking;
    if (in.first == base && out.first == quote) {
        booking.we_bought_base = true;
        booking.base_mojos     = in.second;
        booking.quote_mojos    = out.second;
    } else if (in.first == quote && out.first == base) {
        booking.we_bought_base = false;
        booking.base_mojos     = out.second;
        booking.quote_mojos    = in.second;
    } else {
        return std::nullopt;
    }
    return booking;
}

/// Is a take confirmed at `height` inside the sweep's window at `now`?
[[nodiscard]] constexpr bool in_sweep_window(BlockHeight height, BlockHeight now) noexcept
{
    if (height == 0 || height > now) return false;
    const BlockHeight age = now - height;
    return age >= kTakeSweepGraceBlocks && age <= kTakeSweepLookbackBlocks;
}

/// Is the sweep due at `now`, given when it last ran (0: never)?
[[nodiscard]] constexpr bool take_sweep_due(BlockHeight last_run, BlockHeight now) noexcept
{
    return now > 0 && (last_run == 0 || now < last_run || now - last_run >= kTakeSweepEveryBlocks);
}

/// May the Dexie offer `offer_id` be taken at `now`?  Not while a take of it
/// is unsettled (`pending`, keyed by trade id), and not for
/// kUncertainTakeHoldBlocks after a take request for it that may have reached
/// the wallet unanswered (`uncertain`, offer id -> height of that request).
/// An empty offer id is never refused here: there is nothing to match.
[[nodiscard]] inline bool take_refused(
    const std::unordered_map<std::string, PendingTake>& pending,
    const std::unordered_map<std::string, BlockHeight>& uncertain,
    const std::string&                                  offer_id,
    BlockHeight                                         now)
{
    if (offer_id.empty()) return false;
    for (const auto& [trade_id, take] : pending) {
        (void)trade_id;
        if (take.counterparty_offer_id == offer_id) return true;
    }
    const auto it = uncertain.find(offer_id);
    if (it == uncertain.end()) return false;
    return now >= it->second && now - it->second < kUncertainTakeHoldBlocks;
}

/// Trade ids of the tracked takes, oldest submission first (ties by id, so
/// the order is stable), at most `limit` of them.
[[nodiscard]] inline std::vector<std::string> takes_to_settle(
    const std::unordered_map<std::string, PendingTake>& pending, std::size_t limit)
{
    std::vector<std::pair<BlockHeight, std::string>> order;
    order.reserve(pending.size());
    for (const auto& [trade_id, take] : pending) {
        order.emplace_back(take.submitted_block, trade_id);
    }
    std::sort(order.begin(), order.end());
    std::vector<std::string> out;
    for (const auto& entry : order) {
        if (out.size() >= limit) break;
        out.push_back(entry.second);
    }
    return out;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_TAKE_SETTLEMENT_HPP
