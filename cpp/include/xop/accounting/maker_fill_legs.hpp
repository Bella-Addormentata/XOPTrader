// maker_fill_legs.hpp -- Every leg of a confirmed MAKER fill -- base, quote
// and the XCH creation fee -- booked into the InventoryTracker
// ([FILL-LEGS 2026-09-13]).
//
// WHY THIS EXISTS.  Engine::step_process_fills booked a confirmed maker fill
// into inventory_ through the pair's BASE asset only.  The quote leg (the DBX
// or BYC an ask received, or a bid spent) and the offer's XCH creation fee
// never reached the tracker, while the ledger posted all three legs for the
// same fill (Engine::post_ledger_fill).  Two tracker consumers were wrong as a
// result, in opposite directions:
//
//   * Engine::compute_portfolio_equity_usd walks every tracker record.  An ask
//     removed the XCH it sold but never added the proceeds, so equity read too
//     LOW; a bid added XCH but never removed the quote it spent, so equity read
//     too HIGH and could ratchet the drawdown peak (the pre-trade seed and
//     Step 13 both take a max()).  On 2026-09-11 fourteen 1-XCH XCH/DBX asks
//     left 1,137.997 DBX unbooked -- about $20 at the breach block's mark --
//     and Step 13 tripped MAX DRAWDOWN on the heartbeat that booked the last.
//   * InventoryTracker::inventory_ratio reads the quote record's quantity, so
//     every ratio consumer (A-S reservation offset, skew and cross-pair skew,
//     Step 8's ratio-mode hysteresis that scales Step 7's pools, the book
//     tactician, the circuit breaker, snapshots) saw the book lean further
//     toward base after asks, and less after bids, than it really did.
//
// OfferManager's State books both legs (detect_fills), which is why the
// dashboards looked right while the tracker was wrong.  State skips XCH quote
// legs; this code deliberately does NOT copy that exclusion.
//
// Everything that decides WHAT to book lives here, where ctest can reach it
// (TODO S36: no test constructs an Engine).  The engine resolves the base USD
// price and the factor's trust, then hands the Fill and its PairConfig over
// whole -- there are no positional quantities, denominations or asset ids to
// swap at the call site.
//
// PRICING RULE.  Tracker basis is a USD pseudo-price: USD per display unit in
// kMojosPerXch fixed point.  The base leg keeps the price the engine already
// resolved (to_usd_pseudo, else the asset's own price).  The quote leg is
// priced at the USD value SURRENDERED, at the fill's own exchange rate:
//
//     quote_usd_pseudo = base_usd_pseudo * kMojosPerXch / fill.price
//
// When the base was priced through the pair factor f (base_usd_pseudo =
// llround(fill.price * f)) that is f * 1e12 -- exactly the quote branch of
// Engine::asset_usd_pseudo_price (XCH/BYC at par: 1e12; XCH/DBX: usd_per_xch /
// mid * 1e12; CAT/XCH: usd_per_xch * 1e12).  Rounding error is at most
// 0.5e12 / fill.price pseudo-mojos.
//
// So both legs enter at equal USD COST.  That is a statement about COST BASIS
// only.  Equity marks holdings at live prices and never reads basis, so a
// booked fill still moves equity by its edge against the marks:
//
//     quote_qty * quote_mark - base_qty * base_mark - fee * xch_mark
//
// There is no independent quote-asset price fallback: it would break the cost
// identity, and when f <= 0 the quote has no route through this pair anyway.
//
// UNPRICED LEGS.  If the base resolves to no USD price the quote leg is
// unpriced too (usd_pseudo_price 0, booked through record_fill_unpriced).  A
// placeholder price is never substituted: record_buy's sentinel branch would
// re-mark the ENTIRE holding at that price and clear the sentinel flag,
// permanently destroying the basis with no way for Step 11's mark-at-first-
// observation upgrade to repair it.
//
// UNTRUSTED FACTOR.  Engine::to_usd_pseudo does not consult
// quote_usd_factor_trusted, and for XCH/DBX its factor derives from the pair's
// own ungraded mid.  The implied quote price IS that factor, and a priced buy
// on a sentinel record REPLACES the whole holding's basis and clears the flag
// for good.  So when the factor is not trusted the quote leg books unpriced
// and Step 11 repairs it later from a graded mark.  (The base leg's pricing is
// unchanged by this file.)
//
// NO-LOSS BYPASS.  The engine builds the tracker with no_loss_constraint on.
// A confirmed fill must always change tracked inventory -- never-sell-at-loss
// is a PRE-trade control -- so every priced sale here passes enforce_no_loss
// as false and can fail only on a missing or insufficient holding.  For the
// quote leg that matters: a bid that spends DBX bought at $0.025 at an implied
// $0.02 would otherwise be refused, leaving the quote holding overstated --
// this very defect.  Priced and unpriced sales draw a holding down with the
// same arithmetic; the price only has to be positive.
//
// SENTINELS.  A sale never reads or writes basis_is_seed_sentinel, so a
// sentinel quote record stays repairable after a bid spends from it.  A PRICED
// buy on a sentinel record replaces its basis (correct only because the implied
// price is a real valuation); an UNPRICED buy costs the lot at the existing
// basis and keeps or sets the flag.
//
// FEE LEG.  Booked unpriced as a sale of XCH, so the fee is expensed at basis
// and the XCH basis is preserved.  OfferManager::parse_settled_fill reads only
// the offered/requested amounts, so the fee is not already inside fill.size.
// The engine warns on a refused fee leg and never alerts.
//
// PRECONDITIONS AND LATENT EFFECTS.
//   * No asset may be the BASE of one enabled pair and the QUOTE of another.
//     Quote legs re-average the quote asset's basis, and the no-loss floor,
//     position aging, realized-P&L basis and MTM are all keyed on base assets.
//     Today's enabled set (XCH/BYC, XCH/DBX) satisfies this; re-enabling a
//     CAT/XCH pair or a BYC-base pair does not.
//   * Every applied leg stamps the record's last_fill_block/time.  On a CAT/CAT
//     pair the fee leg would reset XCH's position age with no XCH traded.
//   * Legs are applied independently, base then quote then fee.  A refused leg
//     does not stop the next one: the fill happened on-chain.
//
// Compliant with:
//   ISO/IEC 5055  -- pure decision functions, checked double->Mojo conversion
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
//                    (tests/test_maker_fill_legs.cpp)

#ifndef XOP_ACCOUNTING_MAKER_FILL_LEGS_HPP
#define XOP_ACCOUNTING_MAKER_FILL_LEGS_HPP

#include "xop/config.hpp"
#include "xop/peg_registry.hpp"
#include "xop/risk/inventory.hpp"
#include "xop/types.hpp"

#include <optional>
#include <string_view>

namespace xop::accounting {

/// Asset of the fee leg.  The offer-creation fee is always paid in XCH; the
/// ledger's fee leg uses the same id (Engine::post_ledger_fill).
inline constexpr const char* kFillFeeAssetId = "xch";

/// One leg of a confirmed maker fill, as the tracker should book it.
struct FillInventoryLeg {
    bool applies{false};       ///< false => no InventoryTracker call at all.
    bool is_buy{false};        ///< true => the quantity ENTERS the asset.
    Mojo qty_mojos{0};         ///< > 0 when applies; 0 whenever it does not.
    Mojo usd_pseudo_price{0};  ///< > 0 => priced (record_buy / record_sell);
                               ///< 0 => record_fill_unpriced.
};

/// All three legs of one confirmed maker fill.
struct MakerFillInventoryLegs {
    FillInventoryLeg base{};
    FillInventoryLeg quote{};
    FillInventoryLeg fee{};     ///< Always XCH, always unpriced.
    bool quote_invalid{false};  ///< size > 0 but no quote quantity could be
                                ///< derived: price or a denomination <= 0,
                                ///< or the quantity is outside the Mojo range.
};

/// Decide what the tracker books for one confirmed maker fill.
///
/// @param fill                  The confirmed fill (side, size, price, fee).
/// @param pc                    Its pair: asset ids and denominations.
/// @param base_usd_pseudo       The base leg's USD pseudo-price as the engine
///                              resolved it; <= 0 means no valuation.
/// @param quote_factor_trusted  Engine::quote_usd_factor_trusted(pc).  When
///                              false the quote leg is booked unpriced.
[[nodiscard]] inline MakerFillInventoryLegs maker_fill_inventory_legs(
    const Fill& fill, const PairConfig& pc, Mojo base_usd_pseudo,
    bool quote_factor_trusted) noexcept
{
    MakerFillInventoryLegs out{};
    const bool we_bought_base = (fill.side == Side::Bid);
    const Mojo base_price = (base_usd_pseudo > 0) ? base_usd_pseudo : Mojo{0};

    if (fill.size > 0) {
        out.base.applies = true;
        out.base.is_buy = we_bought_base;
        out.base.qty_mojos = fill.size;
        out.base.usd_pseudo_price = base_price;

        if (fill.price <= 0 || pc.base_mojos_per_unit <= 0
            || pc.quote_mojos_per_unit <= 0) {
            out.quote_invalid = true;
        } else if (const auto q = to_mojo_checked(quote_mojos_for(
                       static_cast<double>(fill.size),
                       static_cast<double>(fill.price),
                       static_cast<double>(pc.base_mojos_per_unit),
                       static_cast<double>(pc.quote_mojos_per_unit)))) {
            // A zero quote is not a leg: the ledger skips a zero delta too.
            if (*q > 0) {
                out.quote.applies = true;
                out.quote.is_buy = !we_bought_base;
                out.quote.qty_mojos = *q;
                if (base_price > 0 && quote_factor_trusted) {
                    out.quote.usd_pseudo_price = to_mojo_checked(
                        static_cast<double>(base_price)
                        * static_cast<double>(kMojosPerXch)
                        / static_cast<double>(fill.price)).value_or(Mojo{0});
                }
            }
        } else {
            out.quote_invalid = true;  // outside the Mojo range
        }
    }

    if (fill.fee_mojos > 0) {
        out.fee.applies = true;
        out.fee.qty_mojos = fill.fee_mojos;
    }
    return out;
}

/// Result of applying the legs.  A flag is false only when the tracker
/// refused that leg (missing or insufficient holding, or an overflow).
struct MakerFillApplyResult {
    bool base_ok{true};
    bool quote_ok{true};
    bool fee_ok{true};
};

namespace detail {

/// Book one leg: unpriced -> record_fill_unpriced; priced buy -> record_buy;
/// priced sale -> record_sell with the no-loss rule bypassed, because a
/// confirmed fill is not a pre-trade decision.
[[nodiscard]] inline bool apply_fill_leg(InventoryTracker& inv,
                                         const AssetId& asset,
                                         const FillInventoryLeg& leg,
                                         BlockHeight block, Timestamp ts)
{
    if (!leg.applies) {
        return true;
    }
    if (leg.usd_pseudo_price <= 0) {
        return inv.record_fill_unpriced(asset, leg.qty_mojos, leg.is_buy,
                                        block, ts);
    }
    if (leg.is_buy) {
        inv.record_buy(asset, leg.qty_mojos, leg.usd_pseudo_price, block, ts);
        return true;
    }
    return inv.record_sell(asset, leg.qty_mojos, leg.usd_pseudo_price, block,
                           ts, /*enforce_no_loss=*/false);
}

}  // namespace detail

/// Apply the legs to the tracker: base, then quote, then fee.  Every leg is
/// attempted even when an earlier one was refused.  Reads the asset ids from
/// the PairConfig itself.  Not noexcept: the tracker can throw bad_alloc,
/// which the engine's per-fill try/catch reports.
[[nodiscard]] inline MakerFillApplyResult apply_maker_fill_legs(
    InventoryTracker& inv, const PairConfig& pc,
    const MakerFillInventoryLegs& legs, BlockHeight block, Timestamp ts)
{
    MakerFillApplyResult r{};
    r.base_ok  = detail::apply_fill_leg(inv, pc.base_asset_id, legs.base, block, ts);
    r.quote_ok = detail::apply_fill_leg(inv, pc.quote_asset_id, legs.quote, block, ts);
    r.fee_ok   = detail::apply_fill_leg(inv, AssetId{kFillFeeAssetId}, legs.fee, block, ts);
    return r;
}

/// Which legs the engine reports as REJECTED (an error log plus an
/// ExposureBreach alert).  A refused fee leg is not a rejection -- it only
/// warns.  An underivable quote quantity IS one: quote moved on-chain and the
/// tracker could not follow it.
///
/// @return "base", "quote", "base+quote", or empty when nothing was rejected.
[[nodiscard]] constexpr std::string_view rejected_fill_legs(
    const MakerFillInventoryLegs& legs,
    const MakerFillApplyResult& applied) noexcept
{
    const bool base_failed = !applied.base_ok;
    const bool quote_failed = !applied.quote_ok || legs.quote_invalid;
    if (base_failed && quote_failed) {
        return "base+quote";
    }
    if (base_failed) {
        return "base";
    }
    if (quote_failed) {
        return "quote";
    }
    return {};
}

}  // namespace xop::accounting

#endif  // XOP_ACCOUNTING_MAKER_FILL_LEGS_HPP
