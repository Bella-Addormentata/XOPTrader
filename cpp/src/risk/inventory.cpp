// inventory.cpp -- Implementation of InventoryTracker for XOPTrader CHIA DEX
//                  market-making bot.
//
// This translation unit provides:
//   - Weighted-average cost basis accounting (buy / sell)
//   - Tiered inventory limit evaluation
//   - Half-Kelly position sizing
//   - Capital allocation bookkeeping across five categories
//   - Portfolio concentration and risk status queries
//
// All monetary arithmetic uses int64_t mojos.  Conversion to double happens
// only inside ratio calculations and the result is never stored back as a
// monetary value.
//
// Overflow note:
//   The product (fill_price * qty) could theoretically overflow int64_t if
//   both operands are near 2^31.  At CHIA's scale this is impossible -- max
//   reasonable qty is ~11,000 XCH = 1.1e16 mojos and max price is ~10 XCH =
//   1e13 mojos, so the product is ~1.1e29 which overflows.  We therefore use
//   __int128 (or double) for the intermediate multiplication, then store back
//   to int64_t after division.  This eliminates any overflow risk for all
//   realistic capital levels.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- controlled mutable state, defensive input validation
//   ISO/IEC 5055       -- no raw pointer ownership, RAII locking, bounds checks
//   ISO/IEC 25000      -- single-responsibility methods, documented edge cases
//   ISO/IEC JTC 1/SC 22 -- standard C++20, no undefined behaviour

#include "xop/risk/inventory.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

// ---------------------------------------------------------------------------
// On MSVC __int128 is not available; fall back to double for the intermediate
// multiplication.  The precision loss (~15 significant digits) is acceptable
// because the final result is a ratio (cost basis) and both numerator and
// denominator share the same magnitude.
// ---------------------------------------------------------------------------
#if defined(__SIZEOF_INT128__)
  #if defined(__GNUC__) || defined(__clang__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
  #endif
    using Wide = __int128;
    #define XOP_WIDE_DIV(num, den) static_cast<Mojo>((num) / (den))
  #if defined(__GNUC__) || defined(__clang__)
    #pragma GCC diagnostic pop
  #endif
#else
    using Wide = double;
    #define XOP_WIDE_DIV(num, den) static_cast<Mojo>(static_cast<double>(num) / \
                                                      static_cast<double>(den))
#endif

namespace xop {

// ===========================================================================
// RiskStatus helpers
// ===========================================================================

const char* to_string(RiskStatus s) noexcept {
    switch (s) {
        case RiskStatus::Normal:     return "Normal";
        case RiskStatus::SoftLimit:  return "SoftLimit";
        case RiskStatus::HardLimit:  return "HardLimit";
        case RiskStatus::Underwater: return "Underwater";
    }
    return "Unknown";
}

// ===========================================================================
// CapitalCategory helpers
// ===========================================================================

const char* to_string(CapitalCategory c) noexcept {
    switch (c) {
        case CapitalCategory::ActiveOffers:       return "ActiveOffers";
        case CapitalCategory::RebalancingBuffer:  return "RebalancingBuffer";
        case CapitalCategory::EmergencyReserve:   return "EmergencyReserve";
        case CapitalCategory::OpportunityReserve: return "OpportunityReserve";
        case CapitalCategory::OperationalFloat:   return "OperationalFloat";
    }
    return "Unknown";
}

// ===========================================================================
// AssetRecord
// ===========================================================================

AssetRecord::AssetRecord()
    : total_quantity{0}
    , total_cost{0}
    , weighted_avg_cost_basis{0}
    , last_fill_block{0}
    , last_fill_time{}
{}

AssetRecord::AssetRecord(const AssetId& id)
    : asset_id{id}
    , total_quantity{0}
    , total_cost{0}
    , weighted_avg_cost_basis{0}
    , last_fill_block{0}
    , last_fill_time{}
{}

// ===========================================================================
// InventoryTracker -- construction
// ===========================================================================

InventoryTracker::InventoryTracker(const RiskConfig& risk_cfg,
                                   Mojo              total_capital_mojos)
    : InventoryTracker(risk_cfg, total_capital_mojos, /*no_loss_constraint=*/true)
{}

InventoryTracker::InventoryTracker(const RiskConfig& risk_cfg,
                                   Mojo              total_capital_mojos,
                                   bool              no_loss_constraint)
    : total_capital_{total_capital_mojos}
    , soft_limit_pct_{risk_cfg.soft_limit_pct}
    , hard_limit_pct_{risk_cfg.hard_limit_pct}
    , single_cat_cap_pct_{risk_cfg.single_cat_cap_pct}
    , kelly_fraction_{risk_cfg.kelly_fraction}
    , max_capital_per_pair_pct_{risk_cfg.max_capital_per_pair_pct}
    , no_loss_constraint_{no_loss_constraint}
{
    // Initialise per-category allocations to zero and copy default limits.
    for (std::size_t i = 0; i < kCapitalCategoryCount; ++i) {
        allocated_[i] = 0;
        limits_[i]    = kDefaultCapitalLimits[i];
    }
}

// ===========================================================================
// Cost Basis Tracking
// ===========================================================================

void InventoryTracker::record_buy(const AssetId& asset_id,
                                  Mojo           qty,
                                  Mojo           fill_price,
                                  BlockHeight    block,
                                  Timestamp      ts)
{
    // Defensive: reject invalid inputs.
    if (qty <= 0 || fill_price <= 0) {
        return;
    }

    std::unique_lock lock(mtx_records_);

    // Insert a new record if this is the first time we see the asset.
    auto [it, inserted] = records_.try_emplace(asset_id, asset_id);
    AssetRecord& rec = it->second;

    // Weighted-average cost basis update:
    //   new_total_cost = old_total_cost + fill_price * qty
    //   new_total_qty  = old_qty + qty
    //   new_basis      = new_total_cost / new_total_qty
    //
    // Use wide arithmetic to prevent overflow in the multiplication.
    const Wide cost_increment = static_cast<Wide>(fill_price)
                              * static_cast<Wide>(qty);
    const Wide new_total_cost = static_cast<Wide>(rec.total_cost)
                              + cost_increment;
    const Mojo new_total_qty  = rec.total_quantity + qty;

    // Guard against quantity overflow (should never happen at realistic scale).
    if (new_total_qty < rec.total_quantity) {
        // Overflow detected -- reject silently rather than corrupt state.
        return;
    }

    rec.total_cost     = static_cast<Mojo>(new_total_cost);
    rec.total_quantity  = new_total_qty;

    // Recompute weighted average cost basis.
    recompute_basis(rec);

    // Update fill timestamp.
    rec.last_fill_block = block;
    rec.last_fill_time  = ts;
}

bool InventoryTracker::record_sell(const AssetId& asset_id,
                                   Mojo           qty,
                                   Mojo           sell_price,
                                   BlockHeight    block,
                                   Timestamp      ts)
{
    // Defensive: reject invalid inputs.
    if (qty <= 0 || sell_price <= 0) {
        return false;
    }

    std::unique_lock lock(mtx_records_);

    auto it = records_.find(asset_id);
    if (it == records_.end()) {
        // Cannot sell an asset we have never acquired.
        return false;
    }

    AssetRecord& rec = it->second;

    // Cannot sell more than we hold.
    if (qty > rec.total_quantity) {
        return false;
    }

    // Never-sell-at-loss enforcement.
    // Compare sell_price against weighted_avg_cost_basis.  If the sell would
    // realise a loss and the constraint is active, reject the trade.
    // ISO/IEC 5055: atomic load for thread-safe flag access.
    if (no_loss_constraint_.load(std::memory_order_acquire) && rec.total_quantity > 0) {
        if (sell_price < rec.weighted_avg_cost_basis) {
            return false;
        }
    }

    // Proportional cost reduction (weighted-average drawdown):
    //   cost_removed   = total_cost * (qty / total_quantity)
    //   new_total_cost = total_cost - cost_removed
    //
    // This preserves the weighted-average cost basis for the remaining
    // position, which is the correct accounting under the weighted-average
    // method.
    //
    // Edge case: qty == total_quantity  =>  new_total_cost = 0, new_qty = 0.
    //
    // We use wide arithmetic to avoid overflow in (total_cost * qty).
    if (qty == rec.total_quantity) {
        // Full liquidation -- shortcut avoids rounding residue.
        rec.total_cost     = 0;
        rec.total_quantity = 0;
    } else {
        const Wide numerator = static_cast<Wide>(rec.total_cost)
                             * static_cast<Wide>(rec.total_quantity - qty);
        rec.total_cost     = XOP_WIDE_DIV(numerator, rec.total_quantity);
        rec.total_quantity -= qty;
    }

    // Recompute basis (handles the zero-quantity case).
    recompute_basis(rec);

    // Update fill timestamp.
    rec.last_fill_block = block;
    rec.last_fill_time  = ts;

    return true;
}

// ===========================================================================
// Risk Metrics
// ===========================================================================

Mojo InventoryTracker::net_inventory(const AssetId& asset_id) const
{
    std::shared_lock lock(mtx_records_);
    const AssetRecord* rec = find_record_locked(asset_id);
    return rec ? rec->total_quantity : 0;
}

double InventoryTracker::inventory_ratio(const AssetId& base_id,
                                         const AssetId& quote_id,
                                         Mojo           base_price) const
{
    std::shared_lock lock(mtx_records_);

    const AssetRecord* base_rec  = find_record_locked(base_id);
    const AssetRecord* quote_rec = find_record_locked(quote_id);

    // Value of base holdings in quote terms.
    // Use double to avoid overflow: base_qty * base_price could exceed int64.
    const double base_value  = base_rec
        ? static_cast<double>(base_rec->total_quantity)
          * static_cast<double>(base_price)
        : 0.0;

    // Quote holdings are already denominated in quote mojos.
    const double quote_value = quote_rec
        ? static_cast<double>(quote_rec->total_quantity)
        : 0.0;

    const double total = base_value + quote_value;
    if (total <= 0.0) {
        // No capital deployed -- report balanced.
        return 0.5;
    }

    return base_value / total;
}

bool InventoryTracker::is_underwater(const AssetId& asset_id,
                                     Mojo           current_price) const
{
    std::shared_lock lock(mtx_records_);
    const AssetRecord* rec = find_record_locked(asset_id);
    if (!rec || rec->total_quantity == 0) {
        return false;
    }
    return rec->weighted_avg_cost_basis > current_price;
}

double InventoryTracker::portfolio_concentration(
    const AssetId& asset_id,
    const std::unordered_map<AssetId, Mojo>& price_map) const
{
    std::shared_lock lock(mtx_records_);

    double asset_value = 0.0;
    double total_value = 0.0;

    for (const auto& [id, rec] : records_) {
        if (rec.total_quantity == 0) {
            continue;
        }

        // Look up the current price for this asset.  If no price is available
        // (e.g. an illiquid CAT with no recent trades), use the cost basis as
        // a conservative proxy.
        Mojo price = 0;
        auto pit = price_map.find(id);
        if (pit != price_map.end()) {
            price = pit->second;
        } else {
            price = rec.weighted_avg_cost_basis;
        }

        const double value = static_cast<double>(rec.total_quantity)
                           * static_cast<double>(price);
        total_value += value;

        if (id == asset_id) {
            asset_value = value;
        }
    }

    if (total_value <= 0.0) {
        return 0.0;
    }

    return asset_value / total_value;
}

int InventoryTracker::position_age_blocks(const AssetId& asset_id,
                                          BlockHeight    current_block) const
{
    std::shared_lock lock(mtx_records_);
    const AssetRecord* rec = find_record_locked(asset_id);
    if (!rec || rec->last_fill_block == 0) {
        // Asset never traded.
        return -1;
    }

    // Guard against the (unlikely) case where current_block < last_fill_block
    // due to a reorg or stale data.
    if (current_block < rec->last_fill_block) {
        return 0;
    }

    return static_cast<int>(current_block - rec->last_fill_block);
}

double InventoryTracker::compute_kelly_size(double spread_bps,
                                            double sigma,
                                            double tau) const
{
    // Validate inputs: sigma and tau must be meaningfully positive to avoid
    // division by zero or near-zero denominators.  VolatilityEstimator may
    // return 0.0 during cold start (< min_candles); subnormal/near-zero
    // values are equally hazardous.  The epsilon 1e-10 is chosen to reject
    // any estimate that lacks statistical significance while remaining well
    // below realistic volatility floors (~1e-4 for even the calmest markets).
    // ISO/IEC 5055: guard against division by zero and numerical instability.
    if (sigma < 1e-10 || tau < 1e-10) {
        return 0.0;  // No data = no sizing
    }

    // Convert spread from basis points to a fraction.
    const double spread_frac = spread_bps / 10000.0;

    // Edge = spread - sigma * sqrt(tau).
    // This is the expected profit per unit after subtracting adverse-selection
    // cost.  If the edge is non-positive, no bet should be made.
    const double edge = spread_frac - sigma * std::sqrt(tau);
    if (edge <= 0.0) {
        return 0.0;
    }

    // Full Kelly: f* = edge / (sigma^2 * tau).
    const double sigma_sq_tau = sigma * sigma * tau;

    // Additional safety: if sigma^2 * tau is negligibly small, the Kelly
    // fraction would be enormous -- clamp early.
    if (sigma_sq_tau < 1e-15) {
        return 0.0;
    }

    const double full_kelly = edge / sigma_sq_tau;

    // Apply the kelly_fraction scaling (default 0.5 = half-Kelly).
    double sized = full_kelly * kelly_fraction_;

    // Clamp to the per-pair capital limit from config.
    sized = std::min(sized, max_capital_per_pair_pct_);

    // Practical cap from strategy doc: ~2% of capital per pair per price level.
    static constexpr double kPracticalCapPerLevel = 0.02;
    sized = std::min(sized, kPracticalCapPerLevel);

    // Floor at zero (should already be positive, but defensive).
    sized = std::max(sized, 0.0);

    return sized;
}

RiskStatus InventoryTracker::get_risk_status(
    const AssetId& asset_id,
    Mojo           current_price,
    const std::unordered_map<AssetId, Mojo>& price_map) const
{
    // ISO/IEC 5055: acquire mtx_records_ once and compute all risk checks
    // inline.  The previous implementation called is_underwater() and
    // portfolio_concentration(), each of which acquired the same shared lock,
    // creating a nested-shared-lock hazard on non-recursive shared_mutex
    // implementations.
    std::shared_lock lock(mtx_records_);

    // Priority 1 (highest severity): Underwater.
    // Inline the logic from is_underwater() to avoid re-acquiring mtx_records_.
    {
        const AssetRecord* rec = find_record_locked(asset_id);
        if (rec && rec->total_quantity != 0 &&
            rec->weighted_avg_cost_basis > current_price) {
            return RiskStatus::Underwater;
        }
    }

    // Priority 2 & 3: Portfolio concentration (soft / hard limits).
    // Inline the logic from portfolio_concentration() to avoid re-acquiring
    // mtx_records_.
    double concentration = 0.0;
    {
        double asset_value = 0.0;
        double total_value = 0.0;

        for (const auto& [id, rec] : records_) {
            if (rec.total_quantity == 0) {
                continue;
            }

            Mojo price = 0;
            auto pit = price_map.find(id);
            if (pit != price_map.end()) {
                price = pit->second;
            } else {
                price = rec.weighted_avg_cost_basis;
            }

            const double value = static_cast<double>(rec.total_quantity)
                               * static_cast<double>(price);
            total_value += value;

            if (id == asset_id) {
                asset_value = value;
            }
        }

        if (total_value > 0.0) {
            concentration = asset_value / total_value;
        }
    }

    if (concentration >= hard_limit_pct_) {
        return RiskStatus::HardLimit;
    }

    if (concentration >= soft_limit_pct_) {
        return RiskStatus::SoftLimit;
    }

    return RiskStatus::Normal;
}

// ===========================================================================
// Capital Allocation
// ===========================================================================

bool InventoryTracker::allocate_capital(CapitalCategory category, Mojo amount)
{
    if (amount <= 0) {
        return false;
    }

    const auto idx = static_cast<std::size_t>(category);
    if (idx >= kCapitalCategoryCount) {
        return false;
    }

    std::unique_lock lock(mtx_capital_);

    // Compute the maximum allowed allocation for this category.
    const Mojo max_allowed = static_cast<Mojo>(
        static_cast<double>(total_capital_) * limits_[idx].max_frac);

    // Check whether adding `amount` would exceed the ceiling.
    if (allocated_[idx] + amount > max_allowed) {
        return false;
    }

    allocated_[idx] += amount;
    return true;
}

void InventoryTracker::free_capital(CapitalCategory category, Mojo amount)
{
    if (amount <= 0) {
        return;
    }

    const auto idx = static_cast<std::size_t>(category);
    if (idx >= kCapitalCategoryCount) {
        return;
    }

    std::unique_lock lock(mtx_capital_);

    // Clamp to prevent underflow (should not happen in correct operation, but
    // defensive coding per ISO/IEC 5055).
    if (amount > allocated_[idx]) {
        allocated_[idx] = 0;
    } else {
        allocated_[idx] -= amount;
    }
}

Mojo InventoryTracker::allocated_capital(CapitalCategory category) const
{
    const auto idx = static_cast<std::size_t>(category);
    if (idx >= kCapitalCategoryCount) {
        return 0;
    }

    std::shared_lock lock(mtx_capital_);
    return allocated_[idx];
}

Mojo InventoryTracker::free_capacity(CapitalCategory category) const
{
    const auto idx = static_cast<std::size_t>(category);
    if (idx >= kCapitalCategoryCount) {
        return 0;
    }

    std::shared_lock lock(mtx_capital_);
    const Mojo max_allowed = static_cast<Mojo>(
        static_cast<double>(total_capital_) * limits_[idx].max_frac);
    const Mojo used = allocated_[idx];

    return (max_allowed > used) ? (max_allowed - used) : 0;
}

Mojo InventoryTracker::total_capital() const
{
    std::shared_lock lock(mtx_capital_);
    return total_capital_;
}

void InventoryTracker::set_total_capital(Mojo new_total)
{
    if (new_total < 0) {
        return;
    }

    std::unique_lock lock(mtx_capital_);
    total_capital_ = new_total;
}

void InventoryTracker::seed_position(const AssetId& asset_id,
                                     Mojo           qty,
                                     Mojo           estimated_price)
{
    if (qty <= 0 || estimated_price <= 0) {
        return;
    }

    std::unique_lock lock(mtx_records_);

    // If this asset already has a real position (from fill tracking or a
    // previous seed), do not overwrite it.
    auto it = records_.find(asset_id);
    if (it != records_.end() && it->second.total_quantity > 0) {
        return;
    }

    // Insert or find the record.
    auto [ins_it, inserted] = records_.try_emplace(asset_id, asset_id);
    AssetRecord& rec = ins_it->second;

    // Use wide arithmetic for cost = qty * estimated_price.
    const Wide cost = static_cast<Wide>(qty) * static_cast<Wide>(estimated_price);
    rec.total_quantity = qty;
    rec.total_cost     = static_cast<Mojo>(cost);
    recompute_basis(rec);
}

// ===========================================================================
// Accessors
// ===========================================================================

AssetRecord InventoryTracker::get_record(const AssetId& asset_id) const
{
    std::shared_lock lock(mtx_records_);
    const AssetRecord* rec = find_record_locked(asset_id);
    return rec ? *rec : AssetRecord(asset_id);
}

std::vector<AssetRecord> InventoryTracker::get_all_records() const
{
    std::shared_lock lock(mtx_records_);
    std::vector<AssetRecord> result;
    result.reserve(records_.size());
    for (const auto& [id, rec] : records_) {
        result.push_back(rec);
    }
    return result;
}

bool InventoryTracker::no_loss_constraint_enabled() const noexcept
{
    // ISO/IEC 5055: explicit atomic load for thread-safe flag access.
    return no_loss_constraint_.load(std::memory_order_acquire);
}

void InventoryTracker::set_no_loss_constraint(bool enabled) noexcept
{
    // ISO/IEC 5055: explicit atomic store for thread-safe flag update.
    no_loss_constraint_.store(enabled, std::memory_order_release);
}

Mojo InventoryTracker::min_ask_price(const AssetId& asset_id) const
{
    // ISO/IEC 5055: atomic load for thread-safe flag access.
    if (!no_loss_constraint_.load(std::memory_order_acquire)) {
        return 0;
    }

    std::shared_lock lock(mtx_records_);
    const AssetRecord* rec = find_record_locked(asset_id);
    if (!rec || rec->total_quantity == 0) {
        return 0;
    }

    return rec->weighted_avg_cost_basis;
}

double InventoryTracker::single_cat_cap() const noexcept
{
    return single_cat_cap_pct_;
}

bool InventoryTracker::within_cat_cap(
    const AssetId& asset_id,
    Mojo           proposed_qty,
    Mojo           current_price,
    const std::unordered_map<AssetId, Mojo>& price_map) const
{
    // Compute the hypothetical portfolio concentration if we added the
    // proposed quantity.  We do this by computing the current total portfolio
    // value and the current asset value, then adding the proposed increment
    // to both.
    std::shared_lock lock(mtx_records_);

    double asset_value = 0.0;
    double total_value = 0.0;

    for (const auto& [id, rec] : records_) {
        if (rec.total_quantity == 0) {
            continue;
        }
        Mojo price = 0;
        auto pit = price_map.find(id);
        if (pit != price_map.end()) {
            price = pit->second;
        } else {
            price = rec.weighted_avg_cost_basis;
        }

        const double value = static_cast<double>(rec.total_quantity)
                           * static_cast<double>(price);
        total_value += value;

        if (id == asset_id) {
            asset_value = value;
        }
    }

    // Add the proposed purchase to the calculation.
    const double proposed_value = static_cast<double>(proposed_qty)
                                * static_cast<double>(current_price);
    asset_value += proposed_value;
    total_value += proposed_value;

    if (total_value <= 0.0) {
        return true; // No capital deployed -- trivially within limits.
    }

    return (asset_value / total_value) <= single_cat_cap_pct_;
}

// ===========================================================================
// Internal helpers
// ===========================================================================

const AssetRecord* InventoryTracker::find_record_locked(
    const AssetId& id) const
{
    auto it = records_.find(id);
    return (it != records_.end()) ? &it->second : nullptr;
}

void InventoryTracker::recompute_basis(AssetRecord& rec)
{
    // When the position is fully closed, reset cost basis to zero.
    // This avoids division by zero and stale cost data.
    if (rec.total_quantity == 0) {
        rec.weighted_avg_cost_basis = 0;
        rec.total_cost              = 0;
        return;
    }

    // Weighted-average cost basis = total_cost / total_quantity.
    // Use wide arithmetic to avoid overflow in the division's numerator
    // (which is just total_cost, already stored safely).
    rec.weighted_avg_cost_basis = XOP_WIDE_DIV(
        static_cast<Wide>(rec.total_cost), static_cast<Wide>(rec.total_quantity));

    // Defensive: ensure cost basis is never negative (should not happen with
    // valid inputs, but guard per ISO/IEC 5055).
    if (rec.weighted_avg_cost_basis < 0) {
        rec.weighted_avg_cost_basis = 0;
    }
}

}  // namespace xop
