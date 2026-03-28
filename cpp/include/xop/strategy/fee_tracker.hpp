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
#include "xop/types.hpp"

#include <cstdint>
#include <deque>
#include <utility>

namespace xop {

// ---------------------------------------------------------------------------
// FeeTracker
// ---------------------------------------------------------------------------

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
    /// @return Recommended fee in mojos.
    std::uint64_t get_recommended_fee(std::uint64_t static_fee_mojos,
                                      BlockHeight   current_block);

    // -- Mempool estimate ingestion -----------------------------------------

    /// Feed the latest mempool fee estimate from the full node RPC
    /// (get_fee_estimate).  The tracker uses this to lower fees when the
    /// mempool is uncongested.
    ///
    /// @param estimated_fee_mojos  Fee estimate for ~60 s target time.
    void update_mempool_estimate(std::uint64_t estimated_fee_mojos);

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
};

}  // namespace xop

#endif  // XOP_STRATEGY_FEE_TRACKER_HPP
