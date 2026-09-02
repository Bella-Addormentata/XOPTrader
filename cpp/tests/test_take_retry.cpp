// ---------------------------------------------------------------------------
// [S40 remainder] The crossed-book taker re-attempted an offer it could never
// fund, 50 times, and told the log the same thing 150 times while doing it.
//
// WHAT WAS MEASURED, before any of this existed
// ---------------------------------------------
// logs/xop_trader.log + .1-.5, 2026-08-31T05:15 -> 2026-09-01T20:30 (39h15m),
// read with grep/sed; data/xop_trader.db opened mode=ro.
//
//   * Step 9c reached TAKING 105 times and logged TOOK once. 104 failures:
//       96  "insufficient funds in wallet 8"
//        8  "Wallet needs to be fully synced before making transactions."
//     186 "insufficient" error lines across all steps in the ~37h the TODO
//     counted, plus 94 wallet-sync warnings.
//
//   * The storm. Offer `Cgp9GrtmnmPf` on XCH/DBX: 50 attempts between
//     13:07:22.215 and 13:53:16.732 on 08-31 -- 45m54.5s, min gap 10.1s,
//     median 13.1s -- of which 45 failed on funding and 5 on wallet sync.
//     Its ask was 97181600000000 on ALL FIFTY detections, byte-identical;
//     only the bid moved, so the logged edge drifted 235.2 -> 39.0 -> 14.4 bps
//     while the offer itself never changed once. It cost 485,908 DBX mojos.
//     DBX spendable during the storm was 134778 / 236551 / 336775 / 338276 --
//     short by 30.4% at the best moment and 72% at the worst. Over the WHOLE
//     39h window DBX spendable never left 111,214 .. 437,875, and not one of
//     the 96 funding failures was ever fundable.
//
//   * A second storm of the same shape: `GxTYZbaujFog`, 36 attempts in
//     15m21s, with bid, ask, edge and size byte-identical throughout.
//
//   * The sync failures are a FLAP, not a condition. 106 sync-gate hits with
//     unsynced_blocks {1:93, 2:4, 3:2, 4:2, 5:1, 6:1, 7:1, 8:1, 9:1} -- 89 of
//     the 94 the TODO counted sat at 1/20 and the max was 4/20 -- grouping
//     into 92 episodes over 35.2h, one every 23.0 min, 87 of them exactly one
//     cycle long (median 0s, mean 2.7s, max 155.3s).
//
// THE DISTINCTION THESE TESTS EXIST TO ENFORCE
// --------------------------------------------
// A funding failure on an offer whose size and price have not changed is
// DETERMINISTIC: take_offer has no size parameter, a Chia offer is atomic, so
// the only mutable input to `spendable >= cost` is OUR BALANCE. A sync
// rejection is TRANSIENT and clears on its own in one cycle. A policy that
// gives them the same schedule is wrong in one direction or the other: share
// the deterministic schedule and the flap suppresses a fundable take every 23
// minutes; share the transient one and the storm comes straight back.
//
// [review 2026-09-01] SEPARATE, not shared and not absent. The first cut read
// "not the same schedule" as "no schedule at all" for the transient class, on
// the strength of a claim about get_wallet_balance that the logs refute (zero
// balance-read failures in 39h; the "fully synced" text belongs to
// get_spendable_coins and create_offer). With the gate returning Attempt on
// every cycle of a desync, the storm reproduced verbatim on that class --
// APersistentDesyncCannotStorm is the test that would have caught it.
//
// Also pinned here:
//   * the UNKNOWN resolution of the pre-trade check (decline, distinctly, and
//     accrue nothing) -- see PinsTheUnknownResolution;
//   * that suppression clears the moment the balance rises, with no timer;
//   * that the state map cannot grow without bound;
//   * that the cost is QUOTE-denominated. Writing `cost = take_size` at the
//     call site compares 5e12 base mojos against a 338,276-mojo DBX wallet and
//     declines every crossed-book take forever while looking like a working
//     guard. That is the highest-value mutation in this file.
//
// WHAT THESE TESTS DO NOT COVER. Nothing in cpp/tests constructs an Engine
// (TODO S36), so the Step 9c/9e/9f call sites are unguarded lines: delete the
// wiring and this suite stays green. These tests hold the POLICY and the
// ARITHMETIC, which is the part that can be held.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#include "xop/execution/take_retry.hpp"

using xop::Mojo;
using xop::execution::add_same_wallet_fee;
using xop::execution::ask_take_cost;
using xop::execution::classify_take_failure;
using xop::execution::decide_funding;
using xop::execution::FundingVerdict;
using xop::execution::funding_hold_blocks;
using xop::execution::OfferFingerprint;
using xop::execution::other_backoff_blocks;
using xop::execution::transient_backoff_blocks;
using xop::execution::SpendableReading;
using xop::execution::TakeDecision;
using xop::execution::TakeFailureClass;
using xop::execution::TakeGate;
using xop::execution::TakeLogEvent;
using xop::execution::TakeRetryBook;
using xop::execution::TakeRetryConfig;

namespace {

// -- the live numbers -------------------------------------------------------
constexpr std::int64_t kXchDenom = 1'000'000'000'000LL;   // XCH base
constexpr std::int64_t kCatDenom = 1000LL;                // DBX / BYC quote

constexpr char kPair[]  = "XCH/DBX";
constexpr char kStorm[] = "Cgp9GrtmnmPf";   // 50 attempts, never fundable
constexpr char kFrozen[] = "GxTYZbaujFog";  // 36 attempts, book frozen 15m

constexpr Mojo kStormSize  = 5'000'000'000'000LL;    // exactly the 5.0 XCH cap
constexpr Mojo kStormAsk   = 97'181'600'000'000LL;   // invariant over 50 looks
constexpr Mojo kStormCost  = 485'908LL;              // DBX mojos
constexpr Mojo kBestBal    = 338'276LL;              // best DBX spendable seen
                                                     // during the storm

// The one take that ever succeeded: 2026-08-31T13:47:43, 1.29 XCH @ 14.4 bps,
// taker_fills.id 607. Sub-cap, and the only attempt whose logged size was not
// exactly the cap.
constexpr Mojo kGoodSize = 1'289'726'060'241LL;
constexpr Mojo kGoodAsk  = 84'570'142'857'143LL;
constexpr Mojo kGoodCost = 109'073LL;
constexpr Mojo kLowBal   = 111'214LL;   // lowest DBX spendable in the window

const OfferFingerprint kStormFp{kStormAsk, kStormSize};

// Exact live error text.
constexpr char kFundsMsg[] = "insufficient funds in wallet 8";
constexpr char kSyncMsg[]  =
    "Wallet needs to be fully synced before making transactions.";

SpendableReading known(Mojo m) { return SpendableReading{true, m}; }
SpendableReading unknown()     { return SpendableReading{}; }   // read_ok=false

TakeRetryConfig live_cfg()
{
    return TakeRetryConfig{};   // the shipped defaults
}

}  // namespace

// ===========================================================================
// 1. Classification. `Other` is the default; an unrecognised message must
//    never land in Funding, because Funding is the class that suppresses.
// ===========================================================================

TEST(TakeRetryClassify, LiveMessagesLandInTheRightClass)
{
    EXPECT_EQ(classify_take_failure(kFundsMsg), TakeFailureClass::Funding);
    EXPECT_EQ(classify_take_failure(kSyncMsg),  TakeFailureClass::Unsynced);

    // The repo's existing classifier (offer_manager.cpp, emergency cancel)
    // also matches this phrasing; both needles are carried over.
    EXPECT_EQ(classify_take_failure("not enough spendable balance"),
              TakeFailureClass::Funding);
}

TEST(TakeRetryClassify, UnrecognisedIsOtherAndNeverFunding)
{
    for (const char* msg : {"",
                            "connection reset by peer",
                            "dexie: 502 Bad Gateway",
                            "trade record not found",
                            "some brand new wallet error nobody has seen"}) {
        const auto c = classify_take_failure(msg);
        EXPECT_EQ(c, TakeFailureClass::Other) << msg;
        EXPECT_NE(c, TakeFailureClass::Funding) << msg;
    }
}

TEST(TakeRetryClassify, CaseAndSurroundingTextDoNotMatter)
{
    EXPECT_EQ(classify_take_failure("Insufficient Funds in wallet 8"),
              TakeFailureClass::Funding);
    EXPECT_EQ(classify_take_failure(
                  "[Engine] Step 9c: XCH/DBX crossed-book take failed: "
                  "Wallet needs to be fully synced before making "
                  "transactions."),
              TakeFailureClass::Unsynced);
}

// ===========================================================================
// 2. THE UNKNOWN RESOLUTION. This is the test the S41 family exists for.
//
//    We resolve UNKNOWN as DECLINE, and as its OWN state -- not as "cannot
//    afford" and not as "can afford". Declining is the fail-closed choice and
//    the evidence makes it the cheap one: a thrown get_wallet_balance is
//    overwhelmingly the same "not fully synced" condition, and a take
//    submitted to an unsynced wallet was rejected 8 times out of 8. Cost of
//    declining: one cycle, ~13s, against 87-of-92 sync episodes that lasted
//    exactly one cycle. Cost of proceeding: two RPCs and a certain rejection.
//
//    Being its own state matters as much as declining: unknown must accrue
//    NOTHING, or the flap starts arming the deterministic hold.
// ===========================================================================

TEST(TakeRetryFunding, PinsTheUnknownResolution)
{
    // A failed read is Unknown -- NOT Insufficient, NOT Fund -- whatever the
    // stale number in the struct happens to be.
    EXPECT_EQ(decide_funding(unknown(), kStormCost), FundingVerdict::Unknown);
    EXPECT_EQ(decide_funding(SpendableReading{false, 999'999'999LL},
                             kStormCost),
              FundingVerdict::Unknown);

    // A cost we could not compute is Unknown, not "free". quote_cost_for_ask
    // returns 0 for anything it cannot price; reading that as Fund would send
    // an unpriced take.
    EXPECT_EQ(decide_funding(known(kBestBal), 0), FundingVerdict::Unknown);
    EXPECT_EQ(decide_funding(known(kBestBal), -1), FundingVerdict::Unknown);

    // A negative balance is a malformed response, not a debt.
    EXPECT_EQ(decide_funding(SpendableReading{true, -5}, kStormCost),
              FundingVerdict::Unknown);

    // And the two knowable answers, for contrast.
    EXPECT_EQ(decide_funding(known(kBestBal), kStormCost),
              FundingVerdict::Insufficient);
    EXPECT_EQ(decide_funding(known(kStormCost), kStormCost),
              FundingVerdict::Fund);   // exactly enough is enough
}

TEST(TakeRetryGate, UnknownDeclinesDistinctlyAndAccruesNothing)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();

    // 40 consecutive cycles with an unreadable balance: every one declines,
    // and declines as DeclineUnknown -- a distinct observable from
    // DeclineInsufficient, so the log can tell a broken read from a real
    // shortfall.
    for (std::uint64_t b = 100; b < 140; ++b) {
        const auto d = book.gate(kPair, kStorm, kStormFp, unknown(),
                                 kStormCost, b, cfg);
        EXPECT_EQ(d.gate, TakeGate::DeclineUnknown) << "block " << b;
        EXPECT_EQ(d.shortfall, 0) << "an unknown balance has no shortfall";
    }

    // Nothing accrued. The moment the read works and the balance covers the
    // cost we take -- 40 unknown cycles bought no hold-off at all.
    const auto* e = book.find(kPair, kStorm);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->rpc_funding_failures, 0u);
    EXPECT_EQ(e->other_failures, 0u);
    EXPECT_EQ(e->transient_failures, 0u);

    const auto ok = book.gate(kPair, kStorm, kStormFp, known(kStormCost),
                              kStormCost, 140, cfg);
    EXPECT_EQ(ok.gate, TakeGate::Attempt);
    EXPECT_EQ(ok.log, TakeLogEvent::Resume);
}

TEST(TakeRetryGate, DefaultConstructedDecisionDeclines)
{
    // Zero enumerator = decline, same discipline as CrossedBookDecision's
    // NoBook and CoinPoolReading's read_ok{false}. A half-initialised decision
    // must never authorise a spend.
    const xop::execution::TakeDecision d{};
    EXPECT_EQ(d.gate, TakeGate::DeclineUnknown);
    EXPECT_NE(d.gate, TakeGate::Attempt);
    EXPECT_EQ(static_cast<int>(TakeGate::DeclineUnknown), 0);
    EXPECT_EQ(static_cast<int>(TakeFailureClass::Other), 0);
}

// ===========================================================================
// 3. THE SCHEDULES MUST NOT BE SHARED.
//
//    This is the test that fails if a transient rejection is ever routed into
//    the deterministic hold, or vice versa. It is the single most important
//    test in the file: 92 sync episodes in 35.2h means a shared schedule
//    suppresses a fundable take roughly every 23 minutes, invisibly.
// ===========================================================================

TEST(TakeRetrySchedule, TransientAndDeterministicHaveSeparateSchedules)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();
    const OfferFingerprint fp{kGoodAsk, kGoodSize};
    const auto rich = known(kGoodCost * 10);   // affordable throughout

    // THE INVARIANT: three classes, three schedules, and a failure of one must
    // never arm another. The transient schedule is an order of magnitude
    // shorter than the deterministic one, and both are shorter than the log
    // heartbeat.
    ASSERT_LT(cfg.transient_backoff_max_blocks, cfg.funding_recheck_blocks * 8);
    ASSERT_LT(cfg.transient_backoff_base_blocks, cfg.funding_recheck_blocks);

    // A single-cycle sync flap -- 87 of the 92 observed episodes.
    EXPECT_EQ(book.gate(kPair, "sync-flap", fp, rich, kGoodCost, 100, cfg).gate,
              TakeGate::Attempt);
    book.note_failure(kPair, "sync-flap", fp, classify_take_failure(kSyncMsg),
                      rich, 100, cfg);

    // It costs exactly transient_backoff_base_blocks on THIS offer, under its
    // own enumerator so the log cannot call it a balance problem.
    const auto held = book.gate(kPair, "sync-flap", fp, rich, kGoodCost,
                                101, cfg);
    EXPECT_EQ(held.gate, TakeGate::DeclineSyncBackoff);
    EXPECT_EQ(held.shortfall, 0);
    EXPECT_EQ(held.ready_block, 100u + cfg.transient_backoff_base_blocks);
    EXPECT_EQ(book.gate(kPair, "sync-flap", fp, rich, kGoodCost,
                        100 + cfg.transient_backoff_base_blocks, cfg).gate,
              TakeGate::Attempt)
        << "a one-cycle flap must cost one short hold, not a deterministic one";

    // And the flap arms NOTHING deterministic. If any of these is non-zero the
    // schedules have been merged and a fundable take is suppressed every ~23
    // minutes, invisibly.
    const auto* te = book.find(kPair, "sync-flap");
    ASSERT_NE(te, nullptr);
    EXPECT_EQ(te->rpc_funding_failures, 0u);
    EXPECT_EQ(te->funding_ready_block, 0u);
    EXPECT_EQ(te->other_failures, 0u);
    EXPECT_EQ(te->other_ready_block, 0u);

    // Contrast 1: a DETERMINISTIC funding rejection on the same fingerprint
    // holds far longer, and under a DIFFERENT enumerator (the coin-lock-lag
    // case -- our own just-posted offers lock whole UTXOs that stay in
    // spendable_balance for tens of seconds).
    book.note_failure(kPair, "funded-but-no", fp,
                      classify_take_failure(kFundsMsg), rich, 100, cfg);
    const auto fh = book.gate(kPair, "funded-but-no", fp, rich,
                              kGoodCost, 101, cfg);
    EXPECT_EQ(fh.gate, TakeGate::DeclineFundingHold);
    EXPECT_GT(fh.ready_block, held.ready_block);

    // Contrast 2: an UNMODELLED failure gets its own small generic backoff,
    // which is neither of the above.
    book.note_failure(kPair, "who-knows", fp,
                      classify_take_failure("dexie: 502 Bad Gateway"),
                      rich, 100, cfg);
    EXPECT_EQ(book.gate(kPair, "who-knows", fp, rich, kGoodCost, 101, cfg).gate,
              TakeGate::DeclineBackoff);
    EXPECT_EQ(book.gate(kPair, "who-knows", fp, rich, kGoodCost,
                        100 + cfg.other_backoff_base_blocks, cfg).gate,
              TakeGate::Attempt);
}

// [review 2026-09-01] The first cut left the transient class with NO bound,
// on the argument that a thrown get_wallet_balance would resolve these to
// DeclineUnknown anyway. The logs refute that -- zero balance-read sync
// failures in 39h -- so on a desync that does not clear, the gate said Attempt
// on every cycle and the 50-attempt storm reproduced verbatim on this class:
// 1 Dexie GET + 1 take_offer + 1 spdlog::error per cycle, unbounded.
TEST(TakeRetrySchedule, APersistentDesyncCannotStorm)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();
    const OfferFingerprint fp{kGoodAsk, kGoodSize};
    const auto rich = known(kGoodCost * 10);   // the balance is FINE throughout

    int attempts = 0;
    for (std::uint64_t b = 0; b < 500; ++b) {
        if (book.gate(kPair, "wedged", fp, rich, kGoodCost, b, cfg).gate
            == TakeGate::Attempt) {
            ++attempts;
            book.note_failure(kPair, "wedged", fp,
                              classify_take_failure(kSyncMsg), rich, b, cfg);
        }
    }
    // Was 500. Bounded by the doubling schedule to one attempt per
    // transient_backoff_max_blocks once it saturates.
    EXPECT_LT(attempts, 30)
        << "an unbounded transient class is the S40 storm on the other axis";
    // But NOT a blacklist. It must keep trying, because the sync condition
    // clears on its own and 87 of 92 episodes lasted a single cycle.
    EXPECT_GE(attempts, 500 / static_cast<int>(
                            cfg.transient_backoff_max_blocks));
}

// [review 2026-09-01] Nothing decayed the failure counters: reset_failure_state_
// is reachable only from a fingerprint change and note_success only from a
// settled take, so `other_failures` was cumulative over the entry's whole
// lifetime despite the config field being named `consecutive` and documented
// "doubling per consecutive failure". Five ISOLATED transport blips, 210 blocks
// apart, escalated the hold on a fully funded 235 bps cross to the 64-block
// ceiling -- for a fault that hits every offer on the book at once and has
// nothing to do with any particular offer.
TEST(TakeRetrySchedule, IsolatedFailuresDoNotEscalateButPersistentOnesDo)
{
    const auto cfg = live_cfg();
    const OfferFingerprint fp{kGoodAsk, kGoodSize};
    const auto rich = known(kGoodCost * 10);

    TakeRetryBook isolated;
    for (int ep = 0; ep < 5; ++ep) {
        const std::uint64_t b = 100 + 210 * static_cast<std::uint64_t>(ep);
        EXPECT_EQ(isolated.gate(kPair, "blip", fp, rich, kGoodCost, b,
                                cfg).gate,
                  TakeGate::Attempt)
            << "episode " << ep << ": a quiet offer must not still be held";
        isolated.note_failure(kPair, "blip", fp,
                              classify_take_failure("Connection refused"),
                              rich, b, cfg);
        const auto d = isolated.gate(kPair, "blip", fp, rich, kGoodCost,
                                     b + 1, cfg);
        EXPECT_EQ(d.gate, TakeGate::DeclineBackoff);
        EXPECT_EQ(d.ready_block, b + cfg.other_backoff_base_blocks)
            << "episode " << ep << ": an isolated blip escalated";
        const auto* e = isolated.find(kPair, "blip");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->other_failures, 1u) << "episode " << ep;
    }

    // The contrast, which the decay must NOT weaken: a fault that keeps
    // firing the moment its hold expires still escalates to the ceiling.
    TakeRetryBook persistent;
    std::uint64_t b = 0;
    std::uint32_t last_span = 0;
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(persistent.gate(kPair, "broken", fp, rich, kGoodCost, b,
                                  cfg).gate,
                  TakeGate::Attempt) << "iteration " << i;
        persistent.note_failure(kPair, "broken", fp,
                                classify_take_failure("Connection refused"),
                                rich, b, cfg);
        const auto d = persistent.gate(kPair, "broken", fp, rich, kGoodCost,
                                       b + 1, cfg);
        ASSERT_EQ(d.gate, TakeGate::DeclineBackoff) << "iteration " << i;
        const auto span = static_cast<std::uint32_t>(d.ready_block - b);
        EXPECT_GE(span, last_span) << "iteration " << i;
        last_span = span;
        b = d.ready_block;          // retry the instant the hold expires
    }
    EXPECT_EQ(last_span, cfg.other_backoff_max_blocks);
}

TEST(TakeRetrySchedule, UnmodelledBackoffIsExponentialAndBounded)
{
    const auto cfg = live_cfg();
    EXPECT_EQ(other_backoff_blocks(0, cfg), 0u);
    EXPECT_EQ(other_backoff_blocks(1, cfg), cfg.other_backoff_base_blocks);
    EXPECT_EQ(other_backoff_blocks(2, cfg),
              cfg.other_backoff_base_blocks * 2u);
    EXPECT_EQ(other_backoff_blocks(3, cfg),
              cfg.other_backoff_base_blocks * 4u);
    // Saturates rather than running away. A backoff that grows without bound
    // is a blacklist with extra steps, and §4 of the taxonomy refuted every
    // blacklist key.
    EXPECT_EQ(other_backoff_blocks(1000, cfg), cfg.other_backoff_max_blocks);
    EXPECT_EQ(other_backoff_blocks(4'000'000'000u, cfg),
              cfg.other_backoff_max_blocks);
}

// ===========================================================================
// 4. SUPPRESSION CLEARS WHEN THE INPUT THAT CAUSED IT CHANGES.
//
//    The TODO says the failure "will repeat until our balance or the offer
//    changes". Both are encoded literally, and neither is a timer: a
//    time-keyed backoff would sit out a 235 bps cross on 5 XCH for the
//    duration of the hold-off, and the balance-keyed gate has zero
//    competitive cost by construction.
// ===========================================================================

TEST(TakeRetryClearing, SuppressionClearsTheCycleTheBalanceRises)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();

    // 30 cycles at the best balance ever observed during the storm.
    for (std::uint64_t b = 0; b < 30; ++b) {
        const auto d = book.gate(kPair, kStorm, kStormFp, known(kBestBal),
                                 kStormCost, b, cfg);
        ASSERT_EQ(d.gate, TakeGate::DeclineInsufficient) << "block " << b;
        EXPECT_EQ(d.shortfall, kStormCost - kBestBal);
    }

    // One mojo short: still declines. Exactly enough: takes, immediately, on
    // the very next cycle, with no hold-off of any kind.
    EXPECT_EQ(book.gate(kPair, kStorm, kStormFp, known(kStormCost - 1),
                        kStormCost, 30, cfg).gate,
              TakeGate::DeclineInsufficient);

    const auto go = book.gate(kPair, kStorm, kStormFp, known(kStormCost),
                              kStormCost, 31, cfg);
    EXPECT_EQ(go.gate, TakeGate::Attempt);
    EXPECT_EQ(go.log, TakeLogEvent::Resume);
    EXPECT_GT(go.suppressed_cycles, 0u);
}

TEST(TakeRetryClearing, RpcProvenFundingHoldClearsWhenTheReadRises)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();
    const OfferFingerprint fp{kGoodAsk, kGoodSize};

    // The read said we could afford it; the wallet disagreed. Hold.
    book.note_failure(kPair, "coin-lock", fp, TakeFailureClass::Funding,
                      known(kGoodCost + 5), 1000, cfg);
    EXPECT_EQ(book.gate(kPair, "coin-lock", fp, known(kGoodCost + 5),
                        kGoodCost, 1001, cfg).gate,
              TakeGate::DeclineFundingHold);
    // A DROP in the read that still covers the cost is not new information.
    EXPECT_EQ(book.gate(kPair, "coin-lock", fp, known(kGoodCost + 1),
                        kGoodCost, 1002, cfg).gate,
              TakeGate::DeclineFundingHold);
    // A RISE above what we had when the wallet said no releases it at once.
    EXPECT_EQ(book.gate(kPair, "coin-lock", fp, known(kGoodCost + 6),
                        kGoodCost, 1003, cfg).gate,
              TakeGate::Attempt);
}

// [review 2026-09-01] THE ORDERING BUG, and the log line that admitted it.
// The hold used to be evaluated BEFORE the ordinary balance comparison and to
// reuse its enumerator. Because the hold is reachable only from a reading that
// said AFFORDABLE, the shortfall guard (`cost > spendable`) never fired, and
// the call site printed "insufficient balance: need 485908 spendable 485913
// short 0 ... Clears the cycle the balance covers it" -- in 100% of the cases
// the hold exists to serve. Self-contradicting, and indistinguishable from the
// benign short-balance decline that fires 96 times in 104.
TEST(TakeRetryClearing, TheFundingHoldNeverMasqueradesAsAShortBalance)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();
    const OfferFingerprint fp{kGoodAsk, kGoodSize};

    book.note_failure(kPair, "coin-lock", fp, TakeFailureClass::Funding,
                      known(kGoodCost + 5), 1000, cfg);

    const auto d = book.gate(kPair, "coin-lock", fp, known(kGoodCost + 5),
                             kGoodCost, 1001, cfg);
    ASSERT_EQ(d.gate, TakeGate::DeclineFundingHold);
    EXPECT_NE(d.gate, TakeGate::DeclineInsufficient)
        << "the hold must not borrow the short-balance enumerator";
    // Nothing here may read as a shortfall, because there is not one.
    EXPECT_EQ(d.shortfall, 0);
    EXPECT_GE(d.spendable, d.cost);
    // And it must carry what the log needs to describe the state it is in:
    // the reading the WALLET rejected, and when the hold expires.
    EXPECT_EQ(d.rejected_at, kGoodCost + 5);
    EXPECT_EQ(d.ready_block, 1000u + cfg.funding_recheck_blocks);

    // A read that is GENUINELY short while the hold is armed is reported as
    // short, with a truthful shortfall -- the ordinary comparison adjudicates
    // first, so the hold can never hide a real balance problem either.
    const auto s = book.gate(kPair, "coin-lock", fp, known(kGoodCost - 7),
                             kGoodCost, 1002, cfg);
    EXPECT_EQ(s.gate, TakeGate::DeclineInsufficient);
    EXPECT_EQ(s.shortfall, 7);
    EXPECT_EQ(s.rejected_at, 0);
}

// [review 2026-09-01] The hold's PRIMARY release -- "the read rose above what
// it was when the wallet said no" -- is structurally unreachable on the case
// it was written for: the coin-lock read is stale-HIGH by construction, so the
// post-recovery read is always lower. The hold therefore degrades to its
// timer, which was a flat 192 blocks (~1h). Since Step 8 posts immediately
// before Step 9 on every cycle, the next attempt after release re-armed it: a
// permanent ~1h duty cycle of suppression on a fundable, crossed offer.
TEST(TakeRetryClearing, TheFundingHoldIsMinutesOnFirstContactNotAnHour)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();
    const OfferFingerprint fp{kGoodAsk, kGoodSize};

    // coin_lock_ledger.hpp measured 73s of staleness -- about 4 blocks. The
    // first hold must be of that order, not of the order of an hour.
    EXPECT_LE(cfg.funding_recheck_blocks, 16u)
        << "a fundable 235bps cross must not be sat out for an hour on the "
           "strength of one stale reading";

    book.note_failure(kPair, "pinned", fp, TakeFailureClass::Funding,
                      known(kGoodCost + 5), 1000, cfg);
    for (std::uint64_t b = 1001; b < 1000 + cfg.funding_recheck_blocks; ++b) {
        EXPECT_EQ(book.gate(kPair, "pinned", fp, known(kGoodCost + 5),
                            kGoodCost, b, cfg).gate,
                  TakeGate::DeclineFundingHold) << "block " << b;
    }
    EXPECT_EQ(book.gate(kPair, "pinned", fp, known(kGoodCost + 5), kGoodCost,
                        1000 + cfg.funding_recheck_blocks, cfg).gate,
              TakeGate::Attempt);

    // A wallet that keeps rejecting a read-affordable take DOES escalate, so
    // the short first hold is not a licence to hammer.
    const std::uint64_t second = 1000 + cfg.funding_recheck_blocks;
    book.note_failure(kPair, "pinned", fp, TakeFailureClass::Funding,
                      known(kGoodCost + 5), second, cfg);
    const auto d = book.gate(kPair, "pinned", fp, known(kGoodCost + 5),
                             kGoodCost, second + 1, cfg);
    ASSERT_EQ(d.gate, TakeGate::DeclineFundingHold);
    EXPECT_EQ(d.ready_block, second + cfg.funding_recheck_blocks * 2)
        << "consecutive proven rejections must lengthen the hold";
    EXPECT_LE(funding_hold_blocks(1000, cfg), cfg.funding_recheck_max_blocks);
}

TEST(TakeRetryClearing, AChangedOfferIsANewOffer)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();

    book.note_failure(kPair, kStorm, kStormFp, TakeFailureClass::Funding,
                      known(kBestBal), 100, cfg);
    book.note_failure(kPair, kStorm, kStormFp, TakeFailureClass::Other,
                      known(kBestBal), 100, cfg);
    ASSERT_NE(book.gate(kPair, kStorm, kStormFp, known(kBestBal),
                        kStormCost, 101, cfg).gate,
              TakeGate::Attempt);

    // The counterparty re-posted under the SAME id at half the size. §4 of the
    // taxonomy showed ids are stable neither way -- two ids were each taken
    // three times, and identical content arrived under four distinct ids -- so
    // the id is a map key, never a verdict. The fingerprint is the verdict.
    const OfferFingerprint reposted{kStormAsk, kStormSize / 2};
    const Mojo half_cost = ask_take_cost(kStormSize / 2, kStormAsk,
                                         kXchDenom, kCatDenom, 0);
    ASSERT_LT(half_cost, kBestBal);
    EXPECT_EQ(book.gate(kPair, kStorm, reposted, known(kBestBal),
                        half_cost, 101, cfg).gate,
              TakeGate::Attempt);

    const auto* e = book.find(kPair, kStorm);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->rpc_funding_failures, 0u);
    EXPECT_EQ(e->other_failures, 0u);
}

// ===========================================================================
// 5. THE LOG. Edges plus a heartbeat, never a level.
//
//    The Cgp9 window carried 153 Step-9c info/error lines, 150 of them one
//    doomed offer -- ~94% of the step's entire output. The live log already
//    carries 3,129 warnings in 5.4h from 18 distinct messages; a fix that
//    adds to that stream is how the next finding gets missed.
// ===========================================================================

TEST(TakeRetryLogging, TheFiftyAttemptStormCollapsesToOneLine)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();

    // 50 cycles over 45m54s. At ~18.75s a block that is ~147 blocks, so the
    // 192-block heartbeat does not come due inside the storm at all.
    int edges = 0, heartbeats = 0, quiet = 0;
    for (int i = 0; i < 50; ++i) {
        const auto d = book.gate(kPair, kStorm, kStormFp, known(kBestBal),
                                 kStormCost, static_cast<std::uint64_t>(3 * i),
                                 cfg);
        ASSERT_EQ(d.gate, TakeGate::DeclineInsufficient);
        switch (d.log) {
            case TakeLogEvent::Edge:      ++edges;      break;
            case TakeLogEvent::Heartbeat: ++heartbeats; break;
            case TakeLogEvent::None:      ++quiet;      break;
            case TakeLogEvent::Resume:    FAIL() << "resumed while broke";
        }
    }
    EXPECT_EQ(edges, 1);          // was: 50 detections + 50 TAKING + 50 error
    EXPECT_EQ(heartbeats, 0);
    EXPECT_EQ(quiet, 49);
}

TEST(TakeRetryLogging, APersistentSuppressionKeepsSayingSo)
{
    TakeRetryBook book;
    auto cfg = live_cfg();

    // The breaker_skip_warned_ regression: an edge with no off-edge and no
    // heartbeat compressed a 4h10m total-quoting outage into ONE line nobody
    // saw. Over ~5 days of a permanently unaffordable offer we must keep
    // hearing about it.
    int edges = 0, heartbeats = 0;
    for (std::uint64_t b = 0; b < 2000; ++b) {
        const auto d = book.gate(kPair, kFrozen, kStormFp, known(kBestBal),
                                 kStormCost, b, cfg);
        if (d.log == TakeLogEvent::Edge)      ++edges;
        if (d.log == TakeLogEvent::Heartbeat) ++heartbeats;
    }
    EXPECT_EQ(edges, 1);
    EXPECT_EQ(heartbeats, static_cast<int>(1999u / cfg.heartbeat_blocks));
    EXPECT_GT(heartbeats, 0) << "a permanently suppressed offer must not go "
                                "silent -- see breaker_skip_warned_";

    // And the mutation this guards: delete the heartbeat and the suppression
    // becomes invisible for as long as it lasts.
    TakeRetryBook mute;
    cfg.heartbeat_blocks = 0;
    int mute_lines = 0;
    for (std::uint64_t b = 0; b < 2000; ++b) {
        if (mute.gate(kPair, kFrozen, kStormFp, known(kBestBal),
                      kStormCost, b, cfg).log != TakeLogEvent::None) {
            ++mute_lines;
        }
    }
    EXPECT_EQ(mute_lines, 1) << "this is what removing the heartbeat costs";
}

TEST(TakeRetryLogging, AChangeOfReasonIsReportedButRateLimited)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();
    ASSERT_GT(cfg.reason_change_debounce_blocks, 0u);

    EXPECT_EQ(book.gate(kPair, kStorm, kStormFp, known(kBestBal),
                        kStormCost, 10, cfg).log, TakeLogEvent::Edge);
    EXPECT_EQ(book.gate(kPair, kStorm, kStormFp, known(kBestBal),
                        kStormCost, 11, cfg).log, TakeLogEvent::None);

    // The wallet stopped answering. That is a different fact about the world
    // and it gets its own line -- but not instantly, and not every time it
    // flips back and forth. Inside the debounce it stays PENDING.
    const auto u = book.gate(kPair, kStorm, kStormFp, unknown(),
                             kStormCost, 12, cfg);
    EXPECT_EQ(u.gate, TakeGate::DeclineUnknown);
    EXPECT_EQ(u.log, TakeLogEvent::None);

    // ...and is reported once the debounce expires. A pending change must not
    // be swallowed: `suppressed_gate` is the reason last REPORTED, not the
    // reason last seen.
    const auto later = book.gate(kPair, kStorm, kStormFp, unknown(),
                                 kStormCost,
                                 10 + cfg.reason_change_debounce_blocks, cfg);
    EXPECT_EQ(later.gate, TakeGate::DeclineUnknown);
    EXPECT_EQ(later.log, TakeLogEvent::Edge);
}

// [review 2026-09-01] The reason-change branch used to share the first-
// suppression branch and reset suppressed_cycles, suppress_start_block AND
// last_emit_block. Two failures, both breaker_skip_warned_ in new clothes:
// an alternating gate re-armed an Edge EVERY CYCLE (50 alternating cycles =
// 50 lines, half at warn -- worse than the 3-per-cycle it replaced), and the
// heartbeat became unreachable while the operator was told "cycles=39
// blocks=38" about a suppression that had run 400 blocks.
TEST(TakeRetryLogging, AFlappingReasonNeitherStormsNorResetsTheClock)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();

    int lines = 0;
    TakeDecision last{};
    for (std::uint64_t b = 0; b < 400; ++b) {
        // The gate value alternates every single cycle: permanently
        // unaffordable, with the balance read failing on odd blocks.
        const auto reading = (b % 2 == 1) ? unknown() : known(kBestBal);
        last = book.gate(kPair, kStorm, kStormFp, reading, kStormCost, b, cfg);
        ASSERT_NE(last.gate, TakeGate::Attempt) << "block " << b;
        if (last.log != TakeLogEvent::None) ++lines;
    }

    // Bounded by the debounce, not by the cycle count. Was 400.
    EXPECT_LE(lines, 400 / static_cast<int>(cfg.reason_change_debounce_blocks)
                         + 2);
    // But not silent either: a suppression that flaps must still be audible.
    EXPECT_GE(lines, 2);

    // THE CLOCK IS THE REAL FINDING. The operator must be told how long this
    // has actually been going on, not how long since the last flip.
    EXPECT_EQ(last.suppressed_blocks, 399u);
    EXPECT_EQ(last.suppressed_cycles, 400u);
}

// ===========================================================================
// 6. THE MAP IS BOUNDED. An unbounded map is a leak, and a leak on a
//    long-running money-path process is an outage with a delay fuse.
// ===========================================================================

TEST(TakeRetryEviction, TheMapCannotGrowWithoutBound)
{
    TakeRetryBook book;
    TakeRetryConfig cfg = live_cfg();
    cfg.max_entries        = 32;
    cfg.stale_after_blocks = 0;   // age-out DISABLED, so only the hard cap
                                  // can be holding the line here.

    // An adversarial or merely churning book: 5,000 distinct offer ids, never
    // pruned to a live set. This is exactly the shape retain_live cannot cover
    // on a cycle whose book read empty.
    for (std::uint64_t i = 0; i < 5000; ++i) {
        book.gate(kPair, "offer-" + std::to_string(i),
                  OfferFingerprint{kStormAsk, kStormSize},
                  known(kBestBal), kStormCost, i, cfg);
        book.sweep(i, cfg);
        ASSERT_LE(book.size(), cfg.max_entries) << "at insert " << i;
    }
    EXPECT_LE(book.size(), cfg.max_entries);
    EXPECT_GT(book.size(), 0u);
}

TEST(TakeRetryEviction, TheHardCapEvictsTheLEASTRecentlySeen)
{
    TakeRetryBook book;
    TakeRetryConfig cfg = live_cfg();
    cfg.max_entries        = 3;
    cfg.stale_after_blocks = 0;

    for (std::uint64_t i = 0; i < 5; ++i) {
        book.gate(kPair, "o" + std::to_string(i), kStormFp,
                  known(kBestBal), kStormCost, 100 + i, cfg);
    }
    // [review] The two counts are reported SEPARATELY. An age-out is routine;
    // a CAP eviction discards a suppression that may still be live, and the
    // storm it was holding resumes -- so the call site can warn on exactly
    // that and stay silent about the routine one. Returning a single total
    // made the only eviction that matters unreportable.
    const auto swept = book.sweep(200, cfg);
    EXPECT_EQ(swept.capped, 2u);
    EXPECT_EQ(swept.aged, 0u);
    EXPECT_EQ(swept.total(), 2u);
    EXPECT_EQ(book.size(), 3u);
    EXPECT_EQ(book.find(kPair, "o0"), nullptr);
    EXPECT_EQ(book.find(kPair, "o1"), nullptr);
    EXPECT_NE(book.find(kPair, "o4"), nullptr);
}

TEST(TakeRetryEviction, StaleEntriesAgeOutEvenWithoutALiveSet)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();

    book.gate(kPair, kStorm, kStormFp, known(kBestBal), kStormCost, 100, cfg);
    ASSERT_EQ(book.size(), 1u);
    EXPECT_EQ(book.sweep(100 + cfg.stale_after_blocks, cfg).total(), 0u);
    EXPECT_EQ(book.size(), 1u);
    const auto swept = book.sweep(100 + cfg.stale_after_blocks + 1, cfg);
    EXPECT_EQ(swept.aged, 1u);
    EXPECT_EQ(swept.capped, 0u) << "an age-out is not a cap eviction";
    EXPECT_EQ(book.size(), 0u);
}

TEST(TakeRetryEviction, RetainLiveIsScopedToItsOwnPair)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();

    book.gate("XCH/DBX", "a", kStormFp, known(kBestBal), kStormCost, 1, cfg);
    book.gate("XCH/DBX", "b", kStormFp, known(kBestBal), kStormCost, 1, cfg);
    book.gate("XCH/BYC", "c", kStormFp, known(kBestBal), kStormCost, 1, cfg);
    ASSERT_EQ(book.size(), 3u);

    // XCH/DBX's book now shows only `a`. `b` is garbage by construction --
    // an offer absent from the book cannot be re-selected. `c` belongs to a
    // pair we did not look at and must survive.
    book.retain_live("XCH/DBX", std::unordered_set<std::string>{"a"});
    EXPECT_NE(book.find("XCH/DBX", "a"), nullptr);
    EXPECT_EQ(book.find("XCH/DBX", "b"), nullptr);
    EXPECT_NE(book.find("XCH/BYC", "c"), nullptr);
}

// [review 2026-09-01] The header calls this invariant load-bearing -- "one
// transient empty Dexie snapshot would wipe every suppression and resume the
// storm" -- and cites offer_manager.cpp, which clear()s its equivalent
// counters on an empty input list, as the mistake not to copy. It was NOT
// pinned by anything: the mutation `if (live_offer_ids.empty()) { entries_
// .clear(); return; }` at the top of retain_live passed all 23 TakeRetry tests
// and the whole 1,149-test suite, because no test called retain_live with an
// empty set and the only guard was an untested `continue` at the call site.
TEST(TakeRetryEviction, RetainLiveOnAnEmptySetKeepsEverything)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();

    book.gate(kPair, kStorm, kStormFp, known(kBestBal), kStormCost, 1, cfg);
    book.gate(kPair, kFrozen, kStormFp, known(kBestBal), kStormCost, 1, cfg);
    ASSERT_EQ(book.size(), 2u);

    book.retain_live(kPair, std::unordered_set<std::string>{});
    EXPECT_EQ(book.size(), 2u)
        << "an empty book read is a FEED failure, not proof that every "
           "suppressed offer is gone -- clearing here resumes the storm";
    EXPECT_NE(book.find(kPair, kStorm), nullptr);
    EXPECT_NE(book.find(kPair, kFrozen), nullptr);
}

TEST(TakeRetryEviction, ASuccessfulTakeForgetsTheOffer)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();
    book.note_failure(kPair, kStorm, kStormFp, TakeFailureClass::Funding,
                      known(kBestBal), 10, cfg);
    ASSERT_NE(book.find(kPair, kStorm), nullptr);
    book.note_success(kPair, kStorm);
    EXPECT_EQ(book.find(kPair, kStorm), nullptr);
}

// ===========================================================================
// 7. THE DENOMINATION. The silent kill switch.
// ===========================================================================

TEST(TakeRetryCost, CostIsQuoteDenominatedNotBase)
{
    // XCH/DBX: base_mojos_per_unit 1e12 (XCH), quote_mojos_per_unit 1000
    // (DBX is a CAT). Lifting an ask spends QUOTE.
    //
    // The exact rational value is 485908. The multiplication runs in
    // long double, which is 64-bit on MSVC and 80-bit on x86 GCC -- neither
    // holds the 68-bit intermediate exactly -- so this is asserted as a
    // one-mojo band rather than an equality. A recent bug in this area (a
    // clamp that rounded to 2^64) passed MSVC and failed GCC in CI; pretending
    // the arithmetic is exact is how that happens again.
    const Mojo cost = ask_take_cost(kStormSize, kStormAsk,
                                    kXchDenom, kCatDenom, /*fee=*/0);
    EXPECT_GE(cost, kStormCost);
    EXPECT_LE(cost, kStormCost + 1);

    // THE MUTATION. `cost = take_size` -- the base size -- is one token at the
    // call site. Against the same wallet it declines by a factor of ~15
    // million, on every pair, forever, while looking exactly like a working
    // guard.
    EXPECT_EQ(decide_funding(known(kBestBal), cost),
              FundingVerdict::Insufficient);          // correct: we were broke
    EXPECT_NE(cost, kStormSize);
    EXPECT_LT(cost, kStormSize / 1'000'000);

    // And on the take that actually settled, the correct denomination FUNDS
    // where the base denomination would have declined. This is the test that
    // goes red if someone "simplifies" the cost to the base size.
    const Mojo good = ask_take_cost(kGoodSize, kGoodAsk,
                                    kXchDenom, kCatDenom, 0);
    EXPECT_EQ(good, kGoodCost);
    EXPECT_EQ(decide_funding(known(kLowBal), good), FundingVerdict::Fund);
    EXPECT_EQ(decide_funding(known(kLowBal), kGoodSize),
              FundingVerdict::Insufficient);
}

TEST(TakeRetryCost, TheFeeIsAddedONLYWhenItSharesTheSpendWallet)
{
    // XCH/DBX: the fee is XCH from wallet 1, the spend is DBX from wallet 8.
    // A single-wallet check structurally cannot see the fee, so 0 is correct
    // and this term is inert on every currently enabled pair. Step 9e and 9f
    // both omitted it unconditionally, which is only harmless because of that.
    const Mojo dbx = ask_take_cost(kGoodSize, kGoodAsk,
                                   kXchDenom, kCatDenom, 0);
    EXPECT_EQ(dbx, kGoodCost);

    // wmilliETH.b/XCH (config.yaml, enabled:false today, and the memory index
    // records intent to revisit that family): the quote IS xch, so the fee and
    // the spend leave the SAME wallet and the requirement really is cost+fee.
    const Mojo fee = 873'074LL;   // top of the observed live fee range
    const Mojo xch = ask_take_cost(kGoodSize, kGoodAsk,
                                   kXchDenom, kCatDenom, fee);
    EXPECT_EQ(xch, kGoodCost + fee);

    // An unpriceable cost stays unpriceable -- adding a fee must not
    // manufacture one.
    EXPECT_EQ(ask_take_cost(0, kGoodAsk, kXchDenom, kCatDenom, fee), 0);
    EXPECT_EQ(ask_take_cost(kGoodSize, 0, kXchDenom, kCatDenom, fee), 0);
    EXPECT_EQ(add_same_wallet_fee(0, fee), 0);

    // Saturating, not wrapping. A wrapped cost reads as affordable.
    const Mojo huge = std::numeric_limits<Mojo>::max() - 1;
    EXPECT_EQ(add_same_wallet_fee(huge, 100),
              std::numeric_limits<Mojo>::max());
}

// ===========================================================================
// 8. End to end, on the storm as it actually happened.
// ===========================================================================

TEST(TakeRetryStorm, Cgp9WasNeverFundableAndIsAttemptedZeroTimes)
{
    TakeRetryBook book;
    const auto cfg = live_cfg();

    // The four DBX spendable readings logged by Step 8 during the storm, plus
    // the global extremes of the whole 39h window. None of them funds it.
    const std::vector<Mojo> balances{134'778, 236'551, 336'775, 338'276,
                                     111'214, 437'875};
    const Mojo cost = ask_take_cost(kStormSize, kStormAsk,
                                    kXchDenom, kCatDenom, 0);

    int attempts = 0, lines = 0;
    std::uint64_t block = 0;
    for (int cycle = 0; cycle < 50; ++cycle, block += 3) {
        const Mojo bal = balances[static_cast<std::size_t>(cycle)
                                  % balances.size()];
        const auto d = book.gate(kPair, kStorm, kStormFp, known(bal),
                                 cost, block, cfg);
        if (d.gate == TakeGate::Attempt) ++attempts;
        if (d.log != TakeLogEvent::None) ++lines;
    }

    // Today: 50 Dexie GETs + 50 take_offer RPCs + 150 log lines.
    EXPECT_EQ(attempts, 0);
    EXPECT_EQ(lines, 1);

    // And the counterfactual: had the wallet ever held the 485,908 DBX mojos,
    // we would have taken it on the very next cycle. The gate costs no edge.
    EXPECT_EQ(book.gate(kPair, kStorm, kStormFp, known(cost), cost,
                        block, cfg).gate,
              TakeGate::Attempt);
}
