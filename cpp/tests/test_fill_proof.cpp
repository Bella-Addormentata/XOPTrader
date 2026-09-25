// test_fill_proof.cpp -- [FILL-PROOF 2026-09-23]
//
// Is a trade the wallet reports CONFIRMED really a fill
// (execution/fill_proof.hpp)?  The tests marked REAL replay records read from
// the live wallet and full node on 2026-09-23, trimmed to the fields the proof
// reads:
//
//   * 0xdb63709cb9 -- an XCH/BYC bid booked as a fill on 2026-09-22.  One of
//     its three maker coins was spent at 9,325,694 by a fee spend, whose only
//     child outlived the block; the other two were never spent.  Dead.
//   * 0x202ff7d2d8 -- an XCH/DBX ask really taken on 2026-09-16 (trade_log
//     1899).  Both maker coins spent at 9,297,025.  One of them created the
//     1-XCH settlement coin, spent in the same block, and the wallet received
//     the 85,094-mojo DBX payment there.  Settled.
//   * 0x18672b6b0f -- an XCH/DBX bid really taken (trade_log 1883).  Both
//     maker coins spent at 9,249,503.  The DBX coin created the 84,696-mojo
//     settlement coin, spent in the same block, and the wallet received the
//     1-XCH payment there.  Settled.
//
// The synthetic tests vary these records, so every coin name is a real one,
// computed by CoinManager::compute_coin_name as the engine does.

#include <gtest/gtest.h>

#include <xop/execution/coin_manager.hpp>
#include <xop/execution/fill_proof.hpp>
#include <xop/types.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ex = xop::execution;
using nlohmann::json;
using ex::FillProof;
using ex::FillProofResult;

// The engine's adapter shape: any throw is "cannot hash".
std::string chain_name(const ex::CoinRef& ref)
{
    try {
        return ex::CoinManager::compute_coin_name(ref.parent_hex, ref.puzzle_hash_hex,
                                                  static_cast<xop::Mojo>(ref.amount));
    } catch (const std::exception&) {
        return {};
    }
}

// wallet get_offer(file_contents=false), trimmed.
constexpr const char* kPhantomWallet = R"({"trade_id":"0xdb63709cb9b2c3794cbf1a57bb15f2d5c1830dca620b16ac36926d0d60c6c556","status":"CONFIRMED","confirmed_at_index":9325694,"coins_of_interest":[{"amount":21284030165,"parent_coin_info":"0x82ee60315b50419a1cbaf52b44bbc98c787129307fc2578e87995cfba73571b7","puzzle_hash":"0xf4d66eb50f4a2d721cc58a4940f3d678989da55af0387d5f7087403521913dcb"},{"amount":1657,"parent_coin_info":"0xf88d5c260891d06b0daf16f22e974c67b1341f2d1bc8a404e374fdcc475abfbc","puzzle_hash":"0x47d29d6d58ccdb7f597c82b3e3c6f9524b2b8aee03414e5971e67ff0241f5955"},{"amount":1600,"parent_coin_info":"0xdf311e9bd2b13703ae87b9e814a8df3d939d03f508f6abc41b96f265479eb469","puzzle_hash":"0xa240a96af2d708713ff6ef3281dd4d47a737961e8899fa8f49a3f9ed62296a78"}],"summary":{"fees":15000000,"offered":{"ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac":"1864"},"requested":{"xch":"1103203898833"}}})";

// full_node get_coin_records_by_names, include_spent_coins, trimmed.
constexpr const char* kPhantomNode = R"([{"coin":{"amount":21284030165,"parent_coin_info":"0x82ee60315b50419a1cbaf52b44bbc98c787129307fc2578e87995cfba73571b7","puzzle_hash":"0xf4d66eb50f4a2d721cc58a4940f3d678989da55af0387d5f7087403521913dcb"},"confirmed_block_index":9325687,"spent_block_index":9325694,"spent":true},{"coin":{"amount":1600,"parent_coin_info":"0xdf311e9bd2b13703ae87b9e814a8df3d939d03f508f6abc41b96f265479eb469","puzzle_hash":"0xa240a96af2d708713ff6ef3281dd4d47a737961e8899fa8f49a3f9ed62296a78"},"confirmed_block_index":9245763,"spent_block_index":0,"spent":false},{"coin":{"amount":1657,"parent_coin_info":"0xf88d5c260891d06b0daf16f22e974c67b1341f2d1bc8a404e374fdcc475abfbc","puzzle_hash":"0x47d29d6d58ccdb7f597c82b3e3c6f9524b2b8aee03414e5971e67ff0241f5955"},"confirmed_block_index":9325248,"spent_block_index":0,"spent":false}])";

// full_node get_coin_records_by_parent_ids over the maker coins, trimmed: the
// fee spend's change, spent at 9,331,128.
constexpr const char* kPhantomChildren = R"([{"coin":{"amount":21269030165,"parent_coin_info":"0x4ea3d75d6698817cfa79a8e5b609ced34fc10bb4d889db437525b85d42bc8413","puzzle_hash":"0x7eaad1dfc5fb8d060234e0fb3a9b2272c89080312c269623106725e7c0845168"},"confirmed_block_index":9325694,"spent_block_index":9331128,"spent":true}])";

constexpr const char* kAskWallet = R"({"trade_id":"0x202ff7d2d8554a76bf97df827ce65965d5a6a5619496780f698c8222b9a3fcec","status":"CONFIRMED","confirmed_at_index":9297025,"coins_of_interest":[{"amount":999935419490,"parent_coin_info":"0x2c4d49c77fd91113a3717d230cb7c1d44bec20ccfcf146064d9549a86629061e","puzzle_hash":"0x85b236977ac595209c59276f8074b5c4ce9320ee9d7e7987cd201a1b2dcb6360"},{"amount":999955647482,"parent_coin_info":"0xffb0885ba8babec7a41b05aee34195aaf5843a694804f39d5e35fdb784ce2db3","puzzle_hash":"0xe2d5898d50eb165f06653c1e7fcc0e1d626d9181a04ee8fd94ba98ad28bbfa1f"}],"summary":{"fees":10000000,"offered":{"xch":"1000000000000"},"requested":{"db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20":"85094"}}})";

constexpr const char* kAskNode = R"([{"coin":{"amount":999955647482,"parent_coin_info":"0xffb0885ba8babec7a41b05aee34195aaf5843a694804f39d5e35fdb784ce2db3","puzzle_hash":"0xe2d5898d50eb165f06653c1e7fcc0e1d626d9181a04ee8fd94ba98ad28bbfa1f"},"confirmed_block_index":9291973,"spent_block_index":9297025,"spent":true},{"coin":{"amount":999935419490,"parent_coin_info":"0x2c4d49c77fd91113a3717d230cb7c1d44bec20ccfcf146064d9549a86629061e","puzzle_hash":"0x85b236977ac595209c59276f8074b5c4ce9320ee9d7e7987cd201a1b2dcb6360"},"confirmed_block_index":9292565,"spent_block_index":9297025,"spent":true}])";

// [0] the change, [1] the settlement coin (1 XCH at the settlement puzzle,
// created and spent at 9,297,025).
constexpr const char* kAskChildren = R"([{"coin":{"amount":999881066972,"parent_coin_info":"0x10e8fca4a290ed992202011788a5a0385057ac5a089082d143004a1b3b903289","puzzle_hash":"0x5ade11161c958f48d66fb618569c13d5e993d164fbdaa152ae8411808fb8c45f"},"confirmed_block_index":9297025,"spent_block_index":9302353,"spent":true},{"coin":{"amount":1000000000000,"parent_coin_info":"0x10e8fca4a290ed992202011788a5a0385057ac5a089082d143004a1b3b903289","puzzle_hash":"0xcfbfdeed5c4ca2de3d0bf520b9cb4bb7743a359bd2e6a188d19ce7dffc21d3e7"},"confirmed_block_index":9297025,"spent_block_index":9297025,"spent":true}])";

// wallet get_coin_records(confirmed_range [9297025, 9297025], amounts
// [85094]), trimmed: the DBX payment, from the taker's settlement coin.
constexpr const char* kAskPayments = R"([{"parent_coin_info":"0xafe2244082dbd143a68741eac6207204245a98d55e10fc8a68f791ad0a458384","puzzle_hash":"0xece6bdca8e31ca56d1c402256c052fa2209969681237461d8c297ed64c641f3e","amount":85094,"confirmed_height":9297025,"spent_height":0}])";

constexpr const char* kBidWallet = R"({"trade_id":"0x18672b6b0f03801f9c9a83381b10259bdf44ab97ade00ddfb9ee4f454ccaade5","status":"CONFIRMED","confirmed_at_index":9249503,"coins_of_interest":[{"amount":22068721835,"parent_coin_info":"0xc1988b3597a829a09a206b9cbc987481c5baa51d1a7d666d40713ce6510cde22","puzzle_hash":"0x56abd87f93ae2749858de9eaeca4af5872776d420f29aa6775b7da6e53922145"},{"amount":86034,"parent_coin_info":"0x2ca5da1e910894207e6415101b52c862486238d9837121e6137e0e14a66b3448","puzzle_hash":"0xc60002fd91353a41c931103c4343a017fa31186f3ba5b409dd14cd30f646496c"}],"summary":{"fees":877612,"offered":{"db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20":"84696"},"requested":{"xch":"1000000000000"}}})";

constexpr const char* kBidNode = R"([{"coin":{"amount":86034,"parent_coin_info":"0x2ca5da1e910894207e6415101b52c862486238d9837121e6137e0e14a66b3448","puzzle_hash":"0xc60002fd91353a41c931103c4343a017fa31186f3ba5b409dd14cd30f646496c"},"confirmed_block_index":9249420,"spent_block_index":9249503,"spent":true},{"coin":{"amount":22068721835,"parent_coin_info":"0xc1988b3597a829a09a206b9cbc987481c5baa51d1a7d666d40713ce6510cde22","puzzle_hash":"0x56abd87f93ae2749858de9eaeca4af5872776d420f29aa6775b7da6e53922145"},"confirmed_block_index":9247411,"spent_block_index":9249503,"spent":true}])";

// DBX change, XCH change, and the DBX settlement coin (84,696 at the DBX
// settlement puzzle, created and spent at 9,249,503).
constexpr const char* kBidChildren = R"([{"coin":{"amount":1338,"parent_coin_info":"0xca9e21410f8e7e3ce3b6e168a2f36e6d5a5bcd276914e262f59e75620f43571f","puzzle_hash":"0x0a495aa099992803f345bacbbbdbb7a8859ceaadfc2b9ce88a1b345d536dd2b4"},"confirmed_block_index":9249503,"spent_block_index":9275986,"spent":true},{"coin":{"amount":22067844223,"parent_coin_info":"0x26f5ee7bb75a3d5ebf5b8a12eb5950873d4d723633caedbcc895e5449bb11222","puzzle_hash":"0x87fabdee1f2921063bc8d3b4d0907a343d4dddf77dcc96bf352be5edaafda781"},"confirmed_block_index":9249503,"spent_block_index":9253024,"spent":true},{"coin":{"amount":84696,"parent_coin_info":"0xca9e21410f8e7e3ce3b6e168a2f36e6d5a5bcd276914e262f59e75620f43571f","puzzle_hash":"0x2a8269fa3ec2a6968ee95219edc900ada54232beb24ad0f7581f3b3eab613e81"},"confirmed_block_index":9249503,"spent_block_index":9249503,"spent":true}])";

// The 1-XCH payment -- to the same address as the XCH change, but from the
// taker's settlement coin, not from a maker coin.
constexpr const char* kBidPayments = R"([{"parent_coin_info":"0x2dce69d6b2deb73a3c1f004959a1670bcf3907a659f6a932d821e25cc28c48d5","puzzle_hash":"0x87fabdee1f2921063bc8d3b4d0907a343d4dddf77dcc96bf352be5edaafda781","amount":1000000000000,"confirmed_height":9249503,"spent_height":9273574}])";

std::vector<std::string> names_of(const json& wallet_record)
{
    return ex::coin_names_for(ex::parse_coins_of_interest(wallet_record), chain_name);
}

std::vector<json> records_of(const char* text)
{
    const json parsed = json::parse(text);
    return std::vector<json>(parsed.begin(), parsed.end());
}

FillProofResult prove(const json& wallet_record, const std::vector<json>& records)
{
    return ex::prove_fill(names_of(wallet_record), records, chain_name);
}

// The engine's adapter shape: an asset whose settlement puzzle cannot be named,
// or any throw, is "".
std::string settlement_puzzle(const std::string& asset)
{
    try {
        return ex::CoinManager::settlement_puzzle_hash(asset).value_or(std::string{});
    } catch (const std::exception&) {
        return {};
    }
}

// Both stages, the node's way and the wallet's way.
FillProofResult by_node(const json& wallet_record, const std::vector<json>& records,
                        const std::vector<json>& children)
{
    return ex::prove_take_from_children(prove(wallet_record, records), names_of(wallet_record),
                                        children,
                                        ex::offered_settlements(wallet_record, settlement_puzzle));
}

FillProofResult by_wallet(const json& wallet_record, const std::vector<json>& records,
                          const std::vector<json>& payments)
{
    return ex::prove_take_from_payments(prove(wallet_record, records), names_of(wallet_record),
                                        payments, ex::summary_amounts(wallet_record, "requested"));
}

json ask_wallet() { return json::parse(kAskWallet); }
std::vector<json> ask_records() { return records_of(kAskNode); }
std::vector<json> ask_children() { return records_of(kAskChildren); }
std::vector<json> ask_payments() { return records_of(kAskPayments); }

// A ONE-coin offer built from the phantom's consumed XCH coin -- real on-chain
// records, grouped as the review's case: every maker coin spent in one block,
// by a spend that is not a take.
json one_coin_offer()
{
    json record = json::object();
    record["coins_of_interest"] =
        json::array({json::parse(kPhantomWallet)["coins_of_interest"][0]});
    record["summary"] = {{"offered", {{"xch", "1000000000000"}}},
                         {"requested", {{"dbx", "100000"}}}};
    return record;
}

// ---------------------------------------------------------------------------
// REAL: the phantom, and two real takes
// ---------------------------------------------------------------------------

TEST(FillProof, TheCoinNamesAreTheChainsOwn)
{
    // The names the proof asks for are the ids Dexie and the node use.
    const std::vector<std::string> names = names_of(json::parse(kPhantomWallet));
    ASSERT_EQ(names.size(), 3U);
    EXPECT_EQ(names[0], "4ea3d75d6698817cfa79a8e5b609ced34fc10bb4d889db437525b85d42bc8413");
    EXPECT_EQ(names[1], "fe636075405d34d1f22d803c282b6c207fb1e4fa46e4a8b6eb872b4556566f67");
}

TEST(FillProof, ThePhantomFillOf20260922IsDead)
{
    const json wallet = json::parse(kPhantomWallet);
    const FillProofResult proof = prove(wallet, records_of(kPhantomNode));
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9325694U);   // the fee spend: the wallet's "confirmed" height
    EXPECT_EQ(proof.coins, 3U);
    EXPECT_EQ(proof.unspent, 2U);
    EXPECT_FALSE(proof.spent_together);
    // Nearly 4,700 blocks deep by 2026-09-23: closable at the default depth.
    EXPECT_TRUE(ex::dead_offer_closable(proof, 9330000U, 6U));
    // The second stage never revisits it.
    EXPECT_EQ(by_node(wallet, records_of(kPhantomNode), records_of(kPhantomChildren)).verdict,
              FillProof::Dead);
    EXPECT_EQ(by_wallet(wallet, records_of(kPhantomNode), {}).verdict, FillProof::Dead);
}

TEST(FillProof, ATakeSpendsEveryCoinTogetherButThatAloneIsNoFill)
{
    const FillProofResult proof = prove(ask_wallet(), ask_records());
    EXPECT_EQ(proof.verdict, FillProof::SpentTogether);
    EXPECT_EQ(proof.height, 9297025U);   // the wallet's confirmed_at_index too
    EXPECT_EQ(proof.coins, 2U);
    EXPECT_EQ(proof.unspent, 0U);
    EXPECT_TRUE(proof.spent_together);
    EXPECT_FALSE(ex::dead_offer_closable(proof, 9330000U, 6U));
}

TEST(FillProof, TheRealAskShowsItsSettlementCoin)
{
    const FillProofResult proof = by_node(ask_wallet(), ask_records(), ask_children());
    EXPECT_EQ(proof.verdict, FillProof::Settled);
    EXPECT_EQ(proof.height, 9297025U);
    EXPECT_TRUE(proof.spent_together);
}

TEST(FillProof, TheRealBidShowsItsSettlementCoin)
{
    const json wallet = json::parse(kBidWallet);
    EXPECT_EQ(prove(wallet, records_of(kBidNode)).verdict, FillProof::SpentTogether);
    const FillProofResult proof = by_node(wallet, records_of(kBidNode), records_of(kBidChildren));
    EXPECT_EQ(proof.verdict, FillProof::Settled);
    EXPECT_EQ(proof.height, 9249503U);
}

TEST(FillProof, TheWalletSeesTheRealTakesPayments)
{
    EXPECT_EQ(by_wallet(ask_wallet(), ask_records(), ask_payments()).verdict, FillProof::Settled);
    const json bid = json::parse(kBidWallet);
    EXPECT_EQ(by_wallet(bid, records_of(kBidNode), records_of(kBidPayments)).verdict,
              FillProof::Settled);
}

TEST(FillProof, AOneCoinOfferConsumedWithoutATakeIsDead)
{
    // [review #171] The coin was spent in one block, as a take would spend
    // it -- by a fee spend whose only child outlived the block.
    const json offer = one_coin_offer();
    const std::vector<json> coin{records_of(kPhantomNode)[0]};
    const FillProofResult coins = prove(offer, coin);
    ASSERT_EQ(coins.verdict, FillProof::SpentTogether);

    const FillProofResult proof = by_node(offer, coin, records_of(kPhantomChildren));
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9325694U);
    EXPECT_TRUE(proof.spent_together);
    EXPECT_TRUE(ex::dead_offer_closable(proof, 9330000U, 6U));

    // The wallet saw no payment either -- which proves nothing: never Dead.
    EXPECT_EQ(by_wallet(offer, coin, {}).verdict, FillProof::Unknown);
}

// ---------------------------------------------------------------------------
// Stage 1: the maker coins' own records
// ---------------------------------------------------------------------------

TEST(FillProof, CoinsSpentAtDifferentHeightsAreDead)
{
    // What the phantom's surviving coins would show once a later offer reused
    // them: every coin spent, but not together.  "All spent" is not a take.
    std::vector<json> records = ask_records();
    records[1]["spent_block_index"] = 9297030;
    const FillProofResult proof = prove(ask_wallet(), records);
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9297025U);   // the earliest spend
    EXPECT_EQ(proof.unspent, 0U);
    EXPECT_FALSE(proof.spent_together);
}

TEST(FillProof, EveryCoinUnspentIsLive)
{
    std::vector<json> records = ask_records();
    for (auto& record : records) {
        record["spent_block_index"] = 0;
        record["spent"] = false;
    }
    const FillProofResult proof = prove(ask_wallet(), records);
    EXPECT_EQ(proof.verdict, FillProof::Live);
    EXPECT_EQ(proof.unspent, 2U);
    EXPECT_FALSE(ex::dead_offer_closable(proof, 9330000U, 6U));
}

TEST(FillProof, OneCoinSpentAndOneNotIsDead)
{
    std::vector<json> records = ask_records();
    records[0]["spent_block_index"] = 0;
    records[0]["spent"] = false;
    const FillProofResult proof = prove(ask_wallet(), records);
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9297025U);
    EXPECT_EQ(proof.unspent, 1U);
}

TEST(FillProof, AMakerCoinTheAnswerOmitsIsNoProof)
{
    // The node omits coins it does not find; the one it returned is spent.
    std::vector<json> records = ask_records();
    records.pop_back();
    EXPECT_EQ(prove(ask_wallet(), records).verdict, FillProof::Unknown);
    EXPECT_EQ(prove(ask_wallet(), {}).verdict, FillProof::Unknown);
}

TEST(FillProof, ARecordForACoinNotAskedAboutIsNoProof)
{
    std::vector<json> records = ask_records();
    records.push_back(records_of(kPhantomNode)[0]);
    EXPECT_EQ(prove(ask_wallet(), records).verdict, FillProof::Unknown);
}

TEST(FillProof, AContradictoryRecordIsNoProof)
{
    std::vector<json> spent_without_height = ask_records();
    spent_without_height[0]["spent_block_index"] = 0;   // "spent": true stays
    EXPECT_EQ(prove(ask_wallet(), spent_without_height).verdict, FillProof::Unknown);

    std::vector<json> height_but_unspent = ask_records();
    height_but_unspent[0]["spent"] = false;              // the height stays
    EXPECT_EQ(prove(ask_wallet(), height_but_unspent).verdict, FillProof::Unknown);
}

TEST(FillProof, AnUnreadableRecordIsNoProof)
{
    std::vector<json> string_height = ask_records();
    string_height[0]["spent_block_index"] = "9297025";
    EXPECT_EQ(prove(ask_wallet(), string_height).verdict, FillProof::Unknown);

    std::vector<json> no_coin = ask_records();
    no_coin[0].erase("coin");
    EXPECT_EQ(prove(ask_wallet(), no_coin).verdict, FillProof::Unknown);

    // Every maker coin answered, plus a record with no coin in it: the answer
    // is not the one that was asked for, whatever else it holds.
    std::vector<json> junk_added = ask_records();
    junk_added.push_back(json{{"spent_block_index", 0}, {"spent", false}});
    EXPECT_EQ(prove(ask_wallet(), junk_added).verdict, FillProof::Unknown);
}

TEST(FillProof, DuplicateRecordsMustAgree)
{
    std::vector<json> agreeing = ask_records();
    agreeing.push_back(agreeing[0]);
    EXPECT_EQ(prove(ask_wallet(), agreeing).verdict, FillProof::SpentTogether);

    std::vector<json> disagreeing = ask_records();
    json copy = disagreeing[0];
    copy["spent_block_index"] = 9297026;
    disagreeing.push_back(copy);
    EXPECT_EQ(prove(ask_wallet(), disagreeing).verdict, FillProof::Unknown);
}

TEST(FillProof, NoCoinsToAskAboutIsNoProof)
{
    EXPECT_EQ(ex::prove_fill(std::vector<std::string>{}, ask_records(), chain_name).verdict,
              FillProof::Unknown);
}

// ---------------------------------------------------------------------------
// Stage 2 from the node: the settlement coin
// ---------------------------------------------------------------------------

TEST(TakeFromChildren, ASettlementCoinForAnotherAmountIsNoMark)
{
    std::vector<json> children = ask_children();
    children[1]["coin"]["amount"] = 999999999999ULL;
    const FillProofResult proof = by_node(ask_wallet(), ask_records(), children);
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9297025U);
}

TEST(TakeFromChildren, ACoinThatOutlivedTheBlockIsNoMark)
{
    std::vector<json> children = ask_children();
    children[1]["spent_block_index"] = 9297026;
    EXPECT_EQ(by_node(ask_wallet(), ask_records(), children).verdict, FillProof::Dead);
}

TEST(TakeFromChildren, AChildAtAnotherPuzzleIsNoMark)
{
    // [review #171, round 5] The settlement coin's amount, created and spent
    // in the block -- but at the change's own address, not the settlement
    // puzzle.  An amount alone could be any child.
    std::vector<json> children = ask_children();
    children[1]["coin"]["puzzle_hash"] = children[0]["coin"]["puzzle_hash"];
    const FillProofResult proof = by_node(ask_wallet(), ask_records(), children);
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9297025U);

    // The bid's DBX amount at XCH's settlement puzzle: the right puzzle for
    // another asset is no mark either.
    std::vector<json> bid_children = records_of(kBidChildren);
    bid_children[2]["coin"]["puzzle_hash"] = ask_children()[1]["coin"]["puzzle_hash"];
    EXPECT_EQ(by_node(json::parse(kBidWallet), records_of(kBidNode), bid_children).verdict,
              FillProof::Dead);
}

TEST(TakeFromChildren, AChildOfSomeOtherCoinIsNoAnswer)
{
    std::vector<json> children = ask_children();
    children[0]["coin"]["parent_coin_info"] =
        "0x4ea3d75d6698817cfa79a8e5b609ced34fc10bb4d889db437525b85d42bc8413";
    EXPECT_EQ(by_node(ask_wallet(), ask_records(), children).verdict, FillProof::Unknown);
}

TEST(TakeFromChildren, AChildCreatedAtAnotherHeightIsNoAnswer)
{
    // The maker coins were spent at 9,297,025, so that is where their
    // children were created.  Anything else is not an answer about this spend.
    std::vector<json> children = ask_children();
    children[0]["confirmed_block_index"] = 9297024;
    EXPECT_EQ(by_node(ask_wallet(), ask_records(), children).verdict, FillProof::Unknown);
}

TEST(TakeFromChildren, AnUnreadableChildIsNoAnswer)
{
    std::vector<json> no_height = ask_children();
    no_height[0].erase("confirmed_block_index");
    EXPECT_EQ(by_node(ask_wallet(), ask_records(), no_height).verdict, FillProof::Unknown);

    std::vector<json> contradictory = ask_children();
    contradictory[1]["spent"] = false;   // spent_block_index says spent
    EXPECT_EQ(by_node(ask_wallet(), ask_records(), contradictory).verdict, FillProof::Unknown);

    std::vector<json> no_coin = ask_children();
    no_coin[0].erase("coin");
    EXPECT_EQ(by_node(ask_wallet(), ask_records(), no_coin).verdict, FillProof::Unknown);
}

TEST(TakeFromChildren, NoChildrenAtAllIsDead)
{
    // [review #171, round 5] The node lists every child of the maker coins,
    // and the wrapper refuses a reply without that list -- so an empty list
    // says the spend created none, and a take always creates its settlement
    // coin.  (A coin paid away whole as a fee, say.)
    const FillProofResult proof = by_node(ask_wallet(), ask_records(), {});
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9297025U);
    EXPECT_TRUE(proof.spent_together);
    EXPECT_TRUE(ex::dead_offer_closable(proof, 9330000U, 6U));
}

TEST(TakeFromChildren, NoSettlementIsNoAnswer)
{
    json no_summary = ask_wallet();
    no_summary.erase("summary");
    EXPECT_EQ(by_node(no_summary, ask_records(), ask_children()).verdict, FillProof::Unknown);
    EXPECT_EQ(by_node(no_summary, ask_records(), {}).verdict, FillProof::Unknown);

    // An offered asset whose settlement puzzle cannot be named: the offer
    // cannot be fully described, so nothing about it is proven -- even Dead.
    json unnamed = ask_wallet();
    unnamed["summary"]["offered"]["dbx"] = "100";
    EXPECT_EQ(by_node(unnamed, ask_records(), ask_children()).verdict, FillProof::Unknown);
    EXPECT_EQ(by_node(unnamed, ask_records(), {}).verdict, FillProof::Unknown);
}

TEST(TakeFromChildren, OnlySpentTogetherIsExamined)
{
    for (const FillProof other : {FillProof::Unknown, FillProof::Settled, FillProof::Dead,
                                  FillProof::Live}) {
        FillProofResult in;
        in.verdict = other;
        in.height = 9297025;
        const FillProofResult out = ex::prove_take_from_children(
            in, names_of(ask_wallet()), ask_children(),
            {ex::SettlementCoin{settlement_puzzle("xch"), 1000000000000ULL}});
        EXPECT_EQ(out.verdict, other) << ex::fill_proof_name(other);
        EXPECT_EQ(out.height, 9297025U) << ex::fill_proof_name(other);
    }
}

// ---------------------------------------------------------------------------
// Stage 2 from the wallet: the payment we asked for
// ---------------------------------------------------------------------------

TEST(TakeFromPayments, APaymentFromOurOwnMakerCoinIsNoMark)
{
    // Change from a maker coin can match a requested amount; a take's
    // payment never comes from one.
    std::vector<json> payments = ask_payments();
    payments[0]["parent_coin_info"] =
        "0x10e8fca4a290ed992202011788a5a0385057ac5a089082d143004a1b3b903289";
    EXPECT_EQ(by_wallet(ask_wallet(), ask_records(), payments).verdict, FillProof::Unknown);
}

TEST(TakeFromPayments, APaymentAtAnotherHeightIsNoMark)
{
    std::vector<json> payments = ask_payments();
    payments[0]["confirmed_height"] = 9297026;
    EXPECT_EQ(by_wallet(ask_wallet(), ask_records(), payments).verdict, FillProof::Unknown);
}

TEST(TakeFromPayments, APaymentForAnotherAmountIsNoMark)
{
    std::vector<json> payments = ask_payments();
    payments[0]["amount"] = 85095;
    EXPECT_EQ(by_wallet(ask_wallet(), ask_records(), payments).verdict, FillProof::Unknown);
}

TEST(TakeFromPayments, AnUnreadablePaymentIsNoAnswer)
{
    std::vector<json> text_amount = ask_payments();
    text_amount[0]["amount"] = "85094";
    EXPECT_EQ(by_wallet(ask_wallet(), ask_records(), text_amount).verdict, FillProof::Unknown);

    std::vector<json> no_height = ask_payments();
    no_height[0].erase("confirmed_height");
    EXPECT_EQ(by_wallet(ask_wallet(), ask_records(), no_height).verdict, FillProof::Unknown);
}

TEST(TakeFromPayments, TheWalletsSilenceIsNeverDead)
{
    EXPECT_EQ(by_wallet(ask_wallet(), ask_records(), {}).verdict, FillProof::Unknown);

    json no_summary = ask_wallet();
    no_summary.erase("summary");
    EXPECT_EQ(by_wallet(no_summary, ask_records(), ask_payments()).verdict, FillProof::Unknown);
}

TEST(TakeFromPayments, OnlySpentTogetherIsExamined)
{
    for (const FillProof other : {FillProof::Unknown, FillProof::Settled, FillProof::Dead,
                                  FillProof::Live}) {
        FillProofResult in;
        in.verdict = other;
        in.height = 9297025;
        const FillProofResult out = ex::prove_take_from_payments(
            in, names_of(ask_wallet()), ask_payments(), {85094ULL});
        EXPECT_EQ(out.verdict, other) << ex::fill_proof_name(other);
        EXPECT_EQ(out.height, 9297025U) << ex::fill_proof_name(other);
    }
}

// ---------------------------------------------------------------------------
// The record readers
// ---------------------------------------------------------------------------

TEST(SummaryAmounts, ReadsTheWalletsStringsAndNumbers)
{
    EXPECT_EQ(ex::summary_amounts(ask_wallet(), "offered"),
              std::vector<std::uint64_t>{1000000000000ULL});
    EXPECT_EQ(ex::summary_amounts(ask_wallet(), "requested"),
              std::vector<std::uint64_t>{85094ULL});
    const json numbers = {{"summary", {{"offered", {{"xch", 5}, {"dbx", 7U}}}}}};
    std::vector<std::uint64_t> read = ex::summary_amounts(numbers, "offered");
    std::sort(read.begin(), read.end());
    EXPECT_EQ(read, (std::vector<std::uint64_t>{5ULL, 7ULL}));
}

TEST(SummaryAmounts, RefusesAnythingItCannotRead)
{
    const auto side = [](const json& value) {
        return ex::summary_amounts(json{{"summary", {{"offered", {{"xch", value}}}}}}, "offered");
    };
    for (const char* text : {"", "-5", "+5", " 5", "5x", "0", "18446744073709551616"}) {
        EXPECT_TRUE(side(text).empty()) << '"' << text << '"';
    }
    EXPECT_TRUE(side(0).empty());
    EXPECT_TRUE(side(-5).empty());
    EXPECT_TRUE(side(true).empty());
    EXPECT_TRUE(side(1.5).empty());

    // One bad amount voids the side: a partial list is a different offer.
    EXPECT_TRUE(ex::summary_amounts(
        json{{"summary", {{"offered", {{"xch", "5"}, {"dbx", "x"}}}}}}, "offered").empty());
    EXPECT_TRUE(ex::summary_amounts(json::object(), "offered").empty());
    EXPECT_TRUE(ex::summary_amounts(json{{"summary", json::object()}}, "offered").empty());
    EXPECT_TRUE(ex::summary_amounts(
        json{{"summary", {{"offered", json::object()}}}}, "offered").empty());
}

TEST(SummaryAssets, KeepsEachAmountsAsset)
{
    using Assets = std::vector<std::pair<std::string, std::uint64_t>>;
    EXPECT_EQ(ex::summary_assets(ask_wallet(), "offered"),
              (Assets{{"xch", 1000000000000ULL}}));
    EXPECT_EQ(ex::summary_assets(ask_wallet(), "requested"),
              (Assets{{"db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20",
                       85094ULL}}));
}

// [review #171, round 5] Each offered asset's settlement coin is known by its
// puzzle.  The golden values are the chain's own: the real takes' settlement
// coins, in the fixtures above.
TEST(SettlementPuzzleHash, MatchesTheChainsOwnSettlementCoins)
{
    const std::string dbx = "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20";
    const std::string offer_mod = "cfbfdeed5c4ca2de3d0bf520b9cb4bb7743a359bd2e6a188d19ce7dffc21d3e7";
    const std::string dbx_settlement = "2a8269fa3ec2a6968ee95219edc900ada54232beb24ad0f7581f3b3eab613e81";

    EXPECT_EQ(ex::CoinManager::settlement_puzzle_hash("xch").value_or(""), offer_mod);
    EXPECT_EQ(ex::CoinManager::settlement_puzzle_hash("XCH").value_or(""), offer_mod);
    EXPECT_EQ(ask_children()[1]["coin"]["puzzle_hash"], "0x" + offer_mod);

    // CAT v2 around OFFER_MOD, for DBX's TAIL: the real bid 0x18672b6b0f's
    // settlement coin.
    EXPECT_EQ(ex::CoinManager::settlement_puzzle_hash(dbx).value_or(""), dbx_settlement);
    EXPECT_EQ(ex::CoinManager::settlement_puzzle_hash("0x" + dbx).value_or(""), dbx_settlement);
    std::string upper = dbx;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    EXPECT_EQ(ex::CoinManager::settlement_puzzle_hash(upper).value_or(""), dbx_settlement);
    EXPECT_EQ(records_of(kBidChildren)[2]["coin"]["puzzle_hash"], "0x" + dbx_settlement);

    // Anything else cannot be named.
    for (const std::string& asset : {std::string{}, std::string{"dbx"}, std::string{"xch "},
                                     dbx.substr(1), dbx + "0", "zz" + dbx.substr(2)}) {
        EXPECT_FALSE(ex::CoinManager::settlement_puzzle_hash(asset).has_value()) << asset;
    }
}

TEST(OfferedSettlements, EveryOfferedAssetOrNothing)
{
    const auto settlements = [](const json& record) {
        return ex::offered_settlements(record, settlement_puzzle);
    };
    const std::vector<ex::SettlementCoin> ask = settlements(ask_wallet());
    ASSERT_EQ(ask.size(), 1U);
    EXPECT_EQ(ask[0].puzzle_hash_hex, settlement_puzzle("xch"));
    EXPECT_EQ(ask[0].amount, 1000000000000ULL);

    const std::vector<ex::SettlementCoin> bid = settlements(json::parse(kBidWallet));
    ASSERT_EQ(bid.size(), 1U);
    EXPECT_EQ(bid[0].puzzle_hash_hex,
              "2a8269fa3ec2a6968ee95219edc900ada54232beb24ad0f7581f3b3eab613e81");
    EXPECT_EQ(bid[0].amount, 84696ULL);

    // One asset that cannot be named voids the list, as one bad amount does.
    json partly = ask_wallet();
    partly["summary"]["offered"]["dbx"] = "100";
    EXPECT_TRUE(settlements(partly).empty());
    json no_summary = ask_wallet();
    no_summary.erase("summary");
    EXPECT_TRUE(settlements(no_summary).empty());
}

TEST(CoinRecordSpentHeight, ReadsEitherFieldAndRefusesADisagreement)
{
    EXPECT_EQ(ex::coin_record_spent_height(json{{"spent_block_index", 7}}), 7U);
    EXPECT_EQ(ex::coin_record_spent_height(json{{"spent_block_index", 0}}), 0U);
    EXPECT_EQ(ex::coin_record_spent_height(json{{"spent", false}}), 0U);
    EXPECT_FALSE(ex::coin_record_spent_height(json{{"spent", true}}).has_value());
    EXPECT_FALSE(ex::coin_record_spent_height(json{{"spent_block_index", -1}}).has_value());
    // An unreadable height is not "no height": the flag must not stand in.
    EXPECT_FALSE(ex::coin_record_spent_height(json{{"spent_block_index", "7"}, {"spent", false}}).has_value());
    EXPECT_FALSE(ex::coin_record_spent_height(json{{"spent_block_index", 7}, {"spent", false}}).has_value());
    EXPECT_FALSE(ex::coin_record_spent_height(json{{"spent_block_index", 0}, {"spent", true}}).has_value());
    EXPECT_FALSE(ex::coin_record_spent_height(json::object()).has_value());
    EXPECT_FALSE(ex::coin_record_spent_height(json::array()).has_value());
}

TEST(CoinRecordHeights, AHeightABlockHeightCannotHoldIsNoHeight)
{
    // [review round 3] The engine narrows every proven height to BlockHeight
    // (32 bits).  One past that range would wrap to an old block.
    constexpr std::uint64_t kTop = 4294967295ULL;
    EXPECT_EQ(ex::coin_record_spent_height(json{{"spent_block_index", kTop}}), kTop);
    EXPECT_FALSE(ex::coin_record_spent_height(json{{"spent_block_index", kTop + 1U}}).has_value());
    EXPECT_EQ(ex::coin_record_confirmed_height(json{{"confirmed_block_index", kTop}}), kTop);
    EXPECT_FALSE(ex::coin_record_confirmed_height(json{{"confirmed_block_index", kTop + 1U}}).has_value());
    EXPECT_FALSE(ex::coin_record_confirmed_height(json{{"confirmed_height", kTop + 1U}}).has_value());

    // 2^32 + 9,297,025 narrows to exactly the real take's height: as a spend
    // height it would have settled at 9,297,025.  It proves nothing instead.
    std::vector<json> wrapped = ask_records();
    wrapped[0]["spent_block_index"] = (kTop + 1U) + 9297025ULL;
    wrapped[0]["spent"] = true;
    EXPECT_EQ(prove(ask_wallet(), wrapped).verdict, FillProof::Unknown);
}

TEST(CoinRecordConfirmedHeight, ReadsTheNodesAndTheWalletsField)
{
    EXPECT_EQ(ex::coin_record_confirmed_height(json{{"confirmed_block_index", 7}}), 7U);
    EXPECT_EQ(ex::coin_record_confirmed_height(json{{"confirmed_height", 7}}), 7U);
    EXPECT_FALSE(ex::coin_record_confirmed_height(json{{"confirmed_block_index", -1}}).has_value());
    EXPECT_FALSE(ex::coin_record_confirmed_height(json{{"confirmed_height", "7"}}).has_value());
    EXPECT_FALSE(ex::coin_record_confirmed_height(json::object()).has_value());
    EXPECT_FALSE(ex::coin_record_confirmed_height(json::array()).has_value());
}

// ---------------------------------------------------------------------------
// live_offer_cancellable (review round 2)
// ---------------------------------------------------------------------------

TEST(LiveOfferCancellable, OnlyAFreshNodeLiveAtDepthPastTheWalletsClaim)
{
    // The wallet claims a take at 100.  The node, in the latest detect_fills
    // call (the 7th), six blocks past that and every maker coin unspent: the
    // take is not on its chain.
    EXPECT_TRUE(ex::live_offer_cancellable(FillProof::Live, true, 100U, 106U, 7U, 7U, 6U));
    EXPECT_TRUE(ex::live_offer_cancellable(FillProof::Live, true, 100U, 500U, 7U, 7U, 6U));

    // Five past: the node may simply not have the take yet.
    EXPECT_FALSE(ex::live_offer_cancellable(FillProof::Live, true, 100U, 105U, 7U, 7U, 6U));
    // The wallet's Live: its store is what mislabelled these offers.
    EXPECT_FALSE(ex::live_offer_cancellable(FillProof::Live, false, 100U, 106U, 7U, 7U, 6U));
    // [round 4] A proof an earlier call made, AT THE SAME HEIGHT: detect_fills
    // can run twice at one block, and the offer may have been taken since.
    EXPECT_FALSE(ex::live_offer_cancellable(FillProof::Live, true, 100U, 106U, 6U, 7U, 6U));
    // [round 4] Call 0 is no call: an offer held before its first proof.
    EXPECT_FALSE(ex::live_offer_cancellable(FillProof::Live, true, 100U, 106U, 0U, 0U, 6U));
    // No claimed height: nothing to measure the node against.
    EXPECT_FALSE(ex::live_offer_cancellable(FillProof::Live, true, 0U, 106U, 7U, 7U, 6U));
    // A claim above the node's own height: the node is behind the wallet.
    EXPECT_FALSE(ex::live_offer_cancellable(FillProof::Live, true, 200U, 106U, 7U, 7U, 6U));

    for (const FillProof other : {FillProof::Unknown, FillProof::Settled, FillProof::Dead,
                                  FillProof::SpentTogether}) {
        EXPECT_FALSE(ex::live_offer_cancellable(other, true, 100U, 106U, 7U, 7U, 6U))
            << ex::fill_proof_name(other);
    }
}

// ---------------------------------------------------------------------------
// dead_offer_closable
// ---------------------------------------------------------------------------

TEST(DeadOfferClosable, OnlyADeadVerdictAtConfirmationDepth)
{
    FillProofResult dead;
    dead.verdict = FillProof::Dead;
    dead.height = 100;
    EXPECT_FALSE(ex::dead_offer_closable(dead, 105U, 6U));   // 5 deep
    EXPECT_TRUE(ex::dead_offer_closable(dead, 106U, 6U));    // 6 deep
    EXPECT_FALSE(ex::dead_offer_closable(dead, 99U, 6U));    // before the spend
    EXPECT_TRUE(ex::dead_offer_closable(dead, 100U, 0U));    // depth 0: at once

    FillProofResult no_height = dead;
    no_height.height = 0;
    EXPECT_FALSE(ex::dead_offer_closable(no_height, 1000U, 6U));

    for (const FillProof other : {FillProof::Unknown, FillProof::Settled, FillProof::Live,
                                  FillProof::SpentTogether}) {
        FillProofResult not_dead = dead;
        not_dead.verdict = other;
        EXPECT_FALSE(ex::dead_offer_closable(not_dead, 1000U, 6U))
            << ex::fill_proof_name(other);
    }
}

// ---------------------------------------------------------------------------
// [review #171, round 16] Which refusal of a coin lookup is one offer's
// ---------------------------------------------------------------------------

TEST(CoinLookupRefusal, OnlyAMissingCoinIsOneOffersRefusal)
{
    // chia 2.7.4 wallet_rpc_api.get_coin_records_by_names, verbatim: the one
    // refusal that names this request's coins...
    EXPECT_TRUE(ex::coin_lookup_refusal_is_offer_local(
        "Coin ID's: ['0x6d4f1b0a2c9e8f7d6b5a4c3e2f1d0b9a8c7e6f5d4b3a2c1e0f9d8b7a6c5e4f3d']"
        " not found."));
    // ...and the three it gives before looking at any coin.
    EXPECT_FALSE(ex::coin_lookup_refusal_is_offer_local(
        "Wallet is not connected to any synced peers."));
    EXPECT_FALSE(ex::coin_lookup_refusal_is_offer_local(
        "Wallet needs to be fully synced before finding coin information"));
    EXPECT_FALSE(ex::coin_lookup_refusal_is_offer_local(
        "No full node peers connected. Please connect to a full node."));
    // rpc_post's text for a refusal that names no error, and nothing at all.
    EXPECT_FALSE(ex::coin_lookup_refusal_is_offer_local("RPC returned success=false"));
    EXPECT_FALSE(ex::coin_lookup_refusal_is_offer_local(""));
}

}  // namespace
