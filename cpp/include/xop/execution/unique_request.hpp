#ifndef XOP_EXECUTION_UNIQUE_REQUEST_HPP
#define XOP_EXECUTION_UNIQUE_REQUEST_HPP
// ---------------------------------------------------------------------------
// unique_request.hpp -- no two live offers ask for the same coin.
//
// [UNIQUE-REQUEST 2026-11-21] Operator-approved (Dorkmo), after a forum tip
// to avoid offers with identical amounts.
//
// WHY IT MATTERS (chia-blockchain 2.7.4 and chia_rs, read from source)
// --------------------------------------------------------------------
// - A taker who combines several offers in one spend bundle pays every
//   request for an asset from ONE settlement coin: Offer.to_valid_spend
//   (chia/wallet/trading/offer.py) puts all the notarized payments for an
//   asset in the solution of the first offered coin.
// - Consensus rejects a spend with two CREATE_COINs of the same puzzle hash
//   and amount: ErrorCode::DuplicateOutput (chia_rs, chia-consensus
//   conditions.rs; NewCoin compares puzzle hash and amount only).
// - The wallet reuses an offer's requested-payment address across offers:
//   TradeManager asks the action scope for a puzzle hash, and the derivation
//   it draws is committed as used only for a pushed transaction
//   (wallet_action_scope.py), which an offer never is. Seen live on
//   2026-11-21: the 39 payments of 38 open offers went to 31 addresses, one
//   of them shared by three offers posted within three minutes.
//
// So two live offers asking for the same amount of the same asset may ask
// for the very same coin, and then no taker can take them together. Each
// still fills on its own. The ladders invite it: the XCH bids of every
// XCH/* pair use the same tier sizes, and the BYC/wUSDC.b bids each ask for
// exactly 1 BYC.
//
// THE RULE: before a tier is posted, what it asks for is moved by the fewest
// mojos that make it differ from every live offer's request. A bid asks for
// one more base mojo per step -- never less, so a tier never drops under its
// minimum size (a trillionth of an XCH, or a thousandth of a 3-decimal CAT).
// An ask asks for one quote mojo less per step, its price lowered just
// enough: in the taker's favour. Resting offers are read back from State
// with the same formula build_offer_dict uses, so an offer posted after a
// nudge is seen as what it really asks for.
// ---------------------------------------------------------------------------

#include "xop/config.hpp"
#include "xop/types.hpp"

#include <cmath>
#include <set>
#include <string>
#include <utility>

namespace xop::execution {

/// What an offer asks to be paid: (asset id, mojos of it).
using RequestedAmount = std::pair<std::string, Mojo>;

/// The requested side of a tier exactly as OfferManager::build_offer_dict
/// asks for it: a bid asks for `size` mojos of the base asset, an ask for
/// floor(quote_mojos_for(size, price)) mojos of the quote asset.
[[nodiscard]] inline RequestedAmount requested_amount(const PairConfig& pair, Side side,
                                                      Mojo price, Mojo size)
{
    if (side == Side::Bid) {
        return {pair.base_asset_id, size};
    }
    const double quote = std::floor(quote_mojos_for(
        static_cast<double>(size), static_cast<double>(price),
        static_cast<double>(pair.base_mojos_per_unit),
        static_cast<double>(pair.quote_mojos_per_unit)));
    return {pair.quote_asset_id, static_cast<Mojo>(quote)};
}

/// Moves `tier` the fewest mojos needed so that what it asks for is in none
/// of `taken` (see the header comment for the direction). Returns false, with
/// the tier unchanged, when `max_steps` steps are not enough -- the tier is
/// then posted as computed: a request two offers share only stops a taker
/// combining them.
[[nodiscard]] inline bool make_request_unique(const PairConfig& pair, TierQuote& tier,
                                              const std::set<RequestedAmount>& taken,
                                              int max_steps = 32)
{
    const RequestedAmount want = requested_amount(pair, tier.side, tier.price, tier.size);
    if (taken.count(want) == 0) {
        return true;
    }
    if (tier.side == Side::Bid) {
        for (int k = 1; k <= max_steps; ++k) {
            if (taken.count({want.first, want.second + k}) == 0) {
                tier.size += k;
                return true;
            }
        }
        return false;
    }
    // An ask: the lowest price whose floored quote is `target`, checked with
    // requested_amount itself so floating-point rounding cannot disagree.
    const double per_price_unit =
        static_cast<double>(tier.size) * static_cast<double>(pair.quote_mojos_per_unit)
        / (static_cast<double>(pair.base_mojos_per_unit) * static_cast<double>(kMojosPerXch));
    if (!(per_price_unit > 0.0) || !std::isfinite(per_price_unit)) {
        return false;
    }
    for (int k = 1; k <= max_steps; ++k) {
        const Mojo target = want.second - k;
        if (target <= 0) {
            return false;
        }
        if (taken.count({want.first, target}) != 0) {
            continue;
        }
        Mojo price = static_cast<Mojo>(std::ceil(static_cast<double>(target) / per_price_unit));
        for (int nudge = 0; nudge < 4 && price > 0 && price < tier.price; ++nudge) {
            const Mojo got = requested_amount(pair, Side::Ask, price, tier.size).second;
            if (got == target) {
                tier.price = price;
                return true;
            }
            price += (got < target) ? 1 : -1;
        }
    }
    return false;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_UNIQUE_REQUEST_HPP
