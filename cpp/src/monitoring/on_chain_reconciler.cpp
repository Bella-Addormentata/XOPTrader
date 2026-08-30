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
#include "xop/execution/wallet_poll_throttle.hpp"
#include "xop/monitoring/reconcile_verdict.hpp"

#include <string_view>

#include <algorithm>
#include <cctype>
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

    // The caller labels wallets per PAIR/side, so one physical wallet (the
    // XCH wallet backs every XCH pair) arrives several times under
    // different labels.  Reconciling it once is correct; re-reporting the
    // same wallet four times per run was a quarter of this monitor's noise.
    std::unordered_set<std::int64_t> seen_wallet_ids;

    for (const auto& [label, wid] : wallet_ids) {
        if (!seen_wallet_ids.insert(wid).second) {
            continue;
        }
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

            // Collect unique puzzle hashes AND each spendable coin's
            // identity (parent, puzzle hash, amount).  Identities, not
            // totals: locked or change coins sharing a reused address
            // inflate the on-chain sum, and that surplus can exactly mask
            // a spendable coin that is MISSING on-chain -- the aggregate
            // subtraction reported OK in the one case this monitor exists
            // to catch.
            auto canon = [](std::string h) {
                if (h.size() > 2 && h[0] == '0'
                        && (h[1] == 'x' || h[1] == 'X')) {
                    h = h.substr(2);
                }
                std::transform(h.begin(), h.end(), h.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                return h;
            };
            auto coin_key = [&canon](const nlohmann::json& c) {
                return canon(c.value("parent_coin_info", std::string{}))
                     + ":" + canon(c.value("puzzle_hash", std::string{}))
                     + ":" + std::to_string(
                           c.contains("amount") ? c["amount"].get<Mojo>()
                                                : Mojo{0});
            };

            std::unordered_set<std::string> puzzle_hashes;
            std::vector<std::pair<std::string, Mojo>> wallet_coins;
            Mojo wallet_spendable = 0;
            for (const auto& coin_rec : coins_json) {
                const auto& coin_obj = coin_rec.contains("coin")
                    ? coin_rec["coin"] : coin_rec;
                if (coin_obj.contains("amount")) {
                    wallet_spendable += coin_obj["amount"].get<Mojo>();
                    wallet_coins.emplace_back(coin_key(coin_obj),
                                              coin_obj["amount"].get<Mojo>());
                }
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
            bool scan_complete = true;
            std::unordered_set<std::string> on_chain_keys;

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
                            on_chain_keys.insert(coin_key(rec["coin"]));
                        }
                    }
                } catch (const std::exception& e) {
                    scan_complete = false;
                    logger_->warn("reconcile_balances: full node query failed "
                                  "for puzzle_hash {}...{}: {}",
                                  ph.substr(0, 8),
                                  ph.substr(ph.size() > 8 ? ph.size() - 4 : 0),
                                  e.what());
                }
            }

            // A partial scan cannot support a shortfall claim: with one
            // address missing from the sum, on_chain < spendable is exactly
            // what a transient node error looks like -- the same false
            // positive this rework exists to remove, re-created by an RPC
            // hiccup.  Skip the comparison and say so.
            if (!scan_complete) {
                logger_->info("reconcile_balances: wallet={} scan incomplete "
                              "-- comparison skipped this run", label);
                continue;
            }

            // Step 4: Compare COIN BY COIN.
            //
            // Each earlier simplification failed differently.  Sums against
            // confirmed_wallet_balance were apples-to-oranges (locked coins
            // inflate confirmed; their addresses never enter the scan):
            // ~7,800 phantom warnings.  Sums against the spendable SUM
            // fixed that but could MASK the real fault: locked or change
            // coins at a reused address inflate the on-chain side, and that
            // surplus can exactly offset a spendable coin missing from the
            // chain -- reporting OK in the one case this monitor exists to
            // catch.
            //
            // So: every spendable coin the wallet claims must exist as an
            // unspent on-chain record, matched by identity (parent, puzzle
            // hash, amount).  Totals are context only.
            Mojo missing_sum = 0;
            std::size_t missing_count = 0;
            for (const auto& [key, amount] : wallet_coins) {
                if (on_chain_keys.find(key) == on_chain_keys.end()) {
                    missing_sum += amount;
                    ++missing_count;
                }
            }
            // Two distinct figures, kept separate: the AGGREGATE difference
            // (context; positive surplus at reused addresses is normal) and
            // the identity-matched MISSING sum (the alert condition).
            // Folding the second into a field named like the first is how a
            // report ends up printing arithmetic that does not add up.
            const Mojo aggregate_diff = on_chain_total - wallet_spendable;

            if (missing_count > 0) {
                BalanceDiscrepancy disc;
                disc.wallet_label      = label;
                disc.wallet_id         = wid;
                disc.wallet_confirmed  = wallet_confirmed;
                disc.wallet_spendable  = wallet_spendable;
                disc.on_chain_total    = on_chain_total;
                disc.difference        = aggregate_diff;
                disc.missing_sum       = missing_sum;
                disc.missing_count     = missing_count;
                disc.on_chain_coin_count = on_chain_count;
                discrepancies.push_back(disc);

                logger_->warn("reconcile_balances: SHORTFALL wallet={} "
                              "-- {} spendable coin(s) totalling {} mojos "
                              "have no unspent on-chain record "
                              "(spendable={} on_chain_sum={} confirmed={} "
                              "for context, {} coins on-chain)",
                              label, missing_count, missing_sum,
                              wallet_spendable, on_chain_total,
                              wallet_confirmed, on_chain_count);
            } else {
                // Normal outcome, including benign surplus at reused
                // addresses: debug, not info -- an OK line per wallet per
                // run is exactly the recurring noise this rework removes.
                logger_->debug("reconcile_balances: wallet={} balance OK "
                               "(every spendable coin found on-chain; "
                               "{} mojos across {} coins)",
                               label, wallet_spendable, wallet_coins.size());
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

    // ------------------------------------------------------------------
    // [S24 2026-08-29] Early-stopped RELEVANCE pagination.
    //
    // This loop was a pre-fix fork of OfferManager::reconcile_offers and
    // still walked the ENTIRE trade archive (20-25k records, 400-500
    // get_all_offers round-trips at ~1.6-2s each) every pass, because the
    // wallet's default ordering sorts pending offers LAST. Live logs put
    // this loop at ~795-845s of every ~818-845s pass -- 97% of S24's
    // "reconciliation dominates the heartbeat", misattributed in TODO.md
    // to block scanning. The [WALLET-LOAD 2026-08-04] fix in
    // offer_manager.cpp (sort_key="RELEVANCE" so the live set is on page
    // one, plus a consecutive-old-pages early stop) collapses it to ~2-4
    // pages; this ports that fix. Absence from the scan is STILL not
    // trusted on its own: the not-found path below individually verifies
    // with get_offer before any stale verdict.
    // ------------------------------------------------------------------
    std::int64_t oldest_tracked_unix = 0;
    for (const auto& po : pending) {
        const auto unix_s =
            std::chrono::duration_cast<std::chrono::seconds>(
                po.created_at_ts.time_since_epoch()).count();
        if (unix_s > 0
            && (oldest_tracked_unix == 0 || unix_s < oldest_tracked_unix)) {
            oldest_tracked_unix = unix_s;
        }
    }
    // [S24] The scan snapshot instant. Ages below are measured against
    // THIS, not against verdict time: on a slow pass an offer created
    // seconds after the snapshot aged past the grace period DURING the
    // scan and took a not-found miss for being absent from pages read
    // before it existed (observed live: "miss 1/3 ... age=842s" for an
    // offer 12s younger than the pass).
    const auto scan_started = std::chrono::system_clock::now();
    const std::int64_t now_unix =
        std::chrono::duration_cast<std::chrono::seconds>(
            scan_started.time_since_epoch()).count();
    const std::int64_t cutoff_unix =
        execution::reconcile_scan_cutoff(oldest_tracked_unix, now_unix);
    execution::ReconcileEarlyStop early_stop;

    // Paginate wallet offers to build a map of known trade IDs.
    std::unordered_map<std::string, int> wallet_offer_status;
    constexpr std::int64_t kPageSize = 50;
    std::int64_t offset = 0;
    std::int64_t pages_scanned = 0;
    bool more = true;
    bool wallet_query_succeeded = false;

    while (more) {
        try {
            auto records = co_await wallet_->get_all_offers(
                offset, offset + kPageSize, /*file_contents=*/false,
                /*include_completed=*/true,
                /*sort_key=*/"RELEVANCE", /*reverse=*/false);

            wallet_query_succeeded = true;
            ++pages_scanned;

            if (records.empty() ||
                static_cast<std::int64_t>(records.size()) < kPageSize) {
                more = false;
            }

            std::int64_t page_newest_created = 0;
            for (const auto& rec : records) {
                page_newest_created = std::max(
                    page_newest_created,
                    rec.value("created_at_time", std::int64_t{0}));
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

            // [S24] With RELEVANCE ordering the live set is at the front;
            // consecutive pages entirely older than the oldest tracked
            // offer (minus adoptee slack) mean the rest of the archive
            // holds nothing relevant.
            if (more && early_stop.observe_page(
                    execution::page_entirely_older(page_newest_created,
                                                   cutoff_unix))) {
                more = false;
            }
        } catch (const std::exception& e) {
            logger_->warn("verify_pending_offer_coins: wallet query failed "
                          "at offset {}: {}", offset, e.what());
            more = false;
        }
    }
    logger_->info("verify_pending_offer_coins: scanned {} page(s) for {} "
                  "pending offers ({} wallet records mapped)",
                  pages_scanned, pending.size(), wallet_offer_status.size());

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
            // [S24] Age at the SCAN SNAPSHOT, not at verdict time -- and
            // no miss accrual during grace: an in-grace absence is the
            // expected wallet propagation delay, not evidence.
            const auto age = scan_started - po.created_at_ts;

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

            auto& miss_count = not_found_counts_[po.offer_id];
            ++miss_count;

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

            // [S24][SETTLE-FIX pattern] Absence from a paged scan is not
            // proof, especially with the early stop above -- ask the wallet
            // about THIS trade id before the one-way verdict. Removing a
            // live offer loses its fill forever (the 2026-07-31 6-XCH
            // incident shape); one targeted RPC per candidate is cheap.
            //
            // [review] Three answers, three meanings:
            //  * a RECORD: route by its status through the unit-tested
            //    classify_direct_lookup -- only explicit terminals reap,
            //    an unknown code keeps the offer tracked (a newer wallet's
            //    status extension must never trigger removals).
            //  * "No trade" (ChiaRPCApplicationError): the wallet was
            //    reachable and definitively has NO record -- get_offer
            //    THROWS on absence, so this catch, not an empty record,
            //    is the authoritative not-found verdict.
            //  * any other error: a blip at verdict time never finalises
            //    a removal -- keep tracked, retry next pass.
            int direct_status = -1;
            try {
                auto rec = co_await wallet_->get_offer(po.offer_id,
                                                       /*file_contents=*/
                                                       false);
                if (rec.contains("status")) {
                    if (rec["status"].is_number()) {
                        direct_status = rec["status"].get<int>();
                    } else if (rec["status"].is_string()) {
                        const auto& ds = rec["status"].get_ref<
                            const std::string&>();
                        if (ds == "PENDING_ACCEPT")       direct_status = 0;
                        else if (ds == "PENDING_CONFIRM") direct_status = 1;
                        else if (ds == "PENDING_CANCEL")  direct_status = 2;
                        else if (ds == "CANCELLED")       direct_status = 3;
                        else if (ds == "CONFIRMED")       direct_status = 4;
                        else if (ds == "FAILED")          direct_status = 5;
                        else {
                            try {
                                direct_status = std::stoi(ds);
                            } catch (...) {
                                direct_status = -1;
                            }
                        }
                    }
                }
            } catch (const rpc::ChiaRPCApplicationError& e) {
                if (std::string_view(e.what()).find("No trade")
                        != std::string_view::npos) {
                    logger_->warn("verify_pending_offer_coins: offer {} NOT "
                                  "FOUND in wallet (direct get_offer "
                                  "confirms) -- marking as stale (pair={} "
                                  "tier={})",
                                  po.offer_id.substr(0, 12),
                                  po.pair_name, po.tier);
                    stale_offer_ids.push_back(po.offer_id);
                    not_found_counts_.erase(po.offer_id);
                    continue;
                }
                logger_->warn("verify_pending_offer_coins: direct get_offer "
                              "failed for {} -- keeping tracked, will retry "
                              "next pass: {}",
                              po.offer_id.substr(0, 12), e.what());
                continue;
            } catch (const std::exception& e) {
                logger_->warn("verify_pending_offer_coins: direct get_offer "
                              "failed for {} -- keeping tracked, will retry "
                              "next pass: {}",
                              po.offer_id.substr(0, 12), e.what());
                continue;
            }

            switch (monitoring::classify_direct_lookup(direct_status)) {
            case monitoring::DirectLookupVerdict::Live:
                logger_->info("verify_pending_offer_coins: offer {} absent "
                              "from the scan but direct lookup says status "
                              "{} -- live, not stale (pair={} tier={})",
                              po.offer_id.substr(0, 12), direct_status,
                              po.pair_name, po.tier);
                not_found_counts_.erase(po.offer_id);
                continue;
            case monitoring::DirectLookupVerdict::DeferToFillDetector:
                logger_->info("verify_pending_offer_coins: offer {} direct "
                              "lookup says CONFIRMED -- deferring to fill "
                              "detector, not marking stale (pair={})",
                              po.offer_id.substr(0, 12), po.pair_name);
                not_found_counts_.erase(po.offer_id);
                continue;
            case monitoring::DirectLookupVerdict::Stale:
                logger_->warn("verify_pending_offer_coins: offer {} direct "
                              "lookup says terminal status {} -- stale "
                              "(pair={} tier={})",
                              po.offer_id.substr(0, 12), direct_status,
                              po.pair_name, po.tier);
                stale_offer_ids.push_back(po.offer_id);
                not_found_counts_.erase(po.offer_id);
                continue;
            case monitoring::DirectLookupVerdict::KeepTracked:
            default:
                logger_->warn("verify_pending_offer_coins: offer {} direct "
                              "lookup returned unrecognised status {} -- "
                              "keeping tracked, not ours to remove "
                              "(pair={} tier={})",
                              po.offer_id.substr(0, 12), direct_status,
                              po.pair_name, po.tier);
                continue;
            }
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
    const auto t0 = std::chrono::steady_clock::now();

    // Phase 1: Verify pending offer coins.
    auto stale_ids = co_await verify_pending_offer_coins();
    const auto t1 = std::chrono::steady_clock::now();

    // Remove stale offers from State.
    for (const auto& oid : stale_ids) {
        state_->remove_offer(oid);
    }

    // Phase 2: Reconcile balances.
    auto discrepancies = co_await reconcile_balances(wallet_ids);
    const auto t2 = std::chrono::steady_clock::now();

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

    // [S24] Per-phase wall clock, so the next regression is a log line
    // instead of a two-day investigation. Pre-fix live shape for
    // reference: verify ~795-845s, balances+fees ~23-27s.
    const auto t3 = std::chrono::steady_clock::now();
    const auto secs = [](auto a, auto b) {
        return std::chrono::duration<double>(b - a).count();
    };
    logger_->info("run_full_reconciliation: complete -- {} stale offers, "
                  "{} balance discrepancies (verify {:.1f}s, balances "
                  "{:.1f}s, fees {:.1f}s, total {:.1f}s)",
                  stale_ids.size(), discrepancies.size(),
                  secs(t0, t1), secs(t1, t2), secs(t2, t3), secs(t0, t3));

    co_return std::make_pair(std::move(stale_ids), std::move(discrepancies));
}

}  // namespace xop
