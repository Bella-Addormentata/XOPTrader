// ---------------------------------------------------------------------------
// pace_controller.hpp -- the pace controller's decisions, as pure functions.
//
// [PACE 2026-09-13] Why it exists.  Avellaneda-Stoikov sizes the bid as
// q_max x max(0, 1 - q/q_max) from TOTAL base holdings (avellaneda.cpp:268-270).
// With 24.57 XCH held against q_max 20, every XCH-base bid was 0, so XCH/BYC
// could not buy XCH with BYC -- while BYC was 62% of the portfolio at
// independent fair value against a 5% +/- 2% target.
//
// What it does, for an overweight CAT K that is the QUOTE of every enabled
// pair touching it (XCH/K only):
//   * WHEN: hysteresis on K's share of the targeted portfolio, valued only at
//     the independent fair value, from wallet-confirmed, validated, fresh
//     balances.  Any gap is DataUnavailable; an Active asset then HOLDS.
//   * HOW MUCH, per rolling day of 4,608 PEAK heights:
//       budget    = (excess + reduced in the horizon) x 4608 / H
//       remaining = min(budget - sold in the last day, excess, XCH headroom)
//       resting   = min(frac x budget, remaining, XCH headroom)
//   * HOW: Step 6 replaces the pair's bid size with the pace pool BEFORE the
//     risk limits run, so they always apply.  Step 7 composes the pool with
//     the risk-tapered bid, the CAT wallet cap, a wallet-truth concentration
//     keep and the drift scale, re-plans the tiers, and zeroes the ask.
//   * PRICE: every kept bid is capped at FV x (1 - max(min_edge, k x sigma)),
//     tiers spaced like the order-book guard, whatever the tightening state
//     (P24), and a resting bid a fair-value drop left above fair value is
//     cancelled (P26).  When behind schedule the bid is tightened toward that
//     cap; never inside the width floor, never crossing, never failing Step 8's
//     BBO proximity check.
//
// Every decision is a pure function of its arguments, so cpp/tests drives it
// directly (S36: no Engine is constructed in xop_tests).  The engine glue
// (engine.cpp, "[PACE]") only gathers inputs and applies outputs.
//
// Spec: specs/pace-controller.v2.md, sections 5.1-5.6.  Every block count is
// a PEAK height (4,608 per day, 18.75 s each), never the 1,662 figure.
// ---------------------------------------------------------------------------

#ifndef XOP_STRATEGY_PACE_CONTROLLER_HPP
#define XOP_STRATEGY_PACE_CONTROLLER_HPP

#include "xop/types.hpp"
#include "xop/peg_registry.hpp"
#include "xop/execution/cross_guard.hpp"
#include "xop/strategy/bbo_sanity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xop::strategy::pace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// One day of PEAK heights (86,400 s / 18.75 s); NOT the 1,662
/// transaction-block figure.
inline constexpr std::uint32_t kPaceWindowBlocks  = 4'608u;
inline constexpr std::uint32_t kPaceMaxTiers      = 16u;
inline constexpr double        kPaceMaxTightenBps = 5'000.0;
inline constexpr double        kPaceMinStepBps    = 1.0;
inline constexpr double        kPaceMaxStepBps    = 1'000.0;
static_assert(kPaceWindowBlocks == 4'608u);
static_assert(kPaceMaxTiers == 16u);

// ---------------------------------------------------------------------------
// Types.  Every member has a default member initializer (GCC
// -Wmissing-field-initializers under designated initialisation).
// ---------------------------------------------------------------------------

/// ZERO ENUMERATOR IS THE FAIL-CLOSED VALUE (book_side_quality.hpp's rule):
/// a value-initialised status reads "no usable data", never "active".
enum class PaceStatus : std::uint8_t {
    DataUnavailable = 0,   ///< inert if the latch was off
    Inactive,
    Active,
    Exhausted,             ///< Active, remaining <= 0
    Hold,                  ///< latch on + DataUnavailable
    ConfigConflict,
};

/// Which step last shrank a managed pair's pace pool (Step 7 log).
enum class PoolBinding : std::uint8_t {
    None = 0,              ///< pair not managed
    Hold,
    Pace,
    Risk,
    Wallet,
    WalletConcentration,
    Drift,
    MinTier,
};

[[nodiscard]] constexpr const char* to_string(PaceStatus s) noexcept
{
    switch (s) {
        case PaceStatus::DataUnavailable: return "DataUnavailable";
        case PaceStatus::Inactive:        return "Inactive";
        case PaceStatus::Active:          return "Active";
        case PaceStatus::Exhausted:       return "Exhausted";
        case PaceStatus::Hold:            return "Hold";
        case PaceStatus::ConfigConflict:  return "ConfigConflict";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* to_string(PoolBinding b) noexcept
{
    switch (b) {
        case PoolBinding::None:                return "None";
        case PoolBinding::Hold:                return "Hold";
        case PoolBinding::Pace:                return "Pace";
        case PoolBinding::Risk:                return "Risk";
        case PoolBinding::Wallet:              return "Wallet";
        case PoolBinding::WalletConcentration: return "WalletConcentration";
        case PoolBinding::Drift:               return "Drift";
        case PoolBinding::MinTier:             return "MinTier";
    }
    return "unknown";
}

/// One targeted (or managed) asset, as the engine sees it this heartbeat.
struct HoldingInput {
    std::string key{};                ///< upper-cased symbol
    bool        targeted{false};      ///< present in asset_target_allocations
    double      target{0.0};
    double      tol{0.0};
    bool        is_xch{false};
    bool        id_resolved{false};   ///< the symbol maps to an asset id
    bool        cache_present{false}; ///< a wallet balance entry exists
    bool        fields_validated{false};
    BlockHeight as_of_block{0};
    double      units{0.0};           ///< confirmed / mojos-per-unit
    bool        fv_present{false};
    bool        fv_tier_available{false};
    bool        fv_feed_fresh{false}; ///< CoinGecko feed age, not the solve stamp
    bool        asset_is_quote{true}; ///< orientation of the fair-value pair
    double      fv_price{0.0};        ///< quote per base, display units
    double      fv_sigma_bps{0.0};
};

/// One configured pair that touches a pace asset.
struct PairInput {
    std::string   name{};
    std::string   base_key{};
    std::string   quote_key{};
    bool          enabled{false};
    bool          quote_valid{false};  ///< cycle_[name].quote_valid after Step 5
    bool          fills_ok{true};
    std::int64_t  base_mpu{0};
    std::int64_t  quote_mpu{0};
    std::optional<double> min_offer_override{};
    bool          xch_base{false};
    std::uint32_t side_tier_count{0};  ///< the pair's LiquidityEngine num_tiers
    bool          fv_ok{false};        ///< P2-usable for this pair, feed freshness included
    double        fv_price{0.0};
    double        fv_sigma_bps{0.0};
};

struct FlowUnits     { double reduced{0.0}; double increased{0.0}; bool recognised{false}; };
struct Progress      { double sold_window{0.0}; double reduced_horizon{0.0}; double increased_horizon{0.0}; std::size_t rows_ignored{0}; };
struct Activation    { bool config_ok{false}; bool active{false}; double enter_level{0.0}; double exit_level{0.0}; };
struct Budget        { double budget_window{0.0}; double remaining{0.0}; double resting_cap{0.0}; bool exhausted{true}; };
struct OfferSizeBand { double min_units{0.0}; double max_units{0.0}; };
struct TierPlan      { std::uint32_t tiers{0}; Mojo tier_size_base_mojos{0}; Mojo min_tier_base_mojos{0}; bool conflict{false}; };

struct Valuation {
    bool   ok{false};
    double total_xch{0.0};
    double managed_xch{0.0};
    double managed_xch_per_unit{0.0};
    double share{0.0};
    std::unordered_map<std::string, double> xch_by_key{};
};

/// One maker (trade_log) or taker (taker_fills) row.
struct FillRow {
    std::string pair_name{};
    bool        is_taker{false};
    std::string side_lower{};          ///< maker: "bid" | "ask"
    Mojo        size_mojos{0};
    Mojo        price_mojos{0};
    bool        we_bought_base{false}; ///< taker
    Mojo        base_delta_mojos{0};
    Mojo        quote_delta_mojos{0};
    BlockHeight block_height{0};
};

/// The only per-asset state kept between heartbeats; reset on restart.
struct AssetMemory {
    bool          active{false};
    std::uint32_t ramp_blocks{0};      ///< <= kPaceWindowBlocks
    BlockHeight   last_block{0};
};

/// Copied from StrategyConfig each heartbeat.
struct PaceParams {
    bool enabled{false};
    std::vector<std::string> assets{};
    std::uint32_t horizon_blocks{64'512u};
    double enter_tol_mult{1.5};
    double exit_tol_mult{1.0};
    double max_resting_frac{0.5};
    double min_tier_units{1.0};
    double max_tier_units{5.0};
    std::uint32_t max_tiers{3u};
    double tighten_step_bps{25.0};
    double tighten_max_bps{300.0};
    double max_fv_sigma_bps{200.0};
    std::uint32_t max_balance_age_blocks{20u};
    double global_min_offer_units{0.1};   ///< strategy.min_offer_size_units
    double global_max_offer_units{5.0};   ///< strategy.max_offer_size_units
};

struct PaceInputs {
    BlockHeight now{0};
    bool        ramp_running{false};   ///< engine in a posting state
    PaceParams  params{};
    std::vector<HoldingInput> holdings{};
    std::vector<PairInput>    pairs{};
    std::vector<FillRow>      fills{};
    std::unordered_map<std::string, AssetMemory> memory{};
};

/// PairCycleState::pace -- one pair's plan for this heartbeat.
struct PairPlan {
    bool          managed{false};      ///< false: every hook is a no-op
    bool          hold{false};         ///< pools 0, no pricing, no cap/increasing cancels
    std::string   asset{};
    std::uint32_t tiers{0};
    Mojo          tier_size_base_mojos{0};
    Mojo          min_tier_base_mojos{0};
    Mojo          pool_base_mojos{0};
    double        resting_cap_units{0.0};   ///< managed units, this pair
    double        remaining_units{0.0};     ///< managed units, this pair
    double        tighten_bps{0.0};
    double        fv_price{0.0};            ///< also units of K per base unit
    double        fv_sigma_bps{0.0};
    OfferSizeBand band{};
    std::uint32_t side_tier_count{0};
    std::uint32_t eligible_pairs{0};
    PoolBinding   binding{PoolBinding::None};
    double        throttle_bid_size_scale{1.0};   ///< set by the Step 7 size post-pass
    std::vector<Mojo> untightened_bid_px{};       ///< set by the price post-pass, index = tier_index
};

struct AssetDecision {
    std::string   key{};
    PaceStatus    status{PaceStatus::DataUnavailable};
    double        share{0.0};
    double        enter_level{0.0};
    double        exit_level{0.0};
    double        excess_units{0.0};
    double        budget_window_units{0.0};
    double        sold_window_units{0.0};
    double        reduced_horizon_units{0.0};
    double        increased_horizon_units{0.0};
    double        remaining_units{0.0};
    double        resting_cap_units{0.0};
    double        headroom_units{0.0};
    double        tighten_bps{0.0};
    std::uint32_t ramp_blocks{0};
    std::uint32_t eligible_pairs{0};
    std::size_t   rows_ignored{0};
    bool          band_conflict{false};
};

struct PaceDecision {
    std::vector<AssetDecision> assets{};
    std::unordered_map<std::string, PairPlan>    pairs{};           ///< by pair name
    std::unordered_map<std::string, AssetMemory> memory{};          ///< COMPLETE new memory
    std::unordered_map<std::string, double>      remaining_units{}; ///< COMPLETE, by asset
};

struct PoolInputs {
    Mojo   bid_after_floor{0};             ///< avail_capital at the Step 7 hook
    Mojo   ask_after_floor{0};             ///< avail_inventory at the Step 7 hook
    Mojo   risk_bid{0};                    ///< pcs.risk_quote.bid_size
    bool   wallet_cap_known{false};
    Mojo   wallet_cap_bid{0};              ///< the CAT wallet cap on the bid
    double wallet_concentration_keep{1.0}; ///< 1.0 when no taper applies
    double drift_bid_scale{1.0};           ///< the drift guard's bid scale
};

struct ComposedPools {
    Mojo          bid{0};
    Mojo          ask{0};
    std::uint32_t tiers{0};
    Mojo          tier_size_base_mojos{0};
    PoolBinding   binding{PoolBinding::None};
};

/// Mojo-scaled doubles (price x 1e12).
struct PriceGuards {
    double fair_value_px{0.0};
    double fv_sigma_bps{0.0};
    double min_edge_bps{0.0};
    double edge_sigma_mult{0.0};
    double centre_px{0.0};
    double min_half_spread_bps{0.0};
    double best_bid_px{0.0};
    double best_ask_px{0.0};
    double book_guard_margin_bps{0.0};
    double published_mid_px{0.0};
    bool   has_bbo{false};
    double bid_tier_ref_px{0.0};
    double effective_mid_px{0.0};
    double max_aggressive_dev{0.0};
    double max_passive_dev{0.0};
    double tier_step_bps{0.0};
};

struct RestingOffer {
    std::string  id{};
    Side         side{Side::Bid};
    std::uint8_t tier{0};
    Mojo         price{0};
    Mojo         size{0};
    bool         cancel_pending{false};
};

struct CancelPick {
    std::vector<std::string> budget_ids{};
    std::vector<std::string> tier_ids{};
    std::vector<std::string> increasing_ids{};
};

/// P24's result, for the Step 7 log.
struct FairValueCap {
    Mojo          cap_px{0};       ///< 0 when the plan's fair value is unusable
    std::uint32_t lowered{0};
    std::uint32_t dropped{0};
};

/// The engine states P25 reads.  A default-constructed value is closed
/// (pace_enabled and flash_crash_normal are false).
struct RefreshGates {
    bool          pace_enabled{false};
    bool          dry_run{false};
    bool          wallet_circuit_open{false};
    bool          watchdog_fired{false};
    std::uint32_t wallet_consecutive_failures{0};
    bool          gui_pause{false};
    bool          breaker_pause{false};
    bool          cancel_all_inflight{false};
    bool          cancel_all_draining{false};
    bool          flash_crash_normal{false};
    bool          xch_recovery{false};
};

// ===========================================================================
// P1, P3 -- block arithmetic
// ===========================================================================

/// P1: a cached wallet balance is usable only when its fields were validated
/// and it is at most `max_age` peak blocks old.  A height regression (as_of
/// ahead of now) counts as zero elapsed, the engine's own convention, and
/// never wraps the unsigned difference.
[[nodiscard]] constexpr bool balance_is_fresh(bool validated, BlockHeight as_of, BlockHeight now,
                                              std::uint32_t max_age) noexcept
{
    return validated && (as_of >= now || now - as_of <= max_age);
}

/// P3: a - b, or 0 when b is ahead (no uint32 wrap).
[[nodiscard]] constexpr std::uint32_t sat_sub_blocks(BlockHeight a, BlockHeight b) noexcept
{
    return a >= b ? a - b : 0u;
}

static_assert(sat_sub_blocks(5u, 9u) == 0u);
static_assert(balance_is_fresh(true, 130u, 120u, 20u));

// ===========================================================================
// P2, P4 -- valuation at independent fair value
// ===========================================================================

/// P2: XCH per unit of an asset from its independent fair value.  nullopt
/// unless the tier is available, the CoinGecko feed is fresh, the price is
/// finite and > 0, and the sigma is finite, >= 0 and no larger than a finite
/// ceiling > 0.  `fv_price` is quote per base: a quote asset is worth
/// 1/price XCH per unit, a base asset price XCH per unit.
[[nodiscard]] inline std::optional<double> xch_per_unit_from_fair_value(
    bool asset_is_quote, bool tier_available, bool feed_fresh,
    double fv_price, double sigma_bps, double max_sigma_bps) noexcept
{
    if (!tier_available) return std::nullopt;
    if (!feed_fresh) return std::nullopt;
    if (!(std::isfinite(fv_price) && fv_price > 0.0)) return std::nullopt;
    if (!(std::isfinite(sigma_bps) && sigma_bps >= 0.0)) return std::nullopt;
    if (!(std::isfinite(max_sigma_bps) && max_sigma_bps > 0.0)) return std::nullopt;
    if (sigma_bps > max_sigma_bps) return std::nullopt;
    const double rate = asset_is_quote ? 1.0 / fv_price : fv_price;
    if (!(std::isfinite(rate) && rate > 0.0)) return std::nullopt;
    return rate;
}

/// P4: the TARGETED portfolio in XCH at independent fair value, summed in
/// input order.  Fails closed (ok = false) on: a missing asset id or cache
/// entry; an unvalidated or stale balance (checked BEFORE the zero-units
/// shortcut); non-finite or negative units; a held asset without a usable
/// rate; the managed key without a rate, even at zero units; the managed key
/// absent or untargeted; a total that is not finite and > 0.
[[nodiscard]] inline Valuation value_portfolio(const std::vector<HoldingInput>& holdings,
                                               std::string_view managed, BlockHeight now,
                                               std::uint32_t max_age, double max_sigma)
{
    Valuation out{};
    double total = 0.0;
    double managed_xch = 0.0;
    double managed_rate = 0.0;
    bool found = false;
    for (const HoldingInput& h : holdings) {
        if (!h.targeted) { continue; }
        if (!h.id_resolved || !h.cache_present) { return Valuation{}; }
        if (!balance_is_fresh(h.fields_validated, h.as_of_block, now, max_age)) { return Valuation{}; }
        if (!(std::isfinite(h.units) && h.units >= 0.0)) { return Valuation{}; }
        const std::optional<double> rate = h.is_xch
            ? std::optional<double>{1.0}
            : xch_per_unit_from_fair_value(h.asset_is_quote, h.fv_present && h.fv_tier_available,
                                           h.fv_feed_fresh, h.fv_price, h.fv_sigma_bps, max_sigma);
        if (h.key == managed) {
            found = true;
            if (!rate) { return Valuation{}; }   // the managed key needs a rate even at 0 units
            managed_rate = *rate;
        }
        if (h.units == 0.0 && h.key != managed) { continue; }
        if (!rate) { return Valuation{}; }   // a held asset without a rate fails closed
        const double v = h.units * *rate;
        if (!std::isfinite(v)) { return Valuation{}; }
        out.xch_by_key[h.key] += v;
        total += v;
        if (h.key == managed) { managed_xch = v; }
    }
    if (!found) { return Valuation{}; }
    if (!(std::isfinite(total) && total > 0.0)) { return Valuation{}; }
    out.ok = true;
    out.total_xch = total;
    out.managed_xch = managed_xch;
    out.managed_xch_per_unit = managed_rate;
    out.share = managed_xch / total;
    return out;
}

// ===========================================================================
// P5, P6 -- activation and excess
// ===========================================================================

/// P5: hysteresis.  Enter when share > target + enter x tol; once active,
/// stay active while share > target + exit x tol.  A non-finite config,
/// tol <= 0, a negative exit multiplier or exit >= enter is a conflict and
/// never activates.  A non-finite share never activates.
[[nodiscard]] inline Activation decide_activation(bool was_active, double share, double target,
                                                  double tol, double enter_mult, double exit_mult) noexcept
{
    Activation a{};
    a.config_ok = std::isfinite(target) && std::isfinite(tol) && std::isfinite(enter_mult)
               && std::isfinite(exit_mult) && tol > 0.0 && exit_mult >= 0.0 && exit_mult < enter_mult;
    a.enter_level = target + enter_mult * tol;
    a.exit_level = target + exit_mult * tol;
    a.active = a.config_ok && std::isfinite(share)
            && (was_active ? share > a.exit_level : share > a.enter_level);
    return a;
}

/// P6: native units of the managed asset above the exit level, 0 when any
/// input is non-finite, the rate is not > 0 or the total is not > 0.
[[nodiscard]] inline double excess_native_units(double managed_xch, double total_xch,
                                                double exit_level, double rate) noexcept
{
    if (!(std::isfinite(managed_xch) && std::isfinite(total_xch) && std::isfinite(exit_level)
          && std::isfinite(rate))) {
        return 0.0;
    }
    if (!(rate > 0.0) || !(total_xch > 0.0)) { return 0.0; }
    return std::max(0.0, managed_xch - exit_level * total_xch) / rate;
}

// ===========================================================================
// P7-P9 -- progress accounting
// ===========================================================================

/// P7a: one maker fill (trade_log row) in native units of the managed asset.
/// A base-managed asset leaves on an ask and arrives on a bid; a quote-managed
/// asset leaves on a bid (we pay quote) and arrives on an ask.  Unrecognised
/// unless side is exactly "bid" or "ask" and size, price and both mpus are > 0.
[[nodiscard]] inline FlowUnits classify_maker_fill(bool managed_is_base, std::string_view side_lower,
                                                   Mojo size, Mojo price,
                                                   std::int64_t base_mpu, std::int64_t quote_mpu) noexcept
{
    FlowUnits out{};
    const bool is_bid = (side_lower == "bid");
    const bool is_ask = (side_lower == "ask");
    if (!is_bid && !is_ask) { return out; }
    if (size <= 0 || price <= 0 || base_mpu <= 0 || quote_mpu <= 0) { return out; }
    if (managed_is_base) {
        const double u = static_cast<double>(size) / static_cast<double>(base_mpu);
        if (is_ask) { out.reduced = u; } else { out.increased = u; }
        out.recognised = true;
        return out;
    }
    const double u = quote_mojos_for(static_cast<double>(size), static_cast<double>(price),
                                     static_cast<double>(base_mpu), static_cast<double>(quote_mpu))
                     / static_cast<double>(quote_mpu);
    if (!std::isfinite(u)) { return out; }
    if (is_bid) { out.reduced = u; } else { out.increased = u; }
    out.recognised = true;
    return out;
}

/// P7b: one taker fill (taker_fills row).  Base managed: bought base
/// increases it, sold base reduces it.  Quote managed: bought base spent the
/// quote (reduces), sold base received it (increases).  Unrecognised unless
/// both mpus are > 0 and the magnitude is finite and > 0.
[[nodiscard]] inline FlowUnits classify_taker_fill(bool managed_is_base, bool we_bought_base,
                                                   Mojo base_delta, Mojo quote_delta,
                                                   std::int64_t base_mpu, std::int64_t quote_mpu) noexcept
{
    FlowUnits out{};
    if (base_mpu <= 0 || quote_mpu <= 0) { return out; }
    if (managed_is_base) {
        const double m = std::fabs(static_cast<double>(base_delta)) / static_cast<double>(base_mpu);
        if (!(std::isfinite(m) && m > 0.0)) { return out; }
        if (we_bought_base) { out.increased = m; } else { out.reduced = m; }
        out.recognised = true;
        return out;
    }
    const double m = std::fabs(static_cast<double>(quote_delta)) / static_cast<double>(quote_mpu);
    if (!(std::isfinite(m) && m > 0.0)) { return out; }
    if (we_bought_base) { out.reduced = m; } else { out.increased = m; }
    out.recognised = true;
    return out;
}

/// P8: sum classified rows.  A row is ignored, and counted, when it is
/// unrecognised or either value is non-finite or < 0.  A row at or above
/// `now` (a height regression) counts in both windows.
[[nodiscard]] inline Progress accumulate_progress(const std::vector<std::pair<BlockHeight, FlowUnits>>& rows,
                                                  BlockHeight now, std::uint32_t horizon) noexcept
{
    Progress p{};
    for (const auto& row : rows) {
        const BlockHeight bh = row.first;
        const FlowUnits& f = row.second;
        if (!f.recognised || !std::isfinite(f.reduced) || !std::isfinite(f.increased)
            || f.reduced < 0.0 || f.increased < 0.0) {
            ++p.rows_ignored;
            continue;
        }
        if (bh >= now || now - bh < kPaceWindowBlocks) { p.sold_window += f.reduced; }
        if (bh >= now || now - bh < horizon) {
            p.reduced_horizon += f.reduced;
            p.increased_horizon += f.increased;
        }
    }
    return p;
}

/// P9: native units of the managed asset the ACQUIRED asset (XCH) can still
/// take before it leaves its own band (target + tol).  +inf when the acquired
/// asset is not targeted; 0 on non-finite inputs or a non-positive rate/total.
[[nodiscard]] inline double acquired_headroom_units(bool acq_targeted, double acq_xch, double total_xch,
                                                    double acq_target, double acq_tol, double rate) noexcept
{
    if (!acq_targeted) { return std::numeric_limits<double>::infinity(); }
    if (!(std::isfinite(acq_xch) && std::isfinite(total_xch) && std::isfinite(acq_target)
          && std::isfinite(acq_tol) && std::isfinite(rate))) {
        return 0.0;
    }
    if (!(rate > 0.0) || !(total_xch > 0.0)) { return 0.0; }
    return std::max(0.0, (acq_target + acq_tol) * total_xch - acq_xch) / rate;
}

// ===========================================================================
// P10, P11 -- budget and tightening
// ===========================================================================

/// P10: the GROSS schedule.  budget = (excess + reduced in the horizon)
/// x 4608 / H is invariant under nominal execution (each unit sold leaves
/// excess and enters the reduced sum), so increases inside the window cannot
/// stall it.  remaining = min(budget - sold today, excess); resting cap =
/// min(frac x budget, remaining, headroom).  Budget{} (exhausted) on
/// non-finite inputs, excess < 0, H < one window, frac outside (0, 1], or a
/// NaN / negative headroom.
[[nodiscard]] inline Budget compute_budget(double excess, const Progress& p, std::uint32_t horizon,
                                           double max_resting_frac, double headroom) noexcept
{
    if (!std::isfinite(excess) || !std::isfinite(p.sold_window) || !std::isfinite(p.reduced_horizon)
        || !std::isfinite(p.increased_horizon) || excess < 0.0) {
        return Budget{};
    }
    if (horizon < kPaceWindowBlocks || !(max_resting_frac > 0.0 && max_resting_frac <= 1.0)) {
        return Budget{};
    }
    if (std::isnan(headroom) || headroom < 0.0) { return Budget{}; }
    Budget b{};
    b.budget_window = (excess + p.reduced_horizon) * static_cast<double>(kPaceWindowBlocks)
                    / static_cast<double>(horizon);
    b.remaining = std::max(0.0, std::min(b.budget_window - p.sold_window, excess));
    b.resting_cap = std::max(0.0, std::min(std::min(max_resting_frac * b.budget_window, b.remaining), headroom));
    b.exhausted = !(b.remaining > 0.0);
    return b;
}

/// P11: price tightening in bps, quantised DOWN to `step_bps`, by how far
/// behind a ramped schedule today's sales are.  The reference is
/// min(budget, excess + sold) so reductions from before this episode cannot
/// saturate it.  The ramp grows over one window from activation and pauses
/// in Hold.  0 unless every input is finite, budget > 0, excess and sold
/// >= 0, step in [1, 1000] and max in (0, 5000].
[[nodiscard]] inline double compute_tighten_bps(double budget, double excess, double sold_window,
                                                std::uint32_t ramp_blocks, double step_bps,
                                                double max_bps) noexcept
{
    if (!(std::isfinite(budget) && std::isfinite(excess) && std::isfinite(sold_window)
          && std::isfinite(step_bps) && std::isfinite(max_bps))) {
        return 0.0;
    }
    if (!(budget > 0.0) || excess < 0.0 || sold_window < 0.0) { return 0.0; }
    if (!(step_bps >= kPaceMinStepBps && step_bps <= kPaceMaxStepBps)) { return 0.0; }
    if (!(max_bps > 0.0 && max_bps <= kPaceMaxTightenBps)) { return 0.0; }
    const double ref = std::min(budget, excess + sold_window);
    if (!(ref > 0.0)) { return 0.0; }
    const double ramp = std::min(1.0, static_cast<double>(ramp_blocks) / static_cast<double>(kPaceWindowBlocks));
    const double behind = std::clamp((ref * ramp - sold_window) / ref, 0.0, 1.0);
    return std::min(max_bps, std::floor(behind * max_bps / step_bps) * step_bps);
}

// ===========================================================================
// P12, P13 -- tier planning
// ===========================================================================

/// P12: the offer-size band Step 7 enforces (engine.cpp's min-offer-size
/// pass, which is NOT changed): min is the pair override, else 1.0 for an
/// XCH-base pair and the global minimum otherwise; max is the global maximum
/// for an XCH-base pair and 0 (no cap) otherwise.
[[nodiscard]] inline OfferSizeBand effective_offer_size_band(bool xch_base, std::optional<double> min_override,
                                                             double global_min, double global_max) noexcept
{
    OfferSizeBand b{};
    b.min_units = min_override.value_or(xch_base ? 1.0 : global_min);
    b.max_units = (xch_base && global_max > 0.0) ? global_max : 0.0;
    return b;
}

/// P13: tiers and tier size, in integer BASE mojos, for a resting cap in
/// managed units at `units_per_base_unit` (the fair value).  Tier sizes lie in
/// [max(pace_min, band min), min(pace_max, band max)]; the count is the number
/// of minimum tiers that fit, clamped by max_tiers, the ladder's own tier
/// count and 16.  A band with no admissible size is a conflict; a cap below
/// one minimum tier, or an unusable cap or divisor, rests nothing.
[[nodiscard]] inline TierPlan plan_tiers(double cap_units, double units_per_base_unit, std::int64_t base_mpu,
                                         double pace_min, double pace_max, OfferSizeBand band,
                                         std::uint32_t max_tiers, std::uint32_t side_tier_count) noexcept
{
    TierPlan out{};
    const double lo = std::max(pace_min, band.min_units);
    const double hi = band.max_units > 0.0 ? std::min(pace_max, band.max_units) : pace_max;
    if (!std::isfinite(lo) || !std::isfinite(hi) || !(lo > 0.0) || hi < lo || base_mpu <= 0) {
        out.conflict = true;
        return out;
    }
    const std::optional<Mojo> min_tier = to_mojo_checked(std::ceil(lo * static_cast<double>(base_mpu)));
    const std::optional<Mojo> hi_m = to_mojo_checked(std::floor(hi * static_cast<double>(base_mpu)));
    if (!min_tier || !hi_m || *min_tier <= 0 || *hi_m < *min_tier) {
        out.conflict = true;
        return out;
    }
    if (!(std::isfinite(cap_units) && cap_units > 0.0)
        || !(std::isfinite(units_per_base_unit) && units_per_base_unit > 0.0)) {
        return out;
    }
    const std::optional<Mojo> cap_base =
        to_mojo_checked(std::floor(cap_units / units_per_base_unit * static_cast<double>(base_mpu)));
    if (!cap_base || *cap_base <= 0) { return out; }
    // An intended integer floor: how many minimum tiers fit, clamped to 16 so
    // the uint32 cast below is safe.
    const std::int64_t n_fit = std::min<std::int64_t>(*cap_base / *min_tier, kPaceMaxTiers);
    const std::uint32_t n = std::min({max_tiers, side_tier_count, static_cast<std::uint32_t>(n_fit), kPaceMaxTiers});
    if (n == 0u) { return out; }
    out.tiers = n;
    out.tier_size_base_mojos = std::min(*hi_m, *cap_base / static_cast<Mojo>(n));
    out.min_tier_base_mojos = *min_tier;
    return out;
}

// ===========================================================================
// P14, P15 -- pools
// ===========================================================================

/// P14: Step 6 hands apply_limits the pace pool as the reducing (bid) side,
/// in place of the strategy's size, BEFORE any risk rule runs; the rules then
/// taper it like any other bid.  A hold plan offers 0.  The ask is left to
/// apply_limits and zeroed later by P15.  Unmanaged: q unchanged.
[[nodiscard]] inline Quote inject_reducing_side(Quote q, const PairPlan& plan) noexcept
{
    if (!plan.managed) { return q; }
    q.bid_size = plan.hold ? Mojo{0} : plan.pool_base_mojos;
    return q;
}

/// P15: Step 7's pace pools.  The bid pool is min(pace pool, risk-tapered
/// bid, CAT wallet cap), then scaled by the wallet-truth concentration keep
/// and the drift guard's bid scale -- every step can only SHRINK it, and
/// `binding` names the last one that did.  Tiers are re-planned from the
/// result, so below one minimum tier nothing posts.  The increasing side (the
/// ask) is always 0.  Unmanaged: both pools pass through untouched.
[[nodiscard]] inline ComposedPools compose_pace_pools(const PairPlan& plan, const PoolInputs& in) noexcept
{
    if (!plan.managed) { return {in.bid_after_floor, in.ask_after_floor, 0u, 0, PoolBinding::None}; }
    if (plan.hold) { return {0, 0, 0u, 0, PoolBinding::Hold}; }
    Mojo pool = std::max<Mojo>(0, plan.pool_base_mojos);
    PoolBinding b = PoolBinding::Pace;
    if (in.risk_bid < pool) { pool = std::max<Mojo>(0, in.risk_bid); b = PoolBinding::Risk; }
    if (in.wallet_cap_known && in.wallet_cap_bid < pool) { pool = std::max<Mojo>(0, in.wallet_cap_bid); b = PoolBinding::Wallet; }
    const double k = in.wallet_concentration_keep;
    if (!(k >= 0.0 && k <= 1.0)) { pool = 0; b = PoolBinding::WalletConcentration; }
    else if (k < 1.0) {
        const Mojo v = to_mojo_checked(static_cast<double>(pool) * k).value_or(0);
        if (v < pool) { pool = v; b = PoolBinding::WalletConcentration; }
    }
    const double d = in.drift_bid_scale;
    if (!(d > 0.0)) { if (pool > 0) { b = PoolBinding::Drift; } pool = 0; }
    else if (d < 0.999) {   // the drift guard's own threshold for applying a scale
        const Mojo v = to_mojo_checked(static_cast<double>(pool) * d).value_or(0);
        if (v < pool) { pool = v; b = PoolBinding::Drift; }
    }
    if (plan.min_tier_base_mojos <= 0 || plan.tier_size_base_mojos <= 0 || plan.tiers == 0u) { return {0, 0, 0u, 0, b}; }
    const std::int64_t n_fit = std::min<std::int64_t>(pool / plan.min_tier_base_mojos, kPaceMaxTiers);
    const std::uint32_t n = std::min(plan.tiers, static_cast<std::uint32_t>(n_fit));
    if (n == 0u) { return {0, 0, 0u, 0, pool > 0 ? PoolBinding::MinTier : b}; }
    const Mojo size = std::min(plan.tier_size_base_mojos, pool / static_cast<Mojo>(n));
    return {size * static_cast<Mojo>(n), 0, n, size, b};
}

// ===========================================================================
// P16, P17 -- price
// ===========================================================================

/// P16: the most aggressive bid, in step quanta up to `tighten_bps` above
/// `base_px`, that is at most floor(ceiling), where the ceiling is
///     min(FV x (1 - max(min_edge, k x sigma)), centre x (1 - min half-spread),
///         best ask x (1 - book margin))
/// and passes both Step 8 gates this header can evaluate: classify_cross_bbo
/// (not crossed) and, with a BBO, classify_tier (Check 2).  There is no relief
/// term, so the bid never goes inside the k-sigma width floor.  Returns
/// base_px unchanged when no candidate qualifies or any guard input is
/// unusable.  Step 8 stays authoritative and re-checks both gates itself.
[[nodiscard]] inline Mojo apply_pace_bid_price(Mojo base_px, double tighten_bps, double step_bps,
                                               const PriceGuards& g) noexcept
{
    if (base_px <= 0) { return base_px; }
    // -- [P16 steps 1-3 begin] preconditions, edge, ceiling (NaN-safe)
    if (!std::isfinite(tighten_bps) || !std::isfinite(step_bps) || !std::isfinite(g.fair_value_px)
        || !std::isfinite(g.fv_sigma_bps) || !std::isfinite(g.min_edge_bps) || !std::isfinite(g.edge_sigma_mult)
        || !std::isfinite(g.centre_px) || !std::isfinite(g.min_half_spread_bps) || !std::isfinite(g.best_bid_px)
        || !std::isfinite(g.best_ask_px) || !std::isfinite(g.book_guard_margin_bps)
        || !std::isfinite(g.published_mid_px) || !std::isfinite(g.bid_tier_ref_px)
        || !std::isfinite(g.effective_mid_px) || !std::isfinite(g.max_aggressive_dev)
        || !std::isfinite(g.max_passive_dev) || !std::isfinite(g.tier_step_bps)) {
        return base_px;
    }
    if (!(g.fair_value_px > 0.0) || !(tighten_bps > 0.0)) { return base_px; }
    if (!(step_bps >= kPaceMinStepBps && step_bps <= kPaceMaxStepBps)) { return base_px; }
    if (!(g.min_edge_bps > 0.0) || g.fv_sigma_bps < 0.0 || g.edge_sigma_mult < 0.0) { return base_px; }
    const double edge = std::max(g.min_edge_bps, g.edge_sigma_mult * g.fv_sigma_bps);
    if (!(edge < 10'000.0)) { return base_px; }
    double ceiling = g.fair_value_px * (1.0 - edge / 1e4);
    if (g.centre_px > 0.0 && g.min_half_spread_bps > 0.0) {
        ceiling = std::min(ceiling, g.centre_px * (1.0 - g.min_half_spread_bps / 1e4));
    }
    if (g.best_ask_px > 0.0) {
        ceiling = std::min(ceiling, g.best_ask_px * (1.0 - g.book_guard_margin_bps / 1e4));
    }
    if (!(std::isfinite(ceiling) && ceiling > 0.0)) { return base_px; }
    // -- [P16 steps 1-3 end]
    // k is at most 5,000 before the cast: tighten is clamped and step >= 1.
    const int k_max = static_cast<int>(std::floor(std::min(tighten_bps, kPaceMaxTightenBps) / step_bps));
    for (int k = k_max; k >= 1; --k) {
        const double raw = static_cast<double>(base_px) * (1.0 + static_cast<double>(k) * step_bps / 1e4);
        const std::optional<Mojo> cand = to_mojo_checked(std::floor(std::min(raw, ceiling)));
        if (!cand || *cand <= base_px) { continue; }
        if (execution::classify_cross_bbo(false, static_cast<double>(*cand), g.best_bid_px, g.best_ask_px,
                                          g.published_mid_px).verdict == execution::CrossVerdict::Crossed) {
            continue;
        }
        if (g.has_bbo && strategy::classify_tier(false, static_cast<double>(*cand), g.bid_tier_ref_px,
                                                 g.effective_mid_px, g.max_aggressive_dev,
                                                 g.max_passive_dev) != strategy::BboVerdict::Pass) {
            continue;
        }
        return *cand;
    }
    return base_px;
}

/// P17: tighten every bid of a ladder with P16, walking bids in ascending
/// tier_index.  A tier P16 moved must stay at least tier_step_bps below the
/// final price of the tier above it, and strictly below it -- the engine
/// passes the order-book guard's own per-tier step, max(50,
/// fair_value_clamp_tier_step_bps) -- but never below its own untightened
/// price.  spread_bps is resynced against centre_px for every walked bid.
/// Asks are untouched.  Returns the untightened bid prices, indexed by
/// tier_index (0 where the ladder has no such bid).
inline std::vector<Mojo> tighten_bid_side(std::vector<TierQuote>& ladder, double tighten_bps,
                                          double step_bps, const PriceGuards& g)
{
    std::vector<TierQuote*> bids;
    bids.reserve(ladder.size());
    for (TierQuote& t : ladder) {
        if (t.side == Side::Bid) { bids.push_back(&t); }
    }
    std::stable_sort(bids.begin(), bids.end(), [](const TierQuote* lhs, const TierQuote* rhs) {
        return lhs->tier_index < rhs->tier_index;
    });
    std::size_t slots = 0;
    for (const TierQuote* p : bids) {
        slots = std::max(slots, static_cast<std::size_t>(p->tier_index) + 1u);
    }
    std::vector<Mojo> untight(slots, Mojo{0});
    std::optional<Mojo> prev_final;
    for (TierQuote* bid : bids) {
        const Mojo orig = bid->price;
        untight[static_cast<std::size_t>(bid->tier_index)] = orig;
        Mojo new_px = apply_pace_bid_price(orig, tighten_bps, step_bps, g);
        if (new_px > orig && prev_final.has_value()) {
            Mojo lim = to_mojo_checked(std::floor(static_cast<double>(*prev_final) * (1.0 - g.tier_step_bps / 1e4))).value_or(0);
            lim = std::min(lim, *prev_final - 1);
            if (new_px > lim) {
                new_px = std::max(orig, lim);
            }
        }
        bid->price = new_px;
        if (g.centre_px > 0.0) {
            bid->spread_bps = (static_cast<double>(bid->price) - g.centre_px) / g.centre_px * 1e4;
        }
        prev_final = bid->price;
    }
    return untight;
}

// ===========================================================================
// P18-P21 -- ladder shape, cancels, reprice, takes
// ===========================================================================

/// P18: the managed pair's ladder as posted.  Every ask is erased (the
/// increasing side); a hold plan erases every bid too.  Bids with
/// tier_index >= plan.tiers are erased -- by INDEX, so a missing tier 0 never
/// promotes tier 2, matching P19's cancels.  Kept bids get the planned size,
/// scaled down by the inventory throttle's bid size scale when it is < 1; a
/// NaN scale or one outside [0, 1] gives size 0, and size-0 bids are erased.
inline void shape_bid_side(std::vector<TierQuote>& ladder, const PairPlan& plan)
{
    if (!plan.managed) { return; }
    const double s = plan.throttle_bid_size_scale;
    std::vector<TierQuote> kept;
    kept.reserve(ladder.size());
    for (const TierQuote& t : ladder) {
        if (t.side == Side::Ask) { continue; }
        if (plan.hold) { continue; }
        if (static_cast<std::uint32_t>(t.tier_index) >= plan.tiers) { continue; }
        Mojo size = plan.tier_size_base_mojos;
        if (!(s >= 0.0 && s <= 1.0)) { size = 0; }
        else if (s < 1.0) { size = to_mojo_checked(std::floor(static_cast<double>(size) * s)).value_or(0); }
        if (size <= 0) { continue; }
        TierQuote shaped = t;
        shaped.size = size;
        kept.push_back(shaped);
    }
    ladder = std::move(kept);
}

/// P24: the fair-value cap on EVERY kept bid of a managed, non-hold plan,
/// whatever the tightening state -- Stage B's tighten 0, the first-day ramp,
/// ahead of schedule, a throttled or residual-widened heartbeat -- so no pace
/// bid is posted above floor(FV x (1 - max(min_edge, k x sigma))), the bound
/// P16 tightens toward.  FV and sigma are the plan's own: the fair value this
/// heartbeat's plan was sized on.  Bids are walked in ascending tier_index
/// against a running ceiling: the cap for the first, and for each later bid
/// also `spacing_bps` (the order-book guard's per-tier step) below the final
/// price of the bid above it, and at least 1 mojo below it.  A bid above its
/// ceiling is lowered to it, with spread_bps resynced against `centre_px` when
/// that is > 0.  A bid whose ceiling is not > 0 cannot fit and is erased.
/// Every bid is erased when the cap cannot be computed (a fair value that is
/// not finite and > 0, min_edge not > 0, a negative or non-finite sigma,
/// multiplier or spacing, or an edge of 100% or more): a bid that cannot be
/// bounded is not posted.  Asks are untouched.  Unmanaged or hold: no-op.
inline FairValueCap cap_bids_at_fair_value(std::vector<TierQuote>& ladder, const PairPlan& plan,
                                           double min_edge_bps, double edge_sigma_mult,
                                           double spacing_bps, double centre_px)
{
    FairValueCap out{};
    if (!plan.managed || plan.hold) { return out; }
    std::optional<Mojo> cap;
    if (std::isfinite(plan.fv_price) && std::isfinite(plan.fv_sigma_bps) && std::isfinite(min_edge_bps)
        && std::isfinite(edge_sigma_mult) && std::isfinite(spacing_bps) && plan.fv_price > 0.0
        && min_edge_bps > 0.0 && plan.fv_sigma_bps >= 0.0 && edge_sigma_mult >= 0.0 && spacing_bps >= 0.0) {
        const double edge = std::max(min_edge_bps, edge_sigma_mult * plan.fv_sigma_bps);
        if (edge < 10'000.0) {
            const double fv_px = plan.fv_price * static_cast<double>(kMojosPerXch);
            cap = to_mojo_checked(std::floor(fv_px * (1.0 - edge / 1e4)));
        }
    }
    std::vector<std::size_t> order;
    order.reserve(ladder.size());
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        if (ladder[i].side == Side::Bid) { order.push_back(i); }
    }
    std::stable_sort(order.begin(), order.end(), [&ladder](std::size_t lhs, std::size_t rhs) {
        return ladder[lhs].tier_index < ladder[rhs].tier_index;
    });
    std::vector<bool> dropped_at(ladder.size(), false);
    std::optional<Mojo> prev_final;
    for (const std::size_t i : order) {
        TierQuote& bid = ladder[i];
        Mojo ceiling = cap.value_or(0);
        if (prev_final.has_value()) {
            Mojo lim = to_mojo_checked(std::floor(static_cast<double>(*prev_final) * (1.0 - spacing_bps / 1e4))).value_or(0);
            lim = std::min(lim, *prev_final - 1);
            ceiling = std::min(ceiling, lim);
        }
        if (ceiling <= 0 || bid.price <= 0) {
            dropped_at[i] = true;
            ++out.dropped;
            continue;
        }
        if (bid.price > ceiling) {
            bid.price = ceiling;
            ++out.lowered;
            if (centre_px > 0.0) {
                bid.spread_bps = (static_cast<double>(bid.price) - centre_px) / centre_px * 1e4;
            }
        }
        prev_final = bid.price;
    }
    if (out.dropped > 0u) {
        std::vector<TierQuote> kept;
        kept.reserve(ladder.size());
        for (std::size_t i = 0; i < ladder.size(); ++i) {
            if (!dropped_at[i]) { kept.push_back(ladder[i]); }
        }
        ladder = std::move(kept);
    }
    out.cap_px = cap.value_or(0);
    return out;
}

/// P19: which resting offers of a managed pair Step 8 cancels.  Nothing for a
/// hold plan (a data blip must not cancel the ladder) or an unmanaged pair.
/// cancel_pending offers are skipped, so a bid whose cancel never landed on
/// chain does not count against the caps (#157 escalates such cancels).
/// Asks go to increasing_ids; bids with
/// tier >= plan.tiers to tier_ids, in input order.  The remaining bids must
/// fit BOTH min(resting cap, remaining) in managed units and the composed
/// pool in base mojos: widest first (tier desc, price asc, id asc) are popped
/// into budget_ids until they do.  A bid whose value is unusable goes to
/// budget_ids first.
inline CancelPick select_resting_to_cancel(const std::vector<RestingOffer>& resting, const PairPlan& plan,
                                           std::int64_t base_mpu, std::int64_t quote_mpu)
{
    CancelPick pick{};
    if (!plan.managed || plan.hold) { return pick; }
    struct Live {
        const RestingOffer* offer{nullptr};
        double              units{0.0};
    };
    std::vector<Live> live;
    for (const RestingOffer& o : resting) {
        if (o.cancel_pending) { continue; }
        if (o.side == Side::Ask) { pick.increasing_ids.push_back(o.id); continue; }
        if (static_cast<std::uint32_t>(o.tier) >= plan.tiers) { pick.tier_ids.push_back(o.id); continue; }
        const double u = (quote_mpu > 0)
            ? quote_mojos_for(static_cast<double>(o.size), static_cast<double>(o.price),
                              static_cast<double>(base_mpu), static_cast<double>(quote_mpu))
                  / static_cast<double>(quote_mpu)
            : std::numeric_limits<double>::quiet_NaN();
        live.push_back(Live{&o, u});
    }
    double cap_u = std::min(plan.resting_cap_units, plan.remaining_units);
    if (!(std::isfinite(cap_u) && cap_u > 0.0)) { cap_u = 0.0; }
    const Mojo cap_b = std::max<Mojo>(0, plan.pool_base_mojos);
    std::vector<Live> good;
    for (const Live& l : live) {
        if (!(std::isfinite(l.units) && l.units > 0.0 && l.offer->size > 0)) {
            pick.budget_ids.push_back(l.offer->id);
        } else {
            good.push_back(l);
        }
    }
    std::sort(good.begin(), good.end(), [](const Live& lhs, const Live& rhs) {
        if (lhs.offer->tier != rhs.offer->tier) { return lhs.offer->tier > rhs.offer->tier; }
        if (lhs.offer->price != rhs.offer->price) { return lhs.offer->price < rhs.offer->price; }
        return lhs.offer->id < rhs.offer->id;
    });
    double total_u = 0.0;
    Mojo total_b = 0;
    for (const Live& l : good) {
        total_u += l.units;
        total_b += l.offer->size;
    }
    for (const Live& l : good) {
        if (!(total_u > cap_u + 1e-9 || total_b > cap_b)) { break; }
        pick.budget_ids.push_back(l.offer->id);
        total_u -= l.units;
        total_b -= l.offer->size;
    }
    return pick;
}

/// P26: the resting reducing bids of a managed, non-hold plan priced ABOVE
/// the plan's fair value -- posted before a fair-value drop that the
/// canceller's adverse-drift threshold (1.0-2.0% by tier) has not yet
/// refreshed.  Step 8 cancels them (reason pace_above_fv), so no pace bid
/// keeps selling the managed asset below fair value.  The bound is the fair
/// value itself, not P24's cap, so ordinary fair-value noise causes no churn.
/// Asks and cancel_pending offers are skipped (asks are already cancelled as
/// the increasing side).  Empty for an unmanaged or hold plan, or when the
/// plan's fair value is not finite and > 0.
inline std::vector<std::string> select_resting_above_fair_value(const std::vector<RestingOffer>& resting,
                                                                const PairPlan& plan)
{
    std::vector<std::string> ids;
    if (!plan.managed || plan.hold) { return ids; }
    const double fv_px = plan.fv_price * static_cast<double>(kMojosPerXch);
    if (!(std::isfinite(fv_px) && fv_px > 0.0)) { return ids; }
    for (const RestingOffer& o : resting) {
        if (o.cancel_pending || o.side != Side::Bid) { continue; }
        if (static_cast<double>(o.price) > fv_px) { ids.push_back(o.id); }
    }
    return ids;
}

/// P20: Step 8 reprices a Fresh resting pace bid only when the desired
/// (tightened) price beats the UNTIGHTENED price at all AND beats the larger
/// of the resting and untightened prices by min_improve_bps, and the offer is
/// at least min_age peak blocks old.  A base-ladder move alone never reprices.
[[nodiscard]] inline bool should_reprice_bid(Mojo resting_px, Mojo desired_px, Mojo untightened_px,
                                             BlockHeight age, std::uint32_t min_age,
                                             double min_improve_bps) noexcept
{
    if (resting_px <= 0 || desired_px <= 0 || untightened_px <= 0) { return false; }
    if (!(std::isfinite(min_improve_bps) && min_improve_bps > 0.0)) { return false; }
    if (age < min_age) { return false; }
    if (desired_px <= untightened_px) { return false; }
    return static_cast<double>(desired_px)
        >= std::max(static_cast<double>(resting_px), static_cast<double>(untightened_px))
               * (1.0 + min_improve_bps / 1e4);
}

/// P21: Step 9f may take an offer that SPENDS a pace-Active asset only at or
/// below its fair value and within the remaining budget.  Every input must be
/// finite; price, units, remaining and fair value must be > 0.
[[nodiscard]] inline bool take_allowed(double take_price, double fv_price, double take_units,
                                       double remaining_units) noexcept
{
    if (!std::isfinite(take_price) || !std::isfinite(fv_price) || !std::isfinite(take_units)
        || !std::isfinite(remaining_units)) {
        return false;
    }
    if (!(take_price > 0.0) || !(take_units > 0.0) || !(remaining_units > 0.0)) { return false; }
    if (!(fv_price > 0.0)) { return false; }
    if (take_price > fv_price) { return false; }
    return take_units <= remaining_units;
}

// ===========================================================================
// P25 -- the wallet balance refresh's gates and backoff (engine glue inputs)
// ===========================================================================

/// P25a: refresh_pace_balances sends no wallet RPC unless pace is on, no
/// engine mode that skips Step 8 is active (dry run, wallet circuit, watchdog,
/// GUI or breaker pause, cancel-all in flight or draining, flash crash, XCH
/// recovery) -- the balances only feed a plan that could not post -- and the
/// wallet has not failed since the last success of Step 2 or Step 8
/// (wallet_consecutive_failures == 0).  Step 8's own wallet-sync and
/// fee-budget checks are not inputs: while either holds Step 8 back, the
/// refresh still runs, bounded by P25c's per-asset backoff.
[[nodiscard]] constexpr bool pace_refresh_gates_open(const RefreshGates& g) noexcept
{
    return g.pace_enabled && !g.dry_run && !g.wallet_circuit_open && !g.watchdog_fired
        && g.wallet_consecutive_failures == 0u && !g.gui_pause && !g.breaker_pause
        && !g.cancel_all_inflight && !g.cancel_all_draining && g.flash_crash_normal && !g.xch_recovery;
}

/// P25b: the refresh age h = max(1, max_balance_age_blocks / 2).  An entry
/// refreshed every h blocks stays fresh.  One failed attempt at age h is
/// retried at age 2h, and the refresh runs before that heartbeat's
/// evaluation, so the heartbeats in between see an age of at most
/// 2h - 1 <= max_age, for every max_age >= 1.  So, while heartbeats do not
/// skip heights, a single transient failure never lets a maintained balance
/// go stale.  A skipped height, or a second failure in a row, can: pace then
/// Holds until a refresh succeeds, which fails safe.
[[nodiscard]] constexpr std::uint32_t pace_refresh_age_blocks(std::uint32_t max_balance_age_blocks) noexcept
{
    return std::max<std::uint32_t>(1u, max_balance_age_blocks / 2u);
}

/// P25c: one asset's refresh is due when its cached entry is missing,
/// unvalidated or at least `refresh_age` blocks old -- and this refresh has
/// not attempted the asset in the last `refresh_age` blocks, whatever that
/// attempt's outcome.  So a failing, timing-out or malformed wallet reply
/// costs at most one RPC per asset per `refresh_age` blocks.  A height
/// regression counts as zero elapsed on both clocks.
[[nodiscard]] constexpr bool pace_refresh_due(bool cached, bool validated, BlockHeight as_of, bool attempted,
                                              BlockHeight last_attempt, BlockHeight now,
                                              std::uint32_t refresh_age) noexcept
{
    const bool stale = !cached || !validated || sat_sub_blocks(now, as_of) >= refresh_age;
    const bool backed_off = attempted && sat_sub_blocks(now, last_attempt) < refresh_age;
    return stale && !backed_off;
}

// ===========================================================================
// P22, P23 -- the heartbeat decision
// ===========================================================================

/// P22: one heartbeat's decision for every pace asset.  Disabled: everything
/// empty, memory included, so a live disable clears the latch.  Per asset K:
///   1. value the targeted portfolio (P4) and check every touching pair's
///      fills query.  Any gap is DataUnavailable: an Active asset HOLDS (latch
///      kept, ramp paused, a hold plan for every enabled touching pair); an
///      inactive one stays inert with its memory cleared;
///   2. hysteresis (P5); an inactive or conflicting asset clears its memory;
///   3. the ramp advances by the saturating block delta only while posting
///      runs, capped at one window;
///   4. excess (P6), progress over every CONFIGURED touching pair, enabled or
///      not (P7, P8), XCH headroom (P9), budget (P10), tightening (P11);
///   5. an enabled touching pair that is not quote-valid or has no usable fair
///      value gets a hold plan; the other enabled pairs split the resting cap
///      and the remaining budget equally and plan tiers (P12, P13).
/// The returned memory and remaining maps are COMPLETE: the caller assigns
/// them wholesale.
inline PaceDecision decide(const PaceInputs& in)
{
    PaceDecision out{};
    if (!in.params.enabled) { return out; }
    const PaceParams& prm = in.params;
    for (const std::string& key : prm.assets) {
        const auto mit = in.memory.find(key);
        const AssetMemory mem = (mit != in.memory.end()) ? mit->second : AssetMemory{};
        AssetDecision dec{};
        dec.key = key;

        std::vector<const PairInput*> touching;
        for (const PairInput& p : in.pairs) {
            if (p.base_key == key || p.quote_key == key) { touching.push_back(&p); }
        }
        const Valuation val = value_portfolio(in.holdings, key, in.now, prm.max_balance_age_blocks,
                                              prm.max_fv_sigma_bps);
        bool fills_ok = true;
        for (const PairInput* p : touching) {
            if (!p->fills_ok) { fills_ok = false; }
        }
        const HoldingInput* managed_holding = nullptr;
        for (const HoldingInput& h : in.holdings) {
            if (h.key == key) { managed_holding = &h; break; }
        }

        if (!val.ok || !fills_ok || managed_holding == nullptr) {
            if (mem.active) {
                out.memory[key] = AssetMemory{true, mem.ramp_blocks, in.now};   // Hold: the ramp pauses
                out.remaining_units[key] = 0.0;
                for (const PairInput* p : touching) {
                    if (!p->enabled) { continue; }
                    PairPlan held{};
                    held.managed = true;
                    held.hold = true;
                    held.asset = key;
                    out.pairs[p->name] = held;
                }
                dec.status = PaceStatus::Hold;
            } else {
                out.memory[key] = AssetMemory{};
                out.remaining_units[key] = 0.0;
                dec.status = PaceStatus::DataUnavailable;
            }
            out.assets.push_back(dec);
            continue;
        }

        const Activation act = decide_activation(mem.active, val.share, managed_holding->target,
                                                 managed_holding->tol, prm.enter_tol_mult, prm.exit_tol_mult);
        dec.share = val.share;
        dec.enter_level = act.enter_level;
        dec.exit_level = act.exit_level;
        if (!act.config_ok || !act.active) {
            out.memory[key] = AssetMemory{};
            out.remaining_units[key] = 0.0;
            dec.status = act.config_ok ? PaceStatus::Inactive : PaceStatus::ConfigConflict;
            out.assets.push_back(dec);
            continue;
        }

        std::uint32_t ramp = 0u;
        if (mem.active) {
            const std::uint64_t advance = in.ramp_running ? static_cast<std::uint64_t>(sat_sub_blocks(in.now, mem.last_block)) : 0u;
            ramp = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                kPaceWindowBlocks, static_cast<std::uint64_t>(mem.ramp_blocks) + advance));
        }
        out.memory[key] = AssetMemory{true, ramp, in.now};

        const double excess = excess_native_units(val.managed_xch, val.total_xch, act.exit_level,
                                                  val.managed_xch_per_unit);

        std::vector<std::pair<BlockHeight, FlowUnits>> rows;
        std::size_t unmatched = 0;
        for (const FillRow& f : in.fills) {
            const PairInput* fill_pair = nullptr;
            for (const PairInput* p : touching) {
                if (p->name == f.pair_name) { fill_pair = p; break; }
            }
            if (fill_pair == nullptr) {
                // A fill naming a pair outside the inputs is ignored; it is
                // counted when that pair's name touches K.
                std::string legs = f.pair_name;
                for (char& c : legs) {
                    if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
                }
                const std::size_t slash = legs.find('/');
                if (slash != std::string::npos
                    && (legs.compare(0, slash, key) == 0
                        || legs.compare(slash + 1, std::string::npos, key) == 0)) {
                    ++unmatched;
                }
                continue;
            }
            const bool managed_is_base = (fill_pair->base_key == key);
            const FlowUnits fu = f.is_taker
                ? classify_taker_fill(managed_is_base, f.we_bought_base, f.base_delta_mojos,
                                      f.quote_delta_mojos, fill_pair->base_mpu, fill_pair->quote_mpu)
                : classify_maker_fill(managed_is_base, f.side_lower, f.size_mojos, f.price_mojos,
                                      fill_pair->base_mpu, fill_pair->quote_mpu);
            rows.emplace_back(f.block_height, fu);
        }
        const Progress progress = accumulate_progress(rows, in.now, prm.horizon_blocks);

        const HoldingInput* xch_holding = nullptr;
        for (const HoldingInput& h : in.holdings) {
            if (h.key == "XCH") { xch_holding = &h; break; }
        }
        double xch_value = 0.0;
        if (const auto xit = val.xch_by_key.find("XCH"); xit != val.xch_by_key.end()) { xch_value = xit->second; }
        const double headroom = (xch_holding == nullptr)
            ? std::numeric_limits<double>::infinity()
            : acquired_headroom_units(xch_holding->targeted, xch_value, val.total_xch,
                                      xch_holding->target, xch_holding->tol, val.managed_xch_per_unit);

        std::vector<const PairInput*> eligible;
        for (const PairInput* p : touching) {
            if (!p->enabled) { continue; }
            if (!p->quote_valid || !p->fv_ok) {
                PairPlan blip{};
                blip.managed = true;
                blip.hold = true;
                blip.asset = key;
                out.pairs[p->name] = blip;
                continue;
            }
            eligible.push_back(p);
        }

        const Budget budget = compute_budget(excess, progress, prm.horizon_blocks, prm.max_resting_frac, headroom);
        const double asset_remaining = std::max(0.0, std::min(budget.remaining, headroom));
        const std::uint32_t n = static_cast<std::uint32_t>(eligible.size());
        const double tighten = budget.exhausted
            ? 0.0
            : compute_tighten_bps(budget.budget_window, excess, progress.sold_window, ramp,
                                  prm.tighten_step_bps, prm.tighten_max_bps);

        for (const PairInput* p : eligible) {
            const OfferSizeBand band = effective_offer_size_band(p->xch_base, p->min_offer_override,
                                                                 prm.global_min_offer_units,
                                                                 prm.global_max_offer_units);
            const double cap = budget.resting_cap / static_cast<double>(n);
            const double rem = asset_remaining / static_cast<double>(n);
            const TierPlan tp = plan_tiers(cap, p->fv_price, p->base_mpu, prm.min_tier_units, prm.max_tier_units,
                                           band, prm.max_tiers, p->side_tier_count);
            PairPlan plan{};
            plan.managed = true;
            plan.asset = key;
            plan.tiers = tp.tiers;
            plan.tier_size_base_mojos = tp.tier_size_base_mojos;
            plan.min_tier_base_mojos = tp.min_tier_base_mojos;
            plan.pool_base_mojos = tp.tier_size_base_mojos * static_cast<Mojo>(tp.tiers);
            plan.resting_cap_units = cap;
            plan.remaining_units = rem;
            plan.tighten_bps = tighten;
            plan.fv_price = p->fv_price;
            plan.fv_sigma_bps = p->fv_sigma_bps;
            plan.band = band;
            plan.side_tier_count = p->side_tier_count;
            plan.eligible_pairs = n;
            if (tp.conflict) { dec.band_conflict = true; }
            out.pairs[p->name] = plan;
        }

        out.remaining_units[key] = asset_remaining;
        dec.status = budget.exhausted ? PaceStatus::Exhausted : PaceStatus::Active;
        dec.excess_units = excess;
        dec.budget_window_units = budget.budget_window;
        dec.sold_window_units = progress.sold_window;
        dec.reduced_horizon_units = progress.reduced_horizon;
        dec.increased_horizon_units = progress.increased_horizon;
        dec.remaining_units = asset_remaining;
        dec.resting_cap_units = budget.resting_cap;
        dec.headroom_units = headroom;
        dec.tighten_bps = tighten;
        dec.ramp_blocks = ramp;
        dec.eligible_pairs = n;
        dec.rows_ignored = progress.rows_ignored + unmatched;
        out.assets.push_back(dec);
    }
    return out;
}

/// P23: after a Step 9f take spent a pace asset, shrink this pair's plan to
/// the asset's new remaining budget before Step 8 posts.  It never grows:
/// tiers and tier size are the minimum of the plan and a fresh P13 plan.
inline void replan_after_take(PairPlan& plan, double asset_remaining_units, const PaceParams& p,
                              std::int64_t base_mpu)
{
    plan.remaining_units = asset_remaining_units
                         / static_cast<double>(std::max<std::uint32_t>(1u, plan.eligible_pairs));
    const TierPlan tp = plan_tiers(std::min(plan.resting_cap_units, plan.remaining_units), plan.fv_price,
                                   base_mpu, p.min_tier_units, p.max_tier_units, plan.band, p.max_tiers,
                                   plan.side_tier_count);
    plan.tiers = std::min(plan.tiers, tp.tiers);
    plan.tier_size_base_mojos = plan.tiers > 0u ? std::min(plan.tier_size_base_mojos, tp.tier_size_base_mojos) : Mojo{0};
    plan.pool_base_mojos = plan.tier_size_base_mojos * static_cast<Mojo>(plan.tiers);
}

}  // namespace xop::strategy::pace

#endif  // XOP_STRATEGY_PACE_CONTROLLER_HPP
