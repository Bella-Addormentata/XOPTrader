// topup_take.hpp -- The warp rebalancer's Dexie buys, booked as takes
// ([MILLIETH-TOPUP 2026-10-24]).
//
// WHY THIS EXISTS.  When the Base hot wallet runs low on ETH and no
// wmilliETH.b is left to unwrap, the GUI's warp rebalancer can take one
// Dexie ask for wmilliETH.b, paid in XCH (gui/services/warp/dexie_buy.py).
// That trade moves two of the engine's wallets, and the engine did not make
// it: without a booking, the ledger invariant would see both balances move
// for no reason and absorb each as an "adjust" (and alert).  The GUI records
// every take it makes in warp_jobs.db's dexie_takes table and marks it
// confirmed once the wallet's trade record says so; the engine books each
// confirmed row exactly like one of its own takes -- base, quote and fee
// legs under "take:<trade_id>" -- plus a taker_fills row for the history.
//
// A take is a TRADE, not external capital: nothing here touches Net
// Deposits.  The tracker saw neither leg, so the engine re-arms Step 11's
// one-shot wallet reconcile for both assets when it books one.
//
// Compliant with:
//   ISO/IEC 5055  -- pure functions, overflow-checked, no UB
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
//                    (tests/test_topup_take.cpp)

#ifndef XOP_ACCOUNTING_TOPUP_TAKE_HPP
#define XOP_ACCOUNTING_TOPUP_TAKE_HPP

#include "xop/accounting/bridge_ingest.hpp"
#include "xop/types.hpp"

#include <cstdint>
#include <limits>
#include <string>

namespace xop::accounting {

/// One confirmed row of the GUI's dexie_takes table, as read by the engine.
struct TopupTakeRow {
    std::int64_t id{0};
    std::string  trade_id;     ///< Lowercase hex, no 0x (GUI-normalised).
    std::string  asset_id;     ///< The CAT bought.
    Mojo         cat_mojos{0}; ///< Received.
    Mojo         xch_mojos{0}; ///< Paid to the maker.
    Mojo         fee_mojos{0}; ///< Our network fee.
    std::string  created_at;   ///< ISO-8601 UTC: when the take was submitted.
    /// Chain block the trade confirmed in (the wallet's confirmed_at_index),
    /// 0 when the GUI did not record one.  History only (taker_fills).
    std::uint64_t confirmed_height{0};
    std::string   confirmed_at; ///< When the GUI saw it confirm; "" if not.
};

/// Whether a take's leg belongs AFTER an asset's ledger opening -- i.e. the
/// opening did not already read the post-take balance.  By TIME, against
/// the SUBMIT time, the bridge flows' lower-bound rule: the opening's stamp
/// is its balance OBSERVATION time, while no height is tied to that
/// observation (the opening row's block is the startup block, taken before
/// the reads), so a block comparison could double-count a take that landed
/// in between.  An opening observed after the submit -- the trade may or
/// may not be inside it -- skips that leg: under-booking degrades to the
/// divergence control's adjustment, double-booking would never heal.
[[nodiscard]] inline bool take_leg_after_opening(
    const std::string& created_at, const std::string& opening_time) noexcept
{
    return iso_strictly_after(created_at, opening_time);
}

/// [review] Whether a take's fee still falls inside the fee budget's rolling
/// window at block @p now: confirmed at or after `now - window`, the cutoff
/// FeeTracker::prune applies.  An unknown height (0) counts -- for a budget
/// the conservative reading.
[[nodiscard]] inline bool take_fee_in_window(std::uint64_t confirmed_height,
                                             std::uint64_t now,
                                             std::uint32_t window) noexcept
{
    if (confirmed_height == 0) return true;
    const std::uint64_t cutoff = now > window ? now - window : 0;
    return confirmed_height >= cutoff;
}

/// The ledger shape of a take, from our side.
struct TopupTakeLegs {
    bool        valid{false};
    std::string event_id;          ///< "take:<trade_id>", as engine takes.
    Mojo        base_delta{0};     ///< + CAT received.
    Mojo        quote_delta{0};    ///< - XCH paid.
    Mojo        fee_delta{0};      ///< - XCH fee (0 when none).
    /// XCH mojos per CAT display unit (the taker_fills price convention for
    /// a 1e3-per-unit base, 1e12-per-unit quote pair), rounded.
    Mojo        price_mojos{0};
};

[[nodiscard]] inline bool is_lower_hex64(const std::string& s) noexcept
{
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

/// Classify a row; valid=false (book nothing) for a malformed trade or
/// asset id, a non-positive quantity, a negative fee, or amounts whose sum
/// or price would overflow.
[[nodiscard]] inline TopupTakeLegs classify_topup_take(
    const TopupTakeRow& r) noexcept
{
    TopupTakeLegs out{};
    if (!is_lower_hex64(r.trade_id) || !is_lower_hex64(r.asset_id)) return out;
    if (r.cat_mojos <= 0 || r.xch_mojos <= 0 || r.fee_mojos < 0) return out;
    constexpr Mojo kMax = std::numeric_limits<Mojo>::max();
    if (r.xch_mojos > kMax - r.fee_mojos) return out;
    // [review] price = xch * 1000 / cat, rounded half up, by LONG DIVISION:
    // exact whenever the PRICE fits, never forming xch * 1000 (a buy the
    // GUI allows -- 10,000 XCH for 3,000,000 mojos -- overflows that
    // product though its price, ~3.3e12, does not).  The whole part, then
    // three decimal digits from the remainder (rem < cat, so rem * 10 fits
    // while cat <= kMax / 10 -- some 9e14 CAT units, beyond any real take).
    if (r.cat_mojos > kMax / 10) return out;
    const Mojo whole = r.xch_mojos / r.cat_mojos;
    if (whole > (kMax - 1000) / 1000) return out;   // the price itself
    Mojo price = whole * 1000;
    Mojo rem   = r.xch_mojos % r.cat_mojos;
    for (Mojo place = 100; place > 0; place /= 10) {
        rem *= 10;
        price += (rem / r.cat_mojos) * place;
        rem %= r.cat_mojos;
    }
    out.price_mojos = price + (rem >= r.cat_mojos - rem ? 1 : 0);
    out.event_id    = "take:" + r.trade_id;
    out.base_delta  = r.cat_mojos;
    out.quote_delta = -r.xch_mojos;
    out.fee_delta   = -r.fee_mojos;
    out.valid       = true;
    return out;
}

}  // namespace xop::accounting

#endif  // XOP_ACCOUNTING_TOPUP_TAKE_HPP
