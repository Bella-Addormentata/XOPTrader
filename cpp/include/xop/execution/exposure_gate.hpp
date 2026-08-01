// exposure_gate.hpp -- Pure side-suppression decision for Step 8 balance
// gating.
//
// [2026-08-01] A wallet with pending_change used to suppress its spend side
// outright.  That was backwards for bid fills: buying base creates pending
// change on the base wallet, which suppressed the ask side, so only bids
// could post, and the next bid fill refreshed the pending change -- a
// self-sustaining bid-only loop (measured: 10 bids / 0 asks overnight on
// XCH/BYC; the same pattern on XCH/wUSDC.b).  Pending change from a buy
// means the engine just ACQUIRED base -- more to sell, not less.
//
// The correct question is whether the side's SPENDABLE balance (which
// already excludes coins locked in unconfirmed transactions) can fund the
// side's committed exposure -- live same-side offers plus any planned new
// ladder -- while keeping the configured reserve intact.  This header holds
// that decision as a pure function so the engine's call sites share one
// definition and the logic is unit-testable in isolation
// (tests/test_exposure_gate.cpp).
//
// Compliant with:
//   ISO/IEC 5055 -- no UB: saturating arithmetic, clamped inputs
//   ISO/IEC 25000 -- single responsibility, documented interface

#ifndef XOP_EXECUTION_EXPOSURE_GATE_HPP
#define XOP_EXECUTION_EXPOSURE_GATE_HPP

#include "xop/types.hpp"

#include <limits>

namespace xop::execution {

/// Projected balance of a side's funding wallet if every live same-side
/// offer plus the planned new ladder filled.  Never negative; inputs are
/// clamped to >= 0 and the committed sum saturates instead of overflowing.
///
/// @param spendable_mojos      Spendable balance of the wallet that funds
///                             this side (in-flight coins already excluded).
/// @param pending_spend_mojos  Sum this side's live offers would spend.
/// @param planned_spend_mojos  Sum the candidate new ladder would spend
///                             (0 when only live offers are being checked).
[[nodiscard]] constexpr Mojo projected_balance_after_fills(
    Mojo spendable_mojos,
    Mojo pending_spend_mojos,
    Mojo planned_spend_mojos) noexcept
{
    if (spendable_mojos < 0)     spendable_mojos     = 0;
    if (pending_spend_mojos < 0) pending_spend_mojos = 0;
    if (planned_spend_mojos < 0) planned_spend_mojos = 0;

    // Saturating add of the two exposure legs.
    Mojo committed = pending_spend_mojos;
    if (planned_spend_mojos
        > std::numeric_limits<Mojo>::max() - committed) {
        committed = std::numeric_limits<Mojo>::max();
    } else {
        committed += planned_spend_mojos;
    }

    return (spendable_mojos > committed) ? (spendable_mojos - committed)
                                         : Mojo{0};
}

/// The Step 8 suppression decision: true when, if every live same-side
/// offer plus the planned new ladder filled, the funding wallet would be
/// left below its reserve -- i.e. spendable genuinely cannot cover
/// (committed exposure + reserve).  The mere existence of pending_change
/// is deliberately NOT an input: spendable already excludes in-flight
/// coins, so it needs no separate gate.
///
/// @param reserve_mojos  Balance that must remain after all fills.
[[nodiscard]] constexpr bool exposure_breaches_reserve(
    Mojo spendable_mojos,
    Mojo pending_spend_mojos,
    Mojo planned_spend_mojos,
    Mojo reserve_mojos) noexcept
{
    if (reserve_mojos < 0) reserve_mojos = 0;
    return projected_balance_after_fills(spendable_mojos,
                                         pending_spend_mojos,
                                         planned_spend_mojos)
           < reserve_mojos;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_EXPOSURE_GATE_HPP
