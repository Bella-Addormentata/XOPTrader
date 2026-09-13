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
/// THE ONE-SPEND ASSUMPTION.  Cost is paid per cancellation SPEND, not per
/// offer: trade_manager.py cancel_pending_offers spends every coin that
/// Offer.get_cancellation_coins() returns.  Those can be fewer than the coins
/// the offer itself spent.  offer.py drops each coin that asserts an
/// announcement another coin in the offer makes (and whatever depends on it),
/// because spending that provider already invalidates it -- so an offer's
/// root coin count bounds its cancellation spends only from above.  The 22
/// offers live on 2026-09-13 spent 2 to 7 root coins each; their cancellation
/// spends were not counted.
///
/// At ONE spend per offer and the ceilings below, a full batch of 100 costs
/// at most 3,518,000,000 -- 64% of the bound -- where 1,000 would be 6.4x over
/// it and cancel nothing.  Only that shape is covered: 100 offers of two CAT
/// cancellation spends each would cost up to 7,018,000,000, over the bound.
/// Were every root coin of the 22 live offers to need its own spend at the
/// measured maxima, they would cost up to 1,934,824,884 (35% of the bound),
/// about 88,000,000 each, and one bundle of such offers would overflow past
/// about 62.
///
/// ABOVE 100 OFFERS the daemon splits the sweep again, and the fee-coin
/// collision returns for the later batches.  That is far outside what this
/// deployment rests (the live wallet held 22 open offers of its own on
/// 2026-09-13; OfferManager::cancel_all reserves for 25), and it is the
/// better of the two failures: one bundle too large for the mempool cancels
/// nothing at all.
inline constexpr int kCancelOffersSingleBatchSize = 100;

/// chia 2.7.4 mempool_manager.py: max_tx_clvm_cost = MAX_BLOCK_COST_CLVM // 2.
/// A spend bundle costing more is rejected with Err.BLOCK_COST_EXCEEDS_MAX.
inline constexpr std::uint64_t kMempoolMaxTxClvmCost = 11'000'000'000ULL / 2;

/// Ceiling on ONE cancellation SPEND -- one coin, not one offer.  Measured
/// maximum 34,442,386 (a CAT offer coin; XCH offer coins at most 17,788,814),
/// rounded up.  An observation of this wallet's offers, not a consensus value.
/// An offer whose cancellation needs k spends costs up to k times this; see
/// THE ONE-SPEND ASSUMPTION above.
inline constexpr std::uint64_t kCancelSpendCostCeiling = 35'000'000ULL;

/// Ceiling on the XCH fee spend a batch adds when its fee cannot come out of
/// its first cancellation coin.  Measured maximum 8,817,766; set to cover
/// the largest standard XCH spend seen in any cancellation (17,788,814).
inline constexpr std::uint64_t kCancelFeeSpendCostCeiling = 18'000'000ULL;

/// Upper estimate of the mempool cost of one cancel_offers batch of
/// `batch_size` offers that each cancel with ONE coin spend, plus the batch's
/// fee spend.  It models that shape only and says nothing about an offer that
/// needs more spends (THE ONE-SPEND ASSUMPTION above).  A degenerate size
/// costs the fee spend alone.
[[nodiscard]] constexpr std::uint64_t cancel_batch_cost_ceiling(
    int batch_size) noexcept
{
    const std::uint64_t offers = batch_size > 0
        ? static_cast<std::uint64_t>(batch_size)
        : std::uint64_t{0};
    return offers * kCancelSpendCostCeiling + kCancelFeeSpendCostCeiling;
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

}  // namespace xop::rpc

#endif  // XOP_RPC_WALLET_REQUESTS_HPP
