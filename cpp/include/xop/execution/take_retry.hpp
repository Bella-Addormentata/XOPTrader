#ifndef XOP_EXECUTION_TAKE_RETRY_HPP
#define XOP_EXECUTION_TAKE_RETRY_HPP
// ---------------------------------------------------------------------------
// take_retry.hpp -- may we attempt this take, given what we know and what
//                   already failed? Pre-trade funding verdict + a retry policy
//                   that tells a DETERMINISTIC failure from a TRANSIENT one.
//
// [S40 remainder 2026-09-01] WHAT WAS MEASURED
// --------------------------------------------
// logs/xop_trader.log + .1-.5, 2026-08-31T05:15 -> 2026-09-01T20:30 (39h15m),
// read with grep/sed only:
//
//   Step 9c crossed-book detections .................. 105
//   Step 9c take attempts ............................ 105
//   Successes .......................................... 1
//   Failures ......................................... 104  (99.05%)
//     "insufficient funds in wallet 8" ................ 96
//     "Wallet needs to be fully synced ..." ............ 8
//   Distinct counterparty offer ids attempted .......... 8
//   "insufficient" error lines, all steps, ~37h ...... 186
//
// The worst single offer, `Cgp9GrtmnmPf` on XCH/DBX: 50 attempts between
// 13:07:22 and 13:53:16 on 08-31 -- 45m54s, median gap 13.1s -- of which 45
// failed on funding and 5 on wallet sync. Its `ask=97181600000000` was
// BYTE-IDENTICAL across all 50 detections; only the bid moved, so the logged
// edge drifted 235.2 -> 39.0 -> 14.4 bps while the offer itself never changed.
// A second storm, `GxTYZbaujFog`, ran 36 attempts in 15m21s with the entire
// top of book frozen. 150 of the 153 Step-9c info/error lines in the Cgp9
// window (~94%) were one doomed offer being re-selected.
//
// THE TWO CLASSES ARE NOT THE SAME FAILURE, AND MUST NOT SHARE A SCHEDULE
// ----------------------------------------------------------------------
// DETERMINISTIC -- "insufficient funds". take_offer(offer_text, fee) has no
// size parameter (chia_rpc.hpp:499): a Chia offer is an atomic swap of exactly
// the coins the maker committed. So for a FIXED offer, `size` and `price` are
// immutable, and the take is decided by exactly one inequality
//
//     spendable(spend_wallet) >= quote_cost_for_ask(size, price, ...) [+ fee]
//
// whose only mutable input is OUR BALANCE. Not time. Not an attempt counter.
// Arithmetically, for Cgp9: 5e12 * 97181600000000 * 1000 / (1e12 * 1e12) =
// 485,908 DBX mojos. DBX spendable during the storm was 134778 / 236551 /
// 336775 / 338276 -- short by 30.4% at the best moment and 72% at the worst.
// Across the whole 39h window DBX spendable never left 111,214 .. 437,875,
// and NOT ONE of the 96 funding failures was ever fundable. That is why the
// primary mechanism here is a balance comparison recomputed every cycle and
// not a timer: it is self-invalidating, and the cycle our balance covers the
// cost we take, with zero hold-off.
//
// TRANSIENT -- "Wallet needs to be fully synced". Across the same window there
// were 106 sync-gate hits with unsynced_blocks distributed {1:93, 2:4, 3:2,
// 4:2, 5:1, 6:1, 7:1, 8:1, 9:1} -- i.e. 89 of 94 at 1/20, max 4/20 in the
// TODO's narrower count -- grouped into 92 episodes over 35.2h, one every
// 23.0 min, of which 87 lasted a SINGLE cycle (median duration 0s, max 155s).
// A flap, not a condition. If a sync rejection were allowed to accrue toward
// the FUNDING schedule it would suppress a FUNDABLE take roughly every 23
// minutes for no reason at all.
//
// [review 2026-09-01] The first cut drew the wrong conclusion from that and
// gave Unsynced NO bound at all -- it incremented a counter and armed nothing.
// The counter-argument that made this safe was "a thrown get_wallet_balance is
// overwhelmingly the same not-fully-synced condition, so the DeclineUnknown
// branch catches these anyway". THAT IS FALSE, and the repo's own logs say so:
// across xop_trader.log + .1-.5 there are ZERO get_wallet_balance failures of
// any kind. Every one of the 106 "fully synced" lines comes from
// get_spendable_coins (72), Step 8's own sync gate (95), create_offer (20) or
// selective_cancel (2) -- transaction-creating or coin-enumerating endpoints.
// get_wallet_balance is a plain read (chia_rpc.cpp:811) and it kept answering:
// logs/xop_trader.4.log has Step 8 completing create_offer at 13:07:22.004 and
// take_offer rejected for sync 224 ms later. So on a sync rejection the balance
// read SUCCEEDS, the gate says Attempt, and with no accrual the 50-attempt
// storm shape reproduces verbatim on this class. Verified: 500 consecutive
// blocks of a persistent desync produced 500 attempts and zero suppression.
//
// The two classes therefore get SEPARATE schedules, not a shared one and not
// nothing: transient_backoff_base_blocks{2} doubling to {32}, against
// funding_recheck_blocks{8} doubling to {192}. A one-cycle flap -- 87 of the 92
// observed episodes -- costs 2 blocks (~37s) on ONE offer we had just been
// refused anyway; the longest observed episode (155s, ~8 blocks) costs three
// attempts instead of twelve; and a desync that does not clear is bounded to
// one attempt per ~10 min instead of one per 13s. Both counters DECAY once the
// fault stops recurring (see decay_quiet_), so an isolated flap can never
// escalate. See test_take_retry.cpp, TransientAndDeterministicHaveSeparate-
// Schedules and APersistentDesyncCannotStorm.
//
// WHY NOT A BLACKLIST, ON EITHER KEY
// ----------------------------------
// From taker_fills (607 rows, read-only): two counterparty offer ids were each
// taken THREE times at three distinct block heights with three distinct trade
// ids, so an id-keyed blacklist armed on first failure would have blocked 4 of
// our 23 profitable crossed_book fills. And grouping fills by content --
// (pair, price, size, side) -- shows the counterparty re-posting IDENTICAL
// content under FRESH ids (BYC/wUSDC.b 5000 @ 1001000000000, four fills / four
// distinct ids over 67 min), so a content hash would have blocked five more.
// Neither key is sound. A balance comparison needs no offer identity at all,
// which is the strongest argument for it. The offer id is used here ONLY as a
// map key for LOG SUPPRESSION and for the narrow post-RPC case below -- never
// as a permanent verdict, and every entry carries the (price, size)
// fingerprint so a re-post under the same id resets it.
//
// THE ONE CASE A BALANCE CHECK CANNOT COVER
// -----------------------------------------
// Our own just-posted offers lock whole UTXOs that do not leave
// `spendable_balance` until the wallet processes them (coin_lock_ledger.hpp:
// spendable XCH walked 14.59 -> 0 in 73s while every re-query read the same
// stale figure). Step 8 posts immediately before Step 9. So the read can say
// "affordable" and the wallet still reject on funding. That -- and only that
// -- is what `rpc_funding_failures` exists for.
//
// [review 2026-09-01] The first cut released this hold on "our READ RISES
// above what it was when the wallet said no", with a flat 192-block (~1h)
// timer as the fallback. Both halves were wrong, and in the same direction:
//
//   * The rise is STRUCTURALLY UNREACHABLE on the case the hold is for. The
//     read that armed it is stale-HIGH by construction (it still counts the
//     locked UTXOs). As the locks register the read DROPS; as they resolve it
//     climbs back toward -- but not above -- the inflated figure. So the
//     release condition tests for the one thing the coin-lock path cannot do,
//     and every hold ran the full timer.
//   * The timer was ordered AHEAD of the ordinary balance comparison, so an
//     hour of a fundable 235 bps cross was sat out on the strength of one
//     stale reading -- and because Step 8 posts immediately before Step 9 on
//     every cycle, the next attempt after release re-armed it. A permanent
//     ~1h duty cycle of suppression on an offer we could pay for.
//
// Both are fixed here. The hold is now consulted ONLY inside the
// FundingVerdict::Fund branch -- the ordinary comparison adjudicates first, so
// a read that is genuinely short says so, with a truthful shortfall, and the
// hold can never masquerade as one -- and the timer starts at 8 blocks
// (~2.5 min, roughly 2x the 73s of staleness coin_lock_ledger.hpp measured),
// doubling per CONSECUTIVE proven failure to a 192-block ceiling and decaying
// back to zero once the wallet stops rejecting. It is not a blacklist: any of
// four inputs releases it (fingerprint, a read above the rejected one, the
// timer, or the decay), and it declines under its OWN enumerator,
// TakeGate::DeclineFundingHold, so the log can name the state it is in.
//
// FAIL CLOSED ON UNKNOWN, AND WHY
// -------------------------------
// coin_pool_verdict.hpp states this repo's rule after the eleventh fail-open:
// "a number may never be substituted for a failed read. Unknown is its own
// state, it is the DEFAULT state, and it authorises nothing." SpendableReading
// therefore defaults `read_ok{false}`, decide_funding() tests it FIRST, and an
// unknown balance returns FundingVerdict::Unknown, which the gate renders as
// TakeGate::DeclineUnknown -- a decline, logged distinctly from
// DeclineInsufficient, and accruing NOTHING.
//
// [review 2026-09-01] The justification originally offered for this -- "a
// thrown get_wallet_balance is overwhelmingly the same 'fully synced'
// condition, and such a take was rejected 8 times out of 8, so declining costs
// nothing" -- IS NOT SUPPORTED BY THE LOGS and has been withdrawn. There is
// exactly one balance-read failure in the whole 39h corpus (a CURL timeout at
// 13:51:20.284, from the recovery path, not this one) and zero
// get_wallet_balance sync rejections; the "fully synced" text belongs to
// get_spendable_coins, create_offer and selective_cancel. A failed balance read
// here is a RARE TRANSPORT EVENT, not a sync flap in disguise, and it does not
// stand in for the sync class -- that class is bounded on its own schedule
// above.
//
// The resolution is unchanged and rests on the fail-closed rule alone, which is
// sufficient: an unread balance is not a passed check, and the cost of being
// wrong is ONE cycle (~13s) against an unfunded spend attempt on the money
// path. Note the opposite resolution -- treating unknown as "cannot afford" --
// would be wrong in a different way: it would accrue toward the deterministic
// schedule and suppress a fundable offer. Unknown is a THIRD state precisely so
// it can decline without lying about why.
//
// Every enum in this file is ordered so that the ZERO enumerator declines:
// TakeGate::DeclineUnknown = 0, TakeFailureClass::Other = 0. A
// default-constructed or half-initialised TakeDecision suppresses; it never
// authorises. Same discipline as CrossedBookDecision{verdict{NoBook}} and
// CoinPoolReading{read_ok{false}}.
//
// LOG EDGES AND A HEARTBEAT, NEVER A LEVEL
// ----------------------------------------
// engine.cpp's `breaker_skip_warned_` is this repo's cautionary tale: an
// edge-triggered warning with no off-edge compressed a 4h10m total-quoting
// outage into one line nobody saw. pid_rail_latch.hpp draws the conclusion --
// "log edges AND heartbeat the level" -- and it is copied here rather than
// generalised from there, because that latch is a single-instance hysteresis
// band over a continuous scalar whose exit is INTRINSIC, and this is a
// multi-key discrete-event map whose exit is EXTRINSIC (our balance rose, or
// the offer changed). A latch that can only exit on its own accumulator cannot
// express the one clause that matters here.
//
// [review 2026-09-01] The first cut then reproduced breaker_skip_warned_ by a
// different route. A change of decline REASON took the same branch as a first
// suppression and reset suppress_start_block, last_emit_block AND
// suppressed_cycles. Two consequences, both measured on the unmodified header:
// 400 blocks of a permanently unaffordable offer with the balance read failing
// one cycle in 40 emitted 20 Edge lines, ZERO heartbeats, and told the operator
// `cycles=39 blocks=38` about a suppression that had run 400 blocks. Any reason
// flip more often than heartbeat_blocks starved the heartbeat permanently and
// capped the reported age at one flap interval.
//
// So the clock and the reason are now separate pieces of state. The clock --
// suppress_start_block, last_emit_block -- belongs to "suppressed AT ALL" and
// moves only on a false->true transition, on an emission, and on
// reset_log_state_. The reason drives only WHETHER an extra Edge is emitted,
// and that is rate-limited by reason_change_debounce_blocks so an alternating
// gate cannot re-arm a line every cycle (it did: 50 alternating cycles emitted
// 50 Edge lines, half of them at warn -- worse than the 3-lines-per-cycle it
// replaced). The heartbeat text carries THIS cycle's reason regardless, so
// nothing is lost by debouncing the edge.
//
// Pure header: plain integers, std::string keys, no engine types, no asio, no
// RPC, no spdlog. Driven directly by cpp/tests/test_take_retry.cpp.
//
// WHAT THESE TESTS DO NOT COVER. Nothing in cpp/tests constructs an Engine
// (TODO S36), so the Step 9c / 9e / 9f CALL SITES remain unguarded lines: the
// suite stays green if the wiring is deleted. What the tests can hold is the
// policy itself and, critically, the DENOMINATION of the cost -- see
// ask_take_cost() below and CostIsQuoteDenominatedNotBase.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xop/types.hpp"
#include "xop/execution/take_sizing.hpp"

namespace xop::execution {

// ---------------------------------------------------------------------------
// Failure classification.
//
// `Other` is the ZERO enumerator and the DEFAULT return. An unrecognised
// message must never land in `Funding`: Funding is the class that suppresses,
// and a message we do not understand being read as "we are broke" is how a
// tradable offer gets silently held off. Unrecognised failures get the small
// generic backoff instead -- they still burned two RPCs -- but they never
// enter the funding state machine.
// ---------------------------------------------------------------------------
enum class TakeFailureClass : int {
    Other    = 0,  ///< we do not model this. Small generic backoff.
    Funding  = 1,  ///< DETERMINISTIC for a fixed offer. Balance-keyed.
    Unsynced = 2,  ///< TRANSIENT. Free retry. Accrues nothing.
};

/// ASCII, locale-independent, allocation-free substring search.
/// Deliberately not std::tolower: the RPC text is ASCII and a locale-sensitive
/// classifier on a money path is a portability trap, not a feature.
[[nodiscard]] inline bool contains_ci(std::string_view hay,
                                      std::string_view needle) noexcept
{
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    const auto lower = [](char c) noexcept -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    const std::size_t last = hay.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        std::size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (lower(hay[i + j]) != lower(needle[j])) break;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

/// Classify a take_offer / balance-read exception message.
///
/// The live strings, verbatim from the 39h window:
///   "insufficient funds in wallet 8"
///   "Wallet needs to be fully synced before making transactions."
/// The first two needles are lifted from this repo's only existing
/// insufficient-funds classifier (offer_manager.cpp, emergency cancel).
///
/// Unsynced is tested FIRST. A message carrying both would be a wallet that is
/// unsynced AND short; the sync condition is the one that will clear on its
/// own, and misfiling it as Funding is the expensive direction.
[[nodiscard]] inline TakeFailureClass
classify_take_failure(std::string_view msg) noexcept
{
    if (contains_ci(msg, "fully synced") || contains_ci(msg, "not synced")) {
        return TakeFailureClass::Unsynced;
    }
    if (contains_ci(msg, "insufficient funds")
        || contains_ci(msg, "spendable balance")) {
        return TakeFailureClass::Funding;
    }
    return TakeFailureClass::Other;
}

[[nodiscard]] inline const char* to_string(TakeFailureClass c) noexcept
{
    switch (c) {
        case TakeFailureClass::Funding:  return "deterministic/funding";
        case TakeFailureClass::Unsynced: return "transient/unsynced";
        case TakeFailureClass::Other:    break;
    }
    return "unclassified";
}

// ---------------------------------------------------------------------------
// What we know about our spendable balance. read_ok defaults to FALSE.
//
// This is the inverse of `bal.value("spendable_balance", 0)`, which silently
// substitutes zero for a missing field. In Step 9e/9f that defaulted zero
// happens to decline -- it reads as "broke" -- so it is a latent bug pointing
// the safe way, but it is still a number standing in for a failed read, and
// it produces the WRONG log line and the WRONG accrual class.
// ---------------------------------------------------------------------------
struct SpendableReading {
    bool read_ok{false};   ///< did the balance read actually succeed?
    Mojo spendable{0};     ///< meaningless unless read_ok
};

enum class FundingVerdict : int {
    Unknown      = 0,  ///< we do not know. Decline, accrue nothing.
    Insufficient = 1,  ///< known, and strictly short.
    Fund         = 2,  ///< known, and sufficient.
};

// ---------------------------------------------------------------------------
// decide_funding -- the pre-trade check, as one total function.
//
// The !read_ok clause is FIRST and unconditional. Any later clause that can
// reach Fund from an unknown reading resurrects the S41 family.
//
// cost <= 0 is Unknown, NOT Fund. quote_cost_for_ask() returns 0 for inputs it
// cannot price (non-positive size or price, NaN intermediate), and "we could
// not compute what this costs" is not "it is free". Making this branch return
// Fund is the single most dangerous mutation in this file, because it turns a
// broken price into an unpriced take.
//
// A negative spendable is a malformed response, not a debt. Unknown.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr FundingVerdict
decide_funding(SpendableReading reading, Mojo cost) noexcept
{
    if (!reading.read_ok)      return FundingVerdict::Unknown;
    if (cost <= 0)             return FundingVerdict::Unknown;
    if (reading.spendable < 0) return FundingVerdict::Unknown;
    return (reading.spendable < cost) ? FundingVerdict::Insufficient
                                      : FundingVerdict::Fund;
}

// ---------------------------------------------------------------------------
// ask_take_cost -- what lifting an ASK actually costs the spend wallet.
//
// THE DENOMINATION IS THE WHOLE POINT AND IT IS WHY THIS WRAPPER EXISTS.
// Step 9c's `take_size` is best_ask_size in BASE mojos. 9c always lifts an
// ask, so it always spends QUOTE. Writing `cost = take_size` at the call site
// would compare 5,000,000,000,000 base mojos against a 338,276-mojo DBX wallet
// and decline EVERY crossed-book take on EVERY pair, forever, while looking
// exactly like a working guard. That is a silent kill switch on the money
// path, and it is a one-token edit away at the call site. Keeping the
// conversion behind a named function that a test can hold is the only defence
// available while nothing in cpp/tests can construct an Engine.
//
// @param same_wallet_fee  The network fee is paid in XCH from wallet 1. Pass
//        it ONLY when the asset we are spending IS xch, i.e. when the fee and
//        the spend come out of the SAME wallet; pass 0 otherwise. On XCH/DBX
//        the spend is DBX and a single-wallet check structurally cannot see
//        the fee, so 0 is correct and this term is inert today. It is not
//        inert on the wmilliETH.b/XCH family (config.yaml, currently
//        enabled:false), where the quote IS xch and the requirement really is
//        cost + fee from one wallet. Step 9e and 9f both omitted this.
//
// @return 0 when the cost cannot be computed. Zero is a DECLINE via
//         decide_funding()'s `cost <= 0` clause -- never "free".
// ---------------------------------------------------------------------------
[[nodiscard]] inline Mojo ask_take_cost(Mojo         base_size,
                                        Mojo         price,
                                        std::int64_t base_mojos_per_unit,
                                        std::int64_t quote_mojos_per_unit,
                                        Mojo         same_wallet_fee) noexcept
{
    const Mojo cost = quote_cost_for_ask(base_size, price,
                                         base_mojos_per_unit,
                                         quote_mojos_per_unit);
    if (cost <= 0) return 0;               // unpriceable -> decline
    if (same_wallet_fee <= 0) return cost;
    // Saturating add. A wrapped cost would read as affordable.
    const Mojo headroom = std::numeric_limits<Mojo>::max() - cost;
    return (same_wallet_fee > headroom) ? std::numeric_limits<Mojo>::max()
                                        : cost + same_wallet_fee;
}

/// Same, for a spend denominated in the BASE asset (Step 9e/9f bid takes).
/// Kept beside ask_take_cost so the two sites cannot drift apart; each call
/// site still chooses its own denomination.
[[nodiscard]] inline Mojo add_same_wallet_fee(Mojo cost,
                                              Mojo same_wallet_fee) noexcept
{
    if (cost <= 0) return 0;
    if (same_wallet_fee <= 0) return cost;
    const Mojo headroom = std::numeric_limits<Mojo>::max() - cost;
    return (same_wallet_fee > headroom) ? std::numeric_limits<Mojo>::max()
                                        : cost + same_wallet_fee;
}

// ---------------------------------------------------------------------------
// The offer's identity for retry purposes: NOT its id, but the two numbers
// that decide affordability. Both are immutable for a given Chia offer, so a
// change here means the counterparty re-posted -- and everything we learned
// about the old one is void.
// ---------------------------------------------------------------------------
struct OfferFingerprint {
    Mojo price{0};
    Mojo size{0};
};

[[nodiscard]] constexpr bool operator==(const OfferFingerprint& a,
                                        const OfferFingerprint& b) noexcept
{
    return a.price == b.price && a.size == b.size;
}
[[nodiscard]] constexpr bool operator!=(const OfferFingerprint& a,
                                        const OfferFingerprint& b) noexcept
{
    return !(a == b);
}

// ---------------------------------------------------------------------------
// Tuning. Blocks, not seconds -- everything else in this step is block-keyed.
// Chia blocks are ~18.75s, so 192 blocks is roughly an hour.
// ---------------------------------------------------------------------------
struct TakeRetryConfig {
    /// Generic backoff for TakeFailureClass::Other, doubling per consecutive
    /// failure. 0 disables it entirely.
    std::uint32_t other_backoff_base_blocks{4};
    std::uint32_t other_backoff_max_blocks{64};
    /// Hold for an RPC-proven funding failure whose balance read did NOT
    /// predict it (the coin-lock-lag case). Base is ~2.5 min -- about 2x the
    /// 73s of coin-lock staleness coin_lock_ledger.hpp measured -- doubling
    /// per CONSECUTIVE proven failure to the ceiling. It is not an hour on
    /// first contact: an offer we can afford must not be sat out because one
    /// stale reading disagreed with the wallet once.
    std::uint32_t funding_recheck_blocks{8};
    std::uint32_t funding_recheck_max_blocks{192};
    /// Hold after a "wallet not fully synced" rejection. Deliberately an order
    /// of magnitude smaller than the funding schedule and deliberately NOT
    /// zero: 87 of 92 observed episodes were one cycle long, so 2 blocks costs
    /// ~37s on an offer the wallet just refused, while a desync that does NOT
    /// clear is bounded to one attempt per ~10 min instead of one per 13s.
    std::uint32_t transient_backoff_base_blocks{2};
    std::uint32_t transient_backoff_max_blocks{32};
    /// While suppressed, say so once every this many blocks. 0 disables --
    /// DO NOT. See the header note on breaker_skip_warned_.
    std::uint32_t heartbeat_blocks{192};
    /// A change of decline REASON while already suppressed re-emits an Edge,
    /// but at most this often. 0 means "every change", which is what the first
    /// cut did and which turns an alternating gate into a per-cycle storm.
    std::uint32_t reason_change_debounce_blocks{16};
    /// Drop entries not seen for this long. Guards against a permanently
    /// degraded book feed pinning the map; ~3h.
    std::uint32_t stale_after_blocks{600};
    /// Hard ceiling. Defence against a pathological or adversarial book.
    std::size_t   max_entries{512};
};

/// Doubling, saturating. Shared by all three schedules so they cannot drift
/// apart in shape while keeping their own constants.
[[nodiscard]] inline std::uint32_t
doubling_backoff_blocks(std::uint32_t consecutive,
                        std::uint32_t base,
                        std::uint32_t ceiling) noexcept
{
    if (consecutive == 0 || base == 0) return 0;
    std::uint64_t b = base;
    for (std::uint32_t i = 1; i < consecutive && b < ceiling; ++i) b *= 2;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(b, ceiling));
}

/// Exponential, saturating at other_backoff_max_blocks.
[[nodiscard]] inline std::uint32_t
other_backoff_blocks(std::uint32_t consecutive,
                     const TakeRetryConfig& cfg) noexcept
{
    return doubling_backoff_blocks(consecutive,
                                   cfg.other_backoff_base_blocks,
                                   cfg.other_backoff_max_blocks);
}

/// Exponential, saturating at funding_recheck_max_blocks.
[[nodiscard]] inline std::uint32_t
funding_hold_blocks(std::uint32_t consecutive,
                    const TakeRetryConfig& cfg) noexcept
{
    return doubling_backoff_blocks(consecutive,
                                   cfg.funding_recheck_blocks,
                                   cfg.funding_recheck_max_blocks);
}

/// Exponential, saturating at transient_backoff_max_blocks.
[[nodiscard]] inline std::uint32_t
transient_backoff_blocks(std::uint32_t consecutive,
                         const TakeRetryConfig& cfg) noexcept
{
    return doubling_backoff_blocks(consecutive,
                                   cfg.transient_backoff_base_blocks,
                                   cfg.transient_backoff_max_blocks);
}

// ---------------------------------------------------------------------------
// The gate's verdict. Zero enumerator declines.
// ---------------------------------------------------------------------------
// [review 2026-09-01] DeclineFundingHold and DeclineSyncBackoff are NOT
// cosmetic. The first cut rendered the coin-lock hold as DeclineInsufficient,
// and because that hold is by construction reachable only when the read said
// AFFORDABLE, the shortfall guard left shortfall at 0 and the call site printed
// "insufficient balance: need 485908 spendable 485913 short 0 ... Clears the
// cycle the balance covers it" -- a line that contradicts itself, claims a
// release condition that is already true, and is indistinguishable from the
// benign short-balance case that fires 96 times in 104. The repo's fail-open
// rule is exactly this: an error path must never produce the same observable
// outcome as a different one. One enumerator per state, one message per
// enumerator.
enum class TakeGate : int {
    DeclineUnknown      = 0,  ///< balance unreadable / cost unpriceable
    DeclineInsufficient = 1,  ///< known and short -- the deterministic class
    DeclineBackoff      = 2,  ///< an unmodelled failure is still backing off
    Attempt             = 3,  ///< proceed to the Dexie fetch and take_offer
    DeclineFundingHold  = 4,  ///< read says affordable, the WALLET said no
    DeclineSyncBackoff  = 5,  ///< the wallet was mid-sync; short hold
};

[[nodiscard]] inline const char* to_string(TakeGate g) noexcept
{
    switch (g) {
        case TakeGate::DeclineInsufficient: return "insufficient";
        case TakeGate::DeclineBackoff:      return "backoff";
        case TakeGate::Attempt:             return "attempt";
        case TakeGate::DeclineFundingHold:  return "funding-hold";
        case TakeGate::DeclineSyncBackoff:  return "sync-backoff";
        case TakeGate::DeclineUnknown:      break;
    }
    return "unknown";
}

/// What, if anything, the call site should print this cycle.
enum class TakeLogEvent : int {
    None      = 0,  ///< suppressed, and already reported
    Edge      = 1,  ///< first cycle of a suppression, or the reason changed
    Heartbeat = 2,  ///< still suppressed, N blocks on
    Resume    = 3,  ///< suppression cleared -- the off-edge
};

struct TakeDecision {
    TakeGate      gate{TakeGate::DeclineUnknown};   // default declines
    TakeLogEvent  log{TakeLogEvent::None};
    Mojo          cost{0};
    Mojo          spendable{0};      ///< 0 unless the read succeeded
    Mojo          shortfall{0};      ///< cost - spendable, when known and short
    /// The reading the WALLET rejected, for DeclineFundingHold only. The log
    /// cannot describe that state without it -- see the enum note above.
    Mojo          rejected_at{0};
    /// Block this decline releases on, for the two held classes. 0 otherwise.
    std::uint64_t ready_block{0};
    std::uint32_t suppressed_cycles{0};
    std::uint32_t suppressed_blocks{0};
};

// ---------------------------------------------------------------------------
// Per-offer retry state. Nothing here is a permanent verdict.
// ---------------------------------------------------------------------------
// [review 2026-09-01] `cost_required` and `last_attempt_block` were here and
// were written by every call and READ BY NOTHING -- not by the engine, not by
// the tests, not by find(). A write-only member on a money path is a claim of
// observability that is not true, and it is indistinguishable from a field
// someone forgot to wire. They are deleted rather than "surfaced": the cost is
// already on TakeDecision, which is what the call site logs.
struct TakeRetryEntry {
    std::string      pair;
    OfferFingerprint fp{};

    // -- deterministic class, post-RPC only ------------------------------
    std::uint32_t    rpc_funding_failures{0};
    Mojo             spendable_at_rpc_failure{0};
    bool             spendable_at_rpc_failure_known{false};
    std::uint64_t    funding_ready_block{0};

    // -- unmodelled class -------------------------------------------------
    std::uint32_t    other_failures{0};
    std::uint64_t    other_ready_block{0};

    // -- transient class ---------------------------------------------------
    // This DOES have a ready-block, and it is a short one on its own
    // constants. It must never be merged into the funding schedule -- that
    // would suppress a fundable take every ~23 minutes -- but leaving it with
    // no bound at all reproduced the storm verbatim on this class. See the
    // header note.
    std::uint32_t    transient_failures{0};
    std::uint64_t    transient_ready_block{0};

    // -- log suppression --------------------------------------------------
    // suppress_start_block and last_emit_block belong to "suppressed AT ALL"
    // and survive a change of reason; suppressed_gate drives only whether an
    // extra Edge is due. Resetting the first two on a reason change starved
    // the heartbeat and under-reported the age by an order of magnitude.
    bool             suppressed{false};
    TakeGate         suppressed_gate{TakeGate::DeclineUnknown};
    std::uint32_t    suppressed_cycles{0};
    std::uint64_t    suppress_start_block{0};
    std::uint64_t    last_emit_block{0};

    std::uint64_t    last_touch_block{0};
};

// ---------------------------------------------------------------------------
// TakeRetryBook -- the bounded state map.
//
// Keyed by pair + offer id, because a Dexie offer id is unique per offer but
// the same id appearing under two pairs must not clobber one entry with the
// other's fingerprint (which would reset the suppression and resume a storm).
// ---------------------------------------------------------------------------
class TakeRetryBook {
public:
    [[nodiscard]] static std::string key_of(std::string_view pair,
                                            std::string_view offer_id)
    {
        std::string k;
        k.reserve(pair.size() + 1 + offer_id.size());
        k.append(pair.data(), pair.size());
        k.push_back('\x1f');                 // unit separator: not in an id
        k.append(offer_id.data(), offer_id.size());
        return k;
    }

    // -----------------------------------------------------------------------
    // gate -- the one call the taker makes per candidate per cycle.
    //
    // Order is load-bearing:
    //   0. fingerprint change wipes everything we knew about this offer, then
    //      any schedule whose fault has stopped recurring decays away.
    //   1. unmodelled-failure backoff (it burned RPCs; hold briefly).
    //   2. THE ORDINARY PRE-TRADE COMPARISON, recomputed from THIS cycle's
    //      snapshot. Self-invalidating, and it now runs BEFORE the two held
    //      classes, not after: a read that is genuinely short must be reported
    //      as short, with a truthful shortfall, and must never be able to hide
    //      behind a timer. [review] The first cut ran the funding hold first,
    //      which both sat out fundable crosses for an hour and printed
    //      "insufficient ... short 0" while doing it.
    //   3. only if the comparison says FUND: the RPC-proven funding hold (the
    //      wallet rejected a take our read said we could pay for), then the
    //      short transient hold. Each declines under its own enumerator.
    // -----------------------------------------------------------------------
    TakeDecision gate(std::string_view       pair,
                      std::string_view       offer_id,
                      OfferFingerprint       fp,
                      SpendableReading       reading,
                      Mojo                   cost,
                      std::uint64_t          block,
                      const TakeRetryConfig& cfg)
    {
        TakeDecision d{};
        d.cost      = cost;
        d.spendable = reading.read_ok ? reading.spendable : 0;

        TakeRetryEntry& e = entries_[key_of(pair, offer_id)];
        e.pair.assign(pair.data(), pair.size());
        e.last_touch_block = block;

        if (e.fp != fp) {
            reset_failure_state_(e);
            reset_log_state_(e);
            e.fp = fp;
        }
        decay_quiet_(e, block, cfg);

        TakeGate g = TakeGate::Attempt;
        if (e.other_failures > 0 && block < e.other_ready_block) {
            g           = TakeGate::DeclineBackoff;
            d.ready_block = e.other_ready_block;
        } else {
            switch (decide_funding(reading, cost)) {
                case FundingVerdict::Unknown:
                    g = TakeGate::DeclineUnknown;      break;
                case FundingVerdict::Insufficient:
                    g = TakeGate::DeclineInsufficient; break;
                case FundingVerdict::Fund:
                    // We can pay for it as far as the READ is concerned. Two
                    // things can still hold it, and each says so in its own
                    // words.
                    if (e.rpc_funding_failures > 0
                        && e.spendable_at_rpc_failure_known
                        && reading.spendable <= e.spendable_at_rpc_failure
                        && block < e.funding_ready_block) {
                        g             = TakeGate::DeclineFundingHold;
                        d.rejected_at = e.spendable_at_rpc_failure;
                        d.ready_block = e.funding_ready_block;
                    } else if (e.transient_failures > 0
                               && block < e.transient_ready_block) {
                        g             = TakeGate::DeclineSyncBackoff;
                        d.ready_block = e.transient_ready_block;
                    } else {
                        g = TakeGate::Attempt;
                    }
                    break;
            }
        }
        d.gate = g;

        // Only the ordinary comparison can produce a shortfall. The held
        // classes are reachable only from FundingVerdict::Fund, so a non-zero
        // shortfall on one of them would be arithmetically impossible -- which
        // is precisely why they must not share its enumerator or its message.
        if (g == TakeGate::DeclineInsufficient && reading.read_ok
            && cost > reading.spendable) {
            d.shortfall = cost - reading.spendable;
        }

        if (g == TakeGate::Attempt) {
            if (e.suppressed) {
                d.log               = TakeLogEvent::Resume;
                d.suppressed_cycles = e.suppressed_cycles;
                d.suppressed_blocks = span_(e.suppress_start_block, block);
                reset_log_state_(e);
            }
            return d;
        }

        if (!e.suppressed) {
            e.suppressed           = true;
            e.suppressed_gate      = g;
            e.suppressed_cycles    = 1;
            e.suppress_start_block = block;
            e.last_emit_block      = block;
            d.log                  = TakeLogEvent::Edge;
        } else {
            ++e.suppressed_cycles;
            // The clock is NOT touched here. It belongs to the suppression,
            // not to the reason, and resetting it starved the heartbeat and
            // made cycles=/blocks= under-report by an order of magnitude.
            //
            // `suppressed_gate` is the reason we last REPORTED, not the reason
            // we last saw. It is assigned only where a line is emitted, so a
            // change that arrives inside the debounce stays pending and is
            // reported when the debounce expires instead of being swallowed.
            const std::uint64_t since =
                (block >= e.last_emit_block) ? block - e.last_emit_block : 0;
            if (e.suppressed_gate != g
                && since >= cfg.reason_change_debounce_blocks) {
                e.suppressed_gate = g;
                e.last_emit_block = block;
                d.log             = TakeLogEvent::Edge;
            } else if (cfg.heartbeat_blocks > 0
                       && since >= cfg.heartbeat_blocks) {
                // The heartbeat text carries THIS cycle's reason, so it
                // reports the change too.
                e.suppressed_gate = g;
                e.last_emit_block = block;
                d.log             = TakeLogEvent::Heartbeat;
            }
        }
        d.suppressed_cycles = e.suppressed_cycles;
        d.suppressed_blocks = span_(e.suppress_start_block, block);
        return d;
    }

    // -----------------------------------------------------------------------
    // note_failure -- record what take_offer (or the Dexie fetch inside the
    // same try) actually threw. This is the ONLY place the classification is
    // applied, because the log cannot carry it: Step 9c's catch-all emits
    // funding and sync failures through the identical spdlog::error with the
    // identical message shape and the identical control flow.
    // -----------------------------------------------------------------------
    void note_failure(std::string_view       pair,
                      std::string_view       offer_id,
                      OfferFingerprint       fp,
                      TakeFailureClass       cls,
                      SpendableReading       reading,
                      std::uint64_t          block,
                      const TakeRetryConfig& cfg)
    {
        TakeRetryEntry& e = entries_[key_of(pair, offer_id)];
        e.pair.assign(pair.data(), pair.size());
        e.last_touch_block = block;
        if (e.fp != fp) {
            reset_failure_state_(e);
            reset_log_state_(e);
            e.fp = fp;
        }

        switch (cls) {
            case TakeFailureClass::Funding:
                ++e.rpc_funding_failures;
                e.spendable_at_rpc_failure_known = reading.read_ok;
                e.spendable_at_rpc_failure =
                    reading.read_ok ? reading.spendable : 0;
                e.funding_ready_block =
                    block + funding_hold_blocks(e.rpc_funding_failures, cfg);
                break;

            case TakeFailureClass::Unsynced:
                // Its OWN schedule, an order of magnitude shorter than the
                // funding one and decaying to nothing between episodes. It
                // must not accrue toward `funding_ready_block` -- 92 sync
                // episodes in 35.2h means that would suppress a fundable take
                // every ~23 minutes -- but it must not be free either, or a
                // desync that does not clear reproduces the storm at one
                // attempt per cycle. See the header note.
                ++e.transient_failures;
                e.transient_ready_block =
                    block + transient_backoff_blocks(e.transient_failures,
                                                     cfg);
                break;

            case TakeFailureClass::Other:
                ++e.other_failures;
                e.other_ready_block =
                    block + other_backoff_blocks(e.other_failures, cfg);
                break;
        }
    }

    /// A take settled. Everything we knew about this offer is spent.
    void note_success(std::string_view pair, std::string_view offer_id)
    {
        entries_.erase(key_of(pair, offer_id));
    }

    // -----------------------------------------------------------------------
    // retain_live -- prune to the offer ids actually on this pair's book.
    //
    // CALL ONLY ON A CYCLE WHERE THE BOOK READ NON-EMPTY. offer_manager.cpp's
    // equivalent clear()s its counters when its input list is empty; copying
    // that here would let one transient empty Dexie snapshot wipe every
    // suppression and resume the storm on the next non-empty cycle. Step 9c
    // already `continue`s on an empty book, so the guard is natural.
    //
    // Only entries for THIS pair are considered; another pair's entries are
    // not in this pair's book and must survive.
    // -----------------------------------------------------------------------
    void retain_live(std::string_view pair,
                     const std::unordered_set<std::string>& live_offer_ids)
    {
        // [review 2026-09-01] THE PRECONDITION IS NOW ENFORCED HERE, not just
        // documented. It was previously held only by an untested `continue`
        // at the single call site, and this function pruned an empty set to
        // nothing exactly like the offer_manager.cpp mistake the comment above
        // warns against -- so the invariant the header calls load-bearing was
        // one edit away at all times and no test could see it. An empty book
        // read is a FEED failure, not proof that every suppressed offer left
        // the book; entries that really are gone still age out via sweep().
        if (live_offer_ids.empty()) return;

        for (auto it = entries_.begin(); it != entries_.end();) {
            const bool same_pair =
                it->second.pair.size() == pair.size()
                && std::equal(pair.begin(), pair.end(),
                              it->second.pair.begin());
            if (!same_pair) { ++it; continue; }
            const std::string_view key{it->first};
            const std::size_t sep = key.find('\x1f');
            const std::string id{sep == std::string_view::npos
                                     ? std::string_view{}
                                     : key.substr(sep + 1)};
            if (live_offer_ids.count(id) == 0) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // -----------------------------------------------------------------------
    // sweep -- age-out plus the hard cap. Safe to call every cycle, including
    // cycles where the book read empty.
    //
    // Two rules for opposite failure modes, and each alone reintroduces the
    // other's: retain_live bounds growth but cannot run on an empty book;
    // the age-out survives a feed blip but would not bound an adversarial
    // book on its own. The cap is the backstop for both.
    //
    // [review 2026-09-01] The two counts are returned SEPARATELY because they
    // mean opposite things. An age-out is routine: the offer left the book
    // hours ago. A cap eviction discards a LIVE suppression -- possibly one
    // that is still crossed and still unfundable -- which resumes the storm it
    // was holding. The first cut returned one total and the call site
    // discarded it, so the only eviction that matters was silent.
    // -----------------------------------------------------------------------
    struct SweepResult {
        std::size_t aged{0};    ///< routine
        std::size_t capped{0};  ///< a live suppression was discarded
        [[nodiscard]] std::size_t total() const noexcept
        {
            return aged + capped;
        }
    };

    SweepResult sweep(std::uint64_t block, const TakeRetryConfig& cfg)
    {
        SweepResult dropped{};

        if (cfg.stale_after_blocks > 0) {
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (block > it->second.last_touch_block
                    && (block - it->second.last_touch_block)
                           > cfg.stale_after_blocks) {
                    it = entries_.erase(it);
                    ++dropped.aged;
                } else {
                    ++it;
                }
            }
        }

        if (cfg.max_entries > 0 && entries_.size() > cfg.max_entries) {
            std::vector<std::pair<std::uint64_t, const std::string*>> ages;
            ages.reserve(entries_.size());
            for (const auto& kv : entries_) {
                ages.emplace_back(kv.second.last_touch_block, &kv.first);
            }
            const std::size_t excess = entries_.size() - cfg.max_entries;
            std::partial_sort(
                ages.begin(),
                ages.begin() + static_cast<std::ptrdiff_t>(excess),
                ages.end(),
                [](const auto& a, const auto& b) noexcept {
                    return a.first < b.first;
                });
            std::vector<std::string> doomed;
            doomed.reserve(excess);
            for (std::size_t i = 0; i < excess; ++i) {
                doomed.push_back(*ages[i].second);
            }
            for (const auto& k : doomed) {
                entries_.erase(k);
                ++dropped.capped;
            }
        }
        return dropped;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] const TakeRetryEntry* find(std::string_view pair,
                                             std::string_view offer_id) const
    {
        const auto it = entries_.find(key_of(pair, offer_id));
        return it == entries_.end() ? nullptr : &it->second;
    }

    void clear() noexcept { entries_.clear(); }

private:
    static std::uint32_t span_(std::uint64_t from, std::uint64_t to) noexcept
    {
        if (to <= from) return 0;
        const std::uint64_t d = to - from;
        return d > std::numeric_limits<std::uint32_t>::max()
                   ? std::numeric_limits<std::uint32_t>::max()
                   : static_cast<std::uint32_t>(d);
    }

    static void reset_failure_state_(TakeRetryEntry& e) noexcept
    {
        e.rpc_funding_failures           = 0;
        e.spendable_at_rpc_failure       = 0;
        e.spendable_at_rpc_failure_known = false;
        e.funding_ready_block            = 0;
        e.other_failures                 = 0;
        e.other_ready_block              = 0;
        e.transient_failures             = 0;
        e.transient_ready_block          = 0;
    }

    // -----------------------------------------------------------------------
    // decay_quiet_ -- make "consecutive" actually mean consecutive.
    //
    // [review 2026-09-01] All three counters were cumulative over the entry's
    // whole lifetime. Nothing decayed them: reset_failure_state_ is reachable
    // only from a fingerprint change, and note_success only from a settled
    // take, so a long-lived maker offer we never took accumulated forever.
    // Measured on the unmodified header: five ISOLATED transport blips, 210
    // blocks apart with ten clean Attempt-returning gates between each,
    // escalated the hold on a fully funded 235 bps cross to the 64-block
    // (~20 min) ceiling -- while the config field was named `consecutive` and
    // documented "doubling per consecutive failure". Worse, a transport fault
    // hits every offer on the book at once, so it accrued per-offer state for
    // a condition that had nothing to do with any offer.
    //
    // The rule: once a hold has expired AND a further full hold-span has
    // passed with no new failure of that class, the fault did not recur and
    // the counter is stale. A genuinely persistent fault re-fires inside that
    // window and keeps escalating, so the exponential is preserved exactly
    // where it is wanted. Integer arithmetic only, and the subtraction is
    // guarded -- a clamp that rounded through a 64-bit boundary is a bug this
    // repo has already shipped once.
    // -----------------------------------------------------------------------
    static void decay_quiet_(TakeRetryEntry&        e,
                             std::uint64_t          block,
                             const TakeRetryConfig& cfg) noexcept
    {
        const auto quiet_for = [block](std::uint64_t ready) -> std::uint64_t {
            return (block >= ready) ? (block - ready)
                                    : std::uint64_t{0};
        };

        if (e.other_failures > 0 && block >= e.other_ready_block
            && quiet_for(e.other_ready_block)
                   >= other_backoff_blocks(e.other_failures, cfg)) {
            e.other_failures    = 0;
            e.other_ready_block = 0;
        }

        if (e.transient_failures > 0 && block >= e.transient_ready_block
            && quiet_for(e.transient_ready_block)
                   >= transient_backoff_blocks(e.transient_failures, cfg)) {
            e.transient_failures    = 0;
            e.transient_ready_block = 0;
        }

        if (e.rpc_funding_failures > 0 && block >= e.funding_ready_block
            && quiet_for(e.funding_ready_block)
                   >= funding_hold_blocks(e.rpc_funding_failures, cfg)) {
            e.rpc_funding_failures           = 0;
            e.spendable_at_rpc_failure       = 0;
            e.spendable_at_rpc_failure_known = false;
            e.funding_ready_block            = 0;
        }
    }

    static void reset_log_state_(TakeRetryEntry& e) noexcept
    {
        e.suppressed           = false;
        e.suppressed_gate      = TakeGate::DeclineUnknown;
        e.suppressed_cycles    = 0;
        e.suppress_start_block = 0;
        e.last_emit_block      = 0;
    }

    std::unordered_map<std::string, TakeRetryEntry> entries_;
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_TAKE_RETRY_HPP
