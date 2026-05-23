// on_chain_reconciler.cpp -- Full-node on-chain reconciliation implementation.
//
// Verifies the bot's internal state against blockchain ground truth by
// querying the Chia full node RPC for coin records, block additions/removals,
// and cross-referencing with wallet-reported data.
//
// Error handling:
//   All RPC failures are caught and logged; partial results are returned
//   rather than aborting the entire reconciliation cycle.  The engine
//   decides how to handle discrepancies (alert, correct state, etc.).
//
// Performance:
//   - Coin lookups use batch queries (get_coin_records_by_names) to minimize
//     round-trips.
//   - Puzzle hash queries are deduplicated before calling the full node.
//   - Block fee extraction processes one block at a time to avoid memory
//     spikes on large ranges.

#include "xop/monitoring/on_chain_reconciler.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace xop {

namespace asio = boost::asio;

// ===========================================================================
// Construction
// ===========================================================================

OnChainReconciler::OnChainReconciler(
    std::shared_ptr<rpc::ChiaFullNodeRPC> full_node,
    std::shared_ptr<rpc::ChiaWalletRPC>   wallet,
    std::shared_ptr<State>                state)
    : full_node_(std::move(full_node))
    , wallet_(std::move(wallet))
    , state_(std::move(state))
    , logger_(spdlog::default_logger()->clone("OnChainReconciler"))
{}

// ===========================================================================
// Balance reconciliation
// ===========================================================================

asio::awaitable<std::vector<BalanceDiscrepancy>>
OnChainReconciler::reconcile_balances(
    const std::unordered_map<std::string, std::int64_t>& wallet_ids)
{
    std::vector<BalanceDiscrepancy> discrepancies;

    for (const auto& [label, wid] : wallet_ids) {
        try {
            // Step 1: Get wallet-reported balance.
            auto bal_json = co_await wallet_->get_wallet_balance(wid);
            Mojo wallet_confirmed = 0;
            if (bal_json.contains("confirmed_wallet_balance")) {
                wallet_confirmed =
                    bal_json["confirmed_wallet_balance"].get<Mojo>();
            }

            // Step 2: Get spendable coins from wallet to collect puzzle hashes.
            auto coins_json = co_await wallet_->get_spendable_coins(wid);

            // Collect unique puzzle hashes from our coins.
            std::unordered_set<std::string> puzzle_hashes;
            for (const auto& coin_rec : coins_json) {
                const auto& coin_obj = coin_rec.contains("coin")
                    ? coin_rec["coin"] : coin_rec;
                if (coin_obj.contains("puzzle_hash")) {
                    std::string ph =
                        coin_obj["puzzle_hash"].get<std::string>();
                    // Strip 0x prefix for consistency.
                    if (ph.size() > 2 && ph.substr(0, 2) == "0x") {
                        ph = ph.substr(2);
                    }
                    puzzle_hashes.insert(ph);
                }
            }

            if (puzzle_hashes.empty()) {
                logger_->debug("reconcile_balances: wallet {} ({}) has no "
                               "spendable coins -- skipping on-chain check",
                               label, wid);
                continue;
            }

            // Step 3: Query full node for unspent coins at each puzzle hash.
            Mojo on_chain_total = 0;
            std::size_t on_chain_count = 0;

            for (const auto& ph : puzzle_hashes) {
                try {
                    auto records =
                        co_await full_node_->get_coin_records_by_puzzle_hash(
                            ph, /*include_spent=*/false);

                    for (const auto& rec : records) {
                        if (rec.contains("coin") &&
                            rec["coin"].contains("amount")) {
                            on_chain_total +=
                                rec["coin"]["amount"].get<Mojo>();
                            ++on_chain_count;
                        }
                    }
                } catch (const std::exception& e) {
                    logger_->warn("reconcile_balances: full node query failed "
                                  "for puzzle_hash {}...{}: {}",
                                  ph.substr(0, 8),
                                  ph.substr(ph.size() > 8 ? ph.size() - 4 : 0),
                                  e.what());
                }
            }

            // Step 4: Compare.
            Mojo diff = on_chain_total - wallet_confirmed;

            if (diff != 0) {
                BalanceDiscrepancy disc;
                disc.wallet_label      = label;
                disc.wallet_id         = wid;
                disc.wallet_confirmed  = wallet_confirmed;
                disc.on_chain_total    = on_chain_total;
                disc.difference        = diff;
                disc.on_chain_coin_count = on_chain_count;
                discrepancies.push_back(disc);

                logger_->warn("reconcile_balances: DISCREPANCY wallet={} "
                              "wallet_confirmed={} on_chain={} diff={} "
                              "({} coins on-chain)",
                              label, wallet_confirmed, on_chain_total,
                              diff, on_chain_count);
            } else {
                logger_->info("reconcile_balances: wallet={} balance OK "
                              "({} mojos, {} coins)",
                              label, wallet_confirmed, on_chain_count);
            }
        } catch (const std::exception& e) {
            logger_->warn("reconcile_balances: failed for wallet {} ({}): {}",
                          label, wid, e.what());
        }
    }

    co_return discrepancies;
}

// ===========================================================================
// Offer coin verification
// ===========================================================================

asio::awaitable<std::vector<std::string>>
OnChainReconciler::verify_pending_offer_coins()
{
    std::vector<std::string> stale_offer_ids;
    std::unordered_set<std::string> current_pending_ids;

    auto pending = state_->get_all_offers();
    if (pending.empty()) {
        not_found_counts_.clear();
        co_return stale_offer_ids;
    }

    current_pending_ids.reserve(pending.size());
    for (const auto& po : pending) {
        current_pending_ids.insert(po.offer_id);
    }

    // For each pending offer, query its wallet trade record to find the
    // coins it references, then batch-verify them on-chain.
    //
    // Strategy: get all wallet offers and build a map of trade_id -> status.
    // Offers the wallet reports as PENDING_ACCEPT are assumed live.
    // Offers not found in the wallet at all have been lost -- flag them.

    // Paginate wallet offers to build a map of known trade IDs.
    std::unordered_map<std::string, int> wallet_offer_status;
    constexpr std::int64_t kPageSize = 50;
    std::int64_t offset = 0;
    bool more = true;
    bool wallet_query_succeeded = false;

    while (more) {
        try {
            auto records = co_await wallet_->get_all_offers(
                offset, offset + kPageSize, /*file_contents=*/false);

            wallet_query_succeeded = true;

            if (records.empty() ||
                static_cast<std::int64_t>(records.size()) < kPageSize) {
                more = false;
            }

            for (const auto& rec : records) {
                if (rec.contains("trade_id") && rec.contains("status")) {
                    // Chia wallet returns status as int (older) or
                    // string name (newer).  Map both to the canonical
                    // integer codes used downstream.
                    int status = -1;
                    if (rec["status"].is_number()) {
                        status = rec["status"].get<int>();
                    } else if (rec["status"].is_string()) {
                        const auto& s = rec["status"].get_ref<
                            const std::string&>();
                        if (s == "PENDING_ACCEPT")       status = 0;
                        else if (s == "PENDING_CONFIRM") status = 1;
                        else if (s == "PENDING_CANCEL")  status = 2;
                        else if (s == "CANCELLED")       status = 3;
                        else if (s == "CONFIRMED")       status = 4;
                        else if (s == "FAILED")          status = 5;
                        else {
                            // Try numeric string as last resort.
                            try {
                                status = std::stoi(s);
                            } catch (...) {
                                logger_->warn(
                                    "verify_pending_offer_coins: "
                                    "unparseable status '{}' for trade {}",
                                    s,
                                    rec["trade_id"].get<std::string>()
                                        .substr(0, 12));
                                continue;
                            }
                        }
                    } else {
                        continue;
                    }
                    wallet_offer_status[rec["trade_id"].get<std::string>()] =
                        status;
                }
            }

            offset += kPageSize;
        } catch (const std::exception& e) {
            logger_->warn("verify_pending_offer_coins: wallet query failed "
                          "at offset {}: {}", offset, e.what());
            more = false;
        }
    }

    // If we couldn't retrieve any wallet offers at all, do not proceed
    // with cross-referencing -- we'd falsely mark everything as stale.
    if (!wallet_query_succeeded) {
        logger_->warn("verify_pending_offer_coins: wallet query failed "
                      "completely -- skipping stale detection to avoid "
                      "false cancellations ({} pending offers preserved)",
                      pending.size());
        co_return stale_offer_ids;
    }

    // Cross-reference pending offers against wallet state and on-chain data.
    const auto now = std::chrono::system_clock::now();
    for (const auto& po : pending) {
        auto it = wallet_offer_status.find(po.offer_id);

        if (it == wallet_offer_status.end()) {
            // Grace period: skip offers created less than 120 seconds ago.
            // The Chia wallet may not immediately surface newly-created
            // offers in get_all_offers, especially during sync.  Without
            // this, verify_pending_offer_coins falsely marks 15-second-old
            // offers as NOT FOUND, causing the engine to lose track of them
            // and re-create duplicates that drain XCH to zero.
            constexpr auto kCreationGracePeriod = std::chrono::seconds{600};
            constexpr std::uint32_t kRequiredConsecutiveMisses = 3;
            const auto age = now - po.created_at_ts;
            auto& miss_count = not_found_counts_[po.offer_id];
            ++miss_count;

            if (age < kCreationGracePeriod) {
                logger_->info("verify_pending_offer_coins: offer {} NOT FOUND "
                              "in wallet but only {:.0f}s old -- skipping "
                              "(grace period {}s, pair={} tier={})",
                              po.offer_id.substr(0, 12),
                              std::chrono::duration<double>(age).count(),
                              kCreationGracePeriod.count(),
                              po.pair_name, po.tier);
                continue;
            }

            // Require multiple consecutive misses before declaring stale.
            // Wallet get_all_offers can transiently omit entries during
            // sync/refresh windows; a single miss is not authoritative.
            if (miss_count < kRequiredConsecutiveMisses) {
                logger_->info("verify_pending_offer_coins: offer {} NOT FOUND "
                              "(miss {}/{}) -- preserving for now "
                              "(pair={} tier={}, age={:.0f}s)",
                              po.offer_id.substr(0, 12),
                              miss_count, kRequiredConsecutiveMisses,
                              po.pair_name, po.tier,
                              std::chrono::duration<double>(age).count());
                continue;
            }

            // Offer not found in wallet at all -- wallet lost track of it.
            // This is a strong signal that the offer was resolved externally
            // or the wallet state was reset.
            logger_->warn("verify_pending_offer_coins: offer {} NOT FOUND "
                          "in wallet -- marking as stale (pair={} tier={})",
                          po.offer_id.substr(0, 12), po.pair_name, po.tier);
            stale_offer_ids.push_back(po.offer_id);
            continue;
        }

        // Offer present in wallet: clear any prior NOT FOUND misses.
        not_found_counts_.erase(po.offer_id);

        // Status codes from Chia:
        //   0 = PENDING_ACCEPT (our offer is out, waiting for a taker)
        //   1 = PENDING_CONFIRM (matched, awaiting on-chain confirmation)
        //   2 = PENDING_CANCEL (cancellation submitted, not confirmed)
        //   3 = CANCELLED (confirmed cancelled)
        //   4 = CONFIRMED (filled and settled)
        //   5 = FAILED
        int status = it->second;

        if (status >= 3) {
            // Terminal state (cancelled, confirmed/filled, or failed) that
            // our normal fill detection or reconciliation missed.
            //
            // IMPORTANT: status 4 = CONFIRMED (filled).  Do NOT mark filled
            // offers as stale here -- engine.cpp records stale_offer_ids
            // with cancel_reason="on_chain_reconcile", which would mislabel
            // a fill as a cancellation in the DB (corrupts fill-rate
            // analytics).  detect_fills() runs on the next poll cycle with
            // include_completed=true and will correctly attribute the fill.
            if (status == 4) {
                logger_->info("verify_pending_offer_coins: offer {} has "
                              "CONFIRMED (filled) status -- deferring to "
                              "fill detector, not marking stale (pair={})",
                              po.offer_id.substr(0, 12), po.pair_name);
                not_found_counts_.erase(po.offer_id);
                continue;
            }
            logger_->warn("verify_pending_offer_coins: offer {} has terminal "
                          "wallet status {} but still in State (pair={})",
                          po.offer_id.substr(0, 12), status, po.pair_name);
            stale_offer_ids.push_back(po.offer_id);
            not_found_counts_.erase(po.offer_id);
        }
    }

    // Drop counters for offers no longer pending in State.
    for (auto it = not_found_counts_.begin(); it != not_found_counts_.end(); ) {
        if (current_pending_ids.find(it->first) == current_pending_ids.end()) {
            it = not_found_counts_.erase(it);
        } else {
            ++it;
        }
    }

    if (!stale_offer_ids.empty()) {
        logger_->info("verify_pending_offer_coins: found {} stale offers "
                      "(out of {} pending)", stale_offer_ids.size(),
                      pending.size());
    }

    co_return stale_offer_ids;
}

// ===========================================================================
// Block fee extraction
// ===========================================================================

asio::awaitable<std::vector<BlockFeeInfo>>
OnChainReconciler::extract_block_fees(
    BlockHeight start_height,
    BlockHeight end_height,
    const std::unordered_set<std::string>& our_puzzle_hashes)
{
    std::vector<BlockFeeInfo> results;

    // Limit the range to prevent excessive RPC calls.
    constexpr BlockHeight kMaxBlockRange = 50;
    if (end_height > start_height + kMaxBlockRange) {
        start_height = end_height - kMaxBlockRange;
    }

    for (BlockHeight h = start_height; h <= end_height; ++h) {
        try {
            // Step 1: Get the block record to obtain header_hash.
            auto block_rec = co_await full_node_->get_block_record_by_height(
                static_cast<std::int64_t>(h));

            if (block_rec.empty() || !block_rec.contains("header_hash")) {
                continue;
            }

            std::string hh = block_rec["header_hash"].get<std::string>();

            // Step 2: Get additions and removals for this block.
            auto ar = co_await full_node_->get_additions_and_removals(hh);

            BlockFeeInfo info;
            info.block_height = h;
            info.header_hash  = hh;

            // Sum additions.
            if (ar.contains("additions") && ar["additions"].is_array()) {
                for (const auto& coin_rec : ar["additions"]) {
                    const auto& coin = coin_rec.contains("coin")
                        ? coin_rec["coin"] : coin_rec;
                    if (coin.contains("amount")) {
                        Mojo amt = coin["amount"].get<Mojo>();
                        info.total_additions += amt;

                        // Check if this coin belongs to us.
                        if (coin.contains("puzzle_hash")) {
                            std::string ph =
                                coin["puzzle_hash"].get<std::string>();
                            if (ph.size() > 2 && ph.substr(0, 2) == "0x") {
                                ph = ph.substr(2);
                            }
                            if (our_puzzle_hashes.count(ph) > 0) {
                                ++info.our_additions;
                                info.our_net_change += amt;
                            }
                        }
                    }
                }
            }

            // Sum removals.
            if (ar.contains("removals") && ar["removals"].is_array()) {
                for (const auto& coin_rec : ar["removals"]) {
                    const auto& coin = coin_rec.contains("coin")
                        ? coin_rec["coin"] : coin_rec;
                    if (coin.contains("amount")) {
                        Mojo amt = coin["amount"].get<Mojo>();
                        info.total_removals += amt;

                        // Check if this coin belongs to us.
                        if (coin.contains("puzzle_hash")) {
                            std::string ph =
                                coin["puzzle_hash"].get<std::string>();
                            if (ph.size() > 2 && ph.substr(0, 2) == "0x") {
                                ph = ph.substr(2);
                            }
                            if (our_puzzle_hashes.count(ph) > 0) {
                                ++info.our_removals;
                                info.our_net_change -= amt;
                            }
                        }
                    }
                }
            }

            // Implied blockchain fee = removals - additions.
            // Positive value = mojos paid to the farmer as fees.
            info.implied_fees = info.total_removals - info.total_additions;

            // Only log blocks where we had activity.
            if (info.our_additions > 0 || info.our_removals > 0) {
                logger_->debug("extract_block_fees: block={} adds={} "
                               "removes={} fee={} our_adds={} our_removes={} "
                               "our_net={}",
                               h, info.total_additions, info.total_removals,
                               info.implied_fees,
                               info.our_additions, info.our_removals,
                               info.our_net_change);
            }

            results.push_back(std::move(info));
        } catch (const std::exception& e) {
            logger_->debug("extract_block_fees: block {} failed: {}",
                           h, e.what());
        }
    }

    co_return results;
}

// ===========================================================================
// Full reconciliation
// ===========================================================================

asio::awaitable<std::pair<
    std::vector<std::string>,
    std::vector<BalanceDiscrepancy>
>> OnChainReconciler::run_full_reconciliation(
    const std::unordered_map<std::string, std::int64_t>& wallet_ids,
    BlockHeight current_block,
    const std::unordered_set<std::string>& our_puzzle_hashes)
{
    logger_->info("run_full_reconciliation: starting at block {} "
                  "(last reconciled: {})",
                  current_block, last_reconciled_block_);

    // Phase 1: Verify pending offer coins.
    auto stale_ids = co_await verify_pending_offer_coins();

    // Remove stale offers from State.
    for (const auto& oid : stale_ids) {
        state_->remove_offer(oid);
    }

    // Phase 2: Reconcile balances.
    auto discrepancies = co_await reconcile_balances(wallet_ids);

    // Phase 3: Extract block fees since last reconciliation.
    if (last_reconciled_block_ > 0 && current_block > last_reconciled_block_) {
        auto fee_info = co_await extract_block_fees(
            last_reconciled_block_ + 1, current_block, our_puzzle_hashes);

        // Log summary of blocks with our activity.
        Mojo total_our_fees = 0;
        std::size_t blocks_with_activity = 0;
        for (const auto& fi : fee_info) {
            if (fi.our_additions > 0 || fi.our_removals > 0) {
                ++blocks_with_activity;
                // Our portion of fees = |our_net_change| when we had
                // coins spent (removals > additions for fee-paying txns).
                if (fi.our_net_change < 0) {
                    total_our_fees += std::abs(fi.our_net_change);
                }
            }
        }

        if (blocks_with_activity > 0) {
            logger_->info("run_full_reconciliation: {} blocks with our "
                          "activity, estimated on-chain fees: {} mojos",
                          blocks_with_activity, total_our_fees);
        }
    }

    last_reconciled_block_ = current_block;

    logger_->info("run_full_reconciliation: complete -- {} stale offers, "
                  "{} balance discrepancies",
                  stale_ids.size(), discrepancies.size());

    co_return std::make_pair(std::move(stale_ids), std::move(discrepancies));
}

}  // namespace xop
