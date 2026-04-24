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
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
// TierStaleness -- per-tier classification for selective refresh
// ---------------------------------------------------------------------------

/// [T5-01] Classification of an existing pending offer relative to the
/// current optimal ladder.  Used by selective_cancel() to decide which
/// offers to refresh and which to keep.
///
/// Scholarly basis:
///   - Gao, X. & Wang, Y. (2020).  "Optimal market making in the presence
///     of latency."  The optimal cancel decision depends on the price
///     deviation of the stale quote relative to the new optimal.
///   - Aït-Sahalia, Y. & Saglam, M. (2017).  "High frequency market
///     making."  Endogenous cancellation threshold derived from adverse
///     selection risk.
enum class TierStaleness : std::uint8_t {
    Fresh   = 0,  ///< Deviation is favorable or within threshold -- keep live.
    Stale   = 1,  ///< Adverse deviation > threshold -- cancel & replace.
    Expired = 2,  ///< Age exceeds TTL -- must cancel regardless of price.
};

/// Result of classifying a single pending offer against the optimal ladder.
struct TierClassification {
    std::string     offer_id;        ///< The pending offer's trade ID.
    TierStaleness   staleness;       ///< Fresh / Stale / Expired.
    double          price_deviation; ///< Absolute fractional price deviation.
    bool            adverse{false};  ///< True when the move hurts us (bid too high / ask too low).
    bool            crossed{false};  ///< True when the offer crossed the mid-price.
    std::uint8_t    tier_index;      ///< Tier index of this offer.
    Side            side;            ///< Bid or Ask.
};

// ---------------------------------------------------------------------------
// RebalanceSnapshot -- context captured at the moment of last rebalance,
//                      used to evaluate whether a new rebalance is needed.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// OrphanDisposition -- decision outcome for a wallet offer found during
//                      startup that the engine does not currently track.
//
// Scholarly basis:
//   Guéant, Lehalle & Fernandez-Tapia (2013): cost-aware cancellation.
//   Gao & Wang (2020): zero-offer gap is the primary adverse selection cost.
//   Aït-Sahalia & Saglam (2017): stale-quote risk = f(deviation, lifetime).
// ---------------------------------------------------------------------------
enum class OrphanDisposition : std::uint8_t {
    Adopt      = 0,  ///< Price acceptable, re-track as a PendingOffer.
    AdoptStale = 1,  ///< Mildly mispriced but cheaper to keep than cancel.
                     ///< Re-track and flag for early refresh next heartbeat.
    Cancel     = 2,  ///< Dangerously mispriced or too old — cancel on-chain.
    Unknown    = 3,  ///< Could not parse offer details — cancel defensively.
};

/// Human-readable label for logging.
inline const char* to_string(OrphanDisposition d) noexcept {
    switch (d) {
        case OrphanDisposition::Adopt:      return "Adopt";
        case OrphanDisposition::AdoptStale: return "AdoptStale";
        case OrphanDisposition::Cancel:     return "Cancel";
        case OrphanDisposition::Unknown:    return "Unknown";
    }
    return "?";
}

/// Full evaluation result for a single orphaned wallet offer.
struct OrphanEvaluation {
    std::string        trade_id;
    OrphanDisposition  disposition{OrphanDisposition::Unknown};
    std::string        pair_name;           ///< Resolved pair, "" if unknown.
    Side               side{Side::Bid};
    Mojo               price{0};            ///< Implied price from summary.
    Mojo               size{0};             ///< Offered quantity.
    double             price_deviation{0};  ///< Fractional deviation from mid.
    bool               adverse{false};      ///< True if deviation hurts us.
    bool               inventory_helper{false}; ///< Helps reduce imbalance.
    Mojo               expected_loss{0};    ///< Estimated adverse selection cost.
    Mojo               cancel_cost{0};      ///< Fee + opportunity cost.
    std::string        reason;              ///< Human-readable explanation.
};

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
     * @param ioc           Boost.Asio io_context (accepted for API stability;
     *                      not stored — async work runs on the caller's strand).
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
        BlockHeight                 block_height,
        double                      fee_reserve_override = 0.0);

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
     * @return Offer IDs that were successfully cancelled.
     */
    asio::awaitable<std::vector<std::string>> cancel_stale(
        const std::string& pair_name,
        BlockHeight        current_block,
        BlockHeight        ttl_blocks);

    /**
     * @brief Cancel every pending offer.  Called during graceful shutdown.
     *
     * Uses the wallet's cancel_offers() bulk endpoint with secure=true
     * to guarantee all locked coins are released on-chain.  Clears the
     * internal pending-offer map in State.
     *
     * @return Offer IDs that were successfully cancelled.
     */
    asio::awaitable<std::vector<std::string>> cancel_all();

    // -- Selective refresh --------------------------------------------------

    /**
     * @brief [T5-01] Classify each pending offer's staleness relative to
     *        the current optimal ladder.
     *
     * Compares every pending offer for the given pair against the new
     * ladder tiers by (side, tier_index) and computes the fractional
     * price deviation.  Offers whose prices have drifted beyond
     * kSelectiveRefreshThreshold are classified as Stale; offers past
     * TTL are classified as Expired; all others are Fresh.
     *
     * @param pair_name     Trading pair name.
     * @param new_ladder    The newly computed optimal tier ladder.
     * @param current_block Current block height (for TTL check).
     * @param ttl_blocks    Maximum offer age in blocks.
     * @param mid_price     Current market mid-price in mojos (for cross detection).
     * @param anchor_active When true (competitive anchor pricing), large
     *                      deviations in EITHER direction trigger staleness,
     *                      not just adverse deviations.
     * @return Per-offer classification results.
     */
    std::vector<TierClassification> classify_tier_staleness(
        const std::string&             pair_name,
        const std::vector<TierQuote>&  new_ladder,
        BlockHeight                    current_block,
        BlockHeight                    ttl_blocks,
        Mojo                           mid_price = 0,
        bool                           anchor_active = false) const;

    /**
     * @brief [T5-01] Cancel only the offers classified as Stale or Expired.
     *
     * Implements the "selective refresh" pattern from Gao & Wang (2020):
     * only cancel the subset of tiers whose prices have drifted beyond
     * the staleness threshold.  Fresh tiers remain live on-chain,
     * eliminating the zero-offer gap for the unaffected levels.
     *
     * @param stale_ids  Offer IDs to cancel (from classify_tier_staleness).
     * @return Offer IDs that were successfully cancelled.
     */
    asio::awaitable<std::vector<std::string>> selective_cancel(
        const std::vector<std::string>& stale_ids);

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

    // -- Wallet ID cache management -----------------------------------------

    /// [T5-10] Invalidate the wallet-ID cache so that the next
    /// post_quotes() call re-queries get_wallets().  Must be called
    /// when the Chia wallet adds or removes CAT wallets at runtime
    /// (e.g. after a token airdrop or wallet recovery).
    void invalidate_wallet_ids() noexcept;

    /// Ensure the asset-to-wallet-ID cache is populated.  Safe to call
    /// multiple times; only the first call performs the RPC query.
    asio::awaitable<void> ensure_wallet_ids();

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
     * @return Offer IDs removed from state (orphans/phantoms).
     */
    asio::awaitable<std::vector<std::string>> reconcile_offers(
        BlockHeight current_block = 0);

    /**
     * @brief Startup reconciliation: scan wallet for orphaned offers.
     *
     * Queries the wallet for ALL PENDING_ACCEPT offers.  Known offers
     * (in the DB) are restored.  Unknown offers ("orphans") are evaluated
     * using cost-aware analysis (Guéant-Lehalle 2013, Gao-Wang 2020):
     *
     *   - Well-priced orphans are ADOPTED into State, saving the cancel
     *     fee and preserving market presence (zero-offer gap avoidance).
     *   - Mildly mispriced orphans are ADOPTED but flagged for early
     *     refresh on the next heartbeat cycle.
     *   - Dangerously mispriced or very old orphans are CANCELLED.
     *   - Unparseable orphans are CANCELLED defensively.
     *
     * @param known_offer_ids  Set of offer IDs the DB considers pending.
     * @param current_block    Current block height (for age computation).
     *                         0 = skip age-based evaluation.
     * @return Offer IDs that were cancelled as orphans.
     */
    asio::awaitable<std::vector<std::string>> startup_reconcile(
        const std::unordered_set<std::string>& known_offer_ids,
        BlockHeight current_block = 0);

    /**
     * @brief Evaluate a single orphaned wallet offer for adoption vs.
     *        cancellation using cost-aware analysis.
     *
     * Parses the trade record's summary field to extract the pair, side,
     * price, and size.  Compares the offer's price against the current
     * dexie mid-price.  Applies the Gao-Wang (2020) cancel-vs-keep
     * decision: cancel only when expected adverse selection loss exceeds
     * the cancellation cost (fee + liquidity opportunity cost).
     *
     * @param trade_record   Wallet trade record JSON.
     * @param current_block  Current block height (for age).
     * @param mid_prices     Map of pair_name -> current mid-price (mojos).
     *                       If the pair is missing, the orphan is Unknown.
     * @return Full evaluation result with disposition and reasoning.
     */
    OrphanEvaluation evaluate_orphan(
        const nlohmann::json& trade_record,
        BlockHeight current_block,
        const std::unordered_map<std::string, Mojo>& mid_prices) const;

    /**
     * @brief Prune stuck transactions from wallet transaction lists.
     *
     * Queries get_transactions() for each wallet involved in trading and
     * checks for unconfirmed transactions that have been pending longer
     * than max_age_seconds.  If found, calls
     * delete_unconfirmed_transactions() to clear them.
     *
     * Stuck transactions occur when the wallet daemon creates local
     * transaction records but fails to broadcast the spend bundle
     * (e.g. due to a coin double-spend conflict from rapid consecutive
     * offer creation).
     *
     * @param wallet_ids      Set of wallet IDs to scan for stuck transactions.
     * @param max_age_seconds Maximum age in seconds before a pending
     *                        transaction is considered stuck (default 600 = 10 min).
     * @return Number of wallets that had stuck transactions cleared.
     */
    asio::awaitable<int> prune_stuck_transactions(
        const std::vector<std::int64_t>& wallet_ids,
        std::int64_t max_age_seconds = 600);

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

    // -- Cancel-reduction constants (public for Engine access) ---------------

    /// Hard TTL multiplier.  The configured offer_ttl_blocks becomes a
    /// "soft" TTL — offers past soft TTL are expired only if they show
    /// adverse deviation above kSoftTtlAdverseThreshold.  The hard TTL
    /// (soft × multiplier) is the absolute safety cap: offers past this
    /// age are always expired regardless of price accuracy.
    static constexpr std::uint32_t kHardTtlMultiplier = 2;

    /**
     * @brief Emergency cancel with reduced or zero fee.
     *
     * When the standard fee cancel fails due to insufficient XCH, this
     * helper checks the actual spendable XCH balance and attempts:
     *   1. Cancel with whatever XCH remains as the fee (secure).
     *   2. Secure cancel with fee=0 (offer coins are the spend inputs).
     *   3. If all else fails, cancel locally (insecure / no on-chain spend).
     *
     * When @p prefer_zero_fee is true (used by UTXO liberation), step 2
     * is tried FIRST to avoid burning spendable XCH on cancel fees.
     *
     * @param offer_id         Trade ID of the offer to cancel.
     * @param context          Log context string (e.g. "cancel_stale",
     *                         "utxo_liberation").
     * @param prefer_zero_fee  If true, try fee=0 secure cancel before the
     *                         descending fee loop (default: false).
     * @return true if the emergency cancel succeeded, false otherwise.
     */
    asio::awaitable<bool> emergency_cancel(
        const std::string& offer_id,
        const std::string& context,
        bool prefer_zero_fee = false);

    /**
     * @brief Parse a wallet trade record into a PendingOffer.
     *
     * Extracts pair name, side, price, and size from the record's
     * summary field by matching offered/requested assets against
     * pair_config_map_.  Returns std::nullopt if the record cannot
     * be meaningfully parsed.
     *
     * Used by reconcile_offers() and startup_reconcile() to adopt
     * wallet offers that are not tracked in engine State.
     *
     * @param trade_record   Wallet trade record JSON.
     * @param current_block  Current block height (for age approximation).
     * @return Populated PendingOffer, or std::nullopt.
     */
    std::optional<PendingOffer> try_parse_wallet_offer(
        const nlohmann::json& trade_record,
        BlockHeight current_block) const;

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
     * @brief [T7-10] Create a single merged offer for all same-side tiers.
     *
     * Sums the mojo amounts across tiers for each wallet ID, producing one
     * aggregate offer per side.  All constituent tiers are tracked with the
     * same offer ID in shared State.  Falls back to individual creation on
     * RPC failure.
     *
     * @param pair          Pair configuration (asset IDs).
     * @param tiers         Same-side tier quotes to merge.
     * @param block_height  Current block height for PendingOffer stamping.
     * @return Number of tiers successfully posted (0 or tiers.size()).
     */
    asio::awaitable<int> post_merged_side(
        const PairConfig&              pair,
        const std::vector<TierQuote>&  tiers,
        BlockHeight                    block_height);

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
     * @brief One-time initialisation of the asset-to-wallet-ID cache.
     *
     * Called lazily on the first post_quotes() invocation.  Queries
     * get_wallets() and populates wallet_id_map_.
     */
    asio::awaitable<void> init_wallet_id_map();

    // -- Member data --------------------------------------------------------

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

    /// [T5-01] Selective refresh: fractional price deviation threshold.
    /// Only *adverse* deviations (bid too high / ask too low) trigger
    /// staleness.  Favorable deviations (bid drifted lower / ask drifted
    /// higher) make the offer more conservative, not dangerous.
    /// Set to 1.0% adverse deviation for tier 0 (innermost).  Outer
    /// tiers apply a wider threshold scaled by kTierThresholdScale.
    /// [v0.7.47] Raised 0.005 -> 0.010 after live audit showed 49/50
    /// XCH/wUSDC.b offers cancelled at 1.26-1.54% deviation while
    /// competitiveness averaged 8.2/10 -- offers were dying just before
    /// they could fill in a trending market (XCH +12% over 3 days).
    static constexpr double kSelectiveRefreshThreshold = 0.010;

    /// Hard crossing threshold: if an offer's *adverse* deviation is
    /// this large the offer has likely crossed the mid-price and must
    /// be cancelled urgently regardless of other considerations.
    static constexpr double kCrossedMidThreshold = 0.02;

    // -- Cancel-reduction parameters ------------------------------------------

    /// Per-tier scaling factor for the adverse-deviation threshold.
    /// Effective threshold = kSelectiveRefreshThreshold × (1 + tier × scale).
    ///   tier 0 → 1.00%   tier 1 → 1.50%   tier 2 → 2.00%   tier 3 → 2.50%
    /// Outer tiers are further from mid-price and tolerate more movement.
    static constexpr double kTierThresholdScale = 0.5;

    /// Minimum offer age (in blocks) before price-deviation cancellation
    /// is considered.  Very young offers are protected from cancel churn
    /// because the round-trip fee (cancel + recreate) exceeds the adverse
    /// selection risk for small deviations.  Crossed-mid is still urgent.
    /// [v0.7.47] Raised 6 -> 12 (~10 min @ 52s blocks) so newly posted
    /// offers have a real chance to fill before adverse-cancel logic
    /// pulls them.
    static constexpr BlockHeight kMinRefreshAgeBlocks = 12;

    /// Gentler adverse-deviation threshold applied between soft and hard
    /// TTL.  Only refresh if the offer has adversely drifted by more than
    /// this fraction.  Must be ≥ the reservation_mid max_deviation_pct
    /// (typically 2%) to avoid false expiry from model+clamp noise.
    static constexpr double kSoftTtlAdverseThreshold = 0.02;

    /// [v0.7.37] Size-based staleness: if a pending offer's size is more
    /// than kSizeStaleThreshold × the new optimal size, treat it as stale.
    /// This catches over-allocation after the market allocator reshuffles
    /// capital fractions.  2.0 = pending offer is ≥ 2× the new optimal.
    static constexpr double kSizeStaleThreshold = 2.0;
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_OFFER_MANAGER_HPP
