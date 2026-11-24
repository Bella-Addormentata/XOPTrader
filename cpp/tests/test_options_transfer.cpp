// test_options_transfer.cpp -- Unit tests for the options sub-account's
// capital flows ([OPTIONS-CAPITAL 2026-11-23]).
//
// The rows below are the three top-ups in the live options database on
// 2026-11-23 (events 7, 8 and 13, with their confirmations 9, 10 and 14):
// 1.5 XCH and 2 wUSDC.b on 2026-11-20, 0.5 XCH on 2026-11-21, a fee of
// 10,000,000 mojos each.  None of them had a ledger entry.

#include <gtest/gtest.h>

#include <xop/accounting/bridge_ingest.hpp>
#include <xop/accounting/options_transfer.hpp>

#include "engine_source_scan.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace xop::accounting;
using xop::Mojo;

constexpr const char* kUsdc = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";
constexpr const char* kTxXch =
    "2bbcfcaa7c11b312edf1a37cb95da12ecacfdb313b04fd5adb9a0f90f0898bf6";
constexpr const char* kTxUsdc =
    "a7939db646490f5b1495aa90edb09569e779bf77b58693e33d986aee945a89d7";

OptionsTopUpRow xch_top_up()
{
    OptionsTopUpRow row;
    row.sent_at = 1'795'214'207;       // 2026-11-20T22:36:47Z
    row.confirmed_at = 1'795'214'519;  // 2026-11-20T22:41:59Z
    row.detail_json = std::string{R"({"amount": 1500000000000, "asset": "XCH", "fee": 10000000, )"}
                      + R"("transaction": "0x)" + kTxXch + R"("})";
    return row;
}

OptionsTopUpRow quote_top_up()
{
    OptionsTopUpRow row;
    row.sent_at = 1'795'214'207;
    row.confirmed_at = 1'795'214'519;
    row.detail_json = std::string{R"({"amount": 2000, "asset": "quote asset", "fee": 10000000, )"}
                      + R"("transaction": "0x)" + kTxUsdc + R"("})";
    return row;
}

OptionsTopUpRow with_detail(const std::string& detail)
{
    OptionsTopUpRow row = xch_top_up();
    row.detail_json = detail;
    return row;
}

}  // namespace

// ============================================================================
// Time
// ============================================================================

TEST(OptionsTransfer, AUnixTimeReadsAsUtcIso)
{
    EXPECT_EQ(iso_from_unix(946'684'800), "2000-01-01T00:00:00Z");
    EXPECT_EQ(iso_from_unix(1'709'164'800), "2024-02-29T00:00:00Z") << "a leap day";
    EXPECT_EQ(iso_from_unix(1'709'251'199), "2024-02-29T23:59:59Z");
    EXPECT_EQ(iso_from_unix(1'795'766'400), "2026-11-27T08:00:00Z");
    EXPECT_EQ(iso_from_unix(1'795'214'207), "2026-11-20T22:36:47Z");
    EXPECT_EQ(iso_from_unix(1'798'761'599), "2026-12-31T23:59:59Z");
    EXPECT_EQ(iso_from_unix(1'798'761'600), "2027-01-01T00:00:00Z");

    // Not a time the book writes: nothing to order by.
    EXPECT_EQ(iso_from_unix(0), "");
    EXPECT_EQ(iso_from_unix(-1), "");
    EXPECT_EQ(iso_from_unix(946'684'799), "");
    EXPECT_EQ(iso_from_unix(7'258'118'400), "");

    // The ledger's own stamps compare with it.
    EXPECT_TRUE(iso_strictly_after(iso_from_unix(1'795'214'519), iso_from_unix(1'795'214'207)));
    EXPECT_TRUE(iso_strictly_after(iso_from_unix(1'795'214'207), "2026-08-23T04:16:11.144Z"));
    EXPECT_FALSE(iso_strictly_after(iso_from_unix(1'795'214'207), "2026-11-20T22:36:47.000Z"))
        << "a tie is not after";
}

// ============================================================================
// A top-up
// ============================================================================

TEST(OptionsTransfer, AnXchTopUpIsAWithdrawalOfTheAmountAndItsFee)
{
    const OptionsFlow flow = classify_options_topup(xch_top_up(), kUsdc);
    ASSERT_EQ(flow.status, OptionsFlowStatus::Ready);
    EXPECT_EQ(flow.event_type, "options_withdrawal");
    EXPECT_EQ(flow.event_id, std::string{"options_topup:"} + kTxXch);
    EXPECT_EQ(flow.floor_iso, "2026-11-20T22:36:47Z") << "the send";
    EXPECT_EQ(flow.at_iso, "2026-11-20T22:41:59Z") << "the confirmation";
    ASSERT_EQ(flow.legs.size(), 2u);
    EXPECT_EQ(flow.legs[0].leg, "options");
    EXPECT_EQ(flow.legs[0].asset_id, "xch");
    EXPECT_EQ(flow.legs[0].delta_mojos, -1'500'000'000'000);
    EXPECT_EQ(flow.legs[1].leg, "fee");
    EXPECT_EQ(flow.legs[1].asset_id, "xch");
    EXPECT_EQ(flow.legs[1].delta_mojos, -10'000'000);
}

TEST(OptionsTransfer, AQuoteTopUpMovesTheQuoteAssetAndPaysItsFeeInXch)
{
    const OptionsFlow flow = classify_options_topup(quote_top_up(), std::string{"0x"} + kUsdc);
    ASSERT_EQ(flow.status, OptionsFlowStatus::Ready);
    EXPECT_EQ(flow.event_id, std::string{"options_topup:"} + kTxUsdc);
    ASSERT_EQ(flow.legs.size(), 2u);
    EXPECT_EQ(flow.legs[0].leg, "options");
    EXPECT_EQ(flow.legs[0].asset_id, kUsdc) << "options.quote_asset_id, lowercase, no 0x";
    EXPECT_EQ(flow.legs[0].delta_mojos, -2'000);
    EXPECT_EQ(flow.legs[1].leg, "fee");
    EXPECT_EQ(flow.legs[1].asset_id, "xch");
    EXPECT_EQ(flow.legs[1].delta_mojos, -10'000'000);
}

TEST(OptionsTransfer, ATopUpWithoutAFeeHasOneLeg)
{
    const OptionsFlow none = classify_options_topup(
        with_detail(std::string{R"({"amount": 5, "asset": "XCH", "transaction": ")"} + kTxXch + R"("})"),
        kUsdc);
    ASSERT_EQ(none.status, OptionsFlowStatus::Ready);
    ASSERT_EQ(none.legs.size(), 1u);
    EXPECT_EQ(none.legs[0].delta_mojos, -5);

    const OptionsFlow zero = classify_options_topup(
        with_detail(std::string{R"({"amount": 5, "asset": "XCH", "fee": 0, "transaction": ")"} + kTxXch
                    + R"("})"),
        kUsdc);
    ASSERT_EQ(zero.status, OptionsFlowStatus::Ready);
    EXPECT_EQ(zero.legs.size(), 1u);
}

TEST(OptionsTransfer, AnUnconfirmedTopUpWaits)
{
    OptionsTopUpRow row = xch_top_up();
    row.confirmed_at = 0;
    const OptionsFlow flow = classify_options_topup(row, kUsdc);
    EXPECT_EQ(flow.status, OptionsFlowStatus::Pending);
    EXPECT_EQ(flow.event_id, std::string{"options_topup:"} + kTxXch) << "it is known all the same";
    EXPECT_EQ(flow.at_iso, "");

    // A confirmation stamped before its own send is not one.
    row.confirmed_at = row.sent_at - 1;
    EXPECT_EQ(classify_options_topup(row, kUsdc).status, OptionsFlowStatus::Pending);
    row.confirmed_at = row.sent_at;
    EXPECT_EQ(classify_options_topup(row, kUsdc).status, OptionsFlowStatus::Ready);
}

TEST(OptionsTransfer, ARowThatCannotBeReadIsNeverBooked)
{
    const auto invalid = [](const std::string& detail, const char* quote = kUsdc) {
        return classify_options_topup(with_detail(detail), quote).status == OptionsFlowStatus::Invalid;
    };
    const std::string tx = std::string{R"("transaction": ")"} + kTxXch + R"(")";
    EXPECT_TRUE(invalid("not json"));
    EXPECT_TRUE(invalid("[1, 2]"));
    EXPECT_TRUE(invalid("{}"));
    // No transaction, or none that is a transaction id: no idempotency key.
    EXPECT_TRUE(invalid(R"({"amount": 5, "asset": "XCH"})"));
    EXPECT_TRUE(invalid(R"({"amount": 5, "asset": "XCH", "transaction": null})"));
    EXPECT_TRUE(invalid(R"({"amount": 5, "asset": "XCH", "transaction": "unknown"})"));
    EXPECT_TRUE(invalid(R"({"amount": 5, "asset": "XCH", "transaction": ""})"));
    // An asset the book does not name, an amount that is not a positive
    // integer, a fee that is not one.
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": 5, "asset": "BYC"})"));
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": 5})"));
    EXPECT_TRUE(invalid("{" + tx + R"(, "asset": "XCH"})"));
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": 0, "asset": "XCH"})"));
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": -5, "asset": "XCH"})"));
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": 1.5, "asset": "XCH"})"));
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": "5", "asset": "XCH"})"));
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": 18446744073709551615, "asset": "XCH"})"));
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": 5, "asset": "XCH", "fee": -1})"));
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": 5, "asset": "XCH", "fee": "auto"})"));
    // The quote asset must be known to say which asset moved.
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": 5, "asset": "quote asset"})", ""));
    EXPECT_TRUE(invalid("{" + tx + R"(, "amount": 5, "asset": "quote asset"})", "wUSDC.b"));
    EXPECT_FALSE(invalid("{" + tx + R"(, "amount": 5, "asset": "XCH"})", ""))
        << "an XCH top-up needs no quote asset";

    // A send with no usable time has no place in the ledger's order.
    OptionsTopUpRow undated = xch_top_up();
    undated.sent_at = 0;
    EXPECT_EQ(classify_options_topup(undated, kUsdc).status, OptionsFlowStatus::Invalid);
}

TEST(OptionsTransfer, TheTransactionIsTheKeyHoweverItIsWritten)
{
    std::string upper = kTxXch;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const OptionsFlow bare = classify_options_topup(
        with_detail(std::string{R"({"amount": 5, "asset": "XCH", "transaction": ")"} + upper + R"("})"),
        kUsdc);
    ASSERT_EQ(bare.status, OptionsFlowStatus::Ready);
    EXPECT_EQ(bare.event_id, classify_options_topup(xch_top_up(), kUsdc).event_id);
}

// ============================================================================
// The fence going up or down
// ============================================================================

TEST(OptionsTransfer, TheFenceGoingDownReturnsWhatTheSubAccountHeld)
{
    const OptionsHeld held{{"xch", 500'000'000'000}, {kUsdc, 2'000}};
    const OptionsFlow down = options_fence_flow(/*went_up=*/false, 3, held, "2026-11-24T10:00:00.250Z");
    ASSERT_EQ(down.status, OptionsFlowStatus::Ready);
    EXPECT_EQ(down.event_type, "options_deposit");
    EXPECT_EQ(down.event_id, "options_fence_down:3");
    EXPECT_EQ(down.floor_iso, "2026-11-24T10:00:00.250Z") << "the moment of the transition's list";
    EXPECT_EQ(down.at_iso, down.floor_iso);
    ASSERT_EQ(down.legs.size(), 2u);
    EXPECT_EQ(down.legs[0].leg, "options");
    EXPECT_EQ(down.legs[0].asset_id, "xch");
    EXPECT_EQ(down.legs[0].delta_mojos, 500'000'000'000);
    EXPECT_EQ(down.legs[1].asset_id, kUsdc);
    EXPECT_EQ(down.legs[1].delta_mojos, 2'000);

    const OptionsFlow up = options_fence_flow(/*went_up=*/true, 4, held, "2026-11-24T11:00:00.000Z");
    ASSERT_EQ(up.status, OptionsFlowStatus::Ready);
    EXPECT_EQ(up.event_type, "options_withdrawal");
    EXPECT_EQ(up.event_id, "options_fence_up:4");
    ASSERT_EQ(up.legs.size(), 2u);
    EXPECT_EQ(up.legs[0].delta_mojos, -500'000'000'000);
    EXPECT_EQ(up.legs[1].delta_mojos, -2'000);

    // An amount no Mojo holds is held at the most one does, never wrapped.
    const OptionsFlow huge = options_fence_flow(
        true, 5, {{"xch", std::numeric_limits<std::uint64_t>::max()}}, "2026-11-24T11:00:00.000Z");
    ASSERT_EQ(huge.legs.size(), 1u);
    EXPECT_EQ(huge.legs[0].delta_mojos, -std::numeric_limits<Mojo>::max());
}

TEST(OptionsTransfer, AFenceEventIsWrittenAlsoWithNothingBehindIt)
{
    // The event is the record of which way the fence went, so it always has
    // a leg: with nothing held, one zero leg on XCH. The same shape adopts
    // the state the fence is found in when the books hold no fence event.
    for (const auto& held : {OptionsHeld{}, OptionsHeld{{"xch", 0}, {kUsdc, 0}}}) {
        const OptionsFlow nothing = options_fence_flow(false, 1, held, "2026-11-24T10:00:00.000Z");
        ASSERT_EQ(nothing.status, OptionsFlowStatus::Ready);
        EXPECT_EQ(nothing.event_id, "options_fence_down:1");
        EXPECT_EQ(nothing.event_type, "options_deposit");
        ASSERT_EQ(nothing.legs.size(), 1u);
        EXPECT_EQ(nothing.legs[0].leg, "options");
        EXPECT_EQ(nothing.legs[0].asset_id, "xch");
        EXPECT_EQ(nothing.legs[0].delta_mojos, 0);
    }
    // Only what is held has a leg otherwise.
    const OptionsFlow one = options_fence_flow(true, 2, {{"xch", 0}, {kUsdc, 2'000}, {"", 9}},
                                               "2026-11-24T10:00:00.000Z");
    ASSERT_EQ(one.status, OptionsFlowStatus::Ready);
    ASSERT_EQ(one.legs.size(), 1u);
    EXPECT_EQ(one.legs[0].asset_id, kUsdc);
    EXPECT_EQ(one.legs[0].delta_mojos, -2'000);
    // No number, no time: no event id to book under.
    EXPECT_EQ(options_fence_flow(true, 0, {{"xch", 5}}, "2026-11-24T10:00:00.000Z").status,
              OptionsFlowStatus::Invalid);
    EXPECT_EQ(options_fence_flow(true, 1, {{"xch", 5}}, "").status, OptionsFlowStatus::Invalid);
}

TEST(OptionsTransfer, TheBooksStandWhereTheNewestFenceEventPutThem)
{
    EXPECT_EQ(booked_fence_state(""), OptionsFenceBooked::Unknown) << "no fence event yet";
    EXPECT_EQ(booked_fence_state("reward:0xabc"), OptionsFenceBooked::Unknown);
    EXPECT_EQ(booked_fence_state("options_fence_up:2"), OptionsFenceBooked::Up);
    EXPECT_EQ(booked_fence_state("options_fence_down:3"), OptionsFenceBooked::Down);
    // A top-up says nothing about it: one booked late, after the fence went
    // down, must not turn the books back to "up".
    EXPECT_EQ(booked_fence_state(std::string{"options_topup:"} + kTxXch),
              OptionsFenceBooked::Unknown);
}

TEST(OptionsTransfer, FenceEventsAreNumberedInOrder)
{
    EXPECT_EQ(next_fence_sequence(""), 1);
    EXPECT_EQ(next_fence_sequence("options_fence_up:1"), 2);
    EXPECT_EQ(next_fence_sequence("options_fence_down:41"), 42);
    // Not a fence event, or one with no number: start over at 1.
    EXPECT_EQ(next_fence_sequence(std::string{"options_topup:"} + kTxXch), 1);
    EXPECT_EQ(next_fence_sequence("options_fence_up:"), 1);
    EXPECT_EQ(next_fence_sequence("options_fence_up:x7"), 1);
    EXPECT_EQ(next_fence_sequence("options_fence_up:999999999999999999999"), 1);
}

// ============================================================================
// Which top-ups are booked
// ============================================================================

TEST(OptionsTransfer, TheStampIsTheNewestFenceUpEventThatTookSomething)
{
    EXPECT_EQ(fence_up_stamp("options_fence_up:4", "2026-11-24T11:00:00.000Z"),
              "2026-11-24T11:00:00.000Z");
    // Event #1 only adopted the state the fence was found in. It took
    // nothing, so no top-up is inside it.
    EXPECT_EQ(fence_up_stamp("options_fence_up:1", "2026-11-24T11:00:00.000Z"), "");
    EXPECT_EQ(fence_up_stamp("options_fence_up:10", "2026-11-24T11:00:00.000Z"),
              "2026-11-24T11:00:00.000Z")
        << "#10 is not #1";
    // None in the books, or not a fence-up event at all.
    EXPECT_EQ(fence_up_stamp("", ""), "");
    EXPECT_EQ(fence_up_stamp("options_fence_down:2", "2026-11-24T11:00:00.000Z"), "");
}

TEST(OptionsTransfer, ATopUpSentBeforeTheFenceWentUpIsInsideThatEvent)
{
    // The fence-up event took what its list held. A top-up sent before that
    // list went out while the engine was not fencing the address, so its
    // coins are in what the event took: booking it too would count them
    // twice, for good.
    const std::string stamp = "2026-11-24T10:00:00.250Z";
    EXPECT_TRUE(topup_inside_fence_event("2026-11-24T09:59:59Z", stamp));
    // Beside a running engine the book sends only once that list is
    // published -- in the same second at the earliest. That one left the
    // trading funds, and is booked.
    EXPECT_FALSE(topup_inside_fence_event("2026-11-24T10:00:00Z", stamp)) << "a tie is after";
    EXPECT_FALSE(topup_inside_fence_event("2026-11-24T10:00:01Z", stamp));
    // No such event in the books: nothing to be inside of.
    EXPECT_FALSE(topup_inside_fence_event(classify_options_topup(xch_top_up(), kUsdc).floor_iso, ""));
    // A stamp that cannot be read is not booked on a guess.
    EXPECT_TRUE(topup_inside_fence_event("", stamp));
    EXPECT_TRUE(topup_inside_fence_event("2026-11-24T10:00:01Z", "soon"));
}

TEST(OptionsTransfer, ATopUpIsCapitalThatLeftOnlyIfTheFenceWasUp)
{
    const std::string sent = classify_options_topup(xch_top_up(), kUsdc).floor_iso;

    // The first run on the live record: the fence is found up, the books
    // begin there (event #1, no stamp), and the three top-ups book.
    EXPECT_EQ(judge_topup(OptionsFenceBooked::Up, sent, fence_up_stamp("options_fence_up:1", sent)),
              OptionsTopUpVerdict::Book);
    // The fence up all along, since an event long before the send.
    EXPECT_EQ(judge_topup(OptionsFenceBooked::Up, sent, "2026-11-19T08:00:00.000Z"),
              OptionsTopUpVerdict::Book);

    // The books stand fence-down: its coins are trading funds until the
    // fence goes up, and that event takes them. Whenever it was sent.
    EXPECT_EQ(judge_topup(OptionsFenceBooked::Down, sent, ""), OptionsTopUpVerdict::Deferred);
    EXPECT_EQ(judge_topup(OptionsFenceBooked::Down, sent, "2026-11-19T08:00:00.000Z"),
              OptionsTopUpVerdict::Deferred);
    // ...and once it has gone up, with a list taken after the send:
    EXPECT_EQ(judge_topup(OptionsFenceBooked::Up, sent, "2026-11-21T08:00:00.000Z"),
              OptionsTopUpVerdict::Inside);

    // The state is adopted before any top-up is judged; were it not, nothing
    // is booked on an unknown one.
    EXPECT_EQ(judge_topup(OptionsFenceBooked::Unknown, sent, ""), OptionsTopUpVerdict::Deferred);
}

TEST(OptionsTransfer, ATopUpIsConfirmedByItsOwnTransaction)
{
    // The wallet's record of the send (get_transaction): this transaction's
    // confirmation and the coins this transaction created. Another coin of
    // the same amount at the address proves nothing about it.
    const OptionsFlow topup = classify_options_topup(xch_top_up(), kUsdc);
    ASSERT_EQ(topup_amount(topup), std::optional<std::uint64_t>{1'500'000'000'000});
    const std::string options_ph = "b18cdf6985b3a061e0c89b671d93f334aedf5be86ce1e79e2297d55f2528cc55";
    const nlohmann::json record = nlohmann::json::parse(R"({
        "confirmed": true, "confirmed_at_height": 9601234, "amount": 1500000000000,
        "fee_amount": 10000000, "name": "0x2bbcfcaa7c11b312edf1a37cb95da12ecacfdb313b04fd5adb9a0f90f0898bf6",
        "additions": [
            {"parent_coin_info": "0xAAAA", "puzzle_hash": "0xb18cdf6985b3a061e0c89b671d93f334aedf5be86ce1e79e2297d55f2528cc55",
             "amount": 1500000000000},
            {"parent_coin_info": "0xAAAA", "puzzle_hash": "0xcccc", "amount": 7399990000000},
            {"parent_coin_info": "0xAAAA", "puzzle_hash": "0xdddd", "amount": 1500000000000},
            {"puzzle_hash": "0xeeee", "amount": 1500000000000},
            {"parent_coin_info": "0xAAAA", "puzzle_hash": "0xffff", "amount": "1500000000000"},
            "not a coin"
        ]})");
    const auto proof = read_topup_transaction(record, topup);
    ASSERT_TRUE(proof);
    EXPECT_TRUE(proof->confirmed);
    // Every coin of exactly the amount sent. Which of them is the top-up's
    // is the caller's to say: the one at the options address.
    ASSERT_EQ(proof->outputs.size(), 2u);
    EXPECT_EQ(proof->outputs[0].parent_coin_info, "0xAAAA");
    EXPECT_EQ(proof->outputs[0].puzzle_hash, "0x" + options_ph);
    EXPECT_EQ(proof->outputs[0].amount, 1'500'000'000'000);
    EXPECT_EQ(proof->outputs[1].puzzle_hash, "0xdddd");

    // Not confirmed yet is an answer too.
    const auto waiting = read_topup_transaction(
        nlohmann::json::parse(R"({"confirmed": false, "additions": []})"), topup);
    ASSERT_TRUE(waiting);
    EXPECT_FALSE(waiting->confirmed);
    EXPECT_TRUE(waiting->outputs.empty());
    // A record without its coins still says whether it confirmed.
    const auto bare = read_topup_transaction(nlohmann::json::parse(R"({"confirmed": true})"), topup);
    ASSERT_TRUE(bare);
    EXPECT_TRUE(bare->confirmed);
    EXPECT_TRUE(bare->outputs.empty());

    // No answer is never read as either.
    EXPECT_FALSE(read_topup_transaction(nlohmann::json::object(), topup));
    EXPECT_FALSE(read_topup_transaction(nlohmann::json::parse(R"({"confirmed": "yes"})"), topup));
    EXPECT_FALSE(read_topup_transaction(nlohmann::json::parse(R"({"confirmed": 1})"), topup));
    EXPECT_FALSE(read_topup_transaction(nlohmann::json::array(), topup));
    EXPECT_FALSE(read_topup_transaction(nlohmann::json(), topup));

    // The quote asset's top-up is looked for by its own amount.
    const OptionsFlow quote = classify_options_topup(quote_top_up(), kUsdc);
    EXPECT_EQ(topup_amount(quote), std::optional<std::uint64_t>{2'000});
    EXPECT_TRUE(read_topup_transaction(record, quote)->outputs.empty());
    // Only a top-up has a coin to look for.
    const OptionsFlow fence = options_fence_flow(false, 2, {{"xch", 5}}, "2026-11-24T10:00:00.000Z");
    EXPECT_FALSE(topup_amount(fence));
    EXPECT_FALSE(topup_amount(OptionsFlow{}));
    ASSERT_TRUE(read_topup_transaction(record, fence));
    EXPECT_TRUE(read_topup_transaction(record, fence)->outputs.empty());
    OptionsFlow odd = topup;
    odd.legs.front().delta_mojos = std::numeric_limits<Mojo>::min();
    EXPECT_FALSE(topup_amount(odd));
}

TEST(OptionsTransfer, AConfirmedTopUpIsPlacedByItsOwnCoinWhenTheFenceGoesDown)
{
    const double lowered = 1'795'300'000.5;
    // The list the fence-down event books holds the top-up's own coin: it
    // arrived before the fence went down, whatever anyone recorded when.
    EXPECT_TRUE(topup_arrived_before_lowering(true, 0, lowered));
    EXPECT_TRUE(topup_arrived_before_lowering(true, 1'795'399'999, lowered));
    // It does not: the coin arrived with the fence down, and is trading
    // funds until the fence goes up and takes it.
    EXPECT_FALSE(topup_arrived_before_lowering(false, 0, lowered));
    EXPECT_FALSE(topup_arrived_before_lowering(false, 1'795'200'000, lowered))
        << "the coin decides, not the book's clock";

    // The wallet's record does not show the coin. Only a confirmation the
    // options book wrote down no later than the fence went down says
    // "before".
    EXPECT_TRUE(topup_arrived_before_lowering(std::nullopt, 1'795'299'999, lowered));
    EXPECT_TRUE(topup_arrived_before_lowering(std::nullopt, 1'795'300'000, lowered));
    EXPECT_FALSE(topup_arrived_before_lowering(std::nullopt, 1'795'300'001, lowered));
    EXPECT_FALSE(topup_arrived_before_lowering(std::nullopt, 0, lowered)) << "no record at all";
}

TEST(OptionsTransfer, TheWalletIsAskedEveryHeartbeatOnlyWhileATopUpMayBeOnItsWay)
{
    // Within two hours of the send: every heartbeat.
    EXPECT_TRUE(topup_proof_due(/*recent=*/true, 9'600'100, 9'600'099));
    EXPECT_TRUE(topup_proof_due(true, 9'600'100, 0));
    // After that the options book has forgotten the send, and it can still
    // confirm: once in this process, then every kOptionsProofEveryBlocks.
    EXPECT_TRUE(topup_proof_due(false, 9'600'100, 0)) << "never asked in this process";
    EXPECT_FALSE(topup_proof_due(false, 9'600'100, 9'600'099));
    EXPECT_FALSE(topup_proof_due(false, 9'600'100 + kOptionsProofEveryBlocks - 1, 9'600'100));
    EXPECT_TRUE(topup_proof_due(false, 9'600'100 + kOptionsProofEveryBlocks, 9'600'100));
    EXPECT_TRUE(topup_proof_due(false, 9'600'000, 9'600'100)) << "a height that ran backwards";
    EXPECT_EQ(kOptionsProofEveryBlocks, 120u);
}

TEST(OptionsTransfer, TheFenceLogIsTakenInOrderAgainstWhereTheBooksStand)
{
    using Books = OptionsFenceBooked;
    using Step = OptionsLogStep;
    // No fence event yet: the books begin in the entry's state.
    EXPECT_EQ(judge_transition(Books::Unknown, true, false, false), Step::Adopt);
    EXPECT_EQ(judge_transition(Books::Unknown, false, false, true), Step::Adopt);
    // The books already stand where the entry puts the fence.
    EXPECT_EQ(judge_transition(Books::Up, true, true, false), Step::Skip);
    EXPECT_EQ(judge_transition(Books::Up, true, false, false), Step::Skip)
        << "found up with the books up needs no list";
    EXPECT_EQ(judge_transition(Books::Down, false, false, true), Step::Skip);
    // It went down: a deposit of its last list (or of the address as it is).
    EXPECT_EQ(judge_transition(Books::Up, false, true, false), Step::BookDown);
    EXPECT_EQ(judge_transition(Books::Up, false, false, false), Step::BookDown);
    // It went up: a withdrawal of its first list, once there is one...
    EXPECT_EQ(judge_transition(Books::Down, true, true, false), Step::BookUp);
    EXPECT_EQ(judge_transition(Books::Down, true, true, true), Step::BookUp);
    EXPECT_EQ(judge_transition(Books::Down, true, false, false), Step::Wait);
    // ...and if it went down again before any list was had, no balance was
    // read behind it: the two entries go together.
    EXPECT_EQ(judge_transition(Books::Down, true, false, true), Step::DropPair);

    // The case the log exists for: down and up again between two passes.
    // The books stand up and the fence stands up, and both changes book.
    Books books = Books::Up;
    EXPECT_EQ(judge_transition(books, /*entry_up=*/false, true, true), Step::BookDown);
    books = booked_fence_state("options_fence_down:7");
    EXPECT_EQ(judge_transition(books, /*entry_up=*/true, true, false), Step::BookUp);
    books = booked_fence_state("options_fence_up:8");
    EXPECT_EQ(books, Books::Up);
}

TEST(OptionsTransfer, AnUnbookedTopUpWidensTheToleranceForTwoHours)
{
    const std::int64_t sent = 1'795'214'207;
    EXPECT_TRUE(topup_slack_applies(sent, sent));
    EXPECT_TRUE(topup_slack_applies(sent, sent + 312)) << "the live lag to the recorded confirmation";
    EXPECT_TRUE(topup_slack_applies(sent, sent + kOptionsPendingSlackSeconds));
    // A send that never confirmed stops loosening the control.
    EXPECT_FALSE(topup_slack_applies(sent, sent + kOptionsPendingSlackSeconds + 1));
    // A clock that ran backwards, or no send time at all.
    EXPECT_FALSE(topup_slack_applies(sent, sent - 1));
    EXPECT_FALSE(topup_slack_applies(0, sent));
    EXPECT_EQ(kOptionsPendingSlackSeconds, 7'200);

    constexpr Mojo kMost = std::numeric_limits<Mojo>::max();
    EXPECT_EQ(add_slack(5, 7), 12);
    EXPECT_EQ(add_slack(5, 0), 5);
    EXPECT_EQ(add_slack(-5, 7), 7) << "a negative tolerance is none";
    EXPECT_EQ(add_slack(5, -7), 5);
    // Held well inside Mojo's range: the control adds its other terms to it.
    EXPECT_EQ(add_slack(kMost, kMost), kMost / 4);
    EXPECT_EQ(add_slack(kMost / 4, 1), kMost / 4);
    EXPECT_EQ(add_slack(kMost / 4 - 1, 1), kMost / 4);
}

// ============================================================================
// The options book's rows
// ============================================================================

TEST(OptionsTransfer, EachSendIsPairedWithItsOwnConfirmation)
{
    // The live rows: two sends in one step, their confirmations five
    // minutes on, then a third send and its confirmation.
    const std::string xch = xch_top_up().detail_json;
    const std::string usdc = quote_top_up().detail_json;
    const std::string later =
        R"({"amount": 500000000000, "asset": "XCH", "fee": 10000000, "transaction": )"
        R"("0xf3b68696e8fc484acdc859eaf8b9191db78d1dd62620b71fa743bbb0e1059ea7"})";
    const auto confirmed = [](const char* tx) {
        return std::string{R"({"asset": "XCH", "transaction": "0x)"} + tx + R"("})";
    };
    const std::vector<OptionsEventRow> events{
        {7, 1'795'214'207, "topup_sent", xch},
        {8, 1'795'214'207, "topup_sent", usdc},
        {9, 1'795'214'519, "topup_confirmed", confirmed(kTxXch)},
        {10, 1'795'214'519, "topup_confirmed", confirmed(kTxUsdc)},
        {11, 1'795'214'600, "minted", "{}"},
        {13, 1'795'221'184, "topup_sent", later},
    };
    const auto rows = join_options_topups(events);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].sent_event_id, 7);
    EXPECT_EQ(rows[0].sent_at, 1'795'214'207);
    EXPECT_EQ(rows[0].confirmed_at, 1'795'214'519);
    EXPECT_EQ(rows[1].sent_event_id, 8);
    EXPECT_EQ(rows[1].confirmed_at, 1'795'214'519);
    EXPECT_EQ(rows[2].sent_event_id, 13);
    EXPECT_EQ(rows[2].confirmed_at, 0) << "its confirmation is not written yet";
    EXPECT_EQ(classify_options_topup(rows[0], kUsdc).status, OptionsFlowStatus::Ready);
    EXPECT_EQ(classify_options_topup(rows[1], kUsdc).status, OptionsFlowStatus::Ready);
    EXPECT_EQ(classify_options_topup(rows[2], kUsdc).status, OptionsFlowStatus::Pending);

    // A confirmation counts only after its own send, and only for it.
    const std::vector<OptionsEventRow> odd{
        {1, 100, "topup_confirmed", confirmed(kTxXch)},
        {2, 200, "topup_sent", xch},
        {3, 300, "topup_confirmed", confirmed(kTxUsdc)},
        {4, 500, "topup_confirmed", confirmed(kTxXch)},
        {5, 400, "topup_confirmed", confirmed(kTxXch)},
        {6, 600, "topup_sent", "not json"},
    };
    const auto odd_rows = join_options_topups(odd);
    ASSERT_EQ(odd_rows.size(), 2u);
    EXPECT_EQ(odd_rows[0].confirmed_at, 400) << "the earliest of its own, after the send";
    EXPECT_EQ(odd_rows[1].confirmed_at, 0);
    EXPECT_EQ(classify_options_topup(odd_rows[1], kUsdc).status, OptionsFlowStatus::Invalid);
}

// ============================================================================
// The note
// ============================================================================

TEST(OptionsTransfer, TheNoteCarriesTheUsdForNetDeposits)
{
    // 1.5 XCH out at $0.95: -$1.425, which the bridge parser reads back.
    const auto val = value_bridge_flow(-1'500'000'000'000, 1e12, 0.95);
    ASSERT_GT(val.fmv_pseudo_price, 0);
    EXPECT_NEAR(val.flow_usd, -1.425, 1e-9);
    const std::string note = options_note(val.flow_usd, 0.95, "top-up 2bbcfcaa7c11b312");
    EXPECT_NE(note.find("options sub-account withdrawal"), std::string::npos);
    EXPECT_NE(note.find("ref=top-up 2bbcfcaa7c11b312"), std::string::npos);
    EXPECT_NEAR(parse_bridge_flow_usd(note), -1.425, 1e-9);

    const std::string back = options_note(1.9, 1.0, "fence down #3");
    EXPECT_NE(back.find("options sub-account deposit"), std::string::npos);
    EXPECT_NEAR(parse_bridge_flow_usd(back), 1.9, 1e-9);

    // A reference of any length cannot run the note over.
    const std::string longer = options_note(-1.0, 1.0, std::string(500, 'r'));
    EXPECT_LT(longer.size(), 224u);
    EXPECT_NEAR(parse_bridge_flow_usd(longer), -1.0, 1e-9);
}

// ============================================================================
// The engine's wiring (xop_tests constructs no Engine: engine.cpp is read)
// ============================================================================

namespace {

using xop_tests::engine_scan::count_of;
using xop_tests::engine_scan::function_body;
using xop_tests::engine_scan::position_of;
using xop_tests::engine_scan::read_engine_cpp;
using xop_tests::engine_scan::squeeze;

const char* const kStep =
    "asio::awaitable<void> Engine::step_ingest_options_transfers(BlockHeight block_height)";
const char* const kBookFlow = "asio::awaitable<Engine::OptionsBooking> Engine::book_options_flow(";
const char* const kTopUps = "asio::awaitable<bool> Engine::book_options_topups(";
const char* const kProof =
    "asio::awaitable<std::optional<Engine::OptionsProof>> Engine::options_topup_proof(";

constexpr std::size_t npos = std::string::npos;

}  // namespace

TEST(OptionsTransferWiring, TheStepRunsAfterTheBridgeScansAndBeforeTheInvariant)
{
    const std::string beat = function_body(
        read_engine_cpp(), "asio::awaitable<void> Engine::on_new_block_coro(BlockHeight block_height)");
    ASSERT_FALSE(beat.empty());
    const std::size_t takes = position_of(beat, "co_await step_ingest_topup_takes(block_height)");
    const std::size_t options = position_of(beat, "co_await step_ingest_options_transfers(block_height)");
    const std::size_t invariant = position_of(beat, "step_check_ledger_invariant(block_height)");
    ASSERT_NE(takes, npos);
    ASSERT_NE(options, npos);
    ASSERT_NE(invariant, npos);
    EXPECT_LT(takes, options);
    EXPECT_LT(options, invariant) << "a top-up is explained flow by the time the books are tied";
    EXPECT_EQ(count_of(beat, "co_await step_ingest_options_transfers(block_height)"), 1u);
}

TEST(OptionsTransferWiring, TheStepStandsDownWithTheLedgerAndTheFence)
{
    const std::string step = function_body(read_engine_cpp(), kStep);
    ASSERT_FALSE(step.empty());
    // The slack is rebuilt by every pass, also one that stands down: it is
    // taken away before any guard can return...
    const std::size_t cleared = position_of(step, "owed_before.swap(options_pending_slack_);");
    const std::size_t first_return = position_of(step, "co_return;");
    ASSERT_NE(cleared, npos);
    EXPECT_LT(cleared, first_return);
    // ...and put back by the two passes that cannot tell: a ledger that
    // could not be asked, and an options database that could not be read,
    // which is not one with no top-ups.
    EXPECT_EQ(count_of(step, "options_pending_slack_ = std::move(owed_before); co_return;"), 2u);
    const std::size_t unread = position_of(
        step, "const auto events = read_options_events(oc.database_path); if (!events) {");
    ASSERT_NE(unread, npos);

    EXPECT_NE(position_of(step, "if (!acc.ledger_enabled || !acc.options_transfer_ingest_enabled) co_return;"),
              npos);
    EXPECT_NE(position_of(step, "if (!oc.enabled || !coin_fence_) co_return;"), npos);
    EXPECT_NE(position_of(step, "if (!ledger_genesis_done_ || ledger_incomplete_) co_return;"), npos);
    // A ledger that could not be asked is not one with no fence event.
    EXPECT_NE(position_of(step, "options_books_loaded_ = load_options_books(); if (!options_books_loaded_) {"),
              npos);
    const std::string load = function_body(read_engine_cpp(), "bool Engine::load_options_books()");
    EXPECT_NE(position_of(load, "if (!newest || !newest_up) return false;"), npos);
    EXPECT_NE(position_of(load, "accounting::fence_up_stamp(newest_up->event_id, newest_up->entry_time)"),
              npos);
}

TEST(OptionsTransferWiring, ADatabaseThatCannotBeReadIsNotOneWithNoTopUps)
{
    const std::string raw = read_engine_cpp();
    const std::string reader = function_body(
        raw, "std::optional<std::vector<accounting::OptionsEventRow>> read_options_events(");
    ASSERT_FALSE(reader.empty());
    // Read-only, and never waiting long on the GUI's lock.
    EXPECT_NE(position_of(reader, "SQLITE_OPEN_READONLY"), npos);
    EXPECT_NE(position_of(reader, "sqlite3_busy_timeout(raw_db, 250);"), npos);
    // Not there: no rows.  There and unreadable: no answer.
    EXPECT_NE(position_of(reader, "if (!std::filesystem::exists(db_path, exists_ec) && !exists_ec) return rows;"),
              npos);
    EXPECT_NE(position_of(reader, "if (rc_open != SQLITE_OK) return std::nullopt;"), npos);
    EXPECT_NE(position_of(reader, "if (why.find(\"no such table\") != std::string::npos) return rows; "
                                  "return std::nullopt;"),
              npos);
    EXPECT_NE(position_of(reader, "if (rc != SQLITE_DONE) {"), npos);
    EXPECT_EQ(count_of(reader, "return std::nullopt;"), 3u);
    EXPECT_NE(position_of(reader, "WHERE kind IN ('topup_sent', 'topup_confirmed') ORDER BY id;"), npos);
}

TEST(OptionsTransferWiring, EveryFenceChangeIsBookedInOrderAndAcknowledgedOnlyThen)
{
    const std::string step = function_body(read_engine_cpp(), kStep);
    ASSERT_FALSE(step.empty());
    // The log is begun once the books and the record can be read, so its
    // first entry is the state the books compare themselves with.
    const std::size_t rows = position_of(step, "accounting::join_options_topups(*events);");
    const std::size_t begun = position_of(
        step, "if (!options_log_begun_) { coin_fence_->begin_transitions(); options_log_begun_ = true; }");
    const std::size_t log = position_of(step, "coin_fence_->transitions();");
    ASSERT_NE(rows, npos);
    ASSERT_NE(begun, npos);
    ASSERT_NE(log, npos);
    EXPECT_LT(rows, begun) << "nothing is written on a record that could not be read";
    EXPECT_LT(begun, log);
    EXPECT_EQ(count_of(step, "coin_fence_->begin_transitions();"), 1u);

    // Each entry is judged against where the books stand at that point of
    // the pass, not where the fence has ended up.
    EXPECT_NE(position_of(step, "accounting::judge_transition( "
                                "accounting::booked_fence_state(options_last_fence_event_id_), entry.up, "
                                "entry.holding.has_value(), i + 1 < log.size());"),
              npos);
    // Up books its own first list at that list's moment; down, its own last
    // list -- or the address as it is when no list says what came back.
    EXPECT_NE(position_of(step, "accounting::options_fence_flow( /*went_up=*/true, sequence, "
                                "entry.holding->totals, stamp_of(entry.holding->at_unix));"),
              npos);
    EXPECT_NE(position_of(step, "if (back) { lowering.coin_ids = back->coin_ids; } else { "
                                "back = co_await coin_fence_->list_holding(); }"),
              npos);
    EXPECT_NE(position_of(step, "accounting::options_fence_flow( /*went_up=*/false, sequence, "
                                "back->totals, stamp_of(back->at_unix));"),
              npos);
    // Top-ups first: no fence-down event in a pass where one that belongs
    // before it is waiting.
    const std::size_t first = position_of(
        step, "if (co_await book_options_topups(rows, &lowering, block_height, tally)) { "
              "owe_event(down); stalled = true; continue; }");
    const std::size_t down = position_of(
        step, "outcome = co_await book_options_flow(down, /*fence_event=*/true, block_height, tally);");
    ASSERT_NE(first, npos);
    ASSERT_NE(down, npos);
    EXPECT_LT(first, down);

    // An entry leaves the log only once the books hold it and have been
    // read again; one that could not be booked stops the pass and is met
    // again, with what it moves owed to the invariant.
    const std::size_t not_booked = position_of(
        step, "if (outcome != OptionsBooking::Booked && outcome != OptionsBooking::Known) { "
              "stalled = true; continue; }");
    const std::size_t reread = position_of(
        step, "options_books_loaded_ = load_options_books(); if (!options_books_loaded_) { "
              "stalled = true; continue; } coin_fence_->acknowledge(entry.serial);");
    ASSERT_NE(not_booked, npos);
    ASSERT_NE(reread, npos);
    EXPECT_LT(not_booked, reread);
    EXPECT_EQ(count_of(step, "owe_event(up);"), 1u);
    EXPECT_EQ(count_of(step, "owe_event(down);"), 2u);
    // Nothing to book: the books already stand there, or it went up and
    // down again before any list was had.
    EXPECT_NE(position_of(step, "case accounting::OptionsLogStep::Skip: "
                                "coin_fence_->acknowledge(entry.serial); continue;"),
              npos);
    EXPECT_NE(position_of(step, "case accounting::OptionsLogStep::DropPair: "
                                "coin_fence_->acknowledge(log[i + 1].serial); ++i; continue;"),
              npos);
    // Then the top-ups, with the fence as the books now hold it -- not in a
    // pass that stopped at an event.
    const std::size_t last = position_of(
        step, "if (!stalled) { (void)co_await book_options_topups(rows, nullptr, block_height, tally); }");
    ASSERT_NE(last, npos);
    EXPECT_LT(reread, last);
}

TEST(OptionsTransferWiring, ATopUpIsConfirmedByItsOwnTransactionBeforeItIsBooked)
{
    const std::string raw = read_engine_cpp();
    const std::string pass = function_body(raw, kTopUps);
    ASSERT_FALSE(pass.empty());
    const std::size_t judged = position_of(
        pass, "accounting::judge_topup(books, flow.floor_iso, options_fence_up_stamp_);");
    const std::size_t asked = position_of(
        pass, "co_await options_topup_proof(flow, recent || placing, block_height)");
    const std::size_t unconfirmed = position_of(pass, "if (!confirmed) { owe(); continue; }");
    // Its own coin in the fence's list is arrival too: the engine's balances
    // fall with that list, not with the wallet's record of the send.
    const std::size_t listed = position_of(
        pass, "if (!confirmed && coin_id && coin_fence_->is_fenced_coin(*coin_id)) { confirmed = true; }");
    ASSERT_NE(listed, npos);
    const std::size_t placed = position_of(
        pass, "if (!accounting::topup_arrived_before_lowering(coin_returned, row.confirmed_at, "
              "lowering->at_unix)) { continue;");
    const std::size_t booked = position_of(
        pass, "co_await book_options_flow(flow, /*fence_event=*/false, block_height, tally);");
    ASSERT_NE(judged, npos);
    ASSERT_NE(asked, npos);
    ASSERT_NE(unconfirmed, npos);
    ASSERT_NE(placed, npos);
    ASSERT_NE(booked, npos);
    EXPECT_LT(judged, asked);
    EXPECT_LT(asked, listed);
    EXPECT_LT(listed, unconfirmed);
    EXPECT_LT(unconfirmed, placed);
    EXPECT_LT(placed, booked);
    EXPECT_EQ(count_of(pass, "co_await book_options_flow("), 1u);
    EXPECT_NE(position_of(pass, "if (verdict == accounting::OptionsTopUpVerdict::Deferred) {"), npos);
    // One the ledger holds from an earlier process is not "inside": it booked.
    EXPECT_NE(position_of(pass, "if (db_->ledger_has_event(flow.event_id)) { "
                                "options_booked_event_ids_.insert(flow.event_id); } else { "
                                "options_terminal_skip_ids_.insert(flow.event_id);"),
              npos);
    // The wallet is asked when the options book has not recorded the
    // confirmation, or when the top-up's coin is needed to place it.
    EXPECT_NE(position_of(pass, "const bool placing = lowering != nullptr && lowering->coin_ids.has_value();"),
              npos);
    EXPECT_NE(position_of(pass, "if (!confirmed || placing) {"), npos);
    // Its OWN coin, by id: no amount is compared anywhere in the pass.
    EXPECT_NE(position_of(pass, "coin_returned = std::find(ids.begin(), ids.end(), *coin_id) != ids.end();"),
              npos);
    EXPECT_EQ(position_of(pass, "amount"), npos);
    // What is owed to the invariant is counted once a heartbeat.
    EXPECT_NE(position_of(pass, "if (!recent || !options_owed_ids_.insert(flow.event_id).second) return;"),
              npos);
    EXPECT_NE(position_of(pass, "if (outcome == OptionsBooking::Wait || outcome == OptionsBooking::Failed) "
                                "{ owe(); waiting = true; }"),
              npos);

    const std::string proof = function_body(raw, kProof);
    ASSERT_FALSE(proof.empty());
    // One read-only wallet call, behind the wallet circuit, at most once a
    // heartbeat for a top-up and at topup_proof_due's pace.
    const std::size_t due = position_of(
        proof, "if (!accounting::topup_proof_due(ask_now, block_height, last_asked)) co_return std::nullopt;");
    const std::size_t gate = position_of(
        proof, "if (!wallet_ || !wallet_->is_open() || !wallet_step_may_run(\"options ingest\")) {");
    const std::size_t call = position_of(proof, "co_await wallet_->get_transaction(transaction_id);");
    ASSERT_NE(due, npos);
    ASSERT_NE(gate, npos);
    ASSERT_NE(call, npos);
    EXPECT_LT(due, gate);
    EXPECT_LT(gate, call);
    EXPECT_EQ(count_of(proof, "wallet_->"), 2u) << "is_open and get_transaction";
    EXPECT_NE(position_of(proof, "accounting::read_topup_transaction(record, topup)"), npos);
    // The top-up's coin is the output at the options address, named as the
    // fence's refresher names the coins it lists.
    EXPECT_NE(position_of(proof, "if (fenced.count(puzzle_hash) == 0) continue;"), npos);
    EXPECT_NE(position_of(proof, "execution::CoinManager::compute_coin_name( "
                                 "rpc::fence_normalise_hex(out.parent_coin_info), puzzle_hash, "
                                 "out.amount)"),
              npos);
    // No answer is not "unconfirmed": an error leaves it unanswered.
    EXPECT_NE(position_of(proof, "std::optional<OptionsProof> answer; try {"), npos);
}

TEST(OptionsTransferWiring, AFlowIsJournalledBeforeAnythingElseMoves)
{
    const std::string book = function_body(read_engine_cpp(), kBookFlow);
    ASSERT_FALSE(book.empty());
    const std::size_t opening = position_of(book, "!accounting::iso_strictly_after(flow.floor_iso, opening_time)");
    const std::size_t snapshot = position_of(book, "co_await options_wallet_snapshot(asset, block_height)");
    const std::size_t marker = position_of(book, "db_->mark_capital_reconcile_pending(asset_id, \"options\")");
    const std::size_t journal = position_of(book, "db_->append_ledger_entries(entries);");
    const std::size_t known = position_of(book, "if (*inserted != entries.size()) {");
    const std::size_t deposit = position_of(book, "pnl_->add_net_deposit_usd(p.val.flow_usd);");
    const std::size_t inventory = position_of(book, "inventory_->adjust_quantity(");
    ASSERT_NE(opening, npos);
    ASSERT_NE(snapshot, npos);
    ASSERT_NE(marker, npos);
    ASSERT_NE(journal, npos);
    ASSERT_NE(known, npos);
    ASSERT_NE(deposit, npos);
    ASSERT_NE(inventory, npos);
    // One the ledger holds from an earlier process is known before any price
    // or wallet read it could be made to wait on.
    const std::size_t held = position_of(book, "if (db_->ledger_has_event(flow.event_id)) { "
                                               "options_booked_event_ids_.insert(flow.event_id); "
                                               "co_return OptionsBooking::Known; }");
    ASSERT_NE(held, npos);
    EXPECT_LT(held, opening);
    EXPECT_LT(held, position_of(book, "asset_usd_pseudo_price(asset)"));
    EXPECT_LT(held, snapshot);
    EXPECT_LT(opening, journal) << "a flow inside the opening balance is not booked again";
    EXPECT_LT(snapshot, journal) << "the invariant ties the books to a snapshot read after the flow";
    // The durable marker is written before the journal, and a flow whose
    // marker could not be written is not journalled.
    EXPECT_LT(marker, journal);
    EXPECT_NE(position_of(book, "if (!db_->mark_capital_reconcile_pending(asset_id, \"options\")) {"), npos);
    EXPECT_LT(journal, known);
    EXPECT_LT(known, deposit) << "a flow the ledger already held has no second effect";
    EXPECT_LT(known, inventory);
    EXPECT_EQ(count_of(book, "pnl_->add_net_deposit_usd("), 1u);
    // The bridge asset keeps its single inventory writer, and needs no marker.
    EXPECT_EQ(count_of(book, "asset_id == acc.bridge_asset_id && bridge_accounting_operational()"), 2u);
    // A relative move is settled once the tracker is written; wallet truth
    // goes through Step 11's reconcile, marked as capital, and its marker
    // stays until then.
    EXPECT_NE(position_of(book, "tally.inv_changed = true; tally.settled_assets.insert(asset_id);"), npos);
    EXPECT_NE(position_of(book, "inventory_reconcile_capital_[asset_id] = true; "
                                "options_capital_assets_.insert(asset_id);"),
              npos);
    // A fence event is entered at its own moment: the stamp top-ups meet.
    EXPECT_NE(position_of(book, "const std::string entry_time = fence_event ? flow.at_iso :"), npos);
    // The bridge asset's tracked quantity is the bridge scan's to move, which
    // may be a heartbeat later: the peak is discarded once more then.
    EXPECT_NE(position_of(book, "if (in_process) tally.reset_again = true; } else {"), npos);
}

TEST(OptionsTransferWiring, ABookingIsTiedToBalancesReadWithTheFlowsCoinListed)
{
    const std::string raw = read_engine_cpp();
    // The snapshot a booking is tied to is read after the fence's list is
    // refreshed: the balance is the wallet's less that list.
    const std::string snapshot = function_body(
        raw, "asio::awaitable<bool> Engine::options_wallet_snapshot(");
    ASSERT_FALSE(snapshot.empty());
    const std::size_t refreshed = position_of(
        snapshot, "if (coin_fence_ && coin_fence_->enabled()) co_await coin_fence_->refresh_now();");
    const std::size_t read = position_of(snapshot, "co_await fetch_bridge_balance(asset, block_height, &validated);");
    ASSERT_NE(refreshed, npos);
    ASSERT_NE(read, npos);
    EXPECT_LT(refreshed, read);
    EXPECT_EQ(count_of(snapshot, "refresh_now()"), 1u) << "once a heartbeat, with the cache it clears";

    // A bridge-asset flow may reach equity a heartbeat after its booking
    // discarded the peak: it is discarded once more from the next heartbeat.
    const std::string step = function_body(raw, kStep);
    const std::size_t again = position_of(
        step, "if (options_peak_reset_again_ != 0 && block_height >= options_peak_reset_again_) { "
              "options_peak_reset_again_ = 0;");
    const std::size_t first_return = position_of(step, "co_return;");
    const std::size_t owed = position_of(
        step, "if (tally.reset_again) { options_peak_reset_again_ = block_height + 1; }");
    ASSERT_NE(again, npos);
    ASSERT_NE(owed, npos);
    EXPECT_LT(again, first_return) << "also in a pass that then stands down";
    EXPECT_LT(again, owed);
}

TEST(OptionsTransferWiring, TheMarkerOutlivesAStopAndGoesOnlyOnceTheTrackerIsWritten)
{
    const std::string raw = read_engine_cpp();
    const std::string engine = squeeze(raw);
    // A start restores the markers with the inventory they qualify, before
    // any heartbeat can reconcile it.
    const std::size_t restored = engine.find(squeeze("db_->load_inventory_state();"));
    const std::size_t markers = engine.find(squeeze(
        "if (const auto pending = db_->capital_reconcile_pending()) { "
        "for (const auto& asset_id : *pending) { options_capital_assets_.insert(asset_id); "
        "inventory_reconcile_capital_[asset_id] = true; }"));
    ASSERT_NE(restored, npos);
    ASSERT_NE(markers, npos);
    EXPECT_LT(restored, markers);
    EXPECT_EQ(count_of(engine, "db_->capital_reconcile_pending()"), 1u);

    // Step 11's wallet reconcile re-anchors for such an asset, as it does
    // for a secondary bridge asset...
    const std::size_t reconcile = engine.find(squeeze("|| options_capital_assets_.count(aid) > 0)) {"));
    ASSERT_NE(reconcile, npos);
    EXPECT_LT(reconcile, engine.find(squeeze("pnl_window_rebase_assets_[aid] = block_height;"), reconcile));
    EXPECT_EQ(count_of(engine, "|| options_capital_assets_.count(aid) > 0)) {"), 1u);
    // ...and drops the marker only after the tracker is written.
    const std::size_t settled = engine.find(squeeze(
        "if (options_capital_assets_.erase(aid) > 0) { options_settled.push_back(aid); }"));
    const std::size_t written = engine.find(squeeze(
        "if (mutated || !options_settled.empty()) { persist_inventory_state(); }"));
    const std::size_t dropped = engine.find(squeeze(
        "for (const auto& settled : options_settled) { "
        "if (db_) db_->clear_capital_reconcile_pending(settled); }"));
    ASSERT_NE(settled, npos);
    ASSERT_NE(written, npos);
    ASSERT_NE(dropped, npos);
    EXPECT_LT(settled, written);
    EXPECT_LT(written, dropped);

    // A flow that moved the tracker itself: the same order, in the step.
    const std::string step = function_body(raw, kStep);
    const std::size_t persisted = position_of(step, "if (tally.inv_changed) { persist_inventory_state(); }");
    const std::size_t cleared = position_of(
        step, "for (const auto& asset_id : tally.settled_assets) { "
              "if (options_capital_assets_.count(asset_id) == 0) { "
              "db_->clear_capital_reconcile_pending(asset_id); } }");
    ASSERT_NE(persisted, npos);
    ASSERT_NE(cleared, npos);
    EXPECT_LT(persisted, cleared);
    EXPECT_EQ(count_of(engine, "clear_capital_reconcile_pending("), 2u);
    EXPECT_EQ(count_of(engine, "mark_capital_reconcile_pending("), 1u);
}

TEST(OptionsTransferWiring, TheInvariantAndTheRestartReaderKnowAboutOptionsFlows)
{
    const std::string raw = read_engine_cpp();
    const std::string invariant =
        function_body(raw, "void Engine::step_check_ledger_invariant(BlockHeight block_height)");
    ASSERT_FALSE(invariant.empty());
    EXPECT_NE(position_of(invariant,
                          "const Mojo inflight_slack = accounting::add_slack( std::abs(cached.pending_change), "
                          "options_owed != options_pending_slack_.end() ? options_owed->second : 0);"),
              npos);

    // Net Deposits is rebuilt from these rows on restart too.
    std::string pnl_path = XOP_TESTS_ENGINE_CPP;
    const std::string leaf = "engine.cpp";
    ASSERT_GE(pnl_path.size(), leaf.size());
    pnl_path.replace(pnl_path.size() - leaf.size(), leaf.size(), "monitoring/pnl.cpp");
    std::ifstream in(pnl_path, std::ios::binary);
    ASSERT_TRUE(in.good()) << pnl_path;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    EXPECT_EQ(count_of(squeeze(buffer.str()),
                       "WHERE event_type IN ('bridge_deposit', 'bridge_withdrawal', "
                       "'options_deposit', 'options_withdrawal');"),
              1u);
}
