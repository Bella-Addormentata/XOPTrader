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
// Overflow note (PNL-BASIS-OVERFLOW, fixed 2026-07-30):
//   The product (fill_price * qty) routinely exceeds int64_t: fill_price is
//   an engine pseudo-price (~1e12-1e14) and qty is in base mojos (1 XCH =
//   1e12), so total_cost reaches ~1e24+ for any XCH-sized fill.  The previous
//   code stored the UNDIVIDED product back into an int64 AssetRecord field;
//   on MSVC (Wide = double, no __int128) the out-of-range double->int64 cast
//   saturated to INT64_MIN and recompute_basis clamped the negative basis to
//   0 -- silently zeroing the XCH cost basis on the first bid fill after
//   every restart, which in turn forced realized PnL to 0 on every later
//   ask.  AssetRecord::total_cost is now a double end-to-end: ~15
//   significant digits keeps the derived basis accurate to well under one
//   mojo at any realistic capital level, and there is no int64 round-trip.
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

namespace xop {

namespace {

constexpr Mojo kCatMojosPerUnit = 1'000LL;

Mojo inferred_mojos_per_unit(const AssetId& asset_id) noexcept
{
    return asset_id == "xch" ? kMojosPerXch : kCatMojosPerUnit;
}

} // namespace

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
    , total_cost{0.0}
    , weighted_avg_cost_basis{0}
    , last_fill_block{0}
    , last_fill_time{}
    , basis_is_seed_sentinel{false}
{}

AssetRecord::AssetRecord(const AssetId& id)
    : asset_id{id}
    , total_quantity{0}
    , total_cost{0.0}
    , weighted_avg_cost_basis{0}
    , last_fill_block{0}
    , last_fill_time{}
    , basis_is_seed_sentinel{false}
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

    const Mojo new_total_qty  = rec.total_quantity + qty;
    if (new_total_qty < rec.total_quantity) {
        // Overflow detected -- reject silently rather than corrupt state.
        return;
    }

    if (rec.basis_is_seed_sentinel) {
        // [v0.7.46 #1] First real fill after a synthetic seed.
        // Replace the sentinel basis (Mojo{1}) with the observed fill
        // price instead of weighted-averaging.  The seeded quantity was
        // acquired off-chain at an unknown price; the most defensible
        // estimate is the first observed fill price for this asset.
        // Without this replacement, weighted-averaging a large seeded
        // quantity at basis=1 with a small real fill at basis=P keeps
        // basis ~= 1, causing realized PnL to wildly overstate gains
        // on subsequent sells (XOPTRADER-PNL-COST-BASIS-SENTINEL).
        rec.total_cost              = static_cast<double>(fill_price)
                                    * static_cast<double>(new_total_qty);
        rec.total_quantity          = new_total_qty;
        rec.basis_is_seed_sentinel  = false;
    } else {
        // Weighted-average cost basis update:
        //   new_total_cost = old_total_cost + fill_price * qty
        //   new_total_qty  = old_qty + qty
        //   new_basis      = new_total_cost / new_total_qty
        //
        // total_cost is a double; the product cannot overflow (double range
        // ~1e308) and precision loss is negligible (PNL-BASIS-OVERFLOW).
        rec.total_cost    += static_cast<double>(fill_price)
                           * static_cast<double>(qty);
        rec.total_quantity = new_total_qty;
    }

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
                                   Timestamp      ts,
                                   bool           enforce_no_loss)
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
    if (enforce_no_loss
        && no_loss_constraint_.load(std::memory_order_acquire)
        && rec.total_quantity > 0)
    {
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
    if (qty == rec.total_quantity) {
        // Full liquidation -- shortcut avoids rounding residue.
        rec.total_cost     = 0.0;
        rec.total_quantity = 0;
    } else {
        const double remaining_fraction =
            static_cast<double>(rec.total_quantity - qty)
            / static_cast<double>(rec.total_quantity);
        rec.total_cost     *= remaining_fraction;
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
    return inventory_ratio(base_id,
                           quote_id,
                           base_price,
                           inferred_mojos_per_unit(base_id),
                           inferred_mojos_per_unit(quote_id));
}

double InventoryTracker::inventory_ratio(const AssetId& base_id,
                                         const AssetId& quote_id,
                                         Mojo           base_price,
                                         Mojo           base_mojos_per_unit,
                                         Mojo           quote_mojos_per_unit) const
{
    if (base_price <= 0 || base_mojos_per_unit <= 0 || quote_mojos_per_unit <= 0) {
        return 0.5;
    }

    std::shared_lock lock(mtx_records_);

    const AssetRecord* base_rec  = find_record_locked(base_id);
    const AssetRecord* quote_rec = find_record_locked(quote_id);

    // Normalize both holdings into display units first. Prices are carried in
    // the engine's kMojosPerXch fixed-point format even for CAT quotes, so the
    // pair-specific mojos-per-unit values must be applied here to avoid
    // overstating base exposure on mixed-denomination pairs.
    const double base_units = base_rec
        ? static_cast<double>(base_rec->total_quantity)
          / static_cast<double>(base_mojos_per_unit)
        : 0.0;
    const double quote_units = quote_rec
        ? static_cast<double>(quote_rec->total_quantity)
          / static_cast<double>(quote_mojos_per_unit)
        : 0.0;

    const double quote_per_base = static_cast<double>(base_price)
        / static_cast<double>(kMojosPerXch);
    const double base_value = base_units * quote_per_base;

    const double total = base_value + quote_units;
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

    rec.total_quantity = qty;
    rec.total_cost     = static_cast<double>(qty)
                       * static_cast<double>(estimated_price);
    // [v0.7.46 #1] Mark the basis as a synthetic seed so that the first
    // real record_buy() replaces (not blends) it with the observed fill
    // price.  Only treat estimated_price <= 1 as a sentinel; callers that
    // pass a real estimate (e.g. cost-basis restore from DB) still get
    // standard weighted-average behaviour.
    rec.basis_is_seed_sentinel = (estimated_price <= 1);
    recompute_basis(rec);
}

// ===========================================================================
// Persistence support (PNL-BASIS-PERSIST, 2026-07-30)
// ===========================================================================

void InventoryTracker::restore_record(const AssetId& asset_id,
                                      Mojo           qty,
                                      double         total_cost,
                                      bool           basis_is_seed_sentinel)
{
    if (qty < 0 || total_cost < 0.0) {
        return;
    }

    std::unique_lock lock(mtx_records_);

    auto [it, inserted] = records_.try_emplace(asset_id, asset_id);
    AssetRecord& rec = it->second;

    rec.total_quantity         = qty;
    rec.total_cost             = total_cost;
    rec.basis_is_seed_sentinel = basis_is_seed_sentinel;
    recompute_basis(rec);
}

void InventoryTracker::reseed_basis(const AssetId& asset_id, Mojo price)
{
    if (price <= 0) {
        return;
    }

    std::unique_lock lock(mtx_records_);

    auto it = records_.find(asset_id);
    if (it == records_.end() || it->second.total_quantity <= 0) {
        return;
    }

    AssetRecord& rec = it->second;
    rec.total_cost = static_cast<double>(rec.total_quantity)
                   * static_cast<double>(price);
    rec.basis_is_seed_sentinel = false;
    recompute_basis(rec);
}

bool InventoryTracker::record_fill_unpriced(const AssetId& asset_id,
                                            Mojo           qty,
                                            bool           is_buy,
                                            BlockHeight    block,
                                            Timestamp      ts)
{
    if (qty <= 0) {
        return false;
    }

    std::unique_lock lock(mtx_records_);

    auto [it, inserted] = records_.try_emplace(asset_id, asset_id);
    AssetRecord& rec = it->second;

    if (is_buy) {
        const Mojo new_qty = rec.total_quantity + qty;
        if (new_qty < rec.total_quantity) {
            return false;  // overflow guard
        }
        // Cost the new lot at the CURRENT basis so the weighted average is
        // unchanged.  When the record is still a sentinel this keeps basis
        // at the sentinel value AND keeps the flag set, so the later
        // mark-at-first-observation upgrade still repairs the whole holding.
        rec.total_cost += static_cast<double>(qty)
                        * static_cast<double>(rec.weighted_avg_cost_basis);
        rec.total_quantity = new_qty;

        // A record that never had a real price (basis 0 on a brand-new
        // record, or the 1-mojo wallet seed) now holds quantity whose cost
        // is genuinely unknown.  Flag it as a sentinel so the
        // mark-at-first-observation upgrade repairs it once a mark exists.
        // Without this, an unpriced fill against a fresh record would leave
        // basis 0 with the flag clear -- silently unrepairable.
        if (rec.weighted_avg_cost_basis <= 1) {
            rec.basis_is_seed_sentinel = true;
        }
    } else {
        if (qty > rec.total_quantity) {
            return false;
        }
        if (qty == rec.total_quantity) {
            rec.total_cost     = 0.0;
            rec.total_quantity = 0;
        } else {
            rec.total_cost *= static_cast<double>(rec.total_quantity - qty)
                            / static_cast<double>(rec.total_quantity);
            rec.total_quantity -= qty;
        }
    }

    recompute_basis(rec);
    rec.last_fill_block = block;
    rec.last_fill_time  = ts;
    return true;
}

void InventoryTracker::adjust_quantity(const AssetId& asset_id,
                                       Mojo           new_qty,
                                       Mojo           price_for_additions)
{
    if (new_qty < 0) {
        return;
    }

    std::unique_lock lock(mtx_records_);

    auto [it, inserted] = records_.try_emplace(asset_id, asset_id);
    AssetRecord& rec = it->second;

    if (new_qty == rec.total_quantity) {
        return;
    }

    if (new_qty < rec.total_quantity) {
        // Withdrawal / untracked spend: proportional cost drawdown keeps the
        // weighted-average basis of the remaining position unchanged.
        if (new_qty == 0) {
            rec.total_cost = 0.0;
        } else {
            rec.total_cost *= static_cast<double>(new_qty)
                            / static_cast<double>(rec.total_quantity);
        }
        rec.total_quantity = new_qty;
    } else {
        // Deposit / untracked receipt: mark-at-receipt.  Without a usable
        // price the delta cannot be costed -- leave the record untouched so
        // the caller can retry once market data is available.
        if (price_for_additions <= 0) {
            return;
        }
        const Mojo delta = new_qty - rec.total_quantity;
        rec.total_cost    += static_cast<double>(delta)
                           * static_cast<double>(price_for_additions);
        rec.total_quantity = new_qty;
        // A real price observation replaces sentinel provenance.
        rec.basis_is_seed_sentinel = false;
    }

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
        rec.total_cost              = 0.0;
        return;
    }

    // Weighted-average cost basis = total_cost / total_quantity.
    // total_cost is a double (PNL-BASIS-OVERFLOW); llround keeps the basis
    // faithful to the nearest mojo instead of truncating toward zero (the
    // prior truncation could round a basis of 0.9999 down to the sentinel
    // range and re-trigger the basis-unknown guard).
    rec.weighted_avg_cost_basis = static_cast<Mojo>(std::llround(
        rec.total_cost / static_cast<double>(rec.total_quantity)));

    // Defensive: ensure cost basis is never negative (should not happen with
    // valid inputs, but guard per ISO/IEC 5055).
    if (rec.weighted_avg_cost_basis < 0) {
        rec.weighted_avg_cost_basis = 0;
    }
}

}  // namespace xop
