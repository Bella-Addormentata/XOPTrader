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

#include <nlohmann/json.hpp>

namespace xop::rpc {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// cancel_offers
// ---------------------------------------------------------------------------

/// Offers cancelled per on-chain batch by the daemon's paging loop.
///
/// [BULKCANCEL 2026-09-11] Deliberately pinned to the handler's own default
/// of 5 rather than raised.  The paging loop runs INSIDE the daemon, so this
/// value costs us no client round trips -- it buys only fewer on-chain
/// transactions, at 0.00001 XCH each, against a book that is bounded by the
/// ladder (tens of offers, so single-digit batches).  Raising it would bundle
/// more cancel spends into one transaction, and a bundle rejected for cost
/// takes every cancel in it down together -- trading a trivial fee saving for
/// a new way to fail a shutdown path.  It is named and explicit so that the
/// fee reservation in OfferManager::cancel_offers_charged cannot silently
/// disagree with what the daemon actually does.
inline constexpr int kCancelOffersBatchSize = 5;

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
    return (n_offers + bs - 1) / bs;
}

/// Build the cancel_offers payload.
///
/// @param batch_fee  Fee in mojos charged PER BATCH (the handler's
///                   `batch_fee`; a plain "fee" key is ignored by it).
/// @param secure     On-chain cancel (spends the offer coins) vs local-only.
/// @param batch_size Offers per batch; see kCancelOffersBatchSize.
[[nodiscard]] inline json make_cancel_offers_request(
    std::uint64_t batch_fee,
    bool          secure,
    int           batch_size = kCancelOffersBatchSize)
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

}  // namespace xop::rpc

#endif  // XOP_RPC_WALLET_REQUESTS_HPP
