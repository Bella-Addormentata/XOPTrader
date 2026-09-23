// test_fill_proof.cpp -- [FILL-PROOF 2026-09-23]
//
// Is a trade the wallet reports CONFIRMED really a fill
// (execution/fill_proof.hpp)?  The first three tests replay REAL data: the
// wallet's trade record and the full node's coin records as read on
// 2026-09-23, trimmed to the fields the proof reads.
//
//   * 0xdb63709cb9 -- an XCH/BYC bid booked as a fill on 2026-09-22.  The
//     wallet says CONFIRMED at 9,325,694; the node says one of its three
//     maker coins was spent there and the other two never were.  Dead.
//   * 0x202ff7d2d8 -- an XCH/DBX ask that really was taken on 2026-09-16
//     (Dexie: completed).  Both maker coins spent at 9,297,025.  Settled.
//
// The rest derive from the genuine fill's records, so every coin name is a
// real one, computed by CoinManager::compute_coin_name as the engine does.

#include <gtest/gtest.h>

#include <xop/execution/coin_manager.hpp>
#include <xop/execution/fill_proof.hpp>
#include <xop/types.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <string>
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

// get_offer(file_contents=false), trimmed to the fields the proof reads.
constexpr const char* kPhantomWalletRecord = R"({"trade_id":"0xdb63709cb9b2c3794cbf1a57bb15f2d5c1830dca620b16ac36926d0d60c6c556","status":"CONFIRMED","confirmed_at_index":9325694,"coins_of_interest":[{"amount":21284030165,"parent_coin_info":"0x82ee60315b50419a1cbaf52b44bbc98c787129307fc2578e87995cfba73571b7","puzzle_hash":"0xf4d66eb50f4a2d721cc58a4940f3d678989da55af0387d5f7087403521913dcb"},{"amount":1657,"parent_coin_info":"0xf88d5c260891d06b0daf16f22e974c67b1341f2d1bc8a404e374fdcc475abfbc","puzzle_hash":"0x47d29d6d58ccdb7f597c82b3e3c6f9524b2b8aee03414e5971e67ff0241f5955"},{"amount":1600,"parent_coin_info":"0xdf311e9bd2b13703ae87b9e814a8df3d939d03f508f6abc41b96f265479eb469","puzzle_hash":"0xa240a96af2d708713ff6ef3281dd4d47a737961e8899fa8f49a3f9ed62296a78"}]})";

// full_node get_coin_records_by_names, include_spent_coins, trimmed.
constexpr const char* kPhantomNodeRecords = R"([{"coin":{"amount":21284030165,"parent_coin_info":"0x82ee60315b50419a1cbaf52b44bbc98c787129307fc2578e87995cfba73571b7","puzzle_hash":"0xf4d66eb50f4a2d721cc58a4940f3d678989da55af0387d5f7087403521913dcb"},"confirmed_block_index":9325687,"spent_block_index":9325694,"spent":true},{"coin":{"amount":1600,"parent_coin_info":"0xdf311e9bd2b13703ae87b9e814a8df3d939d03f508f6abc41b96f265479eb469","puzzle_hash":"0xa240a96af2d708713ff6ef3281dd4d47a737961e8899fa8f49a3f9ed62296a78"},"confirmed_block_index":9245763,"spent_block_index":0,"spent":false},{"coin":{"amount":1657,"parent_coin_info":"0xf88d5c260891d06b0daf16f22e974c67b1341f2d1bc8a404e374fdcc475abfbc","puzzle_hash":"0x47d29d6d58ccdb7f597c82b3e3c6f9524b2b8aee03414e5971e67ff0241f5955"},"confirmed_block_index":9325248,"spent_block_index":0,"spent":false}])";

constexpr const char* kGenuineWalletRecord = R"({"trade_id":"0x202ff7d2d8554a76bf97df827ce65965d5a6a5619496780f698c8222b9a3fcec","status":"CONFIRMED","confirmed_at_index":9297025,"coins_of_interest":[{"amount":999935419490,"parent_coin_info":"0x2c4d49c77fd91113a3717d230cb7c1d44bec20ccfcf146064d9549a86629061e","puzzle_hash":"0x85b236977ac595209c59276f8074b5c4ce9320ee9d7e7987cd201a1b2dcb6360"},{"amount":999955647482,"parent_coin_info":"0xffb0885ba8babec7a41b05aee34195aaf5843a694804f39d5e35fdb784ce2db3","puzzle_hash":"0xe2d5898d50eb165f06653c1e7fcc0e1d626d9181a04ee8fd94ba98ad28bbfa1f"}]})";

constexpr const char* kGenuineNodeRecords = R"([{"coin":{"amount":999955647482,"parent_coin_info":"0xffb0885ba8babec7a41b05aee34195aaf5843a694804f39d5e35fdb784ce2db3","puzzle_hash":"0xe2d5898d50eb165f06653c1e7fcc0e1d626d9181a04ee8fd94ba98ad28bbfa1f"},"confirmed_block_index":9291973,"spent_block_index":9297025,"spent":true},{"coin":{"amount":999935419490,"parent_coin_info":"0x2c4d49c77fd91113a3717d230cb7c1d44bec20ccfcf146064d9549a86629061e","puzzle_hash":"0x85b236977ac595209c59276f8074b5c4ce9320ee9d7e7987cd201a1b2dcb6360"},"confirmed_block_index":9292565,"spent_block_index":9297025,"spent":true}])";

std::vector<std::string> names_of(const json& wallet_record)
{
    return ex::coin_names_for(ex::parse_coins_of_interest(wallet_record), chain_name);
}

std::vector<json> records_of(const json& node_records)
{
    return std::vector<json>(node_records.begin(), node_records.end());
}

FillProofResult prove(const json& wallet_record, const std::vector<json>& records)
{
    return ex::prove_fill(names_of(wallet_record), records, chain_name);
}

json genuine_wallet() { return json::parse(kGenuineWalletRecord); }
std::vector<json> genuine_records() { return records_of(json::parse(kGenuineNodeRecords)); }

// ---------------------------------------------------------------------------
// The real cases
// ---------------------------------------------------------------------------

TEST(FillProof, TheCoinNamesAreTheChainsOwn)
{
    // The names the proof asks for are the ids Dexie and the node use.
    const std::vector<std::string> names = names_of(json::parse(kPhantomWalletRecord));
    ASSERT_EQ(names.size(), 3U);
    EXPECT_EQ(names[0], "4ea3d75d6698817cfa79a8e5b609ced34fc10bb4d889db437525b85d42bc8413");
    EXPECT_EQ(names[1], "fe636075405d34d1f22d803c282b6c207fb1e4fa46e4a8b6eb872b4556566f67");
}

TEST(FillProof, ThePhantomFillOf20260922IsDead)
{
    const FillProofResult proof = prove(json::parse(kPhantomWalletRecord),
                                        records_of(json::parse(kPhantomNodeRecords)));
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9325694U);   // the fee coin's spend: the wallet's "confirmed" height
    EXPECT_EQ(proof.coins, 3U);
    EXPECT_EQ(proof.unspent, 2U);
    // Nearly 4,700 blocks deep by 2026-09-23: closable at the default depth.
    EXPECT_TRUE(ex::dead_offer_closable(proof, 9330000U, 6U));
}

TEST(FillProof, AGenuineFillIsSettled)
{
    const FillProofResult proof = prove(genuine_wallet(), genuine_records());
    EXPECT_EQ(proof.verdict, FillProof::Settled);
    EXPECT_EQ(proof.height, 9297025U);   // the wallet's confirmed_at_index too
    EXPECT_EQ(proof.coins, 2U);
    EXPECT_EQ(proof.unspent, 0U);
    EXPECT_FALSE(ex::dead_offer_closable(proof, 9330000U, 6U));
}

// ---------------------------------------------------------------------------
// Variations on the genuine fill
// ---------------------------------------------------------------------------

TEST(FillProof, CoinsSpentAtDifferentHeightsAreDead)
{
    // What the phantom's surviving coins would show once a later offer reused
    // them: every coin spent, but not together.  "All spent" is not a take.
    std::vector<json> records = genuine_records();
    records[1]["spent_block_index"] = 9297030;
    const FillProofResult proof = prove(genuine_wallet(), records);
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9297025U);   // the earliest spend
    EXPECT_EQ(proof.unspent, 0U);
}

TEST(FillProof, EveryCoinUnspentIsLive)
{
    std::vector<json> records = genuine_records();
    for (auto& record : records) {
        record["spent_block_index"] = 0;
        record["spent"] = false;
    }
    const FillProofResult proof = prove(genuine_wallet(), records);
    EXPECT_EQ(proof.verdict, FillProof::Live);
    EXPECT_EQ(proof.unspent, 2U);
    EXPECT_FALSE(ex::dead_offer_closable(proof, 9330000U, 6U));
}

TEST(FillProof, OneCoinSpentAndOneNotIsDead)
{
    std::vector<json> records = genuine_records();
    records[0]["spent_block_index"] = 0;
    records[0]["spent"] = false;
    const FillProofResult proof = prove(genuine_wallet(), records);
    EXPECT_EQ(proof.verdict, FillProof::Dead);
    EXPECT_EQ(proof.height, 9297025U);
    EXPECT_EQ(proof.unspent, 1U);
}

TEST(FillProof, AMakerCoinTheAnswerOmitsIsNoProof)
{
    // The node omits coins it does not find; the one it returned is spent.
    std::vector<json> records = genuine_records();
    records.pop_back();
    EXPECT_EQ(prove(genuine_wallet(), records).verdict, FillProof::Unknown);
    EXPECT_EQ(prove(genuine_wallet(), {}).verdict, FillProof::Unknown);
}

TEST(FillProof, ARecordForACoinNotAskedAboutIsNoProof)
{
    std::vector<json> records = genuine_records();
    records.push_back(records_of(json::parse(kPhantomNodeRecords))[0]);
    EXPECT_EQ(prove(genuine_wallet(), records).verdict, FillProof::Unknown);
}

TEST(FillProof, AContradictoryRecordIsNoProof)
{
    std::vector<json> spent_without_height = genuine_records();
    spent_without_height[0]["spent_block_index"] = 0;   // "spent": true stays
    EXPECT_EQ(prove(genuine_wallet(), spent_without_height).verdict, FillProof::Unknown);

    std::vector<json> height_but_unspent = genuine_records();
    height_but_unspent[0]["spent"] = false;              // the height stays
    EXPECT_EQ(prove(genuine_wallet(), height_but_unspent).verdict, FillProof::Unknown);
}

TEST(FillProof, AnUnreadableRecordIsNoProof)
{
    std::vector<json> string_height = genuine_records();
    string_height[0]["spent_block_index"] = "9297025";
    EXPECT_EQ(prove(genuine_wallet(), string_height).verdict, FillProof::Unknown);

    std::vector<json> no_coin = genuine_records();
    no_coin[0].erase("coin");
    EXPECT_EQ(prove(genuine_wallet(), no_coin).verdict, FillProof::Unknown);

    // Every maker coin answered, plus a record with no coin in it: the answer
    // is not the one that was asked for, whatever else it holds.
    std::vector<json> junk_added = genuine_records();
    junk_added.push_back(json{{"spent_block_index", 0}, {"spent", false}});
    EXPECT_EQ(prove(genuine_wallet(), junk_added).verdict, FillProof::Unknown);
}

TEST(FillProof, DuplicateRecordsMustAgree)
{
    std::vector<json> agreeing = genuine_records();
    agreeing.push_back(agreeing[0]);
    EXPECT_EQ(prove(genuine_wallet(), agreeing).verdict, FillProof::Settled);

    std::vector<json> disagreeing = genuine_records();
    json copy = disagreeing[0];
    copy["spent_block_index"] = 9297026;
    disagreeing.push_back(copy);
    EXPECT_EQ(prove(genuine_wallet(), disagreeing).verdict, FillProof::Unknown);
}

TEST(FillProof, NoCoinsToAskAboutIsNoProof)
{
    EXPECT_EQ(ex::prove_fill(std::vector<std::string>{}, genuine_records(), chain_name).verdict,
              FillProof::Unknown);
}

// ---------------------------------------------------------------------------
// coin_record_spent_height
// ---------------------------------------------------------------------------

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

    for (const FillProof other : {FillProof::Unknown, FillProof::Settled, FillProof::Live}) {
        FillProofResult not_dead = dead;
        not_dead.verdict = other;
        EXPECT_FALSE(ex::dead_offer_closable(not_dead, 1000U, 6U))
            << ex::fill_proof_name(other);
    }
}

}  // namespace
