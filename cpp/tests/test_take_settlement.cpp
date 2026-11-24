// test_take_settlement.cpp -- [TAKE-SETTLE 2026-11-24] a take is booked when
// it settles, not when the wallet accepts it (execution/take_settlement.hpp),
// and the engine glue that makes every take go through that rule.
//
// The cases come from the live trace of 2026-11-24: 13 takes booked since
// 2026-09-03 that never settled (seven on 2026-11-20, Step 9c lifting one
// Dexie offer two or three times), and five settled takes of 2026-11-20 the
// engine never recorded.

#include <gtest/gtest.h>

#include "engine_source_scan.hpp"
#include "xop/execution/take_settlement.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace xop::execution;
using xop::BlockHeight;
using xop::Mojo;
using xop_tests::engine_scan::count_of;
using xop_tests::engine_scan::function_body;
using xop_tests::engine_scan::position_of;
using xop_tests::engine_scan::read_engine_cpp;
using xop_tests::engine_scan::squeeze;

const std::string kDbx = "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20";
const std::string kByc = "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac";
const std::string kUsdc = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";

// One of the five unrecorded takes of 2026-11-20, as Chia 2.7.4 writes it:
// the summary is the MAKER's offer, amounts as decimal text.
nlohmann::json take_record(const std::string& status = "CONFIRMED",
                           std::uint64_t height = 9'600'225)
{
    return {
        {"trade_id", "0x5ed85168beb7f7f2aa"},
        {"is_my_offer", false},
        {"status", status},
        {"confirmed_at_index", height},
        {"summary", {{"offered", {{"xch", "5000000000000"}}},
                     {"requested", {{kDbx, "420000"}}},
                     {"fees", 330000}}},
    };
}

// A flat wallet coin record (get_coin_records), in wallet `wallet_id`, with
// its own coin id.
nlohmann::json coin(std::uint64_t amount, std::uint64_t height, std::int64_t wallet_id = 1)
{
    static int serial = 0;
    return {
        {"id", "0xC" + std::to_string(++serial)},
        {"amount", amount},
        {"confirmed_height", height},
        {"spent_height", 0},
        {"parent_coin_info", "0x" + std::string(64, 'a')},
        {"puzzle_hash", "0x" + std::string(64, 'b')},
        {"wallet_identifier", {{"id", wallet_id}, {"type", wallet_id == 1 ? 0 : 6}}},
    };
}

// The live wallet ids: 1 XCH, 2 wUSDC.b, 4 BYC, 8 DBX.
const std::unordered_map<std::string, std::int64_t> kWallets{
    {"xch", 1}, {kUsdc, 2}, {kByc, 4}, {kDbx, 8}};

PendingTake pending(const std::string& trade_id, const std::string& offer_id,
                    BlockHeight submitted)
{
    PendingTake p;
    p.strategy              = "crossed_book";
    p.trade_id              = trade_id;
    p.counterparty_offer_id = offer_id;
    p.pair_name             = "XCH/DBX";
    p.submitted_block       = submitted;
    return p;
}

}  // namespace

// ============================================================================
// judge_take
// ============================================================================

TEST(TakeSettlement, ASettledTakeWithItsPaymentBooks) {
    EXPECT_EQ(judge_take(WalletCancelState::Confirmed, true, 100, 101), TakeVerdict::Book);
}

TEST(TakeSettlement, ConfirmedWithoutThePaymentWaits) {
    // The wallet's status is its bookkeeping, not evidence (fill_proof.hpp).
    EXPECT_EQ(judge_take(WalletCancelState::Confirmed, false, 100, 101), TakeVerdict::Wait);
}

TEST(TakeSettlement, AFailedOrCancelledTakeBooksNothing) {
    // 2026-11-20: da716925 / f8a59d4b / 7b95898e FAILED; 8acbdbfe, 1f3a9fd8,
    // a757b5f6, 43cbe65b CANCELLED.  All seven were booked as takes.
    EXPECT_EQ(judge_take(WalletCancelState::Failed, false, 100, 101), TakeVerdict::Drop);
    EXPECT_EQ(judge_take(WalletCancelState::Cancelled, false, 100, 101), TakeVerdict::Drop);
    // Even a payment-looking coin does not revive a take the wallet failed.
    EXPECT_EQ(judge_take(WalletCancelState::Failed, true, 100, 101), TakeVerdict::Drop);
}

TEST(TakeSettlement, PendingOrUnreadableWaitsThenGivesUp) {
    for (const auto state : {WalletCancelState::PendingConfirm, WalletCancelState::PendingAccept,
                             WalletCancelState::Unknown, WalletCancelState::PendingCancel}) {
        EXPECT_EQ(judge_take(state, false, 100, 100 + kTakeSettleGiveUpBlocks), TakeVerdict::Wait);
        EXPECT_EQ(judge_take(state, false, 100, 101 + kTakeSettleGiveUpBlocks), TakeVerdict::GiveUp);
    }
    // A settled take is booked however late it is noticed.
    EXPECT_EQ(judge_take(WalletCancelState::Confirmed, true, 100, 100 + 10 * kTakeSettleGiveUpBlocks),
              TakeVerdict::Book);
    // A height that went backwards is not age.
    EXPECT_EQ(judge_take(WalletCancelState::PendingConfirm, false, 100, 50), TakeVerdict::Wait);
}

TEST(TakeSettlement, TheZeroVerdictAuthorisesNothing) {
    EXPECT_EQ(TakeVerdict{}, TakeVerdict::Wait);
    EXPECT_STREQ(to_string(TakeVerdict::Book), "book");
    EXPECT_STREQ(to_string(TakeVerdict::Drop), "drop");
}

// ============================================================================
// read_wallet_take
// ============================================================================

TEST(TakeSettlement, ATakeRecordReadsFromOurSide) {
    const auto take = read_wallet_take(take_record());
    ASSERT_TRUE(take.has_value());
    EXPECT_EQ(take->trade_id, "0x5ed85168beb7f7f2aa");
    EXPECT_EQ(take->height, 9'600'225u);
    // The maker offered 5 XCH: we received it.  It asked 420 DBX: we paid.
    ASSERT_EQ(take->received.size(), 1u);
    EXPECT_EQ(take->received[0], (AssetAmount{"xch", 5'000'000'000'000}));
    ASSERT_EQ(take->paid.size(), 1u);
    EXPECT_EQ(take->paid[0], (AssetAmount{kDbx, 420'000}));
}

TEST(TakeSettlement, NumbersAndPrefixedIdsReadToo) {
    auto r = take_record();
    r["summary"]["offered"]   = {{"XCH", 5'000'000'000'000LL}};
    r["summary"]["requested"] = {{"0x" + kDbx, 420'000}};
    const auto take = read_wallet_take(r);
    ASSERT_TRUE(take.has_value());
    EXPECT_EQ(take->received[0], (AssetAmount{"xch", 5'000'000'000'000}));
    EXPECT_EQ(take->paid[0], (AssetAmount{kDbx, 420'000}));
}

TEST(TakeSettlement, OnlyAConfirmedTakeOfOursReads) {
    auto mine = take_record();
    mine["is_my_offer"] = true;
    EXPECT_FALSE(read_wallet_take(mine).has_value());

    EXPECT_FALSE(read_wallet_take(take_record("FAILED")).has_value());
    EXPECT_FALSE(read_wallet_take(take_record("PENDING_CONFIRM")).has_value());
    EXPECT_FALSE(read_wallet_take(take_record("CONFIRMED", 0)).has_value());

    auto no_flag = take_record();
    no_flag.erase("is_my_offer");
    EXPECT_FALSE(read_wallet_take(no_flag).has_value());

    auto no_summary = take_record();
    no_summary.erase("summary");
    EXPECT_FALSE(read_wallet_take(no_summary).has_value());

    EXPECT_FALSE(read_wallet_take(nlohmann::json::array()).has_value());
}

TEST(TakeSettlement, ASideThatReadsShortDoesNotBookShort) {
    auto fraction = take_record();
    fraction["summary"]["requested"][kDbx] = "420.5";
    EXPECT_FALSE(read_wallet_take(fraction).has_value());

    auto partial = take_record();
    partial["summary"]["offered"] = {{"xch", "5000000000000"}, {kByc, "-1"}};
    EXPECT_FALSE(read_wallet_take(partial).has_value());

    auto empty = take_record();
    empty["summary"]["offered"] = nlohmann::json::object();
    EXPECT_FALSE(read_wallet_take(empty).has_value());

    auto zero = take_record();
    zero["summary"]["offered"]["xch"] = "0";
    EXPECT_FALSE(read_wallet_take(zero).has_value());
}

// ============================================================================
// receipts_paid
// ============================================================================

TEST(TakeSettlement, ThePaymentMustBeInTheStoreAtTheTakesHeight) {
    const std::vector<AssetAmount> received{{"xch", 5'000'000'000'000}};
    EXPECT_TRUE(receipts_paid(received, {coin(5'000'000'000'000, 9'600'225)}, 9'600'225,
                              kWallets));
    // The 2026-11-20 phantoms: nothing of that amount arrived.
    EXPECT_FALSE(receipts_paid(received, {}, 9'600'225, kWallets));
    EXPECT_FALSE(receipts_paid(received, {coin(5'000'000'000'000, 9'600'226)}, 9'600'225,
                               kWallets));
    EXPECT_FALSE(receipts_paid(received, {coin(4'999'999'999'999, 9'600'225)}, 9'600'225,
                               kWallets));
    EXPECT_EQ(receipt_amounts(received), (std::vector<std::uint64_t>{5'000'000'000'000}));
}

TEST(TakeSettlement, ThePaymentMustBeInTheReceivedAssetsOwnWallet) {
    // [review #262] get_coin_records spans every wallet: a 1,000-mojo DBX
    // coin must not pay a 1,000-mojo BYC receipt.
    const std::vector<AssetAmount> byc{{kByc, 1'000}};
    EXPECT_FALSE(receipts_paid(byc, {coin(1'000, 50, /*wallet_id=*/8)}, 50, kWallets));
    EXPECT_TRUE(receipts_paid(byc, {coin(1'000, 50, /*wallet_id=*/4)}, 50, kWallets));
    // An asset the engine cannot place in a wallet proves nothing.
    EXPECT_FALSE(receipts_paid(byc, {coin(1'000, 50, 4)}, 50, {{"xch", 1}}));
    // A record that does not say its wallet proves nothing either.
    auto anonymous = coin(1'000, 50, 4);
    anonymous.erase("wallet_identifier");
    EXPECT_FALSE(receipts_paid(byc, {anonymous}, 50, kWallets));
    EXPECT_EQ(coin_record_wallet_id(coin(1, 1, 8)), std::optional<std::int64_t>{8});
}

TEST(TakeSettlement, OneCoinNeverVouchesForTwoReceipts) {
    const std::vector<AssetAmount> twice{{kDbx, 1'000}, {kByc, 1'000}};
    EXPECT_FALSE(receipts_paid(twice, {coin(1'000, 50, 8)}, 50, kWallets));
    // Two coins of ONE asset do not pay receipts in two assets.
    EXPECT_FALSE(receipts_paid(twice, {coin(1'000, 50, 8), coin(1'000, 50, 8)}, 50, kWallets));
    EXPECT_TRUE(receipts_paid(twice, {coin(1'000, 50, 8), coin(1'000, 50, 4)}, 50, kWallets));
}

TEST(TakeSettlement, AnUnreadableAnswerProvesNothing) {
    const std::vector<AssetAmount> received{{"xch", 1'000}};
    auto bad = coin(1'000, 50);
    bad.erase("parent_coin_info");
    EXPECT_FALSE(receipts_paid(received, {coin(1'000, 50), bad}, 50, kWallets));
    EXPECT_FALSE(receipts_paid({}, {coin(1'000, 50)}, 50, kWallets));
    EXPECT_FALSE(receipts_paid(received, {coin(1'000, 50)}, 0, kWallets));
}

TEST(TakeSettlement, APaymentABlockLateStillSettlesTheTakeAtItsOwnBlock) {
    // [review #262] confirmed_at_index has been seen one block before the
    // payment (2026-09-04, paid at H+1).  Within kReceiptGiveBlocks the take
    // settles, at the block the payment landed in.
    const std::vector<AssetAmount> received{{kByc, 1'630}};
    EXPECT_EQ(receipts_settled_at(received, {coin(1'630, 9'245'442, 4)}, 9'245'441, kWallets,
                                  kReceiptGiveBlocks),
              std::optional<std::uint64_t>{9'245'442});
    // Exactly at the index it settles there; past the give it does not.
    EXPECT_EQ(receipts_settled_at(received, {coin(1'630, 9'245'441, 4)}, 9'245'441, kWallets,
                                  kReceiptGiveBlocks),
              std::optional<std::uint64_t>{9'245'441});
    EXPECT_FALSE(receipts_settled_at(received, {coin(1'630, 9'245'441 + kReceiptGiveBlocks + 1, 4)},
                                     9'245'441, kWallets, kReceiptGiveBlocks).has_value());
    // Never before the index.
    EXPECT_FALSE(receipts_settled_at(received, {coin(1'630, 9'245'440, 4)}, 9'245'441, kWallets,
                                     kReceiptGiveBlocks).has_value());
    // With no give it is the exact-height proof.
    EXPECT_FALSE(receipts_paid(received, {coin(1'630, 9'245'442, 4)}, 9'245'441, kWallets));
    // Two receipts: the take settled when the later one landed; the earliest
    // matching coin is taken for each.
    const std::vector<AssetAmount> two{{kByc, 1'000}, {kDbx, 2'000}};
    EXPECT_EQ(receipts_settled_at(two, {coin(1'000, 51, 4), coin(1'000, 50, 4), coin(2'000, 51, 8)},
                                  50, kWallets, kReceiptGiveBlocks),
              std::optional<std::uint64_t>{51});
}

TEST(TakeSettlement, ACoinAnotherBookedTakeClaimedPaysNothing) {
    // [review #262] Proofs run per trade, over fresh lists: the claims carry
    // "one coin pays one take" across them, booked trades included.
    const std::vector<AssetAmount> received{{"xch", 5'000'000'000'000}};
    const auto paid = coin(5'000'000'000'000, 9'600'516);
    const std::string id = paid.at("id").get<std::string>();
    const auto proof = prove_receipts(received, {paid}, 9'600'516, kWallets, kReceiptGiveBlocks, {});
    ASSERT_TRUE(proof.has_value());
    EXPECT_EQ(proof->settled_at, 9'600'516u);
    EXPECT_EQ(proof->coin_ids, (std::vector<std::string>{normalize_id(id)}));
    // Claimed by another take: no proof.
    EXPECT_FALSE(prove_receipts(received, {paid}, 9'600'516, kWallets, kReceiptGiveBlocks,
                                {normalize_id(id)}).has_value());
    // Its own claim does not stop a replay of the same take.
    const std::unordered_map<std::string, std::string> owners{{normalize_id(id), "0xE08EBC5A"}};
    EXPECT_TRUE(claimed_by_others(owners, "e08ebc5a").empty());
    EXPECT_EQ(claimed_by_others(owners, "0x72a48007"),
              (std::unordered_set<std::string>{normalize_id(id)}));
    // A record with no coin id proves nothing.
    auto anonymous = paid;
    anonymous.erase("id");
    EXPECT_FALSE(prove_receipts(received, {anonymous}, 9'600'516, kWallets, kReceiptGiveBlocks, {})
                     .has_value());
    EXPECT_EQ(record_coin_ids({paid, anonymous}), (std::vector<std::string>{normalize_id(id)}));
}

TEST(TakeSettlement, ATrackedTakeIsToleranceUntilItIsBookedOrDropped) {
    // [review #262] A settled take can wait a few heartbeats to be read.
    std::unordered_map<std::string, PendingTake> takes;
    auto p = pending("0x3d58e29d", "AkA6GJrWFG", 9'600'106);
    p.exposure = {{"xch", 5'000'000'000'000}, {kDbx, 406'000}, {"xch", 330'000}};
    takes.emplace(p.trade_id, p);
    const std::unordered_map<std::string, UncertainTake> uncertain{
        {"nMDyHor9J8", UncertainTake{9'600'463, {{kDbx, 423'000}}}}};
    // Long after the uncertain hold lapsed, the tracked take still counts.
    const auto later = take_hold_slack(takes, uncertain, 9'600'463 + kUncertainTakeHoldBlocks);
    EXPECT_EQ(later.at("xch"), 5'000'000'330'000);
    EXPECT_EQ(later.at(kDbx), 406'000);
    const auto now = take_hold_slack(takes, uncertain, 9'600'463);
    EXPECT_EQ(now.at(kDbx), 406'000 + 423'000);
    EXPECT_TRUE(take_hold_slack({}, {}, 9'600'463).empty());
}

TEST(TakeSettlement, ATakesEventIdHasTwoSpellings) {
    // The engine books "take:0x<id>", the GUI's top-up ingest "take:<id>".
    const auto ids = take_event_ids("0xABCdef");
    EXPECT_EQ(ids[0], "take:0xabcdef");
    EXPECT_EQ(ids[1], "take:abcdef");
    EXPECT_EQ(take_event_ids("abcdef"), ids);
}

// ============================================================================
// take_booking_for_pair
// ============================================================================

TEST(TakeSettlement, TheLegsAreThePairsInEitherDirection) {
    const auto take = read_wallet_take(take_record());
    ASSERT_TRUE(take.has_value());
    // XCH/DBX: base XCH received, quote DBX paid -- we bought base.
    const auto buy = take_booking_for_pair(*take, "xch", kDbx);
    ASSERT_TRUE(buy.has_value());
    EXPECT_TRUE(buy->we_bought_base);
    EXPECT_EQ(buy->base_mojos, 5'000'000'000'000);
    EXPECT_EQ(buy->quote_mojos, 420'000);
    // The same take read against a DBX/XCH pair sells the base.
    const auto sell = take_booking_for_pair(*take, "0x" + kDbx, "XCH");
    ASSERT_TRUE(sell.has_value());
    EXPECT_FALSE(sell->we_bought_base);
    EXPECT_EQ(sell->base_mojos, 420'000);
    EXPECT_EQ(sell->quote_mojos, 5'000'000'000'000);
}

TEST(TakeSettlement, AnotherPairsOrAMultiAssetTakeHasNoLegsHere) {
    const auto take = read_wallet_take(take_record());
    ASSERT_TRUE(take.has_value());
    EXPECT_FALSE(take_booking_for_pair(*take, "xch", kByc).has_value());
    EXPECT_FALSE(take_booking_for_pair(*take, kByc, kUsdc).has_value());
    EXPECT_FALSE(take_booking_for_pair(*take, "xch", "xch").has_value());

    auto multi = take_record();
    multi["summary"]["offered"] = {{"xch", "5000000000000"}, {kByc, "1000"}};
    const auto both = read_wallet_take(multi);
    ASSERT_TRUE(both.has_value());
    EXPECT_FALSE(take_booking_for_pair(*both, "xch", kDbx).has_value());
}

// ============================================================================
// The re-take gate, the settle order and the sweep's window
// ============================================================================

TEST(TakeSettlement, AnOfferWithAnUnsettledTakeIsNotTakenAgain) {
    // 2026-11-20 08:04:14 Step 9c took AkA6GJ.. (3d58e29d, settled); at
    // 08:05:10 it took it again (f8a59d4b, FAILED) and booked both.
    std::unordered_map<std::string, PendingTake> takes;
    takes.emplace("0x3d58e29d", pending("0x3d58e29d", "AkA6GJrWFG", 9'600'106));
    const std::unordered_map<std::string, UncertainTake> none;
    EXPECT_TRUE(take_refused(takes, none, "AkA6GJrWFG", 9'600'108));
    EXPECT_FALSE(take_refused(takes, none, "FnrurLSVNv", 9'600'108));
    EXPECT_FALSE(take_refused(takes, none, "", 9'600'108));
}

TEST(TakeSettlement, AnUncertainRequestHoldsTheOfferForAWhile) {
    const std::unordered_map<std::string, PendingTake> none;
    const std::unordered_map<std::string, UncertainTake> uncertain{
        {"nMDyHor9J8", UncertainTake{9'600'463, {}}}};
    EXPECT_TRUE(take_refused(none, uncertain, "nMDyHor9J8", 9'600'463));
    EXPECT_TRUE(take_refused(none, uncertain, "nMDyHor9J8",
                             9'600'463 + kUncertainTakeHoldBlocks - 1));
    EXPECT_FALSE(take_refused(none, uncertain, "nMDyHor9J8",
                              9'600'463 + kUncertainTakeHoldBlocks));
    EXPECT_FALSE(take_refused(none, uncertain, "nMDyHor9J8", 9'600'000));   // went back
    EXPECT_FALSE(take_refused(none, uncertain, "Dq3TqEWpW5", 9'600'463));
}

TEST(TakeSettlement, AnUncertainTakeIsToleranceUntilTheSweepCanBookIt) {
    // [review #262] What it could move widens the divergence control's
    // tolerance for the whole hold, which outlasts the sweep's grace.
    UncertainTake hold{9'600'463, {{"xch", 5'000'000'000'000}, {kDbx, 423'000}}};
    add_fee_exposure(hold, 330'000);
    add_fee_exposure(hold, 0);
    const std::unordered_map<std::string, UncertainTake> uncertain{
        {"nMDyHor9J8", hold}, {"5Uoyw6hrnJ", UncertainTake{9'600'463, {{"xch", 7}}}}};
    const auto slack = uncertain_take_slack(uncertain, 9'600'463 + kTakeSweepGraceBlocks
                                                           + kTakeSweepEveryBlocks);
    EXPECT_EQ(slack.at("xch"), 5'000'000'000'000 + 330'000 + 7);
    EXPECT_EQ(slack.at(kDbx), 423'000);
    EXPECT_TRUE(uncertain_take_slack(uncertain, 9'600'463 + kUncertainTakeHoldBlocks).empty());
    // Saturating, never wrapping.
    const std::unordered_map<std::string, UncertainTake> huge{
        {"a", UncertainTake{1, {{"xch", std::numeric_limits<Mojo>::max()}}}},
        {"b", UncertainTake{1, {{"xch", 5}}}}};
    EXPECT_EQ(uncertain_take_slack(huge, 1).at("xch"), std::numeric_limits<Mojo>::max());
}

TEST(TakeSettlement, TakesSettleOldestFirstAndAFewAtATime) {
    std::unordered_map<std::string, PendingTake> takes;
    takes.emplace("0xc", pending("0xc", "c", 30));
    takes.emplace("0xa", pending("0xa", "a", 10));
    takes.emplace("0xb2", pending("0xb2", "b2", 20));
    takes.emplace("0xb1", pending("0xb1", "b1", 20));
    EXPECT_EQ(takes_to_settle(takes, 3), (std::vector<std::string>{"0xa", "0xb1", "0xb2"}));
    EXPECT_EQ(takes_to_settle(takes, 10).size(), 4u);
    EXPECT_TRUE(takes_to_settle({}, 4).empty());
}

TEST(TakeSettlement, NoTakeWaitsBehindOthersThatStayPending) {
    // [review #262] Oldest-first alone let four old pending takes starve a
    // fifth that had settled.  The least recently read go first.
    std::unordered_map<std::string, PendingTake> takes;
    for (const char* id : {"0x1", "0x2", "0x3", "0x4"}) {
        takes.emplace(id, pending(id, id, 10));
    }
    takes.emplace("0x5", pending("0x5", "5", 20));
    const auto first = takes_to_settle(takes, 4);
    EXPECT_EQ(first, (std::vector<std::string>{"0x1", "0x2", "0x3", "0x4"}));
    for (const auto& id : first) takes.at(id).last_checked_block = 100;
    EXPECT_EQ(takes_to_settle(takes, 4).front(), "0x5");
}

TEST(TakeSettlement, TheSweepLooksBetweenItsGraceAndItsLookback) {
    static_assert(kUncertainTakeHoldBlocks > kTakeSweepGraceBlocks + kTakeSweepEveryBlocks);
    constexpr BlockHeight kNow = 9'620'000;
    EXPECT_FALSE(in_sweep_window(kNow, kNow));                              // too fresh
    EXPECT_FALSE(in_sweep_window(kNow - kTakeSweepGraceBlocks + 1, kNow));
    EXPECT_TRUE(in_sweep_window(kNow - kTakeSweepGraceBlocks, kNow));
    EXPECT_TRUE(in_sweep_window(kNow - kTakeSweepLookbackBlocks, kNow));
    EXPECT_FALSE(in_sweep_window(kNow - kTakeSweepLookbackBlocks - 1, kNow));  // a repair's
    EXPECT_FALSE(in_sweep_window(0, kNow));
    EXPECT_FALSE(in_sweep_window(kNow + 1, kNow));

    EXPECT_TRUE(take_sweep_due(0, kNow));
    EXPECT_FALSE(take_sweep_due(kNow, kNow + kTakeSweepEveryBlocks - 1));
    EXPECT_TRUE(take_sweep_due(kNow, kNow + kTakeSweepEveryBlocks));
    EXPECT_TRUE(take_sweep_due(kNow, kNow - 5));
    EXPECT_FALSE(take_sweep_due(0, 0));
}

TEST(TakeSettlement, IdsCompareWithoutPrefixOrCase) {
    EXPECT_EQ(normalize_id("0xAbC"), "abc");
    EXPECT_EQ(normalize_id("0XABC"), "abc");
    EXPECT_EQ(normalize_id("abc"), "abc");
    EXPECT_EQ(normalize_id(""), "");
}

// ============================================================================
// Engine wiring (source scans; xop_tests constructs no Engine)
// ============================================================================

TEST(TakeSettlementWiring, NoTakeSiteBooksATakeAnyMore) {
    const std::string raw = read_engine_cpp();
    ASSERT_FALSE(raw.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::string all = squeeze(raw);
    // The five take sites track; none books.
    for (const char* strategy : {"crossed_book", "cross_stable", "peg_arb",
                                 "drift_correct", "xch_recovery"}) {
        EXPECT_EQ(count_of(all, std::string("track_pending_take(\"") + strategy + "\","), 1u)
            << strategy;
        EXPECT_EQ(count_of(all, std::string("record_taker_fill(\"") + strategy + "\","), 0u)
            << strategy;
    }
    // record_taker_fill is called from the settle step and the sweep only
    // (its definition and comments aside).
    const std::string settle = function_body(
        raw, "asio::awaitable<void> Engine::step_settle_pending_takes(BlockHeight block_height)");
    const std::string sweep = function_body(
        raw, "asio::awaitable<void> Engine::step_sweep_missed_takes(BlockHeight block_height)");
    ASSERT_FALSE(settle.empty());
    ASSERT_FALSE(sweep.empty());
    EXPECT_EQ(count_of(settle, "record_taker_fill("), 1u);
    EXPECT_EQ(count_of(sweep, "record_taker_fill(\"wallet_sweep\","), 1u);
}

TEST(TakeSettlementWiring, EveryTakeGoesThroughTheGuardedDoor) {
    const std::string raw = read_engine_cpp();
    const std::string all = squeeze(raw);
    // One wallet take_offer call in the whole engine, inside the guard.
    EXPECT_EQ(count_of(all, "wallet_->take_offer("), 1u);
    const std::string guard = function_body(
        raw, "asio::awaitable<nlohmann::json> Engine::take_offer_guarded(");
    ASSERT_FALSE(guard.empty());
    EXPECT_EQ(count_of(guard, "wallet_->take_offer(offer_text, intent.fee_mojos)"), 1u);
    EXPECT_LT(position_of(guard, "execution::take_refused(pending_takes_, uncertain_take_offers_,"),
              position_of(guard, "wallet_->take_offer(offer_text, intent.fee_mojos)"));
    EXPECT_NE(position_of(guard, "rpc::request_possibly_submitted(e.curl_code(), e.http_code())"),
              std::string::npos);
    // Each site passes the Dexie offer it is taking.
    for (const char* offer : {"best_ask_offer_id", "best_ask_a_id", "c.id", "chosen->id",
                              "cand.offer_id"}) {
        EXPECT_EQ(count_of(all, std::string("take_offer_guarded(") + offer + ","), 1u) << offer;
    }
}

TEST(TakeSettlementWiring, SettlementRunsBeforeTheInvariant) {
    const std::string all = squeeze(read_engine_cpp());
    const std::size_t topup  = all.find(squeeze("co_await step_ingest_topup_takes(block_height);"));
    const std::size_t settle = all.find(squeeze("co_await step_settle_pending_takes(block_height);"));
    const std::size_t sweep  = all.find(squeeze("co_await step_sweep_missed_takes(block_height);"));
    const std::size_t tie    = all.find(squeeze("step_check_ledger_invariant(block_height);"),
                                        sweep == std::string::npos ? 0 : sweep);
    ASSERT_NE(topup, std::string::npos);
    ASSERT_NE(settle, std::string::npos);
    ASSERT_NE(sweep, std::string::npos);
    ASSERT_NE(tie, std::string::npos);
    EXPECT_LT(topup, settle);
    EXPECT_LT(settle, sweep);
    EXPECT_LT(sweep, tie);
}

TEST(TakeSettlementWiring, BookingNeedsThePaymentProof) {
    const std::string raw = read_engine_cpp();
    for (const char* signature :
         {"asio::awaitable<void> Engine::step_settle_pending_takes(BlockHeight block_height)",
          "asio::awaitable<void> Engine::step_sweep_missed_takes(BlockHeight block_height)"}) {
        const std::string body = function_body(raw, signature);
        ASSERT_FALSE(body.empty()) << signature;
        const std::size_t proof = position_of(body, "proof = prove_take_receipts(*take, coins);");
        ASSERT_NE(proof, std::string::npos) << signature;
        EXPECT_LT(proof, position_of(body, "record_taker_fill(")) << signature;
    }
    // The sweep skips what the ledger holds under either spelling, what is
    // being tracked, the GUI's top-ups, and what it already did.
    const std::string sweep = function_body(
        raw, "asio::awaitable<void> Engine::step_sweep_missed_takes(BlockHeight block_height)");
    EXPECT_NE(position_of(sweep, "for (const auto& id : execution::take_event_ids(trade_id))"),
              std::string::npos);
    EXPECT_EQ(count_of(sweep, "in_ledger(take->trade_id)"), 2u);
    EXPECT_NE(position_of(sweep, "tracked.count(id) > 0 || topups.count(id) > 0 "
                                 "|| take_sweep_done_.count(id) > 0"), std::string::npos);
    EXPECT_NE(position_of(sweep, "const auto ids = read_gui_take_ids(store);"),
              std::string::npos);
    EXPECT_NE(position_of(sweep, "execution::in_sweep_window(take->height, block_height)"),
              std::string::npos);
    // Both proofs match each receipt in its own wallet, over the give,
    // honouring other takes' claims, and both book at the block the payment
    // landed in, claiming its coins.
    const std::string all = squeeze(raw);
    EXPECT_EQ(count_of(all, "proof = prove_take_receipts(*take, coins);"), 2u);
    const std::string helper = function_body(
        raw, "std::optional<execution::ReceiptProof> Engine::prove_take_receipts(");
    EXPECT_NE(position_of(helper, "db_->take_receipt_owners(execution::record_coin_ids(coins))"),
              std::string::npos);
    EXPECT_NE(position_of(helper, "receipt_wallet_ids(take.received)"), std::string::npos);
    EXPECT_NE(position_of(helper, "execution::claimed_by_others(*owners, take.trade_id)"),
              std::string::npos);
    EXPECT_EQ(count_of(all, "co_await wallet_->get_coin_records_in_range( take->height, "
                            "take->height + execution::kReceiptGiveBlocks,"), 2u);
    EXPECT_EQ(count_of(all, "static_cast<BlockHeight>(proof->settled_at)"), 2u);
    EXPECT_EQ(count_of(all, "proof->coin_ids"), 2u);
    EXPECT_EQ(count_of(all, "get_coin_records_at_height(take->height"), 0u);
    // Both read evidence only from a wallet synced this heartbeat.
    for (const char* signature :
         {"asio::awaitable<void> Engine::step_settle_pending_takes(BlockHeight block_height)",
          "asio::awaitable<void> Engine::step_sweep_missed_takes(BlockHeight block_height)"}) {
        const std::string body = function_body(raw, signature);
        const std::size_t synced = position_of(body, "co_await secondary_wallet_synced(block_height)");
        ASSERT_NE(synced, std::string::npos) << signature;
        EXPECT_LT(synced, position_of(body, "co_await wallet_->get_coin_records_in_range(")) << signature;
    }
}

TEST(TakeSettlementWiring, ABookingIsOneTransactionAndReplaysNothing) {
    const std::string raw = read_engine_cpp();
    const std::string record = function_body(raw, "void Engine::record_taker_fill(");
    ASSERT_FALSE(record.empty());
    // The row, the legs, the claims and the hold's end commit together.
    EXPECT_NE(position_of(record, "db_->book_take(f, legs, receipt_coin_ids, release_hold_key);"),
              std::string::npos);
    EXPECT_EQ(count_of(record, "db_->insert_taker_fill("), 0u);
    EXPECT_EQ(count_of(record, "db_->append_ledger_entries("), 0u);
    // Revive flows move once: only when this booking wrote the row.
    const std::size_t revive = position_of(record, "if (written->fill_inserted && pc.revive_market");
    ASSERT_NE(revive, std::string::npos);
    EXPECT_LT(position_of(record, "const auto written = db_->book_take("), revive);
    // The settle step forgets a take only once its booking committed.
    const std::string settle = function_body(
        raw, "asio::awaitable<void> Engine::step_settle_pending_takes(BlockHeight block_height)");
    EXPECT_NE(position_of(settle, "proof->coin_ids, \"take:\" + p.trade_id, &booked);"),
              std::string::npos);
    EXPECT_NE(position_of(settle, "if (booked) pending_takes_.erase(trade_id);"), std::string::npos);
    // The sweep sees a booking with every leg suppressed through its row.
    const std::string sweep = function_body(
        raw, "asio::awaitable<void> Engine::step_sweep_missed_takes(BlockHeight block_height)");
    EXPECT_EQ(count_of(sweep, "db_->taker_fill_exists(take->trade_id)"), 2u);
    EXPECT_NE(position_of(sweep, "if (booked) take_sweep_done_.insert(id);"), std::string::npos);
    // No take is sent before the last process's holds are read; the settle
    // step keeps trying to read them.
    const std::string guard = function_body(
        raw, "asio::awaitable<nlohmann::json> Engine::take_offer_guarded(");
    EXPECT_LT(position_of(guard, "if (!take_holds_restored_) {"),
              position_of(guard, "wallet_->take_offer(offer_text, intent.fee_mojos)"));
    EXPECT_NE(position_of(settle, "if (!take_holds_restored_ && !restore_take_holds()) co_return;"),
              std::string::npos);
    // A deep reward scan is stamped done only at the end of the step.
    const std::string reward = function_body(
        raw, "asio::awaitable<void> Engine::step_ingest_reward_inflows(");
    const std::size_t stamp = position_of(reward, "if (deep_scan) last_reward_deep_scan_block_ = block_height;");
    ASSERT_NE(stamp, std::string::npos);
    EXPECT_LT(position_of(reward, "persist_inventory_state();"), stamp);
    EXPECT_EQ(count_of(reward, "last_reward_deep_scan_block_ = block_height;"), 1u);
}

TEST(TakeSettlementWiring, ABookingTiesToFreshBalancesAndNoGuiTakeIsSweptTwice) {
    const std::string raw = read_engine_cpp();
    const std::string all = squeeze(raw);
    // Both steps read the balances a booking moves after its proof and
    // before it books; a read that fails books nothing this time.
    const std::string settle = function_body(
        raw, "asio::awaitable<void> Engine::step_settle_pending_takes(BlockHeight block_height)");
    const std::size_t settle_fresh = position_of(
        settle, "if (!co_await refresh_take_balances(std::move(moved), block_height)) {");
    ASSERT_NE(settle_fresh, std::string::npos);
    EXPECT_LT(position_of(settle, "proof = prove_take_receipts(*take, coins);"), settle_fresh);
    EXPECT_LT(settle_fresh, position_of(settle, "record_taker_fill("));
    EXPECT_NE(position_of(settle, "if (p.fee_mojos > 0) moved.emplace_back(\"xch\");"),
              std::string::npos);
    const std::string sweep = function_body(
        raw, "asio::awaitable<void> Engine::step_sweep_missed_takes(BlockHeight block_height)");
    const std::size_t sweep_fresh = position_of(
        sweep, "if (!co_await refresh_take_balances({pc->base_asset_id, pc->quote_asset_id}, "
               "block_height)) {");
    ASSERT_NE(sweep_fresh, std::string::npos);
    EXPECT_LT(position_of(sweep, "proof = prove_take_receipts(*take, coins);"), sweep_fresh);
    // The re-check after the awaits comes after this one too.
    EXPECT_LT(sweep_fresh, sweep.rfind(squeeze("in_ledger(take->trade_id)")));
    EXPECT_LT(sweep_fresh, position_of(sweep, "record_taker_fill("));
    const std::string fresh = function_body(
        raw, "asio::awaitable<bool> Engine::refresh_take_balances(");
    EXPECT_NE(position_of(fresh, "(void)co_await fetch_bridge_balance(asset, block_height, "
                                 "&validated); if (!validated) co_return false;"),
              std::string::npos);
    // Every GUI take it may still confirm is the GUI's, and a store that
    // cannot be read stops the sweep before it reads the wallet.
    const std::size_t owners = position_of(sweep, "const auto ids = read_gui_take_ids(store); "
                                                  "if (!ids) {");
    ASSERT_NE(owners, std::string::npos);
    const std::size_t deferred = sweep.find(squeeze("co_return;"), owners);
    EXPECT_LT(deferred, position_of(sweep, "co_await wallet_->get_recent_takes("));
    EXPECT_EQ(count_of(all, "SELECT trade_id FROM dexie_takes WHERE status <> 'failed';"), 1u);
    EXPECT_EQ(count_of(sweep, "read_topup_take_rows("), 0u);
    // ... and the top-up ingest books nothing the sweep booked.
    const std::string topup = function_body(
        raw, "asio::awaitable<void> Engine::step_ingest_topup_takes(BlockHeight block_height)");
    const std::size_t swept = position_of(topup, "swept != legs.event_id && db_->ledger_has_event(swept)");
    ASSERT_NE(swept, std::string::npos);
    EXPECT_LT(swept, position_of(topup, "db_->append_ledger_entries(entries)"));
    // The invariant does not run before the saved holds are read back.
    const std::string invariant = function_body(
        raw, "void Engine::step_check_ledger_invariant(BlockHeight block_height)");
    const std::size_t waits = position_of(invariant, "if (!take_holds_restored_) {");
    ASSERT_NE(waits, std::string::npos);
    EXPECT_LT(waits, position_of(invariant, "db_->ledger_balances()"));
}

TEST(TakeSettlementWiring, TheHoldIsWrittenBeforeTheTakeIsSent) {
    const std::string raw = read_engine_cpp();
    const std::string guard = function_body(
        raw, "asio::awaitable<nlohmann::json> Engine::take_offer_guarded(");
    ASSERT_FALSE(guard.empty());
    const std::size_t held = position_of(
        guard, "if (!hold_uncertain_take(counterparty_offer_id, intent, block_height)) {");
    const std::size_t sent = position_of(guard, "wallet_->take_offer(offer_text, intent.fee_mojos)");
    ASSERT_NE(held, std::string::npos);
    ASSERT_NE(sent, std::string::npos);
    EXPECT_LT(held, sent);
    // A definite refusal releases it; a possibly-submitted failure keeps it.
    const std::size_t refused = position_of(guard, "catch (const rpc::ChiaRPCApplicationError&)");
    const std::size_t transport = position_of(guard, "catch (const rpc::ChiaRPCTransportError& e)");
    ASSERT_NE(refused, std::string::npos);
    ASSERT_NE(transport, std::string::npos);
    EXPECT_LT(guard.find(squeeze("release_uncertain_take(counterparty_offer_id);"), refused),
              transport);
    EXPECT_NE(position_of(guard, "if (!rpc::request_possibly_submitted(e.curl_code(), "
                                 "e.http_code())) { release_uncertain_take(counterparty_offer_id);"),
              std::string::npos);
    // Tracking a take releases the request's hold once the pending row is in.
    const std::string track = function_body(raw, "void Engine::track_pending_take(");
    EXPECT_NE(position_of(track, "if (written) release_uncertain_take(counterparty_offer_id);"),
              std::string::npos);
    // A settled take whose record does not map onto its pair stays tracked.
    const std::string settle = function_body(
        raw, "asio::awaitable<void> Engine::step_settle_pending_takes(BlockHeight block_height)");
    const std::size_t unmapped = position_of(settle, "if (!legs || !price) {");
    const std::size_t erased = position_of(settle, "pending_takes_.erase(it);");
    ASSERT_NE(unmapped, std::string::npos);
    EXPECT_LT(unmapped, erased);
}

TEST(TakeSettlementWiring, HoldsAreWrittenReadBackAndKeptWhileUncertain) {
    const std::string raw = read_engine_cpp();
    const std::string all = squeeze(raw);
    // An unanswered request, and a reply with no trade id, both hold the offer.
    const std::string guard = function_body(
        raw, "asio::awaitable<nlohmann::json> Engine::take_offer_guarded(");
    EXPECT_NE(position_of(guard, "hold_uncertain_take(counterparty_offer_id, intent, block_height)"),
              std::string::npos);
    const std::string track = function_body(raw, "void Engine::track_pending_take(");
    EXPECT_NE(position_of(track, "(void)hold_uncertain_take(counterparty_offer_id, intent, "
                                 "block_height);"), std::string::npos);
    EXPECT_NE(position_of(track, "db_->save_take_hold(row)"), std::string::npos);
    // Every way a hold ends deletes it.
    const std::string settle = function_body(
        raw, "asio::awaitable<void> Engine::step_settle_pending_takes(BlockHeight block_height)");
    EXPECT_NE(position_of(settle, "db_->delete_take_hold(\"uncertain:\" + it->first);"),
              std::string::npos);
    EXPECT_NE(position_of(settle, "db_->delete_take_hold(\"take:\" + p.trade_id);"),
              std::string::npos);
    EXPECT_NE(position_of(settle, "polled->second.last_checked_block = block_height;"),
              std::string::npos);
    // Read back at start.
    EXPECT_EQ(count_of(all, "restore_take_holds();"), 1u);
    // The invariant widens its tolerance by the live uncertain holds.
    const std::string invariant = function_body(
        raw, "void Engine::step_check_ledger_invariant(BlockHeight block_height)");
    EXPECT_NE(position_of(invariant, "execution::take_hold_slack(pending_takes_, "
                                     "uncertain_take_offers_, block_height);"), std::string::npos);
    EXPECT_EQ(count_of(invariant, "exposure + fee_slack + held_slack"), 2u);
    EXPECT_EQ(count_of(invariant, "exposure + fee_slack + inflight_slack"), 0u);
    // A take inside an asset's opening books no leg (the fill path's rule).
    const std::string record = function_body(raw, "void Engine::record_taker_fill(");
    EXPECT_NE(position_of(record, "block_height > 0 && block_height <= git->second"),
              std::string::npos);
}
