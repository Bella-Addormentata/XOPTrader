// on_chain_reconciler.hpp -- Full-node on-chain reconciliation for XOPTrader.
//
// Provides ground-truth verification of the bot's internal state against the
// Chia blockchain via the full node RPC.  The wallet RPC provides the primary
// offer lifecycle view, but it can lose state after restarts, bugs, or network
// partitions.  The full node is the authoritative source of truth for:
//
//   1. Coin existence and spent/unspent status.
//   2. Per-block additions (new coins) and removals (spent coins).
//   3. Actual on-chain balances (summing unspent coins by puzzle hash).
//
// This reconciler complements the wallet-based reconciliation in OfferManager
// by cross-checking against on-chain data.  It runs periodically alongside
// the existing wallet reconciliation in Engine Step 8.
//
// Reconciliation outputs:
//   - Balance discrepancies (on-chain vs wallet-reported) → logged + alerted.
//   - Stale offers whose backing coins were spent → removed from State.
//   - Per-block fee extraction from additions/removals → fed to FeeTracker.
//
// Thread safety: NOT thread-safe.  Called from the single-stranded engine
// heartbeat loop (same as all other async subsystems).
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no secrets logged; puzzle hashes are public data
//   ISO/IEC 5055       -- defensive null checks on all RPC responses
//   ISO/IEC 25000      -- single-responsibility, documented invariants

#ifndef XOP_MONITORING_ON_CHAIN_RECONCILER_HPP
#define XOP_MONITORING_ON_CHAIN_RECONCILER_HPP

#include <xop/rpc/chia_rpc.hpp>
#include <xop/state.hpp>
#include <xop/types.hpp>

#include <boost/asio/awaitable.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xop {

namespace asio = boost::asio;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// BalanceDiscrepancy -- result of comparing on-chain vs wallet balances.
// ---------------------------------------------------------------------------

struct BalanceDiscrepancy {
    std::string wallet_label;        ///< Human-readable label (e.g. "xch", asset_id).
    std::int64_t wallet_id{0};       ///< Wallet RPC wallet ID.
    Mojo wallet_confirmed{0};        ///< Balance reported by wallet RPC.
    Mojo on_chain_total{0};          ///< Sum of unspent coins from full node.
    Mojo difference{0};              ///< on_chain - wallet (positive = wallet under-reports).
    std::size_t on_chain_coin_count{0}; ///< Number of unspent coins found.
};

// ---------------------------------------------------------------------------
// BlockFeeInfo -- fee extraction from a single block's coin changes.
// ---------------------------------------------------------------------------

struct BlockFeeInfo {
    BlockHeight block_height{0};
    std::string header_hash;
    Mojo total_additions{0};         ///< Sum of all coins created in this block.
    Mojo total_removals{0};          ///< Sum of all coins spent in this block.
    Mojo implied_fees{0};            ///< removals - additions = fees absorbed by farmer.
    std::size_t our_additions{0};    ///< Coins created to our puzzle hashes.
    std::size_t our_removals{0};     ///< Coins spent from our puzzle hashes.
    Mojo our_net_change{0};          ///< Net mojos change for our puzzle hashes.
};

// ---------------------------------------------------------------------------
// OnChainReconciler
// ---------------------------------------------------------------------------

class OnChainReconciler {
public:
    /// Construct with references to the full node and wallet RPC clients,
    /// and the shared state.
    ///
    /// @param full_node  Full node RPC client (port 8555).
    /// @param wallet     Wallet RPC client (port 9256).
    /// @param state      Shared engine state (pending offers, positions).
    OnChainReconciler(
        std::shared_ptr<rpc::ChiaFullNodeRPC> full_node,
        std::shared_ptr<rpc::ChiaWalletRPC>   wallet,
        std::shared_ptr<State>                state);

    // -- Balance reconciliation ---------------------------------------------

    /// Compare wallet-reported balances against on-chain coin sums for each
    /// of the given wallet IDs.
    ///
    /// For each wallet, queries spendable coins from the wallet to collect
    /// puzzle hashes, then queries the full node for unspent coins at those
    /// puzzle hashes.  Logs discrepancies and returns them.
    ///
    /// @param wallet_ids  Map of label -> wallet_id to check.
    /// @return Vector of discrepancies (empty if everything matches).
    asio::awaitable<std::vector<BalanceDiscrepancy>> reconcile_balances(
        const std::unordered_map<std::string, std::int64_t>& wallet_ids);

    // -- Offer coin verification --------------------------------------------

    /// Verify that coins backing pending offers are still unspent on-chain.
    ///
    /// For each PendingOffer in State, looks up its trade record in the wallet
    /// to find the coin names, then batch-queries the full node to verify
    /// they are unspent.  Returns offer IDs whose coins have been spent
    /// (indicating the offer was filled or cancelled externally).
    ///
    /// @return Vector of offer IDs with spent backing coins.
    asio::awaitable<std::vector<std::string>> verify_pending_offer_coins();

    // -- Block fee extraction -----------------------------------------------

    /// Extract fee information from a range of blocks by examining additions
    /// and removals.  For each block, computes the fee as
    /// (sum_removals - sum_additions) and identifies our coins.
    ///
    /// @param start_height  First block to examine (inclusive).
    /// @param end_height    Last block to examine (inclusive).
    /// @param our_puzzle_hashes  Set of puzzle hashes owned by us.
    /// @return Vector of BlockFeeInfo, one per block in the range.
    asio::awaitable<std::vector<BlockFeeInfo>> extract_block_fees(
        BlockHeight start_height,
        BlockHeight end_height,
        const std::unordered_set<std::string>& our_puzzle_hashes);

    // -- Full reconciliation ------------------------------------------------

    /// Run a complete on-chain reconciliation cycle:
    ///   1. Reconcile balances for all given wallets.
    ///   2. Verify pending offer coins.
    ///   3. Extract fees from blocks since last reconciliation.
    ///
    /// @param wallet_ids         Map of label -> wallet_id.
    /// @param current_block      Current blockchain height.
    /// @param our_puzzle_hashes  Set of our puzzle hashes.
    /// @return Pair: (offer IDs with spent coins, balance discrepancies).
    asio::awaitable<std::pair<
        std::vector<std::string>,
        std::vector<BalanceDiscrepancy>
    >> run_full_reconciliation(
        const std::unordered_map<std::string, std::int64_t>& wallet_ids,
        BlockHeight current_block,
        const std::unordered_set<std::string>& our_puzzle_hashes);

    /// Block height of the last successful reconciliation.
    BlockHeight last_reconciled_block() const noexcept {
        return last_reconciled_block_;
    }

private:
    std::shared_ptr<rpc::ChiaFullNodeRPC> full_node_;
    std::shared_ptr<rpc::ChiaWalletRPC>   wallet_;
    std::shared_ptr<State>                state_;

    /// Block height of the last successful full reconciliation.
    BlockHeight last_reconciled_block_{0};

    // Consecutive reconcile cycles where a pending offer was not found in
    // wallet get_all_offers().  Used to avoid false stale detection when
    // wallet RPC lags or briefly omits new offers.
    std::unordered_map<std::string, std::uint32_t> not_found_counts_;

    /// Logger instance.
    std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace xop

#endif  // XOP_MONITORING_ON_CHAIN_RECONCILER_HPP
