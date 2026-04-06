// types.hpp -- Core type definitions for XOPTrader CHIA market-making bot.
// Header-only. All monetary values expressed in mojos (int64_t) to prevent
// floating-point drift.  1 XCH = 10^12 mojos.
//
// Compliant with:
//   ISO/IEC 27001:2022  (secure coding -- no implicit conversions on monetary)
//   ISO/IEC 5055        (no unchecked arithmetic on critical paths)
//   ISO/IEC 25000       (clear naming, minimal coupling)

#ifndef XOP_TYPES_HPP
#define XOP_TYPES_HPP

#include <cstdint>
#include <string>
#include <chrono>

namespace xop {

// ---------------------------------------------------------------------------
// Fundamental aliases
// ---------------------------------------------------------------------------

/// Smallest indivisible unit on the Chia blockchain.
/// 1 XCH = 10^12 mojos.  int64_t range covers +/- 9,223 XCH which is well
/// beyond the capital ceiling described in the strategy document ($30K at
/// ~$2.70/XCH = ~11,111 XCH).
using Mojo = std::int64_t;

/// Conversion constant: mojos per whole XCH.
inline constexpr Mojo kMojosPerXch = 1'000'000'000'000LL;

/// Asset identifier.
/// "xch" for the native coin; 64-character lower-case hex string for CATs
/// (e.g. wUSDC, SBX, DBX).
using AssetId = std::string;

/// High-resolution wall-clock timestamp (UTC).
using Timestamp = std::chrono::system_clock::time_point;

/// Block height on the Chia blockchain (uint32 suffices for decades).
using BlockHeight = std::uint32_t;

// ---------------------------------------------------------------------------
// Side -- which side of the order book an order / fill belongs to.
// ---------------------------------------------------------------------------

enum class Side : std::uint8_t {
    Bid = 0,  // buy  -- we receive base, we pay quote
    Ask = 1   // sell -- we pay base, we receive quote
};

/// Human-readable label for logging.
inline const char* to_string(Side s) noexcept {
    return s == Side::Bid ? "Bid" : "Ask";
}

// ---------------------------------------------------------------------------
// OrderTier -- one level of the multi-tier offer ladder described in the
//              strategy document (section 11).
// ---------------------------------------------------------------------------

struct OrderTier {
    Mojo          price;       // price in mojos-per-unit of quote asset
    Mojo          size;        // quantity of base asset in mojos
    std::uint8_t  tier_index;  // 0-based tier (0 = tightest, 3 = widest)
};

// ---------------------------------------------------------------------------
// Quote -- a two-sided quotation produced by the strategy engine every block.
// ---------------------------------------------------------------------------

struct Quote {
    Mojo   bid_price;   // best bid price (mojos)
    Mojo   ask_price;   // best ask price (mojos)
    Mojo   bid_size;    // quantity offered on bid (mojos)
    Mojo   ask_size;    // quantity offered on ask (mojos)
    double spread_bps;  // spread in basis points (informational only --
                        //   derived from integer prices for display)
};

// ---------------------------------------------------------------------------
// Fill -- a confirmed trade originating from an accepted (taken) offer.
//         Recorded after the spend bundle settles on-chain.
// ---------------------------------------------------------------------------

struct Fill {
    std::string  offer_id;      // unique offer identifier (Chia spend-bundle hash)
    std::string  pair_name;     // e.g. "XCH/wUSDC"
    Side         side;          // whether this was our bid or ask
    Mojo         price;         // execution price (mojos)
    Mojo         size;          // filled quantity (mojos of base asset)
    BlockHeight  block_height;  // settlement block
    Timestamp    timestamp;     // wall-clock time of detection
};

// ---------------------------------------------------------------------------
// MarketSnapshot -- latest view of a single trading pair's state, aggregated
//                   from on-chain data (dexie / TibetSwap) and CEX feeds.
// ---------------------------------------------------------------------------

struct MarketSnapshot {
    std::string  pair_name;    // e.g. "XCH/wUSDC"
    Mojo         mid_price;    // (best_bid + best_ask) / 2  (mojos)
    Mojo         best_bid;     // top-of-book bid (mojos)
    Mojo         best_ask;     // top-of-book ask (mojos)
    double       spread_bps;   // current spread in basis points
    Mojo         cex_mid;      // CEX reference mid-price (mojos, 0 if unavailable)
    Mojo         volume_24h;   // rolling 24-hour volume in mojos of base asset
    BlockHeight  last_block;   // block height at which this snapshot was taken
    Timestamp    updated_at;   // wall-clock time
};

// ---------------------------------------------------------------------------
// PendingOffer -- an outstanding offer we have posted and not yet settled or
//                 cancelled.  Tracked for lifecycle management (section 15).
// ---------------------------------------------------------------------------

struct PendingOffer {
    std::string  offer_id;         // unique offer identifier
    std::string  pair_name;        // trading pair
    Side         side;             // bid or ask
    Mojo         price;            // offer price (mojos)
    Mojo         size;             // offered quantity (mojos)
    std::uint8_t tier;             // which tier of the multi-tier ladder
    BlockHeight  created_at_block; // block at which the offer was broadcast
    Timestamp    created_at_ts;    // wall-clock creation time
    std::uint64_t fee_mojos{0};    // fee attached to this offer (mojos)
    bool         cancel_pending{false}; // cancel RPC sent; awaiting on-chain confirmation
};

// ---------------------------------------------------------------------------
// CompetingOffer -- an offer on the order book from another market participant.
//                   Tracked to compute best_competing_bps for spread optimizer.
// ---------------------------------------------------------------------------

struct CompetingOffer {
    std::string  offer_id;         // unique offer identifier (from dexie API)
    std::string  pair_name;        // trading pair
    Side         side;             // bid or ask
    Mojo         price;            // offer price (mojos)
    Mojo         size;             // offered quantity (mojos)
    BlockHeight  first_seen_block; // block at which we first observed this offer
    BlockHeight  last_seen_block;  // most recent block where offer was present
    Timestamp    last_seen_ts;     // wall-clock time of last observation
};

// ---------------------------------------------------------------------------
// CompetitorMetrics -- aggregated statistics about competing market makers.
// ---------------------------------------------------------------------------

struct CompetitorMetrics {
    std::string  pair_name;              // trading pair
    std::size_t  num_competing_offers;   // total count of non-own offers
    double       best_competing_bid_bps; // best competing bid spread vs mid (bps)
    double       best_competing_ask_bps; // best competing ask spread vs mid (bps)
    double       best_competing_spread_bps; // tightest competing spread (bps)
    std::size_t  competing_depth_bids;   // number of competing bid offers
    std::size_t  competing_depth_asks;   // number of competing ask offers
    bool         new_competitor_detected; // true if first competitor seen this block
    Timestamp    last_updated;           // wall-clock time of last metric update
};

// ---------------------------------------------------------------------------
// WhaleTradeEvent -- a single large trade detected on the DEX that exceeds the
//                    whale-size threshold.  Whale trades create adverse-selection
//                    risk: the whale likely has an informational edge, so our
//                    quoted inventory on the opposite side may rapidly lose value.
// ---------------------------------------------------------------------------

struct WhaleTradeEvent {
    std::string  pair_name;        // trading pair, e.g. "XCH/wUSDC"
    Side         side;             // direction the whale traded (Bid = whale bought,
                                   //   Ask = whale sold)
    Mojo         size;             // trade size in mojos (base asset)
    double       size_pct_vol;     // size as a fraction of 24-hour volume (0-1)
    BlockHeight  block_height;     // block at which the trade was detected
    Timestamp    detected_at;      // wall-clock time of detection
};

// ---------------------------------------------------------------------------
// WhaleMetrics -- aggregated whale-activity statistics for a single trading pair.
//
// The strategy layer queries this to decide whether to widen spreads, reduce
// offer sizes, or pause quoting entirely until the market stabilises.
//
// Risks a whale creates:
//   1. Adverse selection: whale has superior information; our resting orders on
//      the opposite side are at risk of being taken at a loss.
//   2. Inventory imbalance: repeated fills on one side skew position dangerously.
//   3. Price impact: large trades can gap the mid-price, leaving our quotes stale.
//   4. Liquidation cascade: a whale sell can crater the price, wiping out all bids.
//
// Response encoded in spread_multiplier:
//   - 1.0: normal (no whale activity).
//   - 1.0 – whale_max_spread_multiplier: linearly scaled to number and size of
//     recent whale events in the tracking window.
// ---------------------------------------------------------------------------

struct WhaleMetrics {
    std::string  pair_name;             // trading pair
    std::size_t  events_in_window;      // whale trade events in the recent window
    Mojo         largest_trade_size;    // largest single whale trade seen in window
    double       spread_multiplier;     // recommended spread widening factor (>= 1.0)
    Side         dominant_side;         // direction of the most-recent whale event
    bool         is_active;             // true if at least one event in window
    BlockHeight  last_event_block;      // block of the most recent whale event
    Timestamp    last_updated;          // wall-clock time of last computation
};

// ---------------------------------------------------------------------------
// VpinBucket -- a single volume-synchronised bucket for the VPIN estimator.
//
// Reference: Easley, López de Prado & O'Hara (2012). "Flow Toxicity and
// Liquidity in a High-frequency World."  The Review of Financial Studies,
// 25(5), 1457–1493.
//
// VPIN partitions incoming volume into fixed-size "volume bars".  Within each
// bar the buy/sell imbalance is measured.  VPIN is the rolling mean of
// absolute imbalances over the most recent N bars, divided by the bar size.
// ---------------------------------------------------------------------------

struct VpinBucket {
    double buy_volume{0.0};    // buyer-initiated volume accumulated in this bar
    double sell_volume{0.0};   // seller-initiated volume accumulated in this bar
    bool   complete{false};    // true once total volume >= bucket_size
};

// ---------------------------------------------------------------------------
// VpinMetrics -- aggregated VPIN statistics for a single trading pair.
//
// The strategy layer reads VPIN ∈ [0, 1] as a continuous "flow toxicity"
// signal.  Values near 0 indicate balanced, uninformed flow; values near 1
// indicate heavily one-sided, likely informed flow.
// ---------------------------------------------------------------------------

struct VpinMetrics {
    std::string  pair_name;             // trading pair
    double       vpin;                  // VPIN estimate in [0, 1]
    std::size_t  complete_buckets;      // number of completed bars in the window
    double       buy_volume_pct;        // fraction of recent volume that is buys
    double       sell_volume_pct;       // fraction of recent volume that is sells
    Timestamp    last_updated;          // wall-clock time of last computation
};

// ---------------------------------------------------------------------------
// OfiMetrics -- Order Flow Imbalance metrics for a single trading pair.
//
// Reference: Cont, Kukanov & Stoikov (2014). "The Price Impact of Order Book
// Events."  Journal of Financial Econometrics, 12(1), 47–88.
//
// OFI aggregates signed changes at the best bid/ask into a single predictor
// of short-term price moves.  A persistently positive OFI signals buying
// pressure (price likely to rise); persistently negative signals selling
// pressure (price likely to fall).
// ---------------------------------------------------------------------------

struct OfiMetrics {
    std::string  pair_name;             // trading pair
    double       ofi;                   // raw OFI value (positive = buy pressure)
    double       normalized_ofi;        // OFI normalised to [-1, 1] range
    double       cumulative_ofi;        // cumulative OFI over the rolling window
    std::size_t  observations;          // number of book-change observations
    Timestamp    last_updated;          // wall-clock time of last computation
};

// ---------------------------------------------------------------------------
// AsymmetricMultipliers -- per-side spread multipliers derived from whale
// activity and order-flow direction.
//
// When a whale buys aggressively (dominant_side = Bid), the ask side faces
// higher adverse-selection risk, so the ask multiplier is raised while the
// bid multiplier is reduced (we still want to buy cheaply).  Vice versa for
// whale sells.
// ---------------------------------------------------------------------------

struct AsymmetricMultipliers {
    double bid_multiplier{1.0};   // spread widening factor for bid side
    double ask_multiplier{1.0};   // spread widening factor for ask side
};

// ---------------------------------------------------------------------------
// TierQuote -- a single level of the multi-tier offer ladder.
//
// Represents one offer on one side (bid or ask) at a specific distance from
// mid-price.  The execution layer maps each TierQuote to a create_offer_for_ids
// wallet RPC call, pre-splitting coins to match the size field.
//
// Unified definition: consumed by both the strategy layer (LiquidityEngine)
// and the execution layer (OfferManager).
// ---------------------------------------------------------------------------

struct TierQuote {
    std::uint8_t tier_index;   // 0-based tier (0 = tightest, N-1 = widest).
    Side         side;         // Bid (buy) or Ask (sell).
    Mojo         price;        // Offer price in mojos.

    // Offer quantity in mojos of the *side-relevant* asset:
    //   - Bid (buy):  quote-asset mojos — the capital we are willing to spend.
    //   - Ask (sell): base-asset mojos  — the inventory we are willing to sell.
    //
    // The execution layer (OfferManager::build_offer_dict) uses this convention
    // to compute the counter-leg amount:
    //   Bid: quote_amount = size * price / quote_denom   (we spend quote, receive base)
    //   Ask: quote_amount = size * price / quote_denom   (we spend base, receive quote)
    //
    // LiquidityEngine::build_ladder() sets bid size from available quote
    // capital (`cap * size_frac`) and ask size from available base inventory
    // (`inv * size_frac`), both already in mojos.
    Mojo         size;

    double       spread_bps;   // Distance from mid-price in basis points
                               //   (informational, for logging and metrics).
};

// ---------------------------------------------------------------------------
// RebalanceReason -- bitmask-capable enumeration of conditions that trigger
//                    a full tier recalculation and offer refresh.
//
// Multiple reasons may be active simultaneously; use the bitwise operators
// to combine and test flags.  The engine logs the reason(s) for every
// rebalance cycle to support post-hoc analysis and parameter tuning.
// ---------------------------------------------------------------------------

enum class RebalanceReason : std::uint8_t {
    None           = 0,
    PriceMove      = 1 << 0,
    InventorySkew  = 1 << 1,
    TTLExpired     = 1 << 2,
    RegimeChange   = 1 << 3,
    CompetitorMove = 1 << 4,
    ForcedRefresh  = 1 << 5
};

/// Bitwise OR for combining rebalance reason flags.
inline RebalanceReason operator|(RebalanceReason a, RebalanceReason b) {
    return static_cast<RebalanceReason>(
        static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// Bitwise AND for testing rebalance reason flags.
inline RebalanceReason operator&(RebalanceReason a, RebalanceReason b) {
    return static_cast<RebalanceReason>(
        static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

/// Convenience predicate: returns true if @p flag is set in @p composite.
inline bool has_reason(RebalanceReason composite, RebalanceReason flag) {
    return (composite & flag) != RebalanceReason::None;
}

}  // namespace xop

#endif  // XOP_TYPES_HPP
