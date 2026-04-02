// order_book_tactics.hpp -- Order-book-aware market-making tactics module
//                          for XOPTrader CHIA DEX market-making bot.
//
// Implements the six order-book interaction tactics from the trading
// strategies document section 7.10.  The module analyses the current order
// book state (depth, toxicity, competition, inventory) and recommends which
// tactic to use for quote placement.  The goal is to coexist with unknown
// market makers by finding and filling gaps in the order book rather than
// competing head-on (avoiding destructive Bertrand price wars).
//
// Tactics:
//   0  JoinInside       -- Join the inside queue (low toxicity, thin queue).
//   1  ImproveByOne     -- Improve by one price increment (buy queue priority).
//   2  StepBack         -- Step back / fade one tier (preserve capital).
//   3  LayerMultiple    -- Layer multiple tiers (uncertain direction).
//   4  AsymmetricSize   -- Asymmetric sizing (inventory-driven rebalancing).
//   5  HybridRebalance  -- Hybrid maker-taker rebalancing (extreme imbalance).
//
// Anti-feedback mechanisms (section 7.10):
//   - Minimum spread floor (Bertrand race circuit breaker).
//   - Fill-rate crowding detection.
//   - Inventory-driven quote skew (reinforces A-S / GLFT skew).
//   - Self-detection for own-family offers in multi-instance deployments.
//
// Thread safety: thread-safe via std::shared_mutex (T2-02).
// Read operations (apply, is_market_crowded, is_own_family_offer, config)
// acquire a shared lock.  Write operations (recommend, register_own_offer,
// clear_own_offers) acquire an exclusive lock.
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets; pure algorithmic logic)
//   ISO/IEC 5055        (no raw pointers; bounds-checked containers; no UB)
//   ISO/IEC 25000       (clear naming; comprehensive documentation)
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++20)

#ifndef XOP_STRATEGY_ORDER_BOOK_TACTICS_HPP
#define XOP_STRATEGY_ORDER_BOOK_TACTICS_HPP

#include <xop/strategy/base.hpp>  // MarketRegime

#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_set>

namespace xop {

// ---------------------------------------------------------------------------
// BookTactic -- the six order-book interaction tactics from section 7.10.
//
// Each tactic represents a distinct quoting posture with different trade-offs
// between fill probability, adverse selection risk, and queue priority.
// ---------------------------------------------------------------------------

enum class BookTactic : std::uint8_t {
    JoinInside      = 0,  // Join the inside queue at the current best price.
    ImproveByOne    = 1,  // Improve by one price increment to gain priority.
    StepBack        = 2,  // Step back / fade one tier to preserve capital.
    LayerMultiple   = 3,  // Layer quotes across multiple tiers for breadth.
    AsymmetricSize  = 4,  // Asymmetric sizing: larger on inventory-reducing side.
    HybridRebalance = 5   // Cross the spread to rebalance (taker action).
};

/// Human-readable label for logging and metrics.
inline const char* to_string(BookTactic t) noexcept {
    switch (t) {
        case BookTactic::JoinInside:      return "JoinInside";
        case BookTactic::ImproveByOne:    return "ImproveByOne";
        case BookTactic::StepBack:        return "StepBack";
        case BookTactic::LayerMultiple:   return "LayerMultiple";
        case BookTactic::AsymmetricSize:  return "AsymmetricSize";
        case BookTactic::HybridRebalance: return "HybridRebalance";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// TacticRecommendation -- the output of the tactic selection engine.
//
// Encapsulates the chosen tactic together with sizing adjustments and a
// human-readable rationale for audit logging.
// ---------------------------------------------------------------------------

struct TacticRecommendation {
    BookTactic  tactic;                  // Selected tactic.
    double      confidence;              // [0,1] strength of recommendation.
    double      bid_size_factor;         // Multiplier on bid size (1.0 = normal).
    double      ask_size_factor;         // Multiplier on ask size (1.0 = normal).
    int         target_tier;             // Tier to place quotes at (-1 = model decides).
    double      spread_adjustment_bps;   // Additive spread adjustment in bps
                                         //   (negative = tighten, positive = widen).
    std::string reason;                  // Human-readable rationale for audit log.
};

// ---------------------------------------------------------------------------
// BookState -- snapshot of the order book and market micro-structure.
//
// Populated by the market-data layer each block and passed to the tactic
// recommender.  All rates and ratios are normalised to [0,1] or [-1,1] as
// documented per field.
// ---------------------------------------------------------------------------

struct BookState {
    double      mid_price;            // Current mid-price (quote per base).
    double      best_bid;             // Best bid price on the book.
    double      best_ask;             // Best ask price on the book.
    double      our_spread_bps;       // Our model's recommended base spread (bps).
    double      best_competing_bps;   // Tightest competitor's spread (bps).
    std::size_t bid_depth;            // Number of competing bid offers at or near best.
    std::size_t ask_depth;            // Number of competing ask offers at or near best.
    double      vpin;                 // VPIN flow-toxicity estimate in [0,1].
    double      normalized_ofi;       // Order flow imbalance in [-1,1].
    double      inventory_ratio;      // |q| / q_max in [0,1].
    double      fill_rate_24h;        // 24-hour fill rate in [0,1].
    bool        whale_active;         // True if whale activity detected.
    MarketRegime regime;              // Current market regime classification.
};

// ---------------------------------------------------------------------------
// OrderBookTacticsConfig -- tuning parameters for the tactic selection engine.
//
// Defaults are calibrated for CHIA DEX conditions (section 7.10).  The
// spread_min_bps floor is the primary Bertrand-race circuit breaker.
// ---------------------------------------------------------------------------

struct OrderBookTacticsConfig {
    // -- Spread floor (Bertrand race circuit breaker) -------------------------
    /// Absolute minimum spread in bps.  No tactic may produce a spread below
    /// this value.  Prevents destructive price competition.
    double spread_min_bps{50.0};

    // -- Anti-feedback thresholds ---------------------------------------------
    /// Fill rate below this level indicates a crowded market (too many MMs).
    /// Section 0.3 says target fill rate > 30%; default threshold set to 20%
    /// to trigger step-back only in clearly crowded conditions.
    double crowding_fill_rate_threshold{0.20};

    /// VPIN level above which we step back to avoid informed-flow losses.
    double vpin_step_back_threshold{0.60};

    /// Absolute OFI value above which asymmetric sizing is warranted.
    double ofi_asymmetry_threshold{0.30};

    /// Inventory ratio above which hybrid maker-taker rebalancing activates.
    double hybrid_rebalance_threshold{0.80};

    // -- Queue depth parameters -----------------------------------------------
    /// If either side has more offers than this at the best price, the queue
    /// is considered "deep" (joining it yields poor fill probability).
    std::size_t deep_queue_threshold{5};

    // -- Tactic-specific parameters -------------------------------------------
    /// One-tick improvement increment in bps (ImproveByOne tactic).
    double improvement_tick_bps{1.0};

    /// Size multiplier for the inventory-reducing side (AsymmetricSize tactic).
    double asymmetric_size_ratio{1.5};

    /// Spread widening applied by the StepBack tactic (additive bps).
    double step_back_widening_bps{20.0};

    // -- Tactic hysteresis -------------------------------------------------------
    /// Number of consecutive blocks a new tactic must be selected before the
    /// switch is committed.  Prevents quote instability when inventory_ratio
    /// or other inputs oscillate near a decision threshold.  A value of 1
    /// disables hysteresis (immediate switching, legacy behaviour).
    std::uint32_t tactic_hysteresis_blocks{3};

    // -- Self-detection (multi-instance) --------------------------------------
    /// When true, the tactician filters out own-family offers before computing
    /// competing depth.  Required when running multiple XOPTrader instances
    /// on the same pair to avoid self-competition.
    bool enable_self_detection{false};

    /// Instance fingerprint string for self-detection.
    std::string instance_id;
};

// ---------------------------------------------------------------------------
// OrderBookTactician -- primary class for order-book interaction tactics.
//
// Usage (once per block heartbeat):
//
//     OrderBookTactician tactician(config);
//     BookState state = build_book_state(/* market data */);
//     auto rec = tactician.recommend(state);
//     auto adj = tactician.apply(rec, base_spread_bps);
//     // Use adj.spread_bps, adj.bid_size_factor, adj.ask_size_factor
//     // to modify the quote before submitting.
//
// The tactician is predominantly stateless -- only the own-offer registry
// (for self-detection) carries mutable state across calls.
// ---------------------------------------------------------------------------

class OrderBookTactician {
public:
    /// Construct with the given configuration.  No allocation beyond the
    /// config copy and the (initially empty) self-detection set.
    explicit OrderBookTactician(const OrderBookTacticsConfig& cfg) noexcept;

    // -- Primary interface ----------------------------------------------------

    /// Analyse the current book state and recommend a tactic.
    ///
    /// The selection follows a priority chain (see select_tactic):
    ///   1. HybridRebalance  if inventory critically imbalanced.
    ///   2. StepBack          if VPIN/whale/crowding signals danger.
    ///   3. AsymmetricSize    if inventory skewed and OFI confirms direction.
    ///   4. LayerMultiple     if book is deep on both sides.
    ///   5. ImproveByOne      if queue is deep and spread has room.
    ///   6. JoinInside        otherwise (default, safest posture).
    ///
    /// Note: non-const because it updates internal hysteresis state (T3-19).
    TacticRecommendation recommend(const BookState& state);

    /// Adjusted quote parameters after applying a tactic recommendation.
    struct AdjustedQuote {
        double spread_bps;        // Final spread after adjustment and floor.
        double bid_size_factor;   // Bid-side size multiplier.
        double ask_size_factor;   // Ask-side size multiplier.
    };

    /// Apply the recommendation to a base spread.  Enforces the spread_min_bps
    /// floor as an absolute lower bound (Bertrand race circuit breaker).
    ///
    /// @param rec              The tactic recommendation from recommend().
    /// @param base_spread_bps  The model's base spread before tactic adjustment.
    /// @return Adjusted spread and size factors.
    AdjustedQuote apply(const TacticRecommendation& rec,
                        double base_spread_bps) const;

    // -- Anti-feedback queries ------------------------------------------------

    /// Returns true if the 24-hour fill rate indicates a crowded market
    /// (too many market makers competing for the same flow).
    bool is_market_crowded(double fill_rate_24h) const;

    // -- Self-detection for multi-instance deployments ------------------------

    /// Check whether a given offer ID belongs to our own family of instances.
    /// Always returns false when self-detection is disabled.
    bool is_own_family_offer(const std::string& offer_id) const;

    /// Register one of our own offer IDs for self-detection filtering.
    void register_own_offer(const std::string& offer_id);

    /// Clear all registered own offer IDs (e.g. on restart or reconciliation).
    void clear_own_offers();

    // -- Configuration access -------------------------------------------------

    /// Read-only access to the active configuration.
    /// [T8-01] Returns by value to prevent dangling reference -- the
    /// shared_lock that protects cfg_ would expire before the caller
    /// uses the returned reference.
    OrderBookTacticsConfig config() const noexcept;

private:
    // -- Tactic selection (priority chain) ------------------------------------

    /// Select the appropriate tactic based on the current book state.
    /// Implements the priority chain documented in recommend().
    /// Applies hysteresis: a new tactic must be confirmed for
    /// tactic_hysteresis_blocks consecutive blocks before it replaces the
    /// active tactic, preventing quote instability from threshold
    /// oscillation (T3-19).
    BookTactic select_tactic(const BookState& state);

    // -- Individual tactic evaluators -----------------------------------------
    // Each evaluator builds a TacticRecommendation with tactic-specific
    // confidence, sizing, spread adjustment, and rationale string.

    /// Tactic 0: Join the inside queue.
    /// Low toxicity, thin queue, stable regime.  Maximises fill probability.
    TacticRecommendation eval_join_inside(const BookState& state) const;

    /// Tactic 1: Improve by one price increment.
    /// Deep queue at best price but spread is still wide.  Buys priority.
    TacticRecommendation eval_improve(const BookState& state) const;

    /// Tactic 2: Step back / fade one tier.
    /// High VPIN, whale active, or crowded market.  Preserves capital.
    TacticRecommendation eval_step_back(const BookState& state) const;

    /// Tactic 3: Layer multiple tiers.
    /// Uncertain direction, deep book.  Continuous presence across tiers.
    TacticRecommendation eval_layer(const BookState& state) const;

    /// Tactic 4: Asymmetric sizing.
    /// Inventory imbalanced, OFI confirms direction.  Larger on reducing side.
    TacticRecommendation eval_asymmetric(const BookState& state) const;

    /// Tactic 5: Hybrid maker-taker rebalancing.
    /// Extreme inventory imbalance or stale book.  Crosses the spread.
    TacticRecommendation eval_hybrid_rebalance(const BookState& state) const;

    // -- Thread safety (T2-02) -----------------------------------------------
    // Mutable to allow shared (read) locking in const accessor methods.
    mutable std::shared_mutex mtx_;

    // -- Data members ---------------------------------------------------------

    OrderBookTacticsConfig              cfg_;           // Immutable configuration.
    std::unordered_set<std::string>     own_offer_ids_; // Self-detection registry.

    // -- Tactic hysteresis state (T3-19) --------------------------------------
    // Prevents rapid tactic switching when inputs oscillate near thresholds.
    // A candidate tactic must persist for tactic_hysteresis_blocks consecutive
    // evaluations before it replaces the currently active tactic.

    /// The currently committed tactic; starts as JoinInside (safest posture).
    BookTactic    active_tactic_{BookTactic::JoinInside};

    /// Candidate tactic that is accumulating confirmation blocks.
    BookTactic    pending_tactic_{BookTactic::JoinInside};

    /// Number of consecutive blocks the pending tactic has been selected by
    /// the raw priority chain.  Resets to zero when the raw selection changes.
    std::uint32_t pending_tactic_blocks_{0};
};

}  // namespace xop

#endif  // XOP_STRATEGY_ORDER_BOOK_TACTICS_HPP
