#include <gtest/gtest.h>

#include <xop/execution/coin_lock_ledger.hpp>

#include <vector>

namespace {

using xop::Mojo;
using xop::execution::CoinLockLedger;

constexpr Mojo kXch = 1'000'000'000'000LL;
constexpr Mojo kFee = 28'922;  // the live per-offer fee from the incident

TEST(CoinLockLedgerTest, ReplaysTheIncidentBatchAndStopsAboveTheFloor) {
    // [XCH-LOCK-LEDGER 2026-08-23] 2026-08-23 13:48:58Z: spendable was
    // 14.59 XCH in ~2-XCH coins; the engine posted 5 one-XCH asks and 5
    // CAT-principal bids (fee-coin locks only) and hit spendable=0 in 73
    // seconds because every stale wallet re-query approved the next offer.
    // The self-accounted ledger admits the asks, models the bids' whole-coin
    // fee locks, and refuses before the floor is breached.
    std::vector<Mojo> coins(7, 2 * kXch);
    coins.push_back(kXch * 59 / 100);  // the 0.59 XCH tail coin
    CoinLockLedger ledger(coins, /*floor=*/kXch / 2, /*commit_frac=*/1.0);

    int admitted = 0;
    for (int i = 0; i < 5; ++i) {          // 1-XCH asks
        if (ledger.try_lock(kXch, kFee)) ++admitted;
    }
    for (int i = 0; i < 5; ++i) {          // CAT bids: fee-coin lock only
        if (ledger.try_lock(0, kFee)) ++admitted;
    }

    EXPECT_LT(admitted, 10);               // the incident admitted all 10
    EXPECT_GE(ledger.remaining(), kXch / 2);
}

TEST(CoinLockLedgerTest, CycleCapLimitsCommitmentFraction) {
    // frac 0.5 of (14 - 0.5) leaves ~6.75 XCH commitable: three 2-XCH coin
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

TEST(CoinLockLedgerTest, AccumulatesLargestFirstWhenNoSingleCoinCovers) {
    std::vector<Mojo> coins = {2 * kXch, 2 * kXch, 2 * kXch};
    CoinLockLedger ledger(coins, 0, 1.0);

    ASSERT_TRUE(ledger.try_lock(5 * kXch, 0));  // needs three coins
    EXPECT_EQ(ledger.committed(), 6 * kXch);
    EXPECT_FALSE(ledger.try_lock(kXch, 0));     // pool is empty
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

TEST(CoinLockLedgerTest, FeeOnlyLocksModelWholeCoins) {
    // A CAT/CAT offer spends no XCH principal, but its 28,922-mojo fee
    // still locks a whole coin when nothing smaller exists.
    std::vector<Mojo> coins = {2 * kXch, 2 * kXch};
    CoinLockLedger ledger(coins, kXch / 2, 1.0);

    ASSERT_TRUE(ledger.try_lock(0, kFee));
    EXPECT_EQ(ledger.committed(), 2 * kXch);
    EXPECT_FALSE(ledger.try_lock(0, kFee));  // second lock crosses the cap
}

TEST(CoinLockLedgerTest, NoteLockChargesUnconditionally) {
    // Cancel fees are charged with note_lock: they happen regardless of
    // budget, so the pool must reflect them even past the floor -- and a
    // later try_lock is then correctly refused.
    std::vector<Mojo> coins = {2 * kXch, 2 * kXch};
    CoinLockLedger ledger(coins, kXch / 2, 1.0);

    ledger.note_lock(0, kFee);               // cancel fee locks a whole coin
    EXPECT_EQ(ledger.committed(), 2 * kXch);
    ledger.note_lock(0, kFee);               // second cancel: past the cap,
    EXPECT_EQ(ledger.remaining(), 0);        // charged anyway
    EXPECT_FALSE(ledger.try_lock(0, kFee));  // posting is now refused
}

TEST(CoinLockLedgerTest, NoteLockOnInactiveLedgerIsANoOp) {
    CoinLockLedger ledger;
    ledger.note_lock(kXch, kFee);
    EXPECT_EQ(ledger.committed(), 0);
}

TEST(CoinLockLedgerTest, InactiveLedgerAdmitsEverything) {
    CoinLockLedger ledger;
    EXPECT_FALSE(ledger.active());
    EXPECT_TRUE(ledger.try_lock(1'000 * kXch, kFee));
}

TEST(CoinLockLedgerTest, RefusalLocksNothing) {
    std::vector<Mojo> coins = {2 * kXch};
    CoinLockLedger ledger(coins, kXch, 1.0);

    EXPECT_FALSE(ledger.try_lock(2 * kXch, 0));  // cap/floor breach
    EXPECT_EQ(ledger.committed(), 0);
    EXPECT_EQ(ledger.remaining(), 2 * kXch);
    // The refused coins are still available for a smaller, legal lock.
    EXPECT_TRUE(ledger.try_lock(0, 0));          // zero-need no-op admits
}

}  // namespace
