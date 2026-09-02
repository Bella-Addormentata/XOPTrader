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
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
// FairValue -- an INDEPENDENT reference price for a pair.
//
// "Independent" is the whole point: this value must contain no input from the
// dexie order book for the pair it prices, because its job is to validate that
// very book.  On 2026-08-01 all six XCH/BYC ask tiers were swept in one block,
// because every tier was centred on a dexie mid that nothing had ever checked.
// A number derived from that same mid could not have caught it.
//
// The value is produced by xop::fv::solve_pair (see fair_value_solver.hpp): a
// weighted least-squares solve over the graph whose nodes are assets and whose
// edges are pairs, re-run per pair with that pair's own book edge deleted.
//
// Confidence tiers, assigned from the SOLVE, never from a pair's identity:
//   CexDirect    -- both legs carry a direct external USD anchor and the
//                   solved uncertainty is inside the tight threshold.  No
//                   on-chain book is needed to price this pair at all.
//   Triangulated -- the answer survives deleting any single observation (the
//                   graph is genuinely over-determined here) and the solved
//                   uncertainty is inside the tight threshold.
//   Inferred     -- solved and usable, but along a path with no cross-check,
//                   or with an uncertainty above the tight threshold.
//   Unavailable  -- no path to an anchor, or the solved uncertainty exceeds
//                   the usable threshold.  The caller must WIDEN, never guess.
//
// An earlier revision added a tier that valued a pegged leg at its declared
// peg.  It was removed: a peg is an assumption, not an observation, and BYC has
// never traded at par.  Everything here is now measured.
// ---------------------------------------------------------------------------

enum class FairValueTier {
    Unavailable  = 0,  // No independent source -- do NOT quote against this.
    CexDirect    = 1,  // Both legs anchored by the external price feed.
    Triangulated = 2,  // Over-determined graph solve, cross-checked.
    Inferred     = 3,  // Solved, usable, but not cross-checked.
};

inline const char* to_string(FairValueTier t) noexcept {
    switch (t) {
        case FairValueTier::CexDirect:    return "cex-direct";
        case FairValueTier::Triangulated: return "triangulated";
        case FairValueTier::Inferred:     return "inferred";
        default:                          return "unavailable";
    }
}

struct FairValue {
    double        price{0.0};       // Quote-per-base, independent of the book.
    FairValueTier tier{FairValueTier::Unavailable};
    double        age_seconds{0.0}; // Age of the sample the solve was fed.

    /// 1-sigma uncertainty of log(price) in basis points, from the solve's own
    /// normal matrix.  This is what decides whether the value may be used at
    /// all, and it widens the deviation band so a shakier estimate clamps less
    /// aggressively rather than being trusted like a firm one.
    double        sigma_bps{0.0};

    /// CONSISTENCY RESIDUAL: 10 000 * log(book_mid / fair_value).  How far this
    /// pair's own book sits from what every OTHER observation implies.  Signed:
    /// positive means the book is quoting the base asset richer than the rest
    /// of the graph agrees.  NaN when the pair has no two-sided book to compare
    /// against.  This is the disagreement signal -- it is published even when
    /// the tier is Unavailable, because "these books contradict each other" is
    /// informative regardless of which one is wrong.
    double        residual_bps{0.0};

    /// Observations (anchors + edges) that fed the solve.
    std::size_t   observations{0};
};

// ---------------------------------------------------------------------------
// FairValueObservation -- the raw, book-derived inputs the fair-value solve
// needs from one pair, fetched under a single lock.
//
// Deliberately reports the SELF-FILTERED dexie top of book rather than the
// aggregated mid: the aggregated mid may already blend a CEX reference, and
// feeding that back into a solve anchored on the same CEX feed would double
// count it.  The solve wants the raw market observation and nothing else.
// ---------------------------------------------------------------------------
struct FairValueObservation {
    bool         has_book{false};   // Two-sided third-party book exists.
    double       mid{0.0};          // (best_bid + best_ask) / 2.
    double       spread_bps{0.0};   // Width of that book.
    std::int32_t print_age{0};      // Heartbeats since the mid last moved.
    double       amm_mid{0.0};      // AMM implied mid (0 if none).

    /// Seconds since the AMM sample was actually OBSERVED -- i.e. since the
    /// last SUCCESSFUL pool fetch, not since it was last copied out of the
    /// cache.  Re-stamping a cached sample every heartbeat pinned this at ~0
    /// and made every AMM freshness gate unreachable.
    double       amm_age_seconds{0.0};

    /// Total USD value of BOTH sides of the pool the AMM mid came from, 0 when
    /// unknown.  This is what the AMM edge's weight is derived from: the
    /// "arbitrage holds the pool to fair value" argument is an argument about
    /// how much money defends the price, so the money has to be measured.
    double       amm_pool_usd{0.0};
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

    /// Weight of the AMM implied price in the three-source blend.
    /// When AMM data is available, the blend becomes:
    ///   mid = w_dex * dex_mid + w_cex * cex_mid + w_amm * amm_mid
    /// with weights re-normalised to sum to 1.0.
    ///
    /// DEFAULT 0.0 -- the AMM is an independent VALIDATOR of the mid, not a
    /// contributor to it.  The same TibetSwap sample also feeds the
    /// fair-value solve that checks the ladder; if it fed both, the guard
    /// would be comparing the ladder against a number that had itself set the
    /// ladder's centre.  A validator must not be able to move the thing it
    /// validates, so the blend side is switched off and the solve side kept.
    /// See StrategyConfig::amm_blend_weight for the full rationale.
    double amm_blend_weight{0.0};

    /// Maximum staleness (seconds) of AMM data before it is ignored.
    /// Default 300 s (5 min).  0 = disable freshness check.
    double amm_freshness_threshold_sec{300.0};

    /// Max age (seconds) of an unchanged dexie last-trade print before it is
    /// refused as a mid reference.  <= 0 disables the gate.
    ///
    /// The last trade is the ONLY leg of the published-mid blend that is a
    /// historical print rather than a live quote, and it enters at the full
    /// DEX weight.  Without this, a print of any age -- one case measured at
    /// 13 days -- carried 70% of the mid whenever the third-party book was
    /// empty, which is exactly the state a thin or bid-only pair sits in.
    double dex_last_trade_max_age_sec{1800.0};

    // -- Order-book-derived mid-price (Stoikov micro-price) ----------------
    //
    // [2026-09-02] NOT a VWAP micro-price, whatever the surrounding prose
    // used to say.  The estimator weights each side's TOUCH PRICE by the
    // OPPOSITE side's top-N cumulative depth, which is a convex combination
    // of best_bid and best_ask and is therefore interior to the book by
    // construction.  The VWAP form these comments described was the defect
    // (1,849 invariant-clamp firings over ~39h of live XCH/DBX); see
    // xop/execution/orderbook_mid.hpp.  Do not re-derive it from here.

    /// When true, compute_mid() prefers an order-book-derived mid-price
    /// (Stoikov micro-price: touch prices weighted by opposite-side top-N
    /// depth) over the simple Dexie BBO midpoint.  Computed from the top
    /// `orderbook_mid_depth` levels per side of the dust-filtered competing
    /// offers.
    /// Default: true.
    bool orderbook_mid_enabled{true};

    /// Number of order book levels per side over which each side's
    /// CUMULATIVE DEPTH is summed for the micro-price weighting.  (The
    /// per-side VWAP is still computed from these levels, but only for the
    /// clamp diagnostic -- it is not the price the estimator weights.)
    /// Higher values give a more robust depth measure at the cost of
    /// including offers further from fair value.
    /// Default: 5 levels per side.
    std::size_t orderbook_mid_depth{5};

    /// Layer 2 blend schedule for the order-book mid.  At or below
    /// `microprice_narrow_bps` of relative spread the Stoikov micro-price is
    /// used whole; at or above `microprice_wide_bps` it is discarded for the
    /// plain BBO midpoint; in between the two are blended linearly.
    /// Mirrored from StrategyConfig, where the defaults are justified against
    /// measured per-pair spread distributions.
    double microprice_narrow_bps{200.0};
    double microprice_wide_bps{800.0};

    /// Maximum age (seconds) of an independent fair value before
    /// get_fair_value() reports it as UNAVAILABLE.  The external price feed
    /// polls every 30 s, so 300 s tolerates nine consecutive misses before
    /// the engine is told it is quoting blind.  Must never fall back to the
    /// dexie mid -- an expired fair value is reported as absent, not stale.
    double fair_value_max_age_sec{300.0};

    // -- Published-mid BBO band (Layer 1 for the PUBLISHED mid) --------------
    //
    // compute_orderbook_mid() enforces best_bid <= mid <= best_ask on the
    // order-book mid, but compute_mid() then blends that number with CEX and
    // AMM references, so the PUBLISHED mid could leave the book again -- the
    // exact mechanism by which a broken external reference (the BYC $1.1447
    // artifact, 13% over its $1.01 truth) could drag a healthy pair's mid out
    // of its own executable interval.  The published mid is therefore clamped
    // to the dust-filtered third-party BBO widened by a tolerance band:
    //
    //     band_bps = max(floor_bps, spread_frac * book_spread_bps)
    //     mid in [min(bid,ask) * (1 - band), max(bid,ask) * (1 + band)]
    //
    // applied only while the dex book is two-sided and fresh (a stale book is
    // not "now"; CEX should govern then).  Mirrored from StrategyConfig,
    // where the defaults are justified against measured numbers.

    /// Minimum band (bps) allowed beyond the BBO regardless of spread.
    double published_mid_band_floor_bps{150.0};

    /// Band as a fraction of the book's own relative spread.
    double published_mid_band_spread_frac{0.25};

    // -- [S20 2026-08-24] Published-mid plausibility gate --------------------
    //
    // The BBO band above bounds the mid only while a two-sided fresh book
    // exists; the S20 incidents all travelled the OTHER paths (frozen
    // orderbook_mid after failed offer fetches, last-trade-only mids on an
    // emptied book).  The gate compares every candidate mid against an
    // independent anchor (see mid_gate.hpp) and refuses to publish on a
    // breach: the pair goes no-mid, which every consumer already treats as
    // "do not quote, do not value" -- the repo's established failure mode.

    /// Master switch for the anchor gate AND the anchored offer filter.
    bool mid_gate_enabled{true};

    /// Multiplicative anchor band; candidate/anchor confined to
    /// [1/ratio, ratio].  Wide by design: refuse 187x absurdity, pass real
    /// repricing.  <= 1.0 disables the anchor test.
    double mid_anchor_band_ratio{3.0};

    /// Book-confirmation escape: a fresh two-sided dust-filtered book with
    /// spread in (0, this] bps overrides a band breach ("the whole market
    /// repriced" evidence).  Chosen above BYC/wUSDC.b's measured p50 spread
    /// of 1163 bps with headroom for stress, below absurdity.
    double mid_gate_book_confirm_max_spread_bps{5000.0};

    /// Anchorless fallback: max fractional move vs the last ACCEPTED mid in
    /// one heartbeat.  Chia blocks at ~52 s; nothing honest moves 50% in
    /// one, but a book-confirmed move still passes.  <= 0 disables.
    double mid_gate_max_step_frac{0.5};

    /// [SIDEQUALITY 2026-09-01] Multiplicative band for the PER-SIDE
    /// anchor-agreement test (MarketSnapshot::bid_side_anchor_ok /
    /// ask_side_anchor_ok).  A side whose best dust-filtered price sits
    /// outside [1/ratio, ratio] of the independent anchor is not evidence
    /// about location, and consumers may re-reference away from it.
    ///
    /// Defaulted to the SAME 3.0 as mid_anchor_band_ratio, deliberately.
    /// The published-mid gate already treats 3x from the anchor as the
    /// boundary of the plausible; a side that is individually past that
    /// boundary fails the same test the whole mid would fail, so one
    /// number governs both and they cannot be configured into
    /// disagreement.  It sits WELL INSIDE the per-offer absurdity bound
    /// (offer_absurdity_ratio -> 10.0 at these settings), which is
    /// intentional: the absurdity filter removes offers no honest market
    /// could produce, while this flags a side no honest market would
    /// price.  A side can therefore be disqualified as a REFERENCE while
    /// its offers remain in the book, which is exactly the state XCH/BYC
    /// is in.
    ///
    /// <= 1.0 disables the test (both flags stay true).
    double book_side_anchor_band_ratio{3.0};

    /// Two-sides-agree bypass: when the dust-filtered book is two-sided
    /// and its OWN spread is at most this many bps, neither side is
    /// disqualified however far both sit from the anchor.
    ///
    /// This is the escape that keeps a genuine repricing alive.  If the
    /// whole market moved, both sides move together and the book stays
    /// internally coherent -- exactly the evidence
    /// mid_gate::book_confirms() accepts to override an anchor breach.
    /// Disqualifying sides in that state would strip the confirmation
    /// before the gate could read it, so the two mechanisms are pinned to
    /// the same default (mid_gate_book_confirm_max_spread_bps, 5000).
    /// Dislocation is one side moving ALONE, which leaves a wide spread
    /// and fails this test.
    double book_side_agree_max_spread_bps{5000.0};
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

    // --- Print-age staleness (value-change counter) ---
    // dex_updated_at is rewritten on EVERY heartbeat whether or not the price
    // moved, so it measures when we last LOOKED, not when the price last MOVED.
    // Measured: the BYC/wUSDC.b dexie mid sat at exactly 1.1030 for 26+
    // consecutive snapshots (longest freeze 30.4h, 92.6% of observations
    // unchanged) while reporting an age of 0 seconds.  These two fields track
    // the price itself so a frozen book is detectable.
    double       last_dex_print{0.0}; // Last materially-different dex mid
    std::int32_t dex_print_age{0};    // Heartbeats since it last moved

    // The same treatment for the last-trade print itself.  dex_print_age is
    // derived from the ORDER-BOOK mid, so it stops advancing precisely when
    // no usable two-sided quote remains -- an empty book, or a one-sided one,
    // which is where the last trade becomes the mid.  These two fields age
    // the print directly.  A default-constructed
    // last_trade_changed_at means "never observed to move": the print's age
    // is unknown, not zero, and compute_mid refuses it on that basis.
    double      last_trade_print{0.0};
    Timestamp   last_trade_changed_at{};

    // --- AMM reference (TibetSwap implied price) ---
    double      amm_mid{0.0};       // AMM implied mid-price (0 if unavailable)
    Timestamp   amm_updated_at{};   // When the pool was last successfully READ
                                    // (supplied by the caller, NOT the ingest
                                    // time -- see ingest_amm_mid).
    double      amm_pool_usd{0.0};  // USD value of both pool sides, 0=unknown

    // --- Order-book-derived mid (Stoikov micro-price) ---
    // Touch prices weighted by opposite-side top-N depth, blended toward the
    // plain BBO midpoint as the spread widens.  NOT a VWAP micro-price --
    // that form was unbounded and was corrected 2026-09-02; see
    // xop/execution/orderbook_mid.hpp before changing anything here.
    double      orderbook_mid{0.0}; // Stoikov micro-price from competing offers

    // --- CEX reference ---
    double      cex_mid{0.0};       // CEX mid-price (0 if unavailable)
    Timestamp   cex_updated_at{};   // When CEX data was last refreshed

    // --- Independent fair value (never derived from this pair's book) ---
    double        fair_value{0.0};  // Quote-per-base (0 if unavailable)

    /// The solve's raw estimate, kept even when the tier is Unavailable
    /// because the sigma exceeded the clamp ceiling.  0 only when the solve
    /// produced no anchored answer at all.  Served by
    /// get_fair_value_estimate(); the clamp path never reads it.
    double        fair_value_estimate{0.0};
    FairValueTier fair_value_tier{FairValueTier::Unavailable};
    Timestamp     fair_value_updated_at{};
    double        fair_value_sigma_bps{0.0};
    double        fair_value_residual_bps{0.0};  // NaN when not measurable.
    std::size_t   fair_value_observations{0};
    bool          fair_value_residual_valid{false};

    // --- Block height context ---
    BlockHeight last_block{0};      // Most recent block height observed

    // --- [S20 2026-08-24] Engine-supplied external references ---
    // The engine owns the pair graph, so it computes the triangulated
    // implied cross (mid_gate.hpp) from SIBLING pairs' previous-cycle
    // published mids and injects it here each heartbeat, together with the
    // pair's peg target for stablecoin pairs.  Never derived from this
    // pair's own book or history -- that self-reference was the lock-in
    // mechanism behind the 187.461980 incident.
    double      implied_cross{0.0};        // 0 = no healthy triangle this cycle
    double      peg_target{0.0};           // 0 = not a stablecoin pair
    Timestamp   anchor_updated_at{};       // when the engine last injected
    bool        anchor_order_warned{false};// warn-once: offers ingested
                                           // before any anchor existed

    // --- [S20] BBO provenance ---
    // dex_best_bid/ask have TWO writers: ingest_dexie (the raw dexie
    // ticker, which includes OUR OWN resting offers) and
    // ingest_competing_offers (dust-filtered, third-party only,
    // authoritative).  A timestamp cannot distinguish them, because
    // ingest_dexie runs every heartbeat and overwrites both fields before
    // the offers fetch is even attempted.  This flag records which writer
    // produced the values currently sitting there: set by the filtered
    // ingest, CLEARED by the raw one.  Without it, a throwing offers fetch
    // lets the bot read its own quotes back as third-party evidence --
    // confirming the very band breach it should refuse, and marking equity
    // on it.
    bool        bbo_from_filtered_book{false};

    // [S20 2026-08-24] ...and whether that filtering ran against an
    // independent ANCHOR.  Provenance alone is not enough: on a process's
    // first cycle a pair anchored by CEX or AMM has no anchor yet at
    // ingest time (those legs are ingested later in the heartbeat), so its
    // offers pass through unscreened.  The resulting book is genuinely
    // third-party and genuinely fresh, and would therefore satisfy the
    // gate's confirmation escape -- letting one coherent junk book
    // override the anchor that arrives moments later, publish once, and
    // move the peak.  A book nothing screened is not evidence.
    bool        bbo_filter_had_anchor{false};

    // [SIDEQUALITY 2026-09-01] Per-side agreement with the anchor that
    // screened this book.  Written by ingest_competing_offers alongside
    // the filtered BBO, copied verbatim into MarketSnapshot by
    // publish_snapshot; see the long note on MarketSnapshot for why the
    // offers are FLAGGED rather than removed.  Both default true, and
    // both are forced true whenever no independent reference existed --
    // an unscreened book cannot disqualify anything.
    bool        bid_side_anchor_ok{true};
    bool        ask_side_anchor_ok{true};
    double      book_side_ref{0.0};   // anchor used; 0 = nothing screened

    // [S20 2026-08-24] ...and whether that reference was an INDEPENDENT
    // anchor rather than this pair's own last accepted mid.
    //
    // The two must stay separate.  Screening against our own history is
    // enough to let a fresh book confirm a step rejection -- an anchorless
    // pair has nothing better, and the alternative is a permanent no-mid
    // lockout.  It is NOT enough to mark equity: on cycle 1 an anchorless
    // pair has no reference at all, so a coherent junk book publishes and
    // becomes last_accepted_mid; on cycle 2 that junk value would screen
    // the unchanged book, and collapsing the two flags would then promote
    // it to valuation grade.  That is the self-referential lock-in this
    // whole change exists to break, re-entering through the back door.
    bool        bbo_filter_had_independent_anchor{false};

    // --- [S20] Orderbook-mid provenance ---
    // orderbook_mid is written ONLY by ingest_competing_offers.  When the
    // offers fetch throws, that method never runs and the field froze with
    // no age of its own while dex_updated_at kept being re-stamped -- the
    // mechanism that served one junk micro-price byte-identical for 12+
    // hours.  compute_mid now refuses an orderbook_mid older than the
    // stale threshold.  Default epoch = "never stamped this process":
    // treated as fresh so hand-built states keep pre-S20 behaviour (a
    // non-zero orderbook_mid without a stamp only occurs in tests).
    Timestamp   ob_updated_at{};

    // --- Computed fields ---
    double      mid_price{0.0};     // Aggregated mid (dex + optional cex blend)
    double      spread_bps{0.0};    // Current spread in basis points
    bool        is_stale{true};     // True if data is older than stale_threshold

    // --- [S20] Gate outcome for the CURRENT published values ---
    double      last_accepted_mid{0.0};    // last mid that passed the gate
    bool        mid_valuation_grade{false};// see MarketSnapshot for semantics
    bool        gate_reject_logged{false}; // warn-once per rejection episode

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
    /// @param base_mojos_per_unit  Mojos per unit of the pair's base asset.
    /// @param quote_mojos_per_unit Mojos per unit of the pair's quote asset.
    ///        Bid-side offers are denominated in the quote asset; without
    ///        the correct denomination, the dust filter falsely rejects them.
    void ingest_competing_offers(
        const std::string&                 pair_name,
        const std::vector<CompetingOffer>& competing_offers,
        const std::unordered_set<std::string>& own_offer_ids,
        std::int64_t base_mojos_per_unit  = 1'000'000'000'000LL,
        std::int64_t quote_mojos_per_unit = 1'000'000'000'000LL);

    /// Ingest the current block height from the Chia full node.
    ///
    /// @param block_height  Peak block height from get_blockchain_state().
    void ingest_block_height(BlockHeight block_height);

    /// Ingest a CEX reference mid-price for a pair.
    ///
    /// Re-ingesting a cached price is EXPECTED: the engine derives cex_mid
    /// from its CoinGecko cache on every heartbeat, and a failed fetch
    /// leaves that cache in place deliberately.  What must not be recycled
    /// is the TIMESTAMP.
    ///
    /// `observed_at` must be when the price was FETCHED, not when it was
    /// re-ingested.  It is what every CEX freshness gate measures against --
    /// compute_mid's weight taper, detect_stale, and the cex_age heartbeat
    /// -- so stamping the re-ingest instead makes a cached sample look
    /// permanently new and puts all three gates out of reach.  That was the
    /// bug this parameter exists to close, and ingest_amm_mid below records
    /// the same failure on the AMM path.
    ///
    /// A default-constructed value is NOT stored as-is; it falls back to
    /// now(), so do not pass one alongside cached data.
    ///
    /// @param pair_name    Trading pair identifier (must match dexie pair name).
    /// @param cex_mid      CEX mid-price (quote per base).
    /// @param observed_at  When the price was actually fetched.  Defaults to
    ///                     now() for callers that fetch synchronously at the
    ///                     call site; the engine passes its real fetch time.
    void ingest_cex_reference(const std::string& pair_name,
                              double             cex_mid,
                              Timestamp          observed_at
                                  = Timestamp::clock::now());

    /// Ingest the TibetSwap AMM implied mid-price for a pair.
    /// The implied price is computed from pool reserves: output_reserve / input_reserve.
    ///
    /// CALL THIS ONLY WHEN A POOL FETCH ACTUALLY SUCCEEDED.  `observed_at` is
    /// stored verbatim and is what every AMM freshness gate measures against,
    /// so re-ingesting a cached sample with a fresh timestamp would make the
    /// data look permanently new and every one of those gates unreachable.
    ///
    /// @param pair_name    Trading pair identifier.
    /// @param amm_mid      AMM implied mid-price (quote per base).
    /// @param pool_usd     Total USD value of both pool sides; 0 = unknown,
    ///                     which makes the sample unusable as a weighted
    ///                     observation (it cannot be weighted without depth).
    /// @param observed_at  When the pool was actually read.  Defaults to now()
    ///                     for callers that fetch synchronously at the call
    ///                     site; the engine passes its real fetch time.
    void ingest_amm_mid(const std::string& pair_name,
                        double             amm_mid,
                        double             pool_usd = 0.0,
                        Timestamp          observed_at = Timestamp::clock::now());

    /// [S20 2026-08-24] Inject the engine-computed external references for
    /// one pair: the triangulated implied cross (0 when no healthy triangle
    /// exists this cycle) and the pair's peg target (0 for non-stablecoin
    /// pairs).  The engine owns the pair graph, so the triangulation lives
    /// there; the feed only consumes the result -- as the plausibility
    /// anchor for the published mid and as the reference for the per-offer
    /// outlier filter.  Values must never derive from this pair's own book.
    void ingest_reference_anchor(const std::string& pair_name,
                                 double             implied_cross,
                                 double             peg_target);

    /// Ingest an INDEPENDENT fair value for a pair.
    ///
    /// The caller is responsible for the independence guarantee: the value
    /// must not be derived, directly or indirectly, from this pair's dexie
    /// order book.  Unlike ingest_cex_reference this value is NOT blended
    /// into the composite mid; it exists purely to validate the mid.
    ///
    /// @param pair_name   Trading pair identifier.
    /// @param fair_value  Independent reference price (quote per base), > 0.
    /// @param tier        Provenance/confidence of the value.
    void ingest_fair_value(const std::string& pair_name,
                           double             fair_value,
                           FairValueTier      tier);

    /// Full-fidelity form of the above: stores the solved uncertainty, the
    /// consistency residual and the observation count alongside the price.
    ///
    /// A value whose tier is Unavailable is NOT rejected here -- the price is
    /// zeroed (so nothing downstream can mistake it for usable) while the
    /// residual and sigma are retained.  "The books disagree by 12% and I do
    /// not know which is right" is exactly the state the operator most needs
    /// to see, and discarding it would make the failure invisible.
    void ingest_fair_value(const std::string& pair_name, const FairValue& fv);

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

    /// Independent fair value for a pair, with its confidence tier.
    ///
    /// Returns std::nullopt when there is NO independent source: unknown
    /// pair, nothing ever ingested, or the last sample is older than
    /// config.fair_value_max_age_sec.  It deliberately does NOT fall back to
    /// the dexie mid, the composite mid, or anything else touched by the
    /// order book -- a caller that cannot get a fair value must know it is
    /// quoting blind rather than be handed the very number it wanted checked.
    std::optional<FairValue> get_fair_value(const std::string& pair_name) const;

    /// The solve's raw ESTIMATE for a pair, regardless of confidence tier.
    ///
    /// get_fair_value() withholds any estimate whose solved sigma exceeds
    /// fair_value_max_sigma_bps, because CLAMPING against a reference that
    /// uncertain is theatre.  But for CENTRING and WIDTH the sigma is not a
    /// validity flag -- it is the width instruction: at the 2026-08-01 sweep
    /// the solve knew XCH/BYC was worth ~1.36 +- 467 bps while the book said
    /// 1.2673, and discarding that estimate is what left the ladder centred
    /// 10% from the truth.  This accessor returns the estimate WITH its sigma
    /// and tier (which may be Unavailable) so the quoting path can blend it
    /// by uncertainty instead of ignoring it.
    ///
    /// Returns std::nullopt when there is genuinely NO estimate: unknown
    /// pair, a solve that found no anchored path at all, or a sample older
    /// than config.fair_value_max_age_sec.  Never falls back to anything
    /// derived from this pair's own book.
    std::optional<FairValue> get_fair_value_estimate(
        const std::string& pair_name) const;

    /// CONSISTENCY RESIDUAL for a pair, in basis points:
    ///     10 000 * log(book_mid / independent_fair_value)
    ///
    /// Positive means this pair's own book prices the base asset richer than
    /// every other observation in the graph implies.  Unlike get_fair_value
    /// this is reported even when the tier is Unavailable, because the fact
    /// that two books contradict each other is actionable on its own -- it is
    /// the signal that should widen quotes -- whether or not the solve is
    /// confident enough to say which of them is wrong.
    ///
    /// Returns std::nullopt when the pair is unknown, no solve has run, the
    /// last solve had no book to compare against, or the sample has expired.
    std::optional<double> get_fair_value_residual_bps(
        const std::string& pair_name) const;

    /// Raw book inputs the fair-value solve needs, fetched under one lock.
    /// Returns a default-constructed value (has_book == false) for an unknown
    /// pair or one with no two-sided third-party book.
    FairValueObservation get_fair_value_inputs(
        const std::string& pair_name) const;

    /// Age of the current CEX reference in seconds.
    /// Returns std::nullopt if no CEX reference exists for the pair.
    std::optional<double> get_cex_reference_age_seconds(
        const std::string& pair_name) const;

    /// Whether the data for a pair is considered stale.
    /// Returns true if the pair is unknown.
    bool is_stale(const std::string& pair_name) const;

    /// Staleness fraction for graduated spread widening (T3-06).
    /// Returns 0.0 when data is fresh, 1.0 at stale_threshold, >1.0 beyond.
    /// Returns 1.0 if the pair is unknown.
    double get_staleness_fraction(const std::string& pair_name) const;

    /// Number of consecutive competing-offer ingests during which the dex mid
    /// has not moved by more than 1 bp -- i.e. the age of the last PRINT, as
    /// opposed to the age of the last poll that dex_updated_at records.
    /// Returns 0 if the pair is unknown or has never printed.
    std::int32_t dex_print_age(const std::string& pair_name) const;

    /// [S20] Whether the pair's current published mid is valuation grade:
    /// it passed the plausibility gate AND rests on live evidence (fresh
    /// two-sided third-party book or fresh CEX leg).  False for unknown
    /// pairs, no-mid pairs, and last-trade-only or stale-book mids.  See
    /// MarketSnapshot::mid_valuation_grade.
    ///
    /// Acquires mtx_pairs_ (shared), so like get_mid_price it must NOT be
    /// called from an ArbitrageCallback: check_arbitrage invokes that
    /// callback while refresh() still holds mtx_pairs_ exclusively, and a
    /// shared re-acquisition on the same thread self-deadlocks.  Read the
    /// State snapshot's mid_valuation_grade field from a callback instead.
    bool mid_valuation_grade(const std::string& pair_name) const;

    /// [S20] Whether this pair's CURRENT best bid/ask came from the
    /// dust-filtered third-party ingest (not the raw self-inclusive dexie
    /// ticker), that ingest ran within the stale threshold, and the book
    /// mid is not frozen.  This is the "independent book evidence" test:
    /// callers that need to trust a book -- the plausibility gate's
    /// confirmation escape, and triangulated cross legs -- must use this
    /// rather than is_stale(), which is rescued by a fresh CEX sample and
    /// is derived from a poll timestamp that cannot see a frozen book.
    /// Same locking caveat as mid_valuation_grade.
    bool book_evidence_fresh(const std::string& pair_name) const;

    /// Self-filtered dexie top-of-book as {best_bid, best_ask}.
    /// Post-5e1ceb4 a side is 0.0 when no THIRD-PARTY offer exists there, so
    /// {0, 0} means there is no external market to quote against at all.
    /// Returns {0.0, 0.0} if the pair is unknown.
    std::pair<double, double> get_dex_bbo(const std::string& pair_name) const;

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

    /// Retrieve a snapshot of all competing offers for a pair.
    /// Used by gap-detection logic to find uncovered price ranges.
    /// Returns empty vector if competitor tracking is disabled.
    std::vector<CompetingOffer> get_competing_offers(
        const std::string& pair_name) const;

    /// A competing book together with the per-side verdict that measured
    /// THAT book, read under one lock.
    ///
    /// [review round 5] ingest_dexie resets PairState's per-side verdict to
    /// trusted on every raw ticker poll, because those raw prices were
    /// screened by nothing.  But the OFFERS live in a different store that
    /// nothing clears, so after a failed offers fetch the two desync: the
    /// PairState verdict says "trusted" while competing_offers_ still holds
    /// the previous cycle's junk book.  A consumer reading the offers from
    /// one place and the verdict from the other then anchors against a book
    /// no live verdict describes -- re-arming precisely the self-cross this
    /// work exists to stop.
    ///
    /// So the verdict is stored BESIDE the offers and handed out with them.
    /// A consumer that uses both must use this, not the snapshot.
    struct CompetingBook {
        std::vector<CompetingOffer> offers;
        bool   bid_side_anchor_ok{true};
        bool   ask_side_anchor_ok{true};
        double book_side_ref{0.0};
    };

    [[nodiscard]] CompetingBook get_competing_book(
        const std::string& pair_name) const;

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
    double compute_mid(const PairState& ps) const;

    /// Compute the spread in basis points from best_bid and best_ask.
    /// Returns 0.0 if either side is zero (no two-sided market).
    static double compute_spread_bps(double best_bid, double best_ask);

    /// Detect staleness by comparing the data timestamp to now().
    /// Returns true if (now - ts) > stale_threshold, or if ts is epoch-zero
    /// (never updated).
    bool detect_stale(Timestamp ts) const;

    /// [S20] Run the plausibility gate on the freshly computed mid and set
    /// the gate outcome fields on ps (mid_price zeroed on rejection,
    /// mid_valuation_grade, last_accepted_mid).  Called from refresh()
    /// between compute_mid and append_price_history/publish_snapshot so
    /// every downstream consumer -- price history, flash-crash window,
    /// snapshot readers -- sees only gated values.  Caller holds mtx_pairs_.
    void apply_mid_gate(PairState& ps, const MarketDataConfig& cfg);

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
    /// [review round 5] Per-side verdict for the book in competing_offers_,
    /// written in the SAME critical section so the two cannot desync.
    struct BookQuality { bool bid_ok{true}; bool ask_ok{true}; double ref{0.0}; };
    std::unordered_map<std::string, BookQuality> competing_book_quality_;

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
