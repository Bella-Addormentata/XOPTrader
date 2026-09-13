// ---------------------------------------------------------------------------
// [S33 2026-09-05] Node sync-state parsing -- reachable is not synced.
//
// Both metrics exporters used to publish node_synced as a copy of
// connectivity and node_syncing as a hard false (the member backing it had no
// writer anywhere in the tree), so a full node that answered a peak height
// while still catching up showed solid green and the GUI's
// "Full Node: Syncing..." state was unreachable.
//
// node_sync_from_blockchain_state is the parse both exporters now read
// through, via ChiaFullNodeRPC::last_sync_state().  It is a free function on
// the raw get_blockchain_state response, so it needs no RPC and no
// io_context.
//
// COVERAGE, and where it stops.  The first group pins the PARSE.  The second
// group (NodeHealthFlags, at the bottom) pins the DERIVATION the two exporters
// apply to it -- that used to be `health.node_synced = health.node_connected`
// duplicated at both sites, which no test could see; it now lives in one free
// function, so reinstating it is a change these tests fail on.  What remains
// unreached is only the wiring: that each exporter passes its own reachability
// expression into that function, inside Engine methods no test constructs.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <chrono>

#include "xop/engine.hpp"
#include "xop/rpc/chia_rpc.hpp"

using xop::rpc::ChiaFullNodeRPC;
using xop::rpc::json;
using xop::rpc::node_sync_from_blockchain_state;

// The mutation-killing case: a node that answers a height while catching up.
TEST(NodeSyncState, SyncingNodeWithPeakIsNotReportedSynced)
{
    const json resp = json::parse(R"({
        "blockchain_state": {
            "peak": {"height": 5000000},
            "sync": {"synced": false,
                     "sync_mode": true,
                     "sync_tip_height": 5100000}
        }
    })");

    const auto s = node_sync_from_blockchain_state(resp);
    EXPECT_FALSE(s.synced)
        << "the node itself says it is not synced";
    EXPECT_TRUE(s.syncing)
        << "sync_mode is Chia's name for the in-progress flag";
}

// Guards against a "fix" that simply pins both flags to false.
TEST(NodeSyncState, SyncedNodeReportsSynced)
{
    const json resp = json::parse(R"({
        "blockchain_state": {
            "peak": {"height": 5000000},
            "sync": {"synced": true, "sync_mode": false}
        }
    })");

    const auto s = node_sync_from_blockchain_state(resp);
    EXPECT_TRUE(s.synced);
    EXPECT_FALSE(s.syncing);
}

// "The node did not say" must never be published as "the node is not
// syncing" -- the previous reading is carried instead.
TEST(NodeSyncState, MissingSyncObjectKeepsPreviousReading)
{
    const json resp = json::parse(R"({
        "blockchain_state": {"peak": {"height": 5000000}}
    })");

    const ChiaFullNodeRPC::SyncState previous{/*synced=*/true,
                                              /*syncing=*/false};
    const auto s = node_sync_from_blockchain_state(resp, previous);
    EXPECT_TRUE(s.synced);
    EXPECT_FALSE(s.syncing);
}

// A malformed node response must not throw an nlohmann type_error out of the
// height poll, and must not be read as a fresh "not syncing" either.
TEST(NodeSyncState, NonBooleanFieldsKeepPreviousReading)
{
    const json resp = json::parse(R"({
        "blockchain_state": {"sync": {"synced": "yes", "sync_mode": 1}}
    })");

    const ChiaFullNodeRPC::SyncState previous{/*synced=*/false,
                                              /*syncing=*/true};
    ChiaFullNodeRPC::SyncState s{};
    ASSERT_NO_THROW(s = node_sync_from_blockchain_state(resp, previous));
    EXPECT_FALSE(s.synced);
    EXPECT_TRUE(s.syncing);
}

// An empty response is the "node has never answered" case: defaults hold.
TEST(NodeSyncState, EmptyResponseLeavesTheDefaultsAlone)
{
    const auto s = node_sync_from_blockchain_state(json::object());
    EXPECT_FALSE(s.synced);
    EXPECT_FALSE(s.syncing);
}

// ---------------------------------------------------------------------------
// [S33 2026-09-05] The ENGINE side of the same finding: what the exporters
// publish once the parse has answered.
//
// The parse tests above are honest that they cannot see engine.cpp.  They can
// now, because the derivation itself was lifted out of the two exporters that
// duplicated it (run_startup_analysis and step_export_metrics) into
// node_health_flags().
//
// MUTATION CHECK: reinstate the pre-S33 expressions inside node_health_flags,
//
//     flags.synced  = reachable;      // was health.node_synced = health.node_connected
//     flags.syncing = false;          // node_syncing_ had no writer in the tree
//
// -> ASyncingNodeIsNotPublishedAsSynced goes RED on both counts.
// ---------------------------------------------------------------------------

using xop::node_health_flags;

// The state the whole finding is about, and the one the GUI could never show:
// the node answers a peak height while still catching up.
TEST(NodeHealthFlags, ASyncingNodeIsNotPublishedAsSynced)
{
    const auto flags = node_health_flags(/*reachable=*/true,
                                         /*node_reports_synced=*/false,
                                         /*node_reports_syncing=*/true);
    EXPECT_TRUE(flags.connected);
    EXPECT_FALSE(flags.synced)
        << "reachability is not synchronisation -- this published solid green "
           "through exactly the startup a syncing node produces";
    EXPECT_TRUE(flags.syncing)
        << "the GUI's \"Full Node: Syncing...\" state must be reachable";
}

// Guards against a "fix" that simply pins both flags to false.
TEST(NodeHealthFlags, ASyncedNodeIsPublishedAsSynced)
{
    const auto flags = node_health_flags(true, true, false);
    EXPECT_TRUE(flags.connected);
    EXPECT_TRUE(flags.synced);
    EXPECT_FALSE(flags.syncing);
}

// An unreachable node reports NEITHER: last_sync_state() is a cache, and a
// wallet-sourced heartbeat must not republish a stale node reading as if the
// node had just answered.
TEST(NodeHealthFlags, AnUnreachableNodeRepublishesNothing)
{
    const auto stale = node_health_flags(/*reachable=*/false,
                                         /*node_reports_synced=*/true,
                                         /*node_reports_syncing=*/false);
    EXPECT_FALSE(stale.connected);
    EXPECT_FALSE(stale.synced);
    EXPECT_FALSE(stale.syncing);

    // Same in the other direction: an unreachable node is not "syncing".
    const auto mid_sync = node_health_flags(false, false, true);
    EXPECT_FALSE(mid_sync.synced);
    EXPECT_FALSE(mid_sync.syncing);
}

// End to end over the parse: the response a catching-up node returns, through
// node_sync_from_blockchain_state, through the exporters' derivation.
TEST(NodeHealthFlags, TheParseAndTheDerivationAgreeOnACatchingUpNode)
{
    const json resp = json::parse(R"({
        "blockchain_state": {
            "peak": {"height": 5000000},
            "sync": {"synced": false, "sync_mode": true}
        }
    })");

    const auto parsed = node_sync_from_blockchain_state(resp);
    const auto flags  = node_health_flags(true, parsed.synced, parsed.syncing);
    EXPECT_FALSE(flags.synced);
    EXPECT_TRUE(flags.syncing);
}

// ---------------------------------------------------------------------------
// [S33 2026-09-12] The REACHABILITY argument, which is where the finding
// actually lived.  node_health_flags is honest; both exporters fed it an
// expression that required the node to be the SELECTED HEIGHT SOURCE.
//
// During an auto fallback those are different things.  A full node catching
// up answers every probe while staying behind the wallet, so height_source_
// stays Wallet -- and the gauges published connected=false, syncing=false.
// The GUI then read "Full Node: Disconnected" during precisely the period an
// operator is watching: a reachable node, catching up, about to come back.
//
// node_probe_is_live() is the replacement notion, and it has to keep what the
// height-source gate was protecting: last_sync_state() is a cache, so a
// wallet-sourced heartbeat must not republish a minutes-old node reading.
// Freshness delivers that -- an outage stops producing successful probes.
//
// MUTATION CHECK: make node_probe_is_live ignore freshness,
//
//     return node_client_open;      // the whole body
//
// -> ANeverProbedNodePublishesNothing and AStaleProbeDoesNotRepublishTheCache
//    go RED.  Drop only the never-probed clause and the first goes red alone.
//
// WHAT THESE STILL CANNOT SEE, stated plainly because the group above it
// makes the same admission: no test constructs an Engine, so the two call
// sites' argument is not reachable from ctest.  What is reachable is that
// both now pass the SAME one-line helper (Engine::node_probe_live_now), which
// is why the expression could drift apart at the two sites in the first place.
// ---------------------------------------------------------------------------

using xop::kNodeProbeLivenessWindow;
using xop::node_probe_is_live;

namespace {
using Clock = std::chrono::steady_clock;
}  // namespace

// THE finding: reachable and catching up, while the WALLET is the height
// source.  No height source appears in this call at all -- that is the fix.
TEST(NodeProbeLiveness, AReachableSyncingNodePublishesSyncingOffTheWalletSource)
{
    const auto now  = Clock::now();
    const auto live = node_probe_is_live(/*node_client_open=*/true,
                                         /*last_probe=*/now
                                             - std::chrono::seconds{20},
                                         now);
    ASSERT_TRUE(live)
        << "a probe that answered 20s ago is reachability, whatever the "
           "height source says";

    const auto flags = node_health_flags(live,
                                         /*node_reports_synced=*/false,
                                         /*node_reports_syncing=*/true);
    EXPECT_TRUE(flags.connected)
        << "this published \"Full Node: Disconnected\" for the whole of a "
           "catch-up, because the node was not the selected height source";
    EXPECT_FALSE(flags.synced);
    EXPECT_TRUE(flags.syncing)
        << "the catch-up is the one period an operator is looking";
}

// The window is not a guess: it must cover the throttled recovery cadence,
// or a healthy recovering node blinks dark between two successful probes.
TEST(NodeProbeLiveness, TheWindowCoversTheThrottledProbeCadence)
{
    const auto now = Clock::now();
    // kNodeProbeEveryNPolls (10) * kPollInterval (5s).
    EXPECT_TRUE(node_probe_is_live(true, now - std::chrono::seconds{50}, now));
    EXPECT_GE(kNodeProbeLivenessWindow, std::chrono::seconds{50});
}

// The anti-stale property the height-source gate used to carry: a node that
// has stopped answering must not have its CACHED reading republished by a
// wallet-sourced heartbeat.
TEST(NodeProbeLiveness, AStaleProbeDoesNotRepublishTheCache)
{
    const auto now  = Clock::now();
    const auto live = node_probe_is_live(
        true, now - (kNodeProbeLivenessWindow + std::chrono::seconds{30}),
        now);
    EXPECT_FALSE(live);

    // last_sync_state() still holds the healthy reading from before the
    // outage.  None of it may be published.
    const auto flags = node_health_flags(live, /*synced=*/true,
                                         /*syncing=*/false);
    EXPECT_FALSE(flags.connected);
    EXPECT_FALSE(flags.synced)
        << "the cached reading is stale -- republishing it is the outage-"
           "masking this whole separation exists to stop";
    EXPECT_FALSE(flags.syncing);
}

// "Never probed in this run" is not liveness either: the cached flags are
// still their zero-initialised defaults and say nothing about this node.
TEST(NodeProbeLiveness, ANeverProbedNodePublishesNothing)
{
    const auto flags = node_health_flags(
        node_probe_is_live(true, Clock::time_point{}, Clock::now()),
        /*node_reports_synced=*/true, /*node_reports_syncing=*/false);
    EXPECT_FALSE(flags.connected);
    EXPECT_FALSE(flags.synced);
    EXPECT_FALSE(flags.syncing);
}

// Wallet-only configuration, or a client closed by a startup fallback: there
// is nothing to be reachable.
TEST(NodeProbeLiveness, AClosedNodeClientIsNotLive)
{
    const auto now = Clock::now();
    EXPECT_FALSE(node_probe_is_live(/*node_client_open=*/false, now, now))
        << "a fresh probe stamp cannot make a closed client reachable";
}

// A clock that steps backwards must not read as a stale probe and blank a
// gauge that is answering.
TEST(NodeProbeLiveness, AProbeInTheFutureIsTreatedAsLive)
{
    const auto now = Clock::now();
    EXPECT_TRUE(node_probe_is_live(true, now + std::chrono::seconds{5}, now));
}
