#ifndef XOP_EXECUTION_WALLET_CIRCUIT_HPP
#define XOP_EXECUTION_WALLET_CIRCUIT_HPP
// ---------------------------------------------------------------------------
// wallet_circuit.hpp -- may this heartbeat issue another wallet call?
//
// [WALLET-CIRCUIT 2026-09-13] Two decisions, both read from the wallet
// client's rpc::TransportCounters (rpc/transport_evidence.hpp):
//
//   1. SKIP THE REST OF THIS HEARTBEAT once a wallet call made SINCE THE
//      HEARTBEAT'S MARK has failed at the transport level and nothing has
//      answered since. The next call would be ~15.5 s of doomed retries at the
//      3 s localhost connect timeout (~123.5 s against a daemon that accepts
//      the connection and then hangs), and the second doomed call is where a
//      stall becomes a cascade: blocks 9284260, 9284302 and 9284313 on
//      2026-09-12 each issued 9-11 of them and took 156-173 s.
//
//   2. OPEN THE BREAKER after kWalletCircuitTripFailures consecutive
//      transport failures, counted across callers AND across heartbeats.
//
// WHY THE SKIP CANNOT DEADLOCK. A failure from before the mark never counts
// toward the skip, and the engine takes a new mark at the top of every
// heartbeat, so every heartbeat issues its first wallet call -- the canary. An
// answer resets the streak; three failed canaries open the breaker, and the
// breaker's own probe in the poll loop is what closes it.
//
// KNOWN AND DELIBERATE: the streak belongs to the CLIENT, so wallet calls made
// between heartbeats count too -- the poll loop's wallet-height fallback feeds
// it. A heartbeat whose mark already carries three consecutive failures opens
// the breaker at its first gate without issuing a canary. Three consecutive
// failed wallet calls is the trip condition wherever they were made.
//
// No static_assert pins on these functions: every planned mutation of them
// must compile and fail at RUN time (cpp/tests/test_wallet_circuit.cpp).
//
// Pure header: no I/O, no asio, no RPC client, no logging.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "xop/rpc/transport_evidence.hpp"

namespace xop::execution {

/// Consecutive wallet transport failures that open the breaker.
inline constexpr std::uint32_t kWalletCircuitTripFailures = 3;

/// What a wallet-dependent step should do.
enum class WalletGate : std::uint8_t {
    Run,            ///< Issue the call.
    SkipHeartbeat,  ///< Skip this heartbeat's remaining wallet work.
    OpenCircuit,    ///< Open the breaker (and skip).
};

/// A wallet call that ENDED after @p mark was a transport failure, and no call
/// has answered since.
///
/// Both halves are needed. The streak alone would carry a previous
/// heartbeat's failure into this one and silence its canary; the total alone
/// would keep skipping after the daemon has answered.
[[nodiscard]] constexpr bool unanswered_transport_failure_since(
    const rpc::TransportCounters& mark,
    const rpc::TransportCounters& now) noexcept
{
    return now.consecutive_failures > 0
        && now.transport_failures > mark.transport_failures;
}

/// @param cycle_start    the wallet client's counters at the top of this
///                       heartbeat.
/// @param now            its counters now.
/// @param trip_failures  consecutive failures that open the breaker; 0 never
///                       opens it.
[[nodiscard]] constexpr WalletGate wallet_gate(
    const rpc::TransportCounters& cycle_start,
    const rpc::TransportCounters& now,
    std::uint32_t trip_failures = kWalletCircuitTripFailures) noexcept
{
    if (trip_failures > 0 && now.consecutive_failures >= trip_failures) {
        return WalletGate::OpenCircuit;
    }
    if (unanswered_transport_failure_since(cycle_start, now)) {
        return WalletGate::SkipHeartbeat;
    }
    return WalletGate::Run;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_WALLET_CIRCUIT_HPP
