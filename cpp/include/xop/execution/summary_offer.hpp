#ifndef XOP_EXECUTION_SUMMARY_OFFER_HPP
#define XOP_EXECUTION_SUMMARY_OFFER_HPP
// ---------------------------------------------------------------------------
// summary_offer.hpp -- one of our offers, read back from the wallet's own
// summary of it.
//
// [ADOPT-PRICE 2026-11-23] Operator-approved (Dorkmo).
//
// WHY IT EXISTS
// -------------
// The engine reads an offer back from the wallet when it has no record of
// it: an orphan at boot (OfferManager::evaluate_orphan), or a live offer
// State does not hold (OfferManager::try_parse_wallet_offer -- the reconcile
// mirror, and an offer seen terminal that is live after all).  The summary
// gives the RAW MOJOS of the two legs, and both readers priced the offer as
//
//     quote_mojos / base_mojos * kMojosPerXch
//
// without the two assets' denominations.  That is the engine's price only on
// a pair whose assets have the same decimals.  On XCH/wUSDC.b (1e12 and 1e3
// mojos a unit) it is 1e9 times too small; on wmilliETH.b/XCH it is 1e9
// times too large, past what a Mojo holds.
//
// WHAT THAT DID, from the live offer_log (38 rows on the XCH pairs,
// 2026-04-08 to 2026-11-20): an adopted offer was judged against the market
// at a price nine orders of magnitude off, and cancelled for it.  At the
// engine start of 2026-11-20 22:34Z an ask of 22.8 XCH at 1.011 wUSDC.b was
// adopted at price 1011 and cancelled twelve minutes later as
// "crossed_mid(97112%)"; an XCH/DBX bid at 88.27 was adopted at 88274.
// Every restart paid cancel fees for offers that were priced well, and
// until then the exposure sums counted an adopted bid's spend at a
// billionth of it.
//
// OfferManager::parse_settled_fill already had the right formula (its
// comment: "this codebase has hit 1e9-class unit errors twice").  The two
// readers were the third and fourth copies, and the ones without the
// denominations.  There is now ONE: price_from_legs.
// ---------------------------------------------------------------------------

#include "xop/config.hpp"
#include "xop/execution/unique_request.hpp"
#include "xop/peg_registry.hpp"
#include "xop/types.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>

namespace xop::execution {

/// The engine's price of a trade, from the raw mojos of its two legs:
///
///   price [pseudo] = quote_units_per_base_unit * kMojosPerXch
///                  = quote_mojos * base_mpu * kMojosPerXch
///                    / (base_mojos * quote_mpu)
///
/// the inverse of xop::quote_mojos_for (types.hpp), so
/// quote_mojos_for(base_mojos, price, base_mpu, quote_mpu) gives quote_mojos
/// back.  In double: the numerator reaches 1e36, and a price's true precision
/// is bounded by mojo granularity anyway.  nullopt when a leg or a
/// denomination is not positive, or when the price does not fit a Mojo or
/// rounds to nothing.
[[nodiscard]] inline std::optional<Mojo> price_from_legs(Mojo base_mojos, Mojo quote_mojos,
                                                         Mojo base_mpu, Mojo quote_mpu) noexcept
{
    if (base_mojos <= 0 || quote_mojos <= 0 || base_mpu <= 0 || quote_mpu <= 0) {
        return std::nullopt;
    }
    const double price = static_cast<double>(quote_mojos) * static_cast<double>(base_mpu)
                         * static_cast<double>(kMojosPerXch)
                         / (static_cast<double>(base_mojos) * static_cast<double>(quote_mpu));
    const std::optional<Mojo> rounded = to_mojo_checked(price);
    if (!rounded.has_value() || *rounded <= 0) {
        return std::nullopt;
    }
    return rounded;
}

/// One of our offers on a configured pair, as a wallet trade record's
/// summary describes it.
struct SummaryOffer {
    std::string pair_name;
    Side        side{Side::Bid};
    /// Base mojos: what an ask offers, what a bid asks for.
    Mojo        size{0};
    /// The engine's price (price_from_legs); 0 when it cannot be reckoned.
    Mojo        price{0};
    /// What the offer asks to be paid, exactly as the summary gives it
    /// (unique_request.hpp); nullopt when that amount is not positive.
    std::optional<RequestedAmount> request;
};

/// Reads a summary's two legs (`offered`: what we give, `requested`: what we
/// get; asset id -> raw mojos) as an offer on one of `pairs` (a container of
/// pair name -> PairConfig).  Giving base and getting quote is an ask; giving
/// quote and getting base is a bid.  The first pair in the container's order
/// whose two assets are the two legs wins.  nullopt when no pair matches.
template <typename Pairs>
[[nodiscard]] std::optional<SummaryOffer> offer_from_summary(
    const std::unordered_map<std::string, Mojo>& offered,
    const std::unordered_map<std::string, Mojo>& requested, const Pairs& pairs)
{
    for (const auto& [name, pair] : pairs) {
        const auto offered_base    = offered.find(pair.base_asset_id);
        const auto offered_quote   = offered.find(pair.quote_asset_id);
        const auto requested_base  = requested.find(pair.base_asset_id);
        const auto requested_quote = requested.find(pair.quote_asset_id);
        const bool is_ask = offered_base != offered.end() && requested_quote != requested.end();
        const bool is_bid = offered_quote != offered.end() && requested_base != requested.end();
        if (!is_ask && !is_bid) {
            continue;
        }
        SummaryOffer shape;
        shape.pair_name = name;
        shape.side = is_ask ? Side::Ask : Side::Bid;
        const Mojo base_mojos  = is_ask ? offered_base->second : requested_base->second;
        const Mojo quote_mojos = is_ask ? requested_quote->second : offered_quote->second;
        shape.size = base_mojos;
        shape.price = price_from_legs(base_mojos, quote_mojos, pair.base_mojos_per_unit,
                                      pair.quote_mojos_per_unit)
                          .value_or(0);
        shape.request = request_from_summary(pair, shape.side, requested);
        return shape;
    }
    return std::nullopt;
}

/// How an offer's price stands against the market's mid.
struct PriceStanding {
    /// |price - mid| / mid; 0 when there is no mid.
    double deviation{0.0};
    /// A bid above the mid overpays; an ask below it undersells.
    bool   adverse{false};
};

[[nodiscard]] inline PriceStanding price_standing(Side side, Mojo price, Mojo mid) noexcept
{
    const double mid_d = static_cast<double>(mid);
    const double signed_dev =
        (mid_d > 0.0) ? (static_cast<double>(price) - mid_d) / mid_d : 0.0;
    PriceStanding standing;
    standing.deviation = std::abs(signed_dev);
    standing.adverse = (side == Side::Bid) ? (signed_dev > 0.0) : (signed_dev < 0.0);
    return standing;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_SUMMARY_OFFER_HPP
