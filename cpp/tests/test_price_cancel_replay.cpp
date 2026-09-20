// ---------------------------------------------------------------------------
// [S72 2026-09-20] Replay of price_cancel_mode: margin over RECORDED offers.
//
// The unit tests in test_cross_guard.cpp pin what the margin rule decides.
// This file measures what it would have DONE: every offer the live bot
// created in the 14 days to block 9,319,413 (2026-09-20), frozen by
// scripts/gen_price_cancel_replay_fixture.py out of the engine database
// (read-only), is put through the real predicate, and the counts are pinned.
//
// READ THE NUMBERS FOR WHAT THEY ARE.
//
//   * They were not tuned.  The default retain fraction (0.5) was fixed on a
//     structural argument BEFORE this replay existed -- a cancel threshold
//     equal to the posting threshold flaps, see cross_guard.hpp -- and the
//     table below reports the literal rule (1.0) beside it, unflattering
//     parts included.
//   * Step 7's centre and floor are not persisted; its OUTPUT is
//     (strategy_quotes, one ladder per block).  The replay reconstructs
//         centre' = (inner_bid + inner_ask) / 2
//         floor'  = (inner_ask - inner_bid) / (2 centre')
//     from the innermost tier on each side.  Every tier is at or beyond
//     Step 7's floor edge, so at retain 1.0 "resting price inside the
//     innermost same-side tier" is NECESSARY for a real breach and the count
//     is an UPPER BOUND.  Below 1.0 it is an estimate, not a bound: a side
//     clamped outward by the order-book guard shifts centre' toward it.
//   * It is open-loop.  An offer the margin rule keeps would have had a
//     different later life, and one it cancels would have been reposted at a
//     different price.  "Fires at some block of its recorded life" counts
//     lifetimes, not cancels.
//   * ONE centre.  The live rule cancels only when an offer fails against
//     BOTH of Step 7's centres (shifted ladder centre and fair value).  The
//     recorded ladder yields one centre -- the shifted one -- so the replay
//     runs that test alone, which can only OVERSTATE what the live rule
//     cancels.
//   * Two of the three live pairs recorded ladders in the window (XCH/DBX
//     39,212 blocks, XCH/BYC 4,237); a block with no ladder is skipped, and an
//     offer with none at all is reported as "no witness", never as "kept".
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "xop/execution/cross_guard.hpp"

namespace {

using xop::execution::classify_tier_refresh_margin;
using xop::execution::MarginRefresh;

enum class Cause { PriceAdverse, TtlExpired, ExposureFloor, Filled, Other };
constexpr std::size_t kCauses = 5;

struct ReplayRow {
    Cause        cause;
    bool         is_ask;
    std::int64_t price;
    std::int64_t submit_inner_bid;
    std::int64_t submit_inner_ask;
    std::int64_t worst_inner_bid;
    std::int64_t worst_inner_ask;
};

constexpr ReplayRow kRows[] = {
#include "price_cancel_replay_fixture.inc"
};
// clang-format on

enum class Replay { NoWitness, Keeps, Fires };

/// The real predicate, fed the reconstructed reference.  `crossed` and the
/// minimum-age guard are off: the generator only keeps blocks past
/// kMinRefreshAgeBlocks, and a crossed offer is cancelled under either rule.
[[nodiscard]] Replay replay(const ReplayRow& row, std::int64_t inner_bid,
                            std::int64_t inner_ask, double retain)
{
    if (inner_bid <= 0 || inner_ask <= inner_bid) {
        return Replay::NoWitness;
    }
    const double bid = static_cast<double>(inner_bid);
    const double ask = static_cast<double>(inner_ask);
    const double centre    = (bid + ask) / 2.0;
    const double floor_bps = (ask - bid) / (2.0 * centre) * 10'000.0;
    switch (classify_tier_refresh_margin(
                /*crossed=*/false, /*below_min_age=*/false, row.is_ask,
                static_cast<double>(row.price), centre,
                /*fair_centre=*/0.0, floor_bps, retain)) {
        case MarginRefresh::Stale:       return Replay::Fires;
        case MarginRefresh::Fresh:       return Replay::Keeps;
        case MarginRefresh::NoReference: break;
    }
    return Replay::NoWitness;
}

struct Tally {
    std::array<int, kCauses> fires{};
    std::array<int, kCauses> keeps{};
    std::array<int, kCauses> no_witness{};
    [[nodiscard]] int total_fires() const
    {
        int n = 0;
        for (const int f : fires) n += f;
        return n;
    }
};

[[nodiscard]] Tally tally(double retain, bool at_submit)
{
    Tally t;
    for (const ReplayRow& row : kRows) {
        const auto c = static_cast<std::size_t>(row.cause);
        const Replay r = at_submit
            ? replay(row, row.submit_inner_bid, row.submit_inner_ask, retain)
            : replay(row, row.worst_inner_bid, row.worst_inner_ask, retain);
        switch (r) {
            case Replay::Fires:     ++t.fires[c];      break;
            case Replay::Keeps:     ++t.keeps[c];      break;
            case Replay::NoWitness: ++t.no_witness[c]; break;
        }
    }
    return t;
}

constexpr auto kPrice = static_cast<std::size_t>(Cause::PriceAdverse);
constexpr auto kTtl   = static_cast<std::size_t>(Cause::TtlExpired);
constexpr auto kExpo  = static_cast<std::size_t>(Cause::ExposureFloor);
constexpr auto kFill  = static_cast<std::size_t>(Cause::Filled);
constexpr auto kOther = static_cast<std::size_t>(Cause::Other);

}  // namespace

TEST(PriceCancelReplay, TheFixtureIsTheRecordedFortnight)
{
    // If these move, the fixture was regenerated over a different window and
    // every count below is measuring something else.
    std::array<int, kCauses> n{};
    for (const ReplayRow& row : kRows) {
        ++n[static_cast<std::size_t>(row.cause)];
    }
    EXPECT_EQ(std::size(kRows), 1283u);
    EXPECT_EQ(n[kPrice], 282);
    EXPECT_EQ(n[kTtl],   402);
    EXPECT_EQ(n[kExpo],  528);
    EXPECT_EQ(n[kFill],   15);
    EXPECT_EQ(n[kOther],  56);
}

TEST(PriceCancelReplay, HowManyOfTheRecordedPriceCancelsTheMarginRuleWouldHaveMade)
{
    // The question as asked: at the block the deviation rule submitted each
    // of its 282 price_adverse cancels, would the margin rule have cancelled
    // too?  34 have no ladder within 20 blocks of the submit (no witness);
    // of the other 248:
    struct Expect { double retain; int fires; int keeps; };
    const Expect rows[] = {
        {1.00, 122, 126},   // the literal rule -- an UPPER bound, see banner
        {0.75, 121, 127},
        {0.50,  82, 166},   // the default
        {0.25,  64, 184},
    };
    for (const Expect& e : rows) {
        const Tally t = tally(e.retain, /*at_submit=*/true);
        EXPECT_EQ(t.fires[kPrice], e.fires) << "retain " << e.retain;
        EXPECT_EQ(t.keeps[kPrice], e.keeps) << "retain " << e.retain;
        EXPECT_EQ(t.no_witness[kPrice], 34) << "retain " << e.retain;
    }
}

TEST(PriceCancelReplay, WhatItWouldHaveDoneToEveryOtherOffer)
{
    // The half the question above does not ask, and the reason the literal
    // rule is not the default: the margin rule also fires on offers the
    // deviation rule LEFT ALONE.  Over each offer's whole recorded life (from
    // kMinRefreshAgeBlocks on), lifetimes on which it fires at some block:
    struct Expect { double retain; int total; int price; int ttl; int filled; };
    const Expect rows[] = {
        // At 1.0 it fires on MORE lifetimes (347) than the deviation rule
        // made price cancels (282): offers born AT the floor trip it on any
        // adverse tick.  That is the flapping switch cross_guard.hpp names.
        {1.00, 347, 191, 119, 7},
        {0.75, 247, 135,  80, 7},
        {0.50, 147,  84,  38, 7},
        {0.25,  89,  64,   6, 6},
    };
    for (const Expect& e : rows) {
        const Tally t = tally(e.retain, /*at_submit=*/false);
        EXPECT_EQ(t.total_fires(), e.total) << "retain " << e.retain;
        EXPECT_EQ(t.fires[kPrice], e.price) << "retain " << e.retain;
        EXPECT_EQ(t.fires[kTtl],   e.ttl)   << "retain " << e.retain;
        // 7 of the 15 offers that FILLED rested, at some block, on less than
        // half the edge a new offer would have needed.  By the rule's own
        // definition those are the fills it exists to avoid; to the operator
        // they are 7 of a fortnight's 15 fills.  Disclosed, not argued.
        EXPECT_EQ(t.fires[kFill],  e.filled) << "retain " << e.retain;
    }
}

TEST(PriceCancelReplay, ALowerRetainNeverFiresOnMore)
{
    // The knob is monotone: what fires at r fires at every r' > r.
    int prev_submit = -1;
    int prev_life   = -1;
    for (const double retain : {0.05, 0.25, 0.5, 0.75, 1.0}) {
        const int s = tally(retain, true).total_fires();
        const int l = tally(retain, false).total_fires();
        EXPECT_GE(s, prev_submit) << retain;
        EXPECT_GE(l, prev_life) << retain;
        prev_submit = s;
        prev_life   = l;
    }
}

TEST(PriceCancelReplay, ReportsTheTable)
{
    // Not an assertion: the table the PR quotes, reproducible with
    //   xop_tests --gtest_filter=PriceCancelReplay.ReportsTheTable
    const char* const names[kCauses] = {"price_adverse", "ttl_expired",
                                        "exposure_floor", "filled", "other"};
    for (const bool at_submit : {true, false}) {
        for (const double retain : {1.0, 0.75, 0.5, 0.25}) {
            const Tally t = tally(retain, at_submit);
            std::printf("[replay] %s retain=%.2f fires=%d\n",
                        at_submit ? "at-submit " : "whole-life", retain,
                        t.total_fires());
            for (std::size_t c = 0; c < kCauses; ++c) {
                std::printf("[replay]     %-15s fires=%4d keeps=%4d "
                            "no_witness=%4d\n", names[c], t.fires[c],
                            t.keeps[c], t.no_witness[c]);
            }
        }
    }
}
