// state.cpp -- Implementation of xop::Position and xop::State.
//
// All monetary arithmetic uses int64_t (mojos).  Division rounds toward zero,
// which for cost-basis calculations is conservative (never overstates the
// basis).
//
// Compliant with:
//   ISO/IEC 27001:2022  -- audit-quality logging of every balance mutation
//   ISO/IEC 5055        -- no unchecked casts, overflow-aware arithmetic
//   ISO/IEC 25000       -- single-responsibility methods, RAII locks

#include "xop/state.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace xop {

// ===================================================================
//  Position
// ===================================================================

Position::Position()
    : asset_id{}
    , balance{0}
    , cost_basis{0}
    , total_cost{0.0}
{}

Position::Position(const AssetId& id)
    : asset_id{id}
    , balance{0}
    , cost_basis{0}
    , total_cost{0.0}
{}

// ISO/IEC 5055 -- CWE-190: return false on overflow instead of silently
// dropping the addition, so callers can detect and handle the failure.
[[nodiscard]] bool Position::add(Mojo qty, Mojo unit_price)
{
    // Weighted-average cost basis update.
    //
    // new_total_cost  = old_total_cost + qty * unit_price
    // new_balance     = old_balance    + qty
    // new_cost_basis  = new_total_cost / new_balance
    //
    // Integer division truncates toward zero, which is conservative: it
    // slightly understates cost_basis, making the never-sell-at-loss
    // check marginally more permissive.  This is the safe direction.

    if (qty <= 0) {
        spdlog::warn("Position::add called with non-positive qty={} for asset={}",
                      qty, asset_id);
        return false;
    }

    // [PNL-BASIS-OVERFLOW 2026-07-30] qty * unit_price legitimately exceeds
    // int64: a 1-XCH fill is qty=1e12 mojos at a pseudo-price of ~1.3e12,
    // i.e. ~1.3e24 versus INT64_MAX 9.2e18.  The previous int64 total_cost
    // therefore made this method reject EVERY XCH buy ("[Position] Overflow
    // in cost basis -- addition rejected", 20 occurrences in the live log on
    // 2026-07-29), so the XCH balance only ever decreased.  total_cost is a
    // double now; the product cannot overflow and the only real failure mode
    // left is a non-positive resulting balance.

    const Mojo new_balance = balance + qty;
    if (new_balance <= 0) {
        spdlog::error("[Position] Invalid resulting balance -- addition "
                      "rejected for asset={} qty={} price={}",
                       asset_id, qty, unit_price);
        return false;
    }

    total_cost += static_cast<double>(qty) * static_cast<double>(unit_price);
    balance    = new_balance;
    cost_basis = (balance > 0)
        ? static_cast<Mojo>(std::llround(total_cost
                                         / static_cast<double>(balance)))
        : 0;
    return true;
}

bool Position::remove(Mojo qty)
{
    if (qty <= 0) {
        spdlog::warn("Position::remove called with non-positive qty={} for asset={}",
                      qty, asset_id);
        return false;
    }

    if (qty > balance) {
        spdlog::warn("Position::remove qty={} exceeds balance={} for asset={}",
                      qty, balance, asset_id);
        return false;
    }

    // Proportional drawdown preserves cost_basis:
    //   new_total_cost = total_cost * (balance - qty) / balance
    //
    // [PNL-BASIS-OVERFLOW 2026-07-30] total_cost is a double, so this is a
    // plain scale with no wide-integer intermediate needed.
    if (qty == balance) {
        total_cost = 0.0;   // full exit -- avoid rounding residue
        balance    = 0;
    } else {
        total_cost *= static_cast<double>(balance - qty)
                    / static_cast<double>(balance);
        balance    -= qty;
    }

    cost_basis = (balance > 0)
        ? static_cast<Mojo>(std::llround(total_cost
                                         / static_cast<double>(balance)))
        : 0;

    return true;
}

// ===================================================================
//  BotStatus helpers
// ===================================================================

const char* to_string(BotStatus s) noexcept
{
    switch (s) {
        case BotStatus::Initializing: return "Initializing";
        case BotStatus::Analyzing:    return "Analyzing";
        case BotStatus::Running:      return "Running";
        case BotStatus::Paused:       return "Paused";
        case BotStatus::ShuttingDown: return "ShuttingDown";
        case BotStatus::Stopped:      return "Stopped";
    }
    return "Unknown";
}

// ===================================================================
//  State
// ===================================================================

State::State()
    : status_{BotStatus::Initializing}
{}

// -- status -----------------------------------------------------------

BotStatus State::status() const noexcept
{
    return status_.load(std::memory_order_acquire);
}

void State::set_status(BotStatus s) noexcept
{
    const BotStatus prev = status_.exchange(s, std::memory_order_acq_rel);
    if (prev != s) {
        spdlog::info("BotStatus: {} -> {}", to_string(prev), to_string(s));
    }
}

// -- positions --------------------------------------------------------

void State::record_buy(const AssetId& asset_id, Mojo qty, Mojo unit_price)
{
    std::unique_lock lock(mtx_positions_);

    auto [it, inserted] = positions_.try_emplace(asset_id, asset_id);
    // ISO/IEC 5055 -- CWE-190: propagate overflow detection to caller's log.
    if (!it->second.add(qty, unit_price)) {
        spdlog::error("record_buy: Position::add failed (overflow) "
                      "asset={} qty={} price={}", asset_id, qty, unit_price);
        return;
    }

    spdlog::info("record_buy  asset={} qty={} price={} -> balance={} basis={}",
                  asset_id, qty, unit_price,
                  it->second.balance, it->second.cost_basis);
}

bool State::record_sell(const AssetId& asset_id, Mojo qty)
{
    std::unique_lock lock(mtx_positions_);

    auto it = positions_.find(asset_id);
    if (it == positions_.end()) {
        spdlog::warn("record_sell: unknown asset={}", asset_id);
        return false;
    }

    const bool ok = it->second.remove(qty);
    if (ok) {
        spdlog::info("record_sell asset={} qty={} -> balance={} basis={}",
                      asset_id, qty, it->second.balance, it->second.cost_basis);
    }
    return ok;
}

double State::inventory_skew(const AssetId& base_id, const AssetId& quote_id) const
{
    // Skew = (base_value - quote_value) / (base_value + quote_value)
    //
    // Both values are expressed as total_cost (mojos of quote), which gives a
    // common numeraire.  For XCH (which IS the quote in most pairs) we use
    // the balance directly since total_cost may be zero for assets that were
    // not "bought" but deposited.
    //
    // If neither position exists (no capital deployed) return 0.0 -- neutral.

    std::shared_lock lock(mtx_positions_);

    // total_cost is a double (PNL-BASIS-OVERFLOW); keep the whole ratio in
    // double so the ~1e24-scale values cannot be truncated on the way in.
    double base_val  = 0.0;
    double quote_val = 0.0;

    if (auto it = positions_.find(base_id); it != positions_.end()) {
        // Use total_cost (denominated in quote mojos) when available;
        // fall back to balance for the native quote asset.
        base_val = (it->second.total_cost > 0.0)
                       ? it->second.total_cost
                       : static_cast<double>(it->second.balance);
    }

    if (auto it = positions_.find(quote_id); it != positions_.end()) {
        quote_val = (it->second.total_cost > 0.0)
                        ? it->second.total_cost
                        : static_cast<double>(it->second.balance);
    }

    const double total = base_val + quote_val;
    if (total == 0.0) {
        return 0.0;
    }

    return static_cast<double>(base_val - quote_val)
         / static_cast<double>(total);
}

Position State::get_position(const AssetId& asset_id) const
{
    std::shared_lock lock(mtx_positions_);

    if (auto it = positions_.find(asset_id); it != positions_.end()) {
        return it->second;  // copy
    }
    return Position{asset_id};
}

std::vector<Position> State::get_all_positions() const
{
    std::shared_lock lock(mtx_positions_);

    std::vector<Position> out;
    out.reserve(positions_.size());
    for (const auto& [id, pos] : positions_) {
        out.push_back(pos);
    }
    return out;
}

// -- pending offers ---------------------------------------------------

void State::upsert_offer(const PendingOffer& offer)
{
    std::unique_lock lock(mtx_offers_);
    pending_offers_.insert_or_assign(offer.offer_id, offer);

    spdlog::debug("upsert_offer id={} pair={} side={} price={} size={}",
                   offer.offer_id, offer.pair_name,
                   to_string(offer.side), offer.price, offer.size);
}

bool State::remove_offer(const std::string& offer_id)
{
    std::unique_lock lock(mtx_offers_);

    const auto erased = pending_offers_.erase(offer_id);
    if (erased > 0) {
        spdlog::debug("remove_offer id={}", offer_id);
    }
    return erased > 0;
}

bool State::mark_cancel_pending(const std::string& offer_id)
{
    std::unique_lock lock(mtx_offers_);

    auto it = pending_offers_.find(offer_id);
    if (it == pending_offers_.end()) return false;
    it->second.cancel_pending = true;
    spdlog::debug("mark_cancel_pending id={}", offer_id);
    return true;
}

PendingOffer State::get_offer(const std::string& offer_id) const
{
    std::shared_lock lock(mtx_offers_);

    if (auto it = pending_offers_.find(offer_id); it != pending_offers_.end()) {
        return it->second;  // copy
    }
    return PendingOffer{};
}

std::vector<PendingOffer> State::get_all_offers() const
{
    std::shared_lock lock(mtx_offers_);

    std::vector<PendingOffer> out;
    out.reserve(pending_offers_.size());
    for (const auto& [id, offer] : pending_offers_) {
        out.push_back(offer);
    }
    return out;
}

std::size_t State::offer_count() const
{
    std::shared_lock lock(mtx_offers_);
    return pending_offers_.size();
}

// -- market snapshots -------------------------------------------------

void State::register_pair_asset_keys(const std::string& base_asset_id,
                                     const std::string& quote_asset_id,
                                     const std::string& pair_name)
{
    std::unique_lock lock(mtx_markets_);
    // Only register the natural ordering (base/quote) to preserve
    // price-direction semantics in mark_to_xch probes.
    asset_pair_index_[base_asset_id + "/" + quote_asset_id] = pair_name;
    spdlog::debug("register_pair_asset_keys {}/{} -> {}",
                   base_asset_id.substr(0, 12), quote_asset_id.substr(0, 12),
                   pair_name);
}

void State::update_market(const MarketSnapshot& snap)
{
    std::unique_lock lock(mtx_markets_);
    markets_.insert_or_assign(snap.pair_name, snap);

    spdlog::debug("update_market pair={} mid={} bid={} ask={} spread_bps={:.1f}",
                   snap.pair_name, snap.mid_price,
                   snap.best_bid, snap.best_ask, snap.spread_bps);
}

MarketSnapshot State::get_market(const std::string& key) const
{
    std::shared_lock lock(mtx_markets_);

    // Primary lookup: by human-readable pair name (e.g. "XCH/wUSDC.b").
    if (auto it = markets_.find(key); it != markets_.end()) {
        return it->second;  // copy
    }

    // Secondary lookup: resolve asset-ID-based key (e.g. "xch/<hex>")
    // to the registered pair name, then fetch the snapshot.
    if (auto idx = asset_pair_index_.find(key); idx != asset_pair_index_.end()) {
        if (auto it = markets_.find(idx->second); it != markets_.end()) {
            return it->second;  // copy
        }
    }

    return MarketSnapshot{};
}

std::vector<MarketSnapshot> State::get_all_markets() const
{
    std::shared_lock lock(mtx_markets_);

    std::vector<MarketSnapshot> out;
    out.reserve(markets_.size());
    for (const auto& [name, snap] : markets_) {
        out.push_back(snap);
    }
    return out;
}

// ===========================================================================
// Asset XCH rates
// ===========================================================================

void State::set_asset_xch_rate(const AssetId& asset_id, double xch_mojos_per_asset_mojo)
{
    std::unique_lock lock(mtx_xch_rates_);
    xch_rates_[asset_id] = xch_mojos_per_asset_mojo;
}

double State::get_asset_xch_rate(const AssetId& asset_id) const
{
    std::shared_lock lock(mtx_xch_rates_);
    auto it = xch_rates_.find(asset_id);
    return (it != xch_rates_.end()) ? it->second : 0.0;
}

// ===========================================================================
// Analysis summaries
// ===========================================================================

void State::set_analysis_results(std::vector<PairAnalysisSummary> summaries,
                                  double spread_multiplier)
{
    std::unique_lock lock(mtx_analysis_);
    analysis_summaries_   = std::move(summaries);
    analysis_spread_mult_ = spread_multiplier;
}

std::vector<PairAnalysisSummary> State::get_analysis_summaries() const
{
    std::shared_lock lock(mtx_analysis_);
    return analysis_summaries_;
}

double State::analysis_spread_multiplier() const
{
    std::shared_lock lock(mtx_analysis_);
    return analysis_spread_mult_;
}

}  // namespace xop
