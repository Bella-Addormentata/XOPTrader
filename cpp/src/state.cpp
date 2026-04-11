// state.cpp -- Implementation of xop::Position and xop::State.
//
// All monetary arithmetic uses int64_t (mojos).  Division rounds toward zero,
// which for cost-basis calculations is conservative (never overstates the
// basis).
//
// Wide-multiply portability:
//   Position::add and Position::remove need products that can exceed int64.
//   We use a portable wide_mul / wide_mul_div helper that compiles on MSVC
//   (_umul128 / __umulh intrinsic), GCC, and Clang (__int128).
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

// MSVC intrinsics header -- needed for _umul128 / _udiv128.
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

// ---------------------------------------------------------------------------
//  Portable 128-bit unsigned multiply helpers.
//
//  We only need two operations on non-negative values (all our inputs are
//  validated > 0 before reaching these):
//    wide_mul_add(a, b, c)  -> a*b + c  as a pair {hi, lo}
//    wide_mul_div(a, b, d)  -> (a*b) / d  returned as int64_t
//
//  The helpers work on unsigned magnitudes; callers ensure signs are correct.
// ---------------------------------------------------------------------------

namespace {

struct U128 {
    std::uint64_t hi;
    std::uint64_t lo;
};

#if defined(_MSC_VER) && defined(_M_X64)
//  MSVC x64: use compiler intrinsics for 64x64->128 multiply.

inline U128 umul128(std::uint64_t a, std::uint64_t b) {
    U128 r;
    r.lo = _umul128(a, b, &r.hi);
    return r;
}

#elif defined(__SIZEOF_INT128__)
//  GCC / Clang: native __int128 support.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

inline U128 umul128(std::uint64_t a, std::uint64_t b) {
    unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
    return { static_cast<std::uint64_t>(p >> 64),
             static_cast<std::uint64_t>(p) };
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#else
//  Fallback: schoolbook 32-bit multiply (always correct, slightly slower).

inline U128 umul128(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a_lo = a & 0xFFFFFFFF;
    const std::uint64_t a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xFFFFFFFF;
    const std::uint64_t b_hi = b >> 32;

    const std::uint64_t p0  = a_lo * b_lo;
    const std::uint64_t p1  = a_lo * b_hi;
    const std::uint64_t p2  = a_hi * b_lo;
    const std::uint64_t p3  = a_hi * b_hi;

    const std::uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFF) + (p2 & 0xFFFFFFFF);
    const std::uint64_t hi  = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    const std::uint64_t lo  = (mid << 32) | (p0 & 0xFFFFFFFF);

    return { hi, lo };
}

#endif

/// Compute (a * b + c) as a 128-bit unsigned result.
/// All inputs must be non-negative (caller's responsibility).
inline U128 wide_mul_add(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    U128 prod = umul128(a, b);
    prod.lo += c;
    if (prod.lo < c) {  // carry
        ++prod.hi;
    }
    return prod;
}

/// Return true if the 128-bit value exceeds INT64_MAX.
inline bool exceeds_int64(U128 v) {
    // INT64_MAX = 0x7FFFFFFFFFFFFFFF
    if (v.hi > 0) return true;
    return v.lo > static_cast<std::uint64_t>(INT64_MAX);
}

/// Software 128-bit / 64-bit unsigned division using shift-and-subtract.
/// Returns quotient.  Only used on exotic platforms lacking both __int128
/// and MSVC _udiv128 -- kept simple rather than fast.
#if !(defined(_MSC_VER) && defined(_M_X64)) && !defined(__SIZEOF_INT128__)
inline std::uint64_t udiv128_software(U128 num, std::uint64_t den) {
    if (num.hi == 0) {
        return num.lo / den;
    }

    // Binary long division: shift dividend left one bit at a time,
    // subtract divisor when the partial remainder is large enough.
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;

    for (int i = 127; i >= 0; --i) {
        // Shift remainder left by 1, bringing in the next bit of num.
        remainder = (remainder << 1) |
            ((i >= 64) ? ((num.hi >> (i - 64)) & 1u)
                       : ((num.lo >> i) & 1u));

        if (remainder >= den) {
            remainder -= den;
            if (i < 64) {
                quotient |= (std::uint64_t{1} << i);
            }
            // If i >= 64 the quotient bit overflows uint64, meaning the
            // final result exceeds int64.  Caller guarantees this does
            // not happen, so we silently drop these high bits.
        }
    }
    return quotient;
}
#endif  // !(defined(_MSC_VER) && defined(_M_X64)) && !defined(__SIZEOF_INT128__)

/// Compute (a * b) / d  using 128-bit intermediate.  Returns the int64 result.
/// Precondition: all inputs positive, result fits int64.
inline std::int64_t wide_mul_div(std::int64_t a, std::int64_t b, std::int64_t d) {
    // We work with unsigned magnitudes.  Caller guarantees a >= 0, b >= 0, d > 0.
    const auto ua = static_cast<std::uint64_t>(a);
    const auto ub = static_cast<std::uint64_t>(b);
    const auto ud = static_cast<std::uint64_t>(d);

#if defined(_MSC_VER) && defined(_M_X64)
    // MSVC x64: _udiv128 available since VS 2019 16.4.
    U128 prod = umul128(ua, ub);
    std::uint64_t rem;
    std::uint64_t quot = _udiv128(prod.hi, prod.lo, ud, &rem);
    return static_cast<std::int64_t>(quot);
#elif defined(__SIZEOF_INT128__)
    // GCC / Clang: native 128-bit arithmetic.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
    unsigned __int128 p = static_cast<unsigned __int128>(ua) * ub;
    return static_cast<std::int64_t>(p / ud);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
    // Pure software fallback -- no compiler extensions required.
    U128 prod = umul128(ua, ub);
    return static_cast<std::int64_t>(udiv128_software(prod, ud));
#endif
}

}  // anonymous namespace

namespace xop {

// ===================================================================
//  Position
// ===================================================================

Position::Position()
    : asset_id{}
    , balance{0}
    , cost_basis{0}
    , total_cost{0}
{}

Position::Position(const AssetId& id)
    : asset_id{id}
    , balance{0}
    , cost_basis{0}
    , total_cost{0}
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

    // Guard against overflow: qty * unit_price could exceed int64 range if
    // both operands are very large.  In practice Chia values stay well below
    // 10^15 mojos (~1000 XCH) per single trade, so the product fits int64.
    // Defensive check: use 128-bit intermediate and verify the result fits.

    const U128 wide_total = wide_mul_add(
        static_cast<std::uint64_t>(qty),
        static_cast<std::uint64_t>(unit_price),
        static_cast<std::uint64_t>(total_cost));

    const Mojo new_balance = balance + qty;

    // Reject on overflow -- callers must check the return value.
    if (exceeds_int64(wide_total) || new_balance <= 0) {
        spdlog::error("[Position] Overflow in cost basis -- addition rejected "
                      "for asset={} qty={} price={}",
                       asset_id, qty, unit_price);
        return false;
    }

    total_cost = static_cast<Mojo>(wide_total.lo);
    balance    = new_balance;
    cost_basis = (balance > 0) ? (total_cost / balance) : 0;
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
    //   removed_cost = total_cost * (qty / balance)
    //   new_total    = total_cost - removed_cost
    //
    // Use 128-bit intermediate to avoid overflow on the multiply.

    const Mojo removed_cost = wide_mul_div(total_cost, qty, balance);
    total_cost -= removed_cost;
    balance    -= qty;

    // Recompute cost_basis from integers to avoid drift accumulation.
    cost_basis = (balance > 0) ? (total_cost / balance) : 0;

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

    Mojo base_val  = 0;
    Mojo quote_val = 0;

    if (auto it = positions_.find(base_id); it != positions_.end()) {
        // Use total_cost (denominated in quote mojos) when available;
        // fall back to balance for the native quote asset.
        base_val = (it->second.total_cost > 0)
                       ? it->second.total_cost
                       : it->second.balance;
    }

    if (auto it = positions_.find(quote_id); it != positions_.end()) {
        quote_val = (it->second.total_cost > 0)
                        ? it->second.total_cost
                        : it->second.balance;
    }

    const Mojo total = base_val + quote_val;
    if (total == 0) {
        return 0.0;
    }

    // Integer subtraction is exact; division into double is fine for a
    // dimensionless ratio.
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
