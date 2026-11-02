// ---------------------------------------------------------------------------
// [S33 review] The pruner's wallet-wide delete cannot be aimed.
//
// delete_unconfirmed_transactions(wallet_id) removes EVERY unconfirmed row on
// the wallet. prune_stuck_transactions counted only rows past their age
// threshold, so one stale row authorised destroying this heartbeat's offer
// creations and any unconfirmed secure cancel beside it.
//
// This was dead code until the reverse=false window fix in this same PR made
// the scan able to see unconfirmed rows at all, which is why the gate ships
// here rather than as separate hardening.
//
// SCOPE, stated honestly: these drive the DECISION. That
// prune_stuck_transactions actually calls it, and maps JSON rows onto the two
// counters correctly, is NOT covered -- nothing in cpp/tests constructs an
// OfferManager. Reverting the call site while keeping this header leaves the
// suite green. That is the same tracked gap 8813e91 and test_coin_lock_ledger
// already record, not something this change closes.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>

#include "xop/execution/stuck_tx_verdict.hpp"

using xop::execution::authorises_wallet_wide_delete;
using xop::execution::classify_stuck_row;
using xop::execution::row_outlived_force_delete_window;
using xop::execution::StuckRowClass;

namespace {

TEST(StuckTxVerdict, OneOldRowAloneAuthorisesTheDelete)
{
    EXPECT_TRUE(authorises_wallet_wide_delete(1, 0));
    EXPECT_TRUE(authorises_wallet_wide_delete(37, 0));
}

// THE BUG. One stale row used to authorise deleting everything unconfirmed,
// including a secure cancel broadcast seconds ago.
TEST(StuckTxVerdict, AFreshRowVetoesTheWalletWideDelete)
{
    EXPECT_FALSE(authorises_wallet_wide_delete(1, 1));
}

// The counts are not a vote: the delete takes the fresh row whatever the
// ratio, so any fresh row vetoes it.
TEST(StuckTxVerdict, ManyOldRowsStillLoseToOneFreshRow)
{
    EXPECT_FALSE(authorises_wallet_wide_delete(37, 1));
}

TEST(StuckTxVerdict, NothingOldAuthorisesNothing)
{
    EXPECT_FALSE(authorises_wallet_wide_delete(0, 0));
    EXPECT_FALSE(authorises_wallet_wide_delete(0, 9));
}

// ---------------------------------------------------------------------------
// classify_stuck_row -- the per-row rule the prune loop now delegates to.
//
// [review 2026-09-13] Previously this lived inline in
// prune_stuck_transactions, where no test could reach it, and the verdict
// above was the only covered part. The loop now CALLS this, so these tests
// drive production code rather than a parallel copy.
//
// MUTATION CHECK: make an unreadable age count as PastThreshold instead of
// FreshOrUnknown -> AnUnreadableAgeIsFreshNotOld goes RED. Drop the 3x bundle
// multiplier -> ABroadcastRowGetsThreeTimesTheThreshold goes RED.
// ---------------------------------------------------------------------------

TEST(StuckRowClassification, AConfirmedRowIsNotThePrunersBusiness)
{
    EXPECT_EQ(classify_stuck_row(/*confirmed=*/true, /*age_known=*/true,
                                 /*age_seconds=*/99999, /*has_spend_bundle=*/false,
                                 /*max_age_seconds=*/600),
              StuckRowClass::Confirmed);
}

// THE RULE that keeps the wallet-wide delete from being authorised by a row
// nobody can date.
TEST(StuckRowClassification, AnUnreadableAgeIsFreshNotOld)
{
    EXPECT_EQ(classify_stuck_row(/*confirmed=*/false, /*age_known=*/false,
                                 /*age_seconds=*/0, /*has_spend_bundle=*/false,
                                 /*max_age_seconds=*/600),
              StuckRowClass::FreshOrUnknown)
        << "an age that cannot be read must never help authorise a "
           "wallet-wide delete";
}

TEST(StuckRowClassification, AYoungRowIsFresh)
{
    EXPECT_EQ(classify_stuck_row(false, true, 599, false, 600),
              StuckRowClass::FreshOrUnknown);
}

TEST(StuckRowClassification, AnOldUnbroadcastRowIsPastThreshold)
{
    EXPECT_EQ(classify_stuck_row(false, true, 600, false, 600),
              StuckRowClass::PastThreshold)
        << "the boundary is inclusive: at exactly max_age it is stuck";
}

// The branch reverse=false made reachable for the first time.
TEST(StuckRowClassification, ABroadcastRowGetsThreeTimesTheThreshold)
{
    EXPECT_EQ(classify_stuck_row(false, true, 1799, /*has_spend_bundle=*/true, 600),
              StuckRowClass::FreshOrUnknown)
        << "a broadcast row is not hopeless until it outlives 3x the window";
    EXPECT_EQ(classify_stuck_row(false, true, 1800, /*has_spend_bundle=*/true, 600),
              StuckRowClass::PastThreshold);
}

// The two classifications compose into the verdict the same way the loop does.
TEST(StuckRowClassification, OneUndatableRowVetoesADeleteTheRestWouldAuthorise)
{
    int past = 0, fresh = 0;
    const StuckRowClass rows[] = {
        classify_stuck_row(false, true, 5000, false, 600),   // stuck
        classify_stuck_row(false, true, 5000, false, 600),   // stuck
        classify_stuck_row(false, false, 0, false, 600),     // undatable
    };
    for (const auto c : rows) {
        if (c == StuckRowClass::FreshOrUnknown) ++fresh;
        else if (c == StuckRowClass::PastThreshold) ++past;
    }
    EXPECT_EQ(past, 2);
    EXPECT_EQ(fresh, 1);
    EXPECT_FALSE(authorises_wallet_wide_delete(past, fresh))
        << "two genuinely stuck rows still lose to one row nobody can date";
}

// Catches the call site's one realistic failure: transposed arguments.
TEST(StuckTxVerdict, TheTwoCountsAreNotInterchangeable)
{
    EXPECT_NE(authorises_wallet_wide_delete(1, 0),
              authorises_wallet_wide_delete(0, 1));
}

// [review #214 r3] The force-delete's own row rule.
TEST(ForceDeleteWindow, OnlyARowUnconfirmedForTheWholeWindowArmsTheWipe)
{
    EXPECT_FALSE(row_outlived_force_delete_window(false, true, 599, 600))
        << "a cancel sent under a stale streak is seconds old: never wiped";
    EXPECT_TRUE(row_outlived_force_delete_window(false, true, 600, 600))
        << "the boundary is inclusive, like classify_stuck_row's";
    EXPECT_FALSE(row_outlived_force_delete_window(true, true, 5000, 600))
        << "a confirmed row is history, not a stuck spend";
    EXPECT_FALSE(row_outlived_force_delete_window(false, false, 5000, 600))
        << "an undatable row is not old: unknown authorises nothing";
}

// The escalation is the pruner's backstop: it is NOT gated like the prune.
TEST(ForceDeleteWindow, FreshRowsBesideAStuckOneDoNotHoldTheEscalation)
{
    // One stuck row and one fresh one: the pruner holds...
    int past = 0, fresh = 0;
    for (const auto c : {classify_stuck_row(false, true, 5000, false, 600),
                         classify_stuck_row(false, true, 10, false, 600)}) {
        if (c == StuckRowClass::FreshOrUnknown) ++fresh;
        else if (c == StuckRowClass::PastThreshold) ++past;
    }
    EXPECT_FALSE(authorises_wallet_wide_delete(past, fresh));
    // ...and the escalation still has its old row.
    int outlived = 0;
    for (const std::int64_t age : {5000, 10}) {
        if (row_outlived_force_delete_window(false, true, age, 600)) ++outlived;
    }
    EXPECT_EQ(outlived, 1);
    // A broadcast row gets no 3x here: 600 s is enough.
    EXPECT_EQ(classify_stuck_row(false, true, 600, /*has_spend_bundle=*/true, 600),
              StuckRowClass::FreshOrUnknown);
    EXPECT_TRUE(row_outlived_force_delete_window(false, true, 600, 600));
}

}  // namespace
