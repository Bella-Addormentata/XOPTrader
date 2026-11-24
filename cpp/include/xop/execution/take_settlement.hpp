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
//     each received amount, in that asset's own wallet (receipts_paid).  The
//     maker side has proved its fills the same way since #171 (fill_proof.hpp,
//     prove_take_from_payments): the wallet's status is its own bookkeeping,
//     not evidence.
//   * FAILED or CANCELLED: the take never happened.  Nothing is booked.
//   * Everything else waits: pending, unreadable, or CONFIRMED with the
//     payment not in the store yet.  The takes are read a few per heartbeat,
//     least recently read first, so none waits behind another.  A take still
//     waiting after kTakeSettleGiveUpBlocks stops being tracked, loudly; if it
//     settles after that, the sweep below books it.
//   * While a take of a Dexie offer is unsettled, that offer is not taken
//     again.  Nor is an offer whose take request failed in a way that may
//     have reached the wallet (rpc::request_possibly_submitted), or whose
//     reply named no trade id, for kUncertainTakeHoldBlocks: the wallet may
//     still be taking it.  What such a take could move (UncertainTake) widens
//     the divergence control's tolerance for as long as the hold lasts, so
//     the control does not absorb it before the sweep books it.
//   * Both kinds of hold are written to the database (take_holds) and read
//     back at start, before any take step runs: a restart does not forget an
//     unsettled take or an uncertain request.
//   * THE SWEEP.  A settled take of ours in the wallet's recent trade records
//     that the ledger does not hold and the engine is not tracking -- one
//     whose request timed out, or one taken by hand -- is booked from the
//     wallet's own amounts, after the same payment proof.  The GUI's bridge
//     top-up takes are left to their own ingest (by trade id, and the ledger
//     is asked under both spellings of a take's event id).  Only takes between
//     kTakeSweepGraceBlocks and kTakeSweepLookbackBlocks old: older history is
//     a repair's business, not a heartbeat's.
//
// Pure header: no I/O, no RPC, no logging.
// ---------------------------------------------------------------------------

#include "xop/execution/cancel_escalation.hpp"
#include "xop/execution/fill_proof.hpp"
#include "xop/execution/summary_offer.hpp"
#include "xop/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xop::execution {

/// Heartbeats a take may stay unsettled before it is no longer tracked
/// (~5 h).  A take that settles later is the sweep's.
inline constexpr BlockHeight kTakeSettleGiveUpBlocks{960};

/// How long an offer whose take request may have reached the wallet without
/// an answer is left alone (~20 min): long enough for that take to settle or
/// fail, after which the offer is either gone or takeable again.  Longer
/// than the sweep's grace plus one sweep interval, so the tolerance it adds
/// lasts until the sweep has booked a take that did settle.
inline constexpr BlockHeight kUncertainTakeHoldBlocks{64};

/// Pending takes read per heartbeat, least recently read first: one wallet
/// get_offer each.
inline constexpr std::size_t kTakeSettlePerHeartbeat{4};

/// Blocks a take's payment may land after the trade's confirmed_at_index.
/// The spend and its payment are one block in principle; the wallet's index
/// has been seen one block early (2026-09-04, a maker fill paid at H+1).
inline constexpr std::uint64_t kReceiptGiveBlocks{2};

/// The sweep runs every this many blocks (~3 min) ...
inline constexpr BlockHeight kTakeSweepEveryBlocks{10};
/// ... over this many of the wallet's newest settled takes ...
inline constexpr std::int64_t kTakeSweepWindow{50};
/// ... confirmed at most this long ago (~1 day) ...
inline constexpr BlockHeight kTakeSweepLookbackBlocks{4'608};
/// ... and at least this long ago (~6 min): the settle step books a take it
/// tracks within a heartbeat or two, and the sweep is for the rest.
inline constexpr BlockHeight kTakeSweepGraceBlocks{20};

static_assert(kUncertainTakeHoldBlocks > kTakeSweepGraceBlocks + kTakeSweepEveryBlocks,
              "an uncertain take's tolerance must outlast the sweep that books it");

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
    BlockHeight   last_checked_block{0};  ///< the settle step's last read (0: none)
    /// What the take moves once settled, per asset (fee on xch included):
    /// the divergence control's tolerance while the take is tracked, since a
    /// settled take may wait a few heartbeats to be read and booked.
    std::vector<AssetAmount> exposure;
};

/// A take request that may have reached the wallet without an answer, or
/// whose reply named no trade id: the offer is held back, and what the take
/// could move widens the divergence control's tolerance, until `at` plus
/// kUncertainTakeHoldBlocks.
struct UncertainTake {
    BlockHeight              at{0};
    std::vector<AssetAmount> exposure;   ///< absolute mojos per asset, fee on xch included
};

/// What a take site is about to take: enough to hold the offer back, and to
/// say what the take could move, should the request go unanswered.
struct TakeIntent {
    std::string   strategy;
    std::string   pair_name;
    bool          we_bought_base{false};
    Mojo          base_mojos{0};
    Mojo          price_mojos{0};
    std::uint64_t fee_mojos{0};
};

/// Add a take's network fee to what it could move, on xch, saturating.
inline void add_fee_exposure(UncertainTake& hold, std::uint64_t fee_mojos)
{
    if (fee_mojos == 0U) return;
    constexpr auto kMost = static_cast<std::uint64_t>(std::numeric_limits<Mojo>::max());
    hold.exposure.emplace_back("xch", static_cast<Mojo>(std::min(fee_mojos, kMost)));
}

/// Is an uncertain hold made at `at` still in force at `now`?
[[nodiscard]] constexpr bool uncertain_hold_live(BlockHeight at, BlockHeight now) noexcept
{
    return now >= at && now - at < kUncertainTakeHoldBlocks;
}

/// Add `exposure` into `out`, per asset, saturating.
inline void add_exposure(std::unordered_map<std::string, Mojo>& out,
                         const std::vector<AssetAmount>& exposure)
{
    for (const auto& [asset, mojos] : exposure) {
        if (mojos <= 0) continue;
        Mojo& slot = out[asset];
        slot = (slot > std::numeric_limits<Mojo>::max() - mojos)
            ? std::numeric_limits<Mojo>::max() : slot + mojos;
    }
}

/// Per asset, what the live uncertain holds could have moved.  Saturating.
[[nodiscard]] inline std::unordered_map<std::string, Mojo> uncertain_take_slack(
    const std::unordered_map<std::string, UncertainTake>& uncertain, BlockHeight now)
{
    std::unordered_map<std::string, Mojo> out;
    for (const auto& [offer_id, hold] : uncertain) {
        (void)offer_id;
        if (uncertain_hold_live(hold.at, now)) add_exposure(out, hold.exposure);
    }
    return out;
}

/// The divergence control's extra tolerance from the takes the engine holds:
/// every tracked take until it is booked or dropped, and every live uncertain
/// hold.  Either can have moved the wallet before the ledger books it.
[[nodiscard]] inline std::unordered_map<std::string, Mojo> take_hold_slack(
    const std::unordered_map<std::string, PendingTake>&   pending,
    const std::unordered_map<std::string, UncertainTake>& uncertain,
    BlockHeight                                           now)
{
    auto out = uncertain_take_slack(uncertain, now);
    for (const auto& [trade_id, take] : pending) {
        (void)trade_id;
        add_exposure(out, take.exposure);
    }
    return out;
}

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

/// The wallet id a flat wallet coin record belongs to
/// (`wallet_identifier.id`), or nullopt when it does not say.
[[nodiscard]] inline std::optional<std::int64_t> coin_record_wallet_id(const nlohmann::json& record)
{
    if (!record.is_object()) return std::nullopt;
    const auto ident = record.find("wallet_identifier");
    if (ident == record.end() || !ident->is_object()) return std::nullopt;
    const auto id = ident->find("id");
    if (id == ident->end() || !id->is_number_integer()) return std::nullopt;
    return id->get<std::int64_t>();
}

/// The coin id a flat wallet coin record carries (`id`), normalised, or
/// nullopt when it has none.
[[nodiscard]] inline std::optional<std::string> coin_record_id(const nlohmann::json& record)
{
    if (!record.is_object()) return std::nullopt;
    const auto id = record.find("id");
    if (id == record.end() || !id->is_string()) return std::nullopt;
    std::string out = normalize_id(id->get_ref<const std::string&>());
    if (out.empty()) return std::nullopt;
    return out;
}

/// A take's payment, proven: the block it landed in, and the coins it was.
struct ReceiptProof {
    std::uint64_t            settled_at{0};
    std::vector<std::string> coin_ids;   ///< normalised; claimed by the booking
};

/// When did the take pay us -- or did it?  Every received amount must be
/// matched by its own coin record -- the wallet's flat get_coin_records rows,
/// which span every wallet -- of exactly that amount, IN THAT ASSET'S WALLET
/// (`wallet_ids`: normalised asset id -> wallet id), created from `height` to
/// `height + give`, and not a coin another booked take already claimed
/// (`claimed_elsewhere`, normalised coin ids: the take_receipts table).  The
/// earliest such coin is taken for each receipt.  A 1,000-mojo DBX coin never
/// pays a 1,000-mojo BYC receipt, and one coin never vouches for two receipts
/// -- in this call or, through the claims, in any later one.  Returns the
/// height of the latest receipt matched -- the block the take settled in,
/// which books it -- and the coins, or nullopt.  Fail closed: no receipts,
/// no height, an asset with no wallet, or any record that cannot be read
/// proves nothing.
[[nodiscard]] inline std::optional<ReceiptProof> prove_receipts(
    const std::vector<AssetAmount>&                      received,
    const std::vector<nlohmann::json>&                   records,
    std::uint64_t                                        height,
    const std::unordered_map<std::string, std::int64_t>& wallet_ids,
    std::uint64_t                                        give,
    const std::unordered_set<std::string>&               claimed_elsewhere)
{
    if (received.empty() || height == 0U) return std::nullopt;
    // (created height, wallet id, amount, coin id), earliest first.
    std::vector<std::tuple<std::uint64_t, std::int64_t, std::uint64_t, std::string>> coins;
    coins.reserve(records.size());
    for (const auto& record : records) {
        const auto ref     = parse_coin_ref(record);
        const auto created = coin_record_confirmed_height(record);
        const auto wallet  = coin_record_wallet_id(record);
        const auto id      = coin_record_id(record);
        if (!ref || !created || !wallet || !id) return std::nullopt;
        if (*created >= height && *created - height <= give && claimed_elsewhere.count(*id) == 0) {
            coins.emplace_back(*created, *wallet, ref->amount, *id);
        }
    }
    std::sort(coins.begin(), coins.end());
    ReceiptProof proof;
    for (const auto& r : received) {
        if (r.second <= 0) return std::nullopt;
        const auto wid = wallet_ids.find(r.first);
        if (wid == wallet_ids.end() || wid->second <= 0) return std::nullopt;
        const auto want = static_cast<std::uint64_t>(r.second);
        const auto it = std::find_if(coins.begin(), coins.end(), [&](const auto& c) {
            return std::get<1>(c) == wid->second && std::get<2>(c) == want;
        });
        if (it == coins.end()) return std::nullopt;
        proof.settled_at = std::max(proof.settled_at, std::get<0>(*it));
        proof.coin_ids.push_back(std::get<3>(*it));
        coins.erase(it);
    }
    return proof;
}

/// prove_receipts with no claims to honour: the settlement height only.
[[nodiscard]] inline std::optional<std::uint64_t> receipts_settled_at(
    const std::vector<AssetAmount>&                      received,
    const std::vector<nlohmann::json>&                   records,
    std::uint64_t                                        height,
    const std::unordered_map<std::string, std::int64_t>& wallet_ids,
    std::uint64_t                                        give = 0)
{
    const auto proof = prove_receipts(received, records, height, wallet_ids, give, {});
    if (!proof) return std::nullopt;
    return proof->settled_at;
}

/// The coin ids `records` name, normalised (unreadable ones left out): what
/// to ask take_receipts about before a proof.
[[nodiscard]] inline std::vector<std::string> record_coin_ids(const std::vector<nlohmann::json>& records)
{
    std::vector<std::string> out;
    out.reserve(records.size());
    for (const auto& record : records) {
        if (auto id = coin_record_id(record)) out.push_back(std::move(*id));
    }
    return out;
}

/// The coins claimed by a take other than `trade_id` (owners: coin id ->
/// owning trade id, both normalised).
[[nodiscard]] inline std::unordered_set<std::string> claimed_by_others(
    const std::unordered_map<std::string, std::string>& owners, std::string_view trade_id)
{
    const std::string self = normalize_id(trade_id);
    std::unordered_set<std::string> out;
    for (const auto& [coin, owner] : owners) {
        if (normalize_id(owner) != self) out.insert(coin);
    }
    return out;
}

/// Did the take pay us at exactly `height`?  receipts_settled_at with no give.
[[nodiscard]] inline bool receipts_paid(const std::vector<AssetAmount>&                      received,
                                        const std::vector<nlohmann::json>&                   records,
                                        std::uint64_t                                        height,
                                        const std::unordered_map<std::string, std::int64_t>& wallet_ids)
{
    return receipts_settled_at(received, records, height, wallet_ids, 0).has_value();
}

/// Both spellings a take's ledger event id has: the engine writes
/// "take:" + the wallet's id ("0x..."), the GUI's top-up ingest "take:" +
/// the bare lowercase id (accounting/topup_take.hpp).  Ask the ledger both.
[[nodiscard]] inline std::array<std::string, 2> take_event_ids(std::string_view trade_id)
{
    const std::string bare = normalize_id(trade_id);
    return {"take:0x" + bare, "take:" + bare};
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
/// is unsettled (`pending`, keyed by trade id), and not while an uncertain
/// hold on it is live (`uncertain`, keyed by offer id).  An empty offer id is
/// never refused here: there is nothing to match.
[[nodiscard]] inline bool take_refused(
    const std::unordered_map<std::string, PendingTake>&   pending,
    const std::unordered_map<std::string, UncertainTake>& uncertain,
    const std::string&                                    offer_id,
    BlockHeight                                           now)
{
    if (offer_id.empty()) return false;
    for (const auto& [trade_id, take] : pending) {
        (void)trade_id;
        if (take.counterparty_offer_id == offer_id) return true;
    }
    const auto it = uncertain.find(offer_id);
    return it != uncertain.end() && uncertain_hold_live(it->second.at, now);
}

/// Trade ids of the tracked takes to read this heartbeat, at most `limit`:
/// least recently read first, then oldest submission, then id -- so every
/// take is read within a bounded number of heartbeats, whatever the others
/// do (a never-read take, 0, comes first).
[[nodiscard]] inline std::vector<std::string> takes_to_settle(
    const std::unordered_map<std::string, PendingTake>& pending, std::size_t limit)
{
    std::vector<std::tuple<BlockHeight, BlockHeight, std::string>> order;
    order.reserve(pending.size());
    for (const auto& [trade_id, take] : pending) {
        order.emplace_back(take.last_checked_block, take.submitted_block, trade_id);
    }
    std::sort(order.begin(), order.end());
    std::vector<std::string> out;
    for (const auto& entry : order) {
        if (out.size() >= limit) break;
        out.push_back(std::get<2>(entry));
    }
    return out;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_TAKE_SETTLEMENT_HPP
