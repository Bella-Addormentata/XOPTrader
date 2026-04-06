/**
 * @file offer_manager.cpp
 * @brief Implementation of the CHIA DEX offer lifecycle manager.
 *
 * See offer_manager.hpp for the full interface contract and design rationale.
 *
 * Key implementation notes:
 *   - Offer creation follows the exact Chia wallet RPC protocol:
 *       offer_dict maps wallet_id (int) -> signed mojo amount.
 *       Negative = we are offering (spending), positive = we request.
 *   - Fill detection uses the Chia trade-record status enum:
 *       0 = PENDING, 4 = CONFIRMED, 5 = CANCELLED, 6 = FAILED.
 *   - The never-sell-at-loss constraint is applied in build_tier_ladder():
 *       every ask price is floored at cost_basis * (1 + min_margin_bps/10000).
 *   - Rebalance triggers are pure functions of the current market state vs.
 *     the stored baseline snapshot -- no side effects.
 *
 * ISO/IEC 27001:2022 -- offer bech32 text logged at DEBUG only (not INFO).
 * ISO/IEC 5055       -- checked arithmetic on mojo conversions; no UB paths.
 * ISO/IEC 25000      -- structured logging with context; deterministic cleanup.
 */

#include <xop/execution/offer_manager.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace xop::execution {

// ---------------------------------------------------------------------------
// Chia wallet trade-record status codes (from chia-blockchain source).
// Newer Chia wallet versions return status as a string enum; older versions
// return an integer.  parse_trade_status() handles both.
// ---------------------------------------------------------------------------
namespace trade_status {
    constexpr int kPendingAccept    = 0;
    constexpr int kCancelled        = 3;
    constexpr int kConfirmed        = 4;
    constexpr int kFailed           = 5;

    inline int parse(const nlohmann::json& status_val) {
        if (status_val.is_number_integer()) {
            return status_val.get<int>();
        }
        if (status_val.is_string()) {
            const auto& s = status_val.get_ref<const std::string&>();
            if (s == "PENDING_ACCEPT")  return 0;
            if (s == "PENDING_CONFIRM") return 1;
            if (s == "PENDING_CANCEL")  return 2;
            if (s == "CANCELLED")       return kCancelled;
            if (s == "CONFIRMED")       return kConfirmed;
            if (s == "FAILED")          return kFailed;
        }
        return -1;  // Unknown status.
    }
}  // namespace trade_status

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

OfferManager::OfferManager(asio::io_context&                    /*ioc*/,
                           std::shared_ptr<rpc::ChiaWalletRPC>  wallet,
                           std::shared_ptr<rpc::DexieClient>    dexie_client,
                           std::shared_ptr<State>               state,
                           const AppConfig&                     config)
    : wallet_(std::move(wallet))
    , dexie_client_(std::move(dexie_client))
    , state_(std::move(state))
    , strategy_cfg_(config.strategy)
    , risk_cfg_(config.risk)
    , dexie_cfg_(config.dexie)
    , logger_(spdlog::default_logger()->clone("OfferMgr"))
    , current_fee_mojos_(config.strategy.offer_fee_mojos)
{
    // Validate tier configuration arrays match declared tier count.
    if (strategy_cfg_.tier_spacing_bps.size() != strategy_cfg_.num_tiers) {
        throw std::invalid_argument(
            "tier_spacing_bps length must equal num_tiers");
    }
    if (strategy_cfg_.tier_size_pct.size() != strategy_cfg_.num_tiers) {
        throw std::invalid_argument(
            "tier_size_pct length must equal num_tiers");
    }

    // Build pair config lookup so evaluate_rebalance() can resolve
    // base/quote asset IDs from a pair name without external help.
    // ISO/IEC 5055: deterministic O(1) lookup, value-copy for safety.
    for (const auto& pc : config.pairs) {
        pair_config_map_.emplace(pc.name, pc);
    }

    logger_->info("OfferManager initialised: {} tiers, TTL {} blocks, "
                  "{} pairs",
                  strategy_cfg_.num_tiers,
                  strategy_cfg_.offer_ttl_blocks,
                  pair_config_map_.size());
}

// ---------------------------------------------------------------------------
// post_quotes -- create multi-tier bid + ask offers on-chain
// ---------------------------------------------------------------------------

asio::awaitable<int> OfferManager::post_quotes(
    const PairConfig&              pair,
    const std::vector<TierQuote>&  quotes,
    BlockHeight                    block_height,
    double                         fee_reserve_override)
{
    // If the caller supplied a positive override (e.g. engine recovery
    // mode), use that instead of the configured fee_reserve_xch.
    const double effective_reserve = (fee_reserve_override > 0.0)
        ? fee_reserve_override
        : strategy_cfg_.fee_reserve_xch;
    // Lazy-init the wallet-ID cache on first call.
    if (!wallet_ids_resolved_) {
        co_await init_wallet_id_map();
    }

    const std::int64_t base_wid = resolve_wallet_id(pair.base_asset_id);
    const std::int64_t quote_wid = resolve_wallet_id(pair.quote_asset_id);
    if (base_wid < 0 || quote_wid < 0) {
        logger_->error("Skipping {} -- required wallet IDs are unavailable: "
                       "base='{}' (wid={}), quote='{}' (wid={})",
                       pair.name,
                       pair.base_asset_id, base_wid,
                       pair.quote_asset_id, quote_wid);
        co_return 0;
    }

    // [T7-10] Batch mode: merge same-side tiers into a single RPC call.
    if (strategy_cfg_.batch_offers_enabled && quotes.size() > 1) {
        // Split quotes by side.
        std::vector<TierQuote> bids, asks;
        for (const auto& tq : quotes) {
            if (tq.side == Side::Bid) bids.push_back(tq);
            else                      asks.push_back(tq);
        }

        int bid_count = 0;
        int ask_count = 0;

        // -- XCH fee reserve pre-check (batch mode) ------------------------
        // Verify XCH spendable >= fee_reserve_xch before posting.
        // ALL offers lock XCH UTXOs for on-chain fees, even buy-XCH
        // offers.  No exemptions -- UTXO locking is direction-agnostic.
        bool reserve_breached = false;
        const bool bids_buy_xch = (pair.base_asset_id == "xch");
        const bool asks_buy_xch = (pair.quote_asset_id == "xch");
        if (effective_reserve > 0.0) {
            try {
                auto xch_bal = co_await wallet_->get_wallet_balance(1);
                Mojo xch_spendable = 0;
                if (xch_bal.contains("spendable_balance"))
                    xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
                const auto reserve_mojos = static_cast<Mojo>(std::llround(
                    effective_reserve
                    * static_cast<double>(kMojosPerXch)));
                if (xch_spendable < reserve_mojos) {
                    logger_->warn(
                        "XCH fee reserve pre-check (batch): spendable "
                        "{:.6f} XCH < reserve {:.3f} XCH before {} "
                        "-- skipping all offers",
                        static_cast<double>(xch_spendable) / kMojosPerXch,
                        effective_reserve, pair.name);
                    reserve_breached = true;
                }
            } catch (const std::exception& e) {
                logger_->warn("XCH fee reserve pre-check (batch) failed "
                              "for {}: {}", pair.name, e.what());
            }
        }

        if (!bids.empty() && !reserve_breached) {
            bid_count = co_await post_merged_side(pair, bids, block_height);
        }

        // -- XCH fee reserve guard (batch mode, UTXO-aware) ----------------
        // Check XCH spendable balance after bids; skip asks if below reserve.
        // No buy-XCH exemption: UTXO locking is direction-agnostic.
        if (bid_count > 0 && !asks.empty()
            && effective_reserve > 0.0) {
            try {
                auto xch_bal = co_await wallet_->get_wallet_balance(1);
                Mojo xch_spendable = 0;
                if (xch_bal.contains("spendable_balance"))
                    xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
                const auto reserve_mojos = static_cast<Mojo>(std::llround(
                    effective_reserve
                    * static_cast<double>(kMojosPerXch)));
                if (xch_spendable < reserve_mojos) {
                    logger_->warn(
                        "XCH fee reserve guard (batch): spendable {:.6f} XCH "
                        "< reserve {:.3f} XCH after posting {} bids "
                        "-- skipping ask batch",
                        static_cast<double>(xch_spendable) / kMojosPerXch,
                        effective_reserve, pair.name);
                    reserve_breached = true;
                }
            } catch (const std::exception& e) {
                logger_->warn("XCH fee reserve guard (batch): balance check "
                              "failed after {} bids: {}", pair.name, e.what());
            }
        }

        if (!asks.empty() && !reserve_breached) {
            ask_count = co_await post_merged_side(pair, asks, block_height);
        }

        // [T5-08] Asymmetric ladder guard: if one side posted successfully
        // but the other side failed completely, cancel the posted side to
        // prevent running a one-sided book (pure adverse selection).
        // Per Avellaneda-Stoikov (2008), a market maker with only bids
        // (or only asks) is guaranteed to accumulate inventory directionally
        // with no spread capture to offset the risk.
        //
        // Exception: when XCH reserve is breached and the posted side buys
        // XCH, the other side was *intentionally* suppressed.  Do not
        // cancel in that case -- the one-sided book is by design.
        const bool bid_side_exempt = reserve_breached && bids_buy_xch;
        const bool ask_side_exempt = reserve_breached && asks_buy_xch;
        if (bid_count > 0 && ask_count == 0 && !asks.empty()
            && !bid_side_exempt) {
            logger_->warn("Asymmetric ladder: {} bids posted but 0/{} asks "
                          "-- cancelling bids to prevent one-sided book",
                          bid_count, asks.size());
            // Cancel the just-posted bids.
            auto bid_offers = state_->get_all_offers();
            for (const auto& po : bid_offers) {
                if (po.pair_name == pair.name &&
                    po.side == Side::Bid &&
                    po.created_at_block == block_height &&
                    !po.cancel_pending) {
                    bool cancel_ok = false;
                    bool needs_emergency = false;
                    try {
                        co_await wallet_->cancel_offer(
                            po.offer_id, current_fee_mojos_, /*secure=*/true);
                        cancel_ok = true;
                    } catch (const rpc::ChiaRPCError& e) {
                        const std::string_view msg{e.what()};
                        needs_emergency =
                            msg.find("insufficient funds") != std::string_view::npos ||
                            msg.find("spendable balance") != std::string_view::npos;
                        if (!needs_emergency)
                            logger_->error("Failed to cancel asymmetric bid {}: {}",
                                           po.offer_id.substr(0, 12), e.what());
                    }
                    if (needs_emergency)
                        cancel_ok = co_await emergency_cancel(
                            po.offer_id, "asymmetric_bid");
                    if (cancel_ok) {
                        state_->mark_cancel_pending(po.offer_id);
                        --bid_count;
                    }
                }
            }
        } else if (ask_count > 0 && bid_count == 0 && !bids.empty()
                   && !ask_side_exempt) {
            logger_->warn("Asymmetric ladder: {} asks posted but 0/{} bids "
                          "-- cancelling asks to prevent one-sided book",
                          ask_count, bids.size());
            auto ask_offers = state_->get_all_offers();
            for (const auto& po : ask_offers) {
                if (po.pair_name == pair.name &&
                    po.side == Side::Ask &&
                    po.created_at_block == block_height &&
                    !po.cancel_pending) {
                    bool cancel_ok = false;
                    bool needs_emergency = false;
                    try {
                        co_await wallet_->cancel_offer(
                            po.offer_id, current_fee_mojos_, /*secure=*/true);
                        cancel_ok = true;
                    } catch (const rpc::ChiaRPCError& e) {
                        const std::string_view msg{e.what()};
                        needs_emergency =
                            msg.find("insufficient funds") != std::string_view::npos ||
                            msg.find("spendable balance") != std::string_view::npos;
                        if (!needs_emergency)
                            logger_->error("Failed to cancel asymmetric ask {}: {}",
                                           po.offer_id.substr(0, 12), e.what());
                    }
                    if (needs_emergency)
                        cancel_ok = co_await emergency_cancel(
                            po.offer_id, "asymmetric_ask");
                    if (cancel_ok) {
                        state_->mark_cancel_pending(po.offer_id);
                        --ask_count;
                    }
                }
            }
        }

        int created_count = bid_count + ask_count;
        logger_->info("post_quotes (batched): {}/{} offers created for {}",
                      created_count, quotes.size(), pair.name);
        co_return created_count;
    }

    int created_count = 0;
    bool bid_funds_exhausted = false;
    bool ask_funds_exhausted = false;

    for (const auto& tier : quotes) {
        // Skip further tiers on the side that already reported
        // insufficient funds; the other side may still succeed.
        if (tier.side == Side::Bid && bid_funds_exhausted) continue;
        if (tier.side == Side::Ask && ask_funds_exhausted) continue;

        // -- XCH fee reserve pre-creation guard (per-offer, UTXO-aware) ----
        // Check XCH spendable BEFORE each individual offer creation.
        // The Chia wallet locks entire UTXOs for fee coins -- a single
        // create_offer can lock far more than the 5M mojo fee.  This
        // prevents the last UTXO from being locked when the balance is
        // already at the reserve threshold.
        //
        // ALL offers lock XCH UTXOs for on-chain fees, even offers that
        // buy XCH.  The buy-XCH exemption was incorrect: while filling
        // the offer would increase XCH, creating it still drains spendable
        // XCH via UTXO locking.
        if (effective_reserve > 0.0) {
            try {
                auto xch_bal = co_await wallet_->get_wallet_balance(1);
                Mojo xch_spendable = 0;
                if (xch_bal.contains("spendable_balance"))
                    xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
                const auto reserve_mojos = static_cast<Mojo>(std::llround(
                    effective_reserve
                    * static_cast<double>(kMojosPerXch)));
                if (xch_spendable < reserve_mojos) {
                    logger_->warn(
                        "XCH fee reserve pre-check: spendable {:.6f} XCH "
                        "< reserve {:.3f} XCH before {} {} tier {} "
                        "-- stopping all offers",
                        static_cast<double>(xch_spendable) / kMojosPerXch,
                        effective_reserve,
                        pair.name, to_string(tier.side), tier.tier_index);
                    bid_funds_exhausted = true;
                    ask_funds_exhausted = true;
                    break;
                }
            } catch (const std::exception& e) {
                logger_->warn("XCH fee reserve pre-check failed before "
                              "{} tier {}: {} -- skipping cautiously",
                              pair.name, tier.tier_index, e.what());
                bid_funds_exhausted = true;
                ask_funds_exhausted = true;
                break;
            }
        }

        // Step 1: Build the offer_dict for the wallet RPC.
        json offer_dict = build_offer_dict(pair, tier);
        if (offer_dict.empty()) {
            logger_->warn("Skipping tier {} {} -- could not build offer_dict",
                          tier.tier_index, to_string(tier.side));
            continue;
        }

        // Step 2: Call wallet.create_offer() to produce the spend bundle.
        json result;
        try {
            result = co_await wallet_->create_offer(
                offer_dict, current_fee_mojos_, /*validate_only=*/false);
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("create_offer failed for {} {} tier {}: {}",
                           pair.name, to_string(tier.side),
                           tier.tier_index, e.what());
            const std::string_view msg{e.what()};
            if (msg.find("insufficient funds") != std::string_view::npos ||
                msg.find("spendable balance") != std::string_view::npos) {
                if (tier.side == Side::Bid) {
                    bid_funds_exhausted = true;
                    logger_->warn("Stopping {} BID tiers -- "
                                  "wallet reported insufficient funds",
                                  pair.name);
                } else {
                    ask_funds_exhausted = true;
                    logger_->warn("Stopping {} ASK tiers -- "
                                  "wallet reported insufficient funds",
                                  pair.name);
                }
            }
            continue;
        }

        // Step 3: Extract the bech32m offer text.
        if (!result.contains("offer") || !result["offer"].is_string()) {
            logger_->error("create_offer response missing 'offer' field "
                           "for {} tier {}", pair.name, tier.tier_index);
            continue;
        }
        std::string offer_text = result["offer"].get<std::string>();

        // Extract the trade_id for lifecycle tracking.
        std::string trade_id;
        if (result.contains("trade_record") &&
            result["trade_record"].contains("trade_id")) {
            trade_id = result["trade_record"]["trade_id"].get<std::string>();
        } else {
            logger_->warn("No trade_id in create_offer response for {} tier {}",
                          pair.name, tier.tier_index);
            continue;
        }

        // Step 4: Submit to dexie for cross-platform aggregation (best-effort).
        bool dexie_ok = co_await submit_to_dexie(offer_text);
        if (!dexie_ok) {
            logger_->warn("Dexie submission failed for {} tier {} -- "
                          "offer is still valid on-chain",
                          pair.name, tier.tier_index);
        }

        // Step 5: Track as a PendingOffer in shared state.
        PendingOffer pending;
        pending.offer_id         = trade_id;
        pending.pair_name        = pair.name;
        pending.side             = tier.side;
        pending.price            = tier.price;
        pending.size             = tier.size;
        pending.tier             = tier.tier_index;
        pending.created_at_block = block_height;
        pending.created_at_ts    = std::chrono::system_clock::now();
        pending.fee_mojos        = current_fee_mojos_;

        state_->upsert_offer(pending);
        ++created_count;

        logger_->info("Posted {} {} tier {} @ {} mojos, size {} mojos [{}]",
                      pair.name, to_string(tier.side), tier.tier_index,
                      tier.price, tier.size, trade_id.substr(0, 12));
        // Full offer text at DEBUG only (ISO/IEC 27001: minimise exposure).
        logger_->debug("Offer text: {}...", offer_text.substr(0, 40));

        // -- XCH fee reserve guard (UTXO-aware) ----------------------------
        // The Chia wallet locks entire UTXOs when creating offers, not just
        // the offered amount.  A 0.03 XCH offer can lock a 13 XCH UTXO.
        // Even non-XCH offers lock XCH for fee coins.  Check the actual
        // wallet spendable balance after each creation and stop if below
        // the configured reserve.  Applies to ALL offers including
        // buy-XCH tiers, since UTXO locking is direction-agnostic.
        if (effective_reserve > 0.0) {
            try {
                auto xch_bal = co_await wallet_->get_wallet_balance(1);
                Mojo xch_spendable = 0;
                if (xch_bal.contains("spendable_balance"))
                    xch_spendable = xch_bal["spendable_balance"].get<Mojo>();
                const auto reserve_mojos = static_cast<Mojo>(std::llround(
                    effective_reserve
                    * static_cast<double>(kMojosPerXch)));
                if (xch_spendable < reserve_mojos) {
                    logger_->warn(
                        "XCH fee reserve guard: spendable {:.6f} XCH "
                        "< reserve {:.3f} XCH after posting {} {} tier {} "
                        "-- stopping further offers this heartbeat",
                        static_cast<double>(xch_spendable) / kMojosPerXch,
                        effective_reserve,
                        pair.name, to_string(tier.side), tier.tier_index);
                    bid_funds_exhausted = true;
                    ask_funds_exhausted = true;
                }
            } catch (const std::exception& e) {
                logger_->warn("XCH fee reserve guard: balance check failed "
                              "after {} tier {}: {}",
                              pair.name, tier.tier_index, e.what());
            }
        }
    }

    logger_->info("post_quotes complete: {}/{} offers created for {}",
                  created_count, quotes.size(), pair.name);
    co_return created_count;
}

// ---------------------------------------------------------------------------
// detect_fills -- poll wallet, identify settled offers, emit Fill events
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<Fill>> OfferManager::detect_fills()
{
    std::vector<Fill> fills;

    // Get all known pending offers from state for comparison.
    auto pending_offers = state_->get_all_offers();
    if (pending_offers.empty()) {
        co_return fills;
    }

    // Build a lookup set of pending offer IDs for O(1) membership testing.
    std::unordered_map<std::string, PendingOffer> pending_map;
    pending_map.reserve(pending_offers.size());
    for (auto& po : pending_offers) {
        pending_map.emplace(po.offer_id, std::move(po));
    }

    // Poll the wallet for all trade records (include_completed=true
    // so we see offers that were filled between cancel-submission and
    // cancel-confirmation on-chain).
    constexpr std::int64_t kPageSize = 50;
    std::int64_t offset = 0;
    bool more = true;

    while (more) {
        std::vector<json> trade_records;
        try {
            trade_records = co_await wallet_->get_all_offers(
                offset, offset + kPageSize, /*file_contents=*/false);
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("get_all_offers failed during fill detection: {}",
                           e.what());
            break;
        }

        if (trade_records.empty() ||
            static_cast<std::int64_t>(trade_records.size()) < kPageSize) {
            more = false;
        }

        for (const auto& rec : trade_records) {
            // Extract trade_id and status from the record.
            if (!rec.contains("trade_id") || !rec.contains("status")) {
                continue;
            }
            std::string trade_id = rec["trade_id"].get<std::string>();
            int status = trade_status::parse(rec["status"]);

            // Only process records that are in our pending map.
            auto it = pending_map.find(trade_id);
            if (it == pending_map.end()) {
                continue;
            }

            const PendingOffer& po = it->second;

            if (status == trade_status::kConfirmed) {
                // Offer was taken and settled -- this is a fill.
                Fill fill;
                fill.offer_id     = trade_id;
                fill.pair_name    = po.pair_name;
                fill.side         = po.side;
                fill.price        = po.price;
                fill.size         = po.size;
                fill.timestamp    = std::chrono::system_clock::now();

                // Extract confirmed block height if available.
                if (rec.contains("confirmed_at_index")) {
                    fill.block_height = static_cast<BlockHeight>(
                        rec["confirmed_at_index"].get<std::int64_t>());
                } else {
                    fill.block_height = 0;
                }

                // T1-08: Update position accounting using canonical asset IDs
                // from the pair config, NOT the human-readable pair_name.
                // State::record_buy/record_sell index positions by AssetId
                // (e.g. "xch", hex CAT ID), so passing pair_name (e.g.
                // "XCH/wUSDC") would create phantom position entries keyed
                // by the pair label rather than updating the actual asset
                // balances.  The pair_config_map_ was added to support this
                // lookup.
                //
                // [T5-02] ISO/IEC 5055: position accounting failures must
                // NOT prevent the fill from being emitted or the offer from
                // being removed.  The wallet's confirmed fill is the
                // authoritative record; inventory discrepancy is correctable
                // but a missed fill is not.
                auto pc_it = pair_config_map_.find(po.pair_name);
                if (pc_it == pair_config_map_.end()) {
                    logger_->error(
                        "detect_fills: no pair config for '{}' -- "
                        "cannot update position for trade {}",
                        po.pair_name, trade_id.substr(0, 12));
                } else {
                    const auto& pc = pc_it->second;
                    if (po.side == Side::Bid) {
                        // Bid fill: we BOUGHT base asset.
                        state_->record_buy(pc.base_asset_id, po.size, po.price);
                    } else {
                        // Ask fill: we SOLD base asset.
                        bool sold = state_->record_sell(pc.base_asset_id, po.size);
                        if (!sold) {
                            logger_->error(
                                "record_sell failed for {} (asset={}) "
                                "-- insufficient balance; fill still "
                                "recorded, inventory needs reconciliation",
                                trade_id.substr(0, 12), pc.base_asset_id);
                        }
                    }
                }

                logger_->info("FILL {} {} {} mojos @ {} mojos [{}]",
                              (po.side == Side::Bid) ? "BID" : "ASK",
                              po.pair_name, po.size, po.price,
                              trade_id.substr(0, 12));

                // Remove from pending offers.
                // [T5-02] The fill is emitted regardless of remove_offer
                // result.  If the offer was already removed (e.g. by
                // reconciliation), we still record the fill event.
                if (!state_->remove_offer(trade_id)) {
                    logger_->warn("detect_fills: offer {} already removed "
                                  "from state", trade_id.substr(0, 12));
                }
                fills.push_back(std::move(fill));

            } else if (status == trade_status::kCancelled ||
                       status == trade_status::kFailed) {
                // Offer was cancelled or failed -- remove from tracking.
                // [T5-02] Defensive: log if already removed.
                if (!state_->remove_offer(trade_id)) {
                    logger_->debug("Offer {} already removed from state "
                                   "(status={})", trade_id.substr(0, 12),
                                   status);
                } else {
                    logger_->info("Offer {} removed (status={})",
                                  trade_id.substr(0, 12), status);
                }
            }
            // Status PENDING_ACCEPT / PENDING_CONFIRM: still alive, no action.
        }

        offset += kPageSize;
    }

    if (!fills.empty()) {
        logger_->info("detect_fills: {} new fills detected", fills.size());
    }

    co_return fills;
}

// ---------------------------------------------------------------------------
// cancel_stale -- cancel offers exceeding their block-based TTL
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>> OfferManager::cancel_stale(
    const std::string& pair_name,
    BlockHeight        current_block,
    BlockHeight        ttl_blocks)
{
    auto all_offers = state_->get_all_offers();
    std::vector<std::string> cancelled_ids;

    for (const auto& po : all_offers) {
        // Filter by pair name.
        if (po.pair_name != pair_name) {
            continue;
        }

        // Skip offers already awaiting cancel confirmation.
        if (po.cancel_pending) {
            continue;
        }

        // Check if the offer has exceeded its TTL.
        if (current_block < po.created_at_block + ttl_blocks) {
            continue;
        }

        // Cancel via wallet RPC (secure = true to spend the locked coin).
        bool cancel_ok = false;
        bool needs_emergency = false;
        try {
            co_await wallet_->cancel_offer(
                po.offer_id, current_fee_mojos_, /*secure=*/true);
            cancel_ok = true;
        } catch (const rpc::ChiaRPCError& e) {
            const std::string_view msg{e.what()};
            needs_emergency =
                msg.find("insufficient funds") != std::string_view::npos ||
                msg.find("spendable balance") != std::string_view::npos;
            if (!needs_emergency) {
                logger_->error("Failed to cancel offer {}: {}",
                               po.offer_id.substr(0, 12), e.what());
            }
        }
        if (needs_emergency) {
            cancel_ok = co_await emergency_cancel(
                po.offer_id, "cancel_stale");
        }
        if (cancel_ok) {
            state_->mark_cancel_pending(po.offer_id);
            cancelled_ids.push_back(po.offer_id);

            logger_->info("Cancelled stale offer {} ({} tier {}, age {} blocks)",
                          po.offer_id.substr(0, 12),
                          pair_name, po.tier,
                          current_block - po.created_at_block);
        }
    }

    if (!cancelled_ids.empty()) {
        logger_->info("cancel_stale({}): {} offers cancelled", pair_name,
                      cancelled_ids.size());
    }

    co_return cancelled_ids;
}

// ---------------------------------------------------------------------------
// cancel_all -- shutdown: cancel every pending offer
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>> OfferManager::cancel_all()
{
    logger_->info("cancel_all: initiating bulk cancellation");

    auto all_offers = state_->get_all_offers();
    if (all_offers.empty()) {
        logger_->info("cancel_all: no pending offers to cancel");
        co_return std::vector<std::string>{};
    }

    // ISO/IEC 5055: track which offers were successfully cancelled.
    // Only clear offers whose cancellation succeeded.
    // Failed cancellations remain tracked for retry on next heartbeat.
    std::vector<std::string> cancelled_ids;
    cancelled_ids.reserve(all_offers.size());

    // Attempt bulk cancellation first (wallet cancel_offers endpoint).
    bool bulk_ok = false;
    std::string bulk_err;
    try {
        co_await wallet_->cancel_offers(current_fee_mojos_, /*secure=*/true);
        logger_->info("cancel_all: bulk cancel_offers succeeded");
        bulk_ok = true;
    } catch (const rpc::ChiaRPCError& e) {
        bulk_err = e.what();
    }

    if (bulk_ok) {
        // Bulk success: all offers are considered cancelled.
        logger_->info("cancel_all: bulk cancel_offers succeeded");
        for (const auto& po : all_offers) {
            cancelled_ids.push_back(po.offer_id);
        }
    } else {
        // Bulk cancel failed -- fall back to individual cancellation.
        logger_->warn("Bulk cancel_offers failed: {} -- falling back to "
                      "individual cancellation", bulk_err);

        for (const auto& po : all_offers) {
            bool cancel_ok = false;
            bool needs_emergency = false;
            try {
                co_await wallet_->cancel_offer(
                    po.offer_id, current_fee_mojos_, /*secure=*/true);
                logger_->debug("Cancelled offer {}", po.offer_id.substr(0, 12));
                cancel_ok = true;
            } catch (const rpc::ChiaRPCError& inner_e) {
                const std::string_view msg{inner_e.what()};
                needs_emergency =
                    msg.find("insufficient funds") != std::string_view::npos ||
                    msg.find("spendable balance") != std::string_view::npos;
                if (!needs_emergency) {
                    logger_->error("Failed to cancel offer {}: {}",
                                   po.offer_id.substr(0, 12), inner_e.what());
                }
            }
            if (needs_emergency)
                cancel_ok = co_await emergency_cancel(
                    po.offer_id, "cancel_all");
            if (cancel_ok)
                cancelled_ids.push_back(po.offer_id);
        }
    }

    // Mark offers whose cancellation succeeded as cancel_pending.
    // detect_fills will handle final removal when the wallet confirms.
    for (const auto& id : cancelled_ids) {
        state_->mark_cancel_pending(id);
    }

    logger_->info("cancel_all: {}/{} offers cancelled successfully",
                  cancelled_ids.size(), all_offers.size());
    co_return cancelled_ids;
}

// ---------------------------------------------------------------------------
// evaluate_rebalance -- check all five rebalance triggers
// ---------------------------------------------------------------------------

RebalanceReason OfferManager::evaluate_rebalance(
    const std::string& pair_name,
    Mojo               current_mid,
    BlockHeight        current_block,
    double             current_vol,
    double             current_volume) const
{
    auto it = rebalance_baselines_.find(pair_name);
    if (it == rebalance_baselines_.end()) {
        // No baseline recorded yet -- always trigger initial posting.
        return RebalanceReason::PriceMove;
    }

    const RebalanceSnapshot& base = it->second;
    RebalanceReason result = RebalanceReason::None;

    // Trigger 1: Price deviation > 2% from last rebalance mid.
    if (base.mid_price > 0) {
        double deviation = std::abs(
            static_cast<double>(current_mid - base.mid_price)
            / static_cast<double>(base.mid_price));
        if (deviation > kPriceDeviationThreshold) {
            result = result | RebalanceReason::PriceMove;
            logger_->debug("Rebalance trigger: price deviation {:.2f}% for {}",
                           deviation * 100.0, pair_name);
        }
    }

    // Trigger 2: Inventory skew > 60%.
    // Resolve the pair's base and quote asset IDs from the pair config map
    // so that inventory_skew() receives the correct position keys.
    // ISO/IEC 5055: previous code passed pair_name for both arguments,
    // which produced a degenerate skew of 0 (same numerator and denominator).
    double skew = 0.0;
    auto pair_it = pair_config_map_.find(pair_name);
    if (pair_it != pair_config_map_.end()) {
        const auto& pc = pair_it->second;
        skew = state_->inventory_skew(pc.base_asset_id, pc.quote_asset_id);
    } else {
        logger_->warn("evaluate_rebalance: no pair config for '{}' -- "
                       "skew defaults to 0.0", pair_name);
    }
    if (std::abs(skew) > kInventorySkewThreshold) {
        result = result | RebalanceReason::InventorySkew;
        logger_->debug("Rebalance trigger: inventory skew {:.2f} for {}",
                       skew, pair_name);
    }

    // Trigger 3: Time decay > ~69 blocks (1 hour at 52 s/block).
    if (current_block >= base.block_height + kTimeDecayBlocks) {
        result = result | RebalanceReason::TTLExpired;
        logger_->debug("Rebalance trigger: time decay ({} blocks stale) for {}",
                       current_block - base.block_height, pair_name);
    }

    // Trigger 4: Volume spike > 3x rolling average.
    if (base.volume_avg > 0.0 &&
        current_volume > kVolumeSpikeMultiplier * base.volume_avg) {
        result = result | RebalanceReason::RegimeChange;
        logger_->debug("Rebalance trigger: volume spike ({:.0f} vs avg {:.0f}) "
                       "for {}", current_volume, base.volume_avg, pair_name);
    }

    // Trigger 5: Volatility spike > 2x 7-day average.
    if (base.volatility_7d > 0.0 &&
        current_vol > kVolSpikeMult * base.volatility_7d) {
        result = result | RebalanceReason::ForcedRefresh;
        logger_->debug("Rebalance trigger: vol spike ({:.4f} vs 7d avg {:.4f}) "
                       "for {}", current_vol, base.volatility_7d, pair_name);
    }

    return result;
}

// ---------------------------------------------------------------------------
// record_rebalance -- update the baseline snapshot for a pair
// ---------------------------------------------------------------------------

void OfferManager::record_rebalance(const std::string&       pair_name,
                                    const RebalanceSnapshot& snap)
{
    rebalance_baselines_[pair_name] = snap;
    logger_->debug("Recorded rebalance baseline for {} at block {}, mid={}",
                   pair_name, snap.block_height, snap.mid_price);
}

// ---------------------------------------------------------------------------
// build_tier_ladder -- expand strategy output into multi-tier bid+ask quotes
// ---------------------------------------------------------------------------

std::vector<TierQuote> OfferManager::build_tier_ladder(
    Mojo   mid_price,
    Mojo   total_base,
    Mojo   total_quote,
    Mojo   cost_basis,
    double inv_skew) const
{
    std::vector<TierQuote> ladder;
    ladder.reserve(strategy_cfg_.num_tiers * 2);

    // Compute the never-sell-at-loss floor for ask prices.
    // Floor = cost_basis * (1 + min_profit_margin_bps / 10000).
    const double margin_mult =
        1.0 + strategy_cfg_.min_profit_margin_bps / 10000.0;
    const Mojo ask_floor = (cost_basis > 0)
        ? static_cast<Mojo>(std::ceil(
              static_cast<double>(cost_basis) * margin_mult))
        : 0;

    // Inventory skew adjusts relative sizing between bid and ask sides.
    // Positive skew = long base -> DECREASE bid size (buy less),
    //                               INCREASE ask size (sell more to reduce).
    // Negative skew = short base -> INCREASE bid size (buy more to rebuild),
    //                                DECREASE ask size (sell less).
    // This is the standard market-making inventory-reduction convention:
    // lean against the accumulated position to revert toward neutral.
    // Clamped to [-1, +1] for safety.
    const double clamped_skew = std::clamp(inv_skew, -1.0, 1.0);

    // Skew multiplier: at skew=+1 (long base), bid gets 0.5x (buy less),
    // ask gets 1.5x (sell more).  At skew=0, both get 1.0x (symmetric).
    // ISO/IEC 5055: signs inverted relative to original code which
    // incorrectly reinforced the existing position instead of reducing it.
    const double bid_size_mult = 1.0 - 0.5 * clamped_skew;
    const double ask_size_mult = 1.0 + 0.5 * clamped_skew;

    for (std::uint32_t i = 0; i < strategy_cfg_.num_tiers; ++i) {
        const double spacing_frac =
            strategy_cfg_.tier_spacing_bps[i] / 10000.0;
        const double size_frac = strategy_cfg_.tier_size_pct[i];

        // --- BID tier ---
        // Bid price = mid * (1 - tier_spacing)
        const Mojo bid_price = static_cast<Mojo>(
            std::floor(static_cast<double>(mid_price) * (1.0 - spacing_frac)));

        // Bid size = total_quote allocation, converted to base at bid_price,
        // adjusted by skew.  Floored to prevent zero-size offers.
        Mojo bid_size = 0;
        if (bid_price > 0) {
            bid_size = static_cast<Mojo>(std::floor(
                static_cast<double>(total_quote) * size_frac
                * bid_size_mult
                / static_cast<double>(bid_price)));
        }

        if (bid_price > 0 && bid_size > 0) {
            TierQuote bq;
            bq.side       = Side::Bid;
            bq.tier_index = static_cast<std::uint8_t>(i);
            bq.price      = bid_price;
            bq.size       = bid_size;
            bq.spread_bps = strategy_cfg_.tier_spacing_bps[i];
            ladder.push_back(bq);
        }

        // --- ASK tier ---
        // Ask price = mid * (1 + tier_spacing), but no lower than ask_floor
        // (never-sell-at-loss constraint).
        Mojo ask_price = static_cast<Mojo>(
            std::ceil(static_cast<double>(mid_price) * (1.0 + spacing_frac)));

        // Enforce the never-sell-at-loss floor.
        if (ask_floor > 0 && ask_price < ask_floor) {
            ask_price = ask_floor;
            logger_->debug("Tier {} ask raised to floor {} (cost basis {})",
                           i, ask_floor, cost_basis);
        }

        // Ask size = total_base allocation, adjusted by skew.
        Mojo ask_size = static_cast<Mojo>(std::floor(
            static_cast<double>(total_base) * size_frac * ask_size_mult));

        if (ask_price > 0 && ask_size > 0) {
            TierQuote aq;
            aq.side       = Side::Ask;
            aq.tier_index = static_cast<std::uint8_t>(i);
            aq.price      = ask_price;
            aq.size       = ask_size;
            aq.spread_bps = strategy_cfg_.tier_spacing_bps[i];
            ladder.push_back(aq);
        }
    }

    logger_->debug("build_tier_ladder: {} entries (mid={}, base={}, quote={}, "
                   "cost_basis={}, skew={:.2f})",
                   ladder.size(), mid_price, total_base, total_quote,
                   cost_basis, inv_skew);

    return ladder;
}

// ---------------------------------------------------------------------------
// pending_count -- accessor for current pending offer count
// ---------------------------------------------------------------------------

std::size_t OfferManager::pending_count() const
{
    return state_->offer_count();
}

// ---------------------------------------------------------------------------
// set_dynamic_fee / current_fee -- dynamic fee override
// ---------------------------------------------------------------------------

void OfferManager::set_dynamic_fee(std::uint64_t fee_mojos) noexcept
{
    current_fee_mojos_ = fee_mojos;
}

std::uint64_t OfferManager::current_fee() const noexcept
{
    return current_fee_mojos_;
}

// ---------------------------------------------------------------------------
// invalidate_wallet_ids -- force the wallet-ID cache to be rebuilt
// ---------------------------------------------------------------------------

void OfferManager::invalidate_wallet_ids() noexcept
{
    wallet_ids_resolved_ = false;
    wallet_id_map_.clear();
    logger_->info("Wallet ID cache invalidated -- will re-query on next "
                  "post_quotes()");
}

// ---------------------------------------------------------------------------
// [T5-01] classify_tier_staleness -- direction-aware price-deviation check
//
// Scholarly basis:
//   - Gao & Wang (2020): optimal cancel threshold is a function of the
//     fractional deviation from the current fair price.
//   - Aït-Sahalia & Saglam (2017): stale-quote risk increases with the
//     magnitude of price deviation, not uniformly across all tiers.
//
// Direction-aware approach:
//   Only *adverse* deviations trigger cancellation.  An adverse move is
//   one that makes our offer more generous than intended:
//     - Bid: new optimal < old price → our bid is too high (overpaying)
//     - Ask: new optimal > old price → our ask is too low (underselling)
//   Favorable deviations (bid drifted below optimal, ask drifted above)
//   make the offer more conservative and are safe to leave live.
//
//   Additionally, if the offer has crossed the mid-price it is flagged
//   for urgent cancellation regardless of threshold.
// ---------------------------------------------------------------------------

std::vector<TierClassification> OfferManager::classify_tier_staleness(
    const std::string&             pair_name,
    const std::vector<TierQuote>&  new_ladder,
    BlockHeight                    current_block,
    BlockHeight                    ttl_blocks,
    Mojo                           mid_price) const
{
    std::vector<TierClassification> results;

    auto pending = state_->get_all_offers();
    if (pending.empty()) return results;

    // Build a lookup: (side, tier_index) -> new optimal price.
    // This allows O(1) comparison for each pending offer.
    std::unordered_map<std::string, Mojo> optimal_prices;
    for (const auto& tq : new_ladder) {
        std::string key = std::to_string(static_cast<int>(tq.side))
                        + "_" + std::to_string(tq.tier_index);
        optimal_prices[key] = tq.price;
    }

    const double mid_p = static_cast<double>(mid_price);

    // Precompute hard TTL: absolute safety cap beyond which all offers
    // are expired regardless of price accuracy.
    const BlockHeight hard_ttl = ttl_blocks * kHardTtlMultiplier;

    for (const auto& po : pending) {
        if (po.pair_name != pair_name) continue;
        if (po.cancel_pending) continue;  // Already being cancelled.

        TierClassification tc;
        tc.offer_id   = po.offer_id;
        tc.tier_index = po.tier;
        tc.side       = po.side;

        const BlockHeight age = (current_block > po.created_at_block)
            ? (current_block - po.created_at_block) : 0;
        const bool past_soft_ttl = (age >= ttl_blocks);
        const bool past_hard_ttl = (age >= hard_ttl);

        // Hard TTL: absolute expiration regardless of price.
        // Safety backstop — offers should never live indefinitely.
        if (past_hard_ttl) {
            tc.staleness       = TierStaleness::Expired;
            tc.price_deviation = 1.0;  // maximal
            results.push_back(std::move(tc));
            continue;
        }

        // Look up the optimal price for this tier.
        std::string key = std::to_string(static_cast<int>(po.side))
                        + "_" + std::to_string(po.tier);
        auto opt_it = optimal_prices.find(key);
        if (opt_it == optimal_prices.end()) {
            // Tier no longer exists in the new ladder (e.g. filtered out
            // by fee-vs-gain gating).  Treat as stale.
            tc.staleness       = TierStaleness::Stale;
            tc.price_deviation = 1.0;
            tc.adverse         = true;
            results.push_back(std::move(tc));
            continue;
        }

        // Compute signed price deviation.
        const double old_p = static_cast<double>(po.price);
        const double new_p = static_cast<double>(opt_it->second);
        const double signed_dev = (old_p > 0.0)
            ? (new_p - old_p) / old_p
            : 1.0;

        tc.price_deviation = std::abs(signed_dev);

        // Determine if the deviation is adverse (makes our offer
        // more generous than intended) or favorable (more conservative).
        //   Bid: adverse when new_p < old_p (signed_dev < 0)
        //        → our old bid is higher than the new optimal, overpaying.
        //   Ask: adverse when new_p > old_p (signed_dev > 0)
        //        → our old ask is lower than the new optimal, underselling.
        tc.adverse = (po.side == Side::Bid)
            ? (signed_dev < 0.0)
            : (signed_dev > 0.0);

        // Crossing detection: a Bid above mid or Ask below mid is
        // extremely dangerous (immediate adverse selection risk).
        if (mid_price > 0) {
            if (po.side == Side::Bid && old_p > mid_p) {
                tc.crossed = true;
            } else if (po.side == Side::Ask && old_p < mid_p) {
                tc.crossed = true;
            }
        }

        // Classification logic (cancel-reduction optimisations):
        //
        //   1. Crossed mid-price → always Stale (urgent cancel, no age guard).
        //
        //   2. Soft TTL zone (soft TTL ≤ age < hard TTL):
        //      The offer is old.  Apply a gentler adverse threshold
        //      (kSoftTtlAdverseThreshold = 0.2%) — even a small drift
        //      on an aged offer should trigger a refresh.  But if the
        //      offer is still perfectly priced, keep it.
        //
        //   3. Minimum age guard (age < kMinRefreshAgeBlocks):
        //      Very young offers are protected from cancel churn because
        //      the round-trip fee (cancel + recreate) exceeds the adverse
        //      selection risk for small deviations.  Only crossed-mid
        //      bypasses this.
        //
        //   4. Tier-scaled threshold (normal zone):
        //      Outer tiers tolerate more movement because they are further
        //      from mid and capture larger spreads.
        //      threshold = kSelectiveRefreshThreshold × (1 + tier × scale)
        //        tier 0 → 0.50%   tier 1 → 0.75%
        //        tier 2 → 1.00%   tier 3 → 1.25%

        if (tc.crossed) {
            // (1) Urgent: offer crossed mid-price.
            tc.staleness = TierStaleness::Stale;
        } else if (past_soft_ttl) {
            // (2) Soft TTL zone: gentler threshold on old offers.
            if (tc.adverse && tc.price_deviation > kSoftTtlAdverseThreshold) {
                tc.staleness = TierStaleness::Expired;
            } else {
                tc.staleness = TierStaleness::Fresh;
            }
        } else if (age < kMinRefreshAgeBlocks) {
            // (3) Very young offer: protect from churn.
            tc.staleness = TierStaleness::Fresh;
        } else {
            // (4) Normal zone: tier-scaled adverse threshold.
            const double tier_threshold = kSelectiveRefreshThreshold
                * (1.0 + static_cast<double>(po.tier) * kTierThresholdScale);
            if (tc.adverse && tc.price_deviation > tier_threshold) {
                tc.staleness = TierStaleness::Stale;
            } else {
                tc.staleness = TierStaleness::Fresh;
            }
        }

        results.push_back(std::move(tc));
    }

    // Log summary.
    int fresh = 0, stale = 0, expired = 0;
    for (const auto& tc : results) {
        switch (tc.staleness) {
            case TierStaleness::Fresh:   ++fresh;   break;
            case TierStaleness::Stale:   ++stale;   break;
            case TierStaleness::Expired: ++expired; break;
        }
    }
    logger_->debug("classify_tier_staleness({}): {} fresh, {} stale, "
                   "{} expired", pair_name, fresh, stale, expired);

    return results;
}

// ---------------------------------------------------------------------------
// [T5-01] selective_cancel -- cancel only stale/expired tiers
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>> OfferManager::selective_cancel(
    const std::vector<std::string>& stale_ids)
{
    std::vector<std::string> cancelled_ids;
    if (stale_ids.empty()) co_return cancelled_ids;

    cancelled_ids.reserve(stale_ids.size());

    for (const auto& offer_id : stale_ids) {
        // Skip offers already awaiting cancel confirmation.
        auto existing = state_->get_offer(offer_id);
        if (existing.cancel_pending) {
            continue;
        }
        bool cancel_ok = false;
        bool needs_emergency = false;
        try {
            co_await wallet_->cancel_offer(
                offer_id, current_fee_mojos_, /*secure=*/true);
            cancel_ok = true;
        } catch (const rpc::ChiaRPCError& e) {
            const std::string_view msg{e.what()};
            needs_emergency =
                msg.find("insufficient funds") != std::string_view::npos ||
                msg.find("spendable balance") != std::string_view::npos;
            if (!needs_emergency) {
                logger_->error("selective_cancel: failed to cancel {}: {}",
                               offer_id.substr(0, 12), e.what());
            }
        }
        if (needs_emergency)
            cancel_ok = co_await emergency_cancel(
                offer_id, "selective_cancel");
        if (cancel_ok) {
            if (!state_->mark_cancel_pending(offer_id)) {
                logger_->warn("selective_cancel: offer {} already removed "
                              "from state", offer_id.substr(0, 12));
            }
            cancelled_ids.push_back(offer_id);
            logger_->debug("selective_cancel: cancelled {}",
                           offer_id.substr(0, 12));
        }
    }

    if (!cancelled_ids.empty()) {
        logger_->info("selective_cancel: {}/{} offers cancelled",
                      cancelled_ids.size(), stale_ids.size());
    }

    co_return cancelled_ids;
}

// ---------------------------------------------------------------------------
// [T4-11] reconcile_offers -- Full state reconciliation against wallet.
//
// Detects and corrects three classes of discrepancy:
//   1. Orphans: offers in State but not in wallet (wallet may have cancelled
//      or timed out without our knowledge).
//   2. Phantoms: offers in wallet matching our pending set that have
//      transitioned to a terminal status we missed.
//   3. Status mismatches: offers that changed state between polls.
//
// This is intentionally a heavyweight operation (full wallet scan) and
// should only be called periodically (e.g. every 20 blocks).
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>> OfferManager::reconcile_offers()
{
    std::vector<std::string> removed_ids;

    auto pending_offers = state_->get_all_offers();
    if (pending_offers.empty()) {
        co_return removed_ids;
    }

    // Build a lookup of all pending offer IDs from our state.
    std::unordered_map<std::string, PendingOffer> pending_map;
    pending_map.reserve(pending_offers.size());
    for (auto& po : pending_offers) {
        pending_map.emplace(po.offer_id, std::move(po));
    }

    // Collect all offer IDs found in the wallet for orphan detection.
    std::unordered_set<std::string> wallet_offer_ids;

    // Paginate through all wallet offers.
    constexpr std::int64_t kPageSize = 50;
    std::int64_t offset = 0;
    bool more = true;

    while (more) {
        std::vector<json> trade_records;
        try {
            trade_records = co_await wallet_->get_all_offers(
                offset, offset + kPageSize, /*file_contents=*/false);
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("[reconcile] get_all_offers failed: {}", e.what());
            co_return removed_ids;
        }

        if (trade_records.empty() ||
            static_cast<std::int64_t>(trade_records.size()) < kPageSize) {
            more = false;
        }

        for (const auto& rec : trade_records) {
            if (!rec.contains("trade_id") || !rec.contains("status")) {
                continue;
            }
            std::string trade_id = rec["trade_id"].get<std::string>();
            int status = trade_status::parse(rec["status"]);

            wallet_offer_ids.insert(trade_id);

            // Check if this wallet offer is one we are tracking.
            auto it = pending_map.find(trade_id);
            if (it == pending_map.end()) {
                continue;  // Not one of ours.
            }

            // Detected terminal state that we missed during normal polling.
            if (status == trade_status::kCancelled ||
                status == trade_status::kFailed) {
                state_->remove_offer(trade_id);
                removed_ids.push_back(trade_id);
                logger_->warn("[reconcile] Removed orphaned offer {} "
                              "(wallet status={}, was pending in State)",
                              trade_id.substr(0, 12), status);
            }
            // Note: confirmed fills (status==4) are not processed here --
            // they are handled by detect_fills() which now uses
            // include_completed=true and matches against cancel_pending
            // offers still in State.
        }

        offset += kPageSize;
    }

    // Detect orphans: offers in State that no longer exist in the wallet.
    // This can happen if the wallet was restarted or the offer expired
    // server-side without a cancel call.
    for (const auto& [offer_id, po] : pending_map) {
        if (wallet_offer_ids.find(offer_id) == wallet_offer_ids.end()) {
            state_->remove_offer(offer_id);
            removed_ids.push_back(offer_id);
            logger_->warn("[reconcile] Removed phantom offer {} ({}) "
                          "-- not found in wallet",
                          offer_id.substr(0, 12), po.pair_name);
        }
    }

    if (!removed_ids.empty()) {
        logger_->info("[reconcile] Corrected {} state discrepancies",
                      removed_ids.size());
    } else {
        logger_->debug("[reconcile] State consistent -- no discrepancies");
    }

    co_return removed_ids;
}

// ---------------------------------------------------------------------------
// evaluate_orphan -- cost-aware evaluation of an untracked wallet offer
//
// Scholarly basis:
//   Guéant, Lehalle & Fernandez-Tapia (2013) — cancel only when expected
//     adverse selection loss exceeds the cancellation cost.
//   Gao & Wang (2020) — the zero-offer gap during cancel→repost is the
//     primary adverse selection cost for latent market makers.  Keeping a
//     slightly stale offer is cheaper than having no presence.
//   Aït-Sahalia & Saglam (2017) — stale-quote risk = f(price_deviation,
//     remaining_lifetime, offer_size).  Cancellation threshold should
//     scale with all three factors.
//
// The Chia wallet trade record's "summary" field contains:
//   { "offered":   [ [asset_id, amount], ... ],
//     "requested": [ [asset_id, amount], ... ] }
//
// From this we derive the pair, side, price, and size, then compare the
// offer's implied price against the current market mid-price to decide
// whether to adopt (re-track) or cancel (waste a fee but avoid loss).
// ---------------------------------------------------------------------------

OrphanEvaluation OfferManager::evaluate_orphan(
    const json& trade_record,
    BlockHeight current_block,
    const std::unordered_map<std::string, Mojo>& mid_prices) const
{
    OrphanEvaluation eval;
    eval.trade_id = trade_record.value("trade_id", "");

    // ---- Parse summary to extract offered/requested amounts ---------------
    if (!trade_record.contains("summary") ||
        !trade_record["summary"].is_object()) {
        eval.disposition = OrphanDisposition::Unknown;
        eval.reason = "no summary field in trade record";
        return eval;
    }
    const auto& summary = trade_record["summary"];

    // Parse offered and requested asset→amount maps.
    // Chia wallet format: { "asset_id_hex_or_xch": amount_int, ... }
    std::unordered_map<std::string, Mojo> offered;    // we give
    std::unordered_map<std::string, Mojo> requested;  // we get

    auto parse_side = [](const json& obj,
                         std::unordered_map<std::string, Mojo>& out) {
        if (!obj.is_object()) return;
        for (auto& [asset_id, amount_val] : obj.items()) {
            if (amount_val.is_number_integer()) {
                out[asset_id] = amount_val.get<Mojo>();
            } else if (amount_val.is_number_unsigned()) {
                out[asset_id] = static_cast<Mojo>(
                    amount_val.get<std::uint64_t>());
            }
        }
    };

    if (summary.contains("offered"))   parse_side(summary["offered"],   offered);
    if (summary.contains("requested")) parse_side(summary["requested"], requested);

    if (offered.empty() || requested.empty()) {
        eval.disposition = OrphanDisposition::Unknown;
        eval.reason = "empty offered or requested in summary";
        return eval;
    }

    // ---- Match against our configured pairs --------------------------------
    // For each pair, check if the offered/requested asset IDs match.
    // An offer where we GIVE base and GET quote = ask (we sell).
    // An offer where we GIVE quote and GET base = bid (we buy).
    bool matched = false;
    for (const auto& [pname, pcfg] : pair_config_map_) {
        const std::string& base  = pcfg.base_asset_id;
        const std::string& quote = pcfg.quote_asset_id;

        bool offered_base  = offered.count(base)  > 0;
        bool offered_quote = offered.count(quote)  > 0;
        bool requested_base  = requested.count(base) > 0;
        bool requested_quote = requested.count(quote) > 0;

        if (offered_base && requested_quote) {
            // We gave base, got quote → ASK (we sold base).
            eval.pair_name = pname;
            eval.side      = Side::Ask;
            eval.size      = offered.at(base);
            // Price = quote_amount / base_amount (quote mojos per base mojo).
            // But our pricing is in "quote per unit base", scaled to mojos.
            // For XCH-denominated pairs, both sides are in mojos already.
            const double base_d  = static_cast<double>(offered.at(base));
            const double quote_d = static_cast<double>(requested.at(quote));
            eval.price = (base_d > 0.0)
                ? static_cast<Mojo>(std::llround(quote_d / base_d
                    * static_cast<double>(kMojosPerXch)))
                : 0;
            matched = true;
            break;
        }
        if (offered_quote && requested_base) {
            // We gave quote, got base → BID (we bought base).
            eval.pair_name = pname;
            eval.side      = Side::Bid;
            eval.size      = requested.at(base);
            const double base_d  = static_cast<double>(requested.at(base));
            const double quote_d = static_cast<double>(offered.at(quote));
            eval.price = (base_d > 0.0)
                ? static_cast<Mojo>(std::llround(quote_d / base_d
                    * static_cast<double>(kMojosPerXch)))
                : 0;
            matched = true;
            break;
        }
    }

    if (!matched || eval.price <= 0) {
        eval.disposition = OrphanDisposition::Unknown;
        eval.reason = matched ? "could not compute price"
                              : "no matching pair config for asset IDs";
        return eval;
    }

    // ---- Check orphan adoption is enabled ---------------------------------
    if (!strategy_cfg_.orphan_adopt_enabled) {
        eval.disposition = OrphanDisposition::Cancel;
        eval.reason = "orphan_adopt_enabled=false (legacy mode)";
        return eval;
    }

    // ---- Age check --------------------------------------------------------
    BlockHeight age = 0;
    if (current_block > 0 && trade_record.contains("created_at_time")) {
        // Approximate age from timestamps if created_at_time is available
        // but we don't have the creation block height directly.
        // The wallet trade record has "created_at_time" (epoch seconds).
        // Block time is ~52 seconds.
        const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        double created_at = 0.0;
        if (trade_record["created_at_time"].is_number()) {
            created_at = trade_record["created_at_time"].get<double>();
        }
        if (created_at > 0.0) {
            const double age_seconds = static_cast<double>(now_epoch) - created_at;
            age = static_cast<BlockHeight>(std::max(0.0, age_seconds / 52.0));
        }
    }

    if (age > strategy_cfg_.orphan_max_adopt_age_blocks) {
        eval.disposition = OrphanDisposition::Cancel;
        eval.reason = fmt::format("too old: ~{} blocks > max {}",
                                  age, strategy_cfg_.orphan_max_adopt_age_blocks);
        return eval;
    }

    // ---- Price deviation check --------------------------------------------
    auto mid_it = mid_prices.find(eval.pair_name);
    if (mid_it == mid_prices.end() || mid_it->second <= 0) {
        // No market data available for this pair — can't evaluate.
        // Conservatively cancel.
        eval.disposition = OrphanDisposition::Cancel;
        eval.reason = "no current mid-price available for " + eval.pair_name;
        return eval;
    }

    const Mojo mid = mid_it->second;
    const double mid_d   = static_cast<double>(mid);
    const double price_d = static_cast<double>(eval.price);

    // Signed deviation: positive means our price is ABOVE mid.
    const double signed_dev = (mid_d > 0.0) ? (price_d - mid_d) / mid_d : 0.0;
    eval.price_deviation = std::abs(signed_dev);

    // Adverse direction:
    //   BID too high (signed_dev > 0) → overpaying, adverse.
    //   ASK too low  (signed_dev < 0) → underselling, adverse.
    eval.adverse = (eval.side == Side::Bid)
        ? (signed_dev > 0.0)
        : (signed_dev < 0.0);

    // ---- Inventory helper bonus -------------------------------------------
    // Check if this orphan would help reduce inventory imbalance.
    // If we're long the base asset and this is an ask (sell), it helps.
    // If we're short and this is a bid (buy), it helps.
    // We check via the state's position if available.
    double effective_threshold = strategy_cfg_.orphan_adverse_threshold;
    {
        auto pending = state_->get_all_offers();
        int bid_count = 0, ask_count = 0;
        for (const auto& po : pending) {
            if (po.pair_name == eval.pair_name) {
                if (po.side == Side::Bid) ++bid_count;
                else ++ask_count;
            }
        }
        // If we have more bids than asks, we're accumulating → asks help.
        // If we have more asks than bids, we're depleting → bids help.
        const bool helps_inventory =
            (bid_count > ask_count && eval.side == Side::Ask) ||
            (ask_count > bid_count && eval.side == Side::Bid);
        eval.inventory_helper = helps_inventory;
        if (helps_inventory) {
            effective_threshold += strategy_cfg_.orphan_inventory_bonus;
        }
    }

    // ---- Cost-aware cancel/keep decision ----------------------------------
    //
    // From Gao & Wang (2020), the optimal decision is:
    //   cancel if expected_adverse_loss > cancel_cost
    //
    // expected_adverse_loss = deviation × size × adverse_fill_prob
    // cancel_cost = fee + liquidity_opportunity_cost
    //
    // For simplicity and robustness, we use the threshold approach:
    //   - Non-adverse deviation (favorable): always adopt.
    //   - Adverse but within threshold: adopt (cost to cancel > likely loss).
    //   - Adverse beyond threshold: cancel (likely loss > cancel cost).
    //   - Mild adverse (between half-threshold and threshold): adopt-stale.
    eval.cancel_cost = static_cast<Mojo>(current_fee_mojos_);

    if (!eval.adverse) {
        // Favorable deviation — our offer is more conservative than
        // the current optimal.  Safe to keep: a bid below mid is fine,
        // an ask above mid is fine.
        eval.disposition = OrphanDisposition::Adopt;
        eval.reason = fmt::format("favorable: {} {:.2f}% from mid on {}",
                                  (eval.side == Side::Bid) ? "BID" : "ASK",
                                  eval.price_deviation * 100.0,
                                  eval.pair_name);
    } else if (eval.price_deviation <= effective_threshold * 0.5) {
        // Small adverse deviation — well within tolerance.
        eval.disposition = OrphanDisposition::Adopt;
        eval.reason = fmt::format("small adverse {:.2f}% < {:.1f}% half-threshold",
                                  eval.price_deviation * 100.0,
                                  effective_threshold * 50.0);
    } else if (eval.price_deviation <= effective_threshold) {
        // Moderate adverse — cheaper to keep than cancel, but flag
        // for early refresh on the next heartbeat.
        eval.disposition = OrphanDisposition::AdoptStale;
        eval.expected_loss = static_cast<Mojo>(std::llround(
            eval.price_deviation * static_cast<double>(eval.size)));
        eval.reason = fmt::format("moderate adverse {:.2f}% <= {:.1f}% threshold, "
                                  "adopt-stale{}",
                                  eval.price_deviation * 100.0,
                                  effective_threshold * 100.0,
                                  eval.inventory_helper ? " (inventory helper)" : "");
    } else {
        // Large adverse deviation — expected loss exceeds cancel cost.
        eval.expected_loss = static_cast<Mojo>(std::llround(
            eval.price_deviation * static_cast<double>(eval.size)));
        eval.disposition = OrphanDisposition::Cancel;
        eval.reason = fmt::format("adverse {:.2f}% > {:.1f}% threshold, "
                                  "expected_loss={} > cancel_cost={}",
                                  eval.price_deviation * 100.0,
                                  effective_threshold * 100.0,
                                  eval.expected_loss, eval.cancel_cost);
    }

    return eval;
}

// ---------------------------------------------------------------------------
// startup_reconcile -- scan wallet for orphaned offers on startup
//
// Enhanced with cost-aware orphan evaluation (CAOE): instead of blindly
// cancelling all orphans, evaluates each one against the current market
// mid-price and adopts well-priced orphans to save fees and preserve
// market presence.
//
// Scholarly basis:
//   Guéant, Lehalle & Fernandez-Tapia (2013) — cost-aware cancellation
//   Gao & Wang (2020) — zero-offer gap avoidance for latent market makers
//   Aït-Sahalia & Saglam (2017) — stale-quote risk scaling
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<std::string>> OfferManager::startup_reconcile(
    const std::unordered_set<std::string>& known_offer_ids,
    BlockHeight current_block)
{
    std::vector<std::string> cancelled_ids;

    logger_->info("[startup_reconcile] Scanning wallet for orphaned offers...");

    // ---- Phase 1: Collect all PENDING_ACCEPT offers from wallet -----------
    struct WalletOffer {
        std::string trade_id;
        json        record;
        bool        known;  // true if in our DB
    };
    std::vector<WalletOffer> wallet_offers;

    constexpr std::int64_t kPageSize = 50;
    std::int64_t offset = 0;
    bool more = true;

    while (more) {
        std::vector<json> trade_records;
        try {
            trade_records = co_await wallet_->get_all_offers(
                offset, offset + kPageSize, /*file_contents=*/false);
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("[startup_reconcile] get_all_offers failed: {}",
                           e.what());
            co_return cancelled_ids;
        }

        if (trade_records.empty() ||
            static_cast<std::int64_t>(trade_records.size()) < kPageSize) {
            more = false;
        }

        for (auto& rec : trade_records) {
            if (!rec.contains("trade_id") || !rec.contains("status")) {
                continue;
            }
            std::string trade_id = rec["trade_id"].get<std::string>();
            int status = trade_status::parse(rec["status"]);

            if (status != trade_status::kPendingAccept) {
                continue;
            }

            wallet_offers.push_back(WalletOffer{
                trade_id,
                std::move(rec),
                known_offer_ids.count(trade_id) > 0
            });
        }

        offset += kPageSize;
    }

    // Separate known from orphans.
    std::size_t total_pending = wallet_offers.size();
    std::vector<WalletOffer*> orphans;
    std::size_t restored = 0;
    for (auto& wo : wallet_offers) {
        if (wo.known) {
            ++restored;
        } else {
            orphans.push_back(&wo);
        }
    }

    if (orphans.empty()) {
        logger_->info("[startup_reconcile] Complete: {} wallet offers scanned, "
                      "{} known/restored, 0 orphans",
                      total_pending, restored);
        co_return cancelled_ids;
    }

    // ---- Phase 2: Fetch mid-prices for orphan evaluation ------------------
    // Build the set of pairs that orphans might belong to, then fetch
    // current dexie ticker prices for cost-aware evaluation.
    std::unordered_map<std::string, Mojo> mid_prices;

    if (strategy_cfg_.orphan_adopt_enabled && dexie_client_) {
        // Fetch ticker for each enabled pair.
        for (const auto& [pname, pcfg] : pair_config_map_) {
            if (!pcfg.enabled) continue;
            try {
                auto ticker = co_await dexie_client_->get_ticker(
                    pcfg.base_asset_id, pcfg.quote_asset_id);
                if (ticker.has_value()) {
                    // Mid = (buy + sell) / 2, in XCH mojos.
                    const double mid = (ticker->price_buy + ticker->price_sell) / 2.0;
                    if (mid > 0.0) {
                        mid_prices[pname] = static_cast<Mojo>(std::llround(
                            mid * static_cast<double>(kMojosPerXch)));
                    }
                }
            } catch (const std::exception& e) {
                logger_->warn("[startup_reconcile] Failed to fetch ticker for "
                              "{}: {}", pname, e.what());
            }
        }
        logger_->info("[startup_reconcile] Fetched mid-prices for {} pairs",
                      mid_prices.size());
    }

    // ---- Phase 3: Evaluate each orphan ------------------------------------
    std::size_t adopted = 0, adopted_stale = 0, cancelled = 0, unknown = 0;

    for (auto* wo : orphans) {
        OrphanEvaluation eval = evaluate_orphan(
            wo->record, current_block, mid_prices);

        switch (eval.disposition) {
            case OrphanDisposition::Adopt:
            case OrphanDisposition::AdoptStale: {
                // Re-register this orphan as a tracked PendingOffer.
                PendingOffer po;
                po.offer_id        = wo->trade_id;
                po.pair_name       = eval.pair_name;
                po.side            = eval.side;
                po.price           = eval.price;
                po.size            = eval.size;
                po.tier            = 0;  // Unknown original tier — assign 0.
                po.fee_mojos       = current_fee_mojos_;
                // Approximate creation block from age.
                if (current_block > 0 && wo->record.contains("created_at_time")) {
                    const auto now_epoch = std::chrono::duration_cast<
                        std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                    double created_at = 0.0;
                    if (wo->record["created_at_time"].is_number()) {
                        created_at = wo->record["created_at_time"].get<double>();
                    }
                    if (created_at > 0.0) {
                        const BlockHeight approx_age = static_cast<BlockHeight>(
                            std::max(0.0,
                                (static_cast<double>(now_epoch) - created_at)
                                    / 52.0));
                        po.created_at_block = (current_block > approx_age)
                            ? (current_block - approx_age) : 0;
                    }
                }
                // For AdoptStale, assign a creation block that makes it
                // look older (closer to TTL), triggering early refresh.
                if (eval.disposition == OrphanDisposition::AdoptStale) {
                    // Set created_at_block to (current - ttl + 5) so it
                    // will be classified as Expired on next heartbeat,
                    // triggering an immediate selective refresh.
                    const BlockHeight ttl = strategy_cfg_.offer_ttl_blocks;
                    if (current_block > ttl && ttl > 5) {
                        po.created_at_block = current_block - ttl + 5;
                    }
                }
                state_->upsert_offer(po);

                if (eval.disposition == OrphanDisposition::Adopt) {
                    ++adopted;
                    logger_->info("[startup_reconcile] ADOPTED orphan {} "
                                  "({} {} on {} @ {} mojos) — {}",
                                  wo->trade_id.substr(0, 24),
                                  (eval.side == Side::Bid) ? "BID" : "ASK",
                                  eval.size, eval.pair_name, eval.price,
                                  eval.reason);
                } else {
                    ++adopted_stale;
                    logger_->info("[startup_reconcile] ADOPTED-STALE orphan {} "
                                  "({} {} on {} @ {} mojos) — {}",
                                  wo->trade_id.substr(0, 24),
                                  (eval.side == Side::Bid) ? "BID" : "ASK",
                                  eval.size, eval.pair_name, eval.price,
                                  eval.reason);
                }
                break;
            }

            case OrphanDisposition::Cancel:
            case OrphanDisposition::Unknown: {
                // Cancel this orphan on-chain.
                const char* label = (eval.disposition == OrphanDisposition::Unknown)
                    ? "UNKNOWN" : "CANCEL";
                logger_->warn("[startup_reconcile] {} orphan {} — {}",
                              label, wo->trade_id.substr(0, 24), eval.reason);

                bool cancel_ok = false;
                bool needs_emergency = false;
                try {
                    co_await wallet_->cancel_offer(wo->trade_id,
                                                   current_fee_mojos_,
                                                   /*secure=*/true);
                    cancel_ok = true;
                } catch (const rpc::ChiaRPCError& e) {
                    const std::string_view msg{e.what()};
                    needs_emergency =
                        msg.find("insufficient funds") != std::string_view::npos ||
                        msg.find("spendable balance") != std::string_view::npos;
                    if (!needs_emergency) {
                        logger_->error("[startup_reconcile] Failed to cancel "
                                       "{}: {}", wo->trade_id.substr(0, 24),
                                       e.what());
                    }
                } catch (const std::exception& e) {
                    logger_->error("[startup_reconcile] Failed to cancel "
                                   "{}: {}", wo->trade_id.substr(0, 24),
                                   e.what());
                }
                if (needs_emergency)
                    cancel_ok = co_await emergency_cancel(
                        wo->trade_id, "startup_reconcile");
                if (cancel_ok) {
                    cancelled_ids.push_back(wo->trade_id);
                    if (eval.disposition == OrphanDisposition::Cancel)
                        ++cancelled;
                    else
                        ++unknown;
                }
                break;
            }
        }
    }

    logger_->info("[startup_reconcile] Complete: {} wallet offers, "
                  "{} known/restored, {} adopted, {} adopted-stale, "
                  "{} cancelled, {} unknown-cancelled",
                  total_pending, restored, adopted, adopted_stale,
                  cancelled, unknown);

    co_return cancelled_ids;
}

// ---------------------------------------------------------------------------
// prune_stuck_transactions -- detect and clear stuck wallet transactions
// ---------------------------------------------------------------------------

asio::awaitable<int> OfferManager::prune_stuck_transactions(
    const std::vector<std::int64_t>& wallet_ids,
    std::int64_t max_age_seconds)
{
    int wallets_pruned = 0;
    const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (const auto wid : wallet_ids) {
        try {
            auto txs = co_await wallet_->get_transactions(wid, 0, 200);

            int stuck_count = 0;
            for (const auto& tx : txs) {
                // Only examine unconfirmed transactions.
                if (tx.contains("confirmed") && tx["confirmed"].get<bool>()) {
                    continue;
                }

                // Check age.
                if (!tx.contains("created_at_time")) continue;
                auto created = tx["created_at_time"].get<std::int64_t>();
                auto age = now_epoch - created;

                // Transactions without a spend bundle are stuck immediately
                // after max_age_seconds (wallet failed to build the bundle).
                // Transactions WITH a spend bundle that remain unconfirmed
                // past 3x max_age_seconds (default 30 min) are also stuck:
                // the mempool drops transactions after ~5 minutes, so if
                // they haven't confirmed after 30 min they never will.
                bool has_bundle = tx.contains("spend_bundle") &&
                                  !tx["spend_bundle"].is_null();
                std::int64_t threshold = has_bundle
                    ? max_age_seconds * 3   // 30 min for broadcast-but-dropped
                    : max_age_seconds;       // 10 min for never-broadcast

                if (age < threshold) continue;

                ++stuck_count;

                // Log details for the first few stuck transactions.
                if (stuck_count <= 5) {
                    std::int64_t amount = 0;
                    int tx_type = -1;
                    std::string tx_name = "(unknown)";
                    if (tx.contains("amount")) amount = tx["amount"].get<std::int64_t>();
                    if (tx.contains("type"))   tx_type = tx["type"].get<int>();
                    if (tx.contains("name"))   tx_name = tx["name"].get<std::string>();
                    logger_->warn("[prune_stuck_tx] wallet {} stuck tx: "
                                  "type={} amount={} age={}s bundle={} "
                                  "name={}",
                                  wid, tx_type, amount, age,
                                  has_bundle ? "yes" : "no", tx_name);
                }
            }

            if (stuck_count > 0) {
                logger_->warn("[prune_stuck_tx] wallet {} has {} stuck "
                              "transactions (no spend bundle, age > {}s) "
                              "-- clearing unconfirmed",
                              wid, stuck_count, max_age_seconds);
                co_await wallet_->delete_unconfirmed_transactions(wid);
                ++wallets_pruned;
                logger_->info("[prune_stuck_tx] wallet {} unconfirmed "
                              "transactions cleared", wid);
            }
        } catch (const std::exception& e) {
            logger_->error("[prune_stuck_tx] wallet {} failed: {}",
                           wid, e.what());
        }
    }

    if (wallets_pruned > 0) {
        logger_->info("[prune_stuck_tx] Pruned stuck transactions from "
                      "{} wallet(s)", wallets_pruned);
    }

    co_return wallets_pruned;
}

// ---------------------------------------------------------------------------
// build_offer_dict -- map a TierQuote to the wallet RPC offer_dict format
// ---------------------------------------------------------------------------

json OfferManager::build_offer_dict(const PairConfig& pair,
                                    const TierQuote&  tier) const
{
    // Resolve both asset IDs to wallet IDs.
    const std::int64_t base_wid  = resolve_wallet_id(pair.base_asset_id);
    const std::int64_t quote_wid = resolve_wallet_id(pair.quote_asset_id);

    if (base_wid < 0 || quote_wid < 0) {
        logger_->error("Cannot resolve wallet IDs: base='{}' (wid={}), "
                       "quote='{}' (wid={})",
                       pair.base_asset_id, base_wid,
                       pair.quote_asset_id, quote_wid);
        return json::object();
    }

    // Resolve the per-asset mojo denomination from the pair configuration.
    // XCH uses 10^12 mojos per unit; CAT tokens use 10^3 mojos per unit.
    //
    // ISO/IEC 5055: guard against zero/negative denomination to prevent
    // division-by-zero undefined behaviour or nonsensical mojo amounts.
    const std::int64_t quote_denom = pair.quote_mojos_per_unit;
    const std::int64_t base_denom  = pair.base_mojos_per_unit;
    if (quote_denom <= 0 || base_denom <= 0) {
        logger_->error("build_offer_dict: invalid mojos_per_unit "
                       "(base={}, quote={}) for pair '{}' -- must be > 0",
                       base_denom, quote_denom, pair.name);
        return json::object();
    }

    // tier.size  is in base-asset mojos.
    // tier.price is the exchange rate (quote-per-base units) scaled by
    //            kMojosPerXch (the engine multiplies the strategy's
    //            floating-point price by kMojosPerXch for fixed-point
    //            storage in the Quote struct).
    //
    // To compute the quote-asset mojo amount:
    //   base_units  = tier.size  / base_mojos_per_unit
    //   price_real  = tier.price / kMojosPerXch
    //   quote_units = base_units * price_real
    //   quote_mojos = quote_units * quote_mojos_per_unit
    //
    // Combined:
    //   quote_mojos = tier.size * tier.price * quote_denom
    //               / (base_denom * kMojosPerXch)
    //
    // We compute in double to avoid int64 overflow (the numerator can
    // reach ~10^25 for typical XCH/CAT pairs).
    const double size_d  = static_cast<double>(tier.size);
    const double price_d = static_cast<double>(tier.price);
    const double denom   = static_cast<double>(base_denom)
                         * static_cast<double>(kMojosPerXch);

    json offer_dict = json::object();

    if (tier.side == Side::Bid) {
        // BID: we offer quote-asset mojos (negative), request base-asset
        // mojos (positive).
        // Round up (ceil) so that we offer at least enough quote to cover
        // the requested base amount at the stated price.
        const Mojo quote_amount = static_cast<Mojo>(
            std::ceil(size_d * price_d
                      * static_cast<double>(quote_denom) / denom));

        // Wallet RPC convention: negative = we spend, positive = we receive.
        offer_dict[std::to_string(quote_wid)] = -quote_amount;
        offer_dict[std::to_string(base_wid)]  =  tier.size;

    } else {
        // ASK: we offer base-asset mojos (negative), request quote-asset
        // mojos (positive).
        // Round down (floor) so that we request conservatively, ensuring
        // the offer is attractive to takers.
        const Mojo quote_amount = static_cast<Mojo>(
            std::floor(size_d * price_d
                       * static_cast<double>(quote_denom) / denom));

        offer_dict[std::to_string(base_wid)]  = -tier.size;
        offer_dict[std::to_string(quote_wid)] =  quote_amount;
    }

    return offer_dict;
}

// ---------------------------------------------------------------------------
// [T7-10] post_merged_side -- merge same-side tiers into one RPC call
// ---------------------------------------------------------------------------

asio::awaitable<int> OfferManager::post_merged_side(
    const PairConfig&              pair,
    const std::vector<TierQuote>&  tiers,
    BlockHeight                    block_height)
{
    if (tiers.empty()) co_return 0;

    // Build individual offer_dicts and merge by summing wallet_id amounts.
    json merged_dict = json::object();
    for (const auto& tier : tiers) {
        json single = build_offer_dict(pair, tier);
        if (single.empty()) {
            logger_->warn("Batch: skipping tier {} {} -- could not build dict",
                          tier.tier_index, to_string(tier.side));
            continue;
        }
        for (auto& [key, val] : single.items()) {
            if (merged_dict.contains(key)) {
                merged_dict[key] = merged_dict[key].get<std::int64_t>()
                                 + val.get<std::int64_t>();
            } else {
                merged_dict[key] = val;
            }
        }
    }

    if (merged_dict.empty()) co_return 0;

    // Create the merged offer via RPC.
    // co_await cannot appear inside a catch handler in a C++20 coroutine,
    // so capture any exception info here and perform the fallback after the
    // try/catch block.
    json result;
    bool batch_failed = false;
    std::string batch_err;
    try {
        result = co_await wallet_->create_offer(
            merged_dict, current_fee_mojos_, /*validate_only=*/false);
    } catch (const rpc::ChiaRPCError& e) {
        batch_failed = true;
        batch_err = e.what();
    }

    if (batch_failed) {
        // Fallback: if batch fails, fall through to individual creation.
        logger_->warn("Batch create_offer failed for {} {} -- "
                      "falling back to individual: {}",
                      pair.name, to_string(tiers.front().side), batch_err);
        int fallback_count = 0;
        for (const auto& tier : tiers) {
            json single_dict = build_offer_dict(pair, tier);
            if (single_dict.empty()) continue;
            bool tier_failed = false;
            std::string tier_err;
            json sr;
            try {
                sr = co_await wallet_->create_offer(
                    single_dict, current_fee_mojos_, /*validate_only=*/false);
            } catch (const rpc::ChiaRPCError& e2) {
                tier_failed = true;
                tier_err = e2.what();
            }
            if (tier_failed) {
                logger_->error("Fallback create_offer failed for {} tier {}: {}",
                               pair.name, tier.tier_index, tier_err);
                continue;
            }
            if (sr.contains("offer") && sr["offer"].is_string()
                && sr.contains("trade_record")
                && sr["trade_record"].contains("trade_id")
                && sr["trade_record"]["trade_id"].is_string()) {
                std::string offer_text = sr["offer"].get<std::string>();
                co_await submit_to_dexie(offer_text);
                PendingOffer po;
                po.offer_id         = sr["trade_record"]["trade_id"].get<std::string>();
                po.pair_name        = pair.name;
                po.side             = tier.side;
                po.price            = tier.price;
                po.size             = tier.size;
                po.tier             = tier.tier_index;
                po.created_at_block = block_height;
                po.created_at_ts    = std::chrono::system_clock::now();
                state_->upsert_offer(po);
                ++fallback_count;

                // -- Fee reserve guard (batch fallback, UTXO-aware) ---------
                // Each individual creation in the fallback can lock UTXOs and
                // eat into the fee reserve.  Check after each success.
                if (strategy_cfg_.fee_reserve_xch > 0.0) {
                    try {
                        auto fb_bal = co_await wallet_->get_wallet_balance(1);
                        Mojo fb_spendable = 0;
                        if (fb_bal.contains("spendable_balance"))
                            fb_spendable = fb_bal["spendable_balance"].get<Mojo>();
                        const auto fb_reserve = static_cast<Mojo>(std::llround(
                            strategy_cfg_.fee_reserve_xch
                            * static_cast<double>(kMojosPerXch)));
                        if (fb_spendable < fb_reserve) {
                            logger_->warn(
                                "XCH fee reserve guard (batch fallback): "
                                "spendable {:.6f} XCH < reserve {:.3f} XCH "
                                "after {} tier {} -- stopping fallback",
                                static_cast<double>(fb_spendable) / kMojosPerXch,
                                strategy_cfg_.fee_reserve_xch,
                                pair.name, tier.tier_index);
                            break;
                        }
                    } catch (const std::exception& e) {
                        logger_->warn("XCH fee reserve guard (batch fallback): "
                                      "balance check failed: {}", e.what());
                    }
                }
            }
        }
        co_return fallback_count;
    }

    // Extract trade_id and offer text.
    if (!result.contains("offer") || !result["offer"].is_string()
        || !result.contains("trade_record")
        || !result["trade_record"].contains("trade_id")) {
        logger_->error("Batch: create_offer response missing fields for {} {}",
                       pair.name, to_string(tiers.front().side));
        co_return 0;
    }

    std::string trade_id   = result["trade_record"]["trade_id"].get<std::string>();
    std::string offer_text = result["offer"].get<std::string>();

    // Submit to dexie (best-effort).
    co_await submit_to_dexie(offer_text);

    // Track all constituent tiers with the same offer_id.
    for (const auto& tier : tiers) {
        PendingOffer pending;
        pending.offer_id         = trade_id;
        pending.pair_name        = pair.name;
        pending.side             = tier.side;
        pending.price            = tier.price;
        pending.size             = tier.size;
        pending.tier             = tier.tier_index;
        pending.created_at_block = block_height;
        pending.created_at_ts    = std::chrono::system_clock::now();
        pending.fee_mojos        = current_fee_mojos_;
        state_->upsert_offer(pending);
    }

    logger_->info("Batch: posted {} {} ({} tiers merged) [{}]",
                  pair.name, to_string(tiers.front().side),
                  tiers.size(), trade_id.substr(0, 12));

    co_return static_cast<int>(tiers.size());
}

// ---------------------------------------------------------------------------
// submit_to_dexie -- post offer text to the dexie aggregator (best-effort)
// ---------------------------------------------------------------------------

asio::awaitable<bool> OfferManager::submit_to_dexie(
    const std::string& offer_text)
{
    // Best-effort submission to the Dexie aggregator for cross-platform
    // visibility.  The offer is already valid on-chain regardless of
    // whether Dexie accepts it, so failures are non-fatal.
    //
    // ISO/IEC 27001:2022: full offer bech32m payload is never logged;
    // DexieClient internally truncates the payload in log output.
    // ISO/IEC 5055: null-pointer guard prevents UB if client is absent.

    // Guard: if no DexieClient was injected, skip silently.
    if (!dexie_client_) {
        logger_->debug("submit_to_dexie: no DexieClient configured -- "
                       "skipping aggregator submission");
        co_return false;
    }

    try {
        // Ensure the client session is open before posting.
        if (!dexie_client_->is_open()) {
            dexie_client_->open();
        }

        // POST /v1/offers with JSON body {"offer": "<bech32m text>"}.
        // DexieClient::submit_offer handles rate limiting, retries on
        // 429/5xx, and JSON parsing internally.
        rpc::SubmitResult result = co_await dexie_client_->submit_offer(
            offer_text, dexie_cfg_.claim_rewards);

        if (result.success) {
            logger_->info("submit_to_dexie: accepted (dexie_id={})",
                          result.offer_id);
            co_return true;
        }

        // Dexie rejected the offer (e.g. duplicate, invalid, expired).
        // Log the reason but do not treat as a hard failure.
        logger_->warn("submit_to_dexie: rejected by Dexie -- {}",
                      result.error_message);
        co_return false;

    } catch (const rpc::DexieRateLimitError& e) {
        // Rate-limit exhaustion after max retries.  Non-fatal: the offer
        // remains valid on-chain; aggregator visibility is best-effort.
        logger_->warn("submit_to_dexie: rate-limited -- {}", e.what());
        co_return false;
    } catch (const rpc::DexieClientError& e) {
        // Non-retryable 4xx error (bad request, invalid offer format, etc.).
        logger_->warn("submit_to_dexie: client error -- {}", e.what());
        co_return false;
    } catch (const rpc::DexieServerError& e) {
        // Server-side 5xx that persisted after retries.
        logger_->warn("submit_to_dexie: server error -- {}", e.what());
        co_return false;
    } catch (const rpc::DexieError& e) {
        // Catch-all for any other DexieClient transport error
        // (curl failure, JSON parse error, client not open, etc.).
        logger_->warn("submit_to_dexie: transport error -- {}", e.what());
        co_return false;
    } catch (const std::exception& e) {
        // Defensive catch for unexpected exceptions.  Should not fire in
        // normal operation, but prevents an unhandled exception from
        // propagating and crashing the offer lifecycle coroutine.
        logger_->error("submit_to_dexie: unexpected exception -- {}",
                       e.what());
        co_return false;
    }
}

// ---------------------------------------------------------------------------
// resolve_wallet_id -- look up a wallet ID from an asset ID
// ---------------------------------------------------------------------------

std::int64_t OfferManager::resolve_wallet_id(const AssetId& asset_id) const
{
    // Native XCH is always wallet_id 1.
    if (asset_id == "xch" || asset_id == "XCH") {
        return 1;
    }

    auto it = wallet_id_map_.find(asset_id);
    if (it != wallet_id_map_.end()) {
        return it->second;
    }

    return -1;  // Not found.
}

// ---------------------------------------------------------------------------
// init_wallet_id_map -- one-time cache population from the wallet RPC
// ---------------------------------------------------------------------------

asio::awaitable<void> OfferManager::init_wallet_id_map()
{
    logger_->info("Initialising wallet ID map from wallet RPC...");

    try {
        auto wallets = co_await wallet_->get_wallets();

        for (const auto& w : wallets) {
            // Each wallet record contains "id" (int) and "data" (asset ID
            // for CAT wallets).  Type 6 = CAT wallet.
            if (!w.contains("id")) {
                continue;
            }

            std::int64_t wid = w["id"].get<std::int64_t>();

            // Type 6 is CAT_WALLET in the Chia wallet enumeration.
            if (w.contains("type") && w["type"].get<int>() == 6 &&
                w.contains("data")) {
                std::string asset_id = w["data"].get<std::string>();

                std::transform(asset_id.begin(), asset_id.end(), asset_id.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });

                // Wallet RPC returns CAT asset IDs with a trailing "00"
                // suffix in this field; normalize back to the canonical
                // 64-hex asset ID used throughout config and Dexie.
                if (asset_id.size() == 66 &&
                    asset_id.compare(asset_id.size() - 2, 2, "00") == 0) {
                    asset_id.resize(64);
                }

                wallet_id_map_[asset_id] = wid;
                logger_->debug("Mapped asset {} -> wallet_id {}", asset_id,
                               wid);
            }
        }

        wallet_ids_resolved_ = true;
        logger_->info("Wallet ID map initialised: {} CAT wallets found",
                      wallet_id_map_.size());
    } catch (const rpc::ChiaRPCError& e) {
        logger_->error("Failed to initialise wallet ID map: {}", e.what());
        // Allow retry on next post_quotes() call by leaving the flag false.
    }
}

// ---------------------------------------------------------------------------
// emergency_cancel -- reduced/zero-fee cancel fallback
// ---------------------------------------------------------------------------

asio::awaitable<bool> OfferManager::emergency_cancel(
    const std::string& offer_id,
    const std::string& context,
    bool prefer_zero_fee)
{
    try {
        // When prefer_zero_fee is set (UTXO liberation), try the fee=0
        // secure cancel FIRST so we don't burn spendable XCH on fees.
        if (prefer_zero_fee) {
            logger_->warn("{}: attempting zero-fee secure cancel for {}",
                          context, offer_id.substr(0, 12));
            bool zero_ok = false;
            try {
                co_await wallet_->cancel_offer(
                    offer_id, 0, /*secure=*/true);
                zero_ok = true;
            } catch (const std::exception& e) {
                logger_->debug("{}: zero-fee secure cancel failed for {}: {}",
                               context, offer_id.substr(0, 12), e.what());
            }
            if (zero_ok) {
                logger_->info("{}: secure-cancelled {} with fee=0",
                              context, offer_id.substr(0, 12));
                co_return true;
            }
            // Fall through to descending fee loop.
        }

        auto xch_bal = co_await wallet_->get_wallet_balance(1);
        Mojo xch_spendable = 0;
        if (xch_bal.contains("spendable_balance"))
            xch_spendable = xch_bal["spendable_balance"].get<Mojo>();

        if (xch_spendable > 0) {
            // Try descending fee tiers: 2× dynamic fee, 1× dynamic fee,
            // half, quarter, down to 1 mojo.  If the wallet reports
            // insufficient funds at a given tier, halve and retry.
            // This lets us cancel even when spendable is far below the
            // configured minimum fee.
            const auto fee_cap = static_cast<Mojo>(current_fee_mojos_ * 2);
            Mojo attempt_fee = std::min(
                fee_cap,
                std::max(Mojo{1}, xch_spendable - Mojo{1000}));

            while (attempt_fee >= 1) {
                logger_->warn("{}: emergency cancel {} with fee "
                              "{} mojos (spendable {} mojos)",
                              context, offer_id.substr(0, 12),
                              attempt_fee, xch_spendable);
                bool rpc_failed = false;
                bool insufficient = false;
                try {
                    co_await wallet_->cancel_offer(
                        offer_id,
                        static_cast<std::uint64_t>(attempt_fee),
                        /*secure=*/true);
                } catch (const rpc::ChiaRPCError& e) {
                    rpc_failed = true;
                    const std::string_view msg{e.what()};
                    insufficient =
                        msg.find("insufficient funds")
                            != std::string_view::npos ||
                        msg.find("spendable balance")
                            != std::string_view::npos;
                    if (!insufficient) {
                        logger_->error(
                            "{}: emergency cancel RPC error for {}: {}",
                            context, offer_id.substr(0, 12), e.what());
                    }
                }
                if (!rpc_failed) {
                    logger_->info(
                        "{}: emergency-cancelled {} (fee {} mojos)",
                        context, offer_id.substr(0, 12), attempt_fee);
                    co_return true;
                }
                if (!insufficient) break;  // non-balance error, stop retrying
                // Halve the fee and retry.
                attempt_fee = attempt_fee / 2;
            }
        }

        // Zero spendable or all fee tiers exhausted: try secure cancel
        // with fee=0.  The offer's locked coins serve as the spend
        // bundle inputs, so no additional spendable XCH is needed.
        // Skip if prefer_zero_fee already tried this at the top.
        if (!prefer_zero_fee) {
            logger_->warn("{}: attempting secure cancel with fee=0 for {}",
                          context, offer_id.substr(0, 12));
            bool secure_zero_ok = false;
            try {
                co_await wallet_->cancel_offer(
                    offer_id, 0, /*secure=*/true);
                secure_zero_ok = true;
            } catch (const rpc::ChiaRPCError& e) {
                logger_->debug("{}: secure cancel fee=0 failed for {}: {}",
                               context, offer_id.substr(0, 12), e.what());
            } catch (const std::exception& e) {
                logger_->debug("{}: secure cancel fee=0 exception for {}: {}",
                               context, offer_id.substr(0, 12), e.what());
            }
            if (secure_zero_ok) {
                logger_->info("{}: secure-cancelled {} with fee=0",
                              context, offer_id.substr(0, 12));
                co_return true;
            }
        }

        // Last resort: local-only cancel.
        // No on-chain fee -- drops from wallet but doesn't invalidate the
        // offer on-chain.  Better than leaving funds locked forever.
        logger_->warn("{}: zero XCH spendable -- local-only (insecure) "
                      "cancel for {}", context, offer_id.substr(0, 12));
        co_await wallet_->cancel_offer(
            offer_id, 0, /*secure=*/false);
        logger_->warn("{}: local-only cancelled {} "
                      "(INSECURE -- offer may still be taken on-chain)",
                      context, offer_id.substr(0, 12));
        co_return true;
    } catch (const std::exception& e) {
        logger_->error("{}: emergency cancel also failed for {}: {}",
                       context, offer_id.substr(0, 12), e.what());
    }
    co_return false;
}

}  // namespace xop::execution
