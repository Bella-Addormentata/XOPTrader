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
// posts and records the moved tier, so State and offer_log hold what the
// offer really asks for, also after a restart.
//
// The engine compares a resting offer with its plan exactly in two places,
// and a move must trip neither, or the offer is cancelled and reposted every
// cycle (review of the first version, #252):
//   - [TIERMIN] a resting offer under its tier's minimum is retired
//     ("tier_min_undersized"), so a bid never goes under min_bid_size;
//   - [PACE] the resting bids of a pace-managed pair are held to its pool to
//     the mojo (pace::select_resting_to_cancel), so such a bid never grows.
// And no move may spend more or sell for less than Step 7 allowed:
//   - an ASK asks for more of the quote asset, its price raised just enough
//     (within max_ask_move_bps): never under a no-loss or peg floor;
//   - a BID first asks for less of the base asset (and pays less): never
//     over a budget. Only where its tier minimum forbids that does it ask
//     for more, and then only while what it PAYS stays the same to the mojo
//     -- true of every XCH-base bid, where one base mojo is far under one
//     mojo of the quote asset.
// A tier no allowed move clears is posted as computed: two offers sharing a
// request only stops a taker combining them.
// ---------------------------------------------------------------------------

#include "xop/config.hpp"
#include "xop/types.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
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

/// What a bid pays, in quote mojos: build_offer_dict rounds it up.
[[nodiscard]] inline Mojo bid_offered_quote(const PairConfig& pair, Mojo price, Mojo size) noexcept
{
    return static_cast<Mojo>(std::ceil(quote_leg_mojos(pair, price, size)));
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
    return {pair.quote_asset_id, static_cast<Mojo>(std::floor(quote_leg_mojos(pair, price, size)))};
}

/// How far one tier's request may move (see the header comment).
struct RequestBounds {
    /// A bid's size never goes under this: its tier minimum this cycle, or 1.
    Mojo   min_bid_size{1};
    /// False on a pace-managed pair, whose resting bids are held to a pool.
    bool   bid_may_grow{true};
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
    const RequestedAmount want = requested_amount(pair, tier.side, tier.price, tier.size);
    if (taken.count(want) == 0) {
        return RequestMove::None;
    }
    if (tier.side == Side::Bid) {
        // Less of the base asset, down to the tier's minimum.
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
        // More of it, while what the bid pays does not change.
        if (bounds.bid_may_grow) {
            const Mojo pays = bid_offered_quote(pair, tier.price, tier.size);
            for (int k = 1; k <= bounds.max_steps; ++k) {
                const Mojo size = tier.size + k;
                if (bid_offered_quote(pair, tier.price, size) != pays) {
                    break;
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
    for (int k = 1; k <= bounds.max_steps; ++k) {
        const Mojo target = want.second + k;
        if (taken.count({want.first, target}) != 0) {
            continue;
        }
        Mojo price = static_cast<Mojo>(std::ceil(static_cast<double>(target) / per_price_unit));
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
            if (got > target) {
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

/// What every resting offer asks for. `pair_of(pair_name)` gives the pair's
/// config, or nullptr for an offer whose pair is no longer configured, which
/// is left out.
template <typename PairLookup>
[[nodiscard]] std::set<RequestedAmount> requested_by(const std::vector<PendingOffer>& resting,
                                                     PairLookup&& pair_of)
{
    std::set<RequestedAmount> taken;
    for (const PendingOffer& offer : resting) {
        const PairConfig* pair = pair_of(offer.pair_name);
        if (pair != nullptr) {
            taken.insert(requested_amount(*pair, offer.side, offer.price, offer.size));
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
};

/// Makes every tier of `tiers` ask for an amount neither `taken` nor an
/// earlier tier of the same call asks for, and adds each tier's request to
/// `taken`. `bounds_of(tier)` gives that tier's bounds. Returns the tiers
/// that moved; `shared`, when given, counts those no allowed move cleared.
template <typename BoundsOf>
std::vector<RequestChange> make_requests_unique(const PairConfig& pair,
                                                std::vector<TierQuote>& tiers,
                                                std::set<RequestedAmount>& taken,
                                                BoundsOf&& bounds_of,
                                                std::size_t* shared = nullptr)
{
    std::vector<RequestChange> changes;
    if (shared != nullptr) *shared = 0;
    for (TierQuote& tier : tiers) {
        const Mojo before = requested_amount(pair, tier.side, tier.price, tier.size).second;
        const RequestMove move = make_request_unique(pair, tier, taken, bounds_of(tier));
        const RequestedAmount now = requested_amount(pair, tier.side, tier.price, tier.size);
        if (move == RequestMove::Moved) {
            changes.push_back({tier.tier_index, tier.side, before, now.second});
        } else if (move == RequestMove::Shared && shared != nullptr) {
            ++*shared;
        }
        taken.insert(now);
    }
    return changes;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_UNIQUE_REQUEST_HPP
