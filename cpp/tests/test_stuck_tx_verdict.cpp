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

#include "xop/execution/stuck_tx_verdict.hpp"

using xop::execution::authorises_wallet_wide_delete;

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

// Catches the call site's one realistic failure: transposed arguments.
TEST(StuckTxVerdict, TheTwoCountsAreNotInterchangeable)
{
    EXPECT_NE(authorises_wallet_wide_delete(1, 0),
              authorises_wallet_wide_delete(0, 1));
}

}  // namespace
