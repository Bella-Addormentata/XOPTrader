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
};

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
    // price = xch * 1000 / cat, rounded half up, without overflowing the
    // product or the rounding step: from the quotient and remainder.
    if (r.xch_mojos > kMax / 1000) return out;
    const Mojo scaled = r.xch_mojos * 1000;
    const Mojo quotient  = scaled / r.cat_mojos;
    const Mojo remainder = scaled % r.cat_mojos;
    out.price_mojos = quotient + (remainder >= r.cat_mojos - remainder ? 1 : 0);
    out.event_id    = "take:" + r.trade_id;
    out.base_delta  = r.cat_mojos;
    out.quote_delta = -r.xch_mojos;
    out.fee_delta   = -r.fee_mojos;
    out.valid       = true;
    return out;
}

}  // namespace xop::accounting

#endif  // XOP_ACCOUNTING_TOPUP_TAKE_HPP
