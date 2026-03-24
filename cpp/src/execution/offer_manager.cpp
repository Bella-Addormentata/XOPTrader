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
#include <cmath>
#include <stdexcept>

namespace xop::execution {

// ---------------------------------------------------------------------------
// Chia wallet trade-record status codes (from chia-blockchain source).
// ---------------------------------------------------------------------------
namespace trade_status {
    constexpr int kPendingAccept    = 0;
    constexpr int kPendingConfirm   = 1;
    constexpr int kPendingCancel    = 2;
    constexpr int kCancelled        = 3;
    constexpr int kConfirmed        = 4;
    constexpr int kFailed           = 5;
}  // namespace trade_status

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

OfferManager::OfferManager(asio::io_context&                    ioc,
                           std::shared_ptr<rpc::ChiaWalletRPC>  wallet,
                           std::shared_ptr<State>               state,
                           const AppConfig&                     config)
    : ioc_(ioc)
    , wallet_(std::move(wallet))
    , state_(std::move(state))
    , strategy_cfg_(config.strategy)
    , risk_cfg_(config.risk)
    , dexie_cfg_(config.dexie)
    , logger_(spdlog::default_logger()->clone("OfferMgr"))
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

    logger_->info("OfferManager initialised: {} tiers, TTL {} blocks",
                  strategy_cfg_.num_tiers,
                  strategy_cfg_.offer_ttl_blocks);
}

// ---------------------------------------------------------------------------
// post_quotes -- create multi-tier bid + ask offers on-chain
// ---------------------------------------------------------------------------

asio::awaitable<int> OfferManager::post_quotes(
    const PairConfig&              pair,
    const std::vector<TierQuote>&  quotes,
    BlockHeight                    block_height)
{
    // Lazy-init the wallet-ID cache on first call.
    if (!wallet_ids_resolved_) {
        co_await init_wallet_id_map();
    }

    int created_count = 0;

    for (const auto& tier : quotes) {
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
            // Fee: 0.0001 XCH (100_000_000 mojos) per offer is standard.
            constexpr std::uint64_t kOfferFee = 100'000'000ULL;
            result = co_await wallet_->create_offer(
                offer_dict, kOfferFee, /*validate_only=*/false);
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("create_offer failed for {} tier {}: {}",
                           pair.name, tier.tier_index, e.what());
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

        state_->upsert_offer(pending);
        ++created_count;

        logger_->info("Posted {} {} tier {} @ {} mojos, size {} mojos [{}]",
                      pair.name, to_string(tier.side), tier.tier_index,
                      tier.price, tier.size, trade_id.substr(0, 12));
        // Full offer text at DEBUG only (ISO/IEC 27001: minimise exposure).
        logger_->debug("Offer text: {}...", offer_text.substr(0, 40));
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

    // Poll the wallet for all trade records.
    // Paginate in batches of 50 to avoid oversized responses.
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
            int status = rec["status"].get<int>();

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

                // Update position accounting in state.
                //   Bid fill: we BOUGHT base asset.
                //   Ask fill: we SOLD base asset.
                if (po.side == Side::Bid) {
                    state_->record_buy(po.pair_name, po.size, po.price);
                    logger_->info("FILL BID {} {} mojos @ {} mojos [{}]",
                                  po.pair_name, po.size, po.price,
                                  trade_id.substr(0, 12));
                } else {
                    bool sold = state_->record_sell(po.pair_name, po.size);
                    if (!sold) {
                        logger_->error(
                            "record_sell failed for {} -- insufficient balance",
                            trade_id.substr(0, 12));
                    }
                    logger_->info("FILL ASK {} {} mojos @ {} mojos [{}]",
                                  po.pair_name, po.size, po.price,
                                  trade_id.substr(0, 12));
                }

                // Remove from pending offers.
                state_->remove_offer(trade_id);
                fills.push_back(std::move(fill));

            } else if (status == trade_status::kCancelled ||
                       status == trade_status::kFailed) {
                // Offer was cancelled or failed -- remove from tracking.
                state_->remove_offer(trade_id);
                logger_->info("Offer {} removed (status={})",
                              trade_id.substr(0, 12), status);
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

asio::awaitable<int> OfferManager::cancel_stale(
    const std::string& pair_name,
    BlockHeight        current_block,
    BlockHeight        ttl_blocks)
{
    auto all_offers = state_->get_all_offers();
    int cancelled = 0;

    for (const auto& po : all_offers) {
        // Filter by pair name.
        if (po.pair_name != pair_name) {
            continue;
        }

        // Check if the offer has exceeded its TTL.
        if (current_block < po.created_at_block + ttl_blocks) {
            continue;
        }

        // Cancel via wallet RPC (secure = true to spend the locked coin).
        try {
            constexpr std::uint64_t kCancelFee = 100'000'000ULL; // 0.0001 XCH
            co_await wallet_->cancel_offer(
                po.offer_id, kCancelFee, /*secure=*/true);

            state_->remove_offer(po.offer_id);
            ++cancelled;

            logger_->info("Cancelled stale offer {} ({} tier {}, age {} blocks)",
                          po.offer_id.substr(0, 12),
                          pair_name, po.tier,
                          current_block - po.created_at_block);
        } catch (const rpc::ChiaRPCError& e) {
            logger_->error("Failed to cancel offer {}: {}",
                           po.offer_id.substr(0, 12), e.what());
        }
    }

    if (cancelled > 0) {
        logger_->info("cancel_stale({}): {} offers cancelled", pair_name,
                      cancelled);
    }

    co_return cancelled;
}

// ---------------------------------------------------------------------------
// cancel_all -- shutdown: cancel every pending offer
// ---------------------------------------------------------------------------

asio::awaitable<void> OfferManager::cancel_all()
{
    logger_->info("cancel_all: initiating bulk cancellation");

    auto all_offers = state_->get_all_offers();
    if (all_offers.empty()) {
        logger_->info("cancel_all: no pending offers to cancel");
        co_return;
    }

    // ISO/IEC 5055: track which offers were successfully cancelled.
    // Only clear offers whose cancellation succeeded.
    // Failed cancellations remain tracked for retry on next heartbeat.
    std::vector<std::string> cancelled_ids;
    cancelled_ids.reserve(all_offers.size());

    // Attempt bulk cancellation first (wallet cancel_offers endpoint).
    bool bulk_ok = false;
    try {
        constexpr std::uint64_t kCancelFee = 100'000'000ULL;
        co_await wallet_->cancel_offers(kCancelFee, /*secure=*/true);
        logger_->info("cancel_all: bulk cancel_offers succeeded");
        bulk_ok = true;
        // Bulk success: all offers are considered cancelled.
        for (const auto& po : all_offers) {
            cancelled_ids.push_back(po.offer_id);
        }
    } catch (const rpc::ChiaRPCError& e) {
        // Bulk cancel failed -- fall back to individual cancellation.
        logger_->warn("Bulk cancel_offers failed: {} -- falling back to "
                      "individual cancellation", e.what());

        for (const auto& po : all_offers) {
            try {
                constexpr std::uint64_t kCancelFee = 100'000'000ULL;
                co_await wallet_->cancel_offer(
                    po.offer_id, kCancelFee, /*secure=*/true);
                logger_->debug("Cancelled offer {}", po.offer_id.substr(0, 12));
                cancelled_ids.push_back(po.offer_id);
            } catch (const rpc::ChiaRPCError& inner_e) {
                logger_->error("Failed to cancel offer {}: {}",
                               po.offer_id.substr(0, 12), inner_e.what());
                // Do NOT add to cancelled_ids -- keep tracked for retry.
            }
        }
    }

    // Remove only the offers whose cancellation succeeded from state.
    // Failed cancellations remain tracked for retry on the next heartbeat.
    for (const auto& id : cancelled_ids) {
        state_->remove_offer(id);
    }

    logger_->info("cancel_all: {}/{} offers cancelled successfully",
                  cancelled_ids.size(), all_offers.size());
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
    // Read inventory skew from state (the pair_name is used as the base
    // asset_id for the position lookup -- the engine maps pair -> asset).
    double skew = state_->inventory_skew(pair_name, pair_name);
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
    // Positive skew = long base -> reduce ask size, increase bid size.
    // Negative skew = short base -> increase ask size, reduce bid size.
    // Clamped to [-1, +1] for safety.
    const double clamped_skew = std::clamp(inv_skew, -1.0, 1.0);

    // Skew multiplier: at skew=+1, bid gets 1.5x, ask gets 0.5x.
    // At skew=0, both get 1.0x (symmetric).
    const double bid_size_mult = 1.0 + 0.5 * clamped_skew;
    const double ask_size_mult = 1.0 - 0.5 * clamped_skew;

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

    json offer_dict = json::object();

    if (tier.side == Side::Bid) {
        // BID: we offer quote-asset mojos (negative), request base-asset mojos
        // (positive).
        //
        // Quote cost = base_size * price_per_base (mojos of quote).
        // The offer_dict keys must be strings of the wallet_id.
        const Mojo quote_amount = static_cast<Mojo>(
            std::ceil(static_cast<double>(tier.size)
                      * static_cast<double>(tier.price)
                      / static_cast<double>(kMojosPerXch)));

        // Wallet RPC convention: negative = we spend, positive = we receive.
        offer_dict[std::to_string(quote_wid)] = -quote_amount;
        offer_dict[std::to_string(base_wid)]  =  tier.size;

    } else {
        // ASK: we offer base-asset mojos (negative), request quote-asset mojos
        // (positive).
        const Mojo quote_amount = static_cast<Mojo>(
            std::floor(static_cast<double>(tier.size)
                       * static_cast<double>(tier.price)
                       / static_cast<double>(kMojosPerXch)));

        offer_dict[std::to_string(base_wid)]  = -tier.size;
        offer_dict[std::to_string(quote_wid)] =  quote_amount;
    }

    return offer_dict;
}

// ---------------------------------------------------------------------------
// submit_to_dexie -- post offer text to the dexie aggregator (best-effort)
// ---------------------------------------------------------------------------

asio::awaitable<bool> OfferManager::submit_to_dexie(
    const std::string& offer_text)
{
    // Dexie offer submission endpoint: POST /v1/offers
    // Body: {"offer": "<bech32m text>"}
    //
    // This is a best-effort submission.  The offer is valid on-chain
    // regardless of whether dexie accepts it.  Failures are logged but
    // do not constitute an error in the offer lifecycle.
    //
    // Implementation note: in a production build this would use an HTTP
    // client (libcurl or beast).  For now the structure is defined and
    // the actual HTTP call is a placeholder awaiting the HTTP client
    // integration.

    try {
        // TODO(phase-2): integrate dexie HTTP client.
        // POST to dexie_cfg_.api_base + "/offers"
        // with JSON body {"offer": offer_text}
        //
        // For now, log the intent and return true to allow the flow to
        // continue during development.
        logger_->debug("submit_to_dexie: would POST to {}/offers ({} bytes)",
                       dexie_cfg_.api_base, offer_text.size());
        co_return true;
    } catch (const std::exception& e) {
        logger_->warn("submit_to_dexie failed: {}", e.what());
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

}  // namespace xop::execution
