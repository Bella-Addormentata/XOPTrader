/**
 * @file offer_manager.hpp
 * @brief Offer lifecycle manager for the XOPTrader CHIA DEX market-making bot.
 *
 * Owns the full lifecycle of on-chain offers: creation from strategy-generated
 * quotes, multi-tier ladder posting, fill detection by polling the wallet RPC,
 * stale-offer cancellation, and graceful shutdown (cancel-all).
 *
 * CHIA Offer Model recap (Section 4 of strategy document):
 *   - A "maker" builds an incomplete, partially-signed spend bundle describing
 *     offered and requested assets.
 *   - The bundle is serialised to bech32m text and broadcast (dexie / Splash).
 *   - A "taker" completes the bundle with complementary coin spends.
 *   - Settlement is atomic (all-or-nothing) in one on-chain block (~52 s).
 *   - Cancellation = spending any referenced coin, invalidating the bundle.
 *
 * Multi-Tier Quoting (Section 11):
 *   Each side (bid/ask) of a pair is split into N independent offers, each at
 *   a wider spread from the mid-price and a larger size.  Tier config comes
 *   from StrategyConfig::tier_spacing_bps / tier_size_pct.
 *
 * Rebalancing Triggers (Section 11):
 *   - Price deviation  > 2% from last rebalance  -> cancel & re-post.
 *   - Inventory skew   > 60%                     -> asymmetric spread.
 *   - Time decay       > 1 hour (69 blocks)      -> refresh all.
 *   - Volume spike     > 3x average              -> tighten spreads.
 *   - Volatility spike > 2x 7-day avg            -> widen 50-100%.
 *
 * Thread safety:
 *   OfferManager is designed to run on a single strand.  Internal state
 *   (pending-offer map) is also synchronised through the shared State object,
 *   which uses its own mutexes.
 *
 * ISO/IEC 27001:2022 -- offer text (bech32) not logged at INFO level.
 * ISO/IEC 5055       -- no raw pointers; RAII via shared_ptr on RPC clients.
 * ISO/IEC 25000      -- clear naming, single-responsibility, documented API.
 */

#ifndef XOP_EXECUTION_OFFER_MANAGER_HPP
#define XOP_EXECUTION_OFFER_MANAGER_HPP

#include <xop/config.hpp>
#include <xop/state.hpp>
#include <xop/types.hpp>
#include <xop/rpc/chia_rpc.hpp>
#include <xop/rpc/dexie_client.hpp>
#include <xop/strategy/base.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop::execution {

namespace asio = boost::asio;
using json = nlohmann::json;

// TierQuote and RebalanceReason are defined in <xop/types.hpp> (unified).
// The execution layer uses xop::TierQuote and xop::RebalanceReason directly.
using xop::TierQuote;
using xop::RebalanceReason;

/// Convenience predicate: returns true if at least one rebalance flag is set.
inline bool any_trigger(RebalanceReason r) noexcept {
    return static_cast<std::uint8_t>(r) != 0;
}

// ---------------------------------------------------------------------------
// RebalanceSnapshot -- context captured at the moment of last rebalance,
//                      used to evaluate whether a new rebalance is needed.
// ---------------------------------------------------------------------------

struct RebalanceSnapshot {
    Mojo        mid_price{0};                  ///< Mid-price at last rebalance.
    BlockHeight block_height{0};               ///< Block height at last rebalance.
    double      volume_avg{0.0};               ///< Rolling average volume.
    double      volatility_7d{0.0};            ///< 7-day average volatility.
    std::chrono::steady_clock::time_point ts;  ///< Wall-clock time.
};

// ---------------------------------------------------------------------------
// OfferManager
// ---------------------------------------------------------------------------

/**
 * @brief Manages the full lifecycle of CHIA DEX offers for market making.
 *
 * Responsibilities:
 *   1. Convert strategy QuoteResults into multi-tier offer ladders.
 *   2. Create on-chain offers via the wallet RPC (create_offer_for_ids).
 *   3. Extract bech32m offer text and submit to dexie for aggregation.
 *   4. Track every outstanding offer as a PendingOffer in shared State.
 *   5. Detect fills by polling get_all_offers() and comparing status.
 *   6. Cancel stale offers (block-based TTL) or all offers on shutdown.
 *   7. Evaluate rebalancing triggers and return them to the engine.
 */
class OfferManager {
public:
    /**
     * @brief Construct an OfferManager.
     *
     * @param ioc           Boost.Asio io_context for async operations.
     * @param wallet        Shared pointer to an open ChiaWalletRPC client.
     * @param dexie_client  Shared pointer to an open DexieClient for offer
     *                      aggregation.  May be nullptr if Dexie submission
     *                      is not desired (offers remain valid on-chain only).
     * @param state         Shared pointer to the global mutable state.
     * @param config        Application configuration (strategy + risk params).
     */
    OfferManager(asio::io_context&                         ioc,
                 std::shared_ptr<rpc::ChiaWalletRPC>       wallet,
                 std::shared_ptr<rpc::DexieClient>         dexie_client,
                 std::shared_ptr<State>                    state,
                 const AppConfig&                          config);

    ~OfferManager() = default;

    // Non-copyable, non-movable -- owned by the engine via unique_ptr.
    OfferManager(const OfferManager&)            = delete;
    OfferManager& operator=(const OfferManager&) = delete;
    OfferManager(OfferManager&&)                 = delete;
    OfferManager& operator=(OfferManager&&)      = delete;

    // -- Offer creation (multi-tier) ----------------------------------------

    /**
     * @brief Build and post a full multi-tier bid+ask offer ladder.
     *
     * For each enabled tier (from config.strategy.tier_spacing_bps):
     *   1. Compute the bid and ask prices using the strategy's QuoteResult
     *      and the tier's basis-point offset.
     *   2. Compute the size from the tier's capital percentage.
     *   3. Build the offer_dict (wallet_id -> mojo amount, negative = we
     *      offer, positive = we request).
     *   4. Call wallet.create_offer() to produce the spend bundle.
     *   5. Extract the bech32m offer text from the response.
     *   6. Submit the offer text to dexie for cross-platform aggregation.
     *   7. Track the offer as a PendingOffer in shared State.
     *
     * @param pair           Pair configuration (base/quote asset IDs).
     * @param quotes         Multi-tier quote set from the strategy engine.
     *                       Length must equal config.strategy.num_tiers.
     * @param block_height   Current block height (stamped on PendingOffers).
     * @return Number of offers successfully created (bid + ask).
     */
    asio::awaitable<int> post_quotes(
        const PairConfig&           pair,
        const std::vector<TierQuote>& quotes,
        BlockHeight                 block_height);

    // -- Fill detection -----------------------------------------------------

    /**
     * @brief Poll the wallet for settled offers and return newly detected fills.
     *
     * Queries get_all_offers(), compares each returned trade record against
     * the internal pending-offer map.  An offer whose status transitions to
     * CONFIRMED (status == 4 in the Chia wallet) is treated as filled:
     *   - A Fill event is constructed.
     *   - The PendingOffer is removed from State.
     *   - Position accounting is updated (record_buy for bids, record_sell
     *     for asks) via the shared State object.
     *
     * @return Vector of Fill events detected in this poll cycle.
     */
    asio::awaitable<std::vector<Fill>> detect_fills();

    // -- Cancellation -------------------------------------------------------

    /**
     * @brief Cancel offers for a pair that have exceeded their TTL in blocks.
     *
     * Iterates pending offers matching @p pair_name.  Any offer whose
     * created_at_block + ttl_blocks <= current_block is cancelled via the
     * wallet RPC (secure cancellation -- spends the locked coin on-chain).
     *
     * @param pair_name      Trading pair name (e.g. "XCH/wUSDC").
     * @param current_block  Latest known block height.
     * @param ttl_blocks     Maximum offer age in blocks before cancellation.
     * @return Number of offers actually cancelled.
     */
    asio::awaitable<int> cancel_stale(const std::string& pair_name,
                                      BlockHeight        current_block,
                                      BlockHeight        ttl_blocks);

    /**
     * @brief Cancel every pending offer.  Called during graceful shutdown.
     *
     * Uses the wallet's cancel_offers() bulk endpoint with secure=true
     * to guarantee all locked coins are released on-chain.  Clears the
     * internal pending-offer map in State.
     */
    asio::awaitable<void> cancel_all();

    // -- Rebalancing --------------------------------------------------------

    /**
     * @brief Evaluate all rebalancing triggers for a given pair.
     *
     * Checks the five triggers defined in Section 11 against the snapshot
     * captured at the most recent rebalance:
     *   1. Price deviation  > 2% from last rebalance mid.
     *   2. Inventory skew   > 60%.
     *   3. Time decay       > 1 hour (~69 blocks at 52 s/block).
     *   4. Volume spike     > 3x rolling average.
     *   5. Volatility spike > 2x 7-day average.
     *
     * @param pair_name     Trading pair name.
     * @param current_mid   Current mid-price (mojos).
     * @param current_block Current block height.
     * @param current_vol   Current annualised volatility (dimensionless).
     * @param current_volume Recent volume (mojos over a window).
     * @return Bitmask of RebalanceReason flags.
     */
    RebalanceReason evaluate_rebalance(
        const std::string& pair_name,
        Mojo               current_mid,
        BlockHeight        current_block,
        double             current_vol,
        double             current_volume) const;

    /**
     * @brief Record the current market state as the new rebalance baseline.
     *
     * Called by the engine after a successful cancel + re-post cycle so
     * that subsequent evaluate_rebalance() calls compare against the fresh
     * baseline.
     *
     * @param pair_name  Trading pair name.
     * @param snap       Snapshot to record.
     */
    void record_rebalance(const std::string& pair_name,
                          const RebalanceSnapshot& snap);

    // -- Multi-tier quote generation ----------------------------------------

    /**
     * @brief Expand a strategy's single QuoteResult into a multi-tier ladder.
     *
     * For each tier i in [0, num_tiers):
     *   - Bid price  = mid * (1 - tier_spacing_bps[i] / 10000)
     *   - Ask price  = mid * (1 + tier_spacing_bps[i] / 10000)
     *   - Size       = total_alloc * tier_size_pct[i]
     *
     * Sizes are rounded down to whole mojos.  The never-sell-at-loss
     * constraint is applied to ask prices: ask >= cost_basis + min_margin.
     *
     * @param mid_price      Current mid-price in mojos.
     * @param total_base     Total base-asset mojos available for offers.
     * @param total_quote    Total quote-asset mojos available for offers.
     * @param cost_basis     Weighted-average cost (mojos quote per base).
     * @param inv_skew       Inventory skew [-1, +1] for asymmetric sizing.
     * @return Vector of TierQuote entries (both bid and ask sides).
     */
    std::vector<TierQuote> build_tier_ladder(
        Mojo   mid_price,
        Mojo   total_base,
        Mojo   total_quote,
        Mojo   cost_basis,
        double inv_skew) const;

    // -- Accessors ----------------------------------------------------------

    /// Number of currently tracked pending offers.
    std::size_t pending_count() const;

    // -- Dynamic fee control ------------------------------------------------

    /// Set the per-transaction fee used by subsequent post_quotes(),
    /// cancel_stale(), and cancel_all() calls.  Allows the engine's
    /// FeeTracker to override the static offer_fee_mojos at runtime.
    ///
    /// @param fee_mojos  Fee in mojos.  Must be > 0.
    void set_dynamic_fee(std::uint64_t fee_mojos) noexcept;

    /// Return the fee currently in effect (dynamic or static fallback).
    [[nodiscard]] std::uint64_t current_fee() const noexcept;

    // -- Offer reconciliation -----------------------------------------------

    /**
     * @brief [T4-11] Full reconciliation of in-memory offer state against
     *        the authoritative wallet RPC.
     *
     * Queries get_all_offers() from the wallet, compares each offer against
     * the in-memory pending-offers map in State, and corrects discrepancies:
     *   - Wallet offers missing from State (phantoms) -> add to State.
     *   - State offers missing from wallet (orphans)  -> remove from State.
     *   - Status mismatches                           -> update State.
     *
     * Should be called periodically (e.g. every N blocks) to prevent
     * state drift caused by missed RPC events, partial failures, or
     * wallet restarts.
     *
     * @return Number of discrepancies corrected.
     */
    asio::awaitable<int> reconcile_offers();

private:
    // -- Internal helpers ---------------------------------------------------

    /**
     * @brief Build the wallet RPC offer_dict for a single tier.
     *
     * Maps wallet IDs to signed mojo amounts:
     *   - Negative value = we are offering (spending) this asset.
     *   - Positive value = we are requesting (receiving) this asset.
     *
     * For a bid (we buy base): offer quote (negative), request base (positive).
     * For an ask (we sell base): offer base (negative), request quote (positive).
     *
     * @param pair   Pair configuration (asset IDs -> wallet IDs resolved
     *               via the wallet_id_map_).
     * @param tier   Tier quote with side, price, and size.
     * @return JSON object suitable for create_offer_for_ids.
     */
    json build_offer_dict(const PairConfig& pair,
                          const TierQuote&  tier) const;

    /**
     * @brief Submit bech32m offer text to the dexie aggregator API.
     *
     * Posts to the dexie /v1/offers endpoint for cross-platform visibility.
     * Non-critical: failures are logged but do not abort offer creation
     * (the offer is already valid on-chain).
     *
     * @param offer_text  Bech32m-encoded offer string.
     * @return true if submission succeeded, false on any error.
     */
    asio::awaitable<bool> submit_to_dexie(const std::string& offer_text);

    /**
     * @brief Resolve an asset ID to a wallet ID.
     *
     * "xch" maps to wallet_id 1.  CAT asset IDs are resolved by querying
     * get_wallets() once at startup and caching the mapping.
     *
     * @param asset_id  Asset identifier ("xch" or 64-hex CAT ID).
     * @return Wallet ID (positive integer), or -1 if not found.
     */
    std::int64_t resolve_wallet_id(const AssetId& asset_id) const;

    /**
     * @brief One-time initialisation of the asset-to-wallet-ID cache.
     *
     * Called lazily on the first post_quotes() invocation.  Queries
     * get_wallets() and populates wallet_id_map_.
     */
    asio::awaitable<void> init_wallet_id_map();

    // -- Member data --------------------------------------------------------

    /// Boost.Asio io_context for async timer and dispatch operations.
    asio::io_context& ioc_;

    /// Wallet RPC client (shared -- may be used by CoinManager too).
    std::shared_ptr<rpc::ChiaWalletRPC> wallet_;

    /// Dexie REST client for cross-platform offer aggregation (optional).
    /// May be nullptr if Dexie submission is disabled.
    std::shared_ptr<rpc::DexieClient> dexie_client_;

    /// Global mutable state (positions, pending offers, market snapshots).
    std::shared_ptr<State> state_;

    /// Strategy configuration (tier_spacing_bps, tier_size_pct, etc.).
    StrategyConfig strategy_cfg_;

    /// Risk configuration (soft_limit, min_profit_margin, etc.).
    RiskConfig risk_cfg_;

    /// Dexie API configuration (base URL, rate limits).
    DexieConfig dexie_cfg_;

    /// Per-component logger (spdlog).
    std::shared_ptr<spdlog::logger> logger_;

    /// Cache: asset_id -> wallet_id.  Populated once via init_wallet_id_map().
    std::unordered_map<AssetId, std::int64_t> wallet_id_map_;

    /// Flag indicating whether wallet_id_map_ has been initialised.
    bool wallet_ids_resolved_{false};

    /// Per-pair rebalance baselines for trigger evaluation.
    std::unordered_map<std::string, RebalanceSnapshot> rebalance_baselines_;

    /// Dynamic fee override.  Initialised from strategy_cfg_.offer_fee_mojos;
    /// updated at runtime by set_dynamic_fee() from the engine's FeeTracker.
    std::uint64_t current_fee_mojos_;

    /// O(1) lookup: pair_name -> PairConfig.  Populated once in the
    /// constructor from AppConfig::pairs so that evaluate_rebalance()
    /// can resolve base/quote asset IDs without an external parameter.
    /// ISO/IEC 5055: deterministic lookup, value-copy avoids dangling refs.
    std::unordered_map<std::string, PairConfig> pair_config_map_;

    // -- Configuration constants derived from strategy params ----------------

    /// Price deviation threshold for rebalance trigger (fraction, e.g. 0.02).
    static constexpr double kPriceDeviationThreshold = 0.02;

    /// Inventory skew threshold for rebalance trigger (fraction, e.g. 0.60).
    static constexpr double kInventorySkewThreshold  = 0.60;

    /// Time decay threshold in blocks (~69 blocks = 1 hour at 52 s/block).
    static constexpr BlockHeight kTimeDecayBlocks    = 69;

    /// Volume spike multiplier (3x average triggers rebalance).
    static constexpr double kVolumeSpikeMultiplier   = 3.0;

    /// Volatility spike multiplier (2x 7-day average triggers rebalance).
    static constexpr double kVolSpikeMult            = 2.0;
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_OFFER_MANAGER_HPP
