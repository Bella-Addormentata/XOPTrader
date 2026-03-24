// loss_manager.hpp -- Strategic loss analysis for XOPTrader CHIA DEX market maker.
//
// Implements the mathematical framework for evaluating whether taking a
// deliberate loss on a position is rational, given the alternative costs of
// holding.  This module answers Issue #9: "Is it ever a good strategy to
// take a loss on a position in order to achieve a different goal?"
//
// The five scenarios analysed:
//   1. Inventory rebalancing losses  -- opportunity cost of skewed quotes.
//   2. Adverse selection defence     -- cut-small-loss-or-hold-bigger-loss.
//   3. Opportunity cost of locked capital -- spread income foregone.
//   4. Tax-loss harvesting           -- realised loss offsets realised gain.
//   5. Never-loss optimality         -- when holding is genuinely best.
//
// Design principles:
//   - Every public method is const or pure-functional; the class holds only
//     configuration, never mutable position data.
//   - Monetary values stay in Mojo (int64_t) as long as possible.  Doubles
//     are used only for intermediate rate/probability calculations and are
//     never stored back as monetary values.
//   - Thread-safe: the class is immutable after construction.  All runtime
//     state is read from InventoryTracker and PreTradeCheck via const refs
//     passed to each call.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no side-effects, audit-friendly pure functions
//   ISO/IEC 5055       -- no raw pointer ownership, bounds checks on all inputs
//   ISO/IEC 25000      -- documented interfaces, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard C++20, no undefined behaviour

#ifndef XOP_RISK_LOSS_MANAGER_HPP
#define XOP_RISK_LOSS_MANAGER_HPP

#include "xop/types.hpp"
#include "xop/config.hpp"
#include "xop/risk/inventory.hpp"
#include "xop/risk/limits.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace xop {

// ---------------------------------------------------------------------------
// LossDecision -- the verdict returned by should_rebalance_at_loss().
//
// Fields expose every intermediate quantity so that the strategy layer (and
// the operator via Grafana) can inspect the reasoning, not just the boolean.
// ---------------------------------------------------------------------------

struct LossDecision {
    bool   should_take_loss;        // true  = rebalance now (accept the loss).
                                    // false = hold (the loss is not justified).

    // -- Rebalancing loss analysis (scenario 1) --
    double loss_bps;                // Realised loss if we rebalance now (bps).
    double spread_income_per_block; // Expected spread income per block if we
                                    // rebalance to target_ratio (bps of capital).
    double blocks_to_breakeven;     // Number of blocks of spread income needed
                                    // to recover the realised loss.
    double max_recovery_blocks;     // Configured ceiling -- if blocks_to_breakeven
                                    // exceeds this, the loss is rejected.

    // -- Adverse selection analysis (scenario 2) --
    double vpin;                    // Current VPIN reading for this asset.
    double expected_further_loss_bps; // E[additional loss] if we hold and VPIN
                                      // is elevated (model: sigma * sqrt(tau) * VPIN).
    double ev_hold;                 // Expected value of holding (bps).
    double ev_cut;                  // Expected value of cutting now (bps).

    // -- Opportunity cost analysis (scenario 3) --
    double locked_capital_frac;     // Fraction of total capital locked in this
                                    // underwater position.
    double foregone_spread_per_block; // Spread income this capital would earn if
                                      // redeployed (bps of locked capital).
    double carrying_cost_per_block; // Total carrying cost = foregone spread +
                                    // funding (bps of locked capital).

    // -- Combined expected value --
    double ev_rebalance;            // EV of taking the loss and redeploying.
    double ev_no_action;            // EV of holding the current position.

    // -- Diagnostics --
    std::string rationale;          // Human-readable explanation for logging.
};

// ---------------------------------------------------------------------------
// LossManagerConfig -- tuneable parameters for the strategic loss evaluator.
//
// All thresholds have conservative defaults that strongly favour holding
// (i.e. never-loss behaviour) unless the operator explicitly loosens them.
// ---------------------------------------------------------------------------

struct LossManagerConfig {
    // -- Scenario 1: inventory rebalancing --
    double max_acceptable_loss_bps{0.0};
        // Maximum realised loss (in basis points of position value) that the
        // manager will ever approve.  Default 0 = never approve a loss
        // (equivalent to the strict never-sell-at-loss policy).

    double min_spread_recovery_blocks{100.0};
        // Maximum number of blocks over which the manager is willing to wait
        // for spread income to recoup the loss.  If the break-even takes longer
        // than this, the loss is rejected.  100 blocks ~ 87 minutes at 52s.
        // CHIA-specific: at $2K/day volume, fills are sparse, so this must be
        // generous.

    double target_inventory_ratio{0.50};
        // The ideal inventory ratio (0.5 = perfectly balanced between base and
        // quote).  Rebalancing aims to restore this ratio.

    // -- Scenario 2: adverse selection --
    double vpin_danger_threshold{0.70};
        // VPIN level above which flow is considered toxic.  At CHIA's volume
        // levels, VPIN is noisy, so we use a high threshold.

    double adverse_move_sigma_multiplier{1.5};
        // When computing the expected further loss under adverse selection, we
        // model the adverse move as: sigma * sqrt(tau) * this_multiplier.
        // 1.5 sigma covers ~93% of one-tailed moves.

    double mean_reversion_probability{0.60};
        // Prior probability that the current price deviation will revert to
        // the cost basis within the holding horizon.  Higher values favour
        // holding.  In a mean-reverting market (VR < 0.85) this should be
        // closer to 0.80; in a trending market (VR > 1.15) closer to 0.30.

    // -- Scenario 3: opportunity cost --
    double risk_free_rate_per_block{0.0};
        // Opportunity cost of capital per block, expressed as a fraction.
        // For CHIA DEX at $2K/day this is effectively 0.  Set to a small
        // positive value if there are alternative deployment opportunities
        // (e.g. TibetSwap LP, staking).

    // -- Scenario 4: tax-loss harvesting --
    double marginal_tax_rate{0.0};
        // Marginal tax rate on short-term capital gains.  Set to 0.0 to
        // disable tax-loss harvesting logic entirely.  When > 0, a realised
        // loss generates a tax shield = loss * marginal_tax_rate.

    double unrealised_gains_bps{0.0};
        // Unrealised gains elsewhere in the portfolio that could be offset
        // by a harvested loss (bps of total capital).  The tax benefit is
        // min(loss, unrealised_gains) * marginal_tax_rate.

    // -- Global override --
    bool enabled{false};
        // Master switch.  When false, should_rebalance_at_loss() always
        // returns {should_take_loss = false} without performing any
        // computation.  Default off: the bot starts with strict never-loss
        // behaviour and strategic loss-taking must be explicitly opted into.
};

// ---------------------------------------------------------------------------
// StrategicLossManager -- pure-functional evaluator for loss-taking decisions.
//
// Usage (per-block heartbeat, after strategy computes quotes):
//
//     if (inventory.is_underwater(asset_id, current_price)) {
//         LossDecision d = loss_mgr.should_rebalance_at_loss(
//             asset_id, current_price, target_ratio,
//             inventory, price_map, market_params);
//         if (d.should_take_loss) {
//             // Override the no-loss constraint for this specific rebalance.
//             execute_rebalance(asset_id, target_ratio);
//         }
//     }
//
// Integration with existing risk stack:
//   - Reads cost basis and inventory ratio from InventoryTracker (const ref).
//   - Does NOT bypass PreTradeCheck; the caller must explicitly lower the
//     no-loss constraint on InventoryTracker before executing a loss trade.
//   - The decision is advisory: the strategy layer makes the final call.
// ---------------------------------------------------------------------------

/// Market parameters needed for EV calculations.  Passed per-call so that
/// the loss manager never holds stale data.
struct MarketParams {
    double sigma;              // Per-block volatility (fraction, not bps).
    double fill_rate_per_block; // Probability of a fill occurring per block
                                // for this pair.  At $2K/day volume on CHIA,
                                // this is very low (~0.01-0.05).
    double spread_bps;         // Current half-spread being quoted (bps).
    double vpin;               // Current VPIN reading for this pair [0,1].
    double variance_ratio;     // Current variance ratio (VR < 0.85 = mean-
                                // reverting, VR > 1.15 = trending).
    BlockHeight current_block; // Current blockchain height.
};

class StrategicLossManager {
public:
    // -- Construction -------------------------------------------------------

    /// Construct with a validated configuration.  The config is copied and
    /// stored immutably.
    /// ISO/IEC 5055: not noexcept -- body calls spdlog which may throw.
    explicit StrategicLossManager(const LossManagerConfig& cfg);

    // -- Primary interface --------------------------------------------------

    /// Evaluate whether taking a rebalancing loss on `asset_id` is rational
    /// given the current market conditions and inventory state.
    ///
    /// The method is const and side-effect-free.  It computes the expected
    /// value of two alternatives:
    ///   (A) Take the loss now, rebalance to target_ratio, and earn spread.
    ///   (B) Hold the underwater position, forgo spread income on the
    ///       depleted side, and wait for mean reversion.
    ///
    /// A loss is approved only when ALL of the following hold:
    ///   1. The module is enabled (cfg_.enabled == true).
    ///   2. The loss does not exceed max_acceptable_loss_bps.
    ///   3. The break-even recovery period does not exceed min_spread_recovery_blocks.
    ///   4. EV(rebalance) > EV(hold).
    ///
    /// @param asset_id       The underwater asset to consider selling at a loss.
    /// @param current_price  Current market price of the asset (mojos).
    /// @param target_ratio   Desired inventory ratio after rebalancing [0,1].
    /// @param inventory      Read-only reference to the inventory tracker.
    /// @param price_map      Current prices for all tracked assets.
    /// @param params         Current market microstructure parameters.
    /// @return               A fully populated LossDecision.
    [[nodiscard]]
    LossDecision should_rebalance_at_loss(
        const AssetId&                           asset_id,
        Mojo                                     current_price,
        double                                   target_ratio,
        const InventoryTracker&                  inventory,
        const std::unordered_map<AssetId, Mojo>& price_map,
        const MarketParams&                      params) const;

    // -- Individual scenario evaluators (exposed for unit testing) ----------

    /// Scenario 1: Inventory rebalancing break-even.
    ///
    /// Derives the number of blocks of spread income required to recover a
    /// given rebalancing loss.
    ///
    /// Break-even formula:
    ///
    ///   blocks_to_breakeven = loss_bps
    ///                         / (spread_bps * fill_rate * delta_ratio * 2)
    ///
    /// where:
    ///   loss_bps    = (cost_basis - current_price) / cost_basis * 10000
    ///   spread_bps  = current half-spread (bps)
    ///   fill_rate   = probability of a fill per block
    ///   delta_ratio = |current_ratio - target_ratio| (the imbalance corrected)
    ///   factor of 2 = restoring balance lets us quote BOTH sides, doubling
    ///                  the spread capture vs. one-sided quoting.
    ///
    /// Returns the break-even block count (positive), or +infinity if the
    /// denominator is zero (no spread income possible).
    [[nodiscard]]
    double compute_rebalancing_breakeven(
        double loss_bps,
        double spread_bps,
        double fill_rate_per_block,
        double current_ratio,
        double target_ratio) const noexcept;

    /// Scenario 2: Adverse selection expected value.
    ///
    /// Models the EV of holding vs. cutting when VPIN is elevated.
    ///
    ///   EV(hold) = P(revert) * 0  +  P(adverse) * (-sigma * sqrt(tau) * k)
    ///            = -(1 - P_revert) * sigma * sqrt(tau) * k
    ///
    ///   EV(cut)  = -loss_bps   (the certain cost of cutting now)
    ///
    /// where:
    ///   P(revert)   = mean_reversion_probability (config, regime-dependent)
    ///   sigma       = per-block volatility
    ///   tau         = holding horizon in blocks (= min_spread_recovery_blocks)
    ///   k           = adverse_move_sigma_multiplier (config)
    ///
    /// Returns EV(hold) and EV(cut) in bps.
    [[nodiscard]]
    double compute_adverse_selection_ev_hold(
        double sigma,
        double tau_blocks,
        double mean_reversion_prob) const noexcept;

    [[nodiscard]]
    double compute_adverse_selection_ev_cut(
        double loss_bps) const noexcept;

    /// Scenario 3: Opportunity cost (carrying cost) per block.
    ///
    ///   carrying_cost = foregone_spread + risk_free_rate
    ///
    /// where foregone_spread = spread_bps * fill_rate * (1 - current_ratio)
    ///   for a position that prevents balanced quoting.
    ///
    /// Returns the carrying cost in bps of locked capital per block.
    [[nodiscard]]
    double compute_carrying_cost_per_block(
        double spread_bps,
        double fill_rate_per_block,
        double current_ratio,
        double target_ratio) const noexcept;

    /// Scenario 4: Tax-loss harvesting benefit.
    ///
    ///   tax_benefit = min(loss_bps, unrealised_gains_bps) * marginal_tax_rate
    ///
    /// Returns the benefit in bps of capital (positive = benefits from loss).
    [[nodiscard]]
    double compute_tax_benefit(double loss_bps) const noexcept;

    /// Scenario 5: Never-loss optimality check.
    ///
    /// Returns true when the market conditions strongly favour holding:
    ///   - Variance ratio indicates mean-reversion (VR < 0.85).
    ///   - VPIN is low (< vpin_danger_threshold), i.e. no toxic flow.
    ///   - The position is not excessively large (locked_capital_frac < 0.30).
    ///
    /// When this returns true, the never-loss policy is genuinely optimal and
    /// the loss manager should not override it regardless of other signals.
    [[nodiscard]]
    bool is_never_loss_optimal(
        double variance_ratio,
        double vpin,
        double locked_capital_frac) const noexcept;

    // -- Accessors ----------------------------------------------------------

    /// Read a copy of the current configuration.
    const LossManagerConfig& config() const noexcept;

private:
    LossManagerConfig cfg_;
};

}  // namespace xop

#endif  // XOP_RISK_LOSS_MANAGER_HPP
