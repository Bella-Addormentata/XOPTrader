#ifndef XOP_EXECUTION_OFFER_EXPIRY_HPP
#define XOP_EXECUTION_OFFER_EXPIRY_HPP
// ---------------------------------------------------------------------------
// offer_expiry.hpp -- the on-chain offer-expiry decisions, as pure functions.
//
// [OFFER-EXPIRY 2026-09-12] An offer we create can carry an absolute timelock
// so that, if we ever lose the ability to cancel it, the chain retires it for
// us instead of leaving it takeable forever.
//
// ONLY max_time IS EVER SENT, AND THAT IS NOT A STYLE CHOICE.
// -----------------------------------------------------------
// Chia's create_offer_for_ids accepts four absolute timelock flags:
// min_height, min_time, max_height and max_time.  Per Chia's own Offer RPC
// reference, the reference wallet "will only recognize max_time"; the other
// three are enforced on-chain, but the reference wallet does not apply them
// until it submits the spend bundle to the mempool, so if someone uses the
// reference wallet to take such an offer "the transaction will be initiated,
// but will fail".
//
// For a market maker that is not a safety feature, it is an outage wearing
// one's costume: our offers stay visible on the aggregator, takers keep
// trying, and every attempt fails.  So max_height is never sent, and the
// relative flags (max_blocks_after_created and friends) are not supported by
// this API at all.
//
// WHY THE ECHO IS VERIFIED RATHER THAN ASSUMED
// --------------------------------------------
// A wallet that does not honour max_time does not return an error.  It
// returns a perfectly successful create with the flag simply absent from
// trade_record.valid_times.  Trusting the request would rest an offer we
// believe self-retires and which in fact never does -- the worst of both
// worlds, because it also stops us from watching it.  Every check below
// therefore fails CLOSED: absent, null, wrong type and mismatched all read
// as "not honoured".
//
// Pure header, no engine types and no clock, so cpp/tests/test_offer_expiry
// drives every decision here directly.  OfferManager keeps only thin
// wrappers that supply the clock and the config.
// ---------------------------------------------------------------------------

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

namespace xop::execution {

/// Expiry in seconds that applies to a pair.
///
/// value_or, deliberately, rather than any truthiness test: 0 is a REAL
/// setting meaning "never expire this pair's offers" and must bind, whereas
/// an ABSENT override inherits the global.  Collapsing those two would make
/// a pair opted out of expiry silently inherit it.
[[nodiscard]] inline std::uint32_t effective_offer_expiry_secs(
    const std::optional<std::uint32_t>& pair_override,
    std::uint32_t                       global_secs) noexcept
{
    return pair_override.value_or(global_secs);
}

/// The earliest wall-clock reading that could describe a LIVE Chia offer:
/// mainnet genesis, 2021-03-19.
///
/// [review #150] The guard below rejected only now_unix_s <= 0.  A host
/// reading 1 -- a dead RTC, a VM restored from a cold snapshot, a boot that
/// beat NTP -- passed it and minted a max_time of 86401: an offer born
/// expired.  Nothing downstream catches that, because expiry_echo_ok
/// compares the wallet's echo against what we ASKED for, so an honest wallet
/// echoing 86401 CONFIRMS it and the posting path publishes it.
///
/// This is a PLAUSIBILITY FLOOR, not a skew detector, and the difference is
/// the honest part: with one untrusted clock and no second opinion, a clock
/// merely hours slow is undetectable here -- and the config floor puts
/// expiry_secs above 2x the hard TTL, so "hours slow" is the interesting
/// range.  No trusted timestamp exists at the call site: OfferManager holds
/// no full-node client, no wallet endpoint returns a wall clock, and
/// deriving one from block height is the units error this header already
/// refuses at kExpiryFloorMargin.  What it does catch is every clock wrong
/// enough to be obvious, routed into the same fail-safe as a zero reading.
inline constexpr std::int64_t kMinPlausibleUnixTime = 1'616'162'400;

/// The absolute unix timestamp to send as max_time, or nullopt when no
/// timelock should be attached at all.
///
/// nullopt for a disabled expiry, and nullopt for a clock too far in the
/// past to be describing a live offer: such a host would otherwise turn a
/// safety feature into an offer that is born expired.  Posting without an
/// expiry is the safe way to be wrong here -- the offer still rests, and the
/// bot's own TTL still cancels it.
[[nodiscard]] inline std::optional<std::uint64_t> expiry_max_time_from(
    std::int64_t  now_unix_s,
    std::uint32_t expiry_secs) noexcept
{
    if (expiry_secs == 0) return std::nullopt;
    if (now_unix_s < kMinPlausibleUnixTime) return std::nullopt;
    return static_cast<std::uint64_t>(now_unix_s)
         + static_cast<std::uint64_t>(expiry_secs);
}

/// True only if the wallet echoed back EXACTLY the max_time we requested.
///
/// Not noexcept: nlohmann's accessors are guarded by the type checks above
/// each use, but marking this noexcept would convert any surprise into a
/// terminate() inside a live trading loop.
[[nodiscard]] inline bool expiry_echo_ok(const nlohmann::json& result,
                                         std::uint64_t expected_max_time)
{
    if (!result.contains("trade_record")
        || !result["trade_record"].is_object()) {
        return false;
    }
    const auto& tr = result["trade_record"];
    if (!tr.contains("valid_times") || !tr["valid_times"].is_object()) {
        return false;
    }
    const auto& vt = tr["valid_times"];
    // A null max_time is the shape a wallet returns when it ignored the
    // request, so this type check is the load-bearing one.
    //
    // [review #150] It was `is_number()`, which ALSO admits number_float --
    // and `get<std::uint64_t>()` truncates.  An echoed `expected + 0.5`
    // therefore compared equal and passed a check whose entire purpose is
    // exactness.  Chia's max_time is a uint64; require that representation
    // rather than anything numeric that happens to round to it.
    if (!vt.contains("max_time") || !vt["max_time"].is_number_unsigned()) {
        return false;
    }
    return vt["max_time"].get<std::uint64_t>() == expected_max_time;
}

/// The longest life this bot itself intends for an offer, in seconds.
/// Soft TTL x the hard-TTL multiplier: the point past which offers are
/// expired unconditionally, regardless of price accuracy.
[[nodiscard]] inline double hard_ttl_seconds(std::uint32_t ttl_blocks,
                                             std::uint32_t hard_multiplier,
                                             double secs_per_block) noexcept
{
    return static_cast<double>(ttl_blocks)
         * static_cast<double>(hard_multiplier)
         * secs_per_block;
}

/// Whether a configured expiry is safe to use.
///
/// An expiry INSIDE our own hard TTL would let the chain retire offers the
/// engine still believes are live: no cancel is ever recorded, and the book
/// thins with nothing in the log to explain it.  [review #150] Whether the
/// coins are released is NOT claimed here: max_time governs takeability, and
/// this repo establishes only that spendable selection subtracts
/// get_locked_coins() -- not what that call returns for an EXPIRED trade.
/// A disabled expiry (0) is trivially safe -- there is nothing to outlast.
///
/// Safety margin applied to the floor.
///
/// [review #150] THE TWO QUANTITIES ARE NOT IN THE SAME UNITS, AND CANNOT BE
/// MADE SO.  The hard TTL fires after a COUNT OF OBSERVED BLOCK HEIGHTS;
/// max_time fires at an ABSOLUTE WALL-CLOCK INSTANT.  Converting between
/// them needs a block rate, and the real rate varies.  When blocks arrive
/// slower than the configured mean, the hard TTL stretches in wall-clock
/// terms while a floor computed from that mean does not -- so a value
/// accepted just above the floor expires FIRST, recreating precisely the
/// tracked-as-live state this validation exists to prevent.
///
/// An earlier revision of this header asserted the opposite: that rate drift
/// "cannot desynchronise anything" and could only make the floor "slightly
/// conservative".  That was wrong, and wrong in the dangerous direction.
///
/// The margin is the honest answer: require the expiry to outlast the hard
/// TTL by this factor, so ordinary rate variation cannot invert the
/// ordering.  Being over-conservative costs an operator a longer configured
/// expiry; being under-conservative costs offers that die on-chain while the
/// engine still believes they rest.
inline constexpr double kExpiryFloorMargin = 2.0;

/// Whether a configured expiry is safe to use.
///
/// Strictly greater, not >=: a tie is a race between the chain and the
/// canceller, which is not a race worth entering.
///
/// @param secs_per_block  The CONFIGURED mean inter-block interval
///        (StrategyConfig::block_time_seconds, default 52.0) -- never a
///        constant invented here.  A second source of truth for a safety
///        bound is what this codebase keeps getting bitten by.
[[nodiscard]] inline bool expiry_outlasts_hard_ttl(
    std::uint32_t expiry_secs,
    std::uint32_t ttl_blocks,
    std::uint32_t hard_multiplier,
    double        secs_per_block) noexcept
{
    if (expiry_secs == 0) return true;
    return static_cast<double>(expiry_secs)
         > hard_ttl_seconds(ttl_blocks, hard_multiplier, secs_per_block)
           * kExpiryFloorMargin;
}

// ---------------------------------------------------------------------------
// [S70 2026-09-20] strategy.ttl_cancel_mode: expire -- let the chain age an
// offer out instead of paying for a cancel.
//
// THE PROBLEM.  The hard TTL cancels a correctly priced offer purely for its
// age and reposts it at nearly the same price: 402 of 1,254 cancels in the 14
// days to 2026-09-20, every one a fee-bearing spend into ~97% full blocks.
// An offer that carries a VERIFIED max_time already has an age limit the
// chain enforces for free, so in `expire` mode the hard TTL skips it.
//
// WHAT CHIA 2.7.4 DOES WITH AN EXPIRED OFFER -- read from the source at tag
// 2.7.4 (commit 98aba3d1), not assumed.  This is the half that [review #150]
// above declined to claim:
//
//   * The wallet never retires it.  Nothing in chia/wallet/trade_manager.py
//     or wallet_node.py reads valid_times after the trade is stored; a trade
//     leaves PENDING_ACCEPT only through coins_of_interest_farmed (a coin it
//     watches was spent) or cancel_pending_offers.  An expired offer stays
//     PENDING_ACCEPT indefinitely.
//   * Its coins stay LOCKED.  TradeManager.get_locked_coins (trade_manager.py
//     L210-230) returns the coins of every PENDING_ACCEPT / PENDING_CONFIRM /
//     PENDING_CANCEL trade with no time test, and get_spendable_coins_for_
//     wallet (wallet_state_manager.py L2002-2035) subtracts them.  So an
//     expiry alone returns NO collateral.
//   * cancel_offer with secure=false releases them with no spend.
//     cancel_pending_offers (trade_manager.py L289-292) sets the trade
//     CANCELLED and `continue`s before it builds any transaction, and a
//     CANCELLED trade is outside get_locked_coins.  No fee, no mempool.
//   * Nobody can take it afterwards.  max_time becomes
//     ASSERT_BEFORE_SECONDS_ABSOLUTE on the maker's own signed spend, and
//     chia_rs check_time_locks fails a bundle once
//     `timestamp >= before_seconds_absolute`, where `timestamp` is the
//     PREVIOUS TRANSACTION BLOCK's (mempool_manager.py L829-841 passes
//     self.peak.timestamp).  Transaction-block timestamps only increase, so
//     once one at or past max_time exists no later block can carry the spend.
//
// WHY A LOCAL CANCEL IS NORMALLY UNSAFE AND IS SAFE HERE.  secure=false leaves
// the offer file valid: anyone holding it can still take it, and a take after
// the local cancel is invisible, because get_trades_by_coin skips CANCELLED
// trades, so the wallet never reports CONFIRMED and the fill is never booked.
// That is a race the bot loses silently.  For an EXPIRED offer there is no
// race left to lose -- provided the clock that says "expired" is the chain's.
//
// THE CLOCK IS THE CHAIN'S, NEVER THIS HOST'S.  max_time was minted from the
// host clock but is ENFORCED against block timestamps, which trail wall time
// by one transaction-block interval (~52 s typical, minutes on a slow patch)
// and are themselves only loosely bound to it.  So "now >= max_time" on this
// host proves nothing: the offer can still be taken until a transaction block
// stamped >= max_time exists.  The retire decision therefore reads the
// wallet's own chain clock -- get_timestamp_for_height, asked relative to the
// height the wallet has FINISHED syncing to -- which answers both questions
// at once: no later block can carry the take, and the wallet has already
// processed every block that could.  If it still says PENDING_ACCEPT, the
// offer was never taken and never can be.
//
// THE REORG ALLOWANCE IS A DEPTH, NOT A NUMBER OF SECONDS.  [review #164] The
// first revision required the chain clock to be 600 s PAST max_time and called
// that "~32 blocks".  It is not: transaction-block timestamps must increase,
// but by no particular step, so after a slow patch the FIRST block stamped past
// max_time can be stamped 600 s past it -- satisfying a seconds margin at a
// confirmation depth of one.  Reorg that one block (ordinary at the tip) and the
// previous transaction block is again before max_time: the offer is takeable,
// its trade is CANCELLED in the wallet, and the take is never booked.  So the
// clock is read kExpiredRetireDepthBlocks BELOW the wallet's synced height:
// if the chain was already past max_time that many blocks ago, the block that
// expired the offer is buried at least that deep.
//
// Pure, like the rest of this header.  OfferManager::retire_expired_offers
// supplies the wallet answers; cpp/tests/test_offer_expiry.cpp drives these.
// ---------------------------------------------------------------------------

/// Whether the bot's OWN age limit (the unconditional hard-TTL cancel, and the
/// stuck-offer pass behind it) applies to an offer.
///
/// False only when the operator chose `expire` AND the offer verifiably
/// carries an on-chain expiry.  0 means "no verified expiry" -- an offer
/// created before the feature, one whose echo failed, or one restored from
/// offer_log before the wallet record has been read back -- and such an offer
/// keeps today's hard TTL, because nothing else bounds its life.
[[nodiscard]] constexpr bool age_limit_cancel_applies(
    bool          expire_mode,
    std::uint64_t verified_max_time) noexcept
{
    return !(expire_mode && verified_max_time > 0);
}

/// The max_time a wallet trade record carries, or 0 when it carries none.
///
/// Takes the trade_record OBJECT itself (get_offer's "trade_record", or one
/// element of get_all_offers' "trade_records").  Same fail-closed typing as
/// expiry_echo_ok: a null, a float, a string or a negative number all read as
/// "no verified expiry", so the offer keeps the bot's own hard TTL.
[[nodiscard]] inline std::uint64_t trade_record_max_time(
    const nlohmann::json& trade_record)
{
    if (!trade_record.is_object()
        || !trade_record.contains("valid_times")
        || !trade_record["valid_times"].is_object()) {
        return 0;
    }
    const auto& vt = trade_record["valid_times"];
    if (!vt.contains("max_time") || !vt["max_time"].is_number_unsigned()) {
        return 0;
    }
    const auto max_time = vt["max_time"].get<std::uint64_t>();
    // A timelock before mainnet genesis is not one this bot minted (see
    // kMinPlausibleUnixTime); refuse to reason from it.
    return max_time >= static_cast<std::uint64_t>(kMinPlausibleUnixTime)
        ? max_time : 0;
}

/// How deep the block that expired an offer must be buried before the offer is
/// retired locally, in peak-height blocks.  32 is ~10 min at the 18.75 s
/// cadence (4,608/day) and over five times strategy.confirmation_depth_blocks'
/// default of 6, which is what this engine asks of a FILL: an insecure cancel
/// cannot be taken back, so it waits longer.  The cost is ten more minutes of
/// locked coins on an offer that already cannot be taken.
inline constexpr std::int64_t kExpiredRetireDepthBlocks = 32;

/// The height whose chain clock the retire decision reads: @p synced_height
/// (the wallet's get_height_info, i.e. get_finished_sync_up_to) less the
/// depth.  0 when the wallet is not that deep into the chain -- no clock, and
/// nothing is retired.
[[nodiscard]] constexpr std::int64_t expired_retire_clock_height(
    std::int64_t synced_height) noexcept
{
    return synced_height > kExpiredRetireDepthBlocks
        ? synced_height - kExpiredRetireDepthBlocks
        : 0;
}

/// True once the chain was ALREADY at or past max_time
/// kExpiredRetireDepthBlocks below the wallet's synced height.
/// @p chain_time_at_depth_s is a TRANSACTION-BLOCK timestamp -- the wallet's
/// get_timestamp_for_height(expired_retire_clock_height(...)) -- never a host
/// clock.  `>=`, as consensus has it: a spend asserting BEFORE max_time fails
/// once the previous transaction block's timestamp is >= max_time.  0 on
/// either side reads as "unknown" and is never expired: an unknown max_time
/// by the first clause, an unknown clock because 0 is >= no real max_time.
[[nodiscard]] constexpr bool expired_at_depth(
    std::uint64_t max_time,
    std::uint64_t chain_time_at_depth_s) noexcept
{
    return max_time != 0 && chain_time_at_depth_s >= max_time;
}

/// A cheap PRE-FILTER on the host clock, so a heartbeat with nothing near its
/// expiry asks the wallet nothing.  It only ever decides to LOOK: a fast host
/// clock costs two read-only RPCs, a slow one delays a retire that is already
/// harmless.  The retire itself is decided by expired_at_depth alone.
[[nodiscard]] constexpr bool expiry_worth_checking(
    std::uint64_t max_time,
    std::int64_t  host_now_s) noexcept
{
    return max_time > 0
        && host_now_s > 0
        && static_cast<std::uint64_t>(host_now_s) >= max_time;
}

/// How often retire_expired_offers may repeat a WARN, in peak-height blocks:
/// 96 is ~30 min at 18.75 s.  The pass runs every heartbeat while any offer
/// waits past its expiry, and a wallet that cannot supply the chain clock
/// fails identically each time.
inline constexpr std::uint64_t kExpiryWarnIntervalBlocks = 96;

/// Whether a retire-pass warning may be logged at WARN this block (it goes
/// to debug otherwise).  0 = never warned.  A height that went BACKWARDS (a
/// reorg, a height-source switch) warns rather than staying silent -- stated
/// as its own clause although the unsigned subtraction below would wrap to
/// the same answer, because a rule that is right only by wrap-around reads as
/// a bug to the next person who touches it.
[[nodiscard]] constexpr bool expiry_warn_due(
    std::uint64_t last_warned_block,
    std::uint64_t current_block) noexcept
{
    return last_warned_block == 0
        || current_block < last_warned_block
        || current_block - last_warned_block >= kExpiryWarnIntervalBlocks;
}

/// What to do with a resting offer whose on-chain expiry may have passed.
enum class ExpiredRetire {
    NotExpired,     ///< the chain was not yet past max_time at depth: leave it
    LeaveToWallet,  ///< the wallet says it is no longer PENDING_ACCEPT: a fill
                    ///< or a cancel is in hand, and those paths own it
    Unverified,     ///< the wallet's record does not carry OUR max_time: keep
                    ///< the offer, and let the hard TTL have it back
    RetireLocal,    ///< expired, never taken, never takeable: free the coins
};

/// The retire decision.  Every input is a wallet answer read THIS heartbeat.
///
/// @param wallet_status_pending_accept  get_offer reported PENDING_ACCEPT.
/// @param tracked_max_time   the expiry State holds for the offer.
/// @param record_max_time    trade_record_max_time(get_offer's record).
/// @param chain_time_at_depth_s  the wallet's chain clock, read
///                           kExpiredRetireDepthBlocks below its synced height.
///
/// Order is the contract.  The status is read first because a CONFIRMED
/// trade is a FILL whatever the clock says, and `filled` always wins.  The
/// record must then repeat the expiry we tracked, exactly: a local cancel on
/// an offer whose timelock we cannot re-verify is the insecure cancel this
/// repo otherwise refuses.
[[nodiscard]] constexpr ExpiredRetire decide_expired_retire(
    bool          wallet_status_pending_accept,
    std::uint64_t tracked_max_time,
    std::uint64_t record_max_time,
    std::uint64_t chain_time_at_depth_s) noexcept
{
    if (!wallet_status_pending_accept) {
        return ExpiredRetire::LeaveToWallet;
    }
    if (tracked_max_time == 0 || record_max_time != tracked_max_time) {
        return ExpiredRetire::Unverified;
    }
    return expired_at_depth(tracked_max_time, chain_time_at_depth_s)
        ? ExpiredRetire::RetireLocal
        : ExpiredRetire::NotExpired;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_OFFER_EXPIRY_HPP
