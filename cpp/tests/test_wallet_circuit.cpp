// ---------------------------------------------------------------------------
// [WALLET-CIRCUIT 2026-09-13] The wallet breaker counts transport failures,
// not throws.
//
// THE INCIDENT. On 2026-09-12 three wallet stalls (21:59-22:07, 22:11-22:18,
// 22:40-22:47) each ran for minutes without opening the wallet circuit
// breaker. Block 9284260 took 156,409 ms: wallet calls in series, each
// spending ~15.5 s exhausting libcurl retries (4 attempts x the 3 s localhost
// connect timeout, plus 3.5 s of backoff). The breaker counted THROWS out of
// Step 2 and Step 8, and neither step throws for a stall: detect_fills catches
// every get_offer failure, and Step 8's sync check logs and co_returns.
//
// These tests drive the two pure headers the decision now lives in:
//   rpc/transport_evidence.hpp    how a call ending is classified and counted;
//   execution/wallet_circuit.hpp  skip-the-rest-of-this-heartbeat, or open
//                                 the breaker.
//
// SCOPE, stated plainly: nothing here constructs an Engine or an OfferManager
// (TODO S36), so the call sites that consult these functions -- every
// Engine::wallet_step_may_run gate, the detect_fills poll gate and the S46
// sweep's per-id check -- are NOT covered, and neither is the breaker's
// end-to-end behaviour. That rpc_post feeds the counters IS covered, in
// test_rpc_transport_evidence.cpp.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "xop/execution/wallet_circuit.hpp"
#include "xop/rpc/transport_evidence.hpp"

namespace {

using xop::execution::kWalletCircuitTripFailures;
using xop::execution::unanswered_transport_failure_since;
using xop::execution::wallet_gate;
using xop::execution::WalletGate;
using xop::rpc::is_transport_failure;
using xop::rpc::observe_call_end;
using xop::rpc::RpcCallEnd;
using xop::rpc::TransportCounters;

// Built field by field on purpose: a braced {a, b, c} fixture is one
// transposition away from testing something else.
TransportCounters counters(std::uint64_t failures,
                           std::uint64_t answers,
                           std::uint32_t streak)
{
    TransportCounters c;
    c.transport_failures   = failures;
    c.answered             = answers;
    c.consecutive_failures = streak;
    return c;
}

// ---------------------------------------------------------------------------
// transport_evidence.hpp
// ---------------------------------------------------------------------------

TEST(TransportEvidence, OnlyALibcurlFailureIsATransportFailure)
{
    EXPECT_TRUE(is_transport_failure(RpcCallEnd::CurlFailed));
    EXPECT_FALSE(is_transport_failure(RpcCallEnd::Ok));
    EXPECT_FALSE(is_transport_failure(RpcCallEnd::ApplicationError))
        << "success=false (\"Wallet needs to be fully synced\") is an ANSWER";
    EXPECT_FALSE(is_transport_failure(RpcCallEnd::MalformedBody))
        << "a body that does not parse still came from a server";
    EXPECT_FALSE(is_transport_failure(RpcCallEnd::HttpError))
        << "a fast HTTP error does not cascade";
}

// Block 9284260: ten exhausted wallet calls in series -- six get_offer, two
// unlabeled, the Step 8 sync check and reward ingest. Every caller swallowed
// its exception, and every one still counts.
TEST(TransportEvidence, EveryTimeoutCountsWhoeverSwallowsIt)
{
    TransportCounters c;
    for (int call = 0; call < 10; ++call) {
        c = observe_call_end(c, RpcCallEnd::CurlFailed);
    }
    EXPECT_EQ(c.transport_failures, std::uint64_t{10});
    EXPECT_EQ(c.consecutive_failures, std::uint32_t{10});
    EXPECT_EQ(c.answered, std::uint64_t{0});
}

TEST(TransportEvidence, AnApplicationErrorIsAnAnswerAndResetsTheStreak)
{
    TransportCounters c;
    c = observe_call_end(c, RpcCallEnd::CurlFailed);
    c = observe_call_end(c, RpcCallEnd::CurlFailed);
    c = observe_call_end(c, RpcCallEnd::ApplicationError);
    EXPECT_EQ(c.consecutive_failures, std::uint32_t{0})
        << "the daemon answered, so it is not unreachable";
    EXPECT_EQ(c.transport_failures, std::uint64_t{2});
    EXPECT_EQ(c.answered, std::uint64_t{1});
}

TEST(TransportEvidence, TheStreakSaturatesInsteadOfWrappingToZero)
{
    const std::uint32_t top = (std::numeric_limits<std::uint32_t>::max)();
    const TransportCounters c =
        observe_call_end(counters(7, 0, top), RpcCallEnd::CurlFailed);
    EXPECT_EQ(c.consecutive_failures, top)
        << "a wrapped streak reads 0 -- healthy -- in the middle of an outage";
    EXPECT_EQ(c.transport_failures, std::uint64_t{8});
}

// ---------------------------------------------------------------------------
// wallet_circuit.hpp
// ---------------------------------------------------------------------------

TEST(WalletGate, OneUnansweredTimeoutSkipsTheRestOfThisHeartbeat)
{
    const TransportCounters start{};
    const TransportCounters now = observe_call_end(start, RpcCallEnd::CurlFailed);
    EXPECT_EQ(wallet_gate(start, now), WalletGate::SkipHeartbeat);
    EXPECT_TRUE(unanswered_transport_failure_since(start, now));
}

// The deadlock guard. The previous heartbeat ended mid-stall; the next one
// must still be allowed to find out whether the wallet is back.
TEST(WalletGate, AFailureFromAnEarlierHeartbeatNeverSilencesTheNextCanary)
{
    TransportCounters c;
    c = observe_call_end(c, RpcCallEnd::CurlFailed);
    c = observe_call_end(c, RpcCallEnd::CurlFailed);
    const TransportCounters mark = c;  // the next heartbeat starts here
    EXPECT_EQ(wallet_gate(mark, c), WalletGate::Run)
        << "every heartbeat must issue its first wallet call";
    EXPECT_FALSE(unanswered_transport_failure_since(mark, c));
}

TEST(WalletGate, AnAnswerAfterTheFailureLiftsTheSkip)
{
    const TransportCounters start{};
    TransportCounters c = observe_call_end(start, RpcCallEnd::CurlFailed);
    c = observe_call_end(c, RpcCallEnd::Ok);
    EXPECT_EQ(wallet_gate(start, c), WalletGate::Run)
        << "the wallet answered after the failure; the stall is over";
}

// One canary per heartbeat, each timing out: skip, skip, open.
TEST(WalletGate, AStallSpanningHeartbeatsOpensTheBreakerOnTheThirdCanary)
{
    TransportCounters c;
    const WalletGate expected_after_canary[] = {WalletGate::SkipHeartbeat,
                                                WalletGate::SkipHeartbeat,
                                                WalletGate::OpenCircuit};
    for (const WalletGate expected : expected_after_canary) {
        const TransportCounters mark = c;
        EXPECT_EQ(wallet_gate(mark, c), WalletGate::Run)
            << "every heartbeat issues its canary";
        c = observe_call_end(c, RpcCallEnd::CurlFailed);
        EXPECT_EQ(wallet_gate(mark, c), expected);
    }
}

TEST(WalletGate, AnAnswerBetweenHeartbeatsRestartsTheCount)
{
    TransportCounters c;
    c = observe_call_end(c, RpcCallEnd::CurlFailed);
    c = observe_call_end(c, RpcCallEnd::CurlFailed);
    c = observe_call_end(c, RpcCallEnd::Ok);
    const TransportCounters mark = c;
    c = observe_call_end(c, RpcCallEnd::CurlFailed);
    c = observe_call_end(c, RpcCallEnd::CurlFailed);
    EXPECT_EQ(c.consecutive_failures, std::uint32_t{2});
    EXPECT_EQ(wallet_gate(mark, c), WalletGate::SkipHeartbeat)
        << "four failures in total, but the wallet answered between them";
}

TEST(WalletGate, TheBreakerTripsAtExactlyThreeConsecutiveFailures)
{
    EXPECT_EQ(kWalletCircuitTripFailures, std::uint32_t{3});
    EXPECT_EQ(wallet_gate(counters(2, 5, 2), counters(3, 5, 3)),
              WalletGate::OpenCircuit);
    EXPECT_EQ(wallet_gate(counters(1, 5, 1), counters(2, 5, 2)),
              WalletGate::SkipHeartbeat);
}

// The parameter's documented edge: 0 means "never open", not "open on
// everything" -- which is what a bare `streak >= 0` would do.
TEST(WalletGate, AZeroTripThresholdNeverOpensTheBreaker)
{
    const TransportCounters healthy = counters(0, 9, 0);
    EXPECT_EQ(wallet_gate(healthy, healthy, 0U), WalletGate::Run);
    const TransportCounters stalled = counters(40, 9, 40);
    EXPECT_EQ(wallet_gate(healthy, stalled, 0U), WalletGate::SkipHeartbeat);
}

// The incident's arithmetic replayed through the gate: block 9284260 planned
// ten wallet calls and every one would have failed. The gate lets the first
// through and refuses the rest.
//
// READ THIS NARROWLY. It is OneUnansweredTimeoutSkipsTheRestOfThisHeartbeat
// run as a loop, so no mutation fails one without the other: it pins the
// decision SEQUENCE and nothing more. It does not model the engine. The S46
// intent sweep issues its own first probe above every gate, and calls inside
// OfferManager loops (selective_cancel, post_quotes, reconcile_offers) are not
// individually gated, so a real stall heartbeat issues more than one call. The
// engine wiring itself is not covered (TODO S36).
TEST(WalletGate, ReplayOfBlock9284260IssuesOneDoomedCallNotTen)
{
    const TransportCounters cycle_start{};
    TransportCounters c = cycle_start;
    int issued = 0;
    for (int planned = 0; planned < 10; ++planned) {
        if (wallet_gate(cycle_start, c) != WalletGate::Run) {
            break;
        }
        ++issued;
        c = observe_call_end(c, RpcCallEnd::CurlFailed);
    }
    EXPECT_EQ(issued, 1);
    EXPECT_EQ(wallet_gate(cycle_start, c), WalletGate::SkipHeartbeat);
}

}  // namespace
