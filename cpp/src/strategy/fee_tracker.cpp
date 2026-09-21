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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>

namespace xop {

// ===========================================================================
// Construction
// ===========================================================================

strategy::fee::ControllerConfig fee_controller_config_from(const FeeConfig& cfg)
{
    strategy::fee::ControllerConfig out;
    out.enabled                   = cfg.enabled && cfg.controller_enabled;
    out.target_delay_blocks       = cfg.controller_target_delay_blocks;
    out.kp                        = cfg.controller_kp;
    out.ki                        = cfg.controller_ki;
    out.kd                        = cfg.controller_kd;
    out.max_error                 = cfg.controller_max_error;
    out.max_step_up               = cfg.controller_max_step_up;
    out.min_raise                 = cfg.controller_min_raise;
    out.warmup_observations       = cfg.controller_warmup_observations;
    out.probe_fraction            = cfg.controller_probe_fraction;
    out.probe_after_confirmations = cfg.controller_probe_after_confirmations;
    out.probe_confirmations       = cfg.controller_probe_confirmations;
    out.probe_fail_bump           = cfg.controller_probe_fail_bump;
    out.probe_backoff_cap         = cfg.controller_probe_backoff_cap;
    out.ff_margin                 = cfg.controller_ff_margin;
    out.ff_max_age_blocks         = cfg.controller_ff_max_age_blocks;
    out.costs.offer_attached      = cfg.controller_cost_offer_attached;
    out.costs.cancel_xch          = cfg.controller_cost_cancel_xch;
    out.costs.cancel_cat          = cfg.controller_cost_cancel_cat;
    out.costs.take                = cfg.controller_cost_take;
    return out;
}

FeeTracker::FeeTracker(const FeeConfig& cfg)
    : cfg_(cfg)
    , controller_(fee_controller_config_from(cfg), cfg.min_fee_mojos, cfg.max_fee_mojos)
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

    // [S67] The reachability advisory: what the bounds and gains can do.
    // Advice, never a refusal -- each finding resolves deterministically.
    if (controller_.enabled()) {
        const strategy::fee::Reachability reach = strategy::fee::reachability(
            controller_.config(), cfg_.min_fee_mojos, cfg_.max_fee_mojos);
        spdlog::info("[FeeController] ON: target {} peak heights, anchor {:.6f} mojos/cost, "
                     "level band [{:.2f}, {:.2f}] log2, {} hard signals span it",
                     controller_.config().target_delay_blocks, controller_.anchor_rate(),
                     controller_.level_lo(), controller_.level_hi(), reach.raises_to_span);
        // [review #163] The anchor is min_fee_mojos exactly, so a tiny floor
        // makes a very wide band: every raise is at most one doubling and the
        // next cannot come for a whole target delay.  Say what that costs.
        if (reach.raises_to_span > 16) {
            spdlog::warn("[FeeController] fees.min_fee_mojos ({}) is so far below "
                         "max_fee_mojos ({}) that the loop needs {} raises to cross the "
                         "band -- about {} minutes with no node floor to help. Raise "
                         "min_fee_mojos toward what a cancel normally pays.",
                         cfg_.min_fee_mojos, cfg_.max_fee_mojos, reach.raises_to_span,
                         static_cast<std::uint64_t>(reach.raises_to_span)
                             * (controller_.config().target_delay_blocks + 4U) * 1875U / 6000U);
        }
        if (reach.cannot_raise) {
            spdlog::warn("[FeeController] the gains cannot raise the fee across its band "
                         "(controller_ki is 0, or every gain is) -- only the node floor "
                         "and the probes act");
        }
        // [review #163 r3] FINDING 1: size the budget for the window it is
        // actually spent over, at the prices the controller converges on.  One
        // derivation serves this advisory and the range config.example.yaml
        // recommends, so the documented number cannot drift from the code.
        const std::uint64_t window_cost = strategy::fee::full_mempool_window_cost(
            controller_.config().costs, controller_.config().ff_margin, cfg_.fee_window_blocks);
        const std::uint64_t want_budget = strategy::fee::recommended_window_budget(
            controller_.config().costs, controller_.config().ff_margin, cfg_.fee_window_blocks);
        const std::uint64_t full_cancel = strategy::fee::fee_from_rate(
            strategy::fee::kFullMempoolMinRate * controller_.config().ff_margin,
            controller_.config().costs.cancel_cat);
        const std::uint64_t n_reserve = cfg_.controller_budget_reserve_cancels;
        const std::uint64_t uncapped_reserve =
            (n_reserve != 0 && full_cancel > std::numeric_limits<std::uint64_t>::max() / n_reserve)
                ? std::numeric_limits<std::uint64_t>::max()
                : full_cancel * n_reserve;
        spdlog::info("[FeeController] budget sizing: one window ({} peak heights) costs about {} "
                     "mojos at full-mempool prices; the cancel reserve is {} x {} = {} mojos, "
                     "capped at half the budget ({})",
                     cfg_.fee_window_blocks, window_cost, n_reserve, full_cancel, uncapped_reserve,
                     strategy::fee::budget_reserve(full_cancel, n_reserve,
                                                   cfg_.daily_budget_mojos));
        if (cfg_.daily_budget_mojos < want_budget) {
            spdlog::warn("[FeeController] fees.daily_budget_mojos ({}) is below the {} mojos this "
                         "configuration wants per window of {} peak heights (one window costs "
                         "about {} at full-mempool prices, and the budget should not bind in "
                         "ordinary operation). It WILL bind: offer-attached fees pin at "
                         "fees.min_fee_mojos and one FeeBudgetBound alert fires. Cancels and "
                         "takes are never degraded -- they are paid in full and reported "
                         "FeeBudgetUnfunded.",
                         cfg_.daily_budget_mojos, want_budget, cfg_.fee_window_blocks, window_cost);
        }
        const strategy::fee::ActionClass all[strategy::fee::kActionClassCount] = {
            strategy::fee::ActionClass::OfferAttached, strategy::fee::ActionClass::CancelXch,
            strategy::fee::ActionClass::CancelCat, strategy::fee::ActionClass::Take};
        for (std::size_t i = 0; i < strategy::fee::kActionClassCount; ++i) {
            if (reach.max_fee_below_full_mempool[i]) {
                spdlog::warn("[FeeController] fees.max_fee_mojos ({}) cannot get a {} into a "
                             "FULL mempool: the node needs {} mojos/cost and this class costs "
                             "{} -- raise max_fee_mojos to at least {} to cover every class",
                             cfg_.max_fee_mojos, strategy::fee::to_string(all[i]),
                             strategy::fee::kFullMempoolMinRate,
                             strategy::fee::cost_of(controller_.config().costs, all[i]),
                             reach.max_fee_for_full_mempool);
            }
        }
    }
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
    // [S67] Not with the controller on.  There the budget has ALREADY shaped
    // the fee (controller_fee: the reserve, then the min_fee floor), and an
    // exhausted budget degrades fees instead of stopping the bot.  Refusing
    // every tier here would be exactly the silent stop that rule exists to
    // prevent.  The fee-vs-gain check below still applies.
    if (!controller_.enabled() && !is_within_budget(current_block, fee_mojos)) {
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
                                              BlockHeight   current_block,
                                              strategy::fee::ActionClass action)
{
    if (!cfg_.enabled) {
        return static_fee_mojos;  // Passthrough when disabled.
    }

    // [S67] The closed loop replaces everything below when it is on.
    if (controller_.enabled()) {
        return controller_fee(action, current_block);
    }

    std::uint64_t fee = static_fee_mojos;

    // [S67] fees.cost_aware_estimate: the node's RATE times this action's own
    // CLVM cost, instead of its answer for a plain XCH send.  Same precedence
    // as the legacy estimate below: used only when adaptive and available.
    if (cfg_.adaptive_enabled && cfg_.cost_aware_estimate) {
        const std::uint64_t scaled = strategy::fee::fee_from_rate(
            mempool_rate_, strategy::fee::cost_of(controller_.config().costs, action));
        if (scaled > 0) {
            fee = scaled;
            spdlog::debug("[FeeTracker] Using cost-aware estimate {} mojos for {} "
                          "({:.4f} mojos/cost; static was {})", fee,
                          strategy::fee::to_string(action), mempool_rate_, static_fee_mojos);
        }
    } else if (cfg_.adaptive_enabled && mempool_estimate_ > 0) {
        // When adaptive mode is on and we have a mempool estimate, prefer it.
        fee = mempool_estimate_;
        spdlog::debug("[FeeTracker] Using mempool estimate {} mojos "
                      "(static was {})", fee, static_fee_mojos);
    }

    // Clamp to configured [min, max] band.
    const std::uint64_t pre_clamp = fee;
    fee = std::clamp(fee, cfg_.min_fee_mojos, cfg_.max_fee_mojos);

    // Warn when min_fee floor significantly exceeds the mempool estimate,
    // indicating the floor is configured too conservatively.
    if (pre_clamp < cfg_.min_fee_mojos &&
        cfg_.min_fee_mojos > pre_clamp * 10) {
        spdlog::warn("[FeeTracker] min_fee_mojos ({}) is {}x higher than "
                     "mempool estimate ({} mojos) -- consider lowering "
                     "min_fee_mojos to reduce costs",
                     cfg_.min_fee_mojos,
                     (pre_clamp > 0 ? cfg_.min_fee_mojos / pre_clamp : 0),
                     pre_clamp);
    }

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

// ===========================================================================
// [S67] Cost-aware estimate and the fee controller
// ===========================================================================

strategy::fee::Change FeeTracker::update_feed_forward(double      estimate_rate,
                                                      double      admission_floor_rate,
                                                      BlockHeight current_block)
{
    // The legacy path keeps its own rule: a reading of 0 does not replace the
    // last non-zero one (Step 1 has always skipped est == 0).
    if (std::isfinite(estimate_rate) && estimate_rate > 0.0) {
        mempool_rate_ = estimate_rate;
    }
    return controller_.set_feed_forward(estimate_rate, admission_floor_rate, current_block);
}

strategy::fee::Change FeeTracker::observe(const strategy::fee::Observation& observation)
{
    return controller_.observe(observation);
}

strategy::fee::Ticket FeeTracker::make_ticket(strategy::fee::ActionClass action,
                                              std::uint64_t fee_paid_mojos,
                                              BlockHeight   current_block) const noexcept
{
    strategy::fee::Ticket ticket;
    ticket.cls          = action;
    ticket.submit_block = current_block;
    ticket.submit_level = controller_.level_of(fee_paid_mojos, action);
    return ticket;
}

bool FeeTracker::take_budget_bound_alert() noexcept
{
    const bool due = budget_alert_pending_;
    budget_alert_pending_ = false;
    return due;
}

bool FeeTracker::take_budget_unfunded_alert() noexcept
{
    const bool due = unfunded_alert_pending_;
    unfunded_alert_pending_ = false;
    return due;
}

std::uint64_t FeeTracker::controller_fee(strategy::fee::ActionClass action,
                                         BlockHeight current_block)
{
    const std::uint64_t desired  = controller_.fee_for(action, current_block);
    const std::uint64_t headroom = budget_remaining(current_block);

    // The reserve: enough budget to cancel the resting book at today's price,
    // and [review #163 r3] NEVER more than half the budget.  The uncapped
    // figure exceeds the whole budget at any plausible setting once the loop
    // has converged on a full mempool -- 25 x 232,650,000 = 5,816,250,000 --
    // which pinned every attached fee at min_fee from the first convergent
    // heartbeat (strategy::fee::budget_reserve).
    const std::uint64_t cancel_fee =
        controller_.fee_for(strategy::fee::ActionClass::CancelCat, current_block);
    const std::uint64_t reserve = strategy::fee::budget_reserve(
        cancel_fee, cfg_.controller_budget_reserve_cancels, cfg_.daily_budget_mojos);

    const strategy::fee::BudgetedFee budgeted = strategy::fee::apply_budget(
        desired, headroom, reserve, cfg_.min_fee_mojos, strategy::fee::is_priority(action),
        attached_batch_);

    // [review #163 r3] A priority spend the budget cannot fund is PAID, and
    // said out loud.  Degrading it to min_fee is what produced cancels the
    // node would not admit; the budget's authority over a cancel ends at the
    // report.
    //
    // [review #163 r5] But THIS IS A QUOTE, NOT A SPEND.  Step 8 asks for both
    // cancel classes every heartbeat before any cancellation, and a take fee
    // is computed while a candidate is still being evaluated -- so latching
    // here recorded an episode in which a priority spend was PAID over budget,
    // queued FeeBudgetUnfunded and logged "PAYING IT ANYWAY" on heartbeats
    // where no wallet RPC was sent at all.  The overrun is stashed as inert
    // data; note_priority_spend promotes it when the wallet accepts the spend.
    PendingUnfunded& pending = pending_unfunded_[static_cast<std::size_t>(action)];
    pending.over     = budgeted.over_budget;
    pending.fee      = budgeted.fee;
    pending.headroom = headroom;

    if (budgeted.bound) {
        last_bound_desired_ = desired;
        last_bound_allowed_ = budgeted.fee;
        if (!budget_bound_) {
            budget_bound_         = true;
            budget_alert_pending_ = true;
            spdlog::warn("[FeeController] the fee budget BINDS: {} wants {} mojos, the budget "
                         "allows {} (headroom {} of {} per {} peak heights, reserve {} for {} "
                         "cancels). Offer-attached fees degrade toward min_fee_mojos; cancels "
                         "and takes are NOT degraded and nothing stops. Raise "
                         "fees.daily_budget_mojos.",
                         strategy::fee::to_string(action), desired, budgeted.fee, headroom,
                         cfg_.daily_budget_mojos, cfg_.fee_window_blocks, reserve,
                         cfg_.controller_budget_reserve_cancels);
        }
    } else if (!strategy::fee::is_priority(action) && budget_bound_) {
        // An offer-attached fee is the first thing the budget squeezes, so
        // one that comes back whole means the episode is over.
        budget_bound_ = false;
        spdlog::info("[FeeController] the fee budget no longer binds (headroom {} mojos)",
                     headroom);
    }
    return budgeted.fee;
}

void FeeTracker::note_priority_spend(strategy::fee::ActionClass action,
                                     std::uint64_t              fee_paid_mojos)
{
    // [review #163 r5] The reporting half of controller_fee.  An attached fee
    // is never reported over budget (strategy::fee::apply_budget degrades it
    // instead), and with the controller off nothing here has an opinion.
    if (!controller_.enabled() || !strategy::fee::is_priority(action)) {
        return;
    }
    const PendingUnfunded& quote = pending_unfunded_[static_cast<std::size_t>(action)];
    // The spend counts against the quote it was priced from.  A spend that
    // came in UNDER that quote -- an escalation tier below it, a policy fee --
    // did not overrun the headroom the quote measured, so it is not evidence.
    if (quote.over && fee_paid_mojos >= quote.fee) {
        last_unfunded_fee_      = fee_paid_mojos;
        last_unfunded_headroom_ = quote.headroom;
        last_unfunded_action_   = action;
        if (!budget_unfunded_) {
            budget_unfunded_        = true;
            unfunded_alert_pending_ = true;
            spdlog::warn("[FeeController] the fee budget CANNOT FUND a {}: it PAID {} mojos "
                         "(fees.max_fee_mojos and the node's own floor already bound that) and "
                         "the window had {} left of {}. PAID ANYWAY -- a cancel priced "
                         "below what the node will admit never confirms, keeps its coins "
                         "locked and ends in a wallet-wide force-delete. Raise "
                         "fees.daily_budget_mojos.",
                         strategy::fee::to_string(action), fee_paid_mojos, quote.headroom,
                         cfg_.daily_budget_mojos);
        }
        return;
    }
    if (budget_unfunded_) {
        budget_unfunded_ = false;
        spdlog::info("[FeeController] the fee budget funds priority spends again (a {} of {} "
                     "mojos was accepted within a headroom of {})",
                     strategy::fee::to_string(action), fee_paid_mojos, quote.headroom);
    }
}

}  // namespace xop
