// [UNIQUE-REQUEST 2026-11-22] No two live offers ask for the same coin.
//
// A taker combining offers pays every request for an asset from one coin,
// and two identical outputs of one spend are invalid (DuplicateOutput); the
// wallet reuses an offer's payment address. See unique_request.hpp, which
// also says why each move is the way it is: never under a tier minimum,
// never over a pace pool, never paying more, never selling for less.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "xop/execution/unique_request.hpp"

using xop::kMojosPerXch;
using xop::Mojo;
using xop::PairConfig;
using xop::PendingOffer;
using xop::Side;
using xop::TierQuote;
using xop::execution::bid_offered_quote;
using xop::execution::make_request_unique;
using xop::execution::make_requests_unique;
using xop::execution::requested_amount;
using xop::execution::requested_by;
using xop::execution::RequestBounds;
using xop::execution::RequestedAmount;
using xop::execution::RequestMove;

namespace {

constexpr Mojo kXch = 1'000'000'000'000;  // mojos in 1 XCH
constexpr Mojo kCat = 1'000;              // mojos in 1 unit of a CAT

PairConfig pair_of(const char* name, const char* base, Mojo base_mpu, const char* quote,
                   Mojo quote_mpu)
{
    PairConfig pc{};
    pc.name = name;
    pc.base_asset_id = base;
    pc.quote_asset_id = quote;
    pc.base_mojos_per_unit = base_mpu;
    pc.quote_mojos_per_unit = quote_mpu;
    return pc;
}

TierQuote tier_of(Side side, double price, Mojo size, std::uint8_t tier_index = 0)
{
    TierQuote t{};
    t.tier_index = tier_index;
    t.side = side;
    t.price = static_cast<Mojo>(std::llround(price * static_cast<double>(kMojosPerXch)));
    t.size = size;
    return t;
}

PendingOffer resting(const PairConfig& pair, const TierQuote& tier)
{
    PendingOffer po{};
    po.offer_id = "0xresting";
    po.pair_name = pair.name;
    po.side = tier.side;
    po.price = tier.price;
    po.size = tier.size;
    po.tier = tier.tier_index;
    return po;
}

RequestBounds at_minimum(Mojo min_size, bool may_grow = true)
{
    RequestBounds bounds;
    bounds.min_bid_size = min_size;
    bounds.bid_may_grow = may_grow;
    return bounds;
}

const PairConfig kXchUsdc = pair_of("XCH/wUSDC.b", "xch", kXch, "usdc", kCat);
const PairConfig kXchByc  = pair_of("XCH/BYC", "xch", kXch, "byc", kCat);
const PairConfig kXchDbx  = pair_of("XCH/DBX", "xch", kXch, "dbx", kCat);
const PairConfig kBycUsdc = pair_of("BYC/wUSDC.b", "byc", kCat, "usdc", kCat);
const PairConfig kEthXch  = pair_of("wmilliETH.b/XCH", "eth", kCat, "xch", kXch);

}  // namespace

TEST(UniqueRequest, ReadsTheRequestAsTheOfferIsBuilt)
{
    // A bid asks for its size of the base; an ask for its quote leg, rounded down.
    const TierQuote bid = tier_of(Side::Bid, 0.917154631389, 10 * kXch);
    EXPECT_EQ(requested_amount(kXchUsdc, bid.side, bid.price, bid.size),
              (RequestedAmount{"xch", 10 * kXch}));
    EXPECT_EQ(bid_offered_quote(kXchUsdc, bid.price, bid.size), 9'172) << "9171.546..., rounded up";
    const TierQuote ask = tier_of(Side::Ask, 0.9976, kXch);
    EXPECT_EQ(requested_amount(kXchUsdc, ask.side, ask.price, ask.size),
              (RequestedAmount{"usdc", 997}));
}

TEST(UniqueRequest, AnUnsharedRequestIsLeftAlone)
{
    TierQuote t = tier_of(Side::Bid, 0.86, kXch);
    const TierQuote before = t;
    EXPECT_EQ(make_request_unique(kXchByc, t, {{"xch", 28 * kXch / 10}}), RequestMove::None);
    EXPECT_EQ(t.size, before.size);
    EXPECT_EQ(t.price, before.price);
}

TEST(UniqueRequest, ABidAsksForAMojoLessAndPaysNoMore)
{
    // The four BYC/wUSDC.b bids, each asking for exactly 1 BYC.
    std::vector<TierQuote> tiers;
    for (std::uint8_t i = 0; i < 4; ++i) tiers.push_back(tier_of(Side::Bid, 0.58 - 0.003 * i, kCat, i));
    const std::vector<TierQuote> before = tiers;
    std::set<RequestedAmount> taken;

    const auto moved = make_requests_unique(kBycUsdc, tiers, taken,
                                            [](const TierQuote&) { return RequestBounds{}; });

    EXPECT_EQ(moved.size(), 3u) << "the first is as computed";
    EXPECT_EQ(taken, (std::set<RequestedAmount>{{"byc", 1'000}, {"byc", 999}, {"byc", 998},
                                                 {"byc", 997}}));
    for (std::size_t i = 0; i < tiers.size(); ++i) {
        EXPECT_EQ(tiers[i].price, before[i].price) << "a bid's price is never moved";
        EXPECT_LE(bid_offered_quote(kBycUsdc, tiers[i].price, tiers[i].size),
                  bid_offered_quote(kBycUsdc, before[i].price, before[i].size));
    }
}

TEST(UniqueRequest, ABidAtItsTierMinimumAsksForAMojoMoreAndPaysTheSame)
{
    // [TIERMIN] A resting offer under its tier's minimum is retired, so a
    // 10 XCH tight bid cannot shrink. One XCH mojo is far under a mojo of the
    // quote asset, so a bid for one more pays exactly what it did.
    TierQuote t = tier_of(Side::Bid, 88.520541355931, 10 * kXch);
    const Mojo pays = bid_offered_quote(kXchDbx, t.price, t.size);
    std::set<RequestedAmount> taken{{"xch", 10 * kXch}};

    ASSERT_EQ(make_request_unique(kXchDbx, t, taken, at_minimum(10 * kXch)), RequestMove::Moved);
    EXPECT_EQ(t.size, 10 * kXch + 1);
    EXPECT_EQ(bid_offered_quote(kXchDbx, t.price, t.size), pays);

    taken.insert({"xch", 10 * kXch + 1});
    TierQuote next = tier_of(Side::Bid, 0.917154631389, 10 * kXch);
    ASSERT_EQ(make_request_unique(kXchUsdc, next, taken, at_minimum(10 * kXch)),
              RequestMove::Moved);
    EXPECT_EQ(next.size, 10 * kXch + 2);
}

TEST(UniqueRequest, APaceManagedBidAtItsMinimumStaysAsComputed)
{
    // [PACE] Its resting bids are held to the pool to the mojo: it never
    // grows, and its tier minimum keeps it from shrinking.
    TierQuote t = tier_of(Side::Bid, 0.131338883888, 10 * kXch);
    const TierQuote before = t;
    EXPECT_EQ(make_request_unique(kXchByc, t, {{"xch", 10 * kXch}},
                                  at_minimum(10 * kXch, /*may_grow=*/false)),
              RequestMove::Shared);
    EXPECT_EQ(t.size, before.size);
    EXPECT_EQ(t.price, before.price);

    // Above its minimum it can still ask for less.
    EXPECT_EQ(make_request_unique(kXchByc, t, {{"xch", 10 * kXch}},
                                  at_minimum(9 * kXch, /*may_grow=*/false)),
              RequestMove::Moved);
    EXPECT_EQ(t.size, 10 * kXch - 1);
}

TEST(UniqueRequest, ABidGrowsOnlyWhileWhatItPaysStaysTheSame)
{
    // 1 BYC at 0.5809 pays 580.9 -> 581 mojos; 1.001 BYC would pay 582.
    TierQuote dear = tier_of(Side::Bid, 0.5809, kCat);
    const TierQuote before = dear;
    EXPECT_EQ(make_request_unique(kBycUsdc, dear, {{"byc", kCat}}, at_minimum(kCat)),
              RequestMove::Shared);
    EXPECT_EQ(dear.size, before.size);

    // At 0.5801 it pays 580.1 -> 581 mojos, and so does 1.001 BYC (580.68).
    TierQuote same = tier_of(Side::Bid, 0.5801, kCat);
    const Mojo pays = bid_offered_quote(kBycUsdc, same.price, same.size);
    EXPECT_EQ(make_request_unique(kBycUsdc, same, {{"byc", kCat}}, at_minimum(kCat)),
              RequestMove::Moved);
    EXPECT_EQ(same.size, kCat + 1);
    EXPECT_EQ(bid_offered_quote(kBycUsdc, same.price, same.size), pays);
}

TEST(UniqueRequest, TheSameTightBidOnThreeXchPairsEndsUpDistinct)
{
    // Every XCH pair's tight bid is its 10 XCH tier minimum. XCH/BYC is
    // pace-managed here and posts last.
    const TierQuote dbx_bid  = tier_of(Side::Bid, 88.520541355931, 10 * kXch);
    const TierQuote usdc_bid = tier_of(Side::Bid, 0.917154631389, 10 * kXch);
    const TierQuote byc_bid  = tier_of(Side::Bid, 0.131338883888, 10 * kXch);
    const auto bounds_of = [](bool may_grow) {
        return [may_grow](const TierQuote&) { return at_minimum(10 * kXch, may_grow); };
    };
    std::set<RequestedAmount> taken;

    std::vector<TierQuote> dbx{dbx_bid};
    EXPECT_TRUE(make_requests_unique(kXchDbx, dbx, taken, bounds_of(true)).empty());
    std::vector<TierQuote> usdc{usdc_bid};
    EXPECT_EQ(make_requests_unique(kXchUsdc, usdc, taken, bounds_of(true)).size(), 1u);
    EXPECT_EQ(usdc[0].size, 10 * kXch + 1);

    std::size_t shared = 0;
    std::vector<TierQuote> byc{byc_bid};
    EXPECT_TRUE(make_requests_unique(kXchByc, byc, taken, bounds_of(false), &shared).empty());
    EXPECT_EQ(shared, 1u) << "it shares 10 XCH with XCH/DBX's resting bid for now";

    // XCH/DBX refreshes its bid: it is the one that moves.
    std::vector<TierQuote> dbx_again{dbx_bid};
    std::set<RequestedAmount> now{{"xch", 10 * kXch}, {"xch", 10 * kXch + 1}};  // BYC's, USDC's
    const auto moved = make_requests_unique(kXchDbx, dbx_again, now, bounds_of(true));
    ASSERT_EQ(moved.size(), 1u);
    EXPECT_EQ(moved[0].asked_before, 10 * kXch);
    EXPECT_EQ(moved[0].asked_now, 10 * kXch + 2);
    EXPECT_EQ(now.size(), 3u) << "three pairs, three amounts";
}

TEST(UniqueRequest, AnAskAsksForAMojoMoreAtAHigherPrice)
{
    TierQuote t = tier_of(Side::Ask, 0.9976, kXch);
    const TierQuote before = t;
    std::set<RequestedAmount> taken{{"usdc", 997}};

    ASSERT_EQ(make_request_unique(kXchUsdc, t, taken), RequestMove::Moved);
    EXPECT_EQ(requested_amount(kXchUsdc, t.side, t.price, t.size).second, 998);
    EXPECT_GT(t.price, before.price) << "never under a price floor";
    EXPECT_EQ(t.size, before.size) << "an ask's size is never moved";
    EXPECT_EQ(requested_amount(kXchUsdc, t.side, t.price - 1, t.size).second, 997)
        << "the lowest price that asks for it";
    EXPECT_LT(static_cast<double>(t.price) / static_cast<double>(before.price), 1.0025);

    taken.insert({"usdc", 998});
    TierQuote again = before;
    ASSERT_EQ(make_request_unique(kXchUsdc, again, taken), RequestMove::Moved);
    EXPECT_EQ(requested_amount(kXchUsdc, again.side, again.price, again.size).second, 999);
}

TEST(UniqueRequest, AnAskDoesNotMoveFurtherThanItsCap)
{
    // 0.1 BYC at 1.3019 asks for 130 mojos: one more would be 62 bps dearer.
    TierQuote t = tier_of(Side::Ask, 1.3019, 100);
    const TierQuote before = t;
    ASSERT_EQ(requested_amount(kBycUsdc, t.side, t.price, t.size).second, 130);
    EXPECT_EQ(make_request_unique(kBycUsdc, t, {{"usdc", 130}}), RequestMove::Shared);
    EXPECT_EQ(t.price, before.price);

    RequestBounds wide;
    wide.max_ask_move_bps = 100.0;
    EXPECT_EQ(make_request_unique(kBycUsdc, t, {{"usdc", 130}}, wide), RequestMove::Moved);
    EXPECT_EQ(requested_amount(kBycUsdc, t.side, t.price, t.size).second, 131);
}

TEST(UniqueRequest, AnAskForXchFindsItsPriceToo)
{
    // 0.5 wmilliETH.b: one price unit moves the request by half a mojo of XCH.
    TierQuote half = tier_of(Side::Ask, 1.8549, 500);
    const RequestedAmount want = requested_amount(kEthXch, half.side, half.price, half.size);
    ASSERT_EQ(make_request_unique(kEthXch, half, {want}), RequestMove::Moved);
    EXPECT_EQ(requested_amount(kEthXch, half.side, half.price, half.size),
              (RequestedAmount{"xch", want.second + 1}));

    // 5 wmilliETH.b: one price unit moves the request by five mojos, so the
    // next amount it can ask for is one price unit on.
    TierQuote five = tier_of(Side::Ask, 1.8549, 5'000);
    const TierQuote before = five;
    const RequestedAmount want5 = requested_amount(kEthXch, five.side, five.price, five.size);
    ASSERT_EQ(make_request_unique(kEthXch, five, {want5}), RequestMove::Moved);
    EXPECT_EQ(five.price, before.price + 1);
    const Mojo asks_now = requested_amount(kEthXch, five.side, five.price, five.size).second;
    EXPECT_GT(asks_now, want5.second);
    EXPECT_LE(asks_now, want5.second + 6);
}

TEST(UniqueRequest, OutOfStepsTheTierIsAsComputed)
{
    TierQuote t = tier_of(Side::Bid, 0.58, kCat);
    std::set<RequestedAmount> taken;
    for (Mojo m = kCat - 3; m <= kCat; ++m) taken.insert({"byc", m});
    RequestBounds three;
    three.max_steps = 3;
    three.bid_may_grow = false;
    const TierQuote before = t;
    EXPECT_EQ(make_request_unique(kBycUsdc, t, taken, three), RequestMove::Shared);
    EXPECT_EQ(t.size, before.size);
    EXPECT_EQ(t.price, before.price);
}

TEST(UniqueRequest, RestingOffersAreReadWithWhatTheyReallyAskFor)
{
    // After a restart State holds the moved tier, as offer_log recorded it,
    // so the amount it asks for is still known and still avoided.
    TierQuote moved = tier_of(Side::Bid, 0.917154631389, 10 * kXch + 1);
    const TierQuote ask = tier_of(Side::Ask, 0.9976, kXch);
    PendingOffer stranger = resting(kXchUsdc, ask);
    stranger.pair_name = "XCH/GONE";
    const std::vector<PendingOffer> offers{resting(kXchUsdc, moved), resting(kXchUsdc, ask),
                                           stranger};
    const auto taken = requested_by(offers, [](const std::string& name) -> const PairConfig* {
        return name == kXchUsdc.name ? &kXchUsdc : nullptr;
    });

    EXPECT_EQ(taken, (std::set<RequestedAmount>{{"xch", 10 * kXch + 1}, {"usdc", 997}}))
        << "an offer of a pair no longer configured is left out";

    TierQuote fresh = tier_of(Side::Bid, 88.520541355931, 10 * kXch);
    EXPECT_EQ(make_request_unique(kXchDbx, fresh, taken, at_minimum(10 * kXch)),
              RequestMove::None)
        << "10 XCH itself is free again";
    TierQuote clash = tier_of(Side::Bid, 88.520541355931, 10 * kXch + 1);
    EXPECT_EQ(make_request_unique(kXchDbx, clash, taken, at_minimum(10 * kXch)),
              RequestMove::Moved);
    EXPECT_EQ(clash.size, 10 * kXch) << "a mojo less, down to its minimum";
}
