// market_data.hpp -- Multi-source market data aggregation for XOPTrader CHIA
//                    DEX market-making bot.
//
// Aggregates price data from three source tiers into unified MarketSnapshot
// objects consumed by the strategy and risk layers:
//
//   1. Dexie API  (primary)  -- order book best bid/ask, recent trades, 24h vol
//   2. Chia Full Node        -- block height, mempool activity
//   3. CEX reference (future)-- mid price from OKX / Gate.io for arb detection
//
// Aggregation priority:
//   - Primary mid: dexie best_bid/ask -> mid = (bid + ask) / 2
//   - Fallback:    if no quotes, use last trade price from dexie
//   - Blended:     if CEX reference available, weighted mid =
//                    0.7 * dex_mid + 0.3 * cex_mid
//                  (CEX is 1000x more liquid; 30% weight anchors the DEX mid
//                   toward the globally discovered price)
//
// Staleness:
//   Data older than kStaleThreshold (5 minutes) is marked stale.  The strategy
//   layer should widen spreads or pause quoting when it observes stale data.
//
// Price history:
//   A circular buffer of (block_height, price) tuples per pair feeds the
//   volatility estimator and regime detector.  Default capacity is 1000 blocks
//   (~14.4 hours at 52 s/block).
//
// Arbitrage signals:
//   When dexie mid diverges from CEX mid beyond a configurable threshold, an
//   ArbitrageSignal is emitted for the strategy layer to act on.
//
// Thread safety:
//   MarketSnapshot updates and per-pair state are guarded by std::shared_mutex.
//   Price history has its own shared_mutex for concurrent reads by volatility
//   and regime estimators.  No method acquires more than one mutex, so deadlock
//   is impossible by construction.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no secrets handled; API endpoints from config only
//   ISO/IEC 5055       -- no raw pointers, RAII locking, bounds-checked buffers
//   ISO/IEC 25000      -- documented interfaces, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard C++20, no undefined behaviour

#ifndef XOP_EXECUTION_MARKET_DATA_HPP
#define XOP_EXECUTION_MARKET_DATA_HPP

#include "xop/types.hpp"
#include "xop/config.hpp"
#include "xop/state.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Data older than this threshold is considered stale and should not be
/// trusted for quote computation.  5 minutes was chosen because the CHIA
/// block time is ~52 seconds; missing ~6 consecutive blocks strongly
/// suggests a connectivity problem or an idle market.
inline constexpr auto kStaleThreshold = std::chrono::minutes{5};

/// Default capacity for the per-pair price history circular buffer.
/// 1000 blocks * 52 s/block = 52,000 s = ~14.4 hours of history.
/// Sufficient for the Yang-Zhang volatility estimator (200-block window)
/// and the variance-ratio regime detector (100-block window) with ample
/// look-back margin.
inline constexpr std::size_t kDefaultPriceHistoryCapacity = 1000;

/// Weight assigned to the DEX mid-price when blending with CEX reference.
/// The remaining (1 - kDexWeight) goes to CEX.  CEX carries 30% because
/// its volume is ~1000x larger (Section 10 of strategy doc: $2.4M/day CEX
/// vs ~$2K/day DEX), so its price discovery is far more authoritative.
inline constexpr double kDexWeight = 0.70;

/// Weight assigned to the CEX mid-price when blending.
inline constexpr double kCexWeight = 1.0 - kDexWeight;  // 0.30

// ---------------------------------------------------------------------------
// ArbitrageDirection -- which venue is cheaper.
// ---------------------------------------------------------------------------

enum class ArbitrageDirection : std::uint8_t {
    DexCheap = 0,  // DEX price < CEX price -- buy on DEX, sell on CEX
    CexCheap = 1   // CEX price < DEX price -- buy on CEX, sell on DEX
};

/// Human-readable label for logging.
inline const char* to_string(ArbitrageDirection d) noexcept {
    return d == ArbitrageDirection::DexCheap ? "DexCheap" : "CexCheap";
}

// ---------------------------------------------------------------------------
// ArbitrageSignal -- emitted when DEX-CEX price divergence exceeds threshold.
//
// The strategy layer uses this to decide whether to post aggressive offers
// that capture the convergence (Section 10: CEX-DEX Arbitrage).
// ---------------------------------------------------------------------------

struct ArbitrageSignal {
    std::string        pair_name;       // Trading pair, e.g. "XCH/wUSDC"
    double             dex_price;       // DEX mid-price (quote per base)
    double             cex_price;       // CEX mid-price (quote per base)
    double             divergence_bps;  // abs(dex - cex) / cex * 10000
    ArbitrageDirection direction;       // Which venue is cheaper
    Timestamp          detected_at;     // Wall-clock detection time
};

// ---------------------------------------------------------------------------
// PriceHistoryEntry -- one (block_height, price) observation stored in the
// per-pair circular buffer.
// ---------------------------------------------------------------------------

struct PriceHistoryEntry {
    BlockHeight block_height;  // Block at which this price was observed
    double      price;         // Mid-price in quote-per-base (double for math)
};

// ---------------------------------------------------------------------------
// MarketDataConfig -- tuning parameters for the aggregation layer.
// ---------------------------------------------------------------------------

struct MarketDataConfig {
    /// Maximum entries in the per-pair price history circular buffer.
    std::size_t price_history_capacity{kDefaultPriceHistoryCapacity};

    /// Divergence threshold (basis points) above which an ArbitrageSignal
    /// is emitted.  50 bps is the lower end of the expected edge per
    /// Section 10 ("expected edge: 50-200 bps per arbitrage cycle").
    double arb_threshold_bps{50.0};

    /// Staleness threshold.  Data older than this is flagged as stale.
    std::chrono::minutes stale_threshold{5};

    /// Enable competitor detection and tracking from order book data.
    /// When enabled, MarketDataFeed will parse individual offers and
    /// compute best_competing_bps metrics.
    bool enable_competitor_tracking{true};

    /// Minimum offer size (in mojos) to be considered a competitor offer.
    /// Filters out dust offers that aren't from serious market makers.
    /// Default: 1 XCH = 1e12 mojos.
    Mojo min_competitor_offer_size{1'000'000'000'000LL};

    /// Alert threshold: if competing spread < this value (bps), fire an alert.
    /// Indicates a serious competitor with tight spreads has appeared.
    double competitor_alert_threshold_bps{50.0};

    // -- Whale detection configuration --------------------------------------

    /// Minimum trade size (in mojos) to be classified as a whale trade.
    /// Default: 50 XCH.  Trades at or above this size trigger adverse-selection
    /// guards (spread widening, size reduction).
    Mojo whale_trade_threshold{50LL * 1'000'000'000'000LL};

    /// Minimum fraction of rolling 24-hour volume that makes a trade a whale
    /// trade regardless of absolute size.  This catches whales on illiquid pairs
    /// where 50 XCH may still be a large fraction of daily turnover.
    /// Default: 0.05 = 5 % of 24-hour volume.
    double whale_volume_fraction{0.05};

    /// Number of blocks over which whale events are counted for the activity
    /// window.  Default: 10 blocks (~520 s at 52 s/block).
    std::size_t whale_window_blocks{10};

    /// Maximum spread multiplier applied when whale activity is at its most
    /// intense.  The actual multiplier is linearly interpolated between 1.0
    /// (no whale activity) and this value (maximum whale activity in window).
    /// Default: 3.0  (triple the normal spread when the whale window is full).
    double whale_max_spread_multiplier{3.0};

    // -- VPIN configuration -------------------------------------------------

    /// Volume per VPIN bucket, in base-asset units (e.g. XCH).
    /// Trades are accumulated until this threshold is reached, completing a bar.
    /// Reference: Easley, López de Prado & O'Hara (2012).
    /// Default: 10.0 XCH per bucket.
    double vpin_bucket_size{10.0};

    /// Number of completed buckets in the rolling VPIN window.
    /// VPIN is computed as the mean absolute imbalance over the most recent
    /// N completed buckets.  Default: 50 buckets.
    std::size_t vpin_window_buckets{50};

    // -- OFI configuration --------------------------------------------------

    /// Number of order-book snapshots to retain for OFI computation.
    /// Default: 20 observations.
    std::size_t ofi_window_size{20};

    // -- Asymmetric spread configuration ------------------------------------

    /// Asymmetry factor controlling how much the spread is skewed toward the
    /// informed side during whale activity.  0.0 = symmetric, 1.0 = fully
    /// asymmetric (all widening on the informed side).  Default: 0.5.
    double asymmetric_skew_factor{0.5};

    // -- CEX freshness weighting (T7-12) ------------------------------------

    /// [T7-12] Maximum staleness (seconds) of CEX data before its weight
    /// decays to zero.  The effective CEX weight is:
    ///   w_cex = kCexWeight * max(0, 1 - age_sec / cex_freshness_threshold_sec)
    /// Default 120 s.  0 = disable freshness weighting (legacy fixed blend).
    double cex_freshness_threshold_sec{120.0};
};

// ---------------------------------------------------------------------------
// PairState -- internal per-pair aggregation state.
//
// Not exposed directly; callers read MarketSnapshot via the State object or
// use the typed accessor methods on MarketDataFeed.
// ---------------------------------------------------------------------------

struct PairState {
    std::string pair_name;        // e.g. "XCH/wUSDC"

    // --- Dexie data ---
    double      dex_best_bid{0.0};  // Best bid from dexie order book
    double      dex_best_ask{0.0};  // Best ask from dexie order book
    double      dex_last_trade{0.0};// Most recent trade price on dexie
    double      volume_24h{0.0};    // Rolling 24-hour volume (base asset units)
    Timestamp   dex_updated_at{};   // When dexie data was last refreshed

    // --- CEX reference ---
    double      cex_mid{0.0};       // CEX mid-price (0 if unavailable)
    Timestamp   cex_updated_at{};   // When CEX data was last refreshed

    // --- Block height context ---
    BlockHeight last_block{0};      // Most recent block height observed

    // --- Computed fields ---
    double      mid_price{0.0};     // Aggregated mid (dex + optional cex blend)
    double      spread_bps{0.0};    // Current spread in basis points
    bool        is_stale{true};     // True if data is older than stale_threshold

    explicit PairState(const std::string& name = "") : pair_name(name) {}
};

// ---------------------------------------------------------------------------
// ArbitrageCallback -- signature for the callback invoked when an arbitrage
// signal is detected.  The strategy layer registers its handler at startup.
// ---------------------------------------------------------------------------

using ArbitrageCallback = std::function<void(const ArbitrageSignal&)>;

// ---------------------------------------------------------------------------
// MarketDataFeed -- the primary market data aggregation class.
//
// Lifecycle:
//   1. Construct with config, a reference to the shared State, and an
//      optional ArbitrageCallback.
//   2. Call refresh(enabled_pairs) once per block from the engine heartbeat.
//      This is a non-blocking operation that updates internal state.
//   3. Read aggregated data via get_mid_price, get_spread_bps, etc.
//
// Thread safety:
//   Safe for concurrent reads from multiple strategy threads while a single
//   writer thread calls refresh().  Most methods acquire at most one mutex.
//   The move assignment operator acquires two mutexes per map (this->mtx_
//   and other.mtx_) via std::scoped_lock, which uses the C++17 deadlock-
//   avoidance algorithm to prevent ABBA deadlock (see T3-23).
// ---------------------------------------------------------------------------

class MarketDataFeed {
public:
    // -- Construction -------------------------------------------------------

    /// Construct with configuration, a shared State reference (for writing
    /// MarketSnapshot objects), and an optional arbitrage signal callback.
    ///
    /// @param cfg       Aggregation configuration (thresholds, capacities).
    /// @param state     Shared mutable state — refresh() writes snapshots here.
    /// @param arb_cb    Optional callback invoked when an arb signal fires.
    explicit MarketDataFeed(const MarketDataConfig& cfg,
                            State&                  state,
                            ArbitrageCallback       arb_cb = nullptr);

    // Non-copyable, movable.
    MarketDataFeed(const MarketDataFeed&)            = delete;
    MarketDataFeed& operator=(const MarketDataFeed&) = delete;
    MarketDataFeed(MarketDataFeed&&)                 noexcept;
    MarketDataFeed& operator=(MarketDataFeed&&)      noexcept;

    ~MarketDataFeed();

    // -- Primary interface --------------------------------------------------

    /// Refresh market data for every pair in @p enabled_pairs.
    ///
    /// For each pair, this method:
    ///   1. Ingests dexie order book data (best bid/ask, last trade, volume).
    ///   2. Ingests the current block height from the full node.
    ///   3. Ingests any available CEX reference prices.
    ///   4. Computes the aggregated mid-price using the blending rules.
    ///   5. Detects staleness.
    ///   6. Detects arbitrage signals and invokes the callback if armed.
    ///   7. Appends the observation to the price history circular buffer.
    ///   8. Writes the resulting MarketSnapshot into the shared State.
    ///
    /// This method is intended to be called once per block (~52 s) from the
    /// engine heartbeat.  It does NOT perform HTTP I/O itself; the caller
    /// must supply pre-fetched data via the ingest_* methods, or override
    /// the protected fetch hooks for testing.
    ///
    /// @param enabled_pairs  List of pair names to refresh (e.g. "XCH/wUSDC").
    void refresh(const std::vector<std::string>& enabled_pairs);

    // -- Data ingestion (called before refresh, or by async fetch layer) ----

    /// Ingest dexie order book data for a pair.
    ///
    /// @param pair_name  Trading pair identifier.
    /// @param best_bid   Best bid price (quote per base), or 0 if no bids.
    /// @param best_ask   Best ask price (quote per base), or 0 if no asks.
    /// @param last_trade Last trade price (quote per base), or 0 if none.
    /// @param vol_24h    Rolling 24-hour volume in base asset units.
    void ingest_dexie(const std::string& pair_name,
                      double             best_bid,
                      double             best_ask,
                      double             last_trade,
                      double             vol_24h);

    /// Ingest individual offers from the dexie order book for competitor tracking.
    /// This is called in addition to ingest_dexie() when competitor tracking
    /// is enabled, and processes the full order book (not just best bid/ask).
    ///
    /// @param pair_name     Trading pair identifier.
    /// @param competing_offers Vector of competing offers parsed from API response.
    /// @param own_offer_ids Set of our own offer IDs to exclude from competitor analysis.
    void ingest_competing_offers(
        const std::string&                 pair_name,
        const std::vector<CompetingOffer>& competing_offers,
        const std::unordered_set<std::string>& own_offer_ids);

    /// Ingest the current block height from the Chia full node.
    ///
    /// @param block_height  Peak block height from get_blockchain_state().
    void ingest_block_height(BlockHeight block_height);

    /// Ingest a CEX reference mid-price for a pair.
    ///
    /// @param pair_name  Trading pair identifier (must match dexie pair name).
    /// @param cex_mid    CEX mid-price (quote per base).
    void ingest_cex_reference(const std::string& pair_name,
                              double             cex_mid);

    // -- Typed accessors (thread-safe reads) --------------------------------

    /// Best available mid-price for a pair.
    /// Returns 0.0 if the pair is unknown or has no data.
    double get_mid_price(const std::string& pair_name) const;

    /// Current spread in basis points for a pair.
    /// Returns 0.0 if the pair is unknown or has no quotes.
    double get_spread_bps(const std::string& pair_name) const;

    /// Rolling 24-hour volume in base asset units.
    /// Returns 0.0 if the pair is unknown.
    double get_volume_24h(const std::string& pair_name) const;

    /// CEX reference mid-price for a pair, if available.
    /// Returns std::nullopt if no CEX data has been ingested for this pair,
    /// or if the CEX data is stale.
    std::optional<double> get_cex_reference(const std::string& pair_name) const;

    /// Whether the data for a pair is considered stale.
    /// Returns true if the pair is unknown.
    bool is_stale(const std::string& pair_name) const;

    /// Staleness fraction for graduated spread widening (T3-06).
    /// Returns 0.0 when data is fresh, 1.0 at stale_threshold, >1.0 beyond.
    /// Returns 1.0 if the pair is unknown.
    double get_staleness_fraction(const std::string& pair_name) const;

    /// Retrieve the latest block height ingested from the full node.
    BlockHeight current_block_height() const;

    // -- Price history access (concurrent reads via shared_mutex) -----------

    /// Read the price history for a pair.
    /// Returns an empty vector if the pair has no history.
    /// The returned entries are ordered oldest-to-newest.
    std::vector<PriceHistoryEntry> get_price_history(
        const std::string& pair_name) const;

    /// Number of entries currently stored for a pair's price history.
    /// Returns 0 if the pair is unknown.
    std::size_t price_history_size(const std::string& pair_name) const;

    // -- Competitor metrics access ------------------------------------------

    /// Retrieve the latest competitor metrics for a pair.
    /// Returns std::nullopt if competitor tracking is disabled or if no
    /// competitors have been detected for this pair.
    std::optional<CompetitorMetrics> get_competitor_metrics(
        const std::string& pair_name) const;

    /// Best competing spread (tightest non-own spread) in basis points.
    /// This is the value fed into SpreadOptimizer::compute_spread() as
    /// best_competing_bps.  Returns 0.0 if no competitors detected.
    double get_best_competing_spread_bps(const std::string& pair_name) const;

    /// Total number of competing offers (both sides) for a pair.
    /// Returns 0 if competitor tracking is disabled or no competitors exist.
    std::size_t get_num_competing_offers(const std::string& pair_name) const;

    // -- Arbitrage signal access --------------------------------------------

    /// Retrieve the most recent arbitrage signal for a pair, if any.
    /// Returns std::nullopt if no signal has been emitted or if the pair
    /// is unknown.
    std::optional<ArbitrageSignal> get_latest_arb_signal(
        const std::string& pair_name) const;

    // -- Whale detection ----------------------------------------------------

    /// Record an individual trade and update whale-activity metrics.
    ///
    /// Called by the engine each time a fill is confirmed on the DEX (or when
    /// the order book snapshots reveal a large trade vs. the previous block).
    /// A trade is classified as a "whale trade" when its size meets either
    /// the absolute threshold (whale_trade_threshold) or the fractional-volume
    /// threshold (whale_volume_fraction x vol_24h).
    ///
    /// T3-35: When @p is_own_fill is true, the fill originated from the bot's
    /// own offers.  Own fills are recorded for attribution/calibration only
    /// and are excluded from whale detection to prevent self-reinforcing
    /// toxicity signals that would cause a spread-widening spiral.
    ///
    /// @param pair_name     Trading pair identifier.
    /// @param side          Direction of the trade (Bid = taker bought, Ask = taker sold).
    /// @param size          Trade size in mojos (base asset).
    /// @param block_height  Block at which the trade occurred.
    /// @param is_own_fill   True if this fill was generated by the bot's own
    ///                      offers.  Default false (backward compatible).
    void ingest_trade(const std::string& pair_name,
                      Side               side,
                      Mojo               size,
                      BlockHeight        block_height,
                      bool               is_own_fill = false);

    /// Retrieve the latest whale metrics for a pair.
    /// Returns std::nullopt if no trades have been ingested for this pair or if
    /// no whale events have occurred within the tracking window.
    std::optional<WhaleMetrics> get_whale_metrics(
        const std::string& pair_name) const;

    /// Whether whale activity is currently detected for a pair.
    /// Returns false if the pair is unknown or the tracking window is empty.
    bool is_whale_active(const std::string& pair_name) const;

    /// Recommended spread multiplier based on current whale activity.
    /// Returns 1.0 when no whale activity is detected (no spread widening).
    /// Returns up to whale_max_spread_multiplier when the whale window is full.
    double get_whale_spread_multiplier(const std::string& pair_name) const;

    // -- VPIN (flow toxicity) -----------------------------------------------

    /// Ingest a classified trade for VPIN computation.
    ///
    /// Unlike ingest_trade() (which only records whale-sized trades), this
    /// method feeds every observed trade into the VPIN volume-bar pipeline.
    /// Call this for ALL trades, not just whales.
    ///
    /// T3-35: When @p is_own_fill is true, the fill originated from the bot's
    /// own offers.  Own fills are excluded from VPIN volume-bar accumulation
    /// to prevent self-generated order flow from inflating the toxicity metric.
    /// The fill is logged for attribution but does not alter the VPIN signal.
    ///
    /// @param pair_name     Trading pair identifier.
    /// @param side          Direction of the trade (Bid = taker bought).
    /// @param volume        Trade volume in base-asset units (e.g. XCH).
    /// @param is_own_fill   True if this fill was generated by the bot's own
    ///                      offers.  Default false (backward compatible).
    void ingest_trade_for_vpin(const std::string& pair_name,
                               Side               side,
                               double             volume,
                               bool               is_own_fill = false);

    /// Retrieve the current VPIN (flow-toxicity) metrics for a pair.
    /// Returns std::nullopt if insufficient buckets have been completed.
    std::optional<VpinMetrics> get_vpin_metrics(
        const std::string& pair_name) const;

    /// Current VPIN value for a pair, in [0, 1].
    /// Returns 0.0 if no VPIN data is available (safe default: no toxicity).
    double get_vpin(const std::string& pair_name) const;

    // -- OFI (order flow imbalance) -----------------------------------------

    /// Ingest an order-book snapshot for OFI computation.
    ///
    /// The OFI delta is computed from changes between consecutive snapshots
    /// at the best bid/ask levels.  Call this once per block after
    /// ingest_dexie().
    ///
    /// @param pair_name  Trading pair identifier.
    /// @param best_bid   Best bid price.
    /// @param bid_size   Total size at best bid.
    /// @param best_ask   Best ask price.
    /// @param ask_size   Total size at best ask.
    void ingest_book_snapshot_for_ofi(const std::string& pair_name,
                                      double             best_bid,
                                      double             bid_size,
                                      double             best_ask,
                                      double             ask_size);

    /// Multi-level OFI ingestion (T5-CR2, Xu, Lehalle & Alfonsi 2023).
    ///
    /// Accepts the full visible book depth.  Each level's OFI contribution
    /// is weighted by inverse rank: w_k = 1/(k+1), normalised so weights
    /// sum to 1.  Multi-level OFI explains 10-30% more return variance than
    /// best-level alone on CHIA's typically shallow (2-5 level) book.
    ///
    /// @param pair_name  Trading pair identifier.
    /// @param bids       Bid levels sorted best (highest) first.
    /// @param asks       Ask levels sorted best (lowest) first.
    void ingest_book_snapshot_for_ofi(
        const std::string&              pair_name,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks);

    /// Retrieve the current OFI metrics for a pair.
    /// Returns std::nullopt if fewer than 2 snapshots have been ingested.
    std::optional<OfiMetrics> get_ofi_metrics(
        const std::string& pair_name) const;

    /// Normalised OFI value for a pair, in [-1, 1].
    /// Positive = buy pressure, negative = sell pressure.
    /// Returns 0.0 if no OFI data is available.
    double get_normalized_ofi(const std::string& pair_name) const;

    // -- Asymmetric spread widening -----------------------------------------

    /// Compute per-side spread multipliers that skew widening toward the
    /// informed (toxic) side.
    ///
    /// When a whale buys aggressively (dominant_side = Bid), the ask side
    /// carries higher adverse-selection risk.  This method raises the ask
    /// multiplier and lowers the bid multiplier, preserving the total
    /// widening but distributing it asymmetrically.
    ///
    /// Returns {1.0, 1.0} when no whale activity is detected.
    AsymmetricMultipliers get_asymmetric_spread_multipliers(
        const std::string& pair_name) const;

    // -- Configuration access -----------------------------------------------

    /// Read-only access to the active configuration.
    /// [MEDIUM-1] Returns by value under shared_lock to prevent data races
    /// with concurrent set_*() mutations (ISO/IEC 5055 -- CWE-362).
    MarketDataConfig config() const;

    /// Update the arbitrage threshold at runtime (e.g. from ML tuner).
    void set_arb_threshold_bps(double threshold_bps);

    /// Replace the arbitrage callback (e.g. when strategy layer reconnects).
    void set_arb_callback(ArbitrageCallback cb);

    // -- Whale configuration setters (runtime-tunable) ----------------------

    /// Update the absolute whale-trade size threshold.
    /// @param threshold  Minimum trade size in mojos; must be > 0.
    void set_whale_trade_threshold(Mojo threshold);

    /// Update the volume-fraction whale threshold.
    /// @param fraction  Minimum fraction of 24h volume (0 < fraction <= 1).
    void set_whale_volume_fraction(double fraction);

    /// Update the rolling window length.
    /// @param blocks  Window size in blocks; must be >= 1.
    void set_whale_window_blocks(std::size_t blocks);

    /// Update the maximum spread multiplier.
    /// @param multiplier  Must be >= 1.0.
    void set_whale_max_spread_multiplier(double multiplier);

private:
    // -- Internal helpers ---------------------------------------------------

    /// Compute the aggregated mid-price for a pair using the blending rules.
    ///   1. If dexie has valid bid/ask: dex_mid = (bid + ask) / 2
    ///   2. If dexie has no quotes but has a last trade: dex_mid = last_trade
    ///   3. If CEX reference is available: mid = kDexWeight*dex + kCexWeight*cex
    ///   4. Otherwise: mid = dex_mid
    /// Returns 0.0 if no price data is available at all.
    static double compute_mid(const PairState& ps);

    /// Compute the spread in basis points from best_bid and best_ask.
    /// Returns 0.0 if either side is zero (no two-sided market).
    static double compute_spread_bps(double best_bid, double best_ask);

    /// Detect staleness by comparing the data timestamp to now().
    /// Returns true if (now - ts) > stale_threshold, or if ts is epoch-zero
    /// (never updated).
    bool detect_stale(Timestamp ts) const;

    /// Check for arbitrage divergence and fire the callback if threshold
    /// is exceeded.  Updates the per-pair latest_arb_signal.
    void check_arbitrage(PairState& ps);

    /// Append a price observation to the per-pair circular buffer, evicting
    /// the oldest entry if capacity is reached.
    void append_price_history(const std::string& pair_name,
                              BlockHeight         block,
                              double              price);

    /// Compute CompetitorMetrics from the tracked competing offers for a pair.
    /// Computes best spreads, depth counts, and detects new competitors.
    /// Returns std::nullopt if competitor tracking is disabled.
    std::optional<CompetitorMetrics> compute_competitor_metrics(
        const std::string& pair_name);

    /// Classify a trade as a whale event and, if so, append it to the per-pair
    /// event deque and recompute WhaleMetrics.  Called from ingest_trade().
    void detect_and_update_whale(const std::string& pair_name,
                                 Side               side,
                                 Mojo               size,
                                 BlockHeight        block_height);

    /// Compute the spread multiplier from the count of whale events in the
    /// rolling window.  Linear interpolation: 0 events → 1.0; window_blocks
    /// events → max_multiplier.
    /// @param events_in_window  Number of whale events in the rolling window.
    /// @param window_blocks     Config snapshot of whale_window_blocks.
    /// @param max_multiplier    Config snapshot of whale_max_spread_multiplier.
    static double compute_whale_spread_multiplier(
        std::size_t events_in_window,
        std::size_t window_blocks,
        double      max_multiplier);

    /// Compute VPIN from the completed volume bars for a pair.
    /// VPIN = (1/N) * SUM(|buy_vol_i - sell_vol_i|) / bucket_size
    void recompute_vpin(const std::string& pair_name);

    /// Compute OFI delta from the latest two book snapshots and update metrics.
    void recompute_ofi(const std::string& pair_name);

    /// Recompute whale metrics for all tracked pairs after a config change.
    /// Trims stale events from each pair's event deque and recalculates the
    /// spread multiplier with the updated configuration.
    void recompute_all_whale_metrics();

    /// Build a MarketSnapshot from internal PairState and write it to the
    /// shared State object.
    void publish_snapshot(const PairState& ps);

    /// Look up or create a PairState.  Caller must hold mtx_pairs_ exclusively.
    PairState& get_or_create_pair(const std::string& pair_name);

    // -- Data members -------------------------------------------------------

    // Lock ordering (always acquire in this order to prevent deadlock):
    //   mtx_pairs_ -> mtx_arb_ -> mtx_history_
    //   mtx_config_ (independent, never held with others)
    //   mtx_vpin_ (independent)
    // ISO/IEC 5055 -- CWE-833 (deadlock) prevention by documented ordering.

    /// Configuration (thresholds, capacities).  Guarded by mtx_config_ for
    /// thread-safe runtime updates via the set_* methods.
    mutable std::shared_mutex mtx_config_;
    MarketDataConfig config_;

    /// Reference to the shared global state.  refresh() writes MarketSnapshot
    /// objects here so that strategy and risk layers can read them.
    State& state_;

    /// Callback invoked when an arbitrage signal fires.  May be null.
    ArbitrageCallback arb_callback_;

    /// Per-pair aggregation state.  Guarded by mtx_pairs_.
    mutable std::shared_mutex                          mtx_pairs_;
    std::unordered_map<std::string, PairState>         pairs_;

    /// Per-pair price history circular buffers.  Guarded by mtx_history_.
    mutable std::shared_mutex                          mtx_history_;
    std::unordered_map<std::string, std::deque<PriceHistoryEntry>> history_;

    /// Per-pair latest arbitrage signal.  Guarded by mtx_arb_.
    mutable std::shared_mutex                          mtx_arb_;
    std::unordered_map<std::string, ArbitrageSignal>   latest_arb_;

    /// Per-pair competing offers tracked from order book.  Guarded by mtx_competitors_.
    mutable std::shared_mutex                          mtx_competitors_;
    std::unordered_map<std::string, std::vector<CompetingOffer>> competing_offers_;

    /// Per-pair latest competitor metrics.  Guarded by mtx_competitor_metrics_.
    mutable std::shared_mutex                          mtx_competitor_metrics_;
    std::unordered_map<std::string, CompetitorMetrics> competitor_metrics_;

    /// Per-pair whale trade event deques (ordered oldest-to-newest by block).
    /// Guarded by mtx_whale_events_.
    mutable std::shared_mutex                              mtx_whale_events_;
    std::unordered_map<std::string, std::deque<WhaleTradeEvent>> whale_events_;

    /// Per-pair latest whale metrics.  Guarded by mtx_whale_metrics_.
    mutable std::shared_mutex                          mtx_whale_metrics_;
    std::unordered_map<std::string, WhaleMetrics>      whale_metrics_;

    /// Per-pair VPIN volume-bar state.  Guarded by mtx_vpin_.
    /// Each pair tracks a current (incomplete) bucket and a deque of completed
    /// buckets.  When the current bucket fills, it is pushed to the deque.
    struct VpinState {
        VpinBucket                current_bucket;   // in-progress bar
        std::deque<VpinBucket>    completed;        // completed bars (newest last)
    };
    mutable std::shared_mutex                          mtx_vpin_;
    std::unordered_map<std::string, VpinState>         vpin_state_;

    /// Per-pair latest VPIN metrics.  Guarded by mtx_vpin_metrics_.
    mutable std::shared_mutex                          mtx_vpin_metrics_;
    std::unordered_map<std::string, VpinMetrics>       vpin_metrics_;

    /// Per-pair OFI book-snapshot history.  Guarded by mtx_ofi_.
    struct BookLevel {
        double price{0.0};
        double size{0.0};
    };
    struct BookSnapshot {
        double best_bid{0.0};
        double bid_size{0.0};
        double best_ask{0.0};
        double ask_size{0.0};

        /// Multi-level extension (T5-CR2, Xu, Lehalle & Alfonsi 2023):
        /// When populated, recompute_ofi() uses all levels weighted by
        /// inverse rank distance from mid (w_k = 1/(k+1), normalised).
        std::vector<BookLevel> bid_levels;
        std::vector<BookLevel> ask_levels;
    };
    mutable std::shared_mutex                          mtx_ofi_;
    std::unordered_map<std::string, std::deque<BookSnapshot>> ofi_snapshots_;

    /// Per-pair latest OFI metrics.  Guarded by mtx_ofi_metrics_.
    mutable std::shared_mutex                          mtx_ofi_metrics_;
    std::unordered_map<std::string, OfiMetrics>        ofi_metrics_;

    /// Latest block height from the full node.  Atomic for lock-free reads.
    std::atomic<BlockHeight> block_height_{0};
};

}  // namespace xop

#endif  // XOP_EXECUTION_MARKET_DATA_HPP
