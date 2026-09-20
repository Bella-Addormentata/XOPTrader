// ---------------------------------------------------------------------------
// [CROSSGUARD] The pre-post crossing predicate, both versions.
//
// Step 8 suppresses on the BBO verdict (classify_cross_bbo), matching the
// canceller (classify_tier_staleness). Its own fallback for a missing BBO
// is the +/-5% mid band. The legacy published-mid rule
// (classify_cross_published_mid) has no production caller and is kept only
// as the fixed reference these tests measure the S33 change against.
//
// These tests pin BOTH rules and verify their behavior across normal,
// crossed, inverted, and degenerate order books.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <utility>

#include "xop/execution/cross_guard.hpp"

using xop::execution::classify_cross_bbo;
using xop::execution::classify_cross_published_mid;
using xop::execution::classify_tier_refresh;
using xop::execution::classify_tier_refresh_margin;
using xop::execution::margin_breach_reason;
using xop::execution::margin_reference_usable;
using xop::execution::MarginRefresh;
using xop::execution::resting_edge_bps;
using xop::execution::CrossVerdict;
using xop::execution::TierRefresh;

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

TEST(CrossGuard, OneSidedBooksFallBackOnlyWhenTheRelevantTouchIsMissing)
{
    // [S33 2026-09-12] A bid is judged against best_ask and an ask against
    // best_bid, so the touch missing in each of these is precisely the one
    // that mattered, and the mid band is all that is left to judge with.
    EXPECT_TRUE(classify_cross_bbo(kBid, 100.0, kBestBid, 0.0, kMid)
                    .used_mid_fallback) << "a bid with no best_ask";
    EXPECT_TRUE(classify_cross_bbo(kAsk, 100.0, 0.0, kBestAsk, kMid)
                    .used_mid_fallback) << "an ask with no best_bid";
}

TEST(CrossGuard, AOneSidedBookStillCatchesACrossAgainstTheTouchThatExists)
{
    // [S33 2026-09-12] THE FINDING, in the reviewer's exact numbers. An ask
    // at 99 with a standing bid of 100 and nothing offered is liftable the
    // instant it is posted. The old rule demanded BOTH touches, so it fell
    // through to the +/-5% band, asked 99 < 100*0.95 = 95, answered no, and
    // posted the crossed ask. This is the LIVE suppression gate as of
    // PR #148, and one-sided books are the condition PR #148 exists for.
    const auto ask = classify_cross_bbo(kAsk, 99.0, /*best_bid=*/100.0,
                                        /*best_ask=*/0.0, /*mid=*/100.0);
    EXPECT_EQ(ask.verdict, CrossVerdict::Crossed);
    EXPECT_FALSE(ask.used_mid_fallback)
        << "best_bid is present, so the band is not the reference here";
    EXPECT_FALSE(ask.book_inverted)
        << "inversion is a claim about two touches; there is only one";

    // The mirror: a bid at 101 lifting a lone offer at 100.
    const auto bid = classify_cross_bbo(kBid, 101.0, /*best_bid=*/0.0,
                                        /*best_ask=*/100.0, /*mid=*/100.0);
    EXPECT_EQ(bid.verdict, CrossVerdict::Crossed);
    EXPECT_FALSE(bid.used_mid_fallback);
}

TEST(CrossGuard, AOneSidedBookStillPassesAQuoteThatDoesNotCrossIt)
{
    // The fix must not collapse into "one side missing -> suppress
    // everything": an ask above the lone bid is a perfectly good quote.
    // The canceller's NON-STRICT inequality has to survive the change too.
    EXPECT_EQ(classify_cross_bbo(kAsk, 101.0, 100.0, 0.0, kMid).verdict,
              CrossVerdict::Ok);
    EXPECT_EQ(classify_cross_bbo(kAsk, 100.0, 100.0, 0.0, kMid).verdict,
              CrossVerdict::Crossed) << "at the touch is crossed, not Ok";
    EXPECT_EQ(classify_cross_bbo(kBid, 99.0, 0.0, 100.0, kMid).verdict,
              CrossVerdict::Ok);
    EXPECT_EQ(classify_cross_bbo(kBid, 100.0, 0.0, 100.0, kMid).verdict,
              CrossVerdict::Crossed);
}

TEST(CrossGuard, TheIrrelevantTouchNeitherDecidesNorDisablesTheDecision)
{
    // Whatever the SAME-side touch does -- absent, present, inverted or
    // junk -- it must neither move the verdict nor push us to the fallback.
    for (const double other : {0.0, 101.0, 98.0, kNaN, kInf}) {
        const auto r = classify_cross_bbo(kAsk, 99.0, /*best_bid=*/100.0,
                                          other, kMid);
        EXPECT_EQ(r.verdict, CrossVerdict::Crossed) << "best_ask=" << other;
        EXPECT_FALSE(r.used_mid_fallback) << "best_ask=" << other;
    }
    for (const double other : {0.0, 99.0, 102.0, kNaN, kInf}) {
        const auto r = classify_cross_bbo(kBid, 101.0, other,
                                          /*best_ask=*/100.0, kMid);
        EXPECT_EQ(r.verdict, CrossVerdict::Crossed) << "best_bid=" << other;
        EXPECT_FALSE(r.used_mid_fallback) << "best_bid=" << other;
    }
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

TEST(CrossGuard, ANonFiniteRelevantTouchFallsBackRatherThanTrusting)
{
    // [S33 2026-09-12] The touch that has to be finite is the one being
    // judged against: best_ask for a bid, best_bid for an ask.
    EXPECT_TRUE(classify_cross_bbo(kBid, 100.0, kBestBid, kNaN, kMid)
                    .used_mid_fallback)
        << "a non-finite touch is not a touch; use the documented fallback";
    EXPECT_TRUE(classify_cross_bbo(kAsk, 100.0, kNaN, kBestAsk, kMid)
                    .used_mid_fallback);
    EXPECT_TRUE(classify_cross_bbo(kBid, 100.0, kBestBid, kInf, kMid)
                    .used_mid_fallback) << "infinite likewise";
}

// -- The legacy rule, pinned as a fixed reference ---------------------------

TEST(CrossGuard, SuppressionIsByteIdenticalToThePreShadowRule)
{
    // [S33 2026-09-05] Step 8 no longer calls this predicate -- it suppresses
    // on classify_cross_bbo. What is pinned here is the LEGACY rule Step 8
    // inlined from 4d3f30d until S33:
    //     bid: tier.price > mid  -> suppress
    //     ask: tier.price < mid  -> suppress
    // classify_cross_published_mid must stay byte-identical to it, because
    // the disagreement tests above measure the S33 change against it. If it
    // drifts, those tests silently start measuring something else.
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

TEST(CrossGuard, TheLiveVerdictMovesWithTheBookAndTheLegacyOneDoesNot)
{
    // [S33 2026-09-05] The old contract here was "the BBO verdict never
    // influences suppression". S33 inverted it: the BBO verdict is now the
    // ONLY thing Step 8 suppresses on, and the published-mid rule is the
    // fixed reference the disagreement tests are measured against. So the
    // property worth pinning is that the live rule reacts to the book while
    // the legacy one, which cannot see it, does not.
    const double px = 100.5;
    const auto legacy = classify_cross_published_mid(kBid, px, kMid);
    EXPECT_EQ(legacy, CrossVerdict::Crossed);

    // Same tier, same mid, two different books -- the live rule disagrees
    // with itself, which is exactly what makes it the decision.
    EXPECT_EQ(classify_cross_bbo(kBid, px, 101.0, 99.0, kMid).verdict,
              CrossVerdict::Crossed) << "inverted book: 100.5 >= best_ask 99";
    EXPECT_EQ(classify_cross_bbo(kBid, px, kBestBid, kBestAsk, kMid).verdict,
              CrossVerdict::Ok) << "inside the spread is a valid bid now";

    for (const auto& bk : {std::pair<double, double>{99.0, 101.0},
                           std::pair<double, double>{101.0, 99.0},
                           std::pair<double, double>{0.0, 0.0},
                           std::pair<double, double>{50.0, 500.0}}) {
        const auto live = classify_cross_bbo(kBid, px, bk.first, bk.second,
                                             kMid);
        (void)live;
        EXPECT_EQ(classify_cross_published_mid(kBid, px, kMid), legacy)
            << "the legacy reference must not move with the book";
    }
}

// -- The canceller's refresh zones ------------------------------------------
//
// [S33 2026-09-12] classify_tier_refresh was lifted out of
// OfferManager::classify_tier_staleness, which no test could reach: the
// class will not construct without an io_context, a wallet RPC client, a
// Dexie client and the shared State. Nothing covered the zone ORDER, the
// tier threshold, or the 3x favorable multiplier -- any of which could be
// changed without a single test failing.

namespace {
// kSelectiveRefreshThreshold at tier 0, and kSoftTtlAdverseThreshold.
constexpr double kTierThreshold = 0.010;
constexpr double kSoftTtl       = 0.02;

struct Drift {
    double deviation;
    bool   adverse;
};

/// Mirrors classify_tier_staleness's own deviation arithmetic so the cases
/// below read as "a live bid of 100 whose optimal moved to 97" rather than
/// as pre-digested booleans -- the sign convention (a bid is adverse when
/// the optimal FALLS, an ask when it RISES) is half of the contract.
[[nodiscard]] Drift drift_of(bool is_ask, double live_px, double optimal_px)
{
    const double signed_dev = (optimal_px - live_px) / live_px;
    return Drift{std::abs(signed_dev),
                 is_ask ? (signed_dev > 0.0) : (signed_dev < 0.0)};
}

[[nodiscard]] TierRefresh refresh_of(bool is_ask, double live_px,
                                     double optimal_px, bool below_min_age)
{
    const Drift d = drift_of(is_ask, live_px, optimal_px);
    return classify_tier_refresh(/*crossed=*/false, /*past_soft_ttl=*/false,
                                 below_min_age, d.adverse, d.deviation,
                                 kTierThreshold, kSoftTtl);
}
}  // namespace

TEST(TierRefresh, AdverseDriftRefreshesPastTheTierThresholdOnBothSides)
{
    // Adverse = the market moved against the live quote: the new optimal
    // bid is BELOW ours (we are overpaying), the new optimal ask is ABOVE
    // ours (we are underselling). Tier 0 tolerates 1%.
    EXPECT_EQ(refresh_of(kBid, 100.0, 98.5, false), TierRefresh::Stale)
        << "bid 1.5% too high";
    EXPECT_EQ(refresh_of(kBid, 100.0, 99.5, false), TierRefresh::Fresh)
        << "bid 0.5% too high -- inside the threshold";
    EXPECT_EQ(refresh_of(kAsk, 100.0, 101.5, false), TierRefresh::Stale)
        << "ask 1.5% too low";
    EXPECT_EQ(refresh_of(kAsk, 100.0, 100.5, false), TierRefresh::Fresh)
        << "ask 0.5% too low -- inside the threshold";

    // The inequality is strict. Pinned exactly rather than with a nearby
    // decimal that rounding could drop on either side of the boundary.
    EXPECT_EQ(classify_tier_refresh(false, false, false, /*adverse=*/true,
                                    kTierThreshold, kTierThreshold, kSoftTtl),
              TierRefresh::Fresh) << "exactly at the threshold is not past it";
    EXPECT_EQ(classify_tier_refresh(false, false, false, true,
                                    std::nextafter(kTierThreshold, 1.0),
                                    kTierThreshold, kSoftTtl),
              TierRefresh::Stale) << "one ulp past it is";
}

TEST(TierRefresh, FavorableDriftIsToleratedThreeTimesFurther)
{
    // THE BRANCH 922b183 ADDED IN THIS PR. Favorable = the quote drifted in OUR favour (the
    // optimal bid rose above our live bid; the optimal ask fell below our
    // live ask). It is still earning, so it is refreshed only past 3x the
    // threshold -- 3% at tier 0 -- instead of at 1%.
    EXPECT_EQ(refresh_of(kBid, 100.0, 102.0, false), TierRefresh::Fresh)
        << "bid 2% conservative: past 1x but not 3x -- keep it";
    EXPECT_EQ(refresh_of(kBid, 100.0, 104.0, false), TierRefresh::Stale)
        << "bid 4% conservative: disconnected, refresh it";
    EXPECT_EQ(refresh_of(kAsk, 100.0, 98.0, false), TierRefresh::Fresh);
    EXPECT_EQ(refresh_of(kAsk, 100.0, 96.0, false), TierRefresh::Stale);

    // The multiplier itself, to the ulp on both sides: 2x or 4x in place of
    // 3x fails here, and so does relaxing '>' to '>='.
    EXPECT_EQ(classify_tier_refresh(false, false, false, /*adverse=*/false,
                                    kTierThreshold * 3.0,
                                    kTierThreshold, kSoftTtl),
              TierRefresh::Fresh) << "exactly 3x is not past 3x";
    EXPECT_EQ(classify_tier_refresh(false, false, false, false,
                                    std::nextafter(kTierThreshold * 3.0, 1.0),
                                    kTierThreshold, kSoftTtl),
              TierRefresh::Stale) << "one ulp past 3x is";
    // ... and the widening is real: the same drift, adverse, refreshes.
    EXPECT_EQ(classify_tier_refresh(false, false, false, /*adverse=*/true,
                                    kTierThreshold * 2.0,
                                    kTierThreshold, kSoftTtl),
              TierRefresh::Stale);
}

TEST(TierRefresh, TheMinimumAgeGuardOutranksDriftButNotACross)
{
    // A young offer is protected from churn: the cancel+recreate round trip
    // costs more than the adverse selection it would avoid. Both sides,
    // both directions.
    EXPECT_EQ(refresh_of(kBid, 100.0, 90.0, /*below_min_age=*/true),
              TierRefresh::Fresh) << "10% adverse, but too young to touch";
    EXPECT_EQ(refresh_of(kAsk, 100.0, 110.0, true), TierRefresh::Fresh);
    EXPECT_EQ(refresh_of(kAsk, 100.0, 50.0, true), TierRefresh::Fresh)
        << "50% favorable, still too young";

    // Past the guard the very same drifts refresh -- without this half the
    // test above would also pass against a function that never says Stale.
    EXPECT_EQ(refresh_of(kBid, 100.0, 90.0, false), TierRefresh::Stale);
    EXPECT_EQ(refresh_of(kAsk, 100.0, 110.0, false), TierRefresh::Stale);
    EXPECT_EQ(refresh_of(kAsk, 100.0, 50.0, false), TierRefresh::Stale);

    // A cross bypasses the guard. That ordering is why `crossed` is tested
    // first, and it is what ties this predicate to classify_cross_bbo --
    // the two halves of one decision, which is why they share this header.
    EXPECT_EQ(classify_tier_refresh(/*crossed=*/true, /*past_soft_ttl=*/false,
                                    /*below_min_age=*/true, /*adverse=*/false,
                                    /*price_deviation=*/0.0,
                                    kTierThreshold, kSoftTtl),
              TierRefresh::Stale);
}

TEST(TierRefresh, PastTheSoftTtlOnlyAdverseDriftExpiresAndFavorableRestsToHardTtl)
{
    // An aged offer that drifted AGAINST us ends -- and as Expired, which is
    // a different cancel reason from Stale.
    EXPECT_EQ(classify_tier_refresh(false, /*past_soft_ttl=*/true, false,
                                    /*adverse=*/true, 0.05,
                                    kTierThreshold, kSoftTtl),
              TierRefresh::Expired);
    // A still well-priced old offer is kept rather than churned.
    EXPECT_EQ(classify_tier_refresh(false, true, false, true, 0.01,
                                    kTierThreshold, kSoftTtl),
              TierRefresh::Fresh);
    // Direction keeps mattering past the soft TTL. A quote that drifted in
    // OUR favour is more conservative than the one we would post now, so it
    // rests to the hard TTL instead of paying cancel+repost to be replaced
    // by a worse price. This is the contract kHardTtlMultiplier documents
    // (offer_manager.hpp:821-826).
    EXPECT_EQ(classify_tier_refresh(false, true, false, /*adverse=*/false,
                                    0.05, kTierThreshold, kSoftTtl),
              TierRefresh::Fresh)
        << "5% in our favour on an aged offer: still earning, keep it";
    // ... and the soft-TTL zone outranks the normal zone's 3x favorable
    // rule, so a favorable drift that would be Stale at age 12 is neither
    // Stale nor Expired here: past the soft TTL, favorable means Fresh.
    EXPECT_EQ(classify_tier_refresh(false, true, false, false,
                                    kTierThreshold * 4.0,
                                    kTierThreshold, kSoftTtl),
              TierRefresh::Fresh);
    // The adverse threshold, to the ulp on both sides.
    EXPECT_EQ(classify_tier_refresh(false, true, false, /*adverse=*/true,
                                    kSoftTtl, kTierThreshold, kSoftTtl),
              TierRefresh::Fresh) << "exactly at the threshold is not past it";
    EXPECT_EQ(classify_tier_refresh(false, true, false, true,
                                    std::nextafter(kSoftTtl, 1.0),
                                    kTierThreshold, kSoftTtl),
              TierRefresh::Expired) << "one ulp past the soft-TTL threshold";
    // The soft-TTL zone outranks the minimum-age guard below it.
    EXPECT_EQ(classify_tier_refresh(false, true, /*below_min_age=*/true,
                                    true, 0.05, kTierThreshold, kSoftTtl),
              TierRefresh::Expired);
}

// -- [S72 2026-09-20] The margin rule ----------------------------------------
//
// price_cancel_mode: margin replaces every deviation zone above with one
// question about the OFFER rather than the ladder: would a fill at its
// resting price still earn edge_retain x the edge Step 7 demands of a new
// offer, against Step 7's own centre?  Pinned here: the arithmetic and its
// sign convention, the zone order, the boundary, that a favourable drift can
// never cancel, that a missing reference falls back instead of keeping, and
// the two things that make the rule cheaper than the one it replaces.

namespace {
// XCH/DBX on 2026-09-14 14:21: centre 85.97 DBX, floor 123 bps (the sigma
// term bound), so Step 7's posting edges were 84.9160 / 87.0310.
constexpr double kCentre   = 85.9735;
constexpr double kFloorBps = 123.0;
constexpr double kRetain   = 0.5;

[[nodiscard]] double px_at_edge(bool is_ask, double edge_bps)
{
    return is_ask ? kCentre * (1.0 + edge_bps / 10'000.0)
                  : kCentre * (1.0 - edge_bps / 10'000.0);
}

[[nodiscard]] MarginRefresh margin_of(bool is_ask, double price,
                                      double centre = kCentre,
                                      double retain = kRetain)
{
    return classify_tier_refresh_margin(/*crossed=*/false,
                                        /*below_min_age=*/false, is_ask, price,
                                        centre, kFloorBps, retain);
}
}  // namespace

TEST(MarginRefresh, EdgeIsPositiveOnTheProfitableSideOfTheCentre)
{
    // An ask earns by resting ABOVE the centre, a bid by resting BELOW it.
    EXPECT_NEAR(resting_edge_bps(kAsk, 101.0, 100.0), 100.0, 1e-9);
    EXPECT_NEAR(resting_edge_bps(kBid, 99.0, 100.0), 100.0, 1e-9);
    // The wrong side of the centre is a NEGATIVE edge, not a large one.
    EXPECT_NEAR(resting_edge_bps(kAsk, 99.0, 100.0), -100.0, 1e-9);
    EXPECT_NEAR(resting_edge_bps(kBid, 101.0, 100.0), -100.0, 1e-9);
    // Measured in bps of the CENTRE, which is what Step 7's floor is in.
    EXPECT_NEAR(resting_edge_bps(kAsk, 210.0, 200.0), 500.0, 1e-9);
}

TEST(MarginRefresh, UnusableInputsEarnNothing)
{
    for (const double bad : {0.0, -1.0, kInf, kNaN}) {
        EXPECT_EQ(resting_edge_bps(kAsk, bad, 100.0), 0.0) << bad;
        EXPECT_EQ(resting_edge_bps(kAsk, 100.0, bad), 0.0) << bad;
    }
}

TEST(MarginRefresh, AnOfferAtThePostingFloorIsKept)
{
    // THE case the retain fraction exists for.  Step 7's width-floor pass
    // pushes tiers out to EXACTLY centre x (1 +/- floor), so this is where
    // offers are born.
    EXPECT_EQ(margin_of(kAsk, px_at_edge(kAsk, kFloorBps)), MarginRefresh::Fresh);
    EXPECT_EQ(margin_of(kBid, px_at_edge(kBid, kFloorBps)), MarginRefresh::Fresh);
}

TEST(MarginRefresh, TheCentreMovingHalfTheFloorAgainstItIsWhatCancels)
{
    // Born at the floor (123 bps), retain 0.5: the resting floor is 61.5 bps.
    const double ask_px = px_at_edge(kAsk, kFloorBps);
    const double bid_px = px_at_edge(kBid, kFloorBps);
    // Centre up 50 bps: the ask keeps ~73 bps, the bid GAINS edge.
    const double up50 = kCentre * 1.0050;
    EXPECT_EQ(margin_of(kAsk, ask_px, up50), MarginRefresh::Fresh);
    EXPECT_EQ(margin_of(kBid, bid_px, up50), MarginRefresh::Fresh);
    // Centre up 80 bps: the ask keeps ~43 bps -- under 61.5.  Stale.
    const double up80 = kCentre * 1.0080;
    EXPECT_EQ(margin_of(kAsk, ask_px, up80), MarginRefresh::Stale);
    EXPECT_EQ(margin_of(kBid, bid_px, up80), MarginRefresh::Fresh);
    // And symmetrically for a falling centre.
    const double down80 = kCentre * 0.9920;
    EXPECT_EQ(margin_of(kBid, bid_px, down80), MarginRefresh::Stale);
    EXPECT_EQ(margin_of(kAsk, ask_px, down80), MarginRefresh::Fresh);
}

TEST(MarginRefresh, RetainOneIsTheLiteralRuleAndFlapsAtTheFloor)
{
    // At retain 1.0 the cancel threshold IS the posting floor, so a ONE bp
    // adverse tick refreshes an offer born there.  This is why the default
    // is not 1.0; pinned so the comment in cross_guard.hpp cannot rot.
    const double ask_px = px_at_edge(kAsk, kFloorBps);
    EXPECT_EQ(margin_of(kAsk, ask_px, kCentre * 1.0001, /*retain=*/1.0),
              MarginRefresh::Stale);
    EXPECT_EQ(margin_of(kAsk, ask_px, kCentre * 1.0001, /*retain=*/0.5),
              MarginRefresh::Fresh);
}

TEST(MarginRefresh, TheBoundaryIsStrict)
{
    // Exactly on the resting floor is still acceptable, as exactly on the
    // posting floor is to Step 7.  Every number here is exact in binary:
    // centre 128, price 129 or 127 -> edge 1/128 = 78.125 bps, and the
    // resting floor is 156.25 x 0.5 = 78.125 bps.
    EXPECT_EQ(resting_edge_bps(kAsk, 129.0, 128.0), 78.125);
    EXPECT_EQ(resting_edge_bps(kBid, 127.0, 128.0), 78.125);
    EXPECT_EQ(classify_tier_refresh_margin(false, false, kAsk, 129.0, 128.0,
                                           156.25, 0.5),
              MarginRefresh::Fresh);
    EXPECT_EQ(classify_tier_refresh_margin(
                  false, false, kAsk, std::nextafter(129.0, 0.0), 128.0,
                  156.25, 0.5),
              MarginRefresh::Stale);
    EXPECT_EQ(classify_tier_refresh_margin(false, false, kBid, 127.0, 128.0,
                                           156.25, 0.5),
              MarginRefresh::Fresh);
    EXPECT_EQ(classify_tier_refresh_margin(
                  false, false, kBid, std::nextafter(127.0, 200.0), 128.0,
                  156.25, 0.5),
              MarginRefresh::Stale);
}

TEST(MarginRefresh, FavourableDriftNeverCancelsHoweverLarge)
{
    // The deviation rule refreshes a favourable drift past 3x the tier
    // threshold, and the anchor override past 1x.  Direction is not even an
    // input here: moving away from the centre only ADDS edge.
    for (const double far_bps : {500.0, 2'000.0, 9'000.0}) {
        EXPECT_EQ(margin_of(kAsk, px_at_edge(kAsk, far_bps)),
                  MarginRefresh::Fresh) << far_bps;
        EXPECT_EQ(margin_of(kBid, px_at_edge(kBid, far_bps)),
                  MarginRefresh::Fresh) << far_bps;
    }
}

TEST(MarginRefresh, CrossedOutranksEverything)
{
    // Ahead of the minimum-age guard, ahead of a healthy edge, and ahead of
    // a missing reference -- a cross needs no centre to be a cross.
    EXPECT_EQ(classify_tier_refresh_margin(/*crossed=*/true,
                                           /*below_min_age=*/true, kAsk,
                                           px_at_edge(kAsk, 900.0), kCentre,
                                           kFloorBps, kRetain),
              MarginRefresh::Stale);
    EXPECT_EQ(classify_tier_refresh_margin(true, false, kAsk, 87.0, 0.0, 0.0,
                                           kRetain),
              MarginRefresh::Stale);
}

TEST(MarginRefresh, TheMinimumAgeGuardOutranksTheEdgeTest)
{
    // An offer with NEGATIVE edge, too young to refresh: kept, exactly as
    // the deviation rule keeps a young offer however far it has drifted.
    const double underwater = px_at_edge(kAsk, -50.0);
    EXPECT_EQ(classify_tier_refresh_margin(false, /*below_min_age=*/true, kAsk,
                                           underwater, kCentre, kFloorBps,
                                           kRetain),
              MarginRefresh::Fresh);
    EXPECT_EQ(classify_tier_refresh_margin(false, /*below_min_age=*/false, kAsk,
                                           underwater, kCentre, kFloorBps,
                                           kRetain),
              MarginRefresh::Stale);
}

TEST(MarginRefresh, NoReferenceFallsBackRatherThanKeeping)
{
    // Step 7 leaves centre and floor at 0 until it reaches ladder generation,
    // and the pace pass sends none.  "No reference" must NOT read as Fresh:
    // the caller answers it with the deviation zones, i.e. today's rule.
    const double px = px_at_edge(kAsk, 10.0);   // would be Stale with one
    for (const double bad : {0.0, -1.0, kInf, kNaN}) {
        EXPECT_EQ(classify_tier_refresh_margin(false, false, kAsk, px, bad,
                                               kFloorBps, kRetain),
                  MarginRefresh::NoReference) << "centre " << bad;
        EXPECT_EQ(classify_tier_refresh_margin(false, false, kAsk, px, kCentre,
                                               bad, kRetain),
                  MarginRefresh::NoReference) << "floor " << bad;
        EXPECT_EQ(classify_tier_refresh_margin(false, false, kAsk, bad, kCentre,
                                               kFloorBps, kRetain),
                  MarginRefresh::NoReference) << "price " << bad;
    }
    // retain outside (0, 1] is a config the parser refuses; were one to get
    // here it must not quietly become "never cancel" or "always cancel".
    for (const double bad : {0.0, -0.5, 1.0001, kInf, kNaN}) {
        EXPECT_EQ(classify_tier_refresh_margin(false, false, kAsk, px, kCentre,
                                               kFloorBps, bad),
                  MarginRefresh::NoReference) << "retain " << bad;
        EXPECT_FALSE(margin_reference_usable(kCentre, kFloorBps, bad)) << bad;
    }
    EXPECT_TRUE(margin_reference_usable(kCentre, kFloorBps, 1.0));
    // And the young-offer guard does not mask a missing reference: the
    // deviation zones have their own copy of it.
    EXPECT_EQ(classify_tier_refresh_margin(false, /*below_min_age=*/true, kAsk,
                                           px, 0.0, kFloorBps, kRetain),
              MarginRefresh::NoReference);
}

TEST(MarginRefresh, KeepsWhatTheDeviationRuleCancelledForLadderDrift)
{
    // The recorded shape: an XCH/DBX ask resting well outside the floor while
    // its TIER's new optimal price moves 1.5% further out.  The deviation
    // rule calls that adverse drift past the 1% tier-0 threshold and cancels;
    // the fill it is protecting against would still earn 3x the floor.
    const double resting = px_at_edge(kAsk, 370.0);
    const double new_optimal = resting * 1.015;
    EXPECT_EQ(refresh_of(kAsk, resting, new_optimal, false), TierRefresh::Stale);
    EXPECT_EQ(margin_of(kAsk, resting), MarginRefresh::Fresh);
}

TEST(MarginRefresh, TheReasonStringCarriesBothNumbers)
{
    EXPECT_EQ(margin_breach_reason(41.72, 61.5), "margin_breach(41.7/61.5bps)");
    EXPECT_EQ(margin_breach_reason(-12.0, 61.5), "margin_breach(-12.0/61.5bps)");
    // The prefix is what reports group on; a non-finite input must not
    // produce "nan" or "inf" in a column people filter.
    EXPECT_EQ(margin_breach_reason(kNaN, kInf), "margin_breach(0.0/0.0bps)");
}
