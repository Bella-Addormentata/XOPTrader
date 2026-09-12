// ---------------------------------------------------------------------------
// Dexie reachability freshness -- review comment 3997548811 on PR #148.
//
// THE FINDING: the GUI's Dexie dot was driven by `metrics_connected`, which
// reports only whether the GUI can scrape THIS engine's Prometheus endpoint.
// With the engine healthy and every Dexie ticker failing, the dot stayed
// green -- engine health standing in for venue reachability.
//
// dexie_probe_is_live() is the engine-side signal that replaces it.  These
// tests drive the PURE predicate, which is the only part reachable from
// ctest.
//
// MUTATION CHECK: make dexie_probe_is_live ignore freshness,
//
//     return dexie_client_open;      // the whole body
//
// -> ANeverAnsweredVenueIsNotReachable and AStaleAnswerIsNotReachability go
//    RED.  Drop only the never-answered clause and the first goes red alone.
//
// WHAT THIS CANNOT SEE, stated plainly: no test constructs an Engine, so
// neither the Step 1 stamp nor the two publish sites is reachable from
// ctest.  What IS pinned is that both sites share one accessor
// (Engine::dexie_probe_live_now), which is why the node expressions could
// drift apart at two sites before S33.
// ---------------------------------------------------------------------------

#include <chrono>

#include <gtest/gtest.h>

#include "xop/engine.hpp"

using xop::dexie_probe_is_live;
using xop::kDexieProbeLivenessWindow;

namespace {
using Clock = std::chrono::steady_clock;
}  // namespace

// THE finding, as a test: the engine is up and scrapeable, and Dexie is not
// answering.  The old signal could not express this state at all.
TEST(DexieProbeLiveness, AStaleAnswerIsNotReachability)
{
    const auto now = Clock::now();
    EXPECT_FALSE(dexie_probe_is_live(
        /*dexie_client_open=*/true,
        /*last_success=*/now - kDexieProbeLivenessWindow
            - std::chrono::seconds{1},
        now))
        << "an answer older than the window is memory, not reachability";
}

TEST(DexieProbeLiveness, ARecentAnswerIsReachability)
{
    const auto now = Clock::now();
    EXPECT_TRUE(dexie_probe_is_live(true, now - std::chrono::seconds{30}, now));
}

// Never probed in this run: the absence of evidence is not evidence of
// reachability.  Same clause as node_probe_is_live's.
TEST(DexieProbeLiveness, ANeverAnsweredVenueIsNotReachable)
{
    EXPECT_FALSE(dexie_probe_is_live(true, Clock::time_point{}, Clock::now()))
        << "a default-constructed stamp means nothing has been observed";
}

TEST(DexieProbeLiveness, AClosedClientIsNotReachable)
{
    const auto now = Clock::now();
    EXPECT_FALSE(dexie_probe_is_live(/*dexie_client_open=*/false, now, now));
}

// The boundary is inclusive, and it is the one an off-by-one would move.
TEST(DexieProbeLiveness, TheWindowBoundaryIsInclusive)
{
    const auto now = Clock::now();
    EXPECT_TRUE(dexie_probe_is_live(true, now - kDexieProbeLivenessWindow, now))
        << "exactly at the window is still live";
    EXPECT_FALSE(dexie_probe_is_live(
        true, now - kDexieProbeLivenessWindow - std::chrono::seconds{1}, now));
}

// A clock oddity is not evidence of staleness.
TEST(DexieProbeLiveness, AStampInTheFutureIsNotTreatedAsStale)
{
    const auto now = Clock::now();
    EXPECT_TRUE(dexie_probe_is_live(true, now + std::chrono::seconds{5}, now));
}

// The window is not a guess: Step 1 is BLOCK-gated, and this engine's own
// logs record a worst single block interval of 181 s over 2,418 measured.
// A window at 3x the ~52 s MEAN would blink the gauge dark on one slow
// block -- a false "Disconnected" on a healthy venue.
TEST(DexieProbeLiveness, TheWindowCoversTheWorstObservedBlockInterval)
{
    const auto now = Clock::now();
    EXPECT_TRUE(dexie_probe_is_live(
        true, now - std::chrono::seconds{181}, now))
        << "the worst single block interval this engine has logged must not "
           "darken the gauge";
    EXPECT_TRUE(dexie_probe_is_live(
        true, now - std::chrono::seconds{233}, now))
        << "nor that interval followed by one further block";
}
