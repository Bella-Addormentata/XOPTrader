// fee_tracker.cpp -- Fee budget tracking, dynamic fee selection, and
//                    fee-vs-gain gating implementation.
//
// See fee_tracker.hpp for design rationale and usage.
//
// ISO/IEC 27001:2022 -- fee expenditure is audit-logged via spdlog.
// ISO/IEC 5055       -- bounded containers, overflow-safe arithmetic.
// ISO/IEC 25000      -- documented invariants, single-responsibility.

#include "xop/strategy/fee_tracker.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>

namespace xop {

// ===========================================================================
// Construction
// ===========================================================================

FeeTracker::FeeTracker(const FeeConfig& cfg)
    : cfg_(cfg)
{
    spdlog::info("[FeeTracker] Initialised: enabled={} budget={} mojos/day "
                 "gain_ratio={:.2f} min={} max={} adaptive={} window={} blocks",
                 cfg_.enabled,
                 cfg_.daily_budget_mojos,
                 cfg_.fee_to_gain_max_ratio,
                 cfg_.min_fee_mojos,
                 cfg_.max_fee_mojos,
                 cfg_.adaptive_enabled,
                 cfg_.fee_window_blocks);
}

// ===========================================================================
// Fee recording
// ===========================================================================

void FeeTracker::record_fee(std::uint64_t fee_mojos, BlockHeight block_height)
{
    fee_history_.emplace_back(block_height, fee_mojos);
    cached_total_ += fee_mojos;

    spdlog::debug("[FeeTracker] Recorded fee {} mojos at block {} "
                  "(rolling total now {} mojos)",
                  fee_mojos, block_height, cached_total_);
}

// ===========================================================================
// Budget queries
// ===========================================================================

void FeeTracker::prune(BlockHeight current_block)
{
    if (current_block == cached_prune_block_ && !fee_history_.empty()) {
        return;  // Already pruned for this block.
    }

    // Remove entries older than the rolling window.
    const BlockHeight cutoff =
        (current_block > cfg_.fee_window_blocks)
            ? (current_block - cfg_.fee_window_blocks)
            : 0;

    while (!fee_history_.empty() && fee_history_.front().first < cutoff) {
        cached_total_ -= fee_history_.front().second;
        fee_history_.pop_front();
    }

    cached_prune_block_ = current_block;
}

std::uint64_t FeeTracker::get_rolling_total(BlockHeight current_block)
{
    prune(current_block);
    return cached_total_;
}

bool FeeTracker::is_within_budget(BlockHeight current_block,
                                  std::uint64_t additional_fee_mojos)
{
    const std::uint64_t current = get_rolling_total(current_block);

    // Overflow-safe addition check.
    if (additional_fee_mojos >
        std::numeric_limits<std::uint64_t>::max() - current) {
        return false;
    }

    return (current + additional_fee_mojos) <= cfg_.daily_budget_mojos;
}

std::uint64_t FeeTracker::budget_remaining(BlockHeight current_block)
{
    const std::uint64_t current = get_rolling_total(current_block);
    if (current >= cfg_.daily_budget_mojos) {
        return 0;
    }
    return cfg_.daily_budget_mojos - current;
}

// ===========================================================================
// Fee-vs-gain gating
// ===========================================================================

bool FeeTracker::should_post_offer(std::uint64_t expected_gain_mojos,
                                   std::uint64_t fee_mojos,
                                   BlockHeight   current_block)
{
    if (!cfg_.enabled) {
        return true;  // Gating disabled -- always post.
    }

    // Budget check: would this fee exceed the daily ceiling?
    if (!is_within_budget(current_block, fee_mojos)) {
        spdlog::warn("[FeeTracker] Daily fee budget exhausted "
                     "(rolling={} + pending={} > budget={}). Skipping offer.",
                     get_rolling_total(current_block), fee_mojos,
                     cfg_.daily_budget_mojos);
        return false;
    }

    // Fee-vs-gain ratio check (skip if ratio == 0.0 meaning disabled).
    // Apply cancel_cost_multiplier to account for the round-trip cost:
    // every offer that doesn't fill will also incur a cancellation fee.
    if (cfg_.fee_to_gain_max_ratio > 0.0 && expected_gain_mojos > 0) {
        const double round_trip_fee =
            static_cast<double>(fee_mojos) * cfg_.cancel_cost_multiplier;
        const double ratio = round_trip_fee
                           / static_cast<double>(expected_gain_mojos);
        if (ratio > cfg_.fee_to_gain_max_ratio) {
            spdlog::info("[FeeTracker] Round-trip fee/gain ratio {:.2f} exceeds "
                         "threshold {:.2f} (fee={} x{:.1f} gain={}). "
                         "Skipping offer.",
                         ratio, cfg_.fee_to_gain_max_ratio,
                         fee_mojos, cfg_.cancel_cost_multiplier,
                         expected_gain_mojos);
            return false;
        }
    }

    // Edge case: expected_gain is zero or negative -- always skip.
    // A zero-gain trade with a non-zero fee is pure cost.
    if (expected_gain_mojos == 0 && fee_mojos > 0) {
        spdlog::info("[FeeTracker] Expected gain is 0 with fee {} mojos. "
                     "Skipping offer.", fee_mojos);
        return false;
    }

    return true;
}

// ===========================================================================
// Dynamic fee selection
// ===========================================================================

std::uint64_t FeeTracker::get_recommended_fee(std::uint64_t static_fee_mojos,
                                              BlockHeight   current_block)
{
    if (!cfg_.enabled) {
        return static_fee_mojos;  // Passthrough when disabled.
    }

    std::uint64_t fee = static_fee_mojos;

    // When adaptive mode is on and we have a mempool estimate, prefer it.
    if (cfg_.adaptive_enabled && mempool_estimate_ > 0) {
        fee = mempool_estimate_;
        spdlog::debug("[FeeTracker] Using mempool estimate {} mojos "
                      "(static was {})", fee, static_fee_mojos);
    }

    // Clamp to configured [min, max] band.
    fee = std::clamp(fee, cfg_.min_fee_mojos, cfg_.max_fee_mojos);

    // Further cap by remaining budget headroom.
    const std::uint64_t headroom = budget_remaining(current_block);
    if (fee > headroom) {
        spdlog::warn("[FeeTracker] Fee {} mojos capped to budget headroom "
                     "{} mojos", fee, headroom);
        fee = headroom;

        // If headroom is below the minimum, return 0 to signal "cannot post".
        if (fee < cfg_.min_fee_mojos) {
            spdlog::warn("[FeeTracker] Budget headroom {} mojos below "
                         "min_fee {} mojos -- recommending 0 (skip posting)",
                         fee, cfg_.min_fee_mojos);
            return 0;
        }
    }

    return fee;
}

// ===========================================================================
// Mempool estimate ingestion
// ===========================================================================

void FeeTracker::update_mempool_estimate(std::uint64_t estimated_fee_mojos)
{
    mempool_estimate_ = estimated_fee_mojos;
    spdlog::debug("[FeeTracker] Mempool fee estimate updated to {} mojos",
                  estimated_fee_mojos);
}

}  // namespace xop
