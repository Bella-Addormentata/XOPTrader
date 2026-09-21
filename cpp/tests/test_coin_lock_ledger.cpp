#include <gtest/gtest.h>

#include <xop/execution/coin_lock_ledger.hpp>
#include <xop/execution/offer_manager.hpp>

#include <nlohmann/json.hpp>

#include <vector>

namespace {

using xop::Mojo;
using xop::execution::CoinLockLedger;
using xop::execution::OfferManager;
using xop::execution::reserve_bulk_cancel;
using xop::rpc::kCancelOffersSingleBatchSize;

constexpr Mojo kXch = 1'000'000'000'000LL;
constexpr Mojo kFee = 28'922;  // the live per-offer fee from the incident
constexpr Mojo kBatchFee = 10'000'000;  // live current_fee_mojos_, per batch
// The live wallet refusal text, verbatim (take_retry.hpp classifies it).
constexpr const char* kSweepRefusal =
    "Wallet needs to be fully synced before making transactions.";

TEST(CoinLockLedgerTest, ReplaysTheIncidentBatchExactly) {
    // [XCH-LOCK-LEDGER 2026-08-23] 2026-08-23 13:48:58Z: spendable was
    // 14.59 XCH in ~2-XCH coins; the engine posted 5 one-XCH asks and 5
    // CAT-principal bids (fee-coin locks only) and hit spendable=0 in 73
    // seconds because every stale wallet re-query approved the next offer.
    // Exact modeled trace (review: pin equalities, not inequalities, so
    // cap-arithmetic drift cannot hide): 5 spend-side asks lock a 2-XCH
    // coin each (committed 10, the only cap charges); buy-XCH bids go
    // through the cap-exempt floor-only path -- bid 1 locks the 0.59 tail
    // coin, bid 2 a 2-XCH coin, and bid 3 is refused at the FLOOR (the
    // remaining 2-XCH coin must survive).  7 admitted, exactly 2 XCH
    // remaining -- production admitted all 10 and hit zero.
    std::vector<Mojo> coins(7, 2 * kXch);
    const Mojo tail = kXch * 59 / 100;
    coins.push_back(tail);
    CoinLockLedger ledger(coins, /*floor=*/kXch / 2, /*commit_frac=*/1.0);

    int admitted = 0;
    for (int i = 0; i < 5; ++i) {          // 1-XCH asks: spend side, capped
        if (ledger.try_lock(kXch, kFee)) ++admitted;
    }
    for (int i = 0; i < 5; ++i) {          // buy-XCH bids: fee-coin locks,
        if (ledger.try_lock_floor_only(0, kFee)) ++admitted;   // cap-exempt
    }

    // 5 asks lock a 2-XCH coin each (committed 10); bid 1 locks the 0.59
    // tail, bid 2 a 2-XCH coin -- neither counts against the cap -- and
    // bid 3 would leave the pool below the 0.5 floor: refused THERE, not
    // at the cap.  7 admitted, exactly 2 XCH remaining; production
    // admitted all 10 and hit zero.
    EXPECT_EQ(admitted, 7);
    EXPECT_EQ(ledger.remaining(), 2 * kXch);
    EXPECT_EQ(ledger.committed(), 10 * kXch);
}

TEST(CoinLockLedgerTest, CycleCapLimitsCommitmentFraction) {
    // frac 0.5 of (14 - 0.5) leaves 6.75 XCH commitable: three 2-XCH coin
    // locks fit, the fourth crosses the cap and is refused.
    std::vector<Mojo> coins(7, 2 * kXch);
    CoinLockLedger ledger(coins, kXch / 2, 0.5);

    EXPECT_TRUE(ledger.try_lock(kXch, kFee));
    EXPECT_TRUE(ledger.try_lock(kXch, kFee));
    EXPECT_TRUE(ledger.try_lock(kXch, kFee));
    EXPECT_FALSE(ledger.try_lock(kXch, kFee));
    EXPECT_EQ(ledger.committed(), 6 * kXch);
}

TEST(CoinLockLedgerTest, SmallestCoveringCoinIsSelected) {
    std::vector<Mojo> coins = {3 * kXch, 2 * kXch, 10 * kXch};
    CoinLockLedger ledger(coins, 0, 1.0);

    ASSERT_TRUE(ledger.try_lock(kXch, 0));   // locks the 2-XCH coin
    EXPECT_EQ(ledger.committed(), 2 * kXch);
    EXPECT_EQ(ledger.remaining(), 13 * kXch);
}

TEST(CoinLockLedgerTest, AccumulationOrderIsLargestFirstAndPinned) {
    // Distinguishable coins so a selection-order mutation fails.  Need 5
    // against {1, 2, 4}: no single coin covers; LARGEST-first accumulation
    // (chia coin_selection.py's descending sort / fallback order) locks
    // {4, 2} = 6 with the 1-coin left; smallest-first would lock all
    // three (7, remaining 0) -- the equalities below kill that mutant.
    std::vector<Mojo> coins = {kXch, 2 * kXch, 4 * kXch};
    CoinLockLedger ledger(coins, 0, 1.0);

    ASSERT_TRUE(ledger.try_lock(5 * kXch, 0));
    EXPECT_EQ(ledger.committed(), 6 * kXch);
    EXPECT_EQ(ledger.remaining(), kXch);
}

TEST(CoinLockLedgerTest, KnapsackOfSmallerCoinsIsChargedWhenWalletPrefersIt) {
    // (review, model-fidelity) Chia knapsacks SMALLER coins whenever the
    // sub-need coins sum past the need: need 1.0 against {1.1, 0.9, 0.9}
    // locks 0.9+0.9 = 1.8 on the real wallet, not the 1.1 covering coin.
    // A pure smallest-covering model under-charged exactly this case and
    // left the floor soft; the ledger now charges the larger candidate.
    std::vector<Mojo> coins = {
        kXch * 11 / 10, kXch * 9 / 10, kXch * 9 / 10};
    CoinLockLedger ledger(coins, 0, 1.0);

    ASSERT_TRUE(ledger.try_lock(kXch, 0));
    EXPECT_EQ(ledger.committed(), 2 * (kXch * 9 / 10));
    EXPECT_EQ(ledger.remaining(), kXch * 11 / 10);
}

TEST(CoinLockLedgerTest, KnapsackAccepts501CoinOvershoot) {
    // (review round 11) chia 2.7.3's knapsack checks len > 500 BEFORE
    // adding the next coin, so a 501-coin overshooting set is reachable
    // and select_coins does not revalidate it: 501 two-mojo dust coins
    // vs need 1001 are selected over the 1002-mojo covering coin.
    std::vector<Mojo> coins(501, 2);
    coins.push_back(1'002);
    CoinLockLedger ledger(coins, 0, 1.0);

    ASSERT_TRUE(ledger.try_lock(1'001, 0));
    EXPECT_EQ(ledger.committed(), 1'002);    // the dust total (501 x 2)
    EXPECT_EQ(ledger.remaining(), 1'002);    // the big coin survives
}

TEST(CoinLockLedgerTest, OvershootingKnapsackAccepts500Coins) {
    // (review round 10) The strict <500 rule applies only to the
    // exact-total branch; chia's knapsack fallback accepts an OVERSHOOTING
    // 500-coin result.  500 two-mojo coins vs need 999: the wallet locks
    // the 500 small coins (covering 1000), not the 1000-mojo coin -- and
    // so must the model, or a later lock diverges on identity.
    std::vector<Mojo> coins(500, 2);
    coins.push_back(1'000);
    CoinLockLedger ledger(coins, 0, 1.0);

    ASSERT_TRUE(ledger.try_lock(999, 0));
    EXPECT_EQ(ledger.committed(), 1'000);    // the dust total, overshooting
    EXPECT_EQ(ledger.remaining(), 1'000);    // the big coin survives
    ASSERT_TRUE(ledger.try_lock(2, 0));      // must now lock the 1000 coin
    EXPECT_EQ(ledger.remaining(), 0);
}

TEST(CoinLockLedgerTest, ExactSingleCoinMatchWinsOverKnapsack) {
    // (review round 8) The wallet takes an EXACT one-coin match before any
    // smaller-coin knapsacking: {0.6, 0.6, 1.0} with need 1.0 removes the
    // 1.0 coin, never the 0.6s.
    std::vector<Mojo> coins = {kXch * 6 / 10, kXch * 6 / 10, kXch};
    CoinLockLedger ledger(coins, 0, 1.0);

    ASSERT_TRUE(ledger.try_lock(kXch, 0));
    EXPECT_EQ(ledger.committed(), kXch);
    EXPECT_EQ(ledger.remaining(), 2 * (kXch * 6 / 10));
}

TEST(CoinLockLedgerTest, SmallerCoinMatchRequiresStrictlyUnder500Coins) {
    // (review round 8) chia's exact-all-smaller branch requires
    // len(smaller_coins) < 500; AT 500 the wallet falls back to a covering
    // coin, so the model must too -- 500 one-mojo coins summing exactly to
    // the need still select the 1000-mojo covering coin.
    std::vector<Mojo> coins(500, 1);
    coins.push_back(1'000);
    CoinLockLedger ledger(coins, 0, 1.0);

    ASSERT_TRUE(ledger.try_lock(500, 0));
    EXPECT_EQ(ledger.committed(), 1'000);   // the covering coin, not the dust
    EXPECT_EQ(ledger.remaining(), 500);
}

TEST(CoinLockLedgerTest, KnapsackIdentityIsPreservedAcrossLocks) {
    // (review round 5) {4,4,9} with needs 8 then 4: the wallet knapsacks
    // {4,4} for the first need and must lock 9 for the second, ending at
    // 0 -- so with floor 4 the SECOND lock must be refused.  A max-charge
    // model locked 9 first and 4 second, reported 4 remaining, and let
    // both through: identity fidelity, not just value, keeps the floor
    // honest across a sequence.
    std::vector<Mojo> coins = {4 * kXch, 4 * kXch, 9 * kXch};
    CoinLockLedger ledger(coins, 4 * kXch, 1.0);

    ASSERT_TRUE(ledger.try_lock(8 * kXch, 0));   // locks the two 4s
    EXPECT_EQ(ledger.committed(), 8 * kXch);
    EXPECT_EQ(ledger.remaining(), 9 * kXch);
    EXPECT_FALSE(ledger.try_lock(4 * kXch, 0));  // would lock 9, floor 4
    EXPECT_EQ(ledger.remaining(), 9 * kXch);
}

TEST(CoinLockLedgerTest, OneGiantCoinIsRefusedRatherThanZeroed) {
    // A single 44-XCH coin covering a 1-XCH ask would leave the wallet at
    // literal zero spendable -- the ledger models exactly that and refuses,
    // leaving the CAT sides free to quote while ensure_split heals the pool.
    std::vector<Mojo> coins = {44 * kXch};
    CoinLockLedger ledger(coins, kXch / 2, 1.0);

    EXPECT_FALSE(ledger.try_lock(kXch, kFee));
    EXPECT_EQ(ledger.committed(), 0);
    EXPECT_EQ(ledger.remaining(), 44 * kXch);
}

TEST(CoinLockLedgerTest, NoteLockDrainsThePoolButNotTheOfferCap) {
    // (review, major) Cancel fees are real pool drains but are NOT offer
    // commitment: charging them against the cap let a 4-tier refresh's
    // cancels consume the posting budget before a single replacement
    // posted.  After two cancel drains, an offer lock within the cap must
    // still be admitted.
    std::vector<Mojo> coins(4, 2 * kXch);
    CoinLockLedger ledger(coins, kXch / 2, 0.5);   // cap = 3.75 XCH

    ledger.note_lock(0, kFee);                     // cancel fee: whole coin
    ledger.note_lock(0, kFee);                     // cancel fee: whole coin
    EXPECT_EQ(ledger.committed(), 0);              // cap untouched
    EXPECT_EQ(ledger.remaining(), 4 * kXch);       // pool reflects reality

    EXPECT_TRUE(ledger.try_lock(kXch, kFee));      // 2-XCH lock <= 3.75 cap
    EXPECT_EQ(ledger.committed(), 2 * kXch);
}

TEST(CoinLockLedgerTest, FloorRefusesAfterCancelDrainsEvenWhenCapAdmits) {
    // (review) With cancels bypassing the cap, the floor check is a live,
    // independent guard: drain one coin as a cancel fee, then an offer
    // lock that fits the cap must still refuse on the floor.
    std::vector<Mojo> coins = {2 * kXch, 2 * kXch};
    CoinLockLedger ledger(coins, kXch / 2, 1.0);   // cap = 3.5 XCH

    ledger.note_lock(0, kFee);                     // pool: one 2-XCH coin left
    EXPECT_FALSE(ledger.try_lock(kXch, kFee));     // cap fine; floor refuses
    EXPECT_EQ(ledger.remaining(), 2 * kXch);
}

TEST(CoinLockLedgerTest, RefusalThenSmallerLockIsAdmitted) {
    // (review) The old fixture proved nothing: need==0 admits before the
    // pool is consulted.  This one genuinely shows refusal-then-admit.
    std::vector<Mojo> coins = {kXch, 2 * kXch};
    CoinLockLedger ledger(coins, 0, 0.5);          // cap = 1.5 XCH

    EXPECT_FALSE(ledger.try_lock(2 * kXch, 0));    // locks 2 > cap 1.5
    EXPECT_EQ(ledger.committed(), 0);
    EXPECT_EQ(ledger.remaining(), 3 * kXch);       // refusal locked nothing
    EXPECT_TRUE(ledger.try_lock(kXch, 0));         // the 1-XCH coin fits
    EXPECT_EQ(ledger.committed(), kXch);
}

TEST(CoinLockLedgerTest, FloorOnlyLockIsCapExemptButNeverFloorExempt) {
    // (review round 3) Buy-XCH offers skip the spend cap -- the recovery
    // escapes must not starve -- but an unconditional bypass would let
    // fee-coin locks alone drain the pool to zero, re-opening the incident
    // from the other side.  The floor is the line neither path may cross.
    std::vector<Mojo> coins = {2 * kXch, 2 * kXch};
    CoinLockLedger ledger(coins, kXch / 2, 0.0);   // cap = 0: spend side dead

    EXPECT_FALSE(ledger.try_lock(kXch, kFee));            // cap refuses spend
    EXPECT_TRUE(ledger.try_lock_floor_only(0, kFee));     // buy-XCH admitted
    EXPECT_EQ(ledger.committed(), 0);                     // cap untouched
    EXPECT_FALSE(ledger.try_lock_floor_only(0, kFee));    // floor refuses:
    EXPECT_EQ(ledger.remaining(), 2 * kXch);              // last coin stays
}

TEST(CoinLockLedgerTest, NoteLockOverdrainClearsThePool) {
    std::vector<Mojo> coins = {kXch};
    CoinLockLedger ledger(coins, 0, 1.0);
    ledger.note_lock(5 * kXch, 0);                 // cannot cover: drain all
    EXPECT_EQ(ledger.remaining(), 0);
    EXPECT_EQ(ledger.committed(), 0);
}

TEST(CoinLockLedgerTest, InactiveLedgerAdmitsEverything) {
    CoinLockLedger ledger;
    EXPECT_FALSE(ledger.active());
    EXPECT_TRUE(ledger.try_lock(1'000 * kXch, kFee));
    ledger.note_lock(kXch, kFee);
    EXPECT_EQ(ledger.committed(), 0);
}

// ---------------------------------------------------------------------------
// Bulk-cancel fee reservation ([S33 2026-09-11]: the one change on this branch
// that moves real XCH, and it shipped with no coverage -- reinstating the
// single note_lock(0, fee) turned no test red).  These drive a real ledger
// through the same helper OfferManager::cancel_offers_charged calls, and
// assert on POOL STATE, so none of them can pass vacuously.
// ---------------------------------------------------------------------------

TEST(CoinLockLedgerTest, BulkCancelReservesOneFeeCoinForTheSingleBatch) {
    // [BULKCANCEL-B 2026-09-13] 20 offers is ONE batch at
    // kCancelOffersSingleBatchSize: the daemon charges batch_fee once and
    // selects at most one fee coin.  The old batch size of 5 drained FOUR
    // whole coins here, for a sweep whose later batches would each pick their
    // fee coin blind to the first (see rpc/wallet_requests.hpp).
    std::vector<Mojo> coins(10, kXch);
    CoinLockLedger ledger(coins, /*floor=*/0, /*commit_frac=*/1.0);

    reserve_bulk_cancel(ledger, kBatchFee, /*n_offers=*/20);

    // A WHOLE coin, not kBatchFee: the fee coin is spent whole and its change
    // comes back only when the cancel confirms.
    EXPECT_EQ(ledger.remaining(), 9 * kXch);
    EXPECT_NE(ledger.remaining(), 10 * kXch - kBatchFee);
    // Cancel fees are pool drains, never offer commitment (see note_lock).
    EXPECT_EQ(ledger.committed(), 0);

    // 21 offers is the same single batch -- no longer a fifth one.
    std::vector<Mojo> more(10, kXch);
    CoinLockLedger same_batch(more, 0, 1.0);
    reserve_bulk_cancel(same_batch, kBatchFee, /*n_offers=*/21);
    EXPECT_EQ(same_batch.remaining(), 9 * kXch);

    // Past the batch size the daemon splits the sweep, and the count follows
    // the batch size the request actually carries: two batches, two coins.
    std::vector<Mojo> split_pool(10, kXch);
    CoinLockLedger two_batches(split_pool, 0, 1.0);
    reserve_bulk_cancel(two_batches, kBatchFee,
                        /*n_offers=*/kCancelOffersSingleBatchSize + 1);
    EXPECT_EQ(two_batches.remaining(), 8 * kXch);
}

TEST(CoinLockLedgerTest, BulkCancelWithNoTrackedOffersStillReservesOneBatch) {
    // cancel_all:true cancels offers this process never tracked (a previous
    // instance's book), so an empty tracked book is NOT a free call.  The
    // >= 1 clamp must still reserve one whole fee coin.
    std::vector<Mojo> coins(10, kXch);
    CoinLockLedger ledger(coins, /*floor=*/0, /*commit_frac=*/1.0);

    reserve_bulk_cancel(ledger, kBatchFee, /*n_offers=*/0);

    EXPECT_EQ(ledger.remaining(), 9 * kXch);
    EXPECT_EQ(ledger.committed(), 0);
}

TEST(CoinLockLedgerTest, BulkCancelDrainLeavesLessRoomForTheNextOffer) {
    // The reservation is not bookkeeping -- it decides admissions.  Ten
    // 1-XCH coins behind a 9-XCH fee-reserve floor: the single batch's one
    // whole-coin drain lands the pool exactly ON the floor, so the next offer
    // lock must be refused.  Reserving no coin would leave 10 XCH and wrongly
    // admit it, which is how an under-reservation walks the wallet toward
    // zero spendable.
    std::vector<Mojo> coins(10, kXch);
    CoinLockLedger ledger(coins, /*floor=*/9 * kXch, /*commit_frac=*/1.0);

    reserve_bulk_cancel(ledger, kBatchFee, /*n_offers=*/20);

    EXPECT_EQ(ledger.remaining(), 9 * kXch);
    EXPECT_FALSE(ledger.try_lock(kXch, 0));   // floor refuses; cap is fine
    EXPECT_EQ(ledger.remaining(), 9 * kXch);  // a refusal locks nothing
}

// ---------------------------------------------------------------------------
// [S33 2026-09-12] A REFUSED WALLET-WIDE SWEEP MUST SURVIVE THE PER-ID
// FALLBACK.
//
// cancel_all sends cancel_all:true.  When that wallet-wide sweep is REFUSED
// but the local book is NOT empty, cancel_all falls back to cancelling each
// tracked id -- and cancel_ids() returns a FRESH CancelOutcome.  Assigning it
// (`out = co_await cancel_ids(...)`) discarded sweep_refused wholesale, so a
// fallback that succeeded produced `failed` empty and sweep_refused false:
// CancelLadder::record() took its Done branch, clean() returned true, and
// shutdown logged "All outstanding offers cancelled" after a sweep the wallet
// had REFUSED.  The per-id leg can never reach the untracked book the sweep
// was refused over, so its success proves nothing about it.
//
// MUTATION CHECK, run 2026-09-12, each defect reinstated exactly and alone:
//   M13  drop `fallback.sweep_refused = true;` (i.e. the plain assignment
//        the fold replaced)   -> BOTH tests below FAIL, on sweep_refused and
//                               on all_cancelled().
//   M14  restore [S46]'s extra `&& !fallback.failed.empty()` guard on the
//        last_error carry     -> TheFallbacksFreshOutcomeCannotDropTheRefusal
//                               FAILS: the refusal reaches the operator with
//                               no text at all, which is the one case where
//                               it is the only thing that went wrong.
//   M15  carry the bulk text unconditionally (drop the empty check)
//                             -> ThePerIdLoopsOwnFailureTextWins FAILS: the
//                               sweep's sync refusal overwrites the funding
//                               refusal the per-id loop actually hit, and
//                               worst_class with it.
//
// WHAT THIS DOES NOT REACH, stated plainly rather than implied away: nothing
// in cpp/tests constructs an OfferManager (S36), so cancel_all's WIRING has
// no coverage.  Reinstating the deleted `if (all_offers.empty()) co_return
// out;` early return, or sizing the reservation from the tracked count alone,
// leaves every test in this file green.  Those lines were verified by
// reading, not by ctest.
// ---------------------------------------------------------------------------

TEST(CancelOutcomeFoldTest, TheFallbacksFreshOutcomeCannotDropTheRefusal) {
    // The per-id leg was handed every tracked id and every one went through,
    // so it knows nothing of the sweep and its `failed` is empty.  That is
    // exactly the outcome that used to read as a clean shutdown.
    OfferManager::CancelOutcome fallback;
    fallback.cancelled = {"offer-a", "offer-b"};

    const auto out =
        OfferManager::fold_refused_sweep(fallback, kSweepRefusal);

    EXPECT_TRUE(out.sweep_refused);
    EXPECT_FALSE(out.all_cancelled())
        << "an empty `failed` after a REFUSED sweep is not success";
    EXPECT_EQ(out.last_error, kSweepRefusal)
        << "the refusal is the only thing that went wrong here; reporting it "
           "with no text leaves the operator nothing to read";
    EXPECT_EQ(out.worst_class, xop::execution::TakeFailureClass::Unsynced);
    // The fallback's own work is kept, not thrown away with the assignment.
    EXPECT_EQ(out.cancelled.size(), 2u);
}

TEST(CancelOutcomeFoldTest, ThePerIdLoopsOwnFailureTextWins) {
    // When the per-id leg failed too, ITS text is the operator-facing one:
    // the sweep's self-clearing sync refusal must not overwrite a funding
    // refusal, which is the class that earns the short leash.
    OfferManager::CancelOutcome fallback;
    fallback.failed      = {"offer-c"};
    fallback.last_error  = "insufficient funds in wallet 8";
    fallback.worst_class = xop::execution::TakeFailureClass::Funding;

    const auto out =
        OfferManager::fold_refused_sweep(fallback, kSweepRefusal);

    EXPECT_TRUE(out.sweep_refused);
    EXPECT_EQ(out.last_error, "insufficient funds in wallet 8");
    EXPECT_EQ(out.worst_class, xop::execution::TakeFailureClass::Funding);
    EXPECT_FALSE(out.all_cancelled());
}

// ---------------------------------------------------------------------------
// [review 2026-09-13, round 2] A WALLET-WIDE SWEEP THAT GOT NO ANSWER IS NEVER
// "ALL CANCELLED".
//
// cancel_all's bulk request can fail after it reached the wallet -- a timeout,
// an empty or broken reply, an HTTP 5xx (rpc::cancel_possibly_submitted) --
// and the wallet may still be running it.  cancel_all then sends nothing more,
// puts every tracked id in `failed` and sets bulk_possibly_submitted.  With an
// EMPTY local book there is no id to put in `failed` (the bulk endpoint names
// none), so the flag is the only field that says anything went wrong: the same
// shape as sweep_refused, which all_cancelled() already refuses to read as
// success.  The two flags stay separate on purpose.  A refusal is an ANSWER --
// the sweep did not run, so asking again at once is safe.  This is the absence
// of one, and asking again at once is the duplicate spend.
//
// MUTATION: drop `&& !bulk_possibly_submitted` from all_cancelled() -> FAILS
// here, on the empty-book assertion, and nowhere else.
// NOT REACHED: cancel_all itself (S36; nothing in cpp/tests constructs an
// OfferManager).  tests/test_rpc_retry_wiring.py pins that its
// possibly-submitted branch issues no cancel_ids.
// ---------------------------------------------------------------------------

TEST(CancelOutcomePossiblySubmittedTest, NeverAllCancelledEvenWithAnEmptyBook) {
    // The empty local book: nothing failed by id and nothing was refused --
    // only the flag says the sweep may still be running.
    OfferManager::CancelOutcome empty_book;
    empty_book.last_error = "CURL transport failure: Timeout was reached";
    empty_book.bulk_possibly_submitted = true;
    ASSERT_TRUE(empty_book.failed.empty());
    EXPECT_FALSE(empty_book.sweep_refused)
        << "no answer is not a refusal; the two flags must stay distinct";
    EXPECT_FALSE(empty_book.all_cancelled())
        << "a sweep that may still be running has proved nothing cancelled";

    // A tracked book: every id is reported failed as well.
    OfferManager::CancelOutcome tracked = empty_book;
    tracked.failed = {"offer-a", "offer-b"};
    EXPECT_FALSE(tracked.all_cancelled());

    // Control: nothing failed and nothing flagged is the one clean outcome.
    EXPECT_TRUE(OfferManager::CancelOutcome{}.all_cancelled());
}

// ---------------------------------------------------------------------------
// Ladder-preflight decision table (review round 9: these branches decide
// whether an entire ladder disappears).
// ---------------------------------------------------------------------------

TEST(PreflightDropsTest, DoomedSpendSideKeepsABuyXchSurvivor) {
    const auto d = OfferManager::preflight_side_drops(
        /*any_bid=*/true, /*admit_bid=*/3, /*any_ask=*/true, /*admit_ask=*/0,
        /*bids_buy_xch=*/true, /*asks_buy_xch=*/false);
    EXPECT_TRUE(d.drop_asks);
    EXPECT_FALSE(d.drop_bids);   // survivor buys XCH: deliberate one-sided
}

TEST(PreflightDropsTest, BothSidesDropWhenTheSurvivorDoesNotBuyXch) {
    // CAT/CAT pair: neither side buys XCH, so a doomed ask side takes the
    // bid side down with it rather than stranding a one-sided book.
    const auto d = OfferManager::preflight_side_drops(
        true, 3, true, 0, /*bids_buy_xch=*/false, /*asks_buy_xch=*/false);
    EXPECT_TRUE(d.drop_asks);
    EXPECT_TRUE(d.drop_bids);
}

TEST(PreflightDropsTest, PartialAdmissionDoomsNothing) {
    const auto d = OfferManager::preflight_side_drops(
        true, 3, true, 1, false, false);
    EXPECT_FALSE(d.drop_asks);
    EXPECT_FALSE(d.drop_bids);
}

TEST(PreflightDropsTest, BothSidesFullyRefusedDropsNeitherHere) {
    // Neither side survives, so there is no asymmetry to guard -- the
    // per-tier admission refusals handle it (and nothing posts anyway).
    const auto d = OfferManager::preflight_side_drops(
        true, 0, true, 0, false, false);
    EXPECT_FALSE(d.drop_asks);
    EXPECT_FALSE(d.drop_bids);
}

TEST(PreflightDropsTest, OneSidedLaddersAreLeftAlone) {
    // A bids-only ladder (e.g. the wmilliETH.b revive experiment) has no
    // asymmetry to create: nothing is dropped even when refused.
    const auto d = OfferManager::preflight_side_drops(
        true, 0, /*any_ask=*/false, 0, true, false);
    EXPECT_FALSE(d.drop_bids);
    EXPECT_FALSE(d.drop_asks);
}

// ---------------------------------------------------------------------------
// Wiring helpers (review: the extraction the incident protection rides on
// had zero coverage; a silent regression would degrade the ledger to
// fee-only accounting with every test green).
// ---------------------------------------------------------------------------

TEST(CoinLockLedgerWiringTest, XchPrincipalReadsNegativeWalletOneAmount) {
    nlohmann::json spend_dict = {{"1", -1'000'000'000'000LL},
                                 {"5", 1'000'000LL}};
    EXPECT_EQ(OfferManager::xch_principal_from_offer_dict(spend_dict),
              1'000'000'000'000LL);

    nlohmann::json receive_dict = {{"1", 1'000'000'000'000LL},
                                   {"5", -1'000'000LL}};
    EXPECT_EQ(OfferManager::xch_principal_from_offer_dict(receive_dict), 0);

    nlohmann::json cat_only = {{"4", -500'000LL}, {"5", 500'000LL}};
    EXPECT_EQ(OfferManager::xch_principal_from_offer_dict(cat_only), 0);
}

TEST(CoinLockLedgerWiringTest, SpendableAmountsTolerateBothRecordShapes) {
    std::vector<nlohmann::json> records = {
        {{"coin", {{"amount", 2'000'000'000'000LL},
                   {"puzzle_hash", "0xaa"}}}},          // wrapped
        {{"amount", 590'000'000'000LL}},                 // bare
        {{"coin", {{"puzzle_hash", "0xbb"}}}},           // no amount: skipped
    };
    const auto amounts =
        OfferManager::spendable_amounts_from_coin_records(records);
    ASSERT_EQ(amounts.size(), 2U);
    EXPECT_EQ(amounts[0], 2'000'000'000'000LL);
    EXPECT_EQ(amounts[1], 590'000'000'000LL);
}

// ---------------------------------------------------------------------------
// [MIN-INPUT-COIN review #162] A CAT-funded create can carry
// min_coin_amount, and chia applies it to the XCH fee coin as well.  The
// ledger has to select from the pool the WALLET will select from.
// ---------------------------------------------------------------------------

constexpr Mojo kM = 1'000'000;  // a million mojos

TEST(CoinLockLedgerMinCoinTest, TheFeeCoinTheWalletWillSkipIsNotTheOneCharged) {
    // The review example: fee 10M, a 20M coin, floor 100M.  The wallet cannot
    // see the 20M coin, so it locks the next one up -- a whole 1.5 XCH.
    const std::vector<Mojo> coins = {20 * kM, 3 * kXch / 2, 2 * kXch};
    CoinLockLedger with_floor(coins, 0, 1.0);
    ASSERT_TRUE(with_floor.try_lock_floor_only(0, 10 * kM, 100 * kM));
    EXPECT_EQ(with_floor.remaining(), 2 * kXch + 20 * kM);

    // The same admission before the floor was modelled.
    CoinLockLedger without(coins, 0, 1.0);
    ASSERT_TRUE(without.try_lock_floor_only(0, 10 * kM));
    EXPECT_EQ(without.remaining(), 3 * kXch / 2 + 2 * kXch);
}

TEST(CoinLockLedgerMinCoinTest, TheReserveIsCheckedAgainstWhatTheWalletLocks) {
    // THE CONSEQUENCE.  Reserve 1 XCH, pool {20M, 1.5 XCH}.  Under the floor
    // the wallet locks the 1.5 XCH coin and 20M is all that is left: refused.
    // Charged the 20M coin instead, the ledger admitted it and reported 1.5
    // XCH free that the wallet had already locked.
    const std::vector<Mojo> coins = {20 * kM, 3 * kXch / 2};
    CoinLockLedger ledger(coins, /*floor=*/kXch, 1.0);
    EXPECT_FALSE(ledger.try_lock_floor_only(0, 10 * kM, 100 * kM));
    EXPECT_EQ(ledger.remaining(), 3 * kXch / 2 + 20 * kM);  // nothing locked
}

TEST(CoinLockLedgerMinCoinTest, TheCapIsChargedTheFilteredCoinOnTheSpendPath) {
    // try_lock takes the floor too (a CAT/CAT offer is not buy-XCH).  Cap is
    // half of 3 XCH + 20M.  Each fee lock under the floor takes a whole 1-XCH
    // coin, so the second crosses the cap; charged the 20M coin first, both
    // were admitted.
    const std::vector<Mojo> coins = {20 * kM, kXch, kXch, kXch};
    CoinLockLedger ledger(coins, 0, 0.5);
    EXPECT_TRUE(ledger.try_lock(0, 10 * kM, 100 * kM));
    EXPECT_FALSE(ledger.try_lock(0, 10 * kM, 100 * kM));
    EXPECT_EQ(ledger.committed(), kXch);
    EXPECT_EQ(ledger.remaining(), 2 * kXch + 20 * kM);
}

TEST(CoinLockLedgerMinCoinTest, TheKnapsackUsesOnlyCoinsTheWalletCanSee) {
    // Need 10M, floor 5.5M -- a floor UNDER the need, which is why this test
    // is also the counter-example to "the effect needs floor > fee" [review
    // #162, round 3]: chia filters before it picks a branch, so excluding a
    // sub-fee coin flips the branch by itself.
    // The sub-need coins the wallet can see are {6M}:
    // not enough to knapsack, so it takes the smallest covering coin.  The
    // two 5M coins are below the floor and must not complete the sum.
    const std::vector<Mojo> coins = {5 * kM, 5 * kM, 6 * kM, kXch};
    CoinLockLedger ledger(coins, 0, 1.0);
    ASSERT_TRUE(ledger.try_lock_floor_only(0, 10 * kM, 5 * kM + kM / 2));
    EXPECT_EQ(ledger.remaining(), 16 * kM);

    // And when the visible sub-need coins DO cover the need, they are used,
    // largest first, as before: {6M, 7M} of {3M, 6M, 7M}.
    CoinLockLedger knapsack({3 * kM, 6 * kM, 7 * kM, kXch}, 0, 1.0);
    ASSERT_TRUE(knapsack.try_lock_floor_only(0, 10 * kM, 4 * kM));
    EXPECT_EQ(knapsack.remaining(), 3 * kM + kXch);
}

TEST(CoinLockLedgerMinCoinTest, AnExactMatchBelowTheFloorIsNotAvailable) {
    // The wallet looks for its exact one-coin match among the FILTERED coins.
    CoinLockLedger ledger({10 * kM, 50 * kM, kXch}, 0, 1.0);
    ASSERT_TRUE(ledger.try_lock_floor_only(0, 10 * kM, 20 * kM));
    EXPECT_EQ(ledger.remaining(), 10 * kM + kXch);
}

TEST(CoinLockLedgerMinCoinTest, ACoinExactlyAtTheFloorIsEligible) {
    // chia's filter is inclusive: min_coin_amount <= coin.amount.
    CoinLockLedger ledger({20 * kM, kXch}, 0, 1.0);
    ASSERT_TRUE(ledger.try_lock_floor_only(0, 10 * kM, 20 * kM));
    EXPECT_EQ(ledger.remaining(), kXch);
}

TEST(CoinLockLedgerMinCoinTest, WhenTheFloorLeavesTooLittleTheRetryIsModelled) {
    // No coin at or above the floor: the wallet refuses, and OfferManager
    // re-sends the create once WITHOUT the floor.  That create uses the
    // default selection, so that is what the ledger charges -- refusing here
    // would drop an offer the wallet goes on to fund.
    CoinLockLedger ledger({20 * kM, 30 * kM}, 0, 1.0);
    ASSERT_TRUE(ledger.try_lock_floor_only(0, 10 * kM, 100 * kM));
    EXPECT_EQ(ledger.remaining(), 30 * kM);
}

TEST(CoinLockLedgerMinCoinTest, AFloorNoCoinFallsUnderChangesNothing) {
    // A floor of 0, a negative one, one below every coin and one EQUAL to the
    // smallest coin all leave every coin eligible, so each must reproduce the
    // two-argument calls step for step.  The 3M coin is part of the first
    // fee selection (3M + 7M is the 10M need exactly), so a filter that
    // dropped it would change the trace.  That XCH-funded offers, which send
    // no floor, select as before is pinned by the older tests in this file:
    // they all use the two-argument calls and assert exact traces.
    const std::vector<Mojo> coins = {3 * kM, 7 * kM, 40 * kM,
                                     kXch, 2 * kXch, 2 * kXch};
    for (Mojo min_coin : {Mojo{0}, Mojo{-5}, Mojo{1}, 3 * kM}) {
        CoinLockLedger reference(coins, kXch / 2, 0.75);
        CoinLockLedger subject(coins, kXch / 2, 0.75);
        for (int i = 0; i < 6; ++i) {
            const Mojo principal = (i % 2 == 0) ? kXch : Mojo{0};
            EXPECT_EQ(subject.try_lock(principal, 10 * kM, min_coin),
                      reference.try_lock(principal, 10 * kM))
                << "min_coin " << min_coin << " step " << i;
            EXPECT_EQ(subject.try_lock_floor_only(0, 10 * kM, min_coin),
                      reference.try_lock_floor_only(0, 10 * kM))
                << "min_coin " << min_coin << " step " << i;
            EXPECT_EQ(subject.remaining(), reference.remaining())
                << "min_coin " << min_coin << " step " << i;
            EXPECT_EQ(subject.committed(), reference.committed())
                << "min_coin " << min_coin << " step " << i;
        }
    }
}

}  // namespace
