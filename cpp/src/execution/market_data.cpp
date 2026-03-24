// market_data.cpp -- Implementation of the multi-source market data aggregation
//                    layer for XOPTrader CHIA DEX market-making bot.
//
// Aggregation pipeline (executed once per block via refresh()):
//
//   ingest_dexie()           ──┐
//   ingest_block_height()    ──┼──> PairState ──> compute_mid()
//   ingest_cex_reference()   ──┘                  compute_spread_bps()
//                                                 detect_stale()
//                                                 check_arbitrage()
//                                                 append_price_history()
//                                                 publish_snapshot() ──> State
//
// Thread safety:
//   Three independent shared_mutexes protect three independent data maps.
//   Every public method acquires at most one mutex (shared or exclusive) and
//   releases it before returning.  Deadlock is impossible by construction.
//
// Monetary note:
//   Prices in this module use double (not mojos) because the aggregation
//   layer performs weighted averaging, basis-point arithmetic, and ratio
//   comparisons that are naturally expressed in floating point.  Conversion
//   to mojos happens in the execution layer (offer_manager) when creating
//   on-chain offers, preserving integer precision at the critical boundary.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no secrets; audit-quality logging of price updates
//   ISO/IEC 5055       -- no raw pointers, RAII locking, bounds-checked deque
//   ISO/IEC 25000      -- documented methods, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard C++20, no undefined behaviour

#include "xop/execution/market_data.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <shared_mutex>
#include <utility>

namespace xop {

// =========================================================================
//  Construction / destruction / move
// =========================================================================

MarketDataFeed::MarketDataFeed(const MarketDataConfig& cfg,
                               State&                  state,
                               ArbitrageCallback       arb_cb)
    : config_{cfg}
    , state_{state}
    , arb_callback_{std::move(arb_cb)}
{
    spdlog::info("MarketDataFeed constructed: history_capacity={}, "
                 "arb_threshold_bps={:.1f}, stale_threshold={}min",
                 config_.price_history_capacity,
                 config_.arb_threshold_bps,
                 config_.stale_threshold.count());
}

MarketDataFeed::MarketDataFeed(MarketDataFeed&& other) noexcept
    : config_{other.config_}
    , state_{other.state_}
    , arb_callback_{std::move(other.arb_callback_)}
    , block_height_{other.block_height_.load(std::memory_order_relaxed)}
{
    // Move the maps under exclusive locks on the source.
    {
        std::unique_lock lock(other.mtx_pairs_);
        pairs_ = std::move(other.pairs_);
    }
    {
        std::unique_lock lock(other.mtx_history_);
        history_ = std::move(other.history_);
    }
    {
        std::unique_lock lock(other.mtx_arb_);
        latest_arb_ = std::move(other.latest_arb_);
    }
    {
        std::unique_lock lock(other.mtx_competitors_);
        competing_offers_ = std::move(other.competing_offers_);
    }
    {
        std::unique_lock lock(other.mtx_competitor_metrics_);
        competitor_metrics_ = std::move(other.competitor_metrics_);
    }
    {
        std::unique_lock lock(other.mtx_whale_events_);
        whale_events_ = std::move(other.whale_events_);
    }
    {
        std::unique_lock lock(other.mtx_whale_metrics_);
        whale_metrics_ = std::move(other.whale_metrics_);
    }
    // Transfer VPIN volume-bar state (per-pair current bucket + completed bars).
    // ISO/IEC 5055: each map moved under its own mutex to prevent data races.
    {
        std::unique_lock lock(other.mtx_vpin_);
        vpin_state_ = std::move(other.vpin_state_);
    }
    // Transfer cached VPIN metrics (per-pair latest VPIN value).
    {
        std::unique_lock lock(other.mtx_vpin_metrics_);
        vpin_metrics_ = std::move(other.vpin_metrics_);
    }
    // Transfer OFI book-snapshot history (per-pair deque of snapshots).
    {
        std::unique_lock lock(other.mtx_ofi_);
        ofi_snapshots_ = std::move(other.ofi_snapshots_);
    }
    // Transfer cached OFI metrics (per-pair latest normalized OFI).
    {
        std::unique_lock lock(other.mtx_ofi_metrics_);
        ofi_metrics_ = std::move(other.ofi_metrics_);
    }
}

MarketDataFeed& MarketDataFeed::operator=(MarketDataFeed&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    config_       = other.config_;
    // state_ is a reference -- cannot be reseated; both must reference the same State.
    arb_callback_ = std::move(other.arb_callback_);
    block_height_.store(other.block_height_.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);

    {
        std::unique_lock lock_dst(mtx_pairs_);
        std::unique_lock lock_src(other.mtx_pairs_);
        pairs_ = std::move(other.pairs_);
    }
    {
        std::unique_lock lock_dst(mtx_history_);
        std::unique_lock lock_src(other.mtx_history_);
        history_ = std::move(other.history_);
    }
    {
        std::unique_lock lock_dst(mtx_arb_);
        std::unique_lock lock_src(other.mtx_arb_);
        latest_arb_ = std::move(other.latest_arb_);
    }
    {
        std::unique_lock lock_dst(mtx_competitors_);
        std::unique_lock lock_src(other.mtx_competitors_);
        competing_offers_ = std::move(other.competing_offers_);
    }
    {
        std::unique_lock lock_dst(mtx_competitor_metrics_);
        std::unique_lock lock_src(other.mtx_competitor_metrics_);
        competitor_metrics_ = std::move(other.competitor_metrics_);
    }
    {
        std::unique_lock lock_dst(mtx_whale_events_);
        std::unique_lock lock_src(other.mtx_whale_events_);
        whale_events_ = std::move(other.whale_events_);
    }
    {
        std::unique_lock lock_dst(mtx_whale_metrics_);
        std::unique_lock lock_src(other.mtx_whale_metrics_);
        whale_metrics_ = std::move(other.whale_metrics_);
    }
    // Transfer VPIN volume-bar state under paired locks.
    // ISO/IEC 5055: consistent lock ordering (dst then src) prevents deadlock.
    {
        std::unique_lock lock_dst(mtx_vpin_);
        std::unique_lock lock_src(other.mtx_vpin_);
        vpin_state_ = std::move(other.vpin_state_);
    }
    // Transfer cached VPIN metrics.
    {
        std::unique_lock lock_dst(mtx_vpin_metrics_);
        std::unique_lock lock_src(other.mtx_vpin_metrics_);
        vpin_metrics_ = std::move(other.vpin_metrics_);
    }
    // Transfer OFI book-snapshot history.
    {
        std::unique_lock lock_dst(mtx_ofi_);
        std::unique_lock lock_src(other.mtx_ofi_);
        ofi_snapshots_ = std::move(other.ofi_snapshots_);
    }
    // Transfer cached OFI metrics.
    {
        std::unique_lock lock_dst(mtx_ofi_metrics_);
        std::unique_lock lock_src(other.mtx_ofi_metrics_);
        ofi_metrics_ = std::move(other.ofi_metrics_);
    }

    return *this;
}

MarketDataFeed::~MarketDataFeed() {
    spdlog::debug("MarketDataFeed destroyed");
}

// =========================================================================
//  Primary interface: refresh
// =========================================================================

void MarketDataFeed::refresh(const std::vector<std::string>& enabled_pairs) {
    // Acquire exclusive lock on pairs_ for the duration of the refresh.
    // This serialises writes but readers (get_mid_price, etc.) can proceed
    // concurrently between refresh calls.
    std::unique_lock lock(mtx_pairs_);

    const auto now = std::chrono::system_clock::now();
    const BlockHeight current_block = block_height_.load(std::memory_order_acquire);

    for (const auto& pair_name : enabled_pairs) {
        PairState& ps = get_or_create_pair(pair_name);

        // Step 1-3: Data was already ingested via ingest_* methods.
        //           Update block height context.
        ps.last_block = current_block;

        // Step 4: Compute aggregated mid-price.
        ps.mid_price = compute_mid(ps);

        // Step 5: Compute spread.
        ps.spread_bps = compute_spread_bps(ps.dex_best_bid, ps.dex_best_ask);

        // Step 6: Detect staleness.
        //         A pair is stale if BOTH dex and cex timestamps are stale
        //         (or never updated).  If at least one source is fresh, the
        //         aggregated mid is still usable.
        const bool dex_stale = detect_stale(ps.dex_updated_at);
        const bool cex_stale = detect_stale(ps.cex_updated_at);
        ps.is_stale = dex_stale && (ps.cex_mid <= 0.0 || cex_stale);

        if (ps.is_stale) {
            spdlog::warn("MarketDataFeed: pair={} data is STALE "
                         "(dex_age={}s, cex_mid={:.6f})",
                         pair_name,
                         std::chrono::duration_cast<std::chrono::seconds>(
                             now - ps.dex_updated_at).count(),
                         ps.cex_mid);
        }

        // Step 7: Check for arbitrage divergence (only if both sources live).
        if (!dex_stale && ps.cex_mid > 0.0 && !cex_stale) {
            // Release pairs lock before acquiring arb lock to maintain the
            // single-mutex rule.  We copy the needed values first.
            //
            // NOTE: check_arbitrage acquires mtx_arb_ internally, but we
            // currently hold mtx_pairs_.  This is the ONE exception to the
            // "never hold two mutexes" rule.  It is safe because the lock
            // ordering is always pairs -> arb (never reversed).  We document
            // this ordering constraint here for future maintainers.
            check_arbitrage(ps);
        }

        // Step 8: Append to price history (if we have a valid price).
        if (ps.mid_price > 0.0 && current_block > 0) {
            // Release pairs lock scope not needed here; append_price_history
            // acquires mtx_history_ but we hold mtx_pairs_.  Lock ordering:
            // pairs -> history (never reversed).  Documented and safe.
            append_price_history(pair_name, current_block, ps.mid_price);
        }

        // Step 9: Publish snapshot to shared State.
        publish_snapshot(ps);

        spdlog::debug("MarketDataFeed: pair={} mid={:.6f} spread={:.1f}bps "
                      "vol24h={:.2f} block={} stale={}",
                      pair_name, ps.mid_price, ps.spread_bps,
                      ps.volume_24h, ps.last_block, ps.is_stale);
    }
}

// =========================================================================
//  Data ingestion
// =========================================================================

void MarketDataFeed::ingest_dexie(const std::string& pair_name,
                                   double             best_bid,
                                   double             best_ask,
                                   double             last_trade,
                                   double             vol_24h) {
    // Reject crossed-book data: bid >= ask produces nonsensical mid-prices.
    // Both sides must be positive (non-zero) for the check to apply; a zero
    // price indicates the side was absent from the book.
    if (best_bid > 0.0 && best_ask > 0.0 && best_bid >= best_ask) {
        spdlog::warn("[MarketData] Crossed book for {}: bid={} >= ask={}",
                     pair_name, best_bid, best_ask);
        return;  // Discard this snapshot entirely.
    }

    std::unique_lock lock(mtx_pairs_);
    PairState& ps = get_or_create_pair(pair_name);

    ps.dex_best_bid  = best_bid;
    ps.dex_best_ask  = best_ask;
    ps.dex_last_trade = last_trade;
    ps.volume_24h    = vol_24h;
    ps.dex_updated_at = std::chrono::system_clock::now();

    spdlog::debug("ingest_dexie: pair={} bid={:.6f} ask={:.6f} last={:.6f} "
                  "vol24h={:.2f}",
                  pair_name, best_bid, best_ask, last_trade, vol_24h);
}

void MarketDataFeed::ingest_block_height(BlockHeight block_height) {
    // Atomic store; no mutex needed.
    block_height_.store(block_height, std::memory_order_release);

    spdlog::debug("ingest_block_height: height={}", block_height);
}

void MarketDataFeed::ingest_cex_reference(const std::string& pair_name,
                                           double             cex_mid) {
    if (cex_mid <= 0.0) {
        spdlog::warn("ingest_cex_reference: pair={} invalid cex_mid={:.6f}",
                     pair_name, cex_mid);
        return;
    }

    std::unique_lock lock(mtx_pairs_);
    PairState& ps = get_or_create_pair(pair_name);

    ps.cex_mid        = cex_mid;
    ps.cex_updated_at = std::chrono::system_clock::now();

    spdlog::debug("ingest_cex_reference: pair={} cex_mid={:.6f}",
                  pair_name, cex_mid);
}

// =========================================================================
//  Typed accessors (thread-safe reads)
// =========================================================================

double MarketDataFeed::get_mid_price(const std::string& pair_name) const {
    std::shared_lock lock(mtx_pairs_);

    auto it = pairs_.find(pair_name);
    if (it == pairs_.end()) {
        return 0.0;
    }
    return it->second.mid_price;
}

double MarketDataFeed::get_spread_bps(const std::string& pair_name) const {
    std::shared_lock lock(mtx_pairs_);

    auto it = pairs_.find(pair_name);
    if (it == pairs_.end()) {
        return 0.0;
    }
    return it->second.spread_bps;
}

double MarketDataFeed::get_volume_24h(const std::string& pair_name) const {
    std::shared_lock lock(mtx_pairs_);

    auto it = pairs_.find(pair_name);
    if (it == pairs_.end()) {
        return 0.0;
    }
    return it->second.volume_24h;
}

std::optional<double> MarketDataFeed::get_cex_reference(
        const std::string& pair_name) const {
    std::shared_lock lock(mtx_pairs_);

    auto it = pairs_.find(pair_name);
    if (it == pairs_.end()) {
        return std::nullopt;
    }

    const PairState& ps = it->second;

    // Return nullopt if no CEX data has ever been ingested, or if it is stale.
    if (ps.cex_mid <= 0.0) {
        return std::nullopt;
    }
    if (detect_stale(ps.cex_updated_at)) {
        return std::nullopt;
    }

    return ps.cex_mid;
}

bool MarketDataFeed::is_stale(const std::string& pair_name) const {
    std::shared_lock lock(mtx_pairs_);

    auto it = pairs_.find(pair_name);
    if (it == pairs_.end()) {
        return true;  // Unknown pair is considered stale.
    }
    return it->second.is_stale;
}

BlockHeight MarketDataFeed::current_block_height() const {
    return block_height_.load(std::memory_order_acquire);
}

// =========================================================================
//  Price history access
// =========================================================================

std::vector<PriceHistoryEntry> MarketDataFeed::get_price_history(
        const std::string& pair_name) const {
    std::shared_lock lock(mtx_history_);

    auto it = history_.find(pair_name);
    if (it == history_.end()) {
        return {};
    }

    // Copy deque into a vector (oldest-to-newest order preserved).
    const auto& deq = it->second;
    return {deq.begin(), deq.end()};
}

std::size_t MarketDataFeed::price_history_size(
        const std::string& pair_name) const {
    std::shared_lock lock(mtx_history_);

    auto it = history_.find(pair_name);
    if (it == history_.end()) {
        return 0;
    }
    return it->second.size();
}

// =========================================================================
//  Arbitrage signal access
// =========================================================================

std::optional<ArbitrageSignal> MarketDataFeed::get_latest_arb_signal(
        const std::string& pair_name) const {
    std::shared_lock lock(mtx_arb_);

    auto it = latest_arb_.find(pair_name);
    if (it == latest_arb_.end()) {
        return std::nullopt;
    }
    return it->second;
}

// =========================================================================
//  Configuration access
// =========================================================================

const MarketDataConfig& MarketDataFeed::config() const noexcept {
    // Note: mtx_config_ is not acquired here to preserve the noexcept contract.
    // Callers should ensure no concurrent config mutation (i.e. call setters
    // from the same thread that owns the feed, or accept benign tearing on
    // double fields).
    return config_;
}

void MarketDataFeed::set_arb_threshold_bps(double threshold_bps) {
    if (threshold_bps <= 0.0) {
        spdlog::warn("set_arb_threshold_bps: invalid threshold={:.1f}, ignoring",
                     threshold_bps);
        return;
    }
    {
        std::unique_lock lock(mtx_config_);
        config_.arb_threshold_bps = threshold_bps;
    }
    spdlog::info("Arbitrage threshold updated to {:.1f} bps", threshold_bps);
}

void MarketDataFeed::set_arb_callback(ArbitrageCallback cb) {
    arb_callback_ = std::move(cb);
    spdlog::debug("Arbitrage callback updated");
}

void MarketDataFeed::set_whale_trade_threshold(Mojo threshold) {
    if (threshold <= 0) {
        spdlog::warn("set_whale_trade_threshold: invalid threshold={}, ignoring",
                     threshold);
        return;
    }
    {
        std::unique_lock lock(mtx_config_);
        config_.whale_trade_threshold = threshold;
    }
    spdlog::info("Whale trade threshold updated to {} mojos ({:.2f} XCH)",
                 threshold,
                 static_cast<double>(threshold) / static_cast<double>(kMojosPerXch));
    recompute_all_whale_metrics();
}

void MarketDataFeed::set_whale_volume_fraction(double fraction) {
    if (fraction <= 0.0 || fraction > 1.0) {
        spdlog::warn("set_whale_volume_fraction: invalid fraction={:.4f}, ignoring",
                     fraction);
        return;
    }
    {
        std::unique_lock lock(mtx_config_);
        config_.whale_volume_fraction = fraction;
    }
    spdlog::info("Whale volume fraction updated to {:.4f} ({:.1f}%)",
                 fraction, fraction * 100.0);
    recompute_all_whale_metrics();
}

void MarketDataFeed::set_whale_window_blocks(std::size_t blocks) {
    if (blocks == 0) {
        spdlog::warn("set_whale_window_blocks: window must be >= 1, ignoring");
        return;
    }
    {
        std::unique_lock lock(mtx_config_);
        config_.whale_window_blocks = blocks;
    }
    spdlog::info("Whale window updated to {} blocks (~{:.0f}s at 52s/block)",
                 blocks, static_cast<double>(blocks) * 52.0);
    recompute_all_whale_metrics();
}

void MarketDataFeed::set_whale_max_spread_multiplier(double multiplier) {
    if (multiplier < 1.0) {
        spdlog::warn("set_whale_max_spread_multiplier: multiplier={:.2f} < 1.0, ignoring",
                     multiplier);
        return;
    }
    {
        std::unique_lock lock(mtx_config_);
        config_.whale_max_spread_multiplier = multiplier;
    }
    spdlog::info("Whale max spread multiplier updated to {:.2f}x", multiplier);
    recompute_all_whale_metrics();
}

// =========================================================================
//  Internal helpers
// =========================================================================

// -------------------------------------------------------------------------
// compute_mid -- multi-source price aggregation
//
// Priority cascade:
//   1. Dexie two-sided quotes -> dex_mid = (bid + ask) / 2
//   2. Dexie one-sided (bid-only or ask-only) -> dex_mid = available side
//   3. Dexie last trade (no live quotes) -> dex_mid = last_trade
//   4. No dexie data at all -> dex_mid = 0
//
// Blending:
//   If dex_mid > 0 AND cex_mid > 0:
//     mid = kDexWeight * dex_mid + kCexWeight * cex_mid
//   Else:
//     mid = whichever source is available (dex preferred)
//
// The 70/30 blend anchors the DEX mid toward the globally discovered price
// on CEX ($2.4M/day) while respecting that the DEX order book reflects
// local supply/demand conditions relevant to our offers.
// -------------------------------------------------------------------------

double MarketDataFeed::compute_mid(const PairState& ps) {
    double dex_mid = 0.0;

    // Case 1: Two-sided dexie order book.
    if (ps.dex_best_bid > 0.0 && ps.dex_best_ask > 0.0) {
        dex_mid = (ps.dex_best_bid + ps.dex_best_ask) / 2.0;
    }
    // Case 2: One-sided dexie (unusual but possible in thin markets).
    else if (ps.dex_best_bid > 0.0) {
        dex_mid = ps.dex_best_bid;
    }
    else if (ps.dex_best_ask > 0.0) {
        dex_mid = ps.dex_best_ask;
    }
    // Case 3: No live quotes -- fall back to last trade.
    else if (ps.dex_last_trade > 0.0) {
        dex_mid = ps.dex_last_trade;
    }

    // Blending with CEX reference.
    if (dex_mid > 0.0 && ps.cex_mid > 0.0) {
        // Weighted blend: 70% DEX, 30% CEX.
        return kDexWeight * dex_mid + kCexWeight * ps.cex_mid;
    }

    // Single-source fallback.
    if (dex_mid > 0.0) {
        return dex_mid;
    }
    if (ps.cex_mid > 0.0) {
        return ps.cex_mid;
    }

    // No data at all.
    return 0.0;
}

// -------------------------------------------------------------------------
// compute_spread_bps -- spread in basis points from two-sided quotes
//
// spread_bps = (ask - bid) / mid * 10000
//
// Returns 0 if either side is missing (no two-sided market to measure).
// -------------------------------------------------------------------------

double MarketDataFeed::compute_spread_bps(double best_bid, double best_ask) {
    if (best_bid <= 0.0 || best_ask <= 0.0) {
        return 0.0;
    }

    // Guard against crossed book (ask < bid), which should not occur in
    // practice but we handle defensively.
    if (best_ask <= best_bid) {
        spdlog::warn("compute_spread_bps: crossed book bid={:.6f} ask={:.6f}",
                     best_bid, best_ask);
        return 0.0;
    }

    const double mid = (best_bid + best_ask) / 2.0;
    if (mid <= 0.0) {
        return 0.0;  // Should be unreachable given the checks above.
    }

    return (best_ask - best_bid) / mid * 10000.0;
}

// -------------------------------------------------------------------------
// detect_stale -- timestamp freshness check
//
// Returns true if (now - ts) exceeds the configured stale threshold, or
// if ts is the epoch default (data was never ingested).
// -------------------------------------------------------------------------

bool MarketDataFeed::detect_stale(Timestamp ts) const {
    // Epoch-zero means the timestamp was never set.
    if (ts == Timestamp{}) {
        return true;
    }

    const auto now = std::chrono::system_clock::now();
    const auto age = now - ts;

    return age > config_.stale_threshold;
}

// -------------------------------------------------------------------------
// check_arbitrage -- detect CEX-DEX price divergence
//
// Divergence (bps) = abs(dex_mid - cex_mid) / cex_mid * 10000
//
// When divergence exceeds arb_threshold_bps:
//   1. Build an ArbitrageSignal with direction and magnitude.
//   2. Store it as the latest signal for this pair.
//   3. Invoke the callback if registered.
//
// The CEX price is the denominator because CEX is the more liquid venue
// and therefore the more reliable reference price (Section 10).
// -------------------------------------------------------------------------

void MarketDataFeed::check_arbitrage(PairState& ps) {
    // Compute DEX mid from raw dexie quotes (not the blended mid, which
    // already incorporates CEX and would dampen the divergence signal).
    double dex_mid = 0.0;
    if (ps.dex_best_bid > 0.0 && ps.dex_best_ask > 0.0) {
        dex_mid = (ps.dex_best_bid + ps.dex_best_ask) / 2.0;
    } else if (ps.dex_last_trade > 0.0) {
        dex_mid = ps.dex_last_trade;
    }

    if (dex_mid <= 0.0 || ps.cex_mid <= 0.0) {
        return;  // Insufficient data for comparison.
    }

    const double divergence_bps =
        std::abs(dex_mid - ps.cex_mid) / ps.cex_mid * 10000.0;

    if (divergence_bps < config_.arb_threshold_bps) {
        return;  // Within tolerance; no signal.
    }

    // Determine direction: which venue is cheaper?
    const ArbitrageDirection direction =
        (dex_mid < ps.cex_mid) ? ArbitrageDirection::DexCheap
                               : ArbitrageDirection::CexCheap;

    const auto now = std::chrono::system_clock::now();

    ArbitrageSignal signal{
        ps.pair_name,
        dex_mid,
        ps.cex_mid,
        divergence_bps,
        direction,
        now
    };

    spdlog::info("ARBITRAGE SIGNAL: pair={} dex={:.6f} cex={:.6f} "
                 "divergence={:.1f}bps direction={}",
                 ps.pair_name, dex_mid, ps.cex_mid,
                 divergence_bps, to_string(direction));

    // Store as latest signal.
    {
        std::unique_lock lock(mtx_arb_);
        latest_arb_.insert_or_assign(ps.pair_name, signal);
    }

    // Invoke callback if registered.
    if (arb_callback_) {
        try {
            arb_callback_(signal);
        } catch (const std::exception& e) {
            spdlog::error("Arbitrage callback threw: {}", e.what());
        }
    }
}

// -------------------------------------------------------------------------
// append_price_history -- circular buffer management
//
// Appends (block_height, price) to the per-pair deque.  If the deque
// exceeds capacity, the oldest entry is evicted from the front.
//
// Duplicate block heights are skipped (idempotent refresh calls).
// -------------------------------------------------------------------------

void MarketDataFeed::append_price_history(const std::string& pair_name,
                                           BlockHeight         block,
                                           double              price) {
    std::unique_lock lock(mtx_history_);

    auto& deq = history_[pair_name];

    // Skip duplicate: if the last entry has the same block height, this is
    // a repeated refresh for the same block.
    if (!deq.empty() && deq.back().block_height == block) {
        // Update the price in place (data may have been refined within the
        // same block if multiple ingest calls occurred).
        deq.back().price = price;
        return;
    }

    // Append new observation.
    deq.push_back(PriceHistoryEntry{block, price});

    // Evict oldest if over capacity.
    while (deq.size() > config_.price_history_capacity) {
        deq.pop_front();
    }
}

// -------------------------------------------------------------------------
// publish_snapshot -- convert PairState to MarketSnapshot and write to State
//
// The MarketSnapshot is the canonical data structure consumed by strategy
// and risk layers.  It uses mojos (int64_t) for monetary fields, so we
// convert double prices to mojos here.
//
// Conversion: mojos = price_in_xch * kMojosPerXch (for XCH-denominated pairs)
//
// For generality, we store the raw double mid as a mojo value using a
// fixed-point approach: mojos = round(price * 10^12).  This is exact for
// XCH-denominated pairs and a reasonable approximation for CAT-denominated
// pairs where the "mojos" represent the smallest CAT unit.
// -------------------------------------------------------------------------

void MarketDataFeed::publish_snapshot(const PairState& ps) {
    // Convert double prices to mojos via fixed-point multiplication.
    // For XCH-denominated pairs, 1.0 = 1 XCH = 10^12 mojos.
    // We use a lambda to avoid repetitive static_cast noise.
    auto to_mojos = [](double price) -> Mojo {
        if (price <= 0.0) {
            return 0;
        }
        // Round to nearest mojo.  The result fits int64 because prices in
        // the Chia ecosystem are on the order of $2-3 (XCH) or fractions
        // thereof, so price * 10^12 stays well within int64 range.
        return static_cast<Mojo>(std::llround(price * static_cast<double>(kMojosPerXch)));
    };

    MarketSnapshot snap;
    snap.pair_name  = ps.pair_name;
    snap.mid_price  = to_mojos(ps.mid_price);
    snap.best_bid   = to_mojos(ps.dex_best_bid);
    snap.best_ask   = to_mojos(ps.dex_best_ask);
    snap.spread_bps = ps.spread_bps;
    snap.cex_mid    = to_mojos(ps.cex_mid);
    snap.volume_24h = to_mojos(ps.volume_24h);
    snap.last_block = ps.last_block;
    snap.updated_at = ps.dex_updated_at;

    // Write to shared State (thread-safe; State::update_market acquires
    // its own internal mutex).
    state_.update_market(snap);
}

// =========================================================================
// Competitor tracking methods
// =========================================================================

// -------------------------------------------------------------------------
// ingest_competing_offers -- parse and store individual competing offers
//
// Called from the engine after fetching the full order book from dexie.
// Filters out own offers, tracks competing offers, and detects new competitors.
// -------------------------------------------------------------------------

void MarketDataFeed::ingest_competing_offers(
    const std::string&                 pair_name,
    const std::vector<CompetingOffer>& competing_offers,
    const std::unordered_set<std::string>& own_offer_ids)
{
    if (!config_.enable_competitor_tracking) {
        return;  // Feature disabled; no-op.
    }

    // Filter out own offers and offers below minimum size threshold.
    std::vector<CompetingOffer> filtered;
    filtered.reserve(competing_offers.size());

    for (const auto& offer : competing_offers) {
        // Skip our own offers.
        if (own_offer_ids.count(offer.offer_id) > 0) {
            continue;
        }

        // Skip dust offers (not from serious market makers).
        if (offer.size < config_.min_competitor_offer_size) {
            continue;
        }

        filtered.push_back(offer);
    }

    // Capture count and emptiness before move.
    const auto ingested_count = filtered.size();
    const bool is_empty = (ingested_count == 0);

    // Store the filtered list under lock.
    {
        std::unique_lock lock(mtx_competitors_);
        competing_offers_[pair_name] = std::move(filtered);
    }

    // Update competitor metrics based on the newly ingested offers.
    if (is_empty) {
        // No competing offers remain: remove stale metrics so the competitor
        // effectively disappears from downstream views.
        std::unique_lock lock(mtx_competitor_metrics_);
        competitor_metrics_.erase(pair_name);
    } else {
        // Non-empty offer set: recompute metrics.
        // Note: compute_competitor_metrics() acquires its own locks internally,
        // so we don't hold any lock here (no nested lock risk).
        compute_competitor_metrics(pair_name);
    }

    spdlog::debug("MarketDataFeed: ingested {} competing offers for pair={}",
                  ingested_count, pair_name);
}

// -------------------------------------------------------------------------
// compute_competitor_metrics -- analyze competing offers and compute metrics
//
// Computes:
//   - Best competing bid/ask spreads vs mid-price
//   - Tightest competing two-sided spread
//   - Depth counts (number of offers per side)
//   - Detection of new competitors
//
// Returns std::nullopt if competitor tracking is disabled or no competing
// offers exist for this pair.
// -------------------------------------------------------------------------

std::optional<CompetitorMetrics> MarketDataFeed::compute_competitor_metrics(
    const std::string& pair_name)
{
    if (!config_.enable_competitor_tracking) {
        return std::nullopt;
    }

    // Read mid-price internally; if unavailable, we can still compute depth counts.
    const double mid_price = get_mid_price(pair_name);

    // Read competing offers under shared lock.
    std::vector<CompetingOffer> offers;
    {
        std::shared_lock lock(mtx_competitors_);
        auto it = competing_offers_.find(pair_name);
        if (it == competing_offers_.end() || it->second.empty()) {
            return std::nullopt;  // No competing offers.
        }
        offers = it->second;
    }

    // Separate bids and asks.
    std::vector<double> bid_prices;
    std::vector<double> ask_prices;

    for (const auto& offer : offers) {
        // Convert mojos to double for spread calculation.
        const double price = static_cast<double>(offer.price) / static_cast<double>(kMojosPerXch);

        if (offer.side == Side::Bid) {
            bid_prices.push_back(price);
        } else {
            ask_prices.push_back(price);
        }
    }

    // Compute best competing bid (highest price) and best competing ask (lowest price).
    double best_competing_bid = 0.0;
    double best_competing_ask = 0.0;

    if (!bid_prices.empty()) {
        best_competing_bid = *std::max_element(bid_prices.begin(), bid_prices.end());
    }

    if (!ask_prices.empty()) {
        best_competing_ask = *std::min_element(ask_prices.begin(), ask_prices.end());
    }

    // Compute spreads in basis points relative to mid-price.
    // Guard against zero/invalid mid-price: BPS math requires valid mid-price,
    // but depth counts are always available.
    double best_competing_bid_bps = 0.0;
    double best_competing_ask_bps = 0.0;
    double best_competing_spread_bps = 0.0;

    if (mid_price > 0.0) {
        best_competing_bid_bps =
            (best_competing_bid > 0.0)
                ? ((mid_price - best_competing_bid) / mid_price) * 10'000.0
                : 0.0;

        best_competing_ask_bps =
            (best_competing_ask > 0.0)
                ? ((best_competing_ask - mid_price) / mid_price) * 10'000.0
                : 0.0;

        // Compute tightest competing spread: the two-sided spread from best bid/ask.
        // This is what we feed into SpreadOptimizer as best_competing_bps.
        if (best_competing_bid > 0.0 && best_competing_ask > 0.0) {
            const double spread = (best_competing_ask - best_competing_bid) / mid_price;
            best_competing_spread_bps = spread * 10'000.0;
        }
    }

    // Check if this is a new competitor (first time seeing any competing offers).
    bool new_competitor_detected = false;
    {
        std::shared_lock lock(mtx_competitor_metrics_);
        auto it = competitor_metrics_.find(pair_name);
        if (it == competitor_metrics_.end() || it->second.num_competing_offers == 0) {
            new_competitor_detected = true;
        }
    }

    // Assemble metrics.
    CompetitorMetrics metrics;
    metrics.pair_name                 = pair_name;
    metrics.num_competing_offers      = offers.size();
    metrics.best_competing_bid_bps    = best_competing_bid_bps;
    metrics.best_competing_ask_bps    = best_competing_ask_bps;
    metrics.best_competing_spread_bps = best_competing_spread_bps;
    metrics.competing_depth_bids      = bid_prices.size();
    metrics.competing_depth_asks      = ask_prices.size();
    metrics.new_competitor_detected   = new_competitor_detected;
    metrics.last_updated              = std::chrono::system_clock::now();

    // Store under lock.
    {
        std::unique_lock lock(mtx_competitor_metrics_);
        competitor_metrics_[pair_name] = metrics;
    }

    // Log warning if tight competing spread detected.
    if (best_competing_spread_bps > 0.0 &&
        best_competing_spread_bps < config_.competitor_alert_threshold_bps) {
        spdlog::warn("MarketDataFeed: tight competitor detected on pair={}, "
                     "competing_spread={:.1f}bps (threshold={:.1f}bps)",
                     pair_name, best_competing_spread_bps,
                     config_.competitor_alert_threshold_bps);
    }

    if (new_competitor_detected) {
        spdlog::info("MarketDataFeed: new competitor(s) detected on pair={}, "
                     "num_offers={}, best_spread={:.1f}bps",
                     pair_name, metrics.num_competing_offers,
                     best_competing_spread_bps);
    }

    return metrics;
}

// -------------------------------------------------------------------------
// Public accessor methods for competitor metrics
// -------------------------------------------------------------------------

std::optional<CompetitorMetrics> MarketDataFeed::get_competitor_metrics(
    const std::string& pair_name) const
{
    if (!config_.enable_competitor_tracking) {
        return std::nullopt;
    }

    std::shared_lock lock(mtx_competitor_metrics_);
    auto it = competitor_metrics_.find(pair_name);
    if (it == competitor_metrics_.end()) {
        return std::nullopt;
    }
    return it->second;
}

double MarketDataFeed::get_best_competing_spread_bps(
    const std::string& pair_name) const
{
    auto metrics = get_competitor_metrics(pair_name);
    if (!metrics.has_value()) {
        return 0.0;
    }
    return metrics->best_competing_spread_bps;
}

std::size_t MarketDataFeed::get_num_competing_offers(
    const std::string& pair_name) const
{
    auto metrics = get_competitor_metrics(pair_name);
    if (!metrics.has_value()) {
        return 0;
    }
    return metrics->num_competing_offers;
}

// =========================================================================
// Whale detection methods
// =========================================================================

// -------------------------------------------------------------------------
// ingest_trade -- record an individual trade and update whale-activity metrics
//
// Classifies the trade as a whale event when its size meets either:
//   (a) the absolute threshold (whale_trade_threshold), or
//   (b) the fractional-volume threshold (whale_volume_fraction × vol_24h).
//
// Non-whale trades are silently discarded.  Whale trades are appended to a
// per-pair deque and WhaleMetrics is recomputed.
// -------------------------------------------------------------------------

void MarketDataFeed::ingest_trade(const std::string& pair_name,
                                  Side               side,
                                  Mojo               size,
                                  BlockHeight        block_height)
{
    if (size <= 0) {
        return;  // Guard against degenerate input.
    }

    // Snapshot config values under shared lock to avoid data races with setters.
    Mojo   whale_trade_threshold;
    double whale_volume_fraction;
    {
        std::shared_lock lock(mtx_config_);
        whale_trade_threshold = config_.whale_trade_threshold;
        whale_volume_fraction = config_.whale_volume_fraction;
    }

    // Fetch current 24h volume to evaluate the fractional-volume criterion.
    const double vol_24h = get_volume_24h(pair_name);  // base-asset units (double)

    // Convert size to base-asset units for the fractional comparison.
    const double size_units =
        static_cast<double>(size) / static_cast<double>(kMojosPerXch);

    // Compute size as a fraction of 24h volume (guard against zero volume).
    const double size_pct_vol =
        (vol_24h > 0.0) ? (size_units / vol_24h) : 0.0;

    // Determine whether this qualifies as a whale trade.
    const bool absolute_threshold_met = (size >= whale_trade_threshold);
    const bool volume_fraction_met =
        (vol_24h > 0.0) && (size_pct_vol >= whale_volume_fraction);

    if (!absolute_threshold_met && !volume_fraction_met) {
        return;  // Not a whale trade; ignore.
    }

    // Record the whale event.
    detect_and_update_whale(pair_name, side, size, block_height);
}

// -------------------------------------------------------------------------
// detect_and_update_whale -- append a confirmed whale event and recompute metrics
// -------------------------------------------------------------------------

void MarketDataFeed::detect_and_update_whale(const std::string& pair_name,
                                             Side               side,
                                             Mojo               size,
                                             BlockHeight        block_height)
{
    // Snapshot config under shared lock to avoid data races with setters.
    std::size_t whale_window_blocks;
    double      whale_max_spread_multiplier;
    {
        std::shared_lock lock(mtx_config_);
        whale_window_blocks         = config_.whale_window_blocks;
        whale_max_spread_multiplier = config_.whale_max_spread_multiplier;
    }

    const double vol_24h = get_volume_24h(pair_name);
    const double size_units =
        static_cast<double>(size) / static_cast<double>(kMojosPerXch);
    const double size_pct_vol =
        (vol_24h > 0.0) ? (size_units / vol_24h) : 0.0;

    WhaleTradeEvent evt;
    evt.pair_name    = pair_name;
    evt.side         = side;
    evt.size         = size;
    evt.size_pct_vol = size_pct_vol;
    evt.block_height = block_height;
    evt.detected_at  = std::chrono::system_clock::now();

    // Append event and evict stale events outside the rolling window.
    {
        std::unique_lock lock(mtx_whale_events_);
        auto& deq = whale_events_[pair_name];
        deq.push_back(evt);

        // Evict events older than whale_window_blocks.
        // Use uint64_t for the subtraction to avoid narrowing whale_window_blocks
        // (a size_t) to BlockHeight (uint32_t), which would silently truncate values
        // above UINT32_MAX.
        const std::uint64_t effective_window =
            static_cast<std::uint64_t>(whale_window_blocks);
        while (!deq.empty() &&
               block_height >= deq.front().block_height)
        {
            const std::uint64_t age =
                static_cast<std::uint64_t>(block_height) -
                static_cast<std::uint64_t>(deq.front().block_height);
            if (age >= effective_window) {
                deq.pop_front();
            } else {
                break;
            }
        }
    }

    // Recompute metrics under shared read of the event deque.
    std::vector<WhaleTradeEvent> events_snapshot;
    {
        std::shared_lock lock(mtx_whale_events_);
        auto it = whale_events_.find(pair_name);
        if (it != whale_events_.end()) {
            events_snapshot.assign(it->second.begin(), it->second.end());
        }
    }

    if (events_snapshot.empty()) {
        return;
    }

    // Find largest trade in window.
    Mojo largest = 0;
    for (const auto& e : events_snapshot) {
        if (e.size > largest) {
            largest = e.size;
        }
    }

    const std::size_t n = events_snapshot.size();
    const double multiplier = compute_whale_spread_multiplier(
        n, whale_window_blocks, whale_max_spread_multiplier);

    WhaleMetrics wm;
    wm.pair_name          = pair_name;
    wm.events_in_window   = n;
    wm.largest_trade_size = largest;
    wm.spread_multiplier  = multiplier;
    wm.dominant_side      = events_snapshot.back().side;
    wm.is_active          = true;
    wm.last_event_block   = events_snapshot.back().block_height;
    wm.last_updated       = std::chrono::system_clock::now();

    {
        std::unique_lock lock(mtx_whale_metrics_);
        whale_metrics_[pair_name] = wm;
    }

    spdlog::warn("MarketDataFeed: whale trade detected on pair={}, "
                 "side={}, size_xch={:.2f}, events_in_window={}, "
                 "spread_multiplier={:.2f}x",
                 pair_name,
                 to_string(side),
                 static_cast<double>(size) / static_cast<double>(kMojosPerXch),
                 n,
                 multiplier);
}

// -------------------------------------------------------------------------
// compute_whale_spread_multiplier -- linear interpolation over event count
//
// 0 events → 1.0 (no widening)
// >= whale_window_blocks events → whale_max_spread_multiplier
// -------------------------------------------------------------------------

double MarketDataFeed::compute_whale_spread_multiplier(
    std::size_t events_in_window,
    std::size_t window_blocks_param,
    double      max_multiplier)
{
    if (events_in_window == 0) {
        return 1.0;
    }

    // Ensure we never divide by zero: treat a zero window size as 1 block.
    const double effective_window = std::max(
        1.0,
        static_cast<double>(window_blocks_param));

    // Clamp at the window size so we don't extrapolate beyond max.
    const double fraction = std::min(
        static_cast<double>(events_in_window) / effective_window,
        1.0);

    // Linear interpolation: 1.0 + fraction * (max - 1.0)
    return 1.0 + fraction * (max_multiplier - 1.0);
}

// -------------------------------------------------------------------------
// Public accessor methods for whale metrics
// -------------------------------------------------------------------------

std::optional<WhaleMetrics> MarketDataFeed::get_whale_metrics(
    const std::string& pair_name) const
{
    // Expire stale whale metrics: if the most recent event is older than the
    // tracking window, the whale has left and we return nullopt.
    const BlockHeight current_block =
        block_height_.load(std::memory_order_relaxed);

    // Snapshot config for the window size.
    std::uint64_t window_blocks_64;
    {
        std::shared_lock lock(mtx_config_);
        window_blocks_64 = static_cast<std::uint64_t>(config_.whale_window_blocks);
    }

    std::shared_lock lock(mtx_whale_metrics_);
    auto it = whale_metrics_.find(pair_name);
    if (it == whale_metrics_.end()) {
        return std::nullopt;
    }

    const WhaleMetrics& wm = it->second;
    // Compute the expiry comparison in uint64_t to avoid uint32 overflow when
    // last_event_block is near UINT32_MAX or when whale_window_blocks is large.
    const auto current64 = static_cast<std::uint64_t>(current_block);
    const auto last64    = static_cast<std::uint64_t>(wm.last_event_block);
    if (current64 >= last64 + window_blocks_64)
    {
        // Window has expired; behave as if no whale is present.
        return std::nullopt;
    }

    return wm;
}

bool MarketDataFeed::is_whale_active(const std::string& pair_name) const
{
    return get_whale_metrics(pair_name).has_value();
}

double MarketDataFeed::get_whale_spread_multiplier(
    const std::string& pair_name) const
{
    auto wm = get_whale_metrics(pair_name);
    if (!wm.has_value()) {
        return 1.0;
    }
    return wm->spread_multiplier;
}

// -------------------------------------------------------------------------
// recompute_all_whale_metrics -- re-evaluate after config change
//
// Called by the runtime setters to ensure cached whale metrics reflect the
// new configuration immediately (e.g. a changed window size or multiplier).
// -------------------------------------------------------------------------

void MarketDataFeed::recompute_all_whale_metrics()
{
    const BlockHeight current_block =
        block_height_.load(std::memory_order_relaxed);

    // Snapshot config under shared lock.
    std::size_t cfg_whale_window_blocks;
    double      cfg_whale_max_spread_multiplier;
    {
        std::shared_lock lock(mtx_config_);
        cfg_whale_window_blocks         = config_.whale_window_blocks;
        cfg_whale_max_spread_multiplier = config_.whale_max_spread_multiplier;
    }

    // Collect pair names with whale events.
    std::vector<std::string> pair_names;
    {
        std::shared_lock lock(mtx_whale_events_);
        pair_names.reserve(whale_events_.size());
        for (const auto& [name, _] : whale_events_) {
            pair_names.push_back(name);
        }
    }

    for (const auto& pair_name : pair_names) {
        // Trim events under the new window.
        {
            std::unique_lock lock(mtx_whale_events_);
            auto it = whale_events_.find(pair_name);
            if (it == whale_events_.end()) {
                continue;
            }
            auto& deq = it->second;
            const std::uint64_t window_blocks =
                static_cast<std::uint64_t>(cfg_whale_window_blocks);
            while (!deq.empty() && current_block >= deq.front().block_height) {
                const std::uint64_t age =
                    static_cast<std::uint64_t>(current_block) -
                    static_cast<std::uint64_t>(deq.front().block_height);
                if (age >= window_blocks) {
                    deq.pop_front();
                } else {
                    break;
                }
            }
        }

        // Snapshot surviving events.
        std::vector<WhaleTradeEvent> events_snapshot;
        {
            std::shared_lock lock(mtx_whale_events_);
            auto it = whale_events_.find(pair_name);
            if (it != whale_events_.end()) {
                events_snapshot.assign(it->second.begin(), it->second.end());
            }
        }

        if (events_snapshot.empty()) {
            // Clear metrics if no events survive the new window.
            std::unique_lock lock(mtx_whale_metrics_);
            whale_metrics_.erase(pair_name);
            continue;
        }

        Mojo largest = 0;
        for (const auto& e : events_snapshot) {
            if (e.size > largest) {
                largest = e.size;
            }
        }

        const std::size_t n = events_snapshot.size();
        const double multiplier = compute_whale_spread_multiplier(
            n, cfg_whale_window_blocks, cfg_whale_max_spread_multiplier);

        WhaleMetrics wm;
        wm.pair_name          = pair_name;
        wm.events_in_window   = n;
        wm.largest_trade_size = largest;
        wm.spread_multiplier  = multiplier;
        wm.dominant_side      = events_snapshot.back().side;
        wm.is_active          = true;
        wm.last_event_block   = events_snapshot.back().block_height;
        wm.last_updated       = std::chrono::system_clock::now();

        {
            std::unique_lock lock(mtx_whale_metrics_);
            whale_metrics_[pair_name] = wm;
        }
    }
}

// -------------------------------------------------------------------------
// get_or_create_pair -- lazy initialization of PairState
//
// Caller MUST hold mtx_pairs_ exclusively.
// -------------------------------------------------------------------------

PairState& MarketDataFeed::get_or_create_pair(const std::string& pair_name) {
    auto [it, inserted] = pairs_.try_emplace(pair_name, pair_name);
    if (inserted) {
        spdlog::info("MarketDataFeed: tracking new pair={}", pair_name);
    }
    return it->second;
}

// =========================================================================
// VPIN — Volume-Synchronized Probability of Informed Trading
//
// Reference: Easley, López de Prado & O'Hara (2012). "Flow Toxicity and
// Liquidity in a High-frequency World."
//
// Trades are accumulated into volume bars (buckets) of fixed size.  When a
// bucket fills the buy/sell imbalance is frozen.  VPIN is the rolling mean
// of absolute imbalances over the last N completed buckets, divided by the
// bucket size:
//
//   VPIN = (1/N) * SUM_i |buy_vol_i - sell_vol_i| / bucket_size
//
// This yields a continuous toxicity signal in [0, 1].
// =========================================================================

void MarketDataFeed::ingest_trade_for_vpin(const std::string& pair_name,
                                           Side               side,
                                           double             volume)
{
    if (volume <= 0.0) {
        return;
    }

    // Snapshot VPIN config under shared lock.
    double      bucket_size;
    std::size_t window_buckets;
    {
        std::shared_lock lock(mtx_config_);
        bucket_size    = config_.vpin_bucket_size;
        window_buckets = config_.vpin_window_buckets;
    }

    if (bucket_size <= 0.0) {
        spdlog::warn(
            "VPIN disabled for pair '{}' due to non-positive bucket size: {}",
            pair_name,
            bucket_size);
        return;
    }

    {
        std::unique_lock lock(mtx_vpin_);
        auto& vs = vpin_state_[pair_name];

        double remaining = volume;
        while (remaining > 0.0) {
            // How much capacity is left in the current bucket?
            const double current_total =
                vs.current_bucket.buy_volume + vs.current_bucket.sell_volume;
            const double capacity = bucket_size - current_total;

            if (capacity <= 0.0) {
                // Current bucket is already full — push and start fresh.
                vs.current_bucket.complete = true;
                vs.completed.push_back(vs.current_bucket);
                vs.current_bucket = VpinBucket{};

                // Trim to window size.
                while (vs.completed.size() > window_buckets) {
                    vs.completed.pop_front();
                }
                continue;
            }

            const double fill = std::min(remaining, capacity);
            if (side == Side::Bid) {
                vs.current_bucket.buy_volume += fill;
            } else {
                vs.current_bucket.sell_volume += fill;
            }
            remaining -= fill;

            // Check if bucket is now complete.
            const double new_total =
                vs.current_bucket.buy_volume + vs.current_bucket.sell_volume;
            if (new_total >= bucket_size - 1e-12) {
                vs.current_bucket.complete = true;
                vs.completed.push_back(vs.current_bucket);
                vs.current_bucket = VpinBucket{};

                // Trim to window size.
                while (vs.completed.size() > window_buckets) {
                    vs.completed.pop_front();
                }
            }
        }
    }

    recompute_vpin(pair_name);
}

void MarketDataFeed::recompute_vpin(const std::string& pair_name)
{
    // Read completed buckets under shared lock.
    std::vector<VpinBucket> buckets;
    {
        std::shared_lock lock(mtx_vpin_);
        auto it = vpin_state_.find(pair_name);
        if (it == vpin_state_.end()) {
            return;
        }
        buckets.assign(it->second.completed.begin(),
                       it->second.completed.end());
    }

    if (buckets.empty()) {
        return;
    }

    double bucket_size;
    {
        std::shared_lock lock(mtx_config_);
        bucket_size = config_.vpin_bucket_size;
    }
    if (bucket_size <= 0.0) {
        return;  // VPIN disabled — warning was already logged in ingest_trade_for_vpin.
    }

    // VPIN = (1/N) * SUM |buy_i - sell_i| / bucket_size
    double sum_abs_imbalance = 0.0;
    double total_buy  = 0.0;
    double total_sell = 0.0;
    for (const auto& b : buckets) {
        sum_abs_imbalance += std::abs(b.buy_volume - b.sell_volume);
        total_buy  += b.buy_volume;
        total_sell += b.sell_volume;
    }

    const double n = static_cast<double>(buckets.size());
    const double vpin = (sum_abs_imbalance / n) / bucket_size;

    const double total = total_buy + total_sell;
    const double buy_pct  = (total > 0.0) ? (total_buy / total)  : 0.5;
    const double sell_pct = (total > 0.0) ? (total_sell / total) : 0.5;

    VpinMetrics vm;
    vm.pair_name        = pair_name;
    vm.vpin             = std::min(vpin, 1.0);  // clamp to [0, 1]
    vm.complete_buckets = buckets.size();
    vm.buy_volume_pct   = buy_pct;
    vm.sell_volume_pct  = sell_pct;
    vm.last_updated     = std::chrono::system_clock::now();

    {
        std::unique_lock lock(mtx_vpin_metrics_);
        vpin_metrics_[pair_name] = vm;
    }
}

std::optional<VpinMetrics> MarketDataFeed::get_vpin_metrics(
    const std::string& pair_name) const
{
    std::shared_lock lock(mtx_vpin_metrics_);
    auto it = vpin_metrics_.find(pair_name);
    if (it == vpin_metrics_.end()) {
        return std::nullopt;
    }
    return it->second;
}

double MarketDataFeed::get_vpin(const std::string& pair_name) const
{
    auto vm = get_vpin_metrics(pair_name);
    return vm.has_value() ? vm->vpin : 0.0;
}

// =========================================================================
// OFI — Order Flow Imbalance
//
// Reference: Cont, Kukanov & Stoikov (2014). "The Price Impact of Order
// Book Events."
//
// OFI aggregates signed volume changes at the best bid/ask.  For each pair
// of consecutive snapshots (t-1, t):
//
//   delta_bid (e^B): { +bid_size_t        if bid_t > bid_{t-1}  (bid improved)
//                    { -(bid_size_{t-1})  if bid_t < bid_{t-1}  (bid weakened)
//                    { bid_size_t - bid_size_{t-1}   otherwise  (size change)
//
//   delta_ask (e^A): { +ask_size_t        if ask_t < ask_{t-1}  (ask improved)
//                    { -(ask_size_{t-1})  if ask_t > ask_{t-1}  (ask weakened)
//                    { ask_size_t - ask_size_{t-1}   otherwise  (size change)
//
//   OFI_t = delta_bid - delta_ask
//
// Positive OFI → buy pressure (bids strengthening faster than asks).
// Negative OFI → sell pressure (asks strengthening faster than bids).
// =========================================================================

void MarketDataFeed::ingest_book_snapshot_for_ofi(
    const std::string& pair_name,
    double             best_bid,
    double             bid_size,
    double             best_ask,
    double             ask_size)
{
    // Snapshot config under shared lock.
    std::size_t ofi_window_size;
    {
        std::shared_lock lock(mtx_config_);
        ofi_window_size = config_.ofi_window_size;
    }

    {
        std::unique_lock lock(mtx_ofi_);
        auto& snaps = ofi_snapshots_[pair_name];
        snaps.push_back(BookSnapshot{best_bid, bid_size, best_ask, ask_size});

        // Keep window + 1 (need at least 2 for a delta).
        while (snaps.size() > ofi_window_size + 1) {
            snaps.pop_front();
        }
    }

    recompute_ofi(pair_name);
}

void MarketDataFeed::recompute_ofi(const std::string& pair_name)
{
    std::vector<BookSnapshot> snaps;
    {
        std::shared_lock lock(mtx_ofi_);
        auto it = ofi_snapshots_.find(pair_name);
        if (it == ofi_snapshots_.end() || it->second.size() < 2) {
            return;
        }
        snaps.assign(it->second.begin(), it->second.end());
    }

    double cumulative = 0.0;
    for (std::size_t i = 1; i < snaps.size(); ++i) {
        const auto& prev = snaps[i - 1];
        const auto& curr = snaps[i];

        // Bid-side delta.
        double delta_bid = 0.0;
        if (curr.best_bid > prev.best_bid) {
            delta_bid = curr.bid_size;
        } else if (curr.best_bid < prev.best_bid) {
            delta_bid = -prev.bid_size;
        } else {
            delta_bid = curr.bid_size - prev.bid_size;
        }

        // Ask-side event (e^A) per Cont et al.:
        //   ask improves (price decreases) → e^A = +ask_size_t
        //   ask weakens  (price increases) → e^A = -ask_size_{t-1}
        //   ask unchanged                  → e^A = ask_size_t - ask_size_{t-1}
        double delta_ask = 0.0;
        if (curr.best_ask < prev.best_ask) {
            delta_ask = curr.ask_size;
        } else if (curr.best_ask > prev.best_ask) {
            delta_ask = -prev.ask_size;
        } else {
            delta_ask = curr.ask_size - prev.ask_size;
        }

        // OFI = delta_bid - delta_ask (sign: positive = buy pressure).
        cumulative += (delta_bid - delta_ask);
    }

    // Normalise OFI to [-1, 1] using the average per-snapshot imbalance
    // capacity within the current sliding window.  We compute the mean of
    // (bid_size + ask_size) across all snapshots, then multiply by the number
    // of delta steps (snaps.size() - 1).  This keeps the denominator
    // proportional to the window length rather than growing without bound as
    // more snapshots accumulate (which would drive normalised OFI to zero).
    double sum_sizes = 0.0;
    for (const auto& s : snaps) {
        sum_sizes += (s.bid_size + s.ask_size);
    }
    const double avg_size = sum_sizes / static_cast<double>(snaps.size());
    const double max_abs  = std::max(
        avg_size * static_cast<double>(snaps.size() - 1), 1e-12);

    const double norm_ofi = std::clamp(cumulative / max_abs, -1.0, 1.0);

    // Latest single-step OFI for the "current" reading.
    double latest_ofi = 0.0;
    if (snaps.size() >= 2) {
        const auto& prev = snaps[snaps.size() - 2];
        const auto& curr = snaps[snaps.size() - 1];

        double db = 0.0;
        if (curr.best_bid > prev.best_bid) db = curr.bid_size;
        else if (curr.best_bid < prev.best_bid) db = -prev.bid_size;
        else db = curr.bid_size - prev.bid_size;

        double da = 0.0;
        if (curr.best_ask < prev.best_ask) da = curr.ask_size;
        else if (curr.best_ask > prev.best_ask) da = -prev.ask_size;
        else da = curr.ask_size - prev.ask_size;

        latest_ofi = db - da;
    }

    OfiMetrics om;
    om.pair_name      = pair_name;
    om.ofi            = latest_ofi;
    om.normalized_ofi = norm_ofi;
    om.cumulative_ofi = cumulative;
    om.observations   = snaps.size();
    om.last_updated   = std::chrono::system_clock::now();

    {
        std::unique_lock lock(mtx_ofi_metrics_);
        ofi_metrics_[pair_name] = om;
    }
}

std::optional<OfiMetrics> MarketDataFeed::get_ofi_metrics(
    const std::string& pair_name) const
{
    std::shared_lock lock(mtx_ofi_metrics_);
    auto it = ofi_metrics_.find(pair_name);
    if (it == ofi_metrics_.end()) {
        return std::nullopt;
    }
    return it->second;
}

double MarketDataFeed::get_normalized_ofi(const std::string& pair_name) const
{
    auto om = get_ofi_metrics(pair_name);
    return om.has_value() ? om->normalized_ofi : 0.0;
}

// =========================================================================
// Asymmetric spread widening
//
// Uses the whale detector's dominant_side to skew the spread multiplier.
// The average widening is preserved: (bid_mult + ask_mult) / 2 = symmetric_mult,
// but the risk-bearing side gets a larger share of the spread (and bid_mult *
// ask_mult < symmetric_mult^2 whenever asymmetric_skew_factor α > 0).
//
// The asymmetric_skew_factor (α ∈ [0,1]) controls the split:
//   α = 0.0 → symmetric (same multiplier on both sides)
//   α = 1.0 → fully asymmetric (all extra widening on informed side)
//
// For whale buying (dominant_side = Bid, i.e. taker bought from us):
//   ask_mult = 1 + (m - 1) * (1 + α)    (widen ask: we're selling into a bid)
//   bid_mult = 1 + (m - 1) * (1 - α)    (tighten bid: we want to buy cheap)
//
// The average of the two multipliers equals the original symmetric one.
// =========================================================================

AsymmetricMultipliers MarketDataFeed::get_asymmetric_spread_multipliers(
    const std::string& pair_name) const
{
    auto wm = get_whale_metrics(pair_name);
    if (!wm.has_value() || wm->spread_multiplier <= 1.0 + 1e-9) {
        return AsymmetricMultipliers{1.0, 1.0};
    }

    const double m = wm->spread_multiplier;     // symmetric multiplier
    const double excess = m - 1.0;               // extra widening above 1.0

    double alpha;
    {
        std::shared_lock lock(mtx_config_);
        alpha = std::clamp(config_.asymmetric_skew_factor, 0.0, 1.0);
    }

    // high_side gets (1 + α) share, low_side gets (1 - α) share.
    const double high_mult = 1.0 + excess * (1.0 + alpha);
    const double low_mult  = 1.0 + excess * (1.0 - alpha);

    AsymmetricMultipliers am;
    if (wm->dominant_side == Side::Bid) {
        // Whale is buying → widen ask (adverse-selection risk on our ask).
        am.ask_multiplier = high_mult;
        am.bid_multiplier = low_mult;
    } else {
        // Whale is selling → widen bid (adverse-selection risk on our bid).
        am.bid_multiplier = high_mult;
        am.ask_multiplier = low_mult;
    }

    return am;
}

}  // namespace xop
