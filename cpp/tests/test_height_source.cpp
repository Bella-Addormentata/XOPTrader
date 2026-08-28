// test_height_source.cpp -- S28: the engine must help itself when the full
// node goes away and the wallet is still answering.

#include <gtest/gtest.h>

#include "xop/risk/height_source.hpp"

#include <cstdint>
#include <limits>

using xop::risk::HeightSource;
using xop::risk::HeightSourceState;
using xop::risk::height_fallback_allowed;
using xop::risk::height_is_usable;
using xop::risk::kNodeFailuresBeforeWalletFallback;
using xop::risk::kNodeSuccessesBeforeReturn;
using xop::risk::next_height_source;

namespace {

HeightSource run(HeightSourceState& s, bool ok, int times,
                 bool auto_mode = true) {
    HeightSource last = s.current;
    for (int i = 0; i < times; ++i) {
        last = next_height_source(s, ok, auto_mode);
    }
    return last;
}

}  // namespace

// ---------------------------------------------------------------------------
// The incident
// ---------------------------------------------------------------------------

TEST(HeightSource, FallsBackToTheWalletRatherThanPollingADeadNodeForever) {
    // 2026-08-25: 529 consecutive get_block_height failures over ~2.5h with a
    // healthy wallet RPC the whole time. The heartbeat is driven by observing
    // new blocks, so this was 2.5h of no quote refresh, no cancel, no breaker
    // evaluation -- with offers resting on dexie throughout.
    HeightSourceState s;
    EXPECT_EQ(run(s, false, kNodeFailuresBeforeWalletFallback),
              HeightSource::Wallet);
}

TEST(HeightSource, ASingleFailedPollDoesNotSwitch) {
    // A transient must not flap the source: the two RPCs disagree about the
    // tip by a block or two, so bouncing between them would make the engine
    // reprocess or skip heights.
    HeightSourceState s;
    EXPECT_EQ(next_height_source(s, false, true), HeightSource::FullNode);
}

TEST(HeightSource, ARecoveryBeforeTheThresholdResetsTheCount) {
    HeightSourceState s;
    run(s, false, kNodeFailuresBeforeWalletFallback - 1);
    EXPECT_EQ(next_height_source(s, true, true), HeightSource::FullNode);
    EXPECT_EQ(s.consecutive_node_failures, 0u);
    // ...and the next failure starts from scratch rather than tipping over.
    EXPECT_EQ(next_height_source(s, false, true), HeightSource::FullNode);
}

// ---------------------------------------------------------------------------
// Coming back
// ---------------------------------------------------------------------------

TEST(HeightSource, ReturnsToTheNodeOnceItIsReliablyBack) {
    HeightSourceState s;
    run(s, false, kNodeFailuresBeforeWalletFallback);
    ASSERT_EQ(s.current, HeightSource::Wallet);
    EXPECT_EQ(run(s, true, kNodeSuccessesBeforeReturn), HeightSource::FullNode);
}

TEST(HeightSource, AFlappingNodeDoesNotDragTheSourceBackAndForth) {
    // The dangerous shape: a node answering intermittently while it validates
    // a backlog is WORSE than one cleanly down, because every brief return
    // would otherwise pull the height source back. Returning is not urgent --
    // the wallet is already answering.
    HeightSourceState s;
    run(s, false, kNodeFailuresBeforeWalletFallback);
    ASSERT_EQ(s.current, HeightSource::Wallet);

    for (int cycle = 0; cycle < 20; ++cycle) {
        run(s, true, static_cast<int>(kNodeSuccessesBeforeReturn) - 1);
        EXPECT_EQ(next_height_source(s, false, true), HeightSource::Wallet)
            << "cycle " << cycle;
    }
}

TEST(HeightSource, ReturningNeedsMoreEvidenceThanLeaving) {
    EXPECT_GT(kNodeSuccessesBeforeReturn, kNodeFailuresBeforeWalletFallback)
        << "leaving a dead node should be quicker than trusting it again";
}

// ---------------------------------------------------------------------------
// Configuration pins the source
// ---------------------------------------------------------------------------

TEST(HeightSource, OnlyAutoModeMaySwitch) {
    EXPECT_TRUE(height_fallback_allowed(true));
    EXPECT_FALSE(height_fallback_allowed(false));
}

TEST(HeightSource, ExplicitFullNodeModeNeverSilentlyDowngrades) {
    // The operator asked for the node specifically. Quietly serving wallet
    // heights would hide the very thing they wanted to be told about.
    HeightSourceState s;
    EXPECT_EQ(run(s, false, 100, /*auto_mode=*/false), HeightSource::FullNode);
}

// ---------------------------------------------------------------------------
// A wallet height must not move the engine backwards
// ---------------------------------------------------------------------------

TEST(HeightSource, AWalletHeightBehindTheTipIsRejected) {
    // The wallet is often a block or two behind the node. Accepting a lower
    // height on the way into a fallback would look like a reorg to every
    // downstream consumer.
    EXPECT_TRUE(height_is_usable(9'205'640, 9'205'640));
    EXPECT_TRUE(height_is_usable(9'205'641, 9'205'640));
    EXPECT_FALSE(height_is_usable(9'205'639, 9'205'640));
}

TEST(HeightSource, TheNodesHigherTipIsAcceptedImmediatelyOnTheWayBack) {
    EXPECT_TRUE(height_is_usable(9'205'700, 9'205'640));
}

TEST(HeightSource, ANegativeHeightIsRejected) {
    // BlockHeight is uint32_t; a negative int64 would wrap to an enormous
    // block number and manufacture a phantom "new block".
    EXPECT_FALSE(height_is_usable(-1, 0));
    EXPECT_FALSE(height_is_usable(-9'205'640, 9'205'640));
}

TEST(HeightSource, CountersDoNotOverflowUnderALongOutage) {
    // 529 failures happened once already; the counters saturate rather than
    // wrap, so a genuinely long outage cannot roll one back under threshold.
    HeightSourceState s;
    run(s, false, 5000);
    EXPECT_EQ(s.current, HeightSource::Wallet);
    EXPECT_LE(s.consecutive_node_failures, kNodeFailuresBeforeWalletFallback);
}

// ---------------------------------------------------------------------------
// [S28] The caller narrows to BlockHeight (uint32_t) the moment this returns
// true, so "usable" has to mean "survives that narrowing", not merely
// "non-negative and not going backwards".
// ---------------------------------------------------------------------------

TEST(HeightIsUsable, RejectsHeightsAboveTheBlockHeightRange) {
    // INT64_MAX would arrive downstream as 4,294,967,295 -- a phantom tip
    // millions of blocks ahead that no real height can ever beat, wedging
    // the engine on a number no chain produced.
    EXPECT_FALSE(height_is_usable(std::numeric_limits<std::int64_t>::max(), 0));
    EXPECT_FALSE(height_is_usable(
        static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1,
        0));
}

TEST(HeightIsUsable, AcceptsTheTopOfTheBlockHeightRange) {
    // The boundary itself is representable and must still be accepted.
    EXPECT_TRUE(height_is_usable(
        static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()),
        0));
}

TEST(HeightIsUsable, StillRejectsNegativeAndBackwards) {
    EXPECT_FALSE(height_is_usable(-1, 0));
    EXPECT_FALSE(height_is_usable(99, 100));
    EXPECT_TRUE(height_is_usable(100, 100));
    EXPECT_TRUE(height_is_usable(101, 100));
}
