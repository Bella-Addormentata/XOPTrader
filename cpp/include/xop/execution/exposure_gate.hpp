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

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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

// ---------------------------------------------------------------------------
// [S71 2026-09-20] ONE exposure verdict for both Step 8 sites.
//
// Step 8 asks the question above twice a cycle: BEFORE posting ("would the
// resting offers plus these new tiers breach the reserve?" -- suppress the
// side) and AFTER ("do the resting offers alone breach it?" -- cancel the
// least competitive, reason exposure_floor_rebalance).  In the 14 days to
// 2026-09-20 the second site cancelled 528 offers, 42% of every cancel the
// bot made, at an average age of 27 blocks: it was cancelling what the first
// site had just approved.
//
// WHY THEY DISAGREED, reconstructed from engine.log 2026-09-13 00:35-00:39
// (XCH/DBX asks, reserve 0.1 XCH, five 1-XCH asks resting):
//
//   00:35:09  spendable 6.0398, pending+new 6.0  -> 0.0398 < 0.1, SUPPRESSED
//             (1.2079 XCH of pending_change was in flight from a cancel)
//   00:36:20  the change has confirmed, so spendable is 6.0398 + 1.2079 =
//             7.2477 (inferred: the approving cycle logs no balance, and the
//             pending_change line is gone), pending+new 6.0
//             -> 1.2477 >= 0.1, APPROVED.  Ask tier 5 posted, 1 XCH.
//   00:36:47  the wallet funded that ask from the 1.2079 coin and LOCKED THE
//             WHOLE COIN: spendable is 6.0398 again (logged), pending now 6.0
//             -> 0.0398 < 0.1, BREACH.  Tier 5 cancelled, 24 s old.
//   00:37:02  its cancel puts the coin back in flight (pending_change is the
//             same 1.2079 less the 20,030-mojo fee); when that confirms the
//             loop runs again -- next post 00:39:03, next cancel 00:39:29.
//             267 such cancels on 2026-09-13 alone.
//
// Both sites ran the same arithmetic on the same kind of input, and the input
// is the defect.  The header comment above believes spendable "already
// excludes coins locked in unconfirmed transactions"; in chia 2.7.4 it ALSO
// excludes every coin locked by a resting offer (wallet_state_manager.py
// get_spendable_coins_for_wallet subtracts TradeManager.get_locked_coins()).
// So `spendable - pending` counts a resting offer twice -- once as the locked
// coin already missing from spendable, once as its size -- and posting an
// offer moves the verdict by the size of the COIN the wallet chose, which
// nothing here can predict.  Pending change moves it the same way.
//
// THE UNIFIED INPUTS do not move when an offer locks a coin or a cancel's
// change is in flight:
//
//   owned    the wallet's unconfirmed_wallet_balance: confirmed, minus the
//            removals of our in-flight transactions, plus their additions.
//            Creating an offer is not a transaction, so it leaves this alone;
//            an in-flight cancel nets to its fee; a real outflow (a take we
//            made) shows at once.
//   resting  what EVERY resting offer that spends this asset would spend --
//            all pairs, because owned is wallet-wide and three pairs sell XCH.
//
// owned - resting - planned is then literally the balance left if everything
// fills, which is the question the rule was written to ask.  Approving a
// post moves its size from `planned` to `resting` and changes nothing else,
// so what the first site approves the second cannot condemn; only a real
// change (a fill elsewhere, a fee, a take) can.
//
// Funding is a separate question and keeps its spendable test: new tiers are
// also suppressed when spendable cannot cover them plus the reserve.
//
// Legacy mode runs through the same function and reproduces the two old
// call sites exactly (pinned in test_exposure_gate.cpp), so the switch is a
// true rollback.
// ---------------------------------------------------------------------------

enum class ExposureVerdict {
    Ok,             ///< post freely
    SuppressNew,    ///< do not add exposure; leave resting offers alone
    CancelResting,  ///< resting exposure itself breaches: free `need_to_free`
};

struct ExposureInputs {
    Mojo owned_mojos{0};          ///< unified: unconfirmed_wallet_balance
    Mojo spendable_mojos{0};      ///< free coins now (both rules)
    Mojo resting_spend_mojos{0};  ///< unified: all pairs; legacy: this pair
    Mojo planned_spend_mojos{0};  ///< new tiers; 0 when only resting is judged
    Mojo reserve_mojos{0};        ///< balance that must survive every fill
};

struct ExposureDecision {
    ExposureVerdict verdict{ExposureVerdict::Ok};
    /// CancelResting only: spend to free so the projection regains the FULL
    /// reserve (not merely the hysteresis floor -- stopping at the floor
    /// would leave the side suppressed and one fee from cancelling again).
    Mojo need_to_free{0};
    /// The projected balance the verdict was reached on, for the log.
    Mojo projected_mojos{0};
};

/// reserve x (1 - hysteresis), clamped.  Integer-exact at the ends, which is
/// where it matters: 0 must return the reserve itself and 1 must return 0.
[[nodiscard]] constexpr Mojo exposure_cancel_floor(
    Mojo reserve_mojos, double hysteresis_pct) noexcept
{
    if (reserve_mojos <= 0) return 0;
    if (!(hysteresis_pct > 0.0)) return reserve_mojos;   // also NaN
    if (hysteresis_pct >= 1.0)   return 0;
    const double floor_d =
        static_cast<double>(reserve_mojos) * (1.0 - hysteresis_pct);
    const Mojo floor_m = static_cast<Mojo>(floor_d);      // in [0, reserve]
    return floor_m > reserve_mojos ? reserve_mojos : floor_m;
}

/// The Step 8 exposure verdict.  Both sites call this; the post-hoc site
/// passes planned_spend_mojos = 0.
///
/// @param unified         strategy.exposure_rule == unified.
/// @param hysteresis_pct  unified only; see exposure_cancel_floor.
[[nodiscard]] constexpr ExposureDecision decide_exposure(
    bool                  unified,
    const ExposureInputs& in,
    double                hysteresis_pct) noexcept
{
    ExposureDecision d{};
    const Mojo reserve = in.reserve_mojos < 0 ? Mojo{0} : in.reserve_mojos;

    if (!unified) {
        // The two legacy call sites, verbatim: spendable - (pending + new).
        d.projected_mojos = projected_balance_after_fills(
            in.spendable_mojos, in.resting_spend_mojos, in.planned_spend_mojos);
        if (d.projected_mojos < reserve) {
            if (in.planned_spend_mojos > 0) {
                d.verdict = ExposureVerdict::SuppressNew;
            } else {
                d.verdict      = ExposureVerdict::CancelResting;
                d.need_to_free = reserve - d.projected_mojos;
            }
        }
        return d;
    }

    // Resting exposure first, on its own: is what ALREADY rests a breach deep
    // enough to pay a cancel fee for?
    const Mojo resting_only = projected_balance_after_fills(
        in.owned_mojos, in.resting_spend_mojos, /*planned_spend_mojos=*/0);
    d.projected_mojos = projected_balance_after_fills(
        in.owned_mojos, in.resting_spend_mojos, in.planned_spend_mojos);

    if (resting_only < exposure_cancel_floor(reserve, hysteresis_pct)) {
        d.verdict         = ExposureVerdict::CancelResting;
        d.need_to_free    = reserve - resting_only;
        d.projected_mojos = resting_only;
        return d;
    }
    if (d.projected_mojos < reserve) {
        d.verdict = ExposureVerdict::SuppressNew;
        return d;
    }
    // Funding: the new tiers must come out of coins that are free NOW.
    if (in.planned_spend_mojos > 0
        && projected_balance_after_fills(in.spendable_mojos, 0,
                                         in.planned_spend_mojos) < reserve) {
        d.verdict = ExposureVerdict::SuppressNew;
    }
    return d;
}

/// One resting offer, reduced to what the asset-wide projection needs: the
/// asset it SPENDS (base for an ask, quote for a bid) and how much of it.
struct RestingSpend {
    std::string asset_id;
    Mojo        spend_mojos{0};
    bool        cancel_pending{false};
};

/// Unified rule: what every resting offer that spends @p asset_id would
/// spend, across ALL pairs -- `owned` is wallet-wide, and XCH/DBX, XCH/BYC and
/// XCH/wUSDC.b asks all sell the same XCH.  A cancel_pending offer is left
/// out, exactly as the legacy per-pair sum leaves it out: counting it would
/// make a cancel the rule has just SENT still read as unfreed exposure, and
/// the next cycle would cancel another offer for the same shortfall.
[[nodiscard]] inline Mojo resting_spend_on_asset(
    const std::vector<RestingSpend>& offers,
    std::string_view                 asset_id) noexcept
{
    Mojo total = 0;
    for (const auto& o : offers) {
        if (o.cancel_pending || o.spend_mojos <= 0 || o.asset_id != asset_id) {
            continue;
        }
        const Mojo headroom = std::numeric_limits<Mojo>::max() - total;
        total = (o.spend_mojos > headroom) ? std::numeric_limits<Mojo>::max()
                                           : total + o.spend_mojos;
    }
    return total;
}

/// Whether a resting offer may be an exposure-cancel candidate.  Legacy takes
/// every offer, as it always did; unified spares one younger than
/// @p min_age_blocks, so a breach that appears right after a post suppresses
/// the NEXT post instead of buying back the last one.
[[nodiscard]] constexpr bool exposure_cancel_candidate(
    bool          unified,
    std::uint64_t created_block,
    std::uint64_t current_block,
    std::uint64_t min_age_blocks) noexcept
{
    if (!unified) return true;
    return current_block >= created_block
        && current_block - created_block >= min_age_blocks;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_EXPOSURE_GATE_HPP
