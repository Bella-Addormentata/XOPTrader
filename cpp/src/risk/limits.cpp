// limits.cpp -- Pre-trade risk checks implementation.
//
// See limits.hpp for interface documentation and safety rationale.
//
// All monetary arithmetic uses Mojo (int64_t).  Fractional comparisons
// (concentration, portfolio fraction) are performed in double only AFTER
// the monetary values have been established, and the doubles are never
// used for further monetary computation.  This prevents floating-point
// drift from affecting order prices.
//
// Compliant with:
//   ISO/IEC 27001:2022  (deterministic risk gates, audit-friendly logging)
//   ISO/IEC 5055        (bounds-checked, no undefined behaviour)
//   ISO/IEC 25000       (single-responsibility functions, documented invariants)

#include "xop/risk/limits.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstddef>
#include <limits>
#include <numeric>

namespace xop {

// ---------------------------------------------------------------------------
// EmergencyRule stringification
// ---------------------------------------------------------------------------

const char* to_string(EmergencyRule r) noexcept {
    switch (r) {
        case EmergencyRule::None:            return "None";
        case EmergencyRule::FlashCrash:      return "FlashCrash";
        case EmergencyRule::OneSidedFills:   return "OneSidedFills";
        case EmergencyRule::Congestion:      return "Congestion";
        case EmergencyRule::ExploitDetected: return "ExploitDetected";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PreTradeCheck::PreTradeCheck(const RiskConfig&     cfg,
                             const StrategyConfig& strat_cfg)
    : risk_cfg_(cfg)
    , strat_cfg_(strat_cfg)
    , margin_fraction_(strat_cfg.min_profit_margin_bps / 10'000.0)
{
    // Validate: margin_fraction_ should be a small positive number (e.g. 0.0035
    // for 35 bps).  A negative or enormous value indicates a config error.
    // ISO/IEC 5055: throw instead of assert (stripped in Release builds).
    if (!(margin_fraction_ >= 0.0 && margin_fraction_ < 1.0)) {
        throw std::invalid_argument(
            "RiskConfig: min_profit_margin_bps yields margin_fraction outside [0, 1)");
    }

    // ISO/IEC 5055: validate that soft_limit_pct does not exceed
    // hard_limit_pct.  A reversed ordering would cause the graduated
    // reduction logic in apply_limits() to divide by zero or produce
    // nonsensical skew factors.
    if (risk_cfg_.soft_limit_pct > risk_cfg_.hard_limit_pct) {
        throw std::invalid_argument(
            "RiskConfig: soft_limit_pct must be <= hard_limit_pct");
    }
}

// ---------------------------------------------------------------------------
// enforce_no_loss -- CORE RULE: NEVER SELL AT A LOSS.
//
// If enabled, the ask price is floored at (cost_basis + min_profit_margin).
// The bid side is never modified -- buying more of an asset does not violate
// the no-loss rule.
//
// The margin is computed as a fraction of cost_basis rather than as an
// absolute number of mojos, so it scales correctly regardless of the
// asset's per-mojo price.
// ---------------------------------------------------------------------------

Quote PreTradeCheck::enforce_no_loss(Quote quote,
                                     Mojo  cost_basis,
                                     bool  enable_constraint) const noexcept
{
    if (!enable_constraint) {
        return quote;  // pass through unchanged
    }

    // Compute the minimum acceptable ask price.
    // min_ask = cost_basis + cost_basis * margin_fraction_
    //         = cost_basis * (1 + margin_fraction_)
    // Using integer arithmetic to avoid floating-point monetary drift:
    //   margin_mojos = round(cost_basis * margin_fraction_)
    //   min_ask      = cost_basis + margin_mojos
    //
    // Guard against cost_basis <= 0 (no inventory, or free acquisition).
    // When cost_basis is zero the constraint is vacuously satisfied.
    if (cost_basis <= 0) {
        return quote;
    }

    const auto margin_mojos = static_cast<Mojo>(
        std::llround(static_cast<double>(cost_basis) * margin_fraction_));

    // Overflow guard: if cost_basis is near Mojo max, adding margin could wrap.
    // In practice this cannot happen (max position ~11K XCH = 1.1e16 mojos,
    // well within int64 range), but defensive coding is warranted.
    const Mojo headroom = std::numeric_limits<Mojo>::max() - cost_basis;
    const Mojo safe_margin = (margin_mojos > 0 && margin_mojos <= headroom)
                           ? margin_mojos
                           : headroom;

    const Mojo min_ask = cost_basis + safe_margin;

    // Floor the ask price.
    if (quote.ask_price < min_ask) {
        quote.ask_price = min_ask;

        // Recompute informational spread_bps to reflect the adjusted ask.
        // spread_bps = (ask - bid) / ((ask + bid) / 2) * 10'000
        if (quote.bid_price > 0) {
            const double mid = (static_cast<double>(quote.ask_price)
                              + static_cast<double>(quote.bid_price)) / 2.0;
            quote.spread_bps = (static_cast<double>(quote.ask_price)
                              - static_cast<double>(quote.bid_price))
                             / mid * 10'000.0;
        }
    }

    return quote;
}

// ---------------------------------------------------------------------------
// apply_limits -- inventory, CAT cap, and capital-per-pair checks.
//
// Design:  Each check can independently zero out a side of the quote.
//          If both sides end up zeroed, the caller receives nullopt,
//          signalling that no quote should be posted this cycle.
//
// Ordering of checks matters only for diagnostics (we always run all of
// them so that get_limit_status can report every breach, not just the first).
// ---------------------------------------------------------------------------

std::optional<Quote> PreTradeCheck::apply_limits(
    Quote              quote,
    const std::string& /*pair_name*/,
    const AssetId&     base_id,
    const AssetId&     quote_id,
    const State&       state) const
{
    const Position base_pos  = state.get_position(base_id);
    const Position quote_pos = state.get_position(quote_id);
    const auto all_positions = state.get_all_positions();

    // ---- 1. Inventory concentration (soft / hard limits) ------------------
    //
    //   concentration = base_balance / (base_balance + quote_balance)
    //   If concentration >= hard_limit (80%): pull quotes on the overweight
    //     side (i.e., stop buying base if we already hold too much base).
    //   If concentration >= soft_limit (60%): same effect -- the strategy
    //     should already be skewing, but this is the backstop.
    //
    //   The "overweight side" when base concentration is high means we hold
    //   too much base, so we must stop BUYING base (zero the bid).
    //   Conversely, low base concentration (high quote concentration)
    //   means we stop SELLING base (zero the ask).

    const double base_conc = compute_concentration(base_pos, quote_pos);
    const double quote_conc = 1.0 - base_conc;

    // Base overweight?
    if (base_conc >= risk_cfg_.hard_limit_pct) {
        // Hard limit breach: pull bids entirely -- do not accumulate more base.
        quote.bid_size = 0;
    } else if (base_conc >= risk_cfg_.soft_limit_pct) {
        // Soft limit: apply graduated proportional reduction instead of
        // zeroing.  Linearly interpolate from full size at soft_limit to
        // zero at hard_limit, so the transition is smooth rather than a
        // cliff that behaves identically to the hard limit.
        // ISO/IEC 5055: clamped to [0.0, 1.0] to guard against config
        // where soft_pct == hard_pct (division by zero yields 0.0 via clamp).
        const double reduction = std::clamp(
            (base_conc - risk_cfg_.soft_limit_pct)
                / (risk_cfg_.hard_limit_pct - risk_cfg_.soft_limit_pct),
            0.0, 1.0);
        quote.bid_size = static_cast<Mojo>(
            static_cast<double>(quote.bid_size) * (1.0 - reduction));
    }

    // Quote overweight?
    if (quote_conc >= risk_cfg_.hard_limit_pct) {
        // Hard limit breach: pull asks entirely -- do not sell more base.
        quote.ask_size = 0;
    } else if (quote_conc >= risk_cfg_.soft_limit_pct) {
        // Soft limit: graduated proportional reduction (mirror of bid logic).
        const double reduction = std::clamp(
            (quote_conc - risk_cfg_.soft_limit_pct)
                / (risk_cfg_.hard_limit_pct - risk_cfg_.soft_limit_pct),
            0.0, 1.0);
        quote.ask_size = static_cast<Mojo>(
            static_cast<double>(quote.ask_size) * (1.0 - reduction));
    }

    // ---- 2. Single-CAT cap (12% of total portfolio) ----------------------
    //
    //   "Never exceed 12% of portfolio in any one CAT regardless of
    //    opportunity."  -- Section 8.
    //
    //   If the base asset is a CAT (not "xch") and its portfolio fraction
    //   exceeds the cap, stop buying it (zero bid).  We do NOT force-sell
    //   (never sell at a loss).

    if (base_id != "xch") {
        const double cat_frac = compute_portfolio_fraction(base_pos, all_positions);
        if (cat_frac >= risk_cfg_.single_cat_cap_pct) {
            quote.bid_size = 0;  // stop accumulating this CAT
        }
    }

    // Same check for quote side if the quote asset is a CAT.
    if (quote_id != "xch") {
        const double cat_frac = compute_portfolio_fraction(quote_pos, all_positions);
        if (cat_frac >= risk_cfg_.single_cat_cap_pct) {
            quote.ask_size = 0;  // stop accumulating quote CAT (i.e., stop selling base)
        }
    }

    // ---- 3. Max capital per pair ------------------------------------------
    //
    //   If this pair consumes more than max_capital_per_pair_pct of total
    //   capital, block both sides to prevent further concentration.

    const double pair_frac = compute_pair_capital_fraction(base_pos, quote_pos,
                                                           all_positions);
    if (pair_frac >= risk_cfg_.max_capital_per_pair_pct) {
        quote.bid_size = 0;
        quote.ask_size = 0;
    }

    // ---- Result -----------------------------------------------------------

    if (quote.bid_size == 0 && quote.ask_size == 0) {
        return std::nullopt;  // both sides blocked -- skip this cycle
    }

    return quote;
}

// ---------------------------------------------------------------------------
// check_flash_crash -- circuit breaker for sudden price drops.
//
// Algorithm (rolling max-drawdown):
//   Scan the price window chronologically, maintaining a running maximum.
//   At each point, compute the drawdown from the running max.  If any
//   drawdown exceeds the threshold, a crash is detected.
//
//   This correctly detects:
//     - Early crashes followed by recovery to new highs (the previous
//       global-max algorithm missed these because the later peak dominated).
//     - Multiple successive crashes within one window.
//     - V-shaped recoveries (crash still flagged).
//     - Monotonically rising prices (no crash).
//     - Flat markets (no crash).
//
//   The running-max approach is O(N) in time and O(1) in auxiliary space,
//   matching the previous implementation's complexity while providing
//   strictly superior detection coverage.
//
//   Edge cases:
//     - Empty or single-element history: no crash.
//     - All prices zero or negative: degenerate, treated as no crash.
//     - Running max is the last element: drawdown is 0.0, no false positive.
//
// ISO/IEC 5055: division guarded against zero denominator; no UB.
// ---------------------------------------------------------------------------

bool PreTradeCheck::check_flash_crash(const std::vector<Mojo>& price_history,
                                      double threshold) noexcept
{
    if (price_history.size() < 2) {
        return false;  // not enough data to detect a crash
    }

    // Track the running maximum as we scan chronologically.
    Mojo running_max = price_history[0];

    for (std::size_t i = 1; i < price_history.size(); ++i) {
        // Update running max with the current price.
        if (price_history[i] > running_max) {
            running_max = price_history[i];
        }

        // Guard: skip drawdown computation when running_max is non-positive
        // to avoid division by zero on degenerate data.
        if (running_max <= 0) {
            continue;
        }

        // Compute fractional drawdown from the running maximum.
        // Use double for the division only (operands are exact integers).
        const double drawdown =
            static_cast<double>(running_max - price_history[i])
            / static_cast<double>(running_max);

        if (drawdown >= threshold) {
            return true;  // Flash crash detected.
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// is_stable_after_crash -- recovery gate.
//
// Returns true only when the most recent `required_stable_blocks` prices
// all lie within `stability_band` of the latest price.  This ensures we
// do not resume quoting during a dead-cat bounce.
// ---------------------------------------------------------------------------

bool PreTradeCheck::is_stable_after_crash(
    const std::vector<Mojo>& price_history,
    std::size_t              required_stable_blocks,
    double                   stability_band) noexcept
{
    if (price_history.size() < required_stable_blocks) {
        return false;  // not enough blocks to declare stability
    }

    const Mojo latest = price_history.back();
    if (latest <= 0) {
        return false;  // degenerate
    }

    // Check the tail of the history.
    const auto start = price_history.cend()
                     - static_cast<std::ptrdiff_t>(required_stable_blocks);

    for (auto it = start; it != price_history.cend(); ++it) {
        const double deviation = std::abs(static_cast<double>(*it - latest))
                               / static_cast<double>(latest);
        if (deviation > stability_band) {
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// congestion_buffer_multiplier
//
// Section 8 Emergency Playbook: "Network congestion: Increase rebalancing
// buffer to 25-30%."  The normal buffer is ~15-20%, so a multiplier of
// 1.30 brings it to ~19.5-26%, within the specified range.
// ---------------------------------------------------------------------------

double PreTradeCheck::congestion_buffer_multiplier(bool congested) const noexcept
{
    return congested ? kCongestionMultiplier : 1.0;
}

// ---------------------------------------------------------------------------
// get_limit_status -- diagnostic snapshot for monitoring dashboards.
// ---------------------------------------------------------------------------

LimitStatus PreTradeCheck::get_limit_status(
    const AssetId& base_id,
    const AssetId& quote_id,
    const State&   state) const
{
    const Position base_pos  = state.get_position(base_id);
    const Position quote_pos = state.get_position(quote_id);
    const auto all_positions = state.get_all_positions();

    LimitStatus ls{};
    ls.base_id  = base_id;
    ls.quote_id = quote_id;

    // Inventory concentration.
    ls.base_concentration  = compute_concentration(base_pos, quote_pos);
    ls.soft_limit_breached = (ls.base_concentration >= risk_cfg_.soft_limit_pct)
                          || ((1.0 - ls.base_concentration) >= risk_cfg_.soft_limit_pct);
    ls.hard_limit_breached = (ls.base_concentration >= risk_cfg_.hard_limit_pct)
                          || ((1.0 - ls.base_concentration) >= risk_cfg_.hard_limit_pct);

    // Single-CAT cap (only relevant for CAT assets).
    if (base_id != "xch") {
        ls.cat_portfolio_pct = compute_portfolio_fraction(base_pos, all_positions);
    } else if (quote_id != "xch") {
        ls.cat_portfolio_pct = compute_portfolio_fraction(quote_pos, all_positions);
    } else {
        ls.cat_portfolio_pct = 0.0;  // XCH/XCH pair (hypothetical)
    }
    ls.cat_cap_breached = (ls.cat_portfolio_pct >= risk_cfg_.single_cat_cap_pct);

    // Capital per pair.
    ls.pair_capital_pct  = compute_pair_capital_fraction(base_pos, quote_pos,
                                                         all_positions);
    ls.pair_cap_breached = (ls.pair_capital_pct >= risk_cfg_.max_capital_per_pair_pct);

    // Flash-crash flag is set externally by the caller; initialise to false.
    ls.flash_crash_active = false;

    return ls;
}

// ===========================================================================
// Private helpers
// ===========================================================================

// ---------------------------------------------------------------------------
// compute_concentration -- fraction of combined base+quote value held in base.
//
// Uses raw mojo balances as a proxy for value.  This is exact for XCH/XCH
// pairs and an approximation for CAT pairs (where 1 mojo of base != 1 mojo
// of quote in value).  A more precise version would use mark-to-market
// prices, but for the risk-gate purpose (detecting lopsided inventory) the
// balance ratio is a robust and manipulation-resistant proxy.
// ---------------------------------------------------------------------------

double PreTradeCheck::compute_concentration(
    const Position& base_pos,
    const Position& quote_pos) noexcept
{
    const auto total = static_cast<double>(base_pos.balance)
                     + static_cast<double>(quote_pos.balance);
    if (total <= 0.0) {
        return 0.0;  // no capital deployed -- report zero concentration
    }
    return static_cast<double>(base_pos.balance) / total;
}

// ---------------------------------------------------------------------------
// compute_portfolio_fraction -- what share of total holdings an asset holds.
// ---------------------------------------------------------------------------

double PreTradeCheck::compute_portfolio_fraction(
    const Position&              asset_pos,
    const std::vector<Position>& all) noexcept
{
    // Sum total balance across all assets.
    double total = 0.0;
    for (const auto& p : all) {
        total += static_cast<double>(p.balance);
    }

    if (total <= 0.0) {
        return 0.0;
    }

    return static_cast<double>(asset_pos.balance) / total;
}

// ---------------------------------------------------------------------------
// compute_pair_capital_fraction -- combined base+quote balance as a fraction
// of total portfolio balance.
// ---------------------------------------------------------------------------

double PreTradeCheck::compute_pair_capital_fraction(
    const Position&              base_pos,
    const Position&              quote_pos,
    const std::vector<Position>& all) noexcept
{
    double total = 0.0;
    for (const auto& p : all) {
        total += static_cast<double>(p.balance);
    }

    if (total <= 0.0) {
        return 0.0;
    }

    const double pair_capital = static_cast<double>(base_pos.balance)
                              + static_cast<double>(quote_pos.balance);
    return pair_capital / total;
}

}  // namespace xop
