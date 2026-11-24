// options_transfer.hpp -- Capital moved between the trading funds and the
// options sub-account, booked as EXTERNAL CAPITAL
// ([OPTIONS-CAPITAL 2026-11-23], operator-approved: Dorkmo).
//
// WHY THIS EXISTS.  The options book (gui/services/options) keeps its working
// funds at one address of the trading wallet, the options sub-account, and
// the engine fences that address off (rpc/coin_fence.hpp).  While the fence
// is up, ChiaWalletRPC::get_wallet_balance takes the sub-account's coins out
// of every balance the engine reads: the engine's balances are the TRADING
// funds.  So when the book tops the sub-account up -- a plain wallet send
// from the trading funds to the options address -- the engine's balance
// drops by the amount (and the fee, in XCH), and nothing said why:
//
//   * the ledger had no entry for it, so the books sat above the wallet by
//     the top-up.  The invariant control tolerates that while the resting
//     offers are large, and once they are not it raises LedgerDivergence and
//     posts an "unexplained" adjust;
//   * Net Deposits did not move, so the equity that left read as
//     performance.  For wUSDC.b the bridge scan reconciles the tracked
//     quantity to the wallet at once; for XCH the tracker kept the old
//     quantity until the next restart's one-shot reconcile.
//
// The live record on 2026-11-23: three top-ups in the options database
// (1.5 XCH and 2 wUSDC.b on 2026-11-20, 0.5 XCH on 2026-11-21, a fee of
// 10,000,000 mojos each) and no ledger entry for any of them.
//
// ACCOUNTING TREATMENT.  The same as a warp bridge flow
// (accounting/bridge_ingest.hpp): capital that crosses the edge of the
// trading book is NOT performance.  It books as a first-class ledger event
// (options_withdrawal out of the trading funds, options_deposit back into
// them) BEFORE the divergence control runs, its USD value goes to NET
// DEPOSITS, and a flow booked in this process re-anchors the drawdown peak
// (restart semantics).
//
// WHAT CROSSES THE EDGE.
//   * A top-up.  The book records it in its own database: an `events` row of
//     kind topup_sent (amount, fee, transaction) written when the send is
//     accepted, and one of kind topup_confirmed when the book next sees the
//     wallet report it confirmed -- minutes later.  The engine reads that
//     table READ-ONLY, as it reads the GUI's warp jobs, for WHICH top-ups
//     there are.  WHETHER one has arrived it asks the wallet itself, by the
//     top-up's own transaction (read_topup_transaction): that record says
//     whether it confirmed and which coin it created, and that coin in the
//     fence's list is arrival too.  The engine's balance is the wallet's
//     less that list, so it falls by a top-up when the list takes its coin
//     in, and that is the heartbeat it is booked in.  The book's row is the
//     fallback for a transaction the wallet no longer knows.
//   * The fence going down or up (`engine_fence: auto`).  Down, the
//     sub-account's coins are trading funds again: a deposit of what it
//     held.  Up, they leave: a withdrawal of what it holds.
// What does NOT cross it: a mint, a clawback, an expiry.  Collateral moves
// between the options address and a contract, and no balance the engine
// reads changes.
//
// HOW MUCH A FENCE EVENT MOVES, AND WHEN.  The fence keeps a log of its
// changes (rpc::CoinFence::Transition), each with the list it had then: the
// first list after it went up, the last one before it went down.  The books
// take the entries in order and acknowledge each only once it is booked, so
// two changes between two passes are two events, each with its own list --
// comparing only where the fence ended up would lose whatever the address
// held differently in between.  An event books its list, stamped with its
// moment, however much later it is written, so what reached the address
// afterwards is not in it.  The log begins with the state the fence is found
// in, which is how a change while the engine was not running shows.
//
// WHAT IS BOOKED, and why nothing is counted twice.
//   * A top-up is capital that left only if the fence was up when it
//     arrived.  With the engine running the book sends one only beside a
//     fence that is up and listed, so:
//       - while the books stand with the fence DOWN, no top-up is booked
//         (judge_topup: Deferred).  Its coins are trading funds, and the
//         next fence-up event takes them with everything else there;
//       - a top-up SENT before the newest fence-up event's list is inside
//         that event (topup_inside_fence_event): never booked -- unless that
//         list, when this process booked the event, does not hold the send's
//         own coin (topup_in_rise): it was still on its way, and is booked
//         when it arrives;
//       - any other confirmed top-up is booked.
//   * A top-up's fee is booked on its own (split_topup), once the send has
//     confirmed, wherever its principal goes: a fence event takes coins at
//     the address, and the fee was burned from the trading funds.
//   * Top-ups go first: a fence-down event is booked only in a pass where no
//     confirmed top-up that belongs before it is still waiting (for a price,
//     a wallet snapshot).
//   * The fence going down returns the coins of its list.  A top-up the
//     books do not hold yet is booked ahead of that event when the list
//     holds ITS coin -- the one its own transaction created, by id, never a
//     coin that merely looks like it -- whatever any record says yet -- or
//     when any list the fence had while up held that coin (its sightings:
//     the engine's balances left it out then, though it was spent since).
//     One that has arrived and that no list ever had came after the last
//     one: it is the next fence-up event's.  One whose coin
//     cannot be found yet (the wallet does not answer) holds the event, for
//     as long as the send can still be on its way: two hours
//     (kOptionsPendingSlackSeconds).  All of it is place_topup.  A fence
//     found down at start returns the address as it is, and every confirmed
//     top-up is before that.
//   * A fence event is written at EVERY transition, also when the address
//     held nothing (one zero leg on XCH).  So the direction of the newest
//     fence event is where the books stand, and a restart reads the same
//     answer.  With no fence event at all the state the fence is found in is
//     adopted with a zero event (#1), which takes nothing: no top-up is
//     inside it, and what the address held before the books began is history
//     (KNOWN LIMITS).
//
// A FLOW AND THE INVENTORY TRACKER.  The journal comes first, so a stop can
// fall between it and the tracker holding the flow.  Before the journal a
// marker is written for each asset (Database::mark_capital_reconcile_pending)
// and dropped only once the tracker holds the flow and inventory_state has
// been written -- a write that failed keeps it, and the next write retries.
// A start that finds one knows that Step 11's wallet reconcile, when it
// corrects that asset, is the flow reaching the tracker: it re-anchors the
// drawdown peak, with no second Net Deposits entry and no second move.  The
// bridge asset's tracked quantity is the bridge scan's alone, and it takes
// a flow in only from a settled balance: the drawdown peak is discarded once
// more when it has.
//
// UNTIL A FLOW IS BOOKED the wallet may already have moved by it (a price
// or a wallet snapshot is missing; the wallet could not be asked; an
// earlier fence event is not booked yet).  The engine widens the divergence
// control's tolerance by every such flow it knows of -- the fence events
// after one that could not be booked, and the top-ups not booked yet, each
// within two hours of its send (topup_slack_applies) -- so a transfer is not
// "reconciled" away as unexplained before it is booked.
//
// KNOWN LIMITS.  Each leaves a gap the divergence control absorbs.
//   * A premium is paid to an ordinary address of the wallet, so it reaches
//     the trading funds with no event.
//   * A top-up whose topup_sent row was never written (the app stopped
//     between the send and the write) is not booked.
//   * What happened before the first booked event: the sub-account's first
//     coins, and the fee coins the wallet took from it on 2026-11-20.
//   * With the engine not running while the book acts, the engine sees only
//     what the address holds at its next start.  A top-up sent then, with
//     the books standing fence-down, counts as inside the fence-up event:
//     wrong by its amount when its coin had not arrived yet, or had already
//     gone into a contract.  And a fence that went down then returns what
//     the address holds at that start, less whatever was spent from it.
//   * The fence's log lives in the process.  Changes it had not yet told the
//     books of at a stop are lost; the next start compares the books with
//     where it finds the fence.  So does the list a fence-up event took: after
//     a restart, a top-up sent before that event is inside it by its send
//     time alone.
//   * Switching the options book off in config.yaml (options.enabled, not
//     the toolbar) takes the fence away with no event.
//
// Everything that decides is pure and in this header, unit-tested in
// tests/test_options_transfer.cpp.

#ifndef XOP_ACCOUNTING_OPTIONS_TRANSFER_HPP
#define XOP_ACCOUNTING_OPTIONS_TRANSFER_HPP

#include "xop/types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace xop::accounting {

inline constexpr const char* kOptionsWithdrawal      = "options_withdrawal";
inline constexpr const char* kOptionsDeposit         = "options_deposit";
/// The leg that carries the capital, and the one that carries a send's fee.
inline constexpr const char* kOptionsLeg             = "options";
inline constexpr const char* kOptionsFeeLeg          = "fee";
inline constexpr const char* kOptionsEventPrefix     = "options_";
inline constexpr const char* kOptionsTopUpPrefix     = "options_topup:";
/// A top-up's fee, booked on its own (split_topup).
inline constexpr const char* kOptionsTopUpFeePrefix  = "options_topup_fee:";
inline constexpr const char* kOptionsFencePrefix     = "options_fence_";
inline constexpr const char* kOptionsFenceUpPrefix   = "options_fence_up:";
inline constexpr const char* kOptionsFenceDownPrefix = "options_fence_down:";

/// How long after its send a top-up that is not booked yet still widens the
/// divergence control's tolerance.  The book gives a send an hour to
/// confirm before it forgets it (OptionsBook._TOP_UP_FORGET_SECONDS); twice
/// that, and a send that never confirmed stops loosening the control.
inline constexpr std::int64_t kOptionsPendingSlackSeconds = 7'200;

/// "YYYY-MM-DDTHH:MM:SSZ" for a Unix time, in UTC.  "" for a time outside
/// 2000-01-01 .. 2200-01-01: the book writes `int(time.time())`, and
/// anything else is not a time this ledger can order.  Pure arithmetic (the
/// civil-from-days algorithm), so no C library time zone state is touched.
[[nodiscard]] inline std::string iso_from_unix(std::int64_t unix_seconds)
{
    constexpr std::int64_t kMin = 946'684'800;    // 2000-01-01T00:00:00Z
    constexpr std::int64_t kMax = 7'258'118'400;  // 2200-01-01T00:00:00Z
    if (unix_seconds < kMin || unix_seconds >= kMax) return {};
    const std::int64_t days = unix_seconds / 86'400;
    const std::int64_t secs = unix_seconds % 86'400;
    // Days since 1970-01-01 to a civil date (H. Hinnant, public domain).
    const std::int64_t z   = days + 719'468;
    const std::int64_t era = z / 146'097;
    const std::int64_t doe = z - era * 146'097;
    const std::int64_t yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp  = (5 * doy + 2) / 153;
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year  = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
                  static_cast<long long>(year), static_cast<long long>(month),
                  static_cast<long long>(day), static_cast<long long>(secs / 3'600),
                  static_cast<long long>((secs / 60) % 60), static_cast<long long>(secs % 60));
    return std::string(buf);
}

/// One row of the options book's `events` table, as the engine reads it.
struct OptionsEventRow {
    std::int64_t id{0};       ///< events.id (AUTOINCREMENT).
    std::int64_t at{0};       ///< events.at (Unix seconds).
    std::string  kind;        ///< topup_sent | topup_confirmed
    std::string  detail_json;
};

/// One top-up: the topup_sent row, with the time of the topup_confirmed row
/// for the same transaction when there is one.
struct OptionsTopUpRow {
    std::int64_t sent_event_id{0};  ///< events.id of topup_sent.
    std::int64_t sent_at{0};        ///< events.at of topup_sent (Unix seconds).
    std::int64_t confirmed_at{0};   ///< events.at of topup_confirmed; 0 = not yet.
    std::string  detail_json;       ///< topup_sent's detail.
};

/// One leg of a flow: signed mojos of one asset, + into the trading funds.
struct OptionsLeg {
    std::string leg;
    std::string asset_id;
    Mojo        delta_mojos{0};
};

enum class OptionsFlowStatus {
    Invalid,  ///< Can never book: left to the divergence control.
    Pending,  ///< A top-up the book has not recorded as confirmed yet.
    Ready,
};

/// A movement of capital across the edge of the trading book.
struct OptionsFlow {
    OptionsFlowStatus       status{OptionsFlowStatus::Invalid};
    std::string             event_type;  ///< options_withdrawal | options_deposit
    std::string             event_id;    ///< The ledger's idempotency key.
    std::string             reference;   ///< For the note and the log.
    std::vector<OptionsLeg> legs;
    /// A LOWER bound of when the wallet changed (the send; the transition).
    /// It is what the ledger opening and the inventory tracker's last
    /// absolute write are compared with: a later stamp could put a flow
    /// they already hold after them, and book or move it twice.
    std::string             floor_iso;
    /// When the flow is known to have happened (the confirmation).
    std::string             at_iso;
};

/// Mojos by asset ("xch" or an asset id), as the coin fence counts them
/// (rpc::CoinFence::Totals): an asset's total, or one entry for each coin.
using OptionsHeld = std::vector<std::pair<std::string, std::uint64_t>>;

namespace detail {

/// 64 lowercase hex digits, a 0x prefix dropped; nullopt for anything else.
[[nodiscard]] inline std::optional<std::string> hex64(const std::string& text)
{
    std::string out = text;
    if (out.size() >= 2 && out[0] == '0' && (out[1] == 'x' || out[1] == 'X')) {
        out.erase(0, 2);
    }
    if (out.size() != 64) return std::nullopt;
    for (char& c : out) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isxdigit(uc) == 0) return std::nullopt;
        c = static_cast<char>(std::tolower(uc));
    }
    return out;
}

/// A JSON integer as a Mojo; nullopt for anything else, or one no Mojo holds.
[[nodiscard]] inline std::optional<Mojo> json_mojos(const nlohmann::json& value)
{
    if (value.is_number_unsigned()) {
        const auto v = value.get<std::uint64_t>();
        if (v > static_cast<std::uint64_t>(std::numeric_limits<Mojo>::max())) return std::nullopt;
        return static_cast<Mojo>(v);
    }
    if (value.is_number_integer()) return value.get<Mojo>();
    return std::nullopt;
}

/// "strictly before" by the 19-character "YYYY-MM-DDTHH:MM:SS" prefix, as
/// iso_strictly_after (bridge_ingest.hpp) compares; kept local so this
/// header stands alone.  nullopt when either stamp is too short to compare.
[[nodiscard]] inline std::optional<bool> iso_before(const std::string& a,
                                                    const std::string& b) noexcept
{
    constexpr std::size_t kPrefix = 19;
    if (a.size() < kPrefix || b.size() < kPrefix) return std::nullopt;
    return a.compare(0, kPrefix, b, 0, kPrefix) < 0;
}

}  // namespace detail

/// Pairs each topup_sent row with the first topup_confirmed row after it
/// that names the same transaction.  Done here, not in SQL, so the engine's
/// SQLite needs no JSON functions.  Rows of any other kind are ignored; a
/// sent row whose transaction cannot be read is kept, unconfirmed, for
/// classify_options_topup to refuse.
[[nodiscard]] inline std::vector<OptionsTopUpRow> join_options_topups(
    const std::vector<OptionsEventRow>& events)
{
    const auto transaction_of = [](const std::string& detail_json) -> std::optional<std::string> {
        const nlohmann::json detail = nlohmann::json::parse(detail_json, nullptr, false);
        if (!detail.is_object()) return std::nullopt;
        const auto it = detail.find("transaction");
        if (it == detail.end() || !it->is_string()) return std::nullopt;
        return detail::hex64(it->get<std::string>());
    };
    std::vector<OptionsTopUpRow> rows;
    for (const OptionsEventRow& sent : events) {
        if (sent.kind != "topup_sent") continue;
        OptionsTopUpRow row;
        row.sent_event_id = sent.id;
        row.sent_at       = sent.at;
        row.detail_json   = sent.detail_json;
        if (const auto transaction = transaction_of(sent.detail_json)) {
            for (const OptionsEventRow& confirmed : events) {
                if (confirmed.kind != "topup_confirmed" || confirmed.id <= sent.id) continue;
                if (transaction_of(confirmed.detail_json) != transaction) continue;
                if (row.confirmed_at == 0 || confirmed.at < row.confirmed_at) {
                    row.confirmed_at = confirmed.at;
                }
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

/// Reads one top-up.  `quote_asset_id` is options.quote_asset_id, the asset
/// the book calls "quote asset".  The capital leg, always the first, is the
/// amount sent; the fee leg, when the send paid one, is XCH whatever was
/// sent.
[[nodiscard]] inline OptionsFlow classify_options_topup(const OptionsTopUpRow& row,
                                                        const std::string& quote_asset_id)
{
    OptionsFlow flow;
    const nlohmann::json detail = nlohmann::json::parse(row.detail_json, nullptr, false);
    if (!detail.is_object()) return flow;

    const auto transaction_it = detail.find("transaction");
    if (transaction_it == detail.end() || !transaction_it->is_string()) return flow;
    const auto transaction = detail::hex64(transaction_it->get<std::string>());
    if (!transaction) return flow;  // no idempotency key: never book on a guess

    const auto asset_it = detail.find("asset");
    if (asset_it == detail.end() || !asset_it->is_string()) return flow;
    const std::string what = asset_it->get<std::string>();
    std::string asset_id;
    if (what == "XCH") {
        asset_id = "xch";
    } else if (what == "quote asset") {
        const auto quote = detail::hex64(quote_asset_id);
        if (!quote) return flow;
        asset_id = *quote;
    } else {
        return flow;
    }

    const auto amount_it = detail.find("amount");
    if (amount_it == detail.end()) return flow;
    const auto amount = detail::json_mojos(*amount_it);
    if (!amount || *amount <= 0) return flow;

    Mojo fee = 0;
    if (const auto fee_it = detail.find("fee"); fee_it != detail.end() && !fee_it->is_null()) {
        const auto parsed = detail::json_mojos(*fee_it);
        if (!parsed || *parsed < 0) return flow;
        fee = *parsed;
    }

    flow.floor_iso = iso_from_unix(row.sent_at);
    if (flow.floor_iso.empty()) return flow;  // chronology unknowable

    flow.event_type = kOptionsWithdrawal;
    flow.event_id   = std::string{kOptionsTopUpPrefix} + *transaction;
    flow.reference  = "top-up " + transaction->substr(0, 16);
    flow.legs.push_back({kOptionsLeg, asset_id, -*amount});
    if (fee > 0) flow.legs.push_back({kOptionsFeeLeg, "xch", -fee});

    flow.at_iso = iso_from_unix(row.confirmed_at);
    // A confirmation stamped before its own send is not one to trust.
    flow.status = (!flow.at_iso.empty() && row.confirmed_at >= row.sent_at)
        ? OptionsFlowStatus::Ready
        : OptionsFlowStatus::Pending;
    return flow;
}

/// A top-up's two parts, booked apart: the capital it moved to the options
/// address, and the fee its send burned from the trading funds' XCH.  The
/// fee left the trading funds once the send confirmed, wherever the
/// principal is placed -- a fence event can take the principal (it is at the
/// address), never the fee.  `fee` is nullopt for a send that paid none.
struct OptionsTopUpParts {
    OptionsFlow                principal;
    std::optional<OptionsFlow> fee;
};

[[nodiscard]] inline OptionsTopUpParts split_topup(const OptionsFlow& topup)
{
    OptionsTopUpParts parts;
    parts.principal = topup;
    parts.principal.legs.clear();
    const std::string_view prefix{kOptionsTopUpPrefix};
    const std::string transaction = topup.event_id.rfind(prefix, 0) == 0
        ? topup.event_id.substr(prefix.size()) : topup.event_id;
    for (const auto& leg : topup.legs) {
        if (leg.leg != kOptionsFeeLeg) {
            parts.principal.legs.push_back(leg);
            continue;
        }
        OptionsFlow fee = topup;
        fee.legs      = {leg};
        fee.event_id  = std::string{kOptionsTopUpFeePrefix} + transaction;
        fee.reference = "fee of " + topup.reference;
        parts.fee     = std::move(fee);
    }
    return parts;
}

/// The fence going up (`went_up`) or down, holding `held` (the total of
/// each asset at the options address, at the transition).  Up, that capital
/// leaves the trading funds; down, it returns.  An asset with nothing held
/// has no leg -- except that the event always has one: with nothing held at
/// all it carries a single zero leg on XCH, because the event itself is the
/// record of which way the fence went.  The same shape adopts the state the
/// fence is found in when the books hold no fence event yet: pass nothing
/// held.  `sequence` numbers the fence events (next_fence_sequence), so the
/// same transition gets the same event id however often it is tried.
/// `at_iso` is the moment of the transition's list.
[[nodiscard]] inline OptionsFlow options_fence_flow(bool went_up, std::int64_t sequence,
                                                    const OptionsHeld& held,
                                                    const std::string& at_iso)
{
    OptionsFlow flow;
    if (sequence <= 0 || at_iso.empty()) return flow;
    for (const auto& [asset_id, amount] : held) {
        const Mojo mojos = to_mojo_saturating(amount);
        if (asset_id.empty() || mojos <= 0) continue;
        flow.legs.push_back({kOptionsLeg, asset_id, went_up ? -mojos : mojos});
    }
    if (flow.legs.empty()) flow.legs.push_back({kOptionsLeg, "xch", 0});
    flow.event_type = went_up ? kOptionsWithdrawal : kOptionsDeposit;
    flow.event_id   = std::string{went_up ? kOptionsFenceUpPrefix : kOptionsFenceDownPrefix}
                      + std::to_string(sequence);
    flow.reference  = std::string{went_up ? "fence up #" : "fence down #"} + std::to_string(sequence);
    flow.floor_iso  = at_iso;
    flow.at_iso     = at_iso;
    flow.status     = OptionsFlowStatus::Ready;
    return flow;
}

/// Where the books stand on the fence: the direction of the newest fence
/// event they hold ("" when they hold none).  Unknown is "no fence event
/// yet": the state the fence is found in is then adopted, with a zero event.
enum class OptionsFenceBooked { Unknown, Up, Down };

[[nodiscard]] inline OptionsFenceBooked booked_fence_state(
    const std::string& latest_fence_event_id) noexcept
{
    const auto starts_with = [&](const char* prefix) {
        return latest_fence_event_id.rfind(prefix, 0) == 0;
    };
    if (starts_with(kOptionsFenceDownPrefix)) return OptionsFenceBooked::Down;
    if (starts_with(kOptionsFenceUpPrefix)) return OptionsFenceBooked::Up;
    return OptionsFenceBooked::Unknown;
}

/// The stamp top-ups are compared with: the time of the newest fence-up
/// event that took what the address held (its entry_time, the moment of its
/// list).  "" when the books hold none -- and for event #1, which only
/// adopted the state the fence was found in and took nothing.
[[nodiscard]] inline std::string fence_up_stamp(const std::string& latest_fence_up_event_id,
                                                const std::string& entry_time)
{
    if (latest_fence_up_event_id.rfind(kOptionsFenceUpPrefix, 0) != 0) return {};
    if (latest_fence_up_event_id == std::string{kOptionsFenceUpPrefix} + "1") return {};
    return entry_time;
}

/// Whether a top-up is inside what a fence-up event booked: it was sent
/// before that event's list (`up_stamp`, from fence_up_stamp; "" when there
/// is none), so the engine was not fencing the address when it went out and
/// its coins are in what the event took.  Strictly before: beside a running
/// engine the book sends only once the list is published, in the same
/// second at the earliest.  Fails toward "inside": a stamp that cannot be
/// read is not booked on a guess.
[[nodiscard]] inline bool topup_inside_fence_event(const std::string& topup_floor_iso,
                                                   const std::string& up_stamp) noexcept
{
    if (up_stamp.empty()) return false;
    return detail::iso_before(topup_floor_iso, up_stamp).value_or(true);
}

/// Whether a top-up the send time puts inside the newest fence-up event
/// (topup_inside_fence_event) is in what that event took.  The event booked
/// its list; a send still on its way when the list was taken went out
/// before and is not in it -- the book switched off and on again within
/// minutes.  `rise_coin_ids`: that list's coins, when this process booked the
/// event (nullopt otherwise); `coin_id`: the send's own coin, from the
/// wallet's record of it (nullopt: unknown).  Without either the send time
/// decides, as it must after a restart.
[[nodiscard]] inline bool topup_in_rise(const std::optional<std::vector<std::string>>& rise_coin_ids,
                                        const std::optional<std::string>& coin_id)
{
    if (!rise_coin_ids || !coin_id) return true;
    return std::find(rise_coin_ids->begin(), rise_coin_ids->end(), *coin_id) != rise_coin_ids->end();
}

/// What becomes of a confirmed top-up the ledger does not hold yet.
enum class OptionsTopUpVerdict {
    Book,      ///< The fence was up when it went out: capital that left.
    Deferred,  ///< The books stand fence-down: the next fence-up event takes its coins.
    Inside,    ///< Sent before the newest fence-up event's list: never booked.
};

/// See WHAT IS BOOKED.  `books` is where the books stand now; Unknown (the
/// state is adopted before any top-up is judged) reads as Deferred.
[[nodiscard]] inline OptionsTopUpVerdict judge_topup(OptionsFenceBooked books,
                                                     const std::string& topup_floor_iso,
                                                     const std::string& up_stamp) noexcept
{
    if (books != OptionsFenceBooked::Up) return OptionsTopUpVerdict::Deferred;
    if (topup_inside_fence_event(topup_floor_iso, up_stamp)) {
        return OptionsTopUpVerdict::Inside;
    }
    return OptionsTopUpVerdict::Book;
}

/// What to do with one entry of the fence's log (rpc::CoinFence::Transition),
/// oldest first, given where the books stand.
enum class OptionsLogStep {
    Adopt,     ///< No fence event yet: the books begin in this entry's state.
    Skip,      ///< The books already stand there: nothing to book.
    BookUp,    ///< The fence went up: a withdrawal of its first list.
    BookDown,  ///< The fence went down: a deposit of its last list.
    /// It went up and down again before any list was had.  No balance was
    /// read behind it and nothing reached or left the address for it: the
    /// two entries are dropped together.
    DropPair,
    Wait,      ///< It went up and its coins are not listed yet.
};

[[nodiscard]] constexpr OptionsLogStep judge_transition(OptionsFenceBooked books, bool entry_up,
                                                        bool has_list, bool has_next) noexcept
{
    if (books == OptionsFenceBooked::Unknown) return OptionsLogStep::Adopt;
    if ((books == OptionsFenceBooked::Up) == entry_up) return OptionsLogStep::Skip;
    if (!entry_up) return OptionsLogStep::BookDown;
    if (has_list) return OptionsLogStep::BookUp;
    return has_next ? OptionsLogStep::DropPair : OptionsLogStep::Wait;
}

/// The amount a top-up sent: its capital leg, as a coin's amount.  nullopt
/// for a flow that is not a top-up.
[[nodiscard]] inline std::optional<std::uint64_t> topup_amount(const OptionsFlow& topup) noexcept
{
    if (topup.legs.empty() || topup.legs.front().leg != kOptionsLeg) return std::nullopt;
    const Mojo delta = topup.legs.front().delta_mojos;
    if (delta >= 0 || delta == std::numeric_limits<Mojo>::min()) return std::nullopt;
    return static_cast<std::uint64_t>(-delta);
}

/// One coin a transaction created, as the wallet's record of it lists it.
struct OptionsCoinRef {
    std::string parent_coin_info;
    std::string puzzle_hash;
    Mojo        amount{0};
};

/// What the wallet's own record of a top-up's transaction says (the
/// `transaction` object of its get_transaction answer).  It is this
/// transaction's confirmation and this transaction's coin: another coin of
/// the same amount at the address proves nothing about it.
struct OptionsTopUpProof {
    bool confirmed{false};
    /// The coins it created with exactly the amount sent.  The top-up's own
    /// coin is the one of them at the options address; the caller knows that
    /// address's puzzle hashes and how a coin's id is computed.
    std::vector<OptionsCoinRef> outputs;
};

/// Reads that record for `topup`.  nullopt when it does not say whether the
/// transaction confirmed: no answer is never read as either.
[[nodiscard]] inline std::optional<OptionsTopUpProof> read_topup_transaction(
    const nlohmann::json& transaction, const OptionsFlow& topup)
{
    if (!transaction.is_object()) return std::nullopt;
    const auto confirmed = transaction.find("confirmed");
    if (confirmed == transaction.end() || !confirmed->is_boolean()) return std::nullopt;
    OptionsTopUpProof proof;
    proof.confirmed = confirmed->get<bool>();
    const auto amount = topup_amount(topup);
    const auto additions = transaction.find("additions");
    if (!amount || additions == transaction.end() || !additions->is_array()) return proof;
    for (const auto& coin : *additions) {
        if (!coin.is_object()) continue;
        const auto parent = coin.find("parent_coin_info");
        const auto puzzle = coin.find("puzzle_hash");
        const auto value  = coin.find("amount");
        if (parent == coin.end() || !parent->is_string()) continue;
        if (puzzle == coin.end() || !puzzle->is_string()) continue;
        if (value == coin.end() || !value->is_number_unsigned()) continue;
        if (value->get<std::uint64_t>() != *amount) continue;
        proof.outputs.push_back({parent->get<std::string>(), puzzle->get<std::string>(),
                                 to_mojo_saturating(*amount)});
    }
    return proof;
}

/// What the engine knows about a top-up the books do not hold yet, in one
/// pass (see place_topup).
struct OptionsTopUpFacts {
    /// topup_slack_applies: the send can still be on its way.
    bool         recent{false};
    /// The options book recorded its confirmation.
    bool         book_confirmed{false};
    /// The wallet's own record of the send says it confirmed.
    bool         wallet_confirmed{false};
    /// That record names the coin the send created at the options address...
    bool         coin_known{false};
    /// ...and the fence lists that coin now (the fence is up).
    bool         coin_listed{false};
    /// A fence-down event follows this pass, and the list it returns...
    bool         lowering{false};
    /// ...holds that coin.
    bool         coin_returned{false};
    /// ...or any list the fence had while up before it went down holds it
    /// (rpc::CoinFence::Transition::seen): the engine's balances left it
    /// out then, though it may have been spent since.
    bool         coin_seen{false};
    /// The send went out before those sightings begin (this process started
    /// listing, or the fence last went up), so whether a list had it is not
    /// known.
    bool         sent_before_sightings{false};
};

/// Whether a send went out before the fence's sightings begin
/// (OptionsTopUpFacts::sent_before_sightings).  The options book stamps a
/// send in whole seconds, the fence its first list to the fraction: they are
/// compared by whole second, and a send in the first list's own second is
/// not before it (its coin cannot be in a list of that second: arriving takes
/// a block).  No sightings at all (`seen_since_unix` <= 0): before.
[[nodiscard]] inline bool sent_before_sightings(std::int64_t sent_at, double seen_since_unix) noexcept
{
    if (!(seen_since_unix > 0.0)) return true;
    return static_cast<double>(sent_at) < std::floor(seen_since_unix);
}

/// Where a top-up the books do not hold yet stands.
enum class OptionsTopUpPlace {
    Book,    ///< It left the trading funds: book it (ahead of the event, if one follows).
    NotYet,  ///< Not arrived: nothing to book; the divergence control carries it while recent.
    /// Arrived with the fence going down and not in the list that event
    /// returns: the trading funds never lost it, and the next fence-up event
    /// takes it.  Nothing to book.
    After,
    /// Going down, and which side of the event it belongs on cannot be told
    /// yet: the event waits for it.
    Hold,
};

/// See WHAT IS BOOKED.  The engine's balance is the wallet's less the
/// fence's list, so what decides is the send's OWN coin and the lists it is
/// in -- never a coin that merely looks like it.
[[nodiscard]] constexpr OptionsTopUpPlace place_topup(const OptionsTopUpFacts& f) noexcept
{
    // Its own coin listed -- by the fence now, or in the list a fence-down
    // event returns (the fence's own list is gone by then) -- is arrival,
    // whatever any record says yet.
    const bool listed = f.coin_known && (f.coin_listed || (f.lowering && f.coin_returned));
    const bool arrived = f.book_confirmed || f.wallet_confirmed || listed;
    if (!f.lowering) return arrived ? OptionsTopUpPlace::Book : OptionsTopUpPlace::NotYet;
    if (f.coin_known) {
        // In the list the event returns: the event's deposit gives it back,
        // so its withdrawal goes first.  Listed earlier while the fence was
        // up and spent since (a mint, a cancel): it left the trading funds
        // when the list took it in.
        if (f.coin_returned || f.coin_seen) return OptionsTopUpPlace::Book;
        if (!arrived) return OptionsTopUpPlace::NotYet;
        // Arrived, and no list ever had it while the fence was up: it came
        // after the last one, and the fence went down with it uncounted --
        // unless it was sent before the sightings begin, and an earlier list
        // that is not kept had it.
        return f.sent_before_sightings ? OptionsTopUpPlace::Book : OptionsTopUpPlace::After;
    }
    // Without the wallet's record there is no telling where its coin is:
    // the event waits while the send can still be on its way.  After that
    // the records decide: a confirmed send of two hours ago arrived long
    // before the fence went down.
    if (f.recent) return OptionsTopUpPlace::Hold;
    return arrived ? OptionsTopUpPlace::Book : OptionsTopUpPlace::NotYet;
}

/// How often the wallet is asked about a top-up the options book has not
/// recorded as confirmed, once it is no longer `recent` (still within
/// topup_slack_applies): the book has forgotten the send by then, and it
/// can still confirm.
inline constexpr BlockHeight kOptionsProofEveryBlocks = 120;

/// Whether to ask now.  `last_asked_block` 0 = never asked in this process.
[[nodiscard]] constexpr bool topup_proof_due(bool recent, BlockHeight block,
                                             BlockHeight last_asked_block) noexcept
{
    if (recent || last_asked_block == 0) return true;
    return block < last_asked_block || block - last_asked_block >= kOptionsProofEveryBlocks;
}

/// Whether a top-up sent at `sent_at` and not booked yet still widens the
/// divergence control's tolerance at `now_unix` (see UNTIL THE BOOK HAS
/// RECORDED A CONFIRMATION).
[[nodiscard]] constexpr bool topup_slack_applies(std::int64_t sent_at, std::int64_t now_unix) noexcept
{
    return sent_at > 0 && now_unix >= sent_at && now_unix - sent_at <= kOptionsPendingSlackSeconds;
}

/// `a + b` for two tolerances, a negative one counting as none, held well
/// inside Mojo's range: the divergence control adds its other terms to the
/// sum.
[[nodiscard]] constexpr Mojo add_slack(Mojo a, Mojo b) noexcept
{
    constexpr Mojo kCeiling = std::numeric_limits<Mojo>::max() / 4;
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (a > kCeiling) a = kCeiling;
    if (b > kCeiling) b = kCeiling;
    return a > kCeiling - b ? kCeiling : a + b;
}

/// add_slack of what a fence list holds of one asset (rpc::CoinFence::Totals).
[[nodiscard]] constexpr Mojo add_held_slack(Mojo a, std::uint64_t held) noexcept
{
    return add_slack(a, to_mojo_saturating(held));
}

/// The number of the next fence event, from the newest one the ledger holds
/// ("" when it holds none): one more than its own, or 1.
[[nodiscard]] inline std::int64_t next_fence_sequence(
    const std::string& latest_fence_event_id) noexcept
{
    const auto colon = latest_fence_event_id.rfind(':');
    if (latest_fence_event_id.rfind(kOptionsFencePrefix, 0) != 0 || colon == std::string::npos) {
        return 1;
    }
    std::int64_t n = 0;
    bool any = false;
    for (std::size_t i = colon + 1; i < latest_fence_event_id.size(); ++i) {
        const char c = latest_fence_event_id[i];
        if (c < '0' || c > '9' || n > 100'000'000'000'000LL) return 1;
        n = n * 10 + (c - '0');
        any = true;
    }
    return any ? n + 1 : 1;
}

/// The ledger note of an options leg.  The signed USD rides in the same
/// `flow_usd=` field a bridge note uses, so parse_bridge_flow_usd rebuilds
/// Net Deposits from these rows too (PnLTracker::rehydrate_from_db).
[[nodiscard]] inline std::string options_note(double flow_usd, double usd_per_unit,
                                              const std::string& reference)
{
    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "options sub-account %s; flow_usd=%.10f; px_usd_per_unit=%.10f; ref=%.60s",
                  flow_usd >= 0.0 ? "deposit" : "withdrawal", flow_usd, usd_per_unit,
                  reference.c_str());
    return std::string(buf);
}

}  // namespace xop::accounting

#endif  // XOP_ACCOUNTING_OPTIONS_TRANSFER_HPP
