// cancel_escalation.hpp -- [S14 2026-09-13] Keep an offer cancel_pending
// until the WALLET says it is over, and escalate cancels the CHAIN proves
// never landed.
//
// THE DEFECT.  An accepted cancel RPC is a SUBMISSION, not a verdict.  The
// wallet flips a trade to PENDING_CANCEL before it pushes the spend, and the
// spend can be pruned, refused or never broadcast -- after which the trade
// sits PENDING_CANCEL indefinitely with every maker coin unspent and the offer
// still takeable.  Three XCH/BYC bids posted 2026-08-30 did exactly that for
// thirteen days while offer_log called them 'cancelled'; five XCH/DBX offers
// were counted "stuck" 215 times in four hours by a forced cancel that skipped
// them every time.
//
// WHAT THIS HEADER DECIDES.  Pure: no I/O, no engine types, no clock.
//   * which offers the forced-cancel paths may touch
//     (is_forced_cancel_candidate), and when the stuck summary may log;
//   * when a cancel_pending offer is re-examined and what the answer means
//     (escalation_probe_due, decide_cancel_escalation);
//   * what counts as chain proof (parse_coins_of_interest,
//     classify_coin_records).  It FAILS CLOSED: no proof means no fee, and
//     nothing here ever authorises a terminal stamp -- a spent maker coin can
//     be a FILL, so the wallet stays the only source of a terminal verdict;
//   * how much a re-cancel pays (escalation_fee_mojos), and when a sweep
//     suspended in an RPC must stop before it pays (escalation_must_yield);
//   * when the operator hears about it, in a way a rate limit cannot swallow
//     (unresolved_alert_due, UnresolvedAlertQueue);
//   * what boot does with a wallet PENDING_CANCEL record (startup_scan_bucket,
//     startup_pending_cancel_action).
//
// Heights: peak height advances ~4,608 blocks/day (18.75 s per block), and
// every block count below is converted at that cadence.

#ifndef XOP_EXECUTION_CANCEL_ESCALATION_HPP
#define XOP_EXECUTION_CANCEL_ESCALATION_HPP

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xop::execution {

// ---------------------------------------------------------------------------
// Constants.  The escalation ones are the DEFAULTS of the strategy.cancel_
// escalation_* config keys (see cancel_escalation_config.hpp); the engine
// always reads the configured values through CancelEscalationParams.
// ---------------------------------------------------------------------------

/// Blocks a PENDING_CANCEL offer is left alone before a re-cancel may be
/// considered: 96 blocks x 18.75 s = 30 minutes.  Also the spacing between
/// two escalations of one offer.
inline constexpr std::uint64_t kCancelEscalationWindowBlocks = 96;

/// Fee-bearing re-cancels per offer before it is declared exhausted and the
/// operator is alerted.  Counted across restarts: the engine seeds the count
/// from offer_closure_events ("cancel_escalation_N" events).
inline constexpr std::uint32_t kMaxCancelEscalations = 3;

/// chia 2.7.4 mempool_manager MEMPOOL_MIN_FEE_INCREASE.  A replacement must
/// spend a superset of the conflicting coins, pay a higher fee per cost AND
/// add at least this many mojos, or it is refused with MEMPOOL_CONFLICT.
inline constexpr std::uint64_t kMempoolMinFeeIncreaseMojos = 10'000'000;

/// Ceiling on any single escalated cancel fee (0.0001 XCH).
inline constexpr std::uint64_t kMaxCancelEscalationFeeMojos = 100'000'000;

/// Minimum spacing between two probes of one offer, the first rung of the
/// back-off, and the window for a PENDING_ACCEPT record (nothing is in flight
/// to replace, so waiting the full 96 blocks would only leave it takeable):
/// 8 blocks x 18.75 s = 2.5 minutes.
inline constexpr std::uint64_t kEscalationRetryBlocks = 8;

/// Offers probed per heartbeat.  Each probe is one wallet get_offer plus one
/// node get_coin_records_by_names.
inline constexpr std::size_t kMaxEscalationProbesPerSweep = 5;

/// A stuck summary with an unchanged count logs at most this often:
/// 32 blocks x 18.75 s = 10 minutes.
inline constexpr std::uint64_t kStuckSummaryLogIntervalBlocks = 32;

/// Consecutive failed re-cancel submissions for one offer before the
/// operator is alerted that the re-cancel itself keeps failing.
inline constexpr std::uint32_t kEscalationErrorAlertThreshold = 3;

/// Minimum spacing between two CancelUnresolved alerts.  AlertManager drops
/// a CRITICAL alert sent within 60 s of the previous one for the same rule
/// (alerts.hpp cooldown_critical_); 65 s keeps every send outside it.
inline constexpr std::int64_t kCancelUnresolvedAlertGapMs = 65'000;

/// Offer ids named in one alert; the rest stay queued for the next window.
inline constexpr std::size_t kMaxIdsPerCancelUnresolvedAlert = 12;

/// Wall-clock budget for one escalation sweep.  Checked before each probe;
/// the first failed RPC of any kind also ends the sweep, so a hanging wallet
/// costs one timed-out call per heartbeat, not five.
inline constexpr std::int64_t kEscalationSweepBudgetMs = 60'000;

/// Largest amount CoinManager::compute_coin_name can hash: it takes Mojo
/// (std::int64_t), and a larger value would silently hash a different coin.
inline constexpr std::uint64_t kMaxHashableCoinAmount =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

/// Doublings applied by escalation_backoff_blocks before the window caps it.
inline constexpr std::uint32_t kMaxBackoffDoublings = 16;

/// Chia TradeStatus codes this header distinguishes at boot.
inline constexpr int kWalletStatusPendingAccept = 0;
inline constexpr int kWalletStatusPendingCancel = 2;

/// offer_log.status values (the same strings as database.hpp).
inline constexpr std::string_view kDbStatusPending{"pending"};
inline constexpr std::string_view kDbStatusCancelPending{"cancel_pending"};
inline constexpr std::string_view kDbStatusCancelled{"cancelled"};

static_assert(kMaxCancelEscalationFeeMojos > kMempoolMinFeeIncreaseMojos);

// ---------------------------------------------------------------------------
// Types.  Every member has a default initializer.
// ---------------------------------------------------------------------------

/// The escalation knobs, as configured.  Defaults are the constants above.
struct CancelEscalationParams {
    std::uint64_t window_blocks{kCancelEscalationWindowBlocks};
    std::uint32_t max_escalations{kMaxCancelEscalations};
    std::uint64_t fee_step_mojos{kMempoolMinFeeIncreaseMojos};
    std::uint64_t max_fee_mojos{kMaxCancelEscalationFeeMojos};
    std::uint64_t retry_blocks{kEscalationRetryBlocks};
    std::size_t   max_probes_per_sweep{kMaxEscalationProbesPerSweep};
};

/// What the wallet's trade record says.  Unknown covers a failed read, a
/// missing field and a code this build does not recognise.
enum class WalletCancelState : int {
    Unknown = 0,
    PendingAccept,
    PendingConfirm,
    PendingCancel,
    Cancelled,
    Confirmed,
    Failed,
};

/// What the full node says about the offer's coins_of_interest.
enum class CoinProof : int {
    Unknown = 0,   ///< no answer, a partial answer, or an unreadable one
    AllUnspent,    ///< every requested coin found, none spent
    SomeSpent,     ///< at least one requested coin is spent
};

/// Per-offer escalation state.  Engine-owned, in memory.
struct CancelEscalationTrack {
    /// Block the offer was first seen cancel_pending, or of its last
    /// escalation.  0 = not seen yet.
    std::uint64_t anchor_block{0};
    /// Fee-bearing re-cancels this offer has used.  Seeded at first sight
    /// from its recorded cancel_escalation_N events -- each written BEFORE its
    /// fee is paid, so a submission that then failed counts too -- and raised
    /// by each submission this process completes.
    std::uint32_t escalations{0};
    /// No probe before this block.
    std::uint64_t retry_after_block{0};
    /// Highest fee an escalation of this offer has bid.  Seeded at first
    /// sight from the fees recorded on those events
    /// (Database::max_cancel_escalation_fee), then raised by each escalation
    /// this process pays.
    std::uint64_t last_fee_mojos{0};
    /// Consecutive probes that found nothing to do (drives the back-off).
    std::uint32_t idle_probes{0};
    /// Consecutive failed re-cancel submissions (drives the back-off and
    /// the ResubmitFailing alert).
    std::uint32_t consecutive_errors{0};
    /// Set only when the offer was NAMED in an alert that was actually sent.
    bool          alerted{false};
};

enum class CancelEscalationVerdict : int {
    AnchorNow = 0,     ///< first sighting: record the anchor, nothing else
    Wait,              ///< not due, or a PENDING_CANCEL inside its window
    ResolvedByWallet,  ///< CANCELLED / FAILED / CONFIRMED: the fill and
                       ///< terminal paths own it now
    Resolving,         ///< PENDING_CONFIRM, or a maker coin already spent
    NoProof,           ///< wallet or chain gave no usable answer: pay nothing
    Escalate,          ///< proven stranded: submit a secure re-cancel
    Exhausted,         ///< proven stranded, the cap is spent: alert
};

struct CancelEscalationInput {
    std::uint64_t          current_block{0};
    CancelEscalationTrack  track{};
    WalletCancelState      wallet{WalletCancelState::Unknown};
    CoinProof              coins{CoinProof::Unknown};
    CancelEscalationParams params{};
};

/// One coin of a trade record's coins_of_interest, normalised: lowercase hex
/// without 0x, amount within the range compute_coin_name can hash.
struct CoinRef {
    std::string   parent_hex{};
    std::string   puzzle_hash_hex{};
    std::uint64_t amount{0};
};

/// What boot does with a wallet PENDING_CANCEL record, keyed by offer_log.
enum class StartupPendingCancelAction : int {
    IgnoreNotOurs = 0,     ///< no offer_log row: not a bot offer
    MarkCancelPending,     ///< row 'pending': the cancel was never recorded
    AlreadyCancelPending,  ///< row 'cancel_pending': restore the flag
    ReopenMislabelled,     ///< row 'cancelled' at RPC acceptance: reopen it
    Inconsistent,          ///< any other status: warn, touch nothing
};

/// How startup_reconcile's wallet scan files one trade record.
enum class StartupScanBucket : int {
    Ignore = 0,             ///< a status boot does not act on
    OrphanCandidate,        ///< PENDING_ACCEPT, no offer_log row
    KnownLive,              ///< PENDING_ACCEPT with an offer_log row
    PendingCancelObserved,  ///< PENDING_CANCEL, whoever owns it
};

enum class UnresolvedReason : int {
    StillTakeable = 0,    ///< proven unspent, escalation cap spent
    Unverifiable,         ///< no proof either way for the full ladder
    WalletNotReconciled,  ///< a maker coin is spent, wallet still pending
    ResubmitFailing,      ///< proven unspent, the re-cancel keeps failing
};

struct UnresolvedAlertEntry {
    std::string      offer_id{};
    std::string      pair_name{};
    std::uint32_t    escalations{0};
    UnresolvedReason reason{UnresolvedReason::Unverifiable};
    std::string      detail{};
};

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] constexpr std::uint64_t saturating_mul(std::uint64_t a,
                                                     std::uint64_t b) noexcept
{
    if (a != 0U && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a * b;
}

/// A 32-byte hex string with an optional 0x prefix, lowercased, else nullopt.
[[nodiscard]] inline std::optional<std::string> normalize_hex32(
    const nlohmann::json& value)
{
    if (!value.is_string()) {
        return std::nullopt;
    }
    std::string_view text{value.get_ref<const std::string&>()};
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.size() != 64) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(64);
    for (const char c : text) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            out.push_back(c);
        } else if (c >= 'A' && c <= 'F') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            return std::nullopt;
        }
    }
    return out;
}

}  // namespace detail

[[nodiscard]] constexpr const char* wallet_cancel_state_name(
    WalletCancelState state) noexcept
{
    switch (state) {
        case WalletCancelState::Unknown:        return "UNKNOWN";
        case WalletCancelState::PendingAccept:  return "PENDING_ACCEPT";
        case WalletCancelState::PendingConfirm: return "PENDING_CONFIRM";
        case WalletCancelState::PendingCancel:  return "PENDING_CANCEL";
        case WalletCancelState::Cancelled:      return "CANCELLED";
        case WalletCancelState::Confirmed:      return "CONFIRMED";
        case WalletCancelState::Failed:         return "FAILED";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr const char* escalation_verdict_name(
    CancelEscalationVerdict verdict) noexcept
{
    switch (verdict) {
        case CancelEscalationVerdict::AnchorNow:        return "anchor";
        case CancelEscalationVerdict::Wait:             return "wait";
        case CancelEscalationVerdict::ResolvedByWallet: return "resolved-by-wallet";
        case CancelEscalationVerdict::Resolving:        return "resolving";
        case CancelEscalationVerdict::NoProof:          return "no-proof";
        case CancelEscalationVerdict::Escalate:         return "escalate";
        case CancelEscalationVerdict::Exhausted:        return "exhausted";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// (c) One set for detection and remedy.
// ---------------------------------------------------------------------------

/// Whether a forced (TTL / stuck) cancel may act on this offer.  This is
/// cancel_stale's boundary (current >= created + ttl), written without the
/// created + ttl overflow, and it is the ONE predicate the Step 8 stuck
/// counter, cancel_stale and the STOPDRAIN eligibility count share -- the
/// counter used to count cancel_pending offers that cancel_stale then
/// skipped, so "N stuck offers -- attempting forced cancel" fired every block
/// and cancelled nothing.  cancel_pending offers belong to the escalation.
[[nodiscard]] constexpr bool is_forced_cancel_candidate(
    bool          cancel_pending,
    std::uint64_t created_block,
    std::uint64_t current_block,
    std::uint64_t min_age_blocks) noexcept
{
    return !cancel_pending
        && current_block >= created_block
        && current_block - created_block >= min_age_blocks;
}

/// Whether the per-pair stuck summary may log this block: on any change of
/// the count, and otherwise at most every kStuckSummaryLogIntervalBlocks
/// while it stays non-zero.
[[nodiscard]] constexpr bool stuck_summary_log_due(
    std::size_t   last_count,
    std::size_t   count,
    std::uint64_t last_logged_block,
    std::uint64_t current_block) noexcept
{
    if (count != last_count) {
        return true;
    }
    if (count == 0) {
        return false;
    }
    return current_block >= last_logged_block
        && current_block - last_logged_block >= kStuckSummaryLogIntervalBlocks;
}

// ---------------------------------------------------------------------------
// Reading the wallet and the chain.
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr WalletCancelState wallet_cancel_state_from_code(
    int code) noexcept
{
    switch (code) {
        case 0:  return WalletCancelState::PendingAccept;
        case 1:  return WalletCancelState::PendingConfirm;
        case 2:  return WalletCancelState::PendingCancel;
        case 3:  return WalletCancelState::Cancelled;
        case 4:  return WalletCancelState::Confirmed;
        case 5:  return WalletCancelState::Failed;
        default: break;
    }
    return WalletCancelState::Unknown;
}

/// A trade record's status as an integer or as a TradeStatus name (the table
/// offer_manager.cpp's trade_status::parse uses).  Anything else is Unknown.
[[nodiscard]] inline WalletCancelState wallet_cancel_state_from_record(
    const nlohmann::json& record)
{
    if (!record.is_object()) {
        return WalletCancelState::Unknown;
    }
    const auto it = record.find("status");
    if (it == record.end()) {
        return WalletCancelState::Unknown;
    }
    if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        return value <= 5U
            ? wallet_cancel_state_from_code(static_cast<int>(value))
            : WalletCancelState::Unknown;
    }
    if (it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        return (value >= 0 && value <= 5)
            ? wallet_cancel_state_from_code(static_cast<int>(value))
            : WalletCancelState::Unknown;
    }
    if (it->is_string()) {
        const auto& name = it->get_ref<const std::string&>();
        if (name == "PENDING_ACCEPT") {
            return WalletCancelState::PendingAccept;
        }
        if (name == "PENDING_CONFIRM") {
            return WalletCancelState::PendingConfirm;
        }
        if (name == "PENDING_CANCEL") {
            return WalletCancelState::PendingCancel;
        }
        if (name == "CANCELLED") {
            return WalletCancelState::Cancelled;
        }
        if (name == "CONFIRMED") {
            return WalletCancelState::Confirmed;
        }
        if (name == "FAILED") {
            return WalletCancelState::Failed;
        }
    }
    return WalletCancelState::Unknown;
}

/// One coin object ({parent_coin_info, puzzle_hash, amount}), or nullopt if
/// any field is missing or malformed.  Type-checks before every get<>: a get
/// on a string or a negative value throws, and the fail-closed path must not
/// depend on an exception.
[[nodiscard]] inline std::optional<CoinRef> parse_coin_ref(
    const nlohmann::json& coin)
{
    if (!coin.is_object()) {
        return std::nullopt;
    }
    const auto parent_it = coin.find("parent_coin_info");
    const auto puzzle_it = coin.find("puzzle_hash");
    const auto amount_it = coin.find("amount");
    if (parent_it == coin.end() || puzzle_it == coin.end()
        || amount_it == coin.end()) {
        return std::nullopt;
    }
    auto parent = detail::normalize_hex32(*parent_it);
    auto puzzle = detail::normalize_hex32(*puzzle_it);
    if (!parent || !puzzle) {
        return std::nullopt;
    }
    std::uint64_t amount = 0;
    if (amount_it->is_number_unsigned()) {
        amount = amount_it->get<std::uint64_t>();
    } else if (amount_it->is_number_integer()) {
        const auto signed_amount = amount_it->get<std::int64_t>();
        if (signed_amount < 0) {
            return std::nullopt;
        }
        amount = static_cast<std::uint64_t>(signed_amount);
    } else {
        return std::nullopt;
    }
    if (amount > kMaxHashableCoinAmount) {
        return std::nullopt;
    }
    CoinRef ref;
    ref.parent_hex      = std::move(*parent);
    ref.puzzle_hash_hex = std::move(*puzzle);
    ref.amount          = amount;
    return ref;
}

/// The trade record's coins_of_interest.  ANY malformed element returns an
/// empty vector: a proof built from the coins we could read would be a
/// proof about a different, smaller offer.
[[nodiscard]] inline std::vector<CoinRef> parse_coins_of_interest(
    const nlohmann::json& trade_record)
{
    if (!trade_record.is_object()) {
        return {};
    }
    const auto it = trade_record.find("coins_of_interest");
    if (it == trade_record.end() || !it->is_array()) {
        return {};
    }
    std::vector<CoinRef> refs;
    refs.reserve(it->size());
    for (const auto& element : *it) {
        auto ref = parse_coin_ref(element);
        if (!ref) {
            return {};
        }
        refs.push_back(std::move(*ref));
    }
    return refs;
}

/// Coin names for `refs`, computed by `name_of` (CoinManager::compute_coin_name
/// behind a catch in the engine).  One name that cannot be computed -- an
/// empty string -- voids the whole list.  The callable is taken BY VALUE: a
/// plain function bound to `const F&` is a const-qualified function type,
/// which MSVC reports as C4180.
template <class NameOf>
[[nodiscard]] std::vector<std::string> coin_names_for(
    const std::vector<CoinRef>& refs, NameOf name_of)
{
    std::vector<std::string> names;
    names.reserve(refs.size());
    for (const auto& ref : refs) {
        std::string name = name_of(ref);
        if (name.empty()) {
            return {};
        }
        names.push_back(std::move(name));
    }
    return names;
}

/// Whether one node coin record is spent: spent_block_index > 0 or
/// spent == true.  nullopt when neither field is present or either is
/// malformed.
[[nodiscard]] inline std::optional<bool> coin_record_spent(
    const nlohmann::json& record)
{
    if (!record.is_object()) {
        return std::nullopt;
    }
    bool known = false;
    bool spent = false;
    const auto index_it = record.find("spent_block_index");
    if (index_it != record.end()) {
        if (index_it->is_number_unsigned()) {
            spent = spent || index_it->get<std::uint64_t>() > 0U;
        } else if (index_it->is_number_integer()) {
            const auto index = index_it->get<std::int64_t>();
            if (index < 0) {
                return std::nullopt;
            }
            spent = spent || index > 0;
        } else {
            return std::nullopt;
        }
        known = true;
    }
    const auto flag_it = record.find("spent");
    if (flag_it != record.end()) {
        if (!flag_it->is_boolean()) {
            return std::nullopt;
        }
        spent = spent || flag_it->get<bool>();
        known = true;
    }
    if (!known) {
        return std::nullopt;
    }
    return spent;
}

/// What a get_coin_records_by_names answer proves about `requested_names`.
///
/// [review] The node returns records ONLY for coins it finds and silently
/// omits the rest, so "every record we got is unspent" is not "every coin is
/// unspent".  Each record is matched back to a requested name by hashing its
/// own coin with `name_of`:
///   1. any MATCHED record that is spent  -> SomeSpent (the offer cannot be
///      taken any more; checked first);
///   2. any requested name without a record, or any record that cannot be
///      read or matched                   -> Unknown;
///   3. otherwise                         -> AllUnspent.
template <class NameOf>
[[nodiscard]] CoinProof classify_coin_records(
    const std::vector<std::string>&    requested_names,
    const std::vector<nlohmann::json>& records,
    NameOf                             name_of)
{
    if (requested_names.empty()) {
        return CoinProof::Unknown;
    }
    const std::unordered_set<std::string> wanted(requested_names.begin(),
                                                 requested_names.end());
    std::unordered_set<std::string> answered;
    bool any_spent = false;
    bool any_unusable = false;
    for (const auto& record : records) {
        const auto coin_it = record.find("coin");
        if (coin_it == record.end() || !coin_it->is_object()) {
            any_unusable = true;
            continue;
        }
        const auto ref = parse_coin_ref(*coin_it);
        const std::string name = ref ? std::string{name_of(*ref)} : std::string{};
        if (name.empty() || wanted.count(name) == 0U) {
            any_unusable = true;
            continue;
        }
        const auto spent = coin_record_spent(record);
        if (!spent) {
            any_unusable = true;
            continue;
        }
        if (*spent) {
            any_spent = true;
        }
        answered.insert(name);
    }
    if (any_spent) {
        return CoinProof::SomeSpent;
    }
    if (any_unusable || answered.size() != wanted.size()) {
        return CoinProof::Unknown;
    }
    return CoinProof::AllUnspent;
}

// ---------------------------------------------------------------------------
// (b) The escalation decision.
// ---------------------------------------------------------------------------

/// [review, round 2] The gates another thread, or a co_spawned coroutine, can
/// close while the sweep is suspended in a wallet or node RPC.  The engine
/// reads all three at its entry gate, before each candidate, and again
/// immediately before each fee-bearing call:
///   * graceful_cancel_active -- shutdown() is walking the book;
///   * cancel_all_inflight    -- a co_spawned operator Cancel All is;
///   * watchdog_fired         -- the dead man's switch fired.  It latches the
///     flag on ITS OWN thread, then sends a wallet-wide zero-fee cancel.  An
///     escalated bundle conflicts with that bundle on the offer coin and
///     neither is a superset of the other, so if the escalation reaches the
///     mempool first, the watchdog's whole batch is refused.
/// wallet_circuit_open_ and xch_recovery_mode_ are written only inside the
/// cycle, so they cannot change while the sweep is suspended.
///
/// RESIDUAL RACE: a watchdog that fires after the last read -- while the
/// escalation's record is written or its re-cancel is in flight -- is not
/// stopped by any flag.  Closing that needs a claim both threads take.
struct EscalationAsyncGates {
    bool graceful_cancel_active{false};
    bool cancel_all_inflight{false};
    bool watchdog_fired{false};
};

/// Whether the sweep must stop before its next probe or fee: any
/// asynchronous gate is closed.
[[nodiscard]] constexpr bool escalation_must_yield(
    const EscalationAsyncGates& gates) noexcept
{
    return gates.graceful_cancel_active
        || gates.cancel_all_inflight
        || gates.watchdog_fired;
}

/// Whether a tracked offer may be probed (wallet + node) this block: seen
/// before, at least retry_blocks since the anchor, and past its back-off.
[[nodiscard]] constexpr bool escalation_probe_due(
    const CancelEscalationTrack&  track,
    std::uint64_t                 current_block,
    const CancelEscalationParams& params = CancelEscalationParams{}) noexcept
{
    return track.anchor_block != 0U
        && current_block >= track.anchor_block
        && current_block - track.anchor_block >= params.retry_blocks
        && current_block >= track.retry_after_block;
}

/// The verdict for one probed offer.  Order matters and is pinned by tests:
///   1. never seen              -> AnchorNow
///   2. probe not due           -> Wait
///   3. wallet verdict (every enumerator, no default):
///        CANCELLED/FAILED/CONFIRMED -> ResolvedByWallet
///        PENDING_CONFIRM            -> Resolving
///        unknown                    -> NoProof
///        PENDING_CANCEL inside the window -> Wait (a spend may be in flight)
///        PENDING_CANCEL past it, PENDING_ACCEPT -> read the chain
///   4. a maker coin spent      -> Resolving
///   5. no complete chain proof -> NoProof
///   6. cap spent               -> Exhausted
///   7. otherwise               -> Escalate
[[nodiscard]] constexpr CancelEscalationVerdict decide_cancel_escalation(
    const CancelEscalationInput& in) noexcept
{
    const CancelEscalationTrack&  t = in.track;
    const CancelEscalationParams& p = in.params;
    if (t.anchor_block == 0U) {
        return CancelEscalationVerdict::AnchorNow;
    }
    if (!escalation_probe_due(t, in.current_block, p)) {
        return CancelEscalationVerdict::Wait;
    }
    switch (in.wallet) {
        case WalletCancelState::Cancelled:
        case WalletCancelState::Failed:
        case WalletCancelState::Confirmed:
            return CancelEscalationVerdict::ResolvedByWallet;
        case WalletCancelState::PendingConfirm:
            return CancelEscalationVerdict::Resolving;
        case WalletCancelState::Unknown:
            return CancelEscalationVerdict::NoProof;
        case WalletCancelState::PendingCancel:
            if (in.current_block - t.anchor_block < p.window_blocks) {
                return CancelEscalationVerdict::Wait;
            }
            break;
        case WalletCancelState::PendingAccept:
            break;
    }
    if (in.coins == CoinProof::SomeSpent) {
        return CancelEscalationVerdict::Resolving;
    }
    if (in.coins != CoinProof::AllUnspent) {
        return CancelEscalationVerdict::NoProof;
    }
    if (t.escalations >= p.max_escalations) {
        return CancelEscalationVerdict::Exhausted;
    }
    return CancelEscalationVerdict::Escalate;
}

/// Blocks to wait before the next probe after `streak` consecutive
/// no-action probes (or failed submissions): retry_blocks doubled per step,
/// capped at the window.  8, 16, 32, 64, 96, 96, ... with the defaults.
[[nodiscard]] constexpr std::uint64_t escalation_backoff_blocks(
    std::uint32_t                 streak,
    const CancelEscalationParams& params = CancelEscalationParams{}) noexcept
{
    const std::uint32_t doublings =
        streak < kMaxBackoffDoublings ? streak : kMaxBackoffDoublings;
    std::uint64_t blocks = params.retry_blocks > 0U ? params.retry_blocks : 1U;
    for (std::uint32_t i = 0; i < doublings && blocks < params.window_blocks; ++i) {
        blocks = detail::saturating_mul(blocks, 2U);
    }
    if (params.window_blocks > 0U && blocks > params.window_blocks) {
        blocks = params.window_blocks;
    }
    return blocks;
}

/// The fee for the next escalated re-cancel.
///
/// [review] base + n x step ignored what the CONFLICTING transaction paid.
/// Step 8 cancels at the dynamic fee, emergency_cancel goes up to twice it,
/// and the adaptive tracker can lower the base between attempts -- each of
/// which put the increment below MEMPOOL_MIN_FEE_INCREASE, and the wallet
/// RPC still reports success, so a refused replacement was counted toward
/// the cap.  The floor therefore takes the last escalation fee and a ceiling
/// on the cancel being replaced:
///
///   fee = min(cap, max(base, last escalation fee, prior-cancel ceiling) + step)
///
/// [review, round 2] What those two inputs actually bound.  The floor is NOT
/// the highest fee every earlier cancel of this offer could have paid:
///   * last escalation fee -- the highest fee an escalation of this offer has
///     bid.  It PERSISTS: each escalation records its fee before paying it,
///     and first sighting seeds it back, so after a restart escalation N+1
///     no longer bids exactly what escalation N paid.
///   * prior-cancel ceiling -- twice the dynamic fee AT PROBE TIME, not the
///     fee the original cancel paid.  Initial cancel fees are NOT persisted,
///     and the adaptive fee can move by orders of magnitude between that
///     cancel and this probe (the tracker clamps it only to
///     fees.min_fee_mojos .. fees.max_fee_mojos and the fee budget).
///   * STRUCTURAL LIMIT -- emergency_cancel's top tier pays up to twice the
///     dynamic fee, so up to 2 x fees.max_fee_mojos, while one escalation is
///     capped at params.max_fee_mojos (strategy.cancel_escalation_max_fee_mojos).
///     A conflicting cancel that paid more than cap - step (90,000,000 mojos
///     with the defaults) cannot be outbid by any escalation, whatever is
///     persisted.
///
/// HONEST LIMIT: a valid replacement also needs a SUPERSET of the conflicting
/// spend's coins, and a CAT-leg cancel re-selects its XCH fee coin
/// (coin_lock_ledger.hpp), so no fee guarantees replacement.  The ladder
/// reliably helps when no conflicting transaction exists -- the live stranded
/// case, whose cancel records carry no spend bundle or were pruned.
[[nodiscard]] constexpr std::uint64_t escalation_fee_mojos(
    std::uint64_t                 base_fee,
    std::uint64_t                 last_fee,
    std::uint64_t                 prior_cancel_fee_ceiling,
    const CancelEscalationParams& params = CancelEscalationParams{}) noexcept
{
    std::uint64_t floor_fee = base_fee;
    if (last_fee > floor_fee) {
        floor_fee = last_fee;
    }
    if (prior_cancel_fee_ceiling > floor_fee) {
        floor_fee = prior_cancel_fee_ceiling;
    }
    if (floor_fee >= params.max_fee_mojos) {
        return params.max_fee_mojos;
    }
    if (params.fee_step_mojos >= params.max_fee_mojos - floor_fee) {
        return params.max_fee_mojos;
    }
    return floor_fee + params.fee_step_mojos;
}

/// Whether an offer that keeps producing no action (NoProof, Resolving)
/// has done so for the length of the full ladder -- window x (cap + 1),
/// 384 blocks (~2 h) by default -- and has not been named in an alert yet.
[[nodiscard]] constexpr bool unresolved_alert_due(
    const CancelEscalationTrack&  track,
    std::uint64_t                 current_block,
    const CancelEscalationParams& params = CancelEscalationParams{}) noexcept
{
    const std::uint64_t horizon = detail::saturating_mul(
        params.window_blocks,
        static_cast<std::uint64_t>(params.max_escalations) + 1U);
    return !track.alerted
        && track.anchor_block != 0U
        && current_block >= track.anchor_block
        && current_block - track.anchor_block >= horizon;
}

// ---------------------------------------------------------------------------
// (a) Boot.
// ---------------------------------------------------------------------------

/// How startup_reconcile files one wallet trade record.  It used to keep
/// PENDING_ACCEPT only, so a PENDING_CANCEL trade whose cancel never landed
/// was invisible to the whole engine.
[[nodiscard]] constexpr StartupScanBucket startup_scan_bucket(
    int status_code, bool known) noexcept
{
    if (status_code == kWalletStatusPendingCancel) {
        return StartupScanBucket::PendingCancelObserved;
    }
    if (status_code != kWalletStatusPendingAccept) {
        return StartupScanBucket::Ignore;
    }
    return known ? StartupScanBucket::KnownLive
                 : StartupScanBucket::OrphanCandidate;
}

/// What boot does with a wallet PENDING_CANCEL record, given its offer_log
/// status (nullopt = no row).  Only offers with a row are adopted: sweeping
/// untracked offers the operator keeps on purpose is a policy this does not
/// decide.
[[nodiscard]] inline StartupPendingCancelAction startup_pending_cancel_action(
    const std::optional<std::string>& db_status)
{
    if (!db_status.has_value()) {
        return StartupPendingCancelAction::IgnoreNotOurs;
    }
    if (*db_status == kDbStatusPending) {
        return StartupPendingCancelAction::MarkCancelPending;
    }
    if (*db_status == kDbStatusCancelPending) {
        return StartupPendingCancelAction::AlreadyCancelPending;
    }
    if (*db_status == kDbStatusCancelled) {
        return StartupPendingCancelAction::ReopenMislabelled;
    }
    return StartupPendingCancelAction::Inconsistent;
}

// ---------------------------------------------------------------------------
// Alerts.
// ---------------------------------------------------------------------------

/// [review] A rate limit must not be able to swallow an unresolved offer.
///
/// AlertManager::send_alert returns void and silently drops a CRITICAL alert
/// sent within 60 s of the previous one for the same rule.  With five probes
/// per sweep, eight stranded offers reach Exhausted in two sweeps one
/// heartbeat apart; sending the second batch would be dropped while the
/// offers were already marked alerted, so they would never alert.  The
/// engine therefore queues ids here, sends at most once per
/// kCancelUnresolvedAlertGapMs, and marks an offer alerted only when
/// take_due() hands it out for a send.
class UnresolvedAlertQueue {
public:
    explicit UnresolvedAlertQueue(
        std::int64_t min_gap_ms = kCancelUnresolvedAlertGapMs) noexcept
        : min_gap_ms_(min_gap_ms)
    {
    }

    /// Queue an offer.  Returns false when it is already queued.
    bool enqueue(UnresolvedAlertEntry entry)
    {
        if (contains(entry.offer_id)) {
            return false;
        }
        queued_.push_back(std::move(entry));
        return true;
    }

    /// Drop an offer that resolved before it was announced.
    void forget(const std::string& offer_id)
    {
        queued_.erase(std::remove_if(queued_.begin(), queued_.end(),
                                     [&offer_id](const UnresolvedAlertEntry& e) {
                                         return e.offer_id == offer_id;
                                     }),
                      queued_.end());
    }

    [[nodiscard]] bool contains(const std::string& offer_id) const
    {
        return std::any_of(queued_.begin(), queued_.end(),
                           [&offer_id](const UnresolvedAlertEntry& e) {
                               return e.offer_id == offer_id;
                           });
    }

    [[nodiscard]] std::size_t size() const noexcept { return queued_.size(); }

    /// The entries to announce now, oldest first, at most `max_ids`.  Empty
    /// while the previous send is younger than the gap; whatever is not
    /// returned STAYS queued.
    [[nodiscard]] std::vector<UnresolvedAlertEntry> take_due(std::int64_t now_ms,
                                                             std::size_t  max_ids)
    {
        if (queued_.empty() || max_ids == 0U) {
            return {};
        }
        if (has_sent_ && now_ms - last_sent_ms_ < min_gap_ms_) {
            return {};
        }
        const std::size_t n = std::min(max_ids, queued_.size());
        const auto split = queued_.begin() + static_cast<std::ptrdiff_t>(n);
        std::vector<UnresolvedAlertEntry> out(std::make_move_iterator(queued_.begin()),
                                              std::make_move_iterator(split));
        queued_.erase(queued_.begin(), split);
        has_sent_ = true;
        last_sent_ms_ = now_ms;
        return out;
    }

private:
    std::vector<UnresolvedAlertEntry> queued_{};
    std::int64_t                      min_gap_ms_{kCancelUnresolvedAlertGapMs};
    std::int64_t                      last_sent_ms_{0};
    bool                              has_sent_{false};
};

/// The operator-facing text: every offer id in full, its pair, how many
/// escalations it received, and whether it is STILL TAKEABLE.
[[nodiscard]] inline std::string format_cancel_unresolved_alert(
    const std::vector<UnresolvedAlertEntry>& entries,
    std::size_t                              still_queued)
{
    std::string msg = "[S14] " + std::to_string(entries.size())
        + " cancel-pending offer(s) are UNRESOLVED and need an operator:";
    for (const auto& e : entries) {
        msg += "\n - ";
        msg += e.offer_id;
        msg += " (";
        msg += e.pair_name.empty() ? std::string{"UNKNOWN"} : e.pair_name;
        msg += ", ";
        msg += std::to_string(e.escalations);
        msg += " escalated re-cancel(s)): ";
        switch (e.reason) {
            case UnresolvedReason::StillTakeable:
                msg += "maker coins unspent on-chain -- STILL TAKEABLE, and "
                       "the escalation cap is spent";
                break;
            case UnresolvedReason::Unverifiable:
                msg += "on-chain state unverifiable -- no proof either way, "
                       "so nothing was paid";
                break;
            case UnresolvedReason::WalletNotReconciled:
                msg += "a maker coin is spent on-chain but the wallet still "
                       "reports the trade pending -- not takeable, wallet "
                       "not reconciled";
                break;
            case UnresolvedReason::ResubmitFailing:
                msg += "maker coins unspent on-chain -- STILL TAKEABLE, and "
                       "the re-cancel keeps failing";
                break;
        }
        if (!e.detail.empty()) {
            msg += " [";
            msg += e.detail;
            msg += "]";
        }
    }
    if (still_queued > 0U) {
        msg += "\n(+" + std::to_string(still_queued)
            + " more queued for the next alert)";
    }
    return msg;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_CANCEL_ESCALATION_HPP
