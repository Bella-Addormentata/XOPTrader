// ---------------------------------------------------------------------------
// [S46 2026-09-02] The DB -> wallet leg of startup_reconcile.
//
// WHAT WAS OBSERVED
// -----------------
// A shutdown left seven offers live. The next engine booted, ran
// startup_reconcile, and logged:
//
//     [startup_reconcile] Complete: 0 wallet offers scanned,
//                         0 known/restored, 0 orphans
//
// Seven rows sat pending in offer_log the whole time, AND THAT LINE READ AS
// SUCCESS. The reconcile was WALLET -> DB only: `known_offer_ids` was used
// purely as a membership test to label a wallet record known-vs-orphan, so a
// DB row the wallet never mentioned was never examined at all. And when
// get_all_offers() failed outright, the routine took an EARLY RETURN before
// even that much happened -- reporting zero of everything because it had
// looked at nothing.
//
// That is the close_out fail-open shape: an empty or failed read reported as
// a clean bill of health.
//
// WHAT IS PINNED HERE, AND WHAT IS NOT
// ------------------------------------
// S36: nothing in cpp/tests constructs an Engine or an OfferManager, and
// startup_reconcile is a coroutine over a live wallet RPC. So the two
// DECIDING steps were extracted as total functions and those are what these
// tests hold:
//
//   plan_startup_probe()      -- is this DB row probed, skipped, or marked
//                                unverifiable? This is where the failed-scan
//                                decision lives.
//   classify_startup_probe()  -- what does one get_offer() answer mean?
//   StartupDbLeg::add()       -- which bucket does that verdict land in?
//
// NOT pinned, and stated so no report claims otherwise: the loop in
// offer_manager.cpp that calls them, its try/catch, and everything
// engine.cpp does with the resulting buckets. Those are hand-reasoned and
// carry [UNGUARDED] notes at their call sites.
//
// ON PENDING_CANCEL. It is kept as its own bucket because it is POSITIVE
// evidence -- a cancel spend is already in flight -- and `unverifiable`
// means the absence of evidence; collapsing the two throws information away
// and the operator log is built from these buckets. The engine REPORTS this
// bucket by id. It deliberately does NOT latch State::cancel_pending from
// it: that was tried and withdrawn, because cancel_pending is one-way with
// no clearer and no timeout, so latching it strands the offer for the
// process lifetime in exchange for one saved cancel fee. See TODO.md S47.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "xop/execution/offer_manager.hpp"

using namespace xop::execution;

// ===========================================================================
// THE OBSERVED SYMPTOM, PINNED.
//
// These are the tests that fail if the 2026-09-02 behaviour comes back.
// ===========================================================================

// MUTATION #4, THE ONE THE SCOPE CUT NAMES: make an empty or failed wallet
// read mean "nothing pending". Change the first branch of plan_startup_probe
// to return AlreadyAnswered (or delete the `scan_complete` parameter and
// always trust the mention set) -> this fails.
//
// This is the exact incident: seven DB-pending rows, a wallet read that told
// us nothing, and a routine that reported zero orphans and moved on.
TEST(StartupProbe, AFailedWalletScanMarksEveryDbRowUnverifiableNotResolved)
{
    // Seven rows, as on 2026-09-02.
    const std::vector<std::string> db_pending{
        "0x9a72dfe913", "0x1b83aef024", "0x2c94bfa135", "0x3da5c0b246",
        "0x4eb6d1c357", "0x5fc7e2d468", "0x60d8f3e579"};

    OfferManager::StartupDbLeg leg;
    std::size_t probes = 0;
    for (const auto& id : db_pending) {
        // scan_complete = false: get_all_offers() threw. The mention set is
        // empty, exactly as it was when the old code returned early.
        const auto action = plan_startup_probe(
            /*scan_complete=*/false, /*wallet_mentioned=*/false, probes,
            OfferManager::kMaxStartupDbProbes);
        ASSERT_EQ(action, StartupProbeAction::MarkUnverifiable)
            << "a wallet read that FAILED is not evidence that " << id
            << " is resolved. It is evidence of nothing";
        leg.unverifiable.push_back(id);
    }

    // The whole point: the leg ACCOUNTS FOR ALL SEVEN. The old code reported
    // zero, and zero read as success.
    EXPECT_EQ(leg.total(), 7u)
        << "the 2026-09-02 reconcile reported '0 orphans' with these seven "
           "rows stranded. Anything that reports fewer than all seven here "
           "is that bug";
    EXPECT_EQ(leg.unverifiable.size(), 7u);
    EXPECT_TRUE(leg.terminal.empty())
        << "nothing may be STAMPED off a scan that failed";
}

// The complementary half: the scan SUCCEEDED and simply did not mention the
// row. That row is the one the wallet->DB-only reconcile could not see, and
// it must be probed rather than skipped.
//
// MUTATION: return AlreadyAnswered for an unmentioned row -> fails.
TEST(StartupProbe, ACompleteScanThatOmitsADbRowStillProbesIt)
{
    EXPECT_EQ(plan_startup_probe(/*scan_complete=*/true,
                                 /*wallet_mentioned=*/false, 0,
                                 OfferManager::kMaxStartupDbProbes),
              StartupProbeAction::Probe)
        << "this is the missing direction. A DB row the wallet never "
           "mentioned was invisible to the old reconcile";

    // A row the wallet DID answer about costs no extra RPC.
    EXPECT_EQ(plan_startup_probe(true, true, 0,
                                 OfferManager::kMaxStartupDbProbes),
              StartupProbeAction::AlreadyAnswered);
}

// The probe cap must degrade to "we assume nothing", never to "it is gone".
//
// MUTATION: past the cap, return AlreadyAnswered -> fails. The remainder
// would silently vanish from the leg's accounting, which is the same
// under-reporting shape as the incident.
TEST(StartupProbe, PastTheProbeCapWeAssumeNothing)
{
    constexpr std::size_t kCap = OfferManager::kMaxStartupDbProbes;
    EXPECT_EQ(plan_startup_probe(true, false, kCap, kCap),
              StartupProbeAction::MarkUnverifiable);
    EXPECT_EQ(plan_startup_probe(true, false, kCap + 100, kCap),
              StartupProbeAction::MarkUnverifiable);
    // Just under the cap is still a probe.
    EXPECT_EQ(plan_startup_probe(true, false, kCap - 1, kCap),
              StartupProbeAction::Probe);
}

// The zero enumerator declines, as everywhere else in this repo: a
// default-constructed action must not authorise an RPC and must never mean
// "resolved".
TEST(StartupProbe, TheDefaultActionAuthorisesNothing)
{
    EXPECT_EQ(StartupProbeAction{}, StartupProbeAction::MarkUnverifiable);
    EXPECT_NE(StartupProbeAction{}, StartupProbeAction::Probe);
    EXPECT_NE(StartupProbeAction{}, StartupProbeAction::AlreadyAnswered);
}

// ===========================================================================
// The bucketing rule.
// ===========================================================================

// PENDING_CANCEL is evidence, and the operator log is built from these
// buckets. Collapsing it into `unverifiable` throws that away.
//
// MUTATION: fold kPendingCancel back into the default arm -> fails.
TEST(StartupProbe, PendingCancelIsPositiveEvidenceNotAbsenceOfEvidence)
{
    EXPECT_EQ(classify_startup_probe(trade_status::kPendingCancel),
              StartupProbeVerdict::PendingCancel)
        << "a cancel spend is already in flight, and the operator is told so "
           "by id. 'unverifiable' means we learned nothing, which is the "
           "opposite claim";

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
// must count it -- otherwise the leg's own summary log under-reports, which
// is the under-reporting shape the incident was made of.
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

    // A default leg carries nothing, so a boot with no DB-pending rows
    // reports nothing rather than manufacturing a bucket.
    const OfferManager::StartupDbLeg empty;
    EXPECT_EQ(empty.total(), 0u);
    EXPECT_TRUE(empty.pending_cancel.empty());
}

// ---------------------------------------------------------------------------
// THE OTHER HALF, WHICH IS EASY TO LEAVE UNTESTED.
//
// Everything above pins classify_startup_probe(), a pure function of the
// status code -- and mutating it fails at COMPILE time, via the
// static_asserts in offer_manager.hpp. But the verdict still has to become a
// BUCKET, and when that step lived in offer_manager.cpp as a bare switch,
// nothing reached it: repointing one arm at another vector left the whole
// suite green while changing what the operator is told.
//
// The step is now StartupDbLeg::add(). These are the tests that make that
// mutation fail.
// ---------------------------------------------------------------------------

// MUTATION: in StartupDbLeg::add(), push a PendingCancel verdict onto
// `unverifiable` instead -> fails here.
TEST(StartupProbe, ACancelInFlightVerdictReachesTheCancelInFlightBucket)
{
    OfferManager::StartupDbLeg leg;
    leg.add(StartupProbeVerdict::PendingCancel, "offer-pc");

    ASSERT_EQ(leg.pending_cancel.size(), 1u)
        << "this vector is what the engine's operator warning is built from; "
           "an id that misses it is never named to anyone";
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
