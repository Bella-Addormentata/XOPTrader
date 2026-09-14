#ifndef XOP_RPC_TRANSPORT_EVIDENCE_HPP
#define XOP_RPC_TRANSPORT_EVIDENCE_HPP
// ---------------------------------------------------------------------------
// transport_evidence.hpp -- did the daemon ANSWER, or did the transport fail?
//
// [WALLET-CIRCUIT 2026-09-13] The wallet circuit breaker used to count THROWS
// out of heartbeat Steps 2 and 8, and neither step throws for a stalled
// wallet: detect_fills catches ChiaRPCError around every get_offer, and
// Step 8's sync check logs and co_returns. On 2026-09-12 block 9284260 took
// 156,409 ms -- ten wallet calls in series, each spending ~15.5 s in libcurl
// (4 attempts x the 3 s localhost connect timeout, plus 3.5 s of backoff) --
// and the breaker never opened, in any of three stall windows.
//
// The fix moves the evidence to the one place every wallet call passes
// through: ChiaRPCBase::rpc_post records how each call ENDED, whoever swallows
// the exception afterwards.
//
// THE RULE. Only a libcurl failure on the attempt that ENDS rpc_post counts --
// retries exhausted, or a code that was never retryable (those throw on the
// first attempt). Everything else proves the daemon answered, and resets the
// streak:
//   * an HTTP response of any status -- the request reached a server;
//   * a body that is not JSON -- same;
//   * success=false, e.g. "Wallet needs to be fully synced before making
//     transactions" (22:08:48 on 2026-09-12) -- a refusal is an answer.
// The breaker exists to stop TIMEOUT CASCADES. A fast HTTP error or a refusal
// costs milliseconds and does not cascade, so it must not open the breaker.
//
// Pure header: an enum, a struct and two constexpr functions. No I/O, no asio,
// no curl, no logging.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <limits>

namespace xop::rpc {

/// How one rpc_post call ended.  Recorded once per call that got past the
/// is_open() check; a call on a client that is not open records nothing.
enum class RpcCallEnd : std::uint8_t {
    Ok,                ///< HTTP 2xx with a JSON body that is not success=false.
    ApplicationError,  ///< The daemon answered success=false.
    MalformedBody,     ///< HTTP 2xx whose body did not parse as JSON.
    HttpError,         ///< A non-2xx HTTP status on the final attempt.
    CurlFailed,        ///< libcurl failed on the final attempt: nothing answered.
};

/// True only for the ending that means nothing answered.
[[nodiscard]] constexpr bool is_transport_failure(RpcCallEnd end) noexcept
{
    return end == RpcCallEnd::CurlFailed;
}

/// Running transport evidence for ONE RPC client: two monotonic totals and
/// the current streak of consecutive transport failures.
struct TransportCounters {
    std::uint64_t transport_failures{0};    ///< Calls that ended CurlFailed.
    std::uint64_t answered{0};              ///< Calls that ended any other way.
    std::uint32_t consecutive_failures{0};  ///< CurlFailed endings since the last answer.
};

/// Fold one call ending into @p counters.
///
/// The streak SATURATES rather than wrapping: a wrapped streak would read 0 --
/// "healthy" -- in the middle of an outage.
[[nodiscard]] constexpr TransportCounters observe_call_end(
    TransportCounters counters, RpcCallEnd end) noexcept
{
    if (is_transport_failure(end)) {
        ++counters.transport_failures;
        if (counters.consecutive_failures
            < (std::numeric_limits<std::uint32_t>::max)()) {
            ++counters.consecutive_failures;
        }
    } else {
        ++counters.answered;
        counters.consecutive_failures = 0;
    }
    return counters;
}

}  // namespace xop::rpc

#endif  // XOP_RPC_TRANSPORT_EVIDENCE_HPP
