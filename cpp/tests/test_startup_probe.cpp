// ---------------------------------------------------------------------------
// [F2 2026-09-03] The DB->wallet leg's bucketing rule.
//
// WHAT WAS WRONG
// --------------
// startup_reconcile's DB->wallet leg probes every DB-pending row the wallet's
// own scan never mentioned, using get_offer(). It bucketed the answer with an
// if/else chain that ended in a bare `else`:
//
//     } else {
//         // kPendingConfirm, kPendingCancel, or an unrecognised code.
//         db_leg_.unverifiable.push_back(id);
//     }
//
// PENDING_CANCEL went into `unverifiable`. But `unverifiable` means, in that
// file's own words, "we learned NOTHING" -- and PENDING_CANCEL is the
// opposite: it is positive evidence that a cancel spend is ALREADY IN FLIGHT
// against that offer's coins.
//
// THE ROUND TRIP THAT COSTS MONEY
// -------------------------------
//   1. the leg buckets the row as unverifiable;
//   2. engine.cpp deliberately concludes nothing about unverifiable rows --
//      correct for that bucket -- so the row keeps status="pending";
//   3. the row restores into State as a PendingOffer built field by field,
//      and cancel_pending is not among the fields set. It defaults to FALSE
//      (types.hpp);
//   4. cancel_stale skips only cancel_pending offers. It sees false, and any
//      row older than offer_ttl_blocks{400} -- which a restored row's
//      created_at_block makes likely -- gets a SECOND secure cancel on the
//      first heartbeat.
//
// A second secure cancel is a second fee, and worse than the fee: the bundle
// respends coins already spent or already in the mempool, so it is usually
// REJECTED rather than confirmed, leaving a never-confirmable local
// transaction holding a locked fee coin. Locked XCH against fee_reserve_xch
// is what pushes the NEXT cancel down the insufficient-funds branch into the
// emergency ladder.
//
// The repo already got this right one layer over: recheck_terminal calls
// state_->mark_cancel_pending() for kPendingCancel, with a comment saying
// exactly this. The startup leg did not.
//
// WHY THIS FILE EXISTS RATHER THAN A TEST INSIDE OfferManager
// ----------------------------------------------------------
// S36: nothing in cpp/tests constructs an Engine, and startup_reconcile is a
// coroutine over a live wallet RPC. So the DECIDING part was extracted into
// classify_startup_probe(), a total function of the parsed status code, and
// that is what is pinned here. What remains in offer_manager.cpp is a switch
// that pushes onto one of five vectors.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <string>

#include "xop/execution/offer_manager.hpp"

using namespace xop::execution;

// THE FINDING. This is the assertion whose absence let F2 ship.
//
// MUTATION: fold kPendingCancel back into the default arm -> fails.
TEST(StartupProbe, PendingCancelIsPositiveEvidenceNotAbsenceOfEvidence)
{
    EXPECT_EQ(classify_startup_probe(trade_status::kPendingCancel),
              StartupProbeVerdict::PendingCancel)
        << "a cancel spend is already in flight. Bucketing that as "
           "'unverifiable' restores the row with cancel_pending=false and "
           "pays a second secure cancel fee for one spend";

    // And it is emphatically NOT the bucket that means "we learned nothing".
    EXPECT_NE(classify_startup_probe(trade_status::kPendingCancel),
              StartupProbeVerdict::Unverifiable);

    // Nor is it terminal. The spend has not landed; nothing may be stamped.
    EXPECT_NE(classify_startup_probe(trade_status::kPendingCancel),
              StartupProbeVerdict::Terminal);
}

// Terminal means the wallet is finished with it, and only these two codes do.
//
// MUTATION: add kPendingCancel or kPendingConfirm to the Terminal arm -> the
// row gets stamped "cancelled" in the DB while its spend is still in flight.
TEST(StartupProbe, OnlyCancelledAndFailedAreTerminal)
{
    EXPECT_EQ(classify_startup_probe(trade_status::kCancelled),
              StartupProbeVerdict::Terminal);
    EXPECT_EQ(classify_startup_probe(trade_status::kFailed),
              StartupProbeVerdict::Terminal);

    EXPECT_NE(classify_startup_probe(trade_status::kPendingAccept),
              StartupProbeVerdict::Terminal);
    EXPECT_NE(classify_startup_probe(trade_status::kPendingConfirm),
              StartupProbeVerdict::Terminal);
    EXPECT_NE(classify_startup_probe(trade_status::kConfirmed),
              StartupProbeVerdict::Terminal);
}

// The 2026-07-31 defect, pinned. A CONFIRMED row FILLED while we were away;
// stamping it cancelled put six XCH/BYC fills off the books.
//
// MUTATION: map kConfirmed to Terminal -> fails.
TEST(StartupProbe, ConfirmedIsAFillAndIsNeverTerminal)
{
    EXPECT_EQ(classify_startup_probe(trade_status::kConfirmed),
              StartupProbeVerdict::Confirmed);
    EXPECT_NE(classify_startup_probe(trade_status::kConfirmed),
              StartupProbeVerdict::Terminal);
}

TEST(StartupProbe, PendingAcceptIsAnOrdinaryRestingOffer)
{
    EXPECT_EQ(classify_startup_probe(trade_status::kPendingAccept),
              StartupProbeVerdict::StillLive);
}

// Unknown must read as "no evidence", never as a verdict that authorises an
// action. A status string from a future wallet build must not be able to get
// a row stamped, adopted, or marked cancel-pending.
//
// MUTATION: make the default arm anything but Unverifiable -> fails.
TEST(StartupProbe, AnUnrecognisedCodeAuthorisesNothing)
{
    EXPECT_EQ(classify_startup_probe(trade_status::kUnknown),
              StartupProbeVerdict::Unverifiable);
    EXPECT_EQ(classify_startup_probe(trade_status::kPendingConfirm),
              StartupProbeVerdict::Unverifiable);

    // Anything at all that nobody has seen before.
    for (const int code : {6, 7, 42, 999, -2, -17}) {
        EXPECT_EQ(classify_startup_probe(code),
                  StartupProbeVerdict::Unverifiable)
            << "status code " << code << " is not evidence of anything";
    }
}

// The bucket must survive into StartupDbLeg as its own vector, and total()
// must count it -- otherwise the leg's own summary log under-reports and the
// engine has nothing to iterate when it marks the restored entries.
//
// MUTATION: drop pending_cancel from total() -> fails.
TEST(StartupProbe, TheLegCountsTheCancelInFlightBucket)
{
    OfferManager::StartupDbLeg leg;
    leg.terminal.push_back("a");
    leg.still_live.push_back("b");
    leg.confirmed.push_back("c");
    leg.pending_cancel.push_back("d");
    leg.unverifiable.push_back("e");

    EXPECT_EQ(leg.total(), 5u)
        << "every probed row is in exactly one bucket and total() is what the "
           "operator-facing log reports";

    // A default leg carries nothing, so a boot with no DB-pending rows cannot
    // manufacture a cancel-pending mark.
    const OfferManager::StartupDbLeg empty;
    EXPECT_EQ(empty.total(), 0u);
    EXPECT_TRUE(empty.pending_cancel.empty());
}

// ---------------------------------------------------------------------------
// [F6 2026-09-03] THE OTHER HALF OF F2, WHICH WAS NOT TESTED.
//
// Everything above pins classify_startup_probe(), a pure function of the
// status code -- and mutating it does fail, at COMPILE time, via the
// static_asserts in offer_manager.hpp. But the verdict has to become a bucket
// before it becomes a cancel_pending flag, and that step lived in
// offer_manager.cpp as a bare switch that no test reached. Repointing its
// PendingCancel arm at `unverifiable` reinstated the whole duplicate-fee
// defect with all 1265 tests green.
//
// The step is now StartupDbLeg::add(). These are the tests that make the
// mutation fail.
// ---------------------------------------------------------------------------

// MUTATION: in StartupDbLeg::add(), push a PendingCancel verdict onto
// `unverifiable` instead -> fails here. That single edit is the F2 defect end
// to end: the row lands in the bucket that means "we learned nothing", the
// engine's restore_cancel_pending set stays empty, the row restores with
// cancel_pending=false, and cancel_stale charges a second secure cancel for
// one in-flight spend.
TEST(StartupProbe, ACancelInFlightVerdictReachesTheCancelInFlightBucket)
{
    OfferManager::StartupDbLeg leg;
    leg.add(StartupProbeVerdict::PendingCancel, "offer-pc");

    ASSERT_EQ(leg.pending_cancel.size(), 1u)
        << "this vector is the ONLY input to the engine's "
           "restore_cancel_pending set; an id that misses it is restored "
           "chargeable";
    EXPECT_EQ(leg.pending_cancel.front(), "offer-pc");
    EXPECT_TRUE(leg.unverifiable.empty());
    EXPECT_TRUE(leg.terminal.empty());
    EXPECT_TRUE(leg.confirmed.empty());
    EXPECT_TRUE(leg.still_live.empty());
}

// And the whole map, so no arm can be quietly repointed at another bucket.
//
// MUTATION: swap any two arms of the switch in add() -> fails.
TEST(StartupProbe, EveryVerdictLandsInItsOwnBucketAndOnlyThatOne)
{
    OfferManager::StartupDbLeg leg;
    leg.add(StartupProbeVerdict::Terminal,      "t");
    leg.add(StartupProbeVerdict::StillLive,     "s");
    leg.add(StartupProbeVerdict::Confirmed,     "c");
    leg.add(StartupProbeVerdict::PendingCancel, "p");
    leg.add(StartupProbeVerdict::Unverifiable,  "u");

    ASSERT_EQ(leg.total(), 5u) << "exactly one bucket each, no drops";
    ASSERT_EQ(leg.terminal.size(), 1u);
    ASSERT_EQ(leg.still_live.size(), 1u);
    ASSERT_EQ(leg.confirmed.size(), 1u);
    ASSERT_EQ(leg.pending_cancel.size(), 1u);
    ASSERT_EQ(leg.unverifiable.size(), 1u);
    EXPECT_EQ(leg.terminal.front(),       "t");
    EXPECT_EQ(leg.still_live.front(),     "s");
    EXPECT_EQ(leg.confirmed.front(),      "c");
    EXPECT_EQ(leg.pending_cancel.front(), "p");
    EXPECT_EQ(leg.unverifiable.front(),   "u");
}

// The two halves composed, which is the sequence the engine actually depends
// on: a wallet status of PENDING_CANCEL must end up in pending_cancel. Either
// half can be mutated alone and this catches both.
TEST(StartupProbe, TheWalletStatusReachesTheBucketThroughBothHalves)
{
    OfferManager::StartupDbLeg leg;
    for (const int status : {trade_status::kCancelled,
                             trade_status::kFailed,
                             trade_status::kConfirmed,
                             trade_status::kPendingAccept,
                             trade_status::kPendingCancel,
                             trade_status::kPendingConfirm,
                             trade_status::kUnknown}) {
        leg.add(classify_startup_probe(status), std::to_string(status));
    }

    ASSERT_EQ(leg.pending_cancel.size(), 1u)
        << "PENDING_CANCEL, and nothing else, is a cancel already in flight";
    EXPECT_EQ(leg.pending_cancel.front(),
              std::to_string(trade_status::kPendingCancel));
    EXPECT_EQ(leg.terminal.size(), 2u);       // cancelled, failed
    EXPECT_EQ(leg.confirmed.size(), 1u);      // confirmed
    EXPECT_EQ(leg.still_live.size(), 1u);     // pending_accept
    EXPECT_EQ(leg.unverifiable.size(), 2u);   // pending_confirm, unknown
}
