// ---------------------------------------------------------------------------
// [BULKCANCEL 2026-09-11] The wire format of the two wallet RPCs whose
// payloads were wrong on main, and the row ordering those payloads select.
//
// Both defects were invisible to every existing test because the only
// evidence of either is the KEY NAMES in a JSON object that nothing but the
// Chia daemon ever read.  These pin the keys, and simulate the daemon's
// documented ORDER BY over a synthetic 250-row table so the 200-row window
// each consumer actually reads is observable from ctest.
//
// [S33 2026-09-11] WHAT THE ORDERING TESTS DO AND DO NOT PROVE.  TxOrderRow
// and order_as_daemon() below are a TEST-ONLY MODEL of the daemon's
// chia/wallet/transaction_sorting.py -- they have no production caller and
// they used to live in the production header wallet_requests.hpp, where
// they would eventually have been read as a shipped contract.  They are
// test code and they live here now.  So the three ordering tests validate
// THIS ENCODING of the daemon's documented SQL against the `reverse` value
// production actually sends; they cannot catch a wrong model of the daemon
// itself.  The shipped half of the claim -- that the payload carries
// sort_key RELEVANCE with reverse=false, for the wallet and window asked
// for -- is pinned directly by GetTransactionsForwardsTheWindowItIsGiven.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "xop/rpc/wallet_requests.hpp"

using nlohmann::json;
using xop::rpc::cancel_batch_cost_ceiling;
using xop::rpc::cancel_offers_batch_count;
using xop::rpc::kCancelCoinsPerOfferCeiling;
using xop::rpc::kCancelFeeSpendCostCeiling;
using xop::rpc::kCancelOffersSingleBatchSize;
using xop::rpc::kMempoolMaxTxClvmCost;
using xop::rpc::make_cancel_offers_request;
using xop::rpc::census_full_node_peers;
using xop::rpc::is_localhost_peer_host;
using xop::rpc::kNodeTypeFullNode;
using xop::rpc::make_get_connections_request;
using xop::rpc::make_get_timestamp_for_height_request;
using xop::rpc::make_get_transactions_request;
using xop::rpc::parse_timestamp_for_height_response;

namespace {

// ---------------------------------------------------------------------------
// The daemon ORDER BY model (test-only -- see the file banner).
// ---------------------------------------------------------------------------

/// The three columns chia/wallet/transaction_sorting.py RELEVANCE orders by.
struct TxOrderRow {
    bool         confirmed{false};
    std::int64_t confirmed_at_height{0};
    std::int64_t created_at_time{0};
};

/// Order `rows` the way the wallet daemon's SQL would for `request`.
///
/// chia/wallet/transaction_sorting.py:
///   RELEVANCE  = "ORDER BY confirmed {ASC}, confirmed_at_height {DESC},
///                          created_at_time {DESC}"
///   ascending()  substitutes ASC="ASC",  DESC="DESC"   (reverse=false)
///   descending() substitutes ASC="DESC", DESC="ASC"    (reverse=true)
///
/// So the two settings are not "same rows, flipped" -- they select a
/// different 200-row WINDOW out of a 33,000-row table:
///
///   reverse=true  -> confirmed DESC, height ASC  -> CONFIRMED rows first and
///                    OLDEST first.  Unconfirmed rows sort LAST, so past 200
///                    confirmed transactions they are unreachable.
///   reverse=false -> confirmed ASC,  height DESC -> UNCONFIRMED rows first,
///                    then confirmed NEWEST first.
///
/// Only RELEVANCE is modelled; any other sort_key returns `rows` untouched,
/// so a test pinning an ordering fails loudly rather than passing vacuously.
///
/// This encodes the daemon's documented contract, not the daemon itself.
[[nodiscard]] std::vector<TxOrderRow> order_as_daemon(
    std::vector<TxOrderRow> rows, const json& request)
{
    if (request.value("sort_key", std::string{}) != "RELEVANCE") {
        return rows;
    }
    const bool reverse = request.value("reverse", false);

    std::stable_sort(rows.begin(), rows.end(),
                     [reverse](const TxOrderRow& a, const TxOrderRow& b) {
        if (a.confirmed != b.confirmed) {
            // confirmed ASC puts false (unconfirmed) first; DESC puts true
            // (confirmed) first.
            return reverse ? (a.confirmed > b.confirmed)
                           : (a.confirmed < b.confirmed);
        }
        if (a.confirmed_at_height != b.confirmed_at_height) {
            return reverse ? (a.confirmed_at_height < b.confirmed_at_height)
                           : (a.confirmed_at_height > b.confirmed_at_height);
        }
        return reverse ? (a.created_at_time < b.created_at_time)
                       : (a.created_at_time > b.created_at_time);
    });
    return rows;
}

/// The window both consumers read: get_transactions(wallet_id, 0, 200).
constexpr std::size_t kWindow = 200;

/// 249 confirmed rows (ascending height) plus one unconfirmed row, which is
/// how a busy wallet looks: the live XCH wallet carries 32,966 rows and the
/// DBX reward wallet 9,640, so the window is a small slice of a large table.
std::vector<TxOrderRow> busy_wallet_rows() {
    std::vector<TxOrderRow> rows;
    rows.reserve(250);
    for (std::int64_t i = 0; i < 249; ++i) {
        rows.push_back(TxOrderRow{true, 9'000'000LL + i, 1'700'000'000LL + i});
    }
    // Unconfirmed: height 0, created most recently.  This is the row the
    // stuck-transaction pruner exists to find.
    rows.push_back(TxOrderRow{false, 0, 1'700'000'999LL});
    return rows;
}

}  // namespace

// ---------------------------------------------------------------------------
// cancel_offers -- the request the daemon actually reads
// ---------------------------------------------------------------------------

TEST(WalletRequests, CancelOffersPassesTheFeeAsBatchFee) {
    const json req = make_cancel_offers_request(10'000'000ULL, /*secure=*/true);

    // The handler reads request.batch_fee.  A "fee" key is silently ignored,
    // which is how every bulk cancel on main went out at ZERO fee.
    ASSERT_TRUE(req.contains("batch_fee"));
    EXPECT_EQ(req["batch_fee"].get<std::uint64_t>(), 10'000'000ULL);
    EXPECT_FALSE(req.contains("fee"));
}

TEST(WalletRequests, CancelOffersSetsCancelAll) {
    const json req = make_cancel_offers_request(0, /*secure=*/true);

    // Without cancel_all the handler's query_key is None; None keys XCH in
    // Offer.arbitrage(), so the filter keeps only offers with an XCH leg and
    // a CAT/CAT offer is never cancelled.  The paging loop then breaks on the
    // first batch that matched nothing, stopping the scan early.
    EXPECT_TRUE(req.value("cancel_all", false));
}

TEST(WalletRequests, CancelOffersIsOneBatch) {
    // [BULKCANCEL-B 2026-09-13] A wallet-wide sweep must go out as ONE batch.
    // In chia 2.7.4 every batch pays batch_fee again, and a batch that funds
    // its fee from the free XCH pool picks that coin blind to the batches
    // before it and is pushed as its own bundle -- so a sweep split into
    // batches cannot be funded as a whole.  See wallet_requests.hpp.
    const json req = make_cancel_offers_request(0, /*secure=*/true);
    const int batch_size = req.value("batch_size", -1);
    EXPECT_EQ(batch_size, kCancelOffersSingleBatchSize);

    // OfferManager::cancel_all reserves for at least 25 offers when it cannot
    // see the wallet's book (kUnknownWalletBookBound, offer_manager.cpp), and
    // the live wallet held 22 open offers of its own on 2026-09-13.  The old
    // batch size of 5 split either book into five batches.
    EXPECT_GE(batch_size, 25);
    EXPECT_EQ(cancel_offers_batch_count(25, batch_size), 1);

    // CancelOffers.batch_size is a uint16 in chia 2.7.4
    // (wallet_request_types.py).
    EXPECT_LE(batch_size, 65535);
}

TEST(WalletRequests, OneBatchFitsTheMempoolCostBoundAtTwoSpendsPerOffer) {
    // [BULKCANCEL-B 2026-09-13] The risk the old batch size guarded against is
    // real: mempool_manager.py rejects a bundle costing more than
    // max_tx_clvm_cost, and one rejected bundle takes every cancel in it down.
    // So the single batch is sized under that bound at measured per-spend
    // costs rather than raised without limit.
    //
    // [review 2026-09-13, operator decision] The ceilings are per cancellation
    // SPEND, and an offer can need more than one, so the spend count is an
    // explicit argument, costed at kCancelCoinsPerOfferCeiling: two, twice the
    // shape of 18,185 of the 18,187 offers this wallet has cancelled.  See THE
    // TWO-SPEND BASIS in wallet_requests.hpp.
    EXPECT_EQ(kMempoolMaxTxClvmCost, 5'500'000'000ULL);  // 11e9 // 2
    EXPECT_EQ(kCancelCoinsPerOfferCeiling, 2);
    EXPECT_LE(cancel_batch_cost_ceiling(kCancelOffersSingleBatchSize, 2),
              kMempoolMaxTxClvmCost);
    // The default spend count is the ceiling the batch is sized at.
    EXPECT_EQ(cancel_batch_cost_ceiling(kCancelOffersSingleBatchSize),
              cancel_batch_cost_ceiling(kCancelOffersSingleBatchSize,
                                        kCancelCoinsPerOfferCeiling));

    // A batch of 100 -- this change's first size -- costs up to 7,018,000,000
    // at two spends per offer: over the bound.
    EXPECT_GT(cancel_batch_cost_ceiling(100, 2), kMempoolMaxTxClvmCost);

    // A first draft of this change proposed 1000 per batch.  At these
    // ceilings that bundle is 6.4x over the bound even at one spend per
    // offer: a sweep that size would cancel nothing.
    EXPECT_GT(cancel_batch_cost_ceiling(1000, 1), kMempoolMaxTxClvmCost);

    // A degenerate size or spend count costs the fee spend alone, and a spend
    // count too large for the type saturates instead of wrapping.
    EXPECT_EQ(cancel_batch_cost_ceiling(0), kCancelFeeSpendCostCeiling);
    EXPECT_EQ(cancel_batch_cost_ceiling(-5), kCancelFeeSpendCostCeiling);
    EXPECT_EQ(cancel_batch_cost_ceiling(50, 0), kCancelFeeSpendCostCeiling);
    EXPECT_EQ(cancel_batch_cost_ceiling(50, -1), kCancelFeeSpendCostCeiling);
    EXPECT_EQ(cancel_batch_cost_ceiling(std::numeric_limits<int>::max(),
                                        std::numeric_limits<int>::max()),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(WalletRequests, CancelOffersCarriesSecureBothWays) {
    EXPECT_TRUE(make_cancel_offers_request(0, true).value("secure", false));
    EXPECT_FALSE(make_cancel_offers_request(0, false).value("secure", true));
}

// ---------------------------------------------------------------------------
// The fee reservation arithmetic
// ---------------------------------------------------------------------------

TEST(WalletRequests, BatchCountIsACeilingNotOne) {
    // batch_fee is charged PER BATCH.  Reserving one fee for the whole call
    // under-reserves XCH by a factor of ceil(n / batch_size) -- the
    // 2026-08-23 zero-spendable shape.
    EXPECT_EQ(cancel_offers_batch_count(0, 5), 0);
    EXPECT_EQ(cancel_offers_batch_count(1, 5), 1);
    EXPECT_EQ(cancel_offers_batch_count(5, 5), 1);
    EXPECT_EQ(cancel_offers_batch_count(6, 5), 2);
    EXPECT_EQ(cancel_offers_batch_count(20, 5), 4);
    EXPECT_EQ(cancel_offers_batch_count(21, 5), 5);
}

TEST(WalletRequests, BatchCountSurvivesADegenerateBatchSize) {
    // Fails toward over-reserving: one batch per offer.
    EXPECT_EQ(cancel_offers_batch_count(7, 0), 7);
    EXPECT_EQ(cancel_offers_batch_count(7, -1), 7);
    EXPECT_EQ(cancel_offers_batch_count(-3, 5), 0);
}

// [review 2026-09-12] The ceiling must not be computed as
// (n_offers + bs - 1) / bs: that addition is signed overflow -- UB -- once
// n_offers comes within bs of INT64_MAX.  Unreachable from today's only
// caller (reserve_bulk_cancel passes max(book size, 25)), but a WRAPPED
// count goes negative and reserve_bulk_cancel's `batches < 1` clamp would
// collapse it to a single batch -- relaundering it into the exact
// single-fee under-reservation this ceiling exists to prevent.
TEST(WalletRequests, BatchCountIsSafeAtTheInt64Boundary) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

    EXPECT_EQ(cancel_offers_batch_count(kMax, 1), kMax);
    EXPECT_EQ(cancel_offers_batch_count(kMax, 5), 1'844'674'407'370'955'162LL);
    EXPECT_EQ(cancel_offers_batch_count(kMax - 1, 5),
              1'844'674'407'370'955'162LL);
    EXPECT_GT(cancel_offers_batch_count(kMax, 5), 0);
}

// ---------------------------------------------------------------------------
// get_transactions -- the payload, then which 200 rows it selects
// ---------------------------------------------------------------------------

TEST(WalletRequests, GetTransactionsForwardsTheWindowItIsGiven) {
    // [S33 2026-09-11] The refactor moved these three keys out of an inline
    // JSON literal.  Nothing asserted they survived the move: with start and
    // end swapped, or a key dropped, BOTH consumers silently read the wrong
    // window and every other test here stays green.
    const json req = make_get_transactions_request(8, 0, 200);

    EXPECT_EQ(req.value("wallet_id", -1), 8);
    EXPECT_EQ(req.value("start", -1), 0);
    EXPECT_EQ(req.value("end", -1), 200);
    EXPECT_EQ(req.value("sort_key", std::string{}), "RELEVANCE");
    EXPECT_FALSE(req.value("reverse", true));

    // An asymmetric, non-zero window: with 0..200 alone a swapped start/end
    // or a wallet_id/start mix-up could pass by coincidence.
    const json other = make_get_transactions_request(1, 40, 90);
    EXPECT_EQ(other.value("wallet_id", -1), 1);
    EXPECT_EQ(other.value("start", -1), 40);
    EXPECT_EQ(other.value("end", -1), 90);
}

TEST(WalletRequests, TheTransactionWindowContainsUnconfirmedRows) {
    // OfferManager::prune_stuck_transactions skips every confirmed row, so a
    // window with no unconfirmed row in it makes stuck-cancel pruning dead
    // code on any wallet with 200+ confirmed transactions.
    const auto ordered = order_as_daemon(
        busy_wallet_rows(), make_get_transactions_request(1, 0, 200));

    ASSERT_GE(ordered.size(), kWindow);
    bool saw_unconfirmed = false;
    for (std::size_t i = 0; i < kWindow; ++i) {
        if (!ordered[i].confirmed) {
            saw_unconfirmed = true;
        }
    }
    EXPECT_TRUE(saw_unconfirmed)
        << "the pruner's 0..200 window contains no unconfirmed row, so it "
           "can never see a stuck transaction";
}

TEST(WalletRequests, ConfirmedRowsArriveNewestFirst) {
    // The reward scan's comment claims "Newest 200 transactions" and its
    // genesis filter drops anything at or below the opening block, so an
    // oldest-first window books nothing.
    const auto ordered = order_as_daemon(
        busy_wallet_rows(), make_get_transactions_request(8, 0, 200));

    ASSERT_GE(ordered.size(), kWindow);
    std::int64_t first_confirmed_height = -1;
    for (std::size_t i = 0; i < kWindow; ++i) {
        if (ordered[i].confirmed && first_confirmed_height < 0) {
            first_confirmed_height = ordered[i].confirmed_at_height;
        }
    }
    EXPECT_EQ(first_confirmed_height, 9'000'248LL)
        << "confirmed rows are not newest-first; the reward scan is reading "
           "the oldest 200 records of the wallet";
}

TEST(WalletRequests, UnconfirmedRowsSortAheadOfConfirmedOnes) {
    const auto ordered = order_as_daemon(
        busy_wallet_rows(), make_get_transactions_request(1, 0, 200));

    ASSERT_FALSE(ordered.empty());
    EXPECT_FALSE(ordered.front().confirmed);
}

// Characterisation, NOT a regression guard: this pins the OLD setting so that
// the tests above cannot pass merely because order_as_daemon ignores the
// `reverse` key.  It hardcodes reverse=true and so stays green either way.
TEST(WalletRequests, ReverseTrueIsWhatBuriedTheUnconfirmedRows) {
    json legacy = make_get_transactions_request(1, 0, 200);
    legacy["reverse"] = true;

    const auto ordered = order_as_daemon(busy_wallet_rows(), legacy);

    ASSERT_GE(ordered.size(), kWindow);
    // Confirmed first, oldest first -- and the unconfirmed row dead last,
    // far outside the 200-row window.
    EXPECT_TRUE(ordered.front().confirmed);
    EXPECT_EQ(ordered.front().confirmed_at_height, 9'000'000LL);
    EXPECT_FALSE(ordered.back().confirmed);
    for (std::size_t i = 0; i < kWindow; ++i) {
        EXPECT_TRUE(ordered[i].confirmed);
    }
}

// ---------------------------------------------------------------------------
// [S70 2026-09-20] get_timestamp_for_height -- the wallet's chain clock.
//
// The expired-offer retire frees an offer's coins with an INSECURE cancel on
// the strength of this one number, so both directions are pinned: the key the
// handler reads (chia 2.7.4 wallet_request_types.py GetTimestampForHeight has
// exactly one field, `height`), and a parser that reads anything other than
// the wallet's own uint64 as "no clock".  The response below is the live
// 2.7.4 wallet's, captured 2026-09-20.
// ---------------------------------------------------------------------------

TEST(WalletRequests, TimestampForHeightSendsTheHeightAndNothingElse) {
    const json p = make_get_timestamp_for_height_request(9'319'422);
    ASSERT_TRUE(p.is_object());
    EXPECT_EQ(p.size(), 1u);
    ASSERT_TRUE(p.contains("height"));
    EXPECT_EQ(p["height"].get<std::int64_t>(), 9'319'422);
}

TEST(WalletRequests, TimestampForHeightParsesTheLiveResponse) {
    // Parsed from TEXT, as rpc_post parses the wire: that is what makes a
    // non-negative integer number_unsigned.  A json built from an int
    // literal is number_integer and would (rightly) read as "no clock".
    const json live = json::parse(
        R"({"success": true, "timestamp": 1789916920})");
    ASSERT_TRUE(live["timestamp"].is_number_unsigned());
    EXPECT_EQ(parse_timestamp_for_height_response(live), 1'789'916'920ull);
}

TEST(WalletRequests, TimestampForHeightFailsClosedOnAnythingElse) {
    // 0 means "no chain clock", and nothing is retired on it.
    EXPECT_EQ(parse_timestamp_for_height_response(json::object()), 0u);
    EXPECT_EQ(parse_timestamp_for_height_response(json{{"success", true}}), 0u);
    EXPECT_EQ(parse_timestamp_for_height_response(
                  json{{"timestamp", nullptr}}), 0u);
    EXPECT_EQ(parse_timestamp_for_height_response(
                  json{{"timestamp", "1789916920"}}), 0u);
    EXPECT_EQ(parse_timestamp_for_height_response(
                  json{{"timestamp", 1789916920.0}}), 0u);
    EXPECT_EQ(parse_timestamp_for_height_response(
                  json{{"timestamp", -1}}), 0u);
    EXPECT_EQ(parse_timestamp_for_height_response(json::array()), 0u);
    EXPECT_EQ(parse_timestamp_for_height_response(json(nullptr)), 0u);
    // get_height_info's field of a similar name is NOT this one: it follows
    // the header peak, not the height the wallet has finished syncing to.
    EXPECT_EQ(parse_timestamp_for_height_response(json::parse(
                  R"({"latest_timestamp": 1789916920})")), 0u);
}

// ---------------------------------------------------------------------------
// [review #164 2026-09-21] get_connections -- WHOSE timestamp it was.
//
// The timestamp above is one connected peer's unvalidated assertion, so the
// retire pass gates on the peer SET.  This parser is the only thing standing
// between that gate and a response it did not understand, so every way of not
// understanding one is pinned here, and each must read as "do not act".
// ---------------------------------------------------------------------------

namespace {

/// A get_connections row in the live 2.7.4 shape (captured read-only from the
/// running wallet on 2026-09-21; ids shortened).
json connection_row(int type, const json& peer_host) {
    return json{
        {"bytes_read",       350754209},
        {"bytes_written",    9855265},
        {"creation_time",    1789574414.6896603},
        {"last_message_time", 1790002944.2846308},
        {"local_port",       0},
        {"node_id",          "0xec9efefa2d8566141bf6eb47cdc6bed05fcb700f"},
        {"peer_host",        peer_host},
        {"peer_port",        8444},
        {"peer_server_port", 8444},
        {"type",             type}
    };
}

json connections(std::initializer_list<json> rows) {
    json arr = json::array();
    for (const auto& row : rows) {
        arr.push_back(row);
    }
    return json{{"success", true}, {"connections", std::move(arr)}};
}

/// A localhost full-node row with one field replaced, for the fail-closed
/// cases below.
json row_with(const char* key, const json& value) {
    json row = connection_row(kNodeTypeFullNode, "127.0.0.1");
    row[key] = value;
    return row;
}

}  // namespace

TEST(WalletRequests, ConnectionsRequestAsksForFullNodesOnly) {
    const json p = make_get_connections_request();
    ASSERT_TRUE(p.is_object());
    EXPECT_EQ(p.size(), 1u);
    ASSERT_TRUE(p.contains("node_type"));
    // chia/server/outbound_message.py NodeType.FULL_NODE.
    EXPECT_EQ(p["node_type"].get<int>(), 1);
    EXPECT_EQ(kNodeTypeFullNode, 1);
}

TEST(WalletRequests, TheLocalhostSetIsChiasOwn) {
    // chia/util/network.py:140-141, verbatim -- the set for which
    // is_trusted_peer returns True with no further test.
    EXPECT_TRUE(is_localhost_peer_host("127.0.0.1"));
    EXPECT_TRUE(is_localhost_peer_host("localhost"));
    EXPECT_TRUE(is_localhost_peer_host("::1"));
    EXPECT_TRUE(is_localhost_peer_host("0:0:0:0:0:0:0:1"));
    // Everything else, including things that merely look local.  A prefix
    // test would admit the whole 127/8 range plus any host whose name starts
    // with "localhost"; chia's rule is exact membership and so is this.
    EXPECT_FALSE(is_localhost_peer_host("127.0.0.2"));
    EXPECT_FALSE(is_localhost_peer_host("127.0.0.1:8444"));
    EXPECT_FALSE(is_localhost_peer_host("localhost.attacker.example"));
    EXPECT_FALSE(is_localhost_peer_host("192.168.1.10"));
    EXPECT_FALSE(is_localhost_peer_host("::2"));
    EXPECT_FALSE(is_localhost_peer_host(""));
}

TEST(WalletRequests, CensusReadsTheLiveOnePeerResponse) {
    // Parsed from TEXT, as rpc_post parses the wire.  This is the live
    // wallet's answer on 2026-09-21: exactly one full node, on this host.
    const json live = json::parse(R"({
        "connections": [{
            "bytes_read": 350754209, "bytes_written": 9855265,
            "creation_time": 1789574414.6896603,
            "last_message_time": 1790002944.2846308, "local_port": 0,
            "node_id": "0xec9efefa2d8566141bf6eb47cdc6bed05fcb700f",
            "peer_host": "127.0.0.1", "peer_port": 8444,
            "peer_server_port": 8444, "type": 1
        }],
        "success": true
    })");
    const auto c = census_full_node_peers(live);
    EXPECT_TRUE(c.readable);
    EXPECT_EQ(c.full_node_peers, 1);
    EXPECT_EQ(c.non_local_peers, 0);
    EXPECT_TRUE(c.first_non_local_host.empty());
}

TEST(WalletRequests, CensusCountsEveryNonLocalFullNodeAndNamesTheFirst) {
    const auto c = census_full_node_peers(connections({
        connection_row(kNodeTypeFullNode, "127.0.0.1"),
        connection_row(kNodeTypeFullNode, "203.0.113.7"),
        connection_row(kNodeTypeFullNode, "198.51.100.9")
    }));
    EXPECT_TRUE(c.readable);
    EXPECT_EQ(c.full_node_peers, 3);
    EXPECT_EQ(c.non_local_peers, 2);
    // The operator has to be able to see WHICH stranger, not just that there
    // was one.
    EXPECT_EQ(c.first_non_local_host, "203.0.113.7");
}

TEST(WalletRequests, CensusIgnoresConnectionsThatAreNotFullNodes) {
    // NodeType 2 is HARVESTER, 3 FARMER, 6 WALLET.  None of them can answer
    // get_timestamp_for_height, so none of them may fail the gate either --
    // otherwise a daemon that ignored node_type would wedge the retire.
    const auto c = census_full_node_peers(connections({
        connection_row(6, "10.0.0.5"),
        connection_row(3, "10.0.0.6"),
        connection_row(kNodeTypeFullNode, "127.0.0.1")
    }));
    EXPECT_TRUE(c.readable);
    EXPECT_EQ(c.full_node_peers, 1);
    EXPECT_EQ(c.non_local_peers, 0);
}

TEST(WalletRequests, CensusReadsAnEmptyPeerListAsReadableAndEmpty) {
    // Readable, but with nothing that could have answered: a DIFFERENT state
    // from "we could not tell", and the caller distinguishes them.
    const auto c = census_full_node_peers(connections({}));
    EXPECT_TRUE(c.readable);
    EXPECT_EQ(c.full_node_peers, 0);
    EXPECT_EQ(c.non_local_peers, 0);
}

TEST(WalletRequests, CensusFailsClosedOnAnythingItCannotAccountFor) {
    for (const json& bad : {json::object(),
                            json{{"success", true}},
                            json{{"connections", nullptr}},
                            json{{"connections", "127.0.0.1"}},
                            json{{"connections", json::object()}},
                            json::array(),
                            json(nullptr)}) {
        const auto c = census_full_node_peers(bad);
        EXPECT_FALSE(c.readable) << bad.dump();
        EXPECT_EQ(c.full_node_peers, 0) << bad.dump();
    }
}

TEST(WalletRequests, OneUnclassifiableRowPoisonsTheWholeCensus) {
    // The row we cannot type may BE the full node about to answer the clock,
    // so a census that quietly skipped it would report "all local" on a peer
    // set it never read.  Each of these leaves readable == false even though
    // a genuine localhost full node is sitting right beside it.
    for (const json& bad_row : {json("not an object"),
                                json::object(),
                                row_with("type", json(nullptr)),
                                row_with("type", json("1")),
                                row_with("type", json(1.0))}) {
        const auto c = census_full_node_peers(connections({
            connection_row(kNodeTypeFullNode, "127.0.0.1"), bad_row}));
        EXPECT_FALSE(c.readable) << bad_row.dump();
        EXPECT_EQ(c.full_node_peers, 0) << bad_row.dump();
    }
}

TEST(WalletRequests, AFullNodeWithNoUsableHostCountsAsAStranger) {
    // "We could not tell where this peer is" is not evidence that it is here.
    for (const json& host : {json(nullptr), json(12345), json::object()}) {
        const auto c = census_full_node_peers(connections({
            connection_row(kNodeTypeFullNode, host)}));
        EXPECT_TRUE(c.readable) << host.dump();
        EXPECT_EQ(c.full_node_peers, 1) << host.dump();
        EXPECT_EQ(c.non_local_peers, 1) << host.dump();
        EXPECT_EQ(c.first_non_local_host, "<no peer_host>") << host.dump();
    }
    // A row missing the key entirely reads the same way.
    json row = connection_row(kNodeTypeFullNode, "127.0.0.1");
    row.erase("peer_host");
    const auto c = census_full_node_peers(connections({row}));
    EXPECT_TRUE(c.readable);
    EXPECT_EQ(c.non_local_peers, 1);
}
