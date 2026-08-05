// test_wallet_poll_throttle.cpp -- Unit tests for the wallet-RPC polling
// throttles ([WALLET-LOAD 2026-08-04]).
//
// Live numbers behind the scenarios: ~26 tracked offers, ~19 min
// heartbeat, reconcile every ~6 min over a 14,100+-record trade archive
// (~282 pages of 50).
//
// ISO/IEC 27001:2022 -- no secrets; pure logic verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/config.hpp>
#include <xop/execution/wallet_poll_throttle.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace xop::execution;

// ============================================================================
// Age gate: a just-posted offer cannot have settled
// ============================================================================

TEST(FillPollThrottleTest, AgeGateSkipsFreshOffers) {
    // min_age = 2 (config default): ages 0 and 1 are skipped even when the
    // market sits on top of the offer -- a spend needs a block to confirm.
    for (std::int64_t age : {0, 1}) {
        EXPECT_FALSE(fill_poll_due(age, 2, 0, 10, 3, 7, /*striking=*/true))
            << "age " << age;
        EXPECT_FALSE(fill_poll_due(age, 2, 0, 10, 3, 7, /*striking=*/false));
    }
    // At the gate: polled.
    EXPECT_TRUE(fill_poll_due(2, 2, 0, 10, 3, 7, false));
    // Unknown age (-1, e.g. adopted offer with created_at_block 0): the
    // gate does not apply -- fail-safe toward polling.
    EXPECT_TRUE(fill_poll_due(-1, 2, 0, 10, 3, 7, false));
    // Gate disabled by config 0.
    EXPECT_TRUE(fill_poll_due(0, 0, 0, 10, 3, 7, false));
}

// ============================================================================
// Backoff schedule and its striking-distance reset
// ============================================================================

TEST(FillPollThrottleTest, BackoffSchedule) {
    // Defaults M=10, K=3.  Below M consecutive pending polls: every
    // heartbeat.
    for (std::uint32_t consecutive : {0u, 5u, 9u}) {
        for (std::uint64_t hb = 1; hb <= 6; ++hb) {
            EXPECT_TRUE(fill_poll_due(50, 2, consecutive, 10, 3, hb, false));
        }
    }
    // At/after M: only every 3rd heartbeat.
    int polled = 0;
    for (std::uint64_t hb = 1; hb <= 9; ++hb) {
        const bool due = fill_poll_due(50, 2, 10, 10, 3, hb, false);
        EXPECT_EQ(due, hb % 3 == 0) << "hb " << hb;
        polled += due ? 1 : 0;
    }
    EXPECT_EQ(polled, 3);   // 3 of 9 heartbeats -> ~1 poll/hour at 19 min

    // M = 0 disables the backoff entirely.
    EXPECT_TRUE(fill_poll_due(50, 2, 1000, 0, 3, 7, false));
    // K <= 1 is every-heartbeat regardless.
    EXPECT_TRUE(fill_poll_due(50, 2, 1000, 10, 1, 7, false));
    EXPECT_TRUE(fill_poll_due(50, 2, 1000, 10, 0, 7, false));
}

TEST(FillPollThrottleTest, StrikingDistanceResetsBackoff) {
    // A deeply backed-off offer is polled the moment the book is near it.
    EXPECT_TRUE(fill_poll_due(500, 2, 999, 10, 3, 7, /*striking=*/true));

    // within_striking_distance: offer posted 230 bps from mid
    // (post_spread_bps = 230, e.g. an outer XCH/wUSDC.b tier at mid
    // 1.48e12).  Reset bound = 2 x 230 = 460 bps.
    const xop::Mojo mid_at_post = 1'480'000'000'000LL;
    const xop::Mojo price = 1'514'040'000'000LL;   // +230 bps
    // Mid rallies toward the ask: now 100 bps away -> in range.
    EXPECT_TRUE(within_striking_distance(
        price, 1'498'960'000'000LL /* ~100bps below price */, 230.0));
    // Mid at post distance (230 bps) -> in range (230 <= 460).
    EXPECT_TRUE(within_striking_distance(price, mid_at_post, 230.0));
    // Mid far below: 800 bps away -> out of range, backoff may engage.
    EXPECT_FALSE(within_striking_distance(
        price, 1'397'000'000'000LL /* ~838bps below price */, 230.0));

    // FAIL-SAFE branches: unknown post distance, missing market data, or
    // degenerate factor all mean "stay vigilant" (always poll).
    EXPECT_TRUE(within_striking_distance(price, mid_at_post, 0.0));
    EXPECT_TRUE(within_striking_distance(price, 0, 230.0));
    EXPECT_TRUE(within_striking_distance(0, mid_at_post, 230.0));
    EXPECT_TRUE(within_striking_distance(price, mid_at_post, 230.0, 0.0));
}

TEST(FillPollThrottleTest, ConfigDefaults) {
    const xop::StrategyConfig d{};
    EXPECT_EQ(d.detect_fills_min_age_blocks, 2u);
    EXPECT_EQ(d.detect_fills_backoff_polls, 10u);
    EXPECT_EQ(d.detect_fills_backoff_interval, 3u);
}

// ============================================================================
// Reconcile pagination early-stop
// ============================================================================

TEST(ReconcileEarlyStopTest, CutoffAndPageClassification) {
    // Oldest tracked offer created at t=1,000,000; 24 h slack.
    EXPECT_EQ(reconcile_scan_cutoff(1'000'000, 2'000'000), 913'600);
    // Nothing tracked: anchor at now.
    EXPECT_EQ(reconcile_scan_cutoff(0, 2'000'000), 1'913'600);
    // Base smaller than slack: clamps to 0 (scan everything -- fail open).
    EXPECT_EQ(reconcile_scan_cutoff(3'600, 5'000), 0);

    // Page classification: strictly older than the cutoff.
    EXPECT_TRUE(page_entirely_older(913'599, 913'600));
    EXPECT_FALSE(page_entirely_older(913'600, 913'600));
    // Unparseable page timestamps (0) fail OPEN -- keep scanning.
    EXPECT_FALSE(page_entirely_older(0, 913'600));
    // Zero cutoff (fail-open case above) never stops.
    EXPECT_FALSE(page_entirely_older(5, 0));
}

TEST(ReconcileEarlyStopTest, RequiresConsecutiveOldPages) {
    ReconcileEarlyStop stop;   // default P = 2
    EXPECT_FALSE(stop.observe_page(true));    // 1 old page: keep going
    EXPECT_FALSE(stop.observe_page(false));   // a new page RESETS the run
    EXPECT_FALSE(stop.observe_page(true));
    EXPECT_TRUE(stop.observe_page(true));     // 2 consecutive: stop
}

// ----------------------------------------------------------------------------
// Simulated page stream: the early-stopped scan terminates quickly and
// still sees every tracked offer and every PENDING_ACCEPT adoptee.
//
// The stream mimics the VERIFIED RELEVANCE ordering (docs.chia.net
// offer-rpc + chia-blockchain trade_store.py): pending statuses first,
// then terminal records by created_at_time DESC.
// ----------------------------------------------------------------------------

struct SimRecord {
    std::string  id;
    std::int64_t created_at;
    bool         pending;
};

TEST(ReconcileEarlyStopTest, SimulatedStreamSeesAllTrackedThenStops) {
    constexpr std::int64_t kDay = 86'400;
    const std::int64_t now = 100 * kDay;

    // Tracked set: 26 live offers created over the last 3 days (like the
    // live book), plus one tracked offer that went terminal (cancelled
    // behind our back yesterday) -- reconcile must see its record.
    std::vector<SimRecord> stream;
    std::int64_t oldest_tracked = 0;
    std::set<std::string> tracked_ids;
    for (int i = 0; i < 26; ++i) {
        SimRecord r{"live" + std::to_string(i),
                    now - (i % 3) * kDay - i * 60, true};
        tracked_ids.insert(r.id);
        oldest_tracked = (oldest_tracked == 0)
                             ? r.created_at
                             : std::min(oldest_tracked, r.created_at);
        stream.push_back(r);
    }
    // One untracked PENDING_ACCEPT adoptee (lost tracking ~2 days ago).
    stream.push_back({"adoptee", now - 2 * kDay, true});
    // RELEVANCE: pending first.  Then the terminal region, newest first,
    // including the tracked-but-terminal offer...
    SimRecord tracked_terminal{"lost_cancel", now - 1 * kDay, false};
    tracked_ids.insert(tracked_terminal.id);
    oldest_tracked = std::min(oldest_tracked, tracked_terminal.created_at);
    std::vector<SimRecord> terminals{tracked_terminal};
    // ...and a 14,100-record archive stretching back 200 days.
    for (int i = 0; i < 14'100; ++i) {
        terminals.push_back({"hist" + std::to_string(i),
                             now - 4 * kDay - i * (200 * kDay / 14'100),
                             false});
    }
    // Sort terminal region by created_at DESC (the wallet's ordering) and
    // append after the pending block.
    std::sort(terminals.begin(), terminals.end(),
              [](const SimRecord& a, const SimRecord& b) {
                  return a.created_at > b.created_at;
              });
    stream.insert(stream.end(), terminals.begin(), terminals.end());

    // Drive the exact production loop primitives over 50-record pages.
    const std::int64_t cutoff = reconcile_scan_cutoff(oldest_tracked, now);
    ReconcileEarlyStop stop;
    constexpr std::size_t kPageSize = 50;
    std::set<std::string> seen;
    std::size_t pages = 0;
    for (std::size_t off = 0; off < stream.size(); off += kPageSize) {
        ++pages;
        const std::size_t end = std::min(off + kPageSize, stream.size());
        std::int64_t page_newest = 0;
        for (std::size_t i = off; i < end; ++i) {
            page_newest = std::max(page_newest, stream[i].created_at);
            seen.insert(stream[i].id);
        }
        if (stop.observe_page(page_entirely_older(page_newest, cutoff))) {
            break;
        }
    }

    // TERMINATES: a handful of pages, not the ~283-page full walk.
    const std::size_t full_walk_pages =
        (stream.size() + kPageSize - 1) / kPageSize;
    EXPECT_GE(full_walk_pages, 283u);
    EXPECT_LE(pages, 6u) << "early stop must cut ~283 pages to a handful";

    // COMPLETE: every tracked offer (live + terminal) and the adoptee
    // were seen before the stop.
    for (const auto& id : tracked_ids) {
        EXPECT_TRUE(seen.count(id)) << "tracked offer " << id
                                    << " missed by the early-stopped scan";
    }
    EXPECT_TRUE(seen.count("adoptee"));
}

}  // namespace
