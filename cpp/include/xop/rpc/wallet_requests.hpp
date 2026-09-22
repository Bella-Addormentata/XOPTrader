// ---------------------------------------------------------------------------
// wallet_requests.hpp -- pure request construction for the two Chia wallet
// RPCs whose payloads have to match the daemon's handler EXACTLY.
//
// [BULKCANCEL 2026-09-11] Split out of chia_rpc.cpp so the wire format is
// reachable from ctest.  Both endpoints below were wrong on main in ways no
// test could see, because the only evidence of the defect is the KEY NAMES in
// a JSON object that nothing but the daemon ever read:
//
//   cancel_offers   sent {"fee", "secure"}.  The handler reads request
//                   .batch_fee, so "fee" was silently ignored and every bulk
//                   cancel went out at ZERO fee.  cancel_all defaults to
//                   false, and the handler's filter then keeps only records
//                   whose Offer.arbitrage() dict contains query_key -- which
//                   is None, and None keys XCH -- so only offers with an XCH
//                   leg were ever cancelled and a CAT/CAT offer never was.
//                   Worse, the handler's paging loop breaks on the first
//                   batch containing no MATCHING offer, so the scan stopped
//                   early rather than running the book out.
//
//   get_transactions sent sort_key RELEVANCE with reverse=true, which is the
//                   exact opposite of what both of its consumers need.  The
//                   daemon's ORDER BY, and the 200-row WINDOW each setting
//                   selects out of a 33,000-row table, are modelled in
//                   tests/test_wallet_requests.cpp -- test-only code, so it
//                   is not carried by this production header.
//
// Verified against the Chia 2.7.4 wallet handler source (identical in 2.7.3).
//
// Compliant with:
//   ISO/IEC 5055  -- pure functions, no I/O, no UB
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
//                    (tests/test_wallet_requests.cpp)
// ---------------------------------------------------------------------------

#ifndef XOP_RPC_WALLET_REQUESTS_HPP
#define XOP_RPC_WALLET_REQUESTS_HPP

#include <cstdint>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace xop::rpc {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// cancel_offers
// ---------------------------------------------------------------------------

/// Offers the daemon cancels per batch -- sized so that a wallet-wide sweep
/// of any book this deployment rests goes out as ONE batch.
///
/// [BULKCANCEL-B 2026-09-13] Raised from the handler's own default of 5, which
/// the 2026-09-11 revision of this comment pinned on purpose: a sweep split
/// into batches cannot be funded as a whole.  From the chia 2.7.4 source:
///
///   * wallet_rpc_api.py cancel_offers pages the book batch_size trades at a
///     time and calls TradeManager.cancel_pending_offers once PER BATCH, with
///     every batch inside the one action scope its tx_endpoint wrapper opened.
///   * trade_manager.py cancel_pending_offers charges the fee on the first
///     cancellation coin of each call, so batch_fee is paid once per batch.
///     When that coin is a CAT coin, CATWallet.create_tandem_xch_tx selects
///     the XCH fee coin inside scopes nested in the one cancel_pending_offers
///     opens for that coin with excluded_coin_ids=[].  The choice is recorded
///     only in those nested scopes, never in the scope the batches share, so
///     no other batch sees it -- and nothing reaches the wallet DB, where
///     unconfirmed removals would exclude the coin, until the shared scope
///     closes after the LAST batch.
///     coin_selection.py is deterministic for identical inputs (coins sorted
///     by amount; the knapsack's random.Random has a fixed seed), so those
///     batches all pick the SAME fee coin.
///   * wallet_rpc_metadata.py registers cancel_offers with
///     auto_merge_spends=False, so each batch is pushed as its OWN spend
///     bundle.  mempool_manager.py can_replace admits a conflicting bundle
///     only if it spends a superset of the other's coins, so of the bundles
///     sharing a fee coin at most one can land -- after cancel_pending_offers
///     has already set every trade in every batch PENDING_CANCEL.
///
/// One batch pays batch_fee once, selects at most one fee coin, and ties all
/// of its cancellations into a single bundle through their announcement ring.
///
/// THE RISK THE OLD VALUE GUARDED.  A bundle rejected for cost takes every
/// cancel in it down together, and the bound is real: mempool_manager.py
/// rejects cost > max_tx_clvm_cost (MAX_BLOCK_COST_CLVM // 2) with
/// BLOCK_COST_EXCEEDS_MAX.  So the batch is sized against MEASURED costs, not
/// raised without limit.  Across the 15,330 confirmed cancellation bundles in
/// the live wallet, costed on 2026-09-13 with chia_rs 0.47 at the 2.7.4 cost
/// constants: 18,185 of 18,187 offers cancelled with ONE coin spend, and the
/// other two with two; a CAT offer coin's cancel spend cost at most
/// 34,442,386, an XCH one 17,788,814, and a fee spend 8,817,766; and a bundle
/// cost the sum of its spends to within 504,000.  The largest bundle this
/// wallet ever pushed carried 15 offers at 388,803,130.
///
/// THE TWO-SPEND BASIS.  [review 2026-09-13, operator decision] Cost is paid
/// per cancellation SPEND, not per offer: trade_manager.py
/// cancel_pending_offers spends every coin that Offer.get_cancellation_coins()
/// returns.  Those can be fewer than the coins the offer itself spent.
/// offer.py drops each coin that asserts an announcement another coin in the
/// offer makes (and whatever depends on it), because spending that provider
/// already invalidates it -- so an offer's root coin count bounds its
/// cancellation spends only from above.  The wallet builds the offers, so no
/// per-offer spend count can be enforced from this side.  The batch is
/// therefore costed at kCancelCoinsPerOfferCeiling (2) spends per offer,
/// twice the shape of 18,185 of the 18,187 offers this wallet has cancelled.
///
/// At two spends per offer and the ceilings below, a full batch of 50 costs
/// at most 50 x 2 x 35,000,000 + 18,000,000 = 3,518,000,000 -- 64% of the
/// bound.  A 50-offer bundle fits up to 156 cancellation spends in total.  The
/// 22 offers live on 2026-09-13 cost 1,558,000,000 (28%) at two spends each,
/// and at most 1,934,824,884 (35%) even if every one of their root coins (2 to
/// 7 per offer) needed its own spend at the measured maxima.  A batch of 100
/// would cost 7,018,000,000 at two spends per offer, over the bound, and 1,000
/// is 6.4x over it even at one spend per offer and would cancel nothing.
///
/// RE-MEASURE when the number of cancellation coins per offer changes -- a new
/// offer shape, more coins behind a leg -- not when the book merely grows.
///
/// ABOVE 50 OFFERS the daemon splits the sweep again, and the fee-coin
/// collision returns for the later batches.  That is outside what this
/// deployment rests (the live wallet held 22 open offers of its own on
/// 2026-09-13; OfferManager::cancel_all reserves for 25), and it is the
/// better of the two failures: one bundle too large for the mempool cancels
/// nothing at all.
inline constexpr int kCancelOffersSingleBatchSize = 50;

/// chia 2.7.4 mempool_manager.py: max_tx_clvm_cost = MAX_BLOCK_COST_CLVM // 2.
/// A spend bundle costing more is rejected with Err.BLOCK_COST_EXCEEDS_MAX.
inline constexpr std::uint64_t kMempoolMaxTxClvmCost = 11'000'000'000ULL / 2;

/// Ceiling on ONE cancellation SPEND -- one coin, not one offer.  Measured
/// maximum 34,442,386 (a CAT offer coin; XCH offer coins at most 17,788,814),
/// rounded up.  An observation of this wallet's offers, not a consensus value.
/// An offer whose cancellation needs k spends costs up to k times this; see
/// THE TWO-SPEND BASIS above.
inline constexpr std::uint64_t kCancelSpendCostCeiling = 35'000'000ULL;

/// Ceiling on the XCH fee spend a batch adds when its fee cannot come out of
/// its first cancellation coin.  Measured maximum 8,817,766; set to cover
/// the largest standard XCH spend seen in any cancellation (17,788,814).
inline constexpr std::uint64_t kCancelFeeSpendCostCeiling = 18'000'000ULL;

/// Cancellation spends per offer that kCancelOffersSingleBatchSize is costed
/// at.  [review 2026-09-13, operator decision] A stated ceiling, neither
/// measured nor enforced, resting on this evidence:
///   * 18,185 of the 18,187 offers this wallet has cancelled did so with ONE
///     coin spend, and the other two with two;
///   * chia 2.7.4 Offer.get_cancellation_coins() drops every coin that asserts
///     an announcement another coin in the offer makes, so the root coins an
///     offer spends bound its cancellation spends only from above.
/// See THE TWO-SPEND BASIS above.
inline constexpr int kCancelCoinsPerOfferCeiling = 2;

/// Upper estimate of the mempool cost of one cancel_offers batch of
/// `batch_size` offers that each cancel with at most `cancel_coins_per_offer`
/// coin spends, plus the batch's fee spend.  A degenerate size or spend count
/// costs the fee spend alone, and a spend count too large for the type
/// saturates at the maximum instead of wrapping.
[[nodiscard]] constexpr std::uint64_t cancel_batch_cost_ceiling(
    int batch_size,
    int cancel_coins_per_offer = kCancelCoinsPerOfferCeiling) noexcept
{
    if (batch_size <= 0 || cancel_coins_per_offer <= 0) {
        return kCancelFeeSpendCostCeiling;
    }
    // Each factor is below 2^31, so the spend count is below 2^62 and fits.
    const std::uint64_t spends =
        static_cast<std::uint64_t>(batch_size)
        * static_cast<std::uint64_t>(cancel_coins_per_offer);
    const std::uint64_t max_spends =
        (std::numeric_limits<std::uint64_t>::max()
         - kCancelFeeSpendCostCeiling)
        / kCancelSpendCostCeiling;
    if (spends > max_spends) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return spends * kCancelSpendCostCeiling + kCancelFeeSpendCostCeiling;
}

/// The number of batches -- and therefore the number of times batch_fee is
/// CHARGED -- when the daemon cancels `n_offers` at `batch_size` per batch.
///
/// This is the arithmetic OfferManager::cancel_offers_charged must reserve
/// against: batch_fee is charged PER BATCH, not once per call.
[[nodiscard]] inline std::int64_t cancel_offers_batch_count(
    std::int64_t n_offers, int batch_size) noexcept
{
    if (n_offers <= 0) return 0;
    if (batch_size <= 0) return n_offers;   // degenerate; charge worst case
    const std::int64_t bs = static_cast<std::int64_t>(batch_size);
    // [review 2026-09-12] Subtraction form, NOT (n_offers + bs - 1) / bs:
    // that addition is signed overflow -- UB -- once n_offers comes within bs
    // of INT64_MAX.  Both guards above already established n_offers >= 1 and
    // bs >= 1, and for those operands floor((n - 1) / bs) + 1 == ceil(n / bs)
    // exactly, while n_offers - 1 cannot underflow and the quotient + 1 cannot
    // leave the type.  Unreachable from today's only caller (reserve_bulk_cancel
    // passes max(book size, 25)), but a wrapped count goes NEGATIVE and the
    // `batches < 1` clamp there would relaunder it into the single-fee
    // under-reservation this function exists to prevent.
    return (n_offers - 1) / bs + 1;
}

/// Build the cancel_offers payload.
///
/// @param batch_fee  Fee in mojos charged PER BATCH (the handler's
///                   `batch_fee`; a plain "fee" key is ignored by it).
/// @param secure     On-chain cancel (spends the offer coins) vs local-only.
/// @param batch_size Offers per batch; see kCancelOffersSingleBatchSize.
[[nodiscard]] inline json make_cancel_offers_request(
    std::uint64_t batch_fee,
    bool          secure,
    int           batch_size = kCancelOffersSingleBatchSize)
{
    return json{
        // cancel_all:true is what makes this a BULK cancel at all.  Without
        // it the handler's query_key is None, None keys XCH in
        // Offer.arbitrage(), and the filter silently drops every CAT/CAT
        // offer -- then stops paging at the first batch that matched none.
        {"cancel_all", true},
        {"batch_fee",  batch_fee},
        {"batch_size", batch_size},
        {"secure",     secure}
    };
}

// ---------------------------------------------------------------------------
// get_transactions
// ---------------------------------------------------------------------------

/// Build the get_transactions payload.
///
/// [BULKCANCEL 2026-09-11] reverse=FALSE, and the value is load-bearing for
/// both consumers: under RELEVANCE it selects UNCONFIRMED rows first and
/// then confirmed rows NEWEST first, where reverse=true buried both.  The
/// daemon's ORDER BY is modelled in tests/test_wallet_requests.cpp.
[[nodiscard]] inline json make_get_transactions_request(
    std::int64_t wallet_id, std::int64_t start, std::int64_t end)
{
    return json{
        {"wallet_id", wallet_id},
        {"start",     start},
        {"end",       end},
        {"sort_key",  "RELEVANCE"},
        {"reverse",   false}
    };
}

// ---------------------------------------------------------------------------
// get_timestamp_for_height -- A CONNECTED PEER'S ANSWER, NOT A LOCAL FACT
// ---------------------------------------------------------------------------
//
// [S70 2026-09-20] The one clock an offer's max_time is enforced against is a
// transaction block's timestamp, so the expired-offer retire reads that and
// never this host's clock (execution/offer_expiry.hpp has the argument).
//
// chia 2.7.4: wallet_request_types.py GetTimestampForHeight carries one field,
// `height: uint32`, and GetTimestampForHeightResponse one, `timestamp:
// uint64`.  wallet_node.py get_timestamp_for_height_from_peer returns "the
// timestamp for transaction block at h=height, if not transaction block,
// backtracks until it finds a recent transaction block" -- i.e. the latest
// transaction-block timestamp AT OR BEFORE that height, which is exactly the
// value consensus compares ASSERT_BEFORE_SECONDS_ABSOLUTE with for the next
// block.
//
// [review #164, 2026-09-21] WHAT THIS ENDPOINT DOES *NOT* GIVE YOU, AND AN
// EARLIER REVISION OF THIS COMMENT CLAIMED IT DID.  It said that, asked at
// get_height_info's height, "the answer also bounds what the wallet has SEEN,
// not merely what exists".  The HEIGHT bounds that.  The TIMESTAMP does not,
// and it is not the wallet's own reading of its own chain:
//
//   * wallet_rpc_api.py:934-935 forwards straight to
//     WalletNode.get_timestamp_for_height, which (wallet_node.py:1299-1304)
//     walks get_full_node_peers_in_order() and returns the FIRST non-None
//     answer any peer gives.
//   * It calls get_timestamp_for_height_from_peer with expected_header_hash
//     left at its None default (signature :1230-1232).  In that UNANCHORED
//     mode the only validation on a cache miss is len(response) == 1 and
//     block.height == request_height (:1258-1266); the two header-hash checks
//     (:1247-1251, :1282-1284) are dead code when expected_hash is None, and
//     request_header_blocks (wallet_sync_utils.py:258-272) validates nothing.
//     No signature, no proof of space, no VDF, no consensus check at all.
//   * The value returned is block.foliage_transaction_block.timestamp
//     (:1290-1291), and it is cached UNVALIDATED, per peer
//     (peer_request_cache.py:39-45).
//   * Chia HAS an anchored mode and uses it on the peak path (:1354-1356,
//     which passes new_peak_hb.header_hash).  This RPC path does not.
//
// So the number below is ONE CONNECTED PEER'S ASSERTION about a header block.
// A peer willing to lie can return any timestamp it likes, including one past
// an offer's max_time while the real chain still permits a take, and there is
// no upper bound to catch it: parse_timestamp_for_height_response rejects only
// a non-unsigned value, and execution::expired_at_depth only tests >=, so
// 2^64-1 is accepted.  The 32-block read depth does NOT help -- forging a
// timestamp at height H-32 costs a liar exactly what forging one at H costs.
//
// WHAT DOES HELP is knowing WHOSE answer it is, which is why the census below
// exists and why the retire pass refuses to act without it.

/// Build the get_timestamp_for_height payload.
[[nodiscard]] inline json make_get_timestamp_for_height_request(
    std::int64_t height)
{
    return json{{"height", height}};
}

/// The timestamp in a get_timestamp_for_height response, or 0 when the
/// response does not carry one.  Fails closed like expiry_echo_ok: uint64 is
/// the wallet's type, so a null, a float, a string or a negative all read as
/// "no chain clock" and nothing is retired on them.
[[nodiscard]] inline std::uint64_t parse_timestamp_for_height_response(
    const json& response)
{
    if (!response.is_object() || !response.contains("timestamp")
        || !response["timestamp"].is_number_unsigned()) {
        return 0;
    }
    return response["timestamp"].get<std::uint64_t>();
}

// ---------------------------------------------------------------------------
// get_connections -- WHOSE chain clock the retire would be acting on
// ---------------------------------------------------------------------------
//
// [review #164 2026-09-21] get_timestamp_for_height answers from whichever
// connected full node replies first, with no consensus validation (above).
// The only thing that makes that answer worth an irreversible local cancel is
// the identity of the peers that could have supplied it -- so the retire pass
// asks the wallet for its peer list and refuses to act unless EVERY full-node
// peer is on this host.
//
// Why "every" and not "the one that answered": the RPC does not report which
// peer answered, and get_full_node_peers_in_order() shuffles within buckets,
// so the only sound gate is over the whole candidate set.
//
// The route is the shared RPC server's get_connections
// (chia/rpc/rpc_server.py:286-294 -> default_get_connections :124-141, which
// the wallet reaches through WalletNode.get_connections, wallet_node.py:230).
// Each row carries `type` (NodeType), `peer_host`, `peer_port`, `node_id`.
// Verified read-only against the live 2.7.4 wallet on 2026-09-21: one
// connection, type 1, peer_host "127.0.0.1", peer_port 8444.
//
// THIS GATE IS DELIBERATELY NARROWER THAN CHIA'S OWN TRUST RULE.  chia/util/
// network.py:144-149 is_trusted_peer trusts a peer when is_localhost(host)
// OR its node_id is in the wallet config's `trusted_peers` OR its host is in
// a `trusted_cidrs` entry.  Only the first is checked here, because the other
// two live in the wallet's config file rather than in any RPC answer, and a
// safety gate that reads a second source of truth is the shape this repo keeps
// getting bitten by.  The cost is fail-CLOSED: an operator who trusts a remote
// node by node_id or CIDR gets no retires and keeps the hard TTL, which is a
// documented precondition of `ttl_cancel_mode: expire`, not a defect.

/// chia/server/outbound_message.py NodeType.FULL_NODE.
inline constexpr int kNodeTypeFullNode = 1;

/// chia/util/network.py is_localhost (2.7.4), reproduced exactly -- the set
/// that makes is_trusted_peer answer True unconditionally.
[[nodiscard]] inline bool is_localhost_peer_host(
    const std::string& host) noexcept
{
    return host == "127.0.0.1" || host == "localhost" || host == "::1"
        || host == "0:0:0:0:0:0:0:1";
}

/// What a get_connections answer says about the wallet's full-node peers.
///
/// @p readable is false for any response this parser cannot fully account
/// for -- that is the fail-closed reading, and it is NOT the same as "no
/// peers", which is why the two are separate fields rather than a count of 0.
struct FullNodePeerCensus {
    bool        readable{false};          ///< every row was understood
    int         full_node_peers{0};       ///< rows with type == FULL_NODE
    int         non_local_peers{0};       ///< of those, not on this host
    std::string first_non_local_host{};   ///< for the operator-facing log line
};

/// Census the FULL_NODE connections in a get_connections response.
///
/// Fails closed in one direction only: an unparseable response, or one row
/// whose `type` cannot be read, yields readable == false, and the caller then
/// retires nothing.  A full-node row with no usable `peer_host` counts as
/// NON-local, because "we could not tell where this peer is" is not evidence
/// that it is here.
[[nodiscard]] inline FullNodePeerCensus census_full_node_peers(
    const json& response)
{
    FullNodePeerCensus census{};
    if (!response.is_object() || !response.contains("connections")
        || !response["connections"].is_array()) {
        return census;   // readable stays false
    }
    for (const auto& row : response["connections"]) {
        if (!row.is_object() || !row.contains("type")
            || !row["type"].is_number_integer()) {
            // One row we cannot classify poisons the whole census: it may be
            // the full node we are about to trust a timestamp from.
            return FullNodePeerCensus{};
        }
        if (row["type"].get<std::int64_t>() != kNodeTypeFullNode) {
            continue;
        }
        ++census.full_node_peers;
        const bool has_host = row.contains("peer_host")
                           && row["peer_host"].is_string();
        const std::string host =
            has_host ? row["peer_host"].get<std::string>() : std::string{};
        if (has_host && is_localhost_peer_host(host)) {
            continue;
        }
        ++census.non_local_peers;
        if (census.first_non_local_host.empty()) {
            census.first_non_local_host = has_host ? host : "<no peer_host>";
        }
    }
    census.readable = true;
    return census;
}

/// Build the get_connections payload that asks for full-node peers only.
///
/// The census re-filters on `type` regardless, so a daemon that ignored this
/// key could not widen the gate -- it could only add rows the census then
/// skips.
[[nodiscard]] inline json make_get_connections_request()
{
    return json{{"node_type", kNodeTypeFullNode}};
}

}  // namespace xop::rpc

#endif  // XOP_RPC_WALLET_REQUESTS_HPP
