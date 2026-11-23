#ifndef XOP_EXECUTION_UNIQUE_REQUEST_HPP
#define XOP_EXECUTION_UNIQUE_REQUEST_HPP
// ---------------------------------------------------------------------------
// unique_request.hpp -- no two live offers ask for the same coin.
//
// [UNIQUE-REQUEST 2026-11-22] Operator-approved (Dorkmo), after a forum tip
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
//   the derivation it draws is committed as used only for a pushed
//   transaction (wallet_action_scope.py), which an offer never is. Seen live
//   on 2026-11-21: the 39 payments of 38 open offers went to 31 addresses,
//   one of them shared by three offers posted within three minutes.
//
// So two live offers asking for the same amount of the same asset may ask
// for the very same coin, and then no taker can take them together. Each
// still fills on its own. The ladders invite it: the XCH bids of every
// XCH/* pair use the same tier minimums (10, 8.2, ... 1 XCH), and the
// BYC/wUSDC.b bids each ask for exactly 1 BYC.
//
// THE RULE, AND WHY EACH MOVE IS THE WAY IT IS
// --------------------------------------------
// Before Step 8 posts a tier, what it asks for is moved by the fewest mojos
// that make it differ from what every resting offer asks for. Step 8 then
// posts and records the moved tier, and the request itself is kept with the
// offer in State and in offer_log (PendingOffer::requested_*), so it is known
// to the mojo also after a restart and for an offer read back from the
// wallet, whose price is rounded from its two legs and need not run back
// into the exact amount.
//
// The engine compares a resting offer with its plan exactly in two places,
// and a move must trip neither, or the offer is cancelled and reposted every
// cycle (review of the first version, #252):
//   - [TIERMIN] a resting offer under its tier's minimum is retired
//     ("tier_min_undersized"), and Step 7's dust filter drops a tier under
//     the pair's own minimum size: a bid never goes under min_bid_size;
//   - [PACE] the resting bids of a pace-managed pair are held to its pool to
//     the mojo (pace::select_resting_to_cancel), so such a bid never grows.
// And no move may spend more or sell for less than Step 7 allowed:
//   - an ASK asks for more of the quote asset, its price raised just enough
//     (within max_ask_move_bps): never under a no-loss or peg floor;
//   - a BID first asks for less of the base asset (and pays less): never
//     over a budget. Only where its minimum forbids that does it ask for
//     more. That is free while what it PAYS stays the same to the mojo --
//     true of every XCH-base bid, where one base mojo is far under one mojo
//     of the quote asset.
//     [2026-11-23] Operator-approved (Dorkmo): offers at their minimum
//     follow the rule too. Where more is not free -- a 1 BYC bid of
//     BYC/wUSDC.b, where each BYC mojo costs about a mojo of the quote asset
//     -- the bid pays for the extra mojos at its own price. Only out of what
//     Step 8's exposure check found spare above the quote asset's reserve,
//     held to the side's own share of that asset and to the coin it may lock
//     (bid_extra_pay; execution::spare_above_reserve and within_side_caps),
//     so no budget is passed, no other pair's allocation is spent and the
//     reserve is not touched; and by at most max_bid_growth_bps of its size.
// A tier no allowed move clears is posted as computed: two offers sharing a
// request only stops a taker combining them.
// ---------------------------------------------------------------------------

#include "xop/config.hpp"
#include "xop/execution/take_sizing.hpp"
#include "xop/peg_registry.hpp"
#include "xop/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xop::execution {

/// What an offer asks to be paid: (asset id, mojos of it).
using RequestedAmount = std::pair<std::string, Mojo>;

/// A tier's quote leg before rounding, as OfferManager::build_offer_dict
/// computes it.
[[nodiscard]] inline double quote_leg_mojos(const PairConfig& pair, Mojo price, Mojo size) noexcept
{
    return quote_mojos_for(static_cast<double>(size), static_cast<double>(price),
                           static_cast<double>(pair.base_mojos_per_unit),
                           static_cast<double>(pair.quote_mojos_per_unit));
}

/// [review #255] A rounded quote leg as a Mojo. build_offer_dict casts it
/// directly, which is undefined for a leg no Mojo holds. Step 7 plans no such
/// tier, but these helpers are also run on the records of resting offers, and
/// one adopted from the wallet before v0.10.57 has a price worked out from
/// raw mojos (1e9 times too large on wmilliETH.b/XCH). So here it is defined:
/// nothing for a leg that is not positive, the largest Mojo for one past it.
[[nodiscard]] inline Mojo leg_as_mojo(double rounded_leg) noexcept
{
    if (!(rounded_leg > 0.0)) {
        return 0;  // also NaN
    }
    return to_mojo_checked(rounded_leg).value_or(std::numeric_limits<Mojo>::max());
}

/// What a bid pays, in quote mojos: build_offer_dict rounds it up.
[[nodiscard]] inline Mojo bid_offered_quote(const PairConfig& pair, Mojo price, Mojo size) noexcept
{
    return leg_as_mojo(std::ceil(quote_leg_mojos(pair, price, size)));
}

/// [review #256] What a bid costs as the engine's own books count it: the
/// exact integer ceiling that Step 7's funding fit, Step 8's exposure check
/// and every per-side cap are reckoned with (execution::quote_cost_for_ask).
/// bid_offered_quote() above is what the wallet is asked to pay, rounded
/// from a double as build_offer_dict rounds it, and on a product that is a
/// whole number of mojos the two can differ by one. 0 when it cannot be
/// reckoned (a size or price that is not positive).
[[nodiscard]] inline Mojo bid_planned_cost(const PairConfig& pair, Mojo price, Mojo size) noexcept
{
    return quote_cost_for_ask(BaseMojos{size}, price, BaseMpu{pair.base_mojos_per_unit},
                              QuoteMpu{pair.quote_mojos_per_unit})
        .v;
}

/// [review #256] What moving a bid from `size` to `moved_size` costs beyond
/// what was planned for it. Nothing when neither reckoning goes up: not what
/// the wallet is asked to pay, and not the engine's exact count. Else the
/// dearer of the two after the move, less the exact count before it -- the
/// figure the budgets and caps were set with, so a move charged this way
/// stays inside them by either rounding.
[[nodiscard]] inline Mojo bid_extra_cost(const PairConfig& pair, Mojo price, Mojo size,
                                         Mojo moved_size) noexcept
{
    const Mojo planned = bid_planned_cost(pair, price, size);
    const Mojo planned_moved = bid_planned_cost(pair, price, moved_size);
    const Mojo pays_moved = bid_offered_quote(pair, price, moved_size);
    if (planned_moved <= planned && pays_moved <= bid_offered_quote(pair, price, size)) {
        return 0;
    }
    const Mojo dearer = std::max(planned_moved, pays_moved);
    return dearer > planned ? dearer - planned : Mojo{0};
}

/// The requested side of a tier exactly as build_offer_dict asks for it: a
/// bid asks for `size` mojos of the base asset, an ask for the quote leg
/// rounded down.
[[nodiscard]] inline RequestedAmount requested_amount(const PairConfig& pair, Side side,
                                                      Mojo price, Mojo size)
{
    if (side == Side::Bid) {
        return {pair.base_asset_id, size};
    }
    return {pair.quote_asset_id, leg_as_mojo(std::floor(quote_leg_mojos(pair, price, size)))};
}

/// [review #255] The request of an offer read back from the wallet: the
/// asset on the requested side of `pair`, with the mojos the wallet's own
/// summary gives for it (`requested`: asset id -> mojos). nullopt when the
/// summary does not name that asset with a positive amount.
///
/// An adopted offer's PendingOffer::price is rounded from the summary's two
/// legs (summary_offer.hpp), so requested_amount() run on it can land a mojo
/// off.  Before v0.10.57 it was far worse: the price left out the two
/// assets' denominations, so an ask of 1 XCH for 997 quote mojos was adopted
/// at price 997 and read as asking for nothing.
[[nodiscard]] inline std::optional<RequestedAmount> request_from_summary(
    const PairConfig& pair, Side side, const std::unordered_map<std::string, Mojo>& requested)
{
    const std::string& asset = (side == Side::Bid) ? pair.base_asset_id : pair.quote_asset_id;
    const auto it = requested.find(asset);
    if (it == requested.end() || it->second <= 0) {
        return std::nullopt;
    }
    return RequestedAmount{asset, it->second};
}

/// [review #255] The request of ONE wallet offer made of several tiers of
/// one side (OfferManager::post_merged_side, batch mode): the asset on the
/// requested side of `pair`, with `payload_total`, the amount the merged
/// payload asks for. That is the total of its tiers' requests, which none of
/// them asks for alone, so requested_amount() gives it for no record of that
/// trade. nullopt when the payload asks for no positive amount.
[[nodiscard]] inline std::optional<RequestedAmount> merged_request(const PairConfig& pair,
                                                                  Side side, Mojo payload_total)
{
    if (payload_total <= 0) {
        return std::nullopt;
    }
    return RequestedAmount{(side == Side::Bid) ? pair.base_asset_id : pair.quote_asset_id,
                           payload_total};
}

/// Writes `request` onto `offer` (PendingOffer::requested_*); nothing when
/// there is none to write.
inline void note_request(PendingOffer& offer, const std::optional<RequestedAmount>& request)
{
    if (request.has_value()) {
        offer.requested_asset = request->first;
        offer.requested_mojos = request->second;
    }
}

/// [review #255] What a resting offer asks for: the amount recorded with it,
/// when there is one; else its price and size run through
/// requested_amount(), which is exact for a single-tier offer this engine
/// built (a record from before requests were kept).
/// nullopt when neither is possible (`pair` is null: its pair is no longer
/// configured).
[[nodiscard]] inline std::optional<RequestedAmount> request_of(const PendingOffer& offer,
                                                              const PairConfig* pair)
{
    if (!offer.requested_asset.empty() && offer.requested_mojos > 0) {
        return RequestedAmount{offer.requested_asset, offer.requested_mojos};
    }
    if (pair == nullptr) {
        return std::nullopt;
    }
    const RequestedAmount worked_out =
        requested_amount(*pair, offer.side, offer.price, offer.size);
    if (worked_out.second <= 0) {
        return std::nullopt;  // nothing a new offer could ask for too
    }
    return worked_out;
}

/// [review #255] The smallest size a bid may be moved down to: its tier's
/// minimum this cycle when its side posts tier minimums (`tier_min`), else
/// the pair's own minimum offer size (config.hpp effective_min_offer_units,
/// which Step 7's dust filter enforces). Never under one mojo.
[[nodiscard]] inline Mojo min_bid_size_mojos(const PairConfig& pair, double global_min_units,
                                             std::optional<Mojo> tier_min) noexcept
{
    if (tier_min.has_value()) {
        return *tier_min > 1 ? *tier_min : Mojo{1};
    }
    const double units = effective_min_offer_units(pair, global_min_units);
    const std::optional<Mojo> mojos = ceil_mojo_checked(
        units * static_cast<double>(pair.base_mojos_per_unit));
    return mojos.has_value() && *mojos > 1 ? *mojos : Mojo{1};
}

/// How far one tier's request may move (see the header comment).
struct RequestBounds {
    /// A bid's size never goes under this (min_bid_size_mojos).
    Mojo   min_bid_size{1};
    /// False on a pace-managed pair, whose resting bids are held to a pool.
    bool   bid_may_grow{true};
    /// The most quote mojos a bid may pay beyond what it was sized to pay, to
    /// ask for more of the base asset: what its side has spare above the
    /// reserve. 0 = it grows only while it pays the same.
    Mojo   bid_extra_pay{0};
    /// The most a bid's size may grow where it pays for the growth.
    double max_bid_growth_bps{100.0};
    /// The most an ask's price may rise.
    double max_ask_move_bps{25.0};
    /// The most mojos a request moves either way.
    int    max_steps{32};
};

enum class RequestMove {
    None,    ///< Nothing else asks for it: the tier is as computed.
    Moved,   ///< The tier now asks for an amount nothing else asks for.
    Shared,  ///< No allowed move clears it: the tier is as computed.
};

/// Moves `tier` within `bounds` so that what it asks for is in none of
/// `taken`. The tier is unchanged unless Moved is returned.
[[nodiscard]] inline RequestMove make_request_unique(const PairConfig& pair, TierQuote& tier,
                                                     const std::set<RequestedAmount>& taken,
                                                     const RequestBounds& bounds = {})
{
    constexpr Mojo kMax = std::numeric_limits<Mojo>::max();
    const RequestedAmount want = requested_amount(pair, tier.side, tier.price, tier.size);
    if (taken.count(want) == 0) {
        return RequestMove::None;
    }
    if (tier.side == Side::Bid) {
        // Less of the base asset, down to its minimum.
        for (int k = 1; k <= bounds.max_steps; ++k) {
            const Mojo size = tier.size - k;
            if (size < bounds.min_bid_size || size <= 0) {
                break;
            }
            if (taken.count({want.first, size}) == 0) {
                tier.size = size;
                return RequestMove::Moved;
            }
        }
        // More of it: free while what the bid costs does not change, and past
        // that paid for at its own price, within what its side has spare
        // (bid_extra_pay) and max_bid_growth_bps of its size. The cost is
        // reckoned as the budgets and caps reckon it (bid_extra_cost), and
        // cannot be told of a payment no Mojo holds (leg_as_mojo).
        if (bounds.bid_may_grow && bid_offered_quote(pair, tier.price, tier.size) < kMax) {
            const Mojo may_pay_more = bounds.bid_extra_pay > 0 ? bounds.bid_extra_pay : Mojo{0};
            for (int k = 1; k <= bounds.max_steps && tier.size <= kMax - k; ++k) {
                const Mojo size = tier.size + k;
                if (bid_offered_quote(pair, tier.price, size) >= kMax) {
                    break;
                }
                const Mojo extra = bid_extra_cost(pair, tier.price, tier.size, size);
                if (extra > 0
                    && (extra > may_pay_more
                        || static_cast<double>(k) * 10'000.0
                               > static_cast<double>(tier.size) * bounds.max_bid_growth_bps)) {
                    break;  // every larger size costs at least as much
                }
                if (taken.count({want.first, size}) == 0) {
                    tier.size = size;
                    return RequestMove::Moved;
                }
            }
        }
        return RequestMove::Shared;
    }

    // An ask: the lowest price whose rounded-down quote leg is `target`,
    // checked with requested_amount itself so rounding cannot disagree.
    const double per_price_unit = quote_leg_mojos(pair, 1, tier.size);
    if (!(per_price_unit > 0.0) || !std::isfinite(per_price_unit)) {
        return RequestMove::Shared;
    }
    const double max_price =
        static_cast<double>(tier.price) * (1.0 + bounds.max_ask_move_bps / 10'000.0);
    for (int k = 1; k <= bounds.max_steps && want.second <= kMax - k; ++k) {
        const Mojo target = want.second + k;
        if (taken.count({want.first, target}) != 0) {
            continue;
        }
        // [review #255] Checked: a price this dear may not fit a Mojo at
        // all, and then no dearer one does either.
        const std::optional<Mojo> first =
            ceil_mojo_checked(static_cast<double>(target) / per_price_unit);
        if (!first.has_value()) {
            break;
        }
        Mojo price = *first;
        bool found = false;
        for (int fix = 0; fix < 4 && price > tier.price; ++fix) {
            const Mojo got = requested_amount(pair, Side::Ask, price, tier.size).second;
            if (got == target) {
                // The lowest such price: one unit less must fall short.
                if (requested_amount(pair, Side::Ask, price - 1, tier.size).second == target) {
                    --price;
                    continue;
                }
                found = true;
                break;
            }
            if (got > target || price == kMax) {
                break;  // one price unit moves the request past it
            }
            ++price;
        }
        if (!found) {
            continue;
        }
        if (static_cast<double>(price) > max_price) {
            break;  // every further target is dearer still
        }
        tier.price = price;
        return RequestMove::Moved;
    }
    return RequestMove::Shared;
}

/// What every resting offer asks for (request_of). `pair_of(pair_name)`
/// gives the pair's config, or nullptr when it is no longer configured.
template <typename PairLookup>
[[nodiscard]] std::set<RequestedAmount> requested_by(const std::vector<PendingOffer>& resting,
                                                     PairLookup&& pair_of)
{
    std::set<RequestedAmount> taken;
    for (const PendingOffer& offer : resting) {
        if (const auto request = request_of(offer, pair_of(offer.pair_name))) {
            taken.insert(*request);
        }
    }
    return taken;
}

/// One tier the pass below moved, for the log.
struct RequestChange {
    std::uint8_t tier_index{0};
    Side         side{Side::Bid};
    Mojo         asked_before{0};
    Mojo         asked_now{0};
    /// Quote mojos a bid costs beyond what was planned for it
    /// (bid_extra_cost); 0 for an ask and for a bid that moved for free.
    Mojo         paid_more{0};
};

/// Makes every tier of `tiers` ask for an amount neither `taken` nor an
/// earlier tier of the same call asks for, and adds each tier's request to
/// `taken`. `bounds_of(tier)` gives that tier's bounds. Returns the tiers
/// that moved; `shared`, when given, counts those no allowed move cleared.
///
/// `bid_spare`, when given, is what the pair's bid side may pay beyond its
/// sizing, in quote mojos (execution::within_side_caps of
/// spare_above_reserve). It replaces each bid's bid_extra_pay and is counted
/// down by what each bid pays more, so the bids of one call never pay more
/// than it between them.
template <typename BoundsOf>
std::vector<RequestChange> make_requests_unique(const PairConfig& pair,
                                                std::vector<TierQuote>& tiers,
                                                std::set<RequestedAmount>& taken,
                                                BoundsOf&& bounds_of,
                                                std::size_t* shared = nullptr,
                                                Mojo* bid_spare = nullptr)
{
    std::vector<RequestChange> changes;
    if (shared != nullptr) *shared = 0;
    for (TierQuote& tier : tiers) {
        const Mojo before = requested_amount(pair, tier.side, tier.price, tier.size).second;
        const bool is_bid = (tier.side == Side::Bid);
        RequestBounds bounds = bounds_of(tier);
        if (bid_spare != nullptr) {
            bounds.bid_extra_pay = *bid_spare > 0 ? *bid_spare : Mojo{0};
        }
        const Mojo size_before = tier.size;
        const RequestMove move = make_request_unique(pair, tier, taken, bounds);
        const RequestedAmount now = requested_amount(pair, tier.side, tier.price, tier.size);
        if (move == RequestMove::Moved) {
            Mojo paid_more = 0;
            if (is_bid) {
                // The same reckoning the move was allowed on.
                paid_more = bid_extra_cost(pair, tier.price, size_before, tier.size);
                if (bid_spare != nullptr) {
                    *bid_spare -= paid_more;  // never past it: bid_extra_pay bounds the move
                }
            }
            changes.push_back({tier.tier_index, tier.side, before, now.second, paid_more});
        } else if (move == RequestMove::Shared && shared != nullptr) {
            ++*shared;
        }
        taken.insert(now);
    }
    return changes;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_UNIQUE_REQUEST_HPP
