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

/// The absolute unix timestamp to send as max_time, or nullopt when no
/// timelock should be attached at all.
///
/// nullopt for a disabled expiry, and nullopt for a nonsensical clock: a
/// wallet host whose clock reads at or before the epoch would otherwise turn
/// a safety feature into an offer that is born expired.  Posting without an
/// expiry is the safe way to be wrong here -- the offer still rests, and the
/// bot's own TTL still cancels it.
[[nodiscard]] inline std::optional<std::uint64_t> expiry_max_time_from(
    std::int64_t  now_unix_s,
    std::uint32_t expiry_secs) noexcept
{
    if (expiry_secs == 0) return std::nullopt;
    if (now_unix_s <= 0)  return std::nullopt;
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
/// engine still believes are live: no cancel is ever recorded, the coins
/// quietly unlock, and the book thins with nothing in the log to explain it.
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

}  // namespace xop::execution

#endif  // XOP_EXECUTION_OFFER_EXPIRY_HPP
