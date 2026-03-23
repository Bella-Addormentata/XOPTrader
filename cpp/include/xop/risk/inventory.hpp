// inventory.hpp -- Inventory tracking, cost basis accounting, and risk metrics
//                  for XOPTrader CHIA DEX market-making bot.
//
// This module is the authoritative source of truth for:
//   - Per-asset cost basis (weighted-average method)
//   - Inventory limit enforcement (soft / hard / underwater / aging / CAT cap)
//   - Half-Kelly position sizing
//   - Capital allocation category tracking
//   - Portfolio concentration monitoring
//
// Monetary invariants:
//   ALL monetary values (prices, quantities, cost bases, capital) use int64_t
//   mojos to prevent floating-point drift.  Conversion to double occurs ONLY
//   for ratio calculations (e.g. inventory_ratio, portfolio_concentration) and
//   the result is never stored back as a monetary value.
//
// Thread safety:
//   All public methods acquire at most one mutex (shared or exclusive) and
//   release it before returning.  No method nests locks.  Deadlock is
//   therefore impossible by construction.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- controlled mutable state, audit-ready accessors
//   ISO/IEC 5055       -- no raw pointer ownership, RAII locking, bounds checks
//   ISO/IEC 25000      -- documented interfaces, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard C++17, no undefined behaviour

#ifndef XOP_RISK_INVENTORY_HPP
#define XOP_RISK_INVENTORY_HPP

#include "xop/types.hpp"
#include "xop/config.hpp"

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// RiskStatus -- tiered risk classification for a single asset.
//
// Enum values are ordered by severity so that comparison operators can be used
// to decide whether a higher-severity threshold has been breached.
// ---------------------------------------------------------------------------

enum class RiskStatus : std::uint8_t {
    Normal    = 0, // Inventory within acceptable bounds.
    SoftLimit = 1, // >= soft_limit_pct one-sided -- begin aggressive skewing.
    HardLimit = 2, // >= hard_limit_pct one-sided -- pull quotes on overweight side.
    Underwater = 3 // cost_basis > current market price -- hold, offer above cost only.
};

/// Human-readable label for logging / Prometheus export.
const char* to_string(RiskStatus s) noexcept;

// ---------------------------------------------------------------------------
// CapitalCategory -- mutually exclusive allocation buckets (strategy doc S7).
// ---------------------------------------------------------------------------

enum class CapitalCategory : std::uint8_t {
    ActiveOffers     = 0, // 35-45% -- outstanding bid/ask offers.
    RebalancingBuffer = 1, // 15-20% -- inventory rebalancing after fills.
    EmergencyReserve = 2, // 15-20% -- stablecoin or cold XCH.
    OpportunityReserve = 3, // 10-15% -- new pairs, flash events.
    OperationalFloat = 4  // 5-10%  -- fees, dust, coin splitting.
};

/// Total number of distinct capital categories.
inline constexpr std::size_t kCapitalCategoryCount = 5;

/// Human-readable label for logging.
const char* to_string(CapitalCategory c) noexcept;

// ---------------------------------------------------------------------------
// CapitalLimits -- min/max allocation fractions per category (fractions in
//                  [0, 1]).  Defaults from the strategy document, section 7.
// ---------------------------------------------------------------------------

struct CapitalLimits {
    double min_frac; // Minimum fraction of total capital.
    double max_frac; // Maximum fraction of total capital.
};

/// Default allocation limits derived from the strategy document.
/// Index by static_cast<size_t>(CapitalCategory).
inline constexpr CapitalLimits kDefaultCapitalLimits[kCapitalCategoryCount] = {
    { 0.35, 0.45 }, // ActiveOffers
    { 0.15, 0.20 }, // RebalancingBuffer
    { 0.15, 0.20 }, // EmergencyReserve
    { 0.10, 0.15 }, // OpportunityReserve
    { 0.05, 0.10 }  // OperationalFloat
};

// ---------------------------------------------------------------------------
// AssetRecord -- per-asset accounting snapshot.
//
// Tracks the weighted-average cost basis and fill history required by the
// inventory controls and the never-sell-at-loss constraint.
// ---------------------------------------------------------------------------

struct AssetRecord {
    AssetId     asset_id;         // Which asset this record tracks.
    Mojo        total_quantity;   // Current holdings in mojos (>= 0).
    Mojo        total_cost;       // Cumulative cost of current holdings (mojos).
    Mojo        weighted_avg_cost_basis; // total_cost / total_quantity (mojos),
                                         // or 0 when total_quantity == 0.
    BlockHeight last_fill_block;  // Block height of the most recent fill.
    Timestamp   last_fill_time;   // Wall-clock time of the most recent fill.

    AssetRecord();
    explicit AssetRecord(const AssetId& id);
};

// ---------------------------------------------------------------------------
// InventoryTracker -- the central risk module.
//
// Holds per-asset records, enforces inventory limits, computes position sizes,
// and tracks capital allocation across categories.
//
// Usage (typical per-block loop):
//   1. Call record_buy / record_sell when fills are confirmed on-chain.
//   2. Call get_risk_status before posting new quotes.
//   3. Call compute_kelly_size to determine position size.
//   4. Call allocate_capital / free_capital when offers are created / cancelled.
// ---------------------------------------------------------------------------

class InventoryTracker {
public:
    // -- Construction -------------------------------------------------------

    /// Construct with risk thresholds from configuration and the total capital
    /// deployed (in mojos).  The no-loss constraint is enabled by default.
    explicit InventoryTracker(const RiskConfig& risk_cfg,
                              Mojo              total_capital_mojos);

    /// Construct with explicit no-loss toggle.
    InventoryTracker(const RiskConfig& risk_cfg,
                     Mojo              total_capital_mojos,
                     bool              no_loss_constraint);

    // -- Cost Basis Tracking ------------------------------------------------

    /// Record a purchase of `qty` mojos of `asset_id` at `fill_price` mojos
    /// per unit.  Updates the weighted-average cost basis:
    ///   new_basis = (old_total_cost + fill_price * qty) / (old_qty + qty)
    ///
    /// Preconditions:
    ///   qty > 0, fill_price > 0.  Violations are logged and ignored.
    void record_buy(const AssetId& asset_id,
                    Mojo           qty,
                    Mojo           fill_price,
                    BlockHeight    block,
                    Timestamp      ts);

    /// Record a sale of `qty` mojos of `asset_id` at `sell_price` mojos per
    /// unit.  Reduces total_cost proportionally (weighted-average drawdown):
    ///   new_total_cost = old_total_cost * (1 - qty / old_qty)
    ///
    /// If no_loss_constraint is enabled and sell_price < cost_basis, the sell
    /// is REJECTED and the method returns false.
    ///
    /// Returns false (and does nothing) if:
    ///   - qty > current holdings
    ///   - sell_price < cost_basis AND no_loss_constraint is true
    ///   - qty <= 0 or sell_price <= 0
    [[nodiscard]] bool record_sell(const AssetId& asset_id,
                                   Mojo           qty,
                                   Mojo           sell_price,
                                   BlockHeight    block,
                                   Timestamp      ts);

    // -- Risk Metrics -------------------------------------------------------

    /// Signed net inventory for an asset.
    ///   Positive = long (we hold), negative = short (impossible in spot, but
    ///   the interface supports it for future perp hedging).
    /// Returns 0 if asset is unknown.
    Mojo net_inventory(const AssetId& asset_id) const;

    /// Inventory ratio for a base/quote pair.
    ///   Returns a value in [0.0, 1.0] where 0.5 means perfectly balanced.
    ///   0.0 = all value is in quote, 1.0 = all value is in base.
    ///   `base_price` is the current market price of base in quote terms
    ///   (mojos), used to make the two sides commensurable.
    /// Returns 0.5 if neither side has holdings (no data = balanced).
    double inventory_ratio(const AssetId& base_id,
                           const AssetId& quote_id,
                           Mojo           base_price) const;

    /// True when the weighted-average cost basis of `asset_id` exceeds
    /// `current_price`.  Always false when there are no holdings.
    bool is_underwater(const AssetId& asset_id, Mojo current_price) const;

    /// Fraction of total portfolio value that `asset_id` represents.
    ///   Returns a value in [0.0, 1.0].
    ///   `current_price` converts the asset's holdings into quote-equivalent
    ///   mojos.
    ///   Uses the sum of (holdings_i * price_i) across all known assets as the
    ///   denominator.  `price_map` provides the current market price for every
    ///   tracked asset.
    double portfolio_concentration(
        const AssetId& asset_id,
        const std::unordered_map<AssetId, Mojo>& price_map) const;

    /// Number of blocks since the last fill for an asset.
    ///   Returns -1 if the asset has never been traded.
    ///   A return value > kStalePositionBlocks (~1662 blocks = ~24h) triggers
    ///   the "position aging" control.
    int position_age_blocks(const AssetId& asset_id,
                            BlockHeight    current_block) const;

    /// Half-Kelly position size as a fraction of capital.
    ///   f* = kelly_fraction * (spread_bps/10000 - sigma*sqrt(tau))
    ///                        / (sigma^2 * tau)
    ///
    ///   where tau = time horizon in years.
    ///
    ///   The result is clamped to [0.0, max_capital_per_pair_pct] and further
    ///   capped at the practical 2% per pair per level guideline when the raw
    ///   Kelly exceeds it.
    ///
    ///   Returns 0.0 when the edge is non-positive (no bet should be made).
    double compute_kelly_size(double spread_bps,
                              double sigma,
                              double tau) const;

    /// Composite risk status for an asset, considering all inventory controls:
    ///   1. Underwater check      (cost > market)
    ///   2. Hard limit check      (>= hard_limit_pct)
    ///   3. Soft limit check      (>= soft_limit_pct)
    ///   4. Otherwise Normal
    ///
    /// `current_price` -- market price in mojos.
    /// `price_map`     -- price map for portfolio concentration calculation.
    RiskStatus get_risk_status(
        const AssetId& asset_id,
        Mojo           current_price,
        const std::unordered_map<AssetId, Mojo>& price_map) const;

    // -- Capital Allocation -------------------------------------------------

    /// Allocate `amount` mojos from the given category.
    ///   Returns false if the allocation would exceed the category's maximum
    ///   fraction of total capital.
    [[nodiscard]] bool allocate_capital(CapitalCategory category, Mojo amount);

    /// Return `amount` mojos back to the given category (e.g. when an offer
    /// is cancelled or filled).
    void free_capital(CapitalCategory category, Mojo amount);

    /// Current allocated amount in a category (mojos).
    Mojo allocated_capital(CapitalCategory category) const;

    /// Free (unallocated) capacity remaining in a category (mojos).
    Mojo free_capacity(CapitalCategory category) const;

    /// Total capital across all categories.
    Mojo total_capital() const;

    /// Update the total capital (e.g. after PnL recalculation).
    void set_total_capital(Mojo new_total);

    // -- Accessors ----------------------------------------------------------

    /// Read a copy of the asset record.  Returns a default (zero) record if
    /// the asset has never been seen.
    AssetRecord get_record(const AssetId& asset_id) const;

    /// Read copies of all tracked asset records.
    std::vector<AssetRecord> get_all_records() const;

    /// Whether the no-loss constraint is active.
    bool no_loss_constraint_enabled() const noexcept;

    /// Toggle the no-loss constraint at runtime (e.g. for testing).
    void set_no_loss_constraint(bool enabled) noexcept;

    /// Minimum ask price for an asset, respecting cost basis and no-loss.
    ///   Returns cost_basis if no_loss_constraint is enabled (the strategy
    ///   layer adds min_profit_margin on top).
    ///   Returns 0 if the constraint is disabled or the asset is unknown.
    Mojo min_ask_price(const AssetId& asset_id) const;

    /// Maximum allowed portfolio concentration for any single CAT (fraction).
    /// The strategy layer should check portfolio_concentration < this value
    /// before opening new positions in a CAT.
    double single_cat_cap() const noexcept;

    /// Check whether adding `proposed_qty` mojos of `asset_id` at
    /// `current_price` would breach the single-CAT concentration cap.
    /// Returns true if the proposed position is within limits.
    bool within_cat_cap(const AssetId& asset_id,
                        Mojo           proposed_qty,
                        Mojo           current_price,
                        const std::unordered_map<AssetId, Mojo>& price_map) const;

    // -- Constants ----------------------------------------------------------

    /// ~24 hours in CHIA blocks (24 * 3600 / 52 = 1661.5, rounded up).
    static constexpr int kStalePositionBlocks = 1662;

private:
    // -- Internal helpers ---------------------------------------------------

    /// Look up an asset record under an existing shared lock.
    /// Returns nullptr if not found.  Caller must hold mtx_records_ (shared or
    /// exclusive).
    const AssetRecord* find_record_locked(const AssetId& id) const;

    /// Recompute the weighted-average cost basis after a quantity/cost change.
    /// Handles the total_quantity == 0 edge case (sets cost_basis to 0).
    static void recompute_basis(AssetRecord& rec);

    // -- Data ---------------------------------------------------------------

    mutable std::shared_mutex                         mtx_records_;
    std::unordered_map<AssetId, AssetRecord>          records_;

    mutable std::shared_mutex                         mtx_capital_;
    Mojo                                              total_capital_;
    Mojo allocated_[kCapitalCategoryCount];            // Per-category allocation.
    CapitalLimits limits_[kCapitalCategoryCount];       // Per-category limits.

    // Configuration captured at construction.
    double soft_limit_pct_;
    double hard_limit_pct_;
    double single_cat_cap_pct_;
    double kelly_fraction_;
    double max_capital_per_pair_pct_;
    bool   no_loss_constraint_;
};

}  // namespace xop

#endif  // XOP_RISK_INVENTORY_HPP
