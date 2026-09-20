// fee_tracker.hpp -- Fee budget tracking, dynamic fee selection, and
//                    fee-vs-gain gating for XOPTrader.
//
// Implements two user-specified requirements:
//   1. Don't participate (skip posting) when the blockchain fee is too high
//      relative to the expected gain from that offer tier.
//   2. Track observed fee levels and recommend the smallest fee likely to
//      achieve timely on-chain inclusion.
//
// The tracker maintains a rolling window of fees paid (configurable, default
// ~24 h = 1662 blocks) and enforces a daily budget ceiling.  When adaptive
// mode is enabled, it incorporates the full node's get_fee_estimate RPC
// result to dynamically lower or raise the fee within [min, max].
//
// Thread safety:
//   FeeTracker is designed for single-threaded use on the engine's
//   io_context strand.  No internal locking.
//
// ISO/IEC 27001:2022 -- fee expenditure is audit-logged.
// ISO/IEC 5055       -- no raw pointers, bounded containers, RAII.
// ISO/IEC 25000      -- clear naming, documented invariants.

#ifndef XOP_STRATEGY_FEE_TRACKER_HPP
#define XOP_STRATEGY_FEE_TRACKER_HPP

#include "xop/config.hpp"
#include "xop/strategy/fee_controller.hpp"
#include "xop/types.hpp"

#include <cstdint>
#include <deque>
#include <utility>

namespace xop {

// ---------------------------------------------------------------------------
// FeeTracker
// ---------------------------------------------------------------------------

/// [S67] The controller's tuning, copied out of the `fees:` section.  The
/// controller is enabled only when fees.enabled is too: fees.enabled: false
/// is the documented passthrough and it wins.
[[nodiscard]] strategy::fee::ControllerConfig fee_controller_config_from(const FeeConfig& cfg);

class FeeTracker {
public:
    /// Construct from the fee configuration section.
    explicit FeeTracker(const FeeConfig& cfg);

    // -- Fee recording ------------------------------------------------------

    /// Record a fee payment (after an offer is posted or cancelled).
    /// @param fee_mojos    Fee actually paid (mojos).
    /// @param block_height Block at which the fee was incurred.
    void record_fee(std::uint64_t fee_mojos, BlockHeight block_height);

    // -- Budget queries -----------------------------------------------------

    /// Sum of all fees paid within the rolling window ending at
    /// @p current_block.  Expired entries are pruned.
    /// @return Cumulative fee mojos in the window.
    std::uint64_t get_rolling_total(BlockHeight current_block);

    /// True if spending @p additional_fee_mojos would keep the rolling
    /// total within the daily budget.
    bool is_within_budget(BlockHeight current_block,
                          std::uint64_t additional_fee_mojos = 0);

    // -- Fee-vs-gain gating -------------------------------------------------

    /// Determine whether posting an offer is worthwhile given the expected
    /// gain and the fee that would be paid.
    ///
    /// Returns false (skip the offer) when:
    ///   * fee / expected_gain > fee_to_gain_max_ratio, OR
    ///   * the daily budget would be exceeded.
    ///
    /// @param expected_gain_mojos  Estimated profit if the offer fills.
    /// @param fee_mojos            Blockchain fee for this offer (mojos).
    /// @param current_block        Current block height (for budget check).
    /// @return True if the offer should be posted.
    bool should_post_offer(std::uint64_t expected_gain_mojos,
                           std::uint64_t fee_mojos,
                           BlockHeight   current_block);

    // -- Dynamic fee selection ----------------------------------------------

    /// Return the recommended fee for the next offer, clamped to
    /// [min_fee, max_fee] and respecting the remaining budget headroom.
    ///
    /// When adaptive mode is enabled and a mempool estimate is available,
    /// the estimate is used.  Otherwise the static offer_fee_mojos from
    /// StrategyConfig is returned (clamped to bounds).
    ///
    /// @param static_fee_mojos  The statically configured offer_fee_mojos.
    /// @param current_block     Current block height (for budget check).
    /// @param action            [S67] What the fee pays for.  There is NO
    ///                          default on purpose: a call site that names no
    ///                          class does not compile.  With
    ///                          fees.cost_aware_estimate and the controller
    ///                          both off the class is not read at all.
    /// @return Recommended fee in mojos.  0 means "skip posting" on the
    ///         legacy path only; with the controller on it is never 0 (see
    ///         strategy::fee::apply_budget).
    std::uint64_t get_recommended_fee(std::uint64_t static_fee_mojos,
                                      BlockHeight   current_block,
                                      strategy::fee::ActionClass action);

    // -- Mempool estimate ingestion -----------------------------------------

    /// Feed the latest mempool fee estimate from the full node RPC
    /// (get_fee_estimate).  The tracker uses this to lower fees when the
    /// mempool is uncongested.
    ///
    /// @param estimated_fee_mojos  Fee estimate for ~60 s target time.
    void update_mempool_estimate(std::uint64_t estimated_fee_mojos);

    // -- [S67] Cost-aware estimate and the fee controller -------------------

    /// True when fees.cost_aware_estimate or the controller wants a RATE
    /// from the node (get_fee_rate_estimate) rather than the legacy estimate.
    [[nodiscard]] bool wants_rate_estimate() const noexcept
    {
        return cfg_.enabled && cfg_.adaptive_enabled
            && (cfg_.cost_aware_estimate || controller_.enabled());
    }

    /// True when the closed-loop controller sets the fees.
    [[nodiscard]] bool controller_active() const noexcept { return controller_.enabled(); }

    /// One node reading: the estimate as mojos per cost, and the node's own
    /// admission floor (0 when its mempool has room or it did not say).
    /// Feeds the cost-aware legacy path and the controller's floor.
    strategy::fee::Change update_feed_forward(double      estimate_rate,
                                              double      admission_floor_rate,
                                              BlockHeight current_block);

    /// Feed one observation to the controller.  A no-op returning
    /// ChangeReason::Disabled when the controller is off.
    strategy::fee::Change observe(const strategy::fee::Observation& observation);

    /// The ticket for a spend just submitted at `fee_paid_mojos`.
    [[nodiscard]] strategy::fee::Ticket make_ticket(strategy::fee::ActionClass action,
                                                    std::uint64_t fee_paid_mojos,
                                                    BlockHeight   current_block) const noexcept;

    /// True ONCE per episode in which the budget lowered a fee: the engine
    /// turns it into one operator alert.  The episode ends when an
    /// offer-attached fee next comes back unbound.
    [[nodiscard]] bool take_budget_bound_alert() noexcept;

    /// The fee the budget last refused to pay in full, and what it allowed.
    [[nodiscard]] std::uint64_t last_bound_desired() const noexcept { return last_bound_desired_; }
    [[nodiscard]] std::uint64_t last_bound_allowed() const noexcept { return last_bound_allowed_; }

    [[nodiscard]] const strategy::fee::Controller& controller() const noexcept { return controller_; }

    // -- Accessors ----------------------------------------------------------

    /// True if fee tracking is enabled.
    [[nodiscard]] bool enabled() const noexcept { return cfg_.enabled; }

    /// Remaining budget headroom at @p current_block (mojos).
    std::uint64_t budget_remaining(BlockHeight current_block);

private:
    /// Prune entries older than the rolling window.
    void prune(BlockHeight current_block);

    FeeConfig cfg_;

    /// Rolling fee history: (block_height, fee_mojos).
    /// Oldest entries are at the front; pruned when expired.
    std::deque<std::pair<BlockHeight, std::uint64_t>> fee_history_;

    /// Cached rolling total (updated on prune).
    std::uint64_t cached_total_{0};

    /// Block height at which the cache was last pruned.
    BlockHeight cached_prune_block_{0};

    /// Latest mempool fee estimate (0 = not available).
    std::uint64_t mempool_estimate_{0};

    /// [S67] Latest node estimate as a RATE, mojos per cost (0 = not
    /// available), for fees.cost_aware_estimate.
    double mempool_rate_{0.0};

    /// [S67] The closed loop.  Inert unless fees.controller_enabled.
    strategy::fee::Controller controller_;

    bool          budget_bound_{false};
    bool          budget_alert_pending_{false};
    std::uint64_t last_bound_desired_{0};
    std::uint64_t last_bound_allowed_{0};

    /// The controller path of get_recommended_fee.
    std::uint64_t controller_fee(strategy::fee::ActionClass action, BlockHeight current_block);
};

}  // namespace xop

#endif  // XOP_STRATEGY_FEE_TRACKER_HPP
