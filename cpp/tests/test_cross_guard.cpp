// ---------------------------------------------------------------------------
// [CROSSGUARD] The pre-post crossing predicate, both versions.
//
// Step 8 suppresses on the PUBLISHED-MID verdict; the canceller
// (classify_tier_staleness) acts on the BBO verdict. They were the same rule
// when the guard was written in 4d3f30d and diverged one day later in
// a932a5d, which moved the canceller onto the BBO and left the guard behind.
// The guard has never been modified since.
//
// These pin BOTH rules and, more importantly, the SHAPE OF THEIR
// DISAGREEMENT -- which is the whole output of the shadow counter.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <limits>
#include <utility>

#include "xop/execution/cross_guard.hpp"

using xop::execution::classify_cross_bbo;
using xop::execution::classify_cross_published_mid;
using xop::execution::CrossVerdict;

namespace {
constexpr bool kBid = false;
constexpr bool kAsk = true;

// A normal uncrossed book.
constexpr double kBestBid = 99.0;
constexpr double kBestAsk = 101.0;
constexpr double kMid     = 100.0;

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

// -- Each rule on its own ---------------------------------------------------

TEST(CrossGuard, PublishedMidRuleIsBidAboveMidAskBelowMid)
{
    EXPECT_EQ(classify_cross_published_mid(kBid, 100.5, kMid),
              CrossVerdict::Crossed);
    EXPECT_EQ(classify_cross_published_mid(kBid, 99.5, kMid),
              CrossVerdict::Ok);
    EXPECT_EQ(classify_cross_published_mid(kAsk, 99.5, kMid),
              CrossVerdict::Crossed);
    EXPECT_EQ(classify_cross_published_mid(kAsk, 100.5, kMid),
              CrossVerdict::Ok);
    // Exactly at the mid is NOT crossed on either side: the live rule is a
    // strict inequality, and the shadow must not quietly change that.
    EXPECT_EQ(classify_cross_published_mid(kBid, kMid, kMid),
              CrossVerdict::Ok);
    EXPECT_EQ(classify_cross_published_mid(kAsk, kMid, kMid),
              CrossVerdict::Ok);
}

TEST(CrossGuard, BboRuleMirrorsTheCancellerIncludingItsInequalities)
{
    // offer_manager.cpp: bid crosses iff price >= best_ask;
    //                    ask crosses iff price <= best_bid.
    // Both are NON-strict. Pinned separately from the mid rule's strict
    // ones, because a shadow that quietly used the same inequality on both
    // sides would under-report the disagreement it exists to measure.
    EXPECT_EQ(classify_cross_bbo(kBid, kBestAsk, kBestBid, kBestAsk, kMid)
                  .verdict, CrossVerdict::Crossed);
    EXPECT_EQ(classify_cross_bbo(kBid, kBestAsk - 0.01, kBestBid, kBestAsk, kMid)
                  .verdict, CrossVerdict::Ok);
    EXPECT_EQ(classify_cross_bbo(kAsk, kBestBid, kBestBid, kBestAsk, kMid)
                  .verdict, CrossVerdict::Crossed);
    EXPECT_EQ(classify_cross_bbo(kAsk, kBestBid + 0.01, kBestBid, kBestAsk, kMid)
                  .verdict, CrossVerdict::Ok);
}

// -- The disagreement, which is the point -----------------------------------

TEST(CrossGuard, TheGuardKillsTheProfitableHalfSpreadOnBothSides)
{
    // THE FINDING. On an uncrossed book best_bid <= mid <= best_ask, so the
    // published-mid rule removes every ask in (best_bid, mid] and every bid
    // in [mid, best_ask) that the canceller would leave alone. That interval
    // is exactly the profitable half-spread.
    //
    // A bid at 100.5: inside the spread, below best_ask, a perfectly good
    // competitive bid -- the canceller says Ok, the guard drops it.
    EXPECT_EQ(classify_cross_published_mid(kBid, 100.5, kMid),
              CrossVerdict::Crossed);
    EXPECT_EQ(classify_cross_bbo(kBid, 100.5, kBestBid, kBestAsk, kMid).verdict,
              CrossVerdict::Ok);

    // Mirror on the ask side: 99.5 is above best_bid, so it cannot be lifted
    // immediately, yet the guard removes it.
    EXPECT_EQ(classify_cross_published_mid(kAsk, 99.5, kMid),
              CrossVerdict::Crossed);
    EXPECT_EQ(classify_cross_bbo(kAsk, 99.5, kBestBid, kBestAsk, kMid).verdict,
              CrossVerdict::Ok);
}

TEST(CrossGuard, TheyAgreeOnAGenuineCross)
{
    // The case the guard was built for still works under both rules, which
    // is why the disagreement is one-directional rather than a wholesale
    // difference of opinion.
    EXPECT_EQ(classify_cross_published_mid(kBid, 102.0, kMid),
              CrossVerdict::Crossed);
    EXPECT_EQ(classify_cross_bbo(kBid, 102.0, kBestBid, kBestAsk, kMid).verdict,
              CrossVerdict::Crossed);
    EXPECT_EQ(classify_cross_published_mid(kAsk, 98.0, kMid),
              CrossVerdict::Crossed);
    EXPECT_EQ(classify_cross_bbo(kAsk, 98.0, kBestBid, kBestAsk, kMid).verdict,
              CrossVerdict::Crossed);
}

TEST(CrossGuard, TheyAgreeWellOutsideTheSpread)
{
    for (const double bid_px : {90.0, 95.0, 98.9}) {
        EXPECT_EQ(classify_cross_published_mid(kBid, bid_px, kMid),
                  CrossVerdict::Ok);
        EXPECT_EQ(classify_cross_bbo(kBid, bid_px, kBestBid, kBestAsk, kMid)
                      .verdict, CrossVerdict::Ok);
    }
}

TEST(CrossGuard, TheDisagreementWidensAsTheLadderCentreLeavesTheMid)
{
    // Since 2026-08-01 the ladder centre is a fair-value blend that
    // deliberately leaves the published mid behind -- measured p50 99 bps,
    // p99 737 bps on XCH/DBX. Tier prices sit around the CENTRE while this
    // guard's reference is the MID, so the further they separate the more
    // of the ladder falls in the disagreement interval.
    const double centre = 103.0;          // +300 bps above the mid
    // An ask placed just above the centre is nowhere near best_bid.
    const double ask_px = centre + 0.5;
    EXPECT_EQ(classify_cross_bbo(kAsk, ask_px, kBestBid, kBestAsk, kMid).verdict,
              CrossVerdict::Ok);
    EXPECT_EQ(classify_cross_published_mid(kAsk, ask_px, kMid),
              CrossVerdict::Ok) << "above the mid, so both agree here";

    // But a bid placed just below that centre is above the mid, so the guard
    // drops it while the canceller does not -- the whole bid ladder can fall
    // into this interval once the centre is far enough above the mid.
    const double bid_px = centre - 0.5;   // 102.5: > mid 100, < best_ask 101?
    EXPECT_GT(bid_px, kMid);
    EXPECT_EQ(classify_cross_published_mid(kBid, bid_px, kMid),
              CrossVerdict::Crossed);
    // 102.5 IS above best_ask 101, so here the canceller agrees. Widen the
    // book to show the interval that actually disagrees.
    EXPECT_EQ(classify_cross_bbo(kBid, bid_px, kBestBid, 110.0, kMid).verdict,
              CrossVerdict::Ok)
        << "with a wider book the same bid is a valid competitive quote";
}

// -- Inverted books: routine here, and NOT special-cased --------------------

TEST(CrossGuard, AnInvertedBookIsReportedButStillJudged)
{
    // Dexie has no matching engine, so best_bid > best_ask is routine -- it
    // means nobody has taken the arbitrage yet, not that the data is bad.
    // The CANCELLER does not special-case it, so neither does this: the
    // shadow must predict the canceller, not improve on it. The flag is
    // reported so the log can say the reading came from an inverted book.
    const auto r = classify_cross_bbo(kBid, 100.0, /*bid=*/101.0,
                                      /*ask=*/99.0, kMid);
    EXPECT_TRUE(r.book_inverted);
    EXPECT_EQ(r.verdict, CrossVerdict::Crossed) << "100 >= best_ask 99";

    const auto eq = classify_cross_bbo(kAsk, 100.0, 100.0, 100.0, kMid);
    EXPECT_TRUE(eq.book_inverted) << "bid == ask counts as inverted";
}

// -- Fallbacks and degenerate input -----------------------------------------

TEST(CrossGuard, NoBboFallsBackToTheMidWithTheCancellersFivePercentBuffer)
{
    const auto bid = classify_cross_bbo(kBid, 104.0, 0.0, 0.0, kMid);
    EXPECT_TRUE(bid.used_mid_fallback);
    EXPECT_EQ(bid.verdict, CrossVerdict::Ok) << "104 < 100 * 1.05";
    EXPECT_EQ(classify_cross_bbo(kBid, 106.0, 0.0, 0.0, kMid).verdict,
              CrossVerdict::Crossed);

    const auto ask = classify_cross_bbo(kAsk, 96.0, 0.0, 0.0, kMid);
    EXPECT_TRUE(ask.used_mid_fallback);
    EXPECT_EQ(ask.verdict, CrossVerdict::Ok) << "96 > 100 * 0.95";
    EXPECT_EQ(classify_cross_bbo(kAsk, 94.0, 0.0, 0.0, kMid).verdict,
              CrossVerdict::Crossed);

    // The buffer makes the fallback LOOSER than the guard, so the shadow
    // will report disagreement here too -- correctly.
    EXPECT_EQ(classify_cross_published_mid(kBid, 104.0, kMid),
              CrossVerdict::Crossed);
}

TEST(CrossGuard, OneSidedBooksUseTheFallbackNotHalfABbo)
{
    // A single side is not a BBO. Judging against half of one would invent
    // a reference the book does not contain.
    EXPECT_TRUE(classify_cross_bbo(kBid, 100.0, kBestBid, 0.0, kMid)
                    .used_mid_fallback);
    EXPECT_TRUE(classify_cross_bbo(kAsk, 100.0, 0.0, kBestAsk, kMid)
                    .used_mid_fallback);
}

TEST(CrossGuard, NoReferenceAtAllDecidesNothing)
{
    EXPECT_EQ(classify_cross_bbo(kBid, 100.0, 0.0, 0.0, 0.0).verdict,
              CrossVerdict::Indeterminate);
    EXPECT_EQ(classify_cross_published_mid(kBid, 100.0, 0.0),
              CrossVerdict::Indeterminate);
    // Non-finite inputs decide nothing rather than sailing through a
    // comparison: NaN fails every test, which would read as Ok.
    for (const double bad : {kNaN, kInf}) {
        EXPECT_EQ(classify_cross_published_mid(kBid, bad, kMid),
                  CrossVerdict::Indeterminate);
        EXPECT_EQ(classify_cross_published_mid(kBid, 100.0, bad),
                  CrossVerdict::Indeterminate);
        EXPECT_EQ(classify_cross_bbo(kBid, bad, kBestBid, kBestAsk, kMid)
                      .verdict, CrossVerdict::Indeterminate);
    }
}

TEST(CrossGuard, NonFiniteBboFallsBackRatherThanTrusting)
{
    const auto r = classify_cross_bbo(kBid, 100.0, kNaN, kBestAsk, kMid);
    EXPECT_TRUE(r.used_mid_fallback)
        << "a non-finite touch is not a BBO; use the documented fallback";
}

// -- The claim the whole shadow rests on ------------------------------------

TEST(CrossGuard, SuppressionIsByteIdenticalToThePreShadowRule)
{
    // The shadow's entire safety claim is "suppression unchanged". Step 8
    // previously inlined:
    //     bid: tier.price > mid  -> suppress
    //     ask: tier.price < mid  -> suppress
    // and now suppresses on classify_cross_published_mid(...) == Crossed.
    // If those two ever diverge, a measurement-only change has silently
    // become a behaviour change -- which is the single worst outcome here,
    // and exactly the shape of the regression this codebase already shipped
    // once in Step 8.
    //
    // Swept rather than spot-checked, including the degenerate inputs where
    // an inequality and a guarded predicate are most likely to part company.
    const double mids[]   = {1.0, 100.0, 3.24975, 1.41022765, 1e12};
    const double deltas[] = {-1e9, -1.0, -1e-9, 0.0, 1e-9, 1.0, 1e9};

    for (const double m : mids) {
        for (const double d : deltas) {
            const double px = m + d;
            if (!(px > 0.0)) continue;   // the old rule never saw these

            const bool old_bid_suppress = px > m;
            const bool old_ask_suppress = px < m;

            EXPECT_EQ(classify_cross_published_mid(kBid, px, m)
                          == CrossVerdict::Crossed, old_bid_suppress)
                << "bid mid=" << m << " px=" << px;
            EXPECT_EQ(classify_cross_published_mid(kAsk, px, m)
                          == CrossVerdict::Crossed, old_ask_suppress)
                << "ask mid=" << m << " px=" << px;
        }
    }
}

TEST(CrossGuard, TheShadowVerdictNeverInfluencesSuppression)
{
    // Structural restatement of the same guarantee: the BBO predicate is a
    // pure function of its own arguments and shares no state with the live
    // one, so no BBO value can change what classify_cross_published_mid
    // returns. Demonstrated by holding the tier and mid fixed while moving
    // the book underneath, across an inverted book and a missing one.
    const double px = 100.5;
    const auto live = classify_cross_published_mid(kBid, px, kMid);
    EXPECT_EQ(live, CrossVerdict::Crossed);

    for (const auto& bk : {std::pair<double, double>{99.0, 101.0},
                           std::pair<double, double>{101.0, 99.0},
                           std::pair<double, double>{0.0, 0.0},
                           std::pair<double, double>{50.0, 500.0}}) {
        const auto shadow = classify_cross_bbo(kBid, px, bk.first, bk.second,
                                               kMid);
        (void)shadow;
        EXPECT_EQ(classify_cross_published_mid(kBid, px, kMid), live)
            << "the live verdict must not move with the book";
    }
}
