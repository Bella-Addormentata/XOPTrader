// ---------------------------------------------------------------------------
// [S41] get_spendable_coins() failed OPEN: an RPC error returned the same
// empty vector a genuinely empty wallet returns.
//
// The eleventh member of this repo's documented close_out fail-open family --
// an error path returning the identical value to a valid negative answer.
//
// What was measured, across logs/xop_trader.log and .1-.5:
//
//     get_spendable_coins failed ......... 68
//     "Have 0 mojos total" ............... 34
//     XCH split failed ................... 34
//     count_free_coins failed (handler) ... 0
//
// 68 is twice 34 because both reads in a cycle failed: the engine's
// count_free_coins and then ensure_split's own re-read. The cause was always
// the same application error, "Wallet needs to be fully synced before getting
// all coins" -- and three minutes later the SAME PROCESS logged "33 free
// coins, 58996979793936 mojos free". The wallet held ~59 XCH while the log
// asserted "Have 0 mojos total". A bot that reports insolvency because an RPC
// failed is an operational hazard before any spend is considered, and the
// zero-count reading then authorised a coin split on the strength of it.
//
// WHY THE RETURN TYPE ALONE IS NOT THE FIX. The engine's handler was itself
// fail-open:
//
//     int free_count = 0;
//     try   { free_count = co_await count_free_coins(1); }
//     catch (const std::exception& e) { warn(...); }   // FALLS THROUGH
//     if (free_count >= target) { OK } else { ensure_split(); }  // SPLITS
//
// Making CoinManager throw would have made that handler reachable and it
// would still have split, because free_count keeps its zero. A mechanical
// port to maybe.value_or(0) preserves the defect verbatim. So the policy was
// moved into a pure header where a test can hold it: nothing in cpp/tests
// constructs an Engine (TODO S36), ChiaWalletRPC is final with non-virtual
// methods, and there is no gmock in this repo -- meaning the RPC failure path
// cannot be driven from a unit test at either end. These tests plus the
// signature assertions below are the entire regression surface.
//
// THE RULE: a number may never be substituted for a failed read. Unknown is
// its own state, it is the DEFAULT state, and it authorises nothing.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include "xop/execution/coin_manager.hpp"
#include "xop/execution/coin_pool_verdict.hpp"

using xop::execution::CoinInfo;
using xop::execution::CoinManager;
using xop::execution::CoinPoolAction;
using xop::execution::CoinPoolReading;
using xop::execution::decide_coin_pool_action;
using xop::execution::SplitResult;

namespace asio = boost::asio;

// ---------------------------------------------------------------------------
// The boundary is representable at all -- checked at COMPILE time.
//
// These are the mutation guards for the signature itself. Revert either
// method to a bare count/vector and the build goes red here, which is the
// only place in the repo that can notice: no test can reach the RPC.
// ---------------------------------------------------------------------------

static_assert(
    std::is_same_v<
        decltype(&CoinManager::get_spendable_coins),
        asio::awaitable<std::optional<std::vector<CoinInfo>>> (CoinManager::*)(
            std::int64_t)>,
    "get_spendable_coins must be able to say 'the read did not happen'. "
    "Returning a bare vector makes an RPC failure indistinguishable from an "
    "empty wallet -- that is S41.");

static_assert(
    std::is_same_v<decltype(&CoinManager::count_free_coins),
                   asio::awaitable<std::optional<int>> (CoinManager::*)(
                       std::int64_t)>,
    "count_free_coins must propagate 'I could not count'. The number 0 is not "
    "an answer to a question that was never asked.");

// ---------------------------------------------------------------------------
// std::nullopt and an empty vector are different values.
//
// Stated explicitly because this is the whole bug: before the fix these two
// states were the SAME value, and every coin-pool sizing decision -- the free
// count, the pool-ready count, ensure_split's balance sum -- read the failure
// as "the wallet is empty".
// ---------------------------------------------------------------------------

TEST(CoinPoolVerdict, AFailedReadIsNotAnEmptyWallet)
{
    const std::optional<std::vector<CoinInfo>> read_failed = std::nullopt;
    const std::optional<std::vector<CoinInfo>> wallet_empty{
        std::vector<CoinInfo>{}};

    ASSERT_FALSE(read_failed.has_value());
    ASSERT_TRUE(wallet_empty.has_value());
    EXPECT_TRUE(wallet_empty->empty());
    EXPECT_NE(read_failed.has_value(), wallet_empty.has_value());

    // And they must map to different verdicts, which is the point of the
    // distinction existing at all.
    EXPECT_EQ(decide_coin_pool_action({false, 0}, 10, false),
              CoinPoolAction::Skip);
    EXPECT_EQ(decide_coin_pool_action({true, 0}, 10, false),
              CoinPoolAction::Split);
}

// ---------------------------------------------------------------------------
// The verdict itself
// ---------------------------------------------------------------------------

TEST(CoinPoolVerdict, ReadFailureNeverAuthorisesASplit)
{
    // The matrix matters. A single row with target_count == 0 would pass
    // against a helper that merely reordered the clauses, and reordering is
    // exactly the mutation that reinstates S41.
    const int targets[] = {0, 1, 10, 1000};
    const int counts[]  = {0, 1, 10, INT_MAX};

    for (const int target : targets) {
        for (const int count : counts) {
            for (const bool pending : {false, true}) {
                const CoinPoolReading unknown{false, count};
                EXPECT_EQ(decide_coin_pool_action(unknown, target, pending),
                          CoinPoolAction::Skip)
                    << "an unread pool authorised something: target=" << target
                    << " count=" << count << " pending=" << pending;
            }
        }
    }
}

TEST(CoinPoolVerdict, TheLiteralS41StateSkips)
{
    // pool_ready_count == 0 with read_ok == false IS the production state:
    // the RPC threw, the empty vector counted zero, and zero was below the
    // target of 10, so a split was authorised on a wallet holding ~59 XCH.
    EXPECT_EQ(decide_coin_pool_action({false, 0}, 10, false),
              CoinPoolAction::Skip);
}

TEST(CoinPoolVerdict, GenuinelyEmptyWalletStillSplits)
{
    // Without this, the test above passes vacuously against a helper that
    // returns Skip unconditionally -- the failure mode the mutation-check
    // memory names by name (4 of ~12 tests once passed with the bug back in).
    EXPECT_EQ(decide_coin_pool_action({true, 0}, 10, false),
              CoinPoolAction::Split);
    EXPECT_EQ(decide_coin_pool_action({true, 9}, 10, false),
              CoinPoolAction::Split);
}

TEST(CoinPoolVerdict, HealthyPoolIsSatisfied)
{
    // The literal recovery reading three minutes after the phantom-empty
    // cycle: 31 pool-ready coins against a target of 10.
    EXPECT_EQ(decide_coin_pool_action({true, 31}, 10, false),
              CoinPoolAction::Satisfied);
    EXPECT_EQ(decide_coin_pool_action({true, 10}, 10, false),
              CoinPoolAction::Satisfied);
}

TEST(CoinPoolVerdict, PendingSplitSkipsEvenWhenBelowTarget)
{
    // A count taken while a prior split is still confirming is stale by
    // construction; splitting again would double-spend the pool's intent.
    EXPECT_EQ(decide_coin_pool_action({true, 0}, 10, true),
              CoinPoolAction::Skip);
}

TEST(CoinPoolVerdict, DisabledPoolNeitherSplitsNorReportsUnknown)
{
    // target_count <= 0 means the pool is switched off. This is checked AFTER
    // read_ok on purpose: a disabled pool with an unknown reading is still
    // Skip, so no path reaches Satisfied without a successful read.
    EXPECT_EQ(decide_coin_pool_action({true, 0}, 0, false),
              CoinPoolAction::Satisfied);
    EXPECT_EQ(decide_coin_pool_action({false, 0}, 0, false),
              CoinPoolAction::Skip);
}

TEST(CoinPoolVerdict, ADefaultConstructedReadingIsUnknownAndSpendsNothing)
{
    // read_ok{false} is the inverse of engine.cpp's old `int free_count = 0`:
    // a forgotten or half-initialised reading fails CLOSED.
    const CoinPoolReading fresh;
    EXPECT_FALSE(fresh.read_ok);
    EXPECT_EQ(decide_coin_pool_action(fresh, 10, false), CoinPoolAction::Skip);

    // Also true at compile time, so the default cannot be flipped quietly.
    static_assert(decide_coin_pool_action(CoinPoolReading{}, 10, false)
                      == CoinPoolAction::Skip,
                  "an unknown pool reading must authorise nothing");
}

TEST(CoinPoolVerdict, SplitIsReachableOnlyFromASuccessfulRead)
{
    // The reachability argument for the one residue no test can cover: the
    // engine call site itself. If Split never issues from an unread pool over
    // the whole grid, the only way the engine can spend is behind read_ok.
    for (int target = -2; target <= 40; ++target) {
        for (int count = -2; count <= 40; ++count) {
            for (const bool pending : {false, true}) {
                if (decide_coin_pool_action({false, count}, target, pending)
                        == CoinPoolAction::Split) {
                    FAIL() << "Split issued from an unread pool: target="
                           << target << " count=" << count;
                }
            }
        }
    }
    // Not vacuous: the same grid does produce splits when the read succeeded.
    EXPECT_EQ(decide_coin_pool_action({true, 0}, 40, false),
              CoinPoolAction::Split);
}

// ---------------------------------------------------------------------------
// ensure_split's result must distinguish the two failures
// ---------------------------------------------------------------------------

TEST(CoinPoolVerdict, SplitResultDistinguishesAFailedReadFromAnEmptyWallet)
{
    // "insufficient balance ... Have 0 mojos total" is a claim about the
    // wallet. A function whose enumeration failed never read the wallet and
    // is not entitled to make it. These two must not be the same value.
    const SplitResult read_failed{.success = false, .read_failed = true};
    const SplitResult insufficient{.success = false};

    EXPECT_FALSE(read_failed.success);
    EXPECT_FALSE(insufficient.success);
    EXPECT_TRUE(read_failed.read_failed);
    EXPECT_FALSE(insufficient.read_failed);
    EXPECT_NE(read_failed.read_failed, insufficient.read_failed);

    // The default is the ordinary failure, not the exotic one: only the code
    // that actually observed a failed read may claim it.
    const SplitResult fresh;
    EXPECT_FALSE(fresh.success);
    EXPECT_FALSE(fresh.read_failed);
    EXPECT_EQ(fresh.coins_created, 0);
}
