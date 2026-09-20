// test_node_requests.cpp -- [S67 2026-09-20] the full node's fee endpoints:
// the get_fee_estimate payload, its reply, and the mempool fields of
// get_blockchain_state.
//
// The defect these pin lived in a KEY NAME nothing but the node ever read:
// "spend_type": "send_xch_transaction" asks about a 9.4M-cost XCH send, and
// the answer was paid for 42M-cost CAT cancels and 93M+ takes.  Checked
// against chia-blockchain tag 2.7.4, chia/full_node/full_node_rpc_api.py
// (get_fee_estimate, _validate_fee_estimate_cost, get_blockchain_state).

#include <gtest/gtest.h>

#include <xop/rpc/node_requests.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;
namespace rpc = xop::rpc;

// ---------------------------------------------------------------------------
// Request shape
// ---------------------------------------------------------------------------

TEST(NodeRequests, CostAwareEstimateNamesExactlyOneCostSource)
{
    const json req = rpc::make_fee_estimate_request(300, 42'300'000ULL);
    // _validate_fee_estimate_cost: exactly one of these three, or the request
    // is refused with REQUEST_MUST_CONTAIN_EXACTLY_ONE.
    int sources = 0;
    for (const char* key : {"spend_bundle", "cost", "spend_type"}) {
        if (req.contains(key)) { ++sources; }
    }
    EXPECT_EQ(sources, 1);
    ASSERT_TRUE(req.contains("cost"));
    EXPECT_TRUE(req["cost"].is_number_unsigned());
    EXPECT_EQ(req["cost"].get<std::uint64_t>(), 42'300'000ULL);
    // target_times is an ARRAY of seconds (_validate_target_times iterates it).
    ASSERT_TRUE(req.contains("target_times"));
    ASSERT_TRUE(req["target_times"].is_array());
    ASSERT_EQ(req["target_times"].size(), 1U);
    EXPECT_EQ(req["target_times"][0].get<std::uint64_t>(), 300ULL);
    EXPECT_EQ(req.size(), 2U);
}

TEST(NodeRequests, LegacyEstimateRequestIsByteForByteWhatMainSent)
{
    // fees.cost_aware_estimate: false must change nothing on the wire.
    const json req = rpc::make_fee_estimate_request_legacy(300);
    EXPECT_EQ(req.dump(), R"({"spend_type":"send_xch_transaction","target_times":[300]})");
}

TEST(NodeRequests, ReferenceCostKeepsNineDigitsOfRate)
{
    // The handler returns uint64(rate x cost).  At the live rate 0.373 mojos
    // per cost the reference cost yields 373,000,000 -- nine digits -- where a
    // cost of 1 would have truncated the answer to 0.
    EXPECT_EQ(rpc::kFeeEstimateReferenceCost, 1'000'000'000ULL);
    const json resp = {{"estimates", {373'000'000ULL}}, {"success", true}};
    const auto r = rpc::parse_fee_estimate(resp, rpc::kFeeEstimateReferenceCost);
    ASSERT_TRUE(r.ok);
    EXPECT_DOUBLE_EQ(r.rate, 0.373);
}

// ---------------------------------------------------------------------------
// get_fee_estimate reply
// ---------------------------------------------------------------------------

TEST(NodeRequests, ParsesAFullReply)
{
    // Field names verbatim from the 2.7.4 handler's return dict.  NOTE
    // "mempool_size" here is the mempool's total COST, not its item count.
    const json resp = {
        {"estimates", {5'600'000ULL}},
        {"target_times", {300}},
        {"current_fee_rate", 0.4},
        {"mempool_size", 109'000'000'000ULL},
        {"mempool_fees", 1'234ULL},
        {"num_spends", 800},
        {"mempool_max_size", 110'000'000'000ULL},
        {"full_node_synced", true},
        {"success", true}};
    const auto r = rpc::parse_fee_estimate(resp, 15'000'000ULL);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.estimate_mojos, 5'600'000ULL);
    EXPECT_NEAR(r.rate, 5'600'000.0 / 15'000'000.0, 1e-15);
    ASSERT_TRUE(r.mempool_known);
    EXPECT_EQ(r.mempool_cost, 109'000'000'000ULL);
    EXPECT_EQ(r.mempool_max_cost, 110'000'000'000ULL);
}

TEST(NodeRequests, AnEmptyMempoolEstimateOfZeroIsAReadingNotAFailure)
{
    const auto r = rpc::parse_fee_estimate(json{{"estimates", {0}}}, 1'000);
    EXPECT_TRUE(r.ok);
    EXPECT_DOUBLE_EQ(r.rate, 0.0);
    EXPECT_FALSE(r.mempool_known);   // absent fields are UNKNOWN, not zero
}

TEST(NodeRequests, UnreadableRepliesAreNotOk)
{
    EXPECT_FALSE(rpc::parse_fee_estimate(json::object(), 1'000).ok);
    EXPECT_FALSE(rpc::parse_fee_estimate(json{{"estimates", json::array()}}, 1'000).ok);
    EXPECT_FALSE(rpc::parse_fee_estimate(json{{"estimates", "5"}}, 1'000).ok);
    EXPECT_FALSE(rpc::parse_fee_estimate(json{{"estimates", {-5}}}, 1'000).ok);
    EXPECT_FALSE(rpc::parse_fee_estimate(json{{"estimates", {1.5}}}, 1'000).ok);
    EXPECT_FALSE(rpc::parse_fee_estimate(json::array(), 1'000).ok);
    // A zero cost cannot yield a rate.
    EXPECT_FALSE(rpc::parse_fee_estimate(json{{"estimates", {5}}}, 0).ok);
    // A mistyped mempool field leaves the mempool unknown but the rate usable.
    const auto r = rpc::parse_fee_estimate(
        json{{"estimates", {5}}, {"mempool_size", "big"}, {"mempool_max_size", 10}}, 5);
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.mempool_known);
}

// ---------------------------------------------------------------------------
// get_blockchain_state mempool fields
// ---------------------------------------------------------------------------

json blockchain_state(std::uint64_t cost, std::uint64_t max_cost, json min_rate)
{
    return json{{"blockchain_state",
                 {{"mempool_size", 800},
                  {"mempool_cost", cost},
                  {"mempool_fees", 1},
                  {"mempool_min_fees", {{"cost_5000000", std::move(min_rate)}}},
                  {"mempool_max_total_cost", max_cost},
                  {"block_max_cost", 11'000'000'000ULL}}},
                {"success", true}};
}

TEST(NodeRequests, ReadsTheMempoolFromBlockchainState)
{
    const auto m = rpc::node_mempool_from_blockchain_state(
        blockchain_state(109'990'000'000ULL, 110'000'000'000ULL, 7.25));
    ASSERT_TRUE(m.known);
    EXPECT_EQ(m.cost, 109'990'000'000ULL);
    EXPECT_EQ(m.max_total_cost, 110'000'000'000ULL);   // 10 blocks of 11e9
    EXPECT_DOUBLE_EQ(m.min_fee_rate_5m, 7.25);
    // get_min_fee_rate returns the INTEGER 0 when the mempool has room.
    EXPECT_TRUE(rpc::node_mempool_from_blockchain_state(
                    blockchain_state(1'000, 110'000'000'000ULL, 0)).known);
}

TEST(NodeRequests, AMissingMempoolFieldIsUnknownNeverEmpty)
{
    json resp = blockchain_state(5, 10, 0);
    resp["blockchain_state"].erase("mempool_cost");
    EXPECT_FALSE(rpc::node_mempool_from_blockchain_state(resp).known);
    resp = blockchain_state(5, 10, 0);
    resp["blockchain_state"].erase("mempool_min_fees");
    EXPECT_FALSE(rpc::node_mempool_from_blockchain_state(resp).known);
    resp = blockchain_state(5, 10, 0);
    resp["blockchain_state"]["mempool_min_fees"] = json::object();
    EXPECT_FALSE(rpc::node_mempool_from_blockchain_state(resp).known);
    // The cached startup reply carries zeros: a zero max cost is "not up yet".
    EXPECT_FALSE(rpc::node_mempool_from_blockchain_state(blockchain_state(0, 0, 0)).known);
    EXPECT_FALSE(rpc::node_mempool_from_blockchain_state(blockchain_state(5, 10, "x")).known);
    EXPECT_FALSE(rpc::node_mempool_from_blockchain_state(blockchain_state(5, 10, -1.0)).known);
    EXPECT_FALSE(rpc::node_mempool_from_blockchain_state(json::object()).known);
    EXPECT_FALSE(rpc::node_mempool_from_blockchain_state(json{{"blockchain_state", 7}}).known);
    EXPECT_FALSE(rpc::node_mempool_from_blockchain_state(json::array()).known);
}

// ---------------------------------------------------------------------------
// The admission floor
// ---------------------------------------------------------------------------

TEST(NodeRequests, AdmissionFloorIsZeroWithRoomAndAtLeastFiveAtCapacity)
{
    rpc::MempoolState m;
    m.known = true;
    m.max_total_cost = 110'000'000'000ULL;
    m.cost = 100'000'000'000ULL;                   // 10G of room
    EXPECT_DOUBLE_EQ(rpc::admission_floor_rate(m, 125'000'000ULL), 0.0);
    // mempool.py at_full_capacity: total + cost > max.  Exactly fitting is room.
    m.cost = m.max_total_cost - 125'000'000ULL;
    EXPECT_DOUBLE_EQ(rpc::admission_floor_rate(m, 125'000'000ULL), 0.0);
    m.cost += 1;
    EXPECT_DOUBLE_EQ(rpc::admission_floor_rate(m, 125'000'000ULL), 5.0);
    // The same mempool still has room for a cheaper spend.
    EXPECT_DOUBLE_EQ(rpc::admission_floor_rate(m, 8'400'000ULL), 0.0);
    // Above 5, the node's own minimum governs.
    m.min_fee_rate_5m = 7.25;
    EXPECT_DOUBLE_EQ(rpc::admission_floor_rate(m, 125'000'000ULL), 7.25);
    m.min_fee_rate_5m = 1.0;
    EXPECT_DOUBLE_EQ(rpc::admission_floor_rate(m, 125'000'000ULL), 5.0);
    // Over-full (cost > max) must not underflow the subtraction.
    m.cost = m.max_total_cost + 9;
    EXPECT_DOUBLE_EQ(rpc::admission_floor_rate(m, 1), 5.0);
    // Unknown is not "full" and not "empty": no floor.
    m.known = false;
    EXPECT_DOUBLE_EQ(rpc::admission_floor_rate(m, 125'000'000ULL), 0.0);
}

}  // namespace
