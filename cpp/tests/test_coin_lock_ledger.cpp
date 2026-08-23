#include <gtest/gtest.h>

#include <xop/execution/coin_lock_ledger.hpp>
#include <xop/execution/offer_manager.hpp>

#include <nlohmann/json.hpp>

#include <vector>

namespace {

using xop::Mojo;
using xop::execution::CoinLockLedger;
using xop::execution::OfferManager;

constexpr Mojo kXch = 1'000'000'000'000LL;
constexpr Mojo kFee = 28'922;  // the live per-offer fee from the incident

TEST(CoinLockLedgerTest, ReplaysTheIncidentBatchExactly) {
    // [XCH-LOCK-LEDGER 2026-08-23] 2026-08-23 13:48:58Z: spendable was
    // 14.59 XCH in ~2-XCH coins; the engine posted 5 one-XCH asks and 5
    // CAT-principal bids (fee-coin locks only) and hit spendable=0 in 73
    // seconds because every stale wallet re-query approved the next offer.
    // Exact modeled trace (review: pin equalities, not inequalities, so
    // cap-arithmetic drift cannot hide): 5 asks lock a 2-XCH coin each;
    // bid 1 locks the 0.59 tail coin; bid 2 locks a 2-XCH coin
    // (committed 12.59); bids 3-5 refused at the 14.09 cap.  7 admitted,
    // exactly 2 XCH remaining -- production admitted all 10 and hit zero.
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

TEST(CoinLockLedgerTest, AccumulationOrderIsSmallestFirstAndPinned) {
    // Distinguishable coins so a selection-order mutation fails (review:
    // three equal coins could not tell largest-first from smallest-first).
    // Need 5 against {1, 2, 4}: no single coin covers; smallest-first
    // accumulation locks all three (7); largest-first would lock {4, 2}
    // (6, remaining 1) -- the equalities below kill that mutant.
    std::vector<Mojo> coins = {kXch, 2 * kXch, 4 * kXch};
    CoinLockLedger ledger(coins, 0, 1.0);

    ASSERT_TRUE(ledger.try_lock(5 * kXch, 0));
    EXPECT_EQ(ledger.committed(), 7 * kXch);
    EXPECT_EQ(ledger.remaining(), 0);
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

}  // namespace
