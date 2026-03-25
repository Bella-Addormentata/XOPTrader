// order_book_tactics.cpp -- Order-book interaction tactics implementation.
//
// Implements the six quoting tactics from the trading strategies document
// section 7.10.  Each tactic is evaluated by a dedicated method; the
// select_tactic() priority chain determines which one fires for a given
// book state.
//
// Key design principle: coexistence over competition.  The module aims to
// fill gaps in the order book rather than undercut other market makers,
// avoiding destructive Bertrand price wars that erode spreads to zero.
//
// Anti-feedback mechanisms:
//   1. spread_min_bps floor -- hard circuit breaker on minimum spread.
//   2. Fill-rate crowding   -- detects when too many MMs compete for flow.
//   3. Inventory skew       -- reinforces A-S / GLFT inventory-driven skew.
//   4. Self-detection       -- prevents own-instance competition.
//
// ISO/IEC 27001:2022 -- no secrets handled; pure algorithmic logic.
// ISO/IEC 5055       -- no raw pointers; bounds-checked; no undefined behaviour.
// ISO/IEC 25000      -- comprehensive inline documentation per function.
// ISO/IEC JTC 1/SC 22 -- standard-conforming C++20.

#include <xop/strategy/order_book_tactics.hpp>

#include <algorithm>
#include <cmath>
#include <shared_mutex>
#include <string>

#include <spdlog/spdlog.h>

namespace xop {

// ===========================================================================
// Construction
// ===========================================================================

OrderBookTactician::OrderBookTactician(const OrderBookTacticsConfig& cfg) noexcept
    : cfg_(cfg)
{
    // Reserve a modest bucket count for the self-detection hash set to
    // avoid repeated rehashing during early offer registration.
    if (cfg_.enable_self_detection) {
        own_offer_ids_.reserve(64);
    }
}

// ===========================================================================
// Primary interface -- recommend
// ===========================================================================

TacticRecommendation OrderBookTactician::recommend(const BookState& state)
{
    // T2-02: Exclusive lock -- recommend mutates hysteresis state
    // (active_tactic_, pending_tactic_, pending_tactic_blocks_) via
    // select_tactic().
    std::unique_lock lock(mtx_);

    // Select the best tactic via the priority chain.
    // select_tactic() applies hysteresis internally (T3-19).
    const BookTactic tactic = select_tactic(state);

    // Delegate to the tactic-specific evaluator to build the full
    // recommendation (confidence, sizing, spread adjustment, rationale).
    TacticRecommendation rec{};
    switch (tactic) {
        case BookTactic::JoinInside:
            rec = eval_join_inside(state);
            break;
        case BookTactic::ImproveByOne:
            rec = eval_improve(state);
            break;
        case BookTactic::StepBack:
            rec = eval_step_back(state);
            break;
        case BookTactic::LayerMultiple:
            rec = eval_layer(state);
            break;
        case BookTactic::AsymmetricSize:
            rec = eval_asymmetric(state);
            break;
        case BookTactic::HybridRebalance:
            rec = eval_hybrid_rebalance(state);
            break;
    }

    spdlog::debug("[OrderBookTactics] tactic={} confidence={:.2f} "
                  "spread_adj={:.1f}bps bid_sf={:.2f} ask_sf={:.2f} -- {}",
                  to_string(rec.tactic), rec.confidence,
                  rec.spread_adjustment_bps,
                  rec.bid_size_factor, rec.ask_size_factor,
                  rec.reason);

    return rec;
}

// ===========================================================================
// apply -- translate a recommendation into adjusted quote parameters
// ===========================================================================

OrderBookTactician::AdjustedQuote
OrderBookTactician::apply(const TacticRecommendation& rec,
                          double base_spread_bps) const
{
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

    // Apply the additive spread adjustment from the tactic.
    double adjusted = base_spread_bps + rec.spread_adjustment_bps;

    // Enforce the absolute spread floor -- the Bertrand race circuit breaker.
    // No tactic, regardless of its adjustment, may breach this floor.
    if (adjusted < cfg_.spread_min_bps) {
        spdlog::debug("[OrderBookTactics] Spread floor enforced: "
                      "adjusted={:.1f}bps < min={:.1f}bps",
                      adjusted, cfg_.spread_min_bps);
        adjusted = cfg_.spread_min_bps;
    }

    return AdjustedQuote{
        adjusted,
        rec.bid_size_factor,
        rec.ask_size_factor
    };
}

// ===========================================================================
// Anti-feedback -- crowding detection
// ===========================================================================

bool OrderBookTactician::is_market_crowded(double fill_rate_24h) const
{
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);

    // A fill rate below the crowding threshold means too many MMs are
    // competing for limited flow.  The strategy doc targets > 30% fill
    // rate; the default threshold is 20% (configurable).
    return fill_rate_24h < cfg_.crowding_fill_rate_threshold;
}

// ===========================================================================
// Self-detection -- own-family offer management
// ===========================================================================

bool OrderBookTactician::is_own_family_offer(const std::string& offer_id) const
{
    // T2-02: Shared lock -- read-only access to cfg_ and own_offer_ids_.
    std::shared_lock lock(mtx_);

    // Self-detection is only meaningful when enabled and the set is populated.
    if (!cfg_.enable_self_detection) {
        return false;
    }
    return own_offer_ids_.count(offer_id) > 0;
}

void OrderBookTactician::register_own_offer(const std::string& offer_id)
{
    // T2-02: Exclusive lock -- register_own_offer mutates own_offer_ids_.
    std::unique_lock lock(mtx_);

    if (cfg_.enable_self_detection && !offer_id.empty()) {
        own_offer_ids_.insert(offer_id);
    }
}

void OrderBookTactician::clear_own_offers()
{
    // T2-02: Exclusive lock -- clear_own_offers mutates own_offer_ids_.
    std::unique_lock lock(mtx_);
    own_offer_ids_.clear();
}

// ===========================================================================
// Configuration accessor
// ===========================================================================

const OrderBookTacticsConfig& OrderBookTactician::config() const noexcept
{
    // T2-02: Shared lock -- read-only access to cfg_.
    std::shared_lock lock(mtx_);
    return cfg_;
}

// ===========================================================================
// Tactic selection -- priority chain
// ===========================================================================
//
// The priority chain implements a "most urgent first" ordering.  Conditions
// that pose the greatest risk to capital are checked first; benign conditions
// fall through to the default (JoinInside).
//
// Priority (highest to lowest):
//   1. HybridRebalance  -- inventory critically imbalanced; must act now.
//   2. StepBack          -- toxic flow, whale, or crowding detected.
//   3. AsymmetricSize    -- moderate inventory skew confirmed by OFI.
//   4. LayerMultiple     -- deep book on both sides; spread presence.
//   5. ImproveByOne      -- deep queue and wide spread; buy priority.
//   6. JoinInside        -- default safe posture.

BookTactic OrderBookTactician::select_tactic(const BookState& state)
{
    // --- Raw priority chain -------------------------------------------------
    // Evaluate the priority chain without hysteresis to determine what the
    // current block's inputs would select on their own.  The hysteresis
    // filter below decides whether to commit the switch.

    // --- Priority 1: Extreme inventory imbalance ---------------------------
    // When inventory imbalance exceeds the rebalance threshold, the position
    // is dangerously one-sided.  Hybrid rebalancing crosses the spread to
    // restore balance, accepting taker fees as the cost of risk reduction.
    //
    // inventory_ratio is in [0, 1] centered at 0.5.  Convert to signed
    // imbalance [-0.5, +0.5] where positive = long base, negative = short
    // base, then use the absolute value for symmetric threshold comparison.
    const double imbalance = state.inventory_ratio - 0.5;
    const double abs_imbalance = std::abs(imbalance);

    BookTactic raw_tactic = BookTactic::JoinInside;  // default

    if (abs_imbalance > cfg_.hybrid_rebalance_threshold) {
        raw_tactic = BookTactic::HybridRebalance;
    }
    // --- Priority 2: Toxic flow / whale / crowding -------------------------
    // Step back when any of these danger signals fire:
    //   a) VPIN indicates informed (toxic) order flow.
    //   b) Whale activity detected (large informed participant).
    //   c) Fill rate is too low (market is crowded with MMs).
    else if (state.vpin > cfg_.vpin_step_back_threshold) {
        raw_tactic = BookTactic::StepBack;
    }
    else if (state.whale_active) {
        raw_tactic = BookTactic::StepBack;
    }
    else if (state.fill_rate_24h < cfg_.crowding_fill_rate_threshold) {
        raw_tactic = BookTactic::StepBack;
    }
    // --- Priority 3: Inventory skew + OFI confirmation ---------------------
    // When inventory is moderately skewed AND the order flow imbalance
    // exceeds the asymmetry threshold, use asymmetric sizing to passively
    // rebalance by quoting larger on the inventory-reducing side.
    // Use abs_imbalance (deviation from 0.5) for symmetric skew detection.
    else if (abs_imbalance > 0.1 &&
             std::abs(state.normalized_ofi) > cfg_.ofi_asymmetry_threshold) {
        raw_tactic = BookTactic::AsymmetricSize;
    }
    // --- Priority 4: Deep book on both sides -------------------------------
    // When total depth exceeds twice the deep-queue threshold, the book is
    // crowded on both sides.  Layering across multiple tiers provides
    // continuous presence without concentrating risk at a single level.
    else if ((state.bid_depth + state.ask_depth) > cfg_.deep_queue_threshold * 2) {
        raw_tactic = BookTactic::LayerMultiple;
    }
    // --- Priority 5: Deep queue at best + wide spread ----------------------
    // When the queue at the best price is deep (hard to get filled by
    // joining) but our spread is substantially wider than the best
    // competitor, improving by one tick buys queue priority cheaply.
    else if (state.bid_depth > cfg_.deep_queue_threshold &&
             state.our_spread_bps > state.best_competing_bps + 10.0) {
        raw_tactic = BookTactic::ImproveByOne;
    }
    // --- Priority 6 (default): Join the inside queue -----------------------
    // The safest posture.  Queue behind existing orders at the best price.
    // Works best when toxicity is low, the queue is thin, and the market
    // is in a stable (mean-reverting or random) regime.
    // (raw_tactic already initialised to JoinInside.)

    // --- Hysteresis filter (T3-19) -----------------------------------------
    // Require N consecutive blocks confirming a new tactic before switching.
    // This prevents quote instability when inputs (e.g. inventory_ratio)
    // oscillate near a decision threshold, which reduces fill probability
    // due to frequent order cancellation and re-placement.
    //
    // A threshold of 1 disables hysteresis (immediate switch, legacy
    // behaviour).  The default is 3 blocks.
    //
    // Safety exception: HybridRebalance bypasses hysteresis entirely.
    // It represents a critical risk-management action (extreme inventory
    // imbalance) that must not be delayed.

    const bool bypass_hysteresis =
        (raw_tactic == BookTactic::HybridRebalance);

    if (bypass_hysteresis) {
        // Immediate switch -- capital preservation takes priority.
        active_tactic_         = raw_tactic;
        pending_tactic_        = raw_tactic;
        pending_tactic_blocks_ = 0;
        return active_tactic_;
    }

    if (raw_tactic == active_tactic_) {
        // The raw selection agrees with the committed tactic.  Reset the
        // pending counter so any previous candidate starts over if it
        // reappears later.
        pending_tactic_        = active_tactic_;
        pending_tactic_blocks_ = 0;
        return active_tactic_;
    }

    // The raw selection differs from the committed tactic.
    if (raw_tactic == pending_tactic_) {
        // Same candidate as last block -- increment confirmation counter.
        ++pending_tactic_blocks_;
    } else {
        // Different candidate -- restart the counter for this new candidate.
        pending_tactic_        = raw_tactic;
        pending_tactic_blocks_ = 1;
    }

    // Check whether the pending candidate has been confirmed for enough
    // consecutive blocks to warrant a switch.
    if (pending_tactic_blocks_ >= cfg_.tactic_hysteresis_blocks) {
        spdlog::debug("[OrderBookTactics] Hysteresis confirmed: "
                      "switching {} -> {} after {} consecutive blocks",
                      to_string(active_tactic_), to_string(pending_tactic_),
                      pending_tactic_blocks_);
        active_tactic_         = pending_tactic_;
        pending_tactic_blocks_ = 0;
    }

    return active_tactic_;
}

// ===========================================================================
// Individual tactic evaluators
// ===========================================================================

// ---------------------------------------------------------------------------
// Tactic 0: Join the inside queue
//
// Conditions: low toxicity, thin queue, stable regime.
// Effect: quote at the current best bid/ask with normal sizing.
// Rationale: maximises fill probability in benign conditions.
// ---------------------------------------------------------------------------

TacticRecommendation
OrderBookTactician::eval_join_inside(const BookState& state) const
{
    // Confidence is highest when the queue is thin and VPIN is low.
    // Scale linearly: confidence = 1 when VPIN = 0, dropping toward 0.5
    // as VPIN approaches the step-back threshold.
    const double vpin_factor = 1.0 - 0.5 * (state.vpin / cfg_.vpin_step_back_threshold);
    const double confidence  = std::clamp(vpin_factor, 0.0, 1.0);

    return TacticRecommendation{
        BookTactic::JoinInside,
        confidence,
        1.0,   // bid_size_factor: normal
        1.0,   // ask_size_factor: normal
        0,     // target_tier: best price (tier 0)
        0.0,   // spread_adjustment_bps: no change
        "Join inside queue -- low toxicity, thin queue"
    };
}

// ---------------------------------------------------------------------------
// Tactic 1: Improve by one increment
//
// Conditions: deep queue at best price, spread still wide relative to
//             best competitor.
// Effect: tighten spread by one tick to gain queue priority.
// Rationale: pays a small edge to jump ahead of a deep queue.
// ---------------------------------------------------------------------------

TacticRecommendation
OrderBookTactician::eval_improve(const BookState& state) const
{
    // The spread tightening is exactly one tick (improvement_tick_bps).
    // This is a negative adjustment (tighter = smaller spread).
    const double adjustment = -cfg_.improvement_tick_bps;

    // Confidence scales with how much room we have before hitting the
    // spread floor.  If we are already near the floor, confidence is low.
    const double room = state.our_spread_bps - cfg_.spread_min_bps;
    const double confidence = std::clamp(room / 50.0, 0.0, 1.0);

    return TacticRecommendation{
        BookTactic::ImproveByOne,
        confidence,
        1.0,          // bid_size_factor: normal
        1.0,          // ask_size_factor: normal
        0,            // target_tier: best price (tier 0)
        adjustment,   // tighten by one tick
        "Improve by one tick -- deep queue, spread has room"
    };
}

// ---------------------------------------------------------------------------
// Tactic 2: Step back / fade one tier
//
// Conditions: VPIN high, whale active, OR market crowded (low fill rate).
// Effect: widen spread by step_back_widening_bps; no size change.
// Rationale: preserve capital by reducing adverse-selection exposure.
// ---------------------------------------------------------------------------

TacticRecommendation
OrderBookTactician::eval_step_back(const BookState& state) const
{
    // Build a descriptive reason string based on which trigger fired.
    std::string reason;
    if (state.vpin > cfg_.vpin_step_back_threshold) {
        reason = "Step back -- VPIN=" + std::to_string(state.vpin)
               + " exceeds threshold=" + std::to_string(cfg_.vpin_step_back_threshold);
    } else if (state.whale_active) {
        reason = "Step back -- whale activity detected";
    } else {
        reason = "Step back -- market crowded, fill_rate_24h="
               + std::to_string(state.fill_rate_24h);
    }

    // Confidence is proportional to the severity of the trigger.
    // Higher VPIN or lower fill rate => higher confidence in stepping back.
    double confidence = 0.7;  // Base confidence for any step-back trigger.
    if (state.vpin > cfg_.vpin_step_back_threshold) {
        // Scale from 0.7 to 1.0 as VPIN goes from threshold to 1.0.
        const double excess = (state.vpin - cfg_.vpin_step_back_threshold)
                            / (1.0 - cfg_.vpin_step_back_threshold);
        confidence = 0.7 + 0.3 * std::clamp(excess, 0.0, 1.0);
    }
    if (state.whale_active) {
        confidence = std::max(confidence, 0.85);
    }

    return TacticRecommendation{
        BookTactic::StepBack,
        confidence,
        1.0,                          // bid_size_factor: normal
        1.0,                          // ask_size_factor: normal
        1,                            // target_tier: one level back from best
        cfg_.step_back_widening_bps,  // widen spread
        std::move(reason)
    };
}

// ---------------------------------------------------------------------------
// Tactic 3: Layer multiple tiers
//
// Conditions: deep book on both sides; uncertain directional bias.
// Effect: spread quotes across tiers 0-2 with no net spread change.
// Rationale: captures both fast flow (tight tiers) and slow flow (wide tiers)
//            without concentrating risk at a single level.
// ---------------------------------------------------------------------------

TacticRecommendation
OrderBookTactician::eval_layer(const BookState& state) const
{
    // No spread adjustment -- layering is a sizing/placement strategy,
    // not a spread strategy.  The execution layer handles the multi-tier
    // distribution.
    //
    // Confidence reflects how deep the book is (deeper = more reason to
    // layer rather than concentrate).
    const auto total_depth = state.bid_depth + state.ask_depth;
    const auto threshold_2x = cfg_.deep_queue_threshold * 2;
    const double depth_ratio = (total_depth > 0)
        ? static_cast<double>(total_depth) / static_cast<double>(threshold_2x)
        : 0.0;
    const double confidence = std::clamp(depth_ratio * 0.5, 0.3, 0.9);

    return TacticRecommendation{
        BookTactic::LayerMultiple,
        confidence,
        1.0,   // bid_size_factor: normal (per-tier sizing handled by execution)
        1.0,   // ask_size_factor: normal
        -1,    // target_tier: model decides (multi-tier)
        0.0,   // spread_adjustment_bps: no net change
        "Layer multiple tiers -- deep book, uncertain direction"
    };
}

// ---------------------------------------------------------------------------
// Tactic 4: Asymmetric sizing
//
// Conditions: inventory moderately imbalanced (ratio > 0.4) and OFI
//             confirms the directional skew.
// Effect: increase size on the inventory-reducing side by asymmetric_size_ratio;
//         reduce the other side proportionally.
// Rationale: passively rebalances inventory without crossing the spread.
// ---------------------------------------------------------------------------

TacticRecommendation
OrderBookTactician::eval_asymmetric(const BookState& state) const
{
    // Determine which side needs to be larger to reduce inventory.
    //
    // normalized_ofi > 0 means buying pressure (price rising).
    //   If we are long (inventory > 0), buying pressure means we should
    //   sell more aggressively => enlarge ask side.
    //   If we are short (inventory < 0), we want to buy => enlarge bid side.
    //
    // The inventory_ratio tells us the magnitude of imbalance, and the
    // sign of normalized_ofi tells us the flow direction.  We size up
    // on the inventory-reducing side.
    //
    // Convention: positive OFI = buy pressure = upward price tendency.
    // When OFI > 0 and we are long, we want to sell => ask_size up.
    // When OFI < 0 and we are short, we want to buy => bid_size up.

    const double ratio = cfg_.asymmetric_size_ratio;
    const double inv_ratio = 1.0 / ratio;  // Reciprocal for the other side.

    double bid_sf = 1.0;
    double ask_sf = 1.0;

    if (state.normalized_ofi > 0.0) {
        // Buying pressure -- enlarge ask (sell) to shed long inventory.
        ask_sf = ratio;
        bid_sf = inv_ratio;
    } else {
        // Selling pressure -- enlarge bid (buy) to cover short inventory.
        bid_sf = ratio;
        ask_sf = inv_ratio;
    }

    // Confidence scales with the strength of the OFI signal and
    // the severity of the inventory imbalance (deviation from 0.5).
    // Use abs(imbalance) / 0.5 to normalise to [0, 1] range.
    const double ofi_strength = std::abs(state.normalized_ofi);
    const double inv_imbalance = std::abs(state.inventory_ratio - 0.5);
    const double inv_strength = inv_imbalance / 0.5;
    const double confidence = std::clamp(
        0.5 * ofi_strength + 0.5 * inv_strength, 0.3, 0.95);

    return TacticRecommendation{
        BookTactic::AsymmetricSize,
        confidence,
        bid_sf,
        ask_sf,
        0,     // target_tier: best price
        0.0,   // spread_adjustment_bps: no change (sizing does the work)
        "Asymmetric sizing -- OFI=" + std::to_string(state.normalized_ofi)
            + " inv_ratio=" + std::to_string(state.inventory_ratio)
    };
}

// ---------------------------------------------------------------------------
// Tactic 5: Hybrid maker-taker rebalancing
//
// Conditions: extreme inventory imbalance (ratio > hybrid_rebalance_threshold)
//             or stale book requiring immediate position adjustment.
// Effect: crosses the spread (taker action) on the inventory-reducing side
//         while maintaining passive quotes on the accumulating side.
//         The spread is widened to compensate for taker costs.
// Rationale: when passive rebalancing is too slow, pay the spread to restore
//            a safe inventory level before adverse selection compounds.
// ---------------------------------------------------------------------------

TacticRecommendation
OrderBookTactician::eval_hybrid_rebalance(const BookState& state) const
{
    // The rebalancing side takes liquidity (crosses the spread), so we
    // set its size factor high and the other side low.
    //
    // normalized_ofi sign indicates flow direction; we rebalance against
    // it.  But the primary driver here is the extreme inventory_ratio,
    // not OFI.  We always want to reduce the dominant inventory side.

    double bid_sf = 1.0;
    double ask_sf = 1.0;

    // Inventory ratio > threshold means |q| / q_max is dangerously high.
    // The OFI sign tells us which side the flow is on.  We reduce
    // against the flow:
    //   - If OFI > 0 (buy pressure), we are likely long => sell more.
    //   - If OFI < 0 (sell pressure), we are likely short => buy more.
    //
    // Use aggressive sizing: double the reducing side, halve the other.
    constexpr double kAggressiveRatio = 2.0;
    constexpr double kPassiveRatio    = 0.5;

    if (state.normalized_ofi >= 0.0) {
        // Reduce long position: increase ask size.
        ask_sf = kAggressiveRatio;
        bid_sf = kPassiveRatio;
    } else {
        // Reduce short position: increase bid size.
        bid_sf = kAggressiveRatio;
        ask_sf = kPassiveRatio;
    }

    // Widen the spread to offset taker costs incurred by crossing.
    // The widening is proportional to the inventory imbalance severity.
    // Use abs(imbalance) so that both long and short extremes are measured
    // symmetrically against the threshold.
    const double imb = std::abs(state.inventory_ratio - 0.5);
    const double severity = std::clamp(
        (imb - cfg_.hybrid_rebalance_threshold)
        / (0.5 - cfg_.hybrid_rebalance_threshold),
        0.0, 1.0);
    const double widen_bps = cfg_.step_back_widening_bps * (1.0 + severity);

    // Confidence is very high when we are at the rebalance threshold --
    // this is a risk-management imperative, not a discretionary choice.
    const double confidence = std::clamp(
        0.8 + 0.2 * severity, 0.8, 1.0);

    return TacticRecommendation{
        BookTactic::HybridRebalance,
        confidence,
        bid_sf,
        ask_sf,
        -1,        // target_tier: model decides (may cross spread)
        widen_bps, // widen to offset taker costs
        "Hybrid rebalance -- inv_ratio=" + std::to_string(state.inventory_ratio)
            + " exceeds threshold=" + std::to_string(cfg_.hybrid_rebalance_threshold)
    };
}

}  // namespace xop
