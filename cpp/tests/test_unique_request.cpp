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
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "engine_source_scan.hpp"
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
using xop::execution::merged_request;
using xop::execution::min_bid_size_mojos;
using xop::execution::note_request;
using xop::execution::request_from_summary;
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
        << "an offer of a pair no longer configured, with no request recorded, is left out";

    TierQuote fresh = tier_of(Side::Bid, 88.520541355931, 10 * kXch);
    EXPECT_EQ(make_request_unique(kXchDbx, fresh, taken, at_minimum(10 * kXch)),
              RequestMove::None)
        << "10 XCH itself is free again";
    TierQuote clash = tier_of(Side::Bid, 88.520541355931, 10 * kXch + 1);
    EXPECT_EQ(make_request_unique(kXchDbx, clash, taken, at_minimum(10 * kXch)),
              RequestMove::Moved);
    EXPECT_EQ(clash.size, 10 * kXch) << "a mojo less, down to its minimum";
}

// ---------------------------------------------------------------------------
// [review #255]
// ---------------------------------------------------------------------------

TEST(UniqueRequest, AnAskWhosePriceCannotBeWrittenIsLeftShared)
{
    // 10 XCH at nearly the largest price a Mojo holds: asking for one more
    // quote mojo would need a price past it. The conversion is checked, and
    // the tier is as computed.
    TierQuote t{};
    t.side = Side::Ask;
    t.size = 10 * kXch;
    t.price = 9'223'372'036'854'774'784;
    const RequestedAmount want = requested_amount(kXchUsdc, t.side, t.price, t.size);
    const TierQuote before = t;
    EXPECT_EQ(make_request_unique(kXchUsdc, t, {want}), RequestMove::Shared);
    EXPECT_EQ(t.price, before.price);
    EXPECT_EQ(t.size, before.size);
}

TEST(UniqueRequest, AnOfferReadBackFromTheWalletKeepsItsExactRequest)
{
    // OfferManager::try_parse_wallet_offer adopts an ask of 1 XCH for 997
    // quote mojos at price 997: quote mojos per base mojo x 1e12, without the
    // two assets' decimals. Run through the formula that price asks for
    // nothing, so the request is taken from the wallet's summary and kept.
    PendingOffer adopted{};
    adopted.offer_id = "0xadopted";
    adopted.pair_name = kXchUsdc.name;
    adopted.side = Side::Ask;
    adopted.size = kXch;
    adopted.price = 997;
    const auto config_of = [](const std::string& name) -> const PairConfig* {
        return name == kXchUsdc.name ? &kXchUsdc : nullptr;
    };
    EXPECT_TRUE(requested_by({adopted}, config_of).empty())
        << "precondition: its price and size alone say nothing";

    note_request(adopted, request_from_summary(kXchUsdc, Side::Ask, {{"usdc", 997}, {"xch", 5}}));
    EXPECT_EQ(adopted.requested_asset, "usdc");
    EXPECT_EQ(adopted.requested_mojos, 997);
    const auto taken = requested_by({adopted}, config_of);
    EXPECT_EQ(taken, (std::set<RequestedAmount>{{"usdc", 997}}));

    // A new ask that would ask for the same 997 mojos is moved off it.
    TierQuote fresh = tier_of(Side::Ask, 0.9976, kXch);
    EXPECT_EQ(make_request_unique(kXchUsdc, fresh, taken), RequestMove::Moved);
    EXPECT_EQ(requested_amount(kXchUsdc, fresh.side, fresh.price, fresh.size).second, 998);

    // The recorded request counts even once its pair is no longer configured.
    adopted.pair_name = "XCH/GONE";
    EXPECT_EQ(requested_by({adopted}, config_of).count({"usdc", 997}), 1u);
}

TEST(UniqueRequest, AMergedOfferAsksForTheTotalOfItsTiers)
{
    // Batch mode posts one wallet offer for a whole side
    // (OfferManager::post_merged_side). Two 1 BYC bids then ask for 2 BYC
    // together and for 1 BYC not at all, and every record of that trade
    // carries the total, read from the payload the wallet was sent.
    const TierQuote first = tier_of(Side::Bid, 0.5801, kCat, 0);
    const TierQuote second = tier_of(Side::Bid, 0.5772, kCat, 1);
    const auto total = merged_request(kBycUsdc, Side::Bid, first.size + second.size);
    ASSERT_TRUE(total.has_value());
    EXPECT_EQ(*total, (RequestedAmount{"byc", 2 * kCat}));

    PendingOffer a = resting(kBycUsdc, first);
    PendingOffer b = resting(kBycUsdc, second);
    const auto config_of = [](const std::string&) { return &kBycUsdc; };
    EXPECT_EQ(requested_by({a, b}, config_of), (std::set<RequestedAmount>{{"byc", kCat}}))
        << "precondition: a tier's own price and size give its share, 1 BYC";
    note_request(a, total);
    note_request(b, total);
    const auto taken = requested_by({a, b}, config_of);
    EXPECT_EQ(taken, (std::set<RequestedAmount>{{"byc", 2 * kCat}}));

    // Batching is switched off and the engine restarted: a new 2 BYC bid is
    // moved off the merged offer's request, and a 1 BYC bid is free to post.
    TierQuote two = tier_of(Side::Bid, 0.58, 2 * kCat);
    EXPECT_EQ(make_request_unique(kBycUsdc, two, taken), RequestMove::Moved);
    EXPECT_EQ(two.size, 2 * kCat - 1);
    TierQuote one = tier_of(Side::Bid, 0.58, kCat);
    EXPECT_EQ(make_request_unique(kBycUsdc, one, taken), RequestMove::None);

    // An ask side: each tier's leg is rounded down on its own, so the total
    // is not what one record of total size at the worst price would give.
    const TierQuote low = tier_of(Side::Ask, 1.3019, 100);   // 130.19 -> 130
    const TierQuote high = tier_of(Side::Ask, 1.3199, 100);  // 131.99 -> 131
    const Mojo payload = requested_amount(kBycUsdc, low.side, low.price, low.size).second
                         + requested_amount(kBycUsdc, high.side, high.price, high.size).second;
    EXPECT_EQ(payload, 261);
    EXPECT_EQ(requested_amount(kBycUsdc, Side::Ask, low.price, low.size + high.size).second, 260);
    const auto ask_total = merged_request(kBycUsdc, Side::Ask, payload);
    ASSERT_TRUE(ask_total.has_value());
    EXPECT_EQ(*ask_total, (RequestedAmount{"usdc", 261}));

    EXPECT_FALSE(merged_request(kBycUsdc, Side::Ask, 0).has_value()) << "nothing asked for";
}

TEST(UniqueRequest, ALegNoMojoHoldsIsStillDefined)
{
    // An offer adopted before requests were recorded has a price worked out
    // from raw mojos: on wmilliETH.b/XCH that is 1e9 times too large, or
    // whatever rounding a number that size left behind. Its request is then
    // worked out by the formula, which says "the largest Mojo" for a leg past
    // it and "nothing" for one that is not positive.
    constexpr Mojo kTop = std::numeric_limits<Mojo>::max();
    constexpr Mojo kBottom = std::numeric_limits<Mojo>::min();
    EXPECT_EQ(requested_amount(kEthXch, Side::Ask, kTop, 5'000).second, kTop);
    EXPECT_EQ(bid_offered_quote(kEthXch, kTop, 5'000), kTop);
    EXPECT_EQ(requested_amount(kEthXch, Side::Ask, kBottom, 5'000).second, 0);
    EXPECT_EQ(bid_offered_quote(kEthXch, kBottom, 5'000), 0);

    // A record that works out to nothing reserves nothing.
    PendingOffer old{};
    old.pair_name = kEthXch.name;
    old.side = Side::Ask;
    old.size = 5'000;
    old.price = kBottom;
    EXPECT_TRUE(requested_by({old}, [](const std::string&) { return &kEthXch; }).empty());

    // And a bid whose payment no Mojo holds does not grow: "pays the same"
    // cannot be told of it.
    TierQuote t{};
    t.side = Side::Bid;
    t.size = 5'000;
    t.price = kTop;
    const TierQuote before = t;
    EXPECT_EQ(make_request_unique(kEthXch, t, {{"eth", 5'000}}, at_minimum(5'000)),
              RequestMove::Shared);
    EXPECT_EQ(t.size, before.size);
}

TEST(UniqueRequest, TheSummaryNamesTheAssetOnTheRequestedSide)
{
    const auto bid = request_from_summary(kXchUsdc, Side::Bid, {{"xch", 10 * kXch}, {"usdc", 9}});
    ASSERT_TRUE(bid.has_value());
    EXPECT_EQ(*bid, (RequestedAmount{"xch", 10 * kXch}));
    EXPECT_FALSE(request_from_summary(kXchUsdc, Side::Ask, {{"xch", 5}}).has_value())
        << "the quote asset is not in it";
    EXPECT_FALSE(request_from_summary(kXchUsdc, Side::Ask, {{"usdc", 0}}).has_value());

    PendingOffer untouched{};
    note_request(untouched, std::nullopt);
    EXPECT_TRUE(untouched.requested_asset.empty());
    EXPECT_EQ(untouched.requested_mojos, 0);
}

TEST(UniqueRequest, ABidNeverGoesUnderThePairsMinimumSize)
{
    // BYC/wUSDC.b sets min_offer_size_units_override: 1, and Step 7's dust
    // filter drops a tier under it: a 1 BYC bid cannot become 0.999 BYC.
    PairConfig byc = kBycUsdc;
    byc.min_offer_size_units_override = 1.0;
    EXPECT_EQ(min_bid_size_mojos(byc, /*global_min_units=*/5.0, std::nullopt), kCat);
    EXPECT_EQ(min_bid_size_mojos(kBycUsdc, 5.0, std::nullopt), 5 * kCat)
        << "no override: the global minimum";
    EXPECT_EQ(min_bid_size_mojos(kXchByc, 5.0, std::nullopt), kXch) << "an XCH-base pair: 1 XCH";
    EXPECT_EQ(min_bid_size_mojos(byc, 5.0, Mojo{2'000}), 2'000)
        << "its tier's minimum, on a side posting tier minimums";
    EXPECT_EQ(min_bid_size_mojos(byc, 5.0, Mojo{0}), 1);

    // Four 1 BYC bids at that minimum. None shrinks; one grows, the one that
    // can ask for a mojo more and pay the same; the others are as computed.
    std::vector<TierQuote> tiers{tier_of(Side::Bid, 0.5801, kCat, 0), tier_of(Side::Bid, 0.5809, kCat, 1),
                                 tier_of(Side::Bid, 0.5802, kCat, 2), tier_of(Side::Bid, 0.5803, kCat, 3)};
    const std::vector<TierQuote> before = tiers;
    std::set<RequestedAmount> taken;
    std::size_t shared = 0;
    const Mojo min_size = min_bid_size_mojos(byc, 5.0, std::nullopt);
    const auto moved = make_requests_unique(
        byc, tiers, taken, [min_size](const TierQuote&) { return at_minimum(min_size); }, &shared);

    ASSERT_EQ(moved.size(), 1u);
    EXPECT_EQ(moved[0].tier_index, 2);
    EXPECT_EQ(moved[0].asked_now, kCat + 1);
    EXPECT_EQ(shared, 2u);
    for (std::size_t i = 0; i < tiers.size(); ++i) {
        EXPECT_GE(tiers[i].size, min_size) << "tier " << i;
        EXPECT_EQ(bid_offered_quote(byc, tiers[i].price, tiers[i].size),
                  bid_offered_quote(byc, before[i].price, before[i].size))
            << "tier " << i << " pays what it did";
    }
}

// ---------------------------------------------------------------------------
// [2026-11-23] Offers at their minimum follow the rule too.
// ---------------------------------------------------------------------------

namespace {

// The four BYC/wUSDC.b bids as they rest near the peg, each exactly 1 BYC,
// on a pair whose minimum offer is 1 BYC.
std::vector<TierQuote> byc_bids_at_the_minimum()
{
    return {tier_of(Side::Bid, 0.9997, kCat, 0), tier_of(Side::Bid, 0.9992, kCat, 1),
            tier_of(Side::Bid, 0.9983, kCat, 2), tier_of(Side::Bid, 0.9961, kCat, 3)};
}

}  // namespace

TEST(UniqueRequest, ABidAtItsMinimumPaysForExtraMojosOutOfWhatItsSideHasSpare)
{
    // None can shrink, and near the peg each BYC mojo costs about a mojo of
    // wUSDC.b, so none can grow for free either. With 100 mojos spare above
    // the reserve they ask for 1.000, 1.001, 1.002 and 1.003 BYC, each paying
    // for its extra mojos at its own price, which does not move.
    std::vector<TierQuote> tiers = byc_bids_at_the_minimum();
    const std::vector<TierQuote> before = tiers;
    std::set<RequestedAmount> taken;
    std::size_t shared = 0;
    Mojo spare = 100;
    const auto moved = make_requests_unique(
        kBycUsdc, tiers, taken, [](const TierQuote&) { return at_minimum(kCat); }, &shared, &spare);

    EXPECT_EQ(shared, 0u);
    ASSERT_EQ(moved.size(), 3u);
    EXPECT_EQ(taken, (std::set<RequestedAmount>{{"byc", 1'000}, {"byc", 1'001}, {"byc", 1'002},
                                                 {"byc", 1'003}}));
    Mojo paid_more = 0;
    for (std::size_t i = 0; i < tiers.size(); ++i) {
        EXPECT_EQ(tiers[i].price, before[i].price) << "a bid's price is never moved";
        EXPECT_EQ(tiers[i].size, kCat + static_cast<Mojo>(i));
        paid_more += bid_offered_quote(kBycUsdc, tiers[i].price, tiers[i].size)
                     - bid_offered_quote(kBycUsdc, before[i].price, before[i].size);
    }
    // 1001 x 0.9992 = 1000.2 -> 1001 (was 1000); 1002 x 0.9983 = 1000.3 ->
    // 1001 (was 999); 1003 x 0.9961 = 999.1 -> 1000 (was 997).
    EXPECT_EQ(moved[0].paid_more, 1);
    EXPECT_EQ(moved[1].paid_more, 2);
    EXPECT_EQ(moved[2].paid_more, 3);
    EXPECT_EQ(paid_more, 6);
    EXPECT_EQ(spare, 94) << "counted down by exactly what the bids pay more";
}

TEST(UniqueRequest, ABidPaysNoMoreThanItsSideHasSpare)
{
    // One mojo spare: the second bid takes it, and the rest are as computed.
    std::vector<TierQuote> tiers = byc_bids_at_the_minimum();
    const std::vector<TierQuote> before = tiers;
    std::set<RequestedAmount> taken;
    std::size_t shared = 0;
    Mojo spare = 1;
    const auto moved = make_requests_unique(
        kBycUsdc, tiers, taken, [](const TierQuote&) { return at_minimum(kCat); }, &shared, &spare);
    ASSERT_EQ(moved.size(), 1u);
    EXPECT_EQ(moved[0].tier_index, 1);
    EXPECT_EQ(moved[0].paid_more, 1);
    EXPECT_EQ(shared, 2u);
    EXPECT_EQ(spare, 0);
    EXPECT_EQ(tiers[2].size, before[2].size);
    EXPECT_EQ(tiers[3].size, before[3].size);

    // Nothing spare (the side was not judged, or sits at its reserve), or a
    // figure below zero: as before this rule, nothing is paid.
    for (const Mojo none : {Mojo{0}, Mojo{-5}}) {
        std::vector<TierQuote> again = byc_bids_at_the_minimum();
        std::set<RequestedAmount> seen;
        Mojo left = none;
        EXPECT_TRUE(make_requests_unique(
                        kBycUsdc, again, seen, [](const TierQuote&) { return at_minimum(kCat); },
                        nullptr, &left)
                        .empty());
        EXPECT_EQ(left, none);
        for (const TierQuote& t : again) EXPECT_EQ(t.size, kCat);
    }
}

TEST(UniqueRequest, AFreeMoveLeavesTheSpareAlone)
{
    // An XCH bid at its tier minimum asks for a mojo more and pays the same,
    // and a bid above its minimum asks for a mojo less: neither draws on it.
    Mojo spare = 100;
    std::set<RequestedAmount> taken{{"xch", 10 * kXch}};
    std::vector<TierQuote> xch{tier_of(Side::Bid, 88.520541355931, 10 * kXch)};
    const auto grown = make_requests_unique(
        kXchDbx, xch, taken, [](const TierQuote&) { return at_minimum(10 * kXch); }, nullptr, &spare);
    ASSERT_EQ(grown.size(), 1u);
    EXPECT_EQ(xch[0].size, 10 * kXch + 1);
    EXPECT_EQ(grown[0].paid_more, 0);

    std::set<RequestedAmount> byc_taken{{"byc", kCat}};
    std::vector<TierQuote> byc{tier_of(Side::Bid, 0.9992, kCat)};
    const auto shrunk = make_requests_unique(
        kBycUsdc, byc, byc_taken, [](const TierQuote&) { return RequestBounds{}; }, nullptr, &spare);
    ASSERT_EQ(shrunk.size(), 1u);
    EXPECT_EQ(byc[0].size, kCat - 1) << "less is tried first";
    EXPECT_EQ(shrunk[0].paid_more, 0);
    EXPECT_EQ(spare, 100);
}

TEST(UniqueRequest, PaidGrowthKeepsToOnePercentAndToThePacePool)
{
    // 0.5 BYC: 1% of it is five mojos, so with 500 to 505 taken it is as
    // computed, however much is spare.
    TierQuote half = tier_of(Side::Bid, 0.9992, 500);
    const TierQuote before = half;
    std::set<RequestedAmount> taken;
    for (Mojo m = 500; m <= 505; ++m) taken.insert({"byc", m});
    RequestBounds rich = at_minimum(500);
    rich.bid_extra_pay = 1'000'000;
    EXPECT_EQ(make_request_unique(kBycUsdc, half, taken, rich), RequestMove::Shared);
    EXPECT_EQ(half.size, before.size);
    taken.erase({"byc", 505});
    EXPECT_EQ(make_request_unique(kBycUsdc, half, taken, rich), RequestMove::Moved);
    EXPECT_EQ(half.size, 505);

    // [PACE] A managed pair's bids are held to its pool to the mojo: they do
    // not grow, spare or not.
    TierQuote managed = tier_of(Side::Bid, 0.9992, kCat);
    RequestBounds pool = at_minimum(kCat, /*may_grow=*/false);
    pool.bid_extra_pay = 1'000'000;
    EXPECT_EQ(make_request_unique(kBycUsdc, managed, {{"byc", kCat}}, pool), RequestMove::Shared);
    EXPECT_EQ(managed.size, kCat);
}

// ---------------------------------------------------------------------------
// The wiring, by source scan: no unit test can construct Engine or
// OfferManager (engine_source_scan.hpp).
// ---------------------------------------------------------------------------

namespace {

using xop_tests::engine_scan::count_of;
using xop_tests::engine_scan::position_of;
using xop_tests::engine_scan::read_engine_cpp;
using xop_tests::engine_scan::squeeze;

std::string read_offer_manager_cpp()
{
    std::ifstream in(XOP_TESTS_OFFER_MANAGER_CPP, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

TEST(UniqueRequestWiring, Step8MovesTheTiersItThenPostsAndRecords)
{
    const std::string engine = squeeze(read_engine_cpp());
    ASSERT_FALSE(engine.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t taken_at = position_of(
        engine, "auto requested = execution::requested_by( state_->get_all_offers(), "
                "[&](const std::string& name) { return find_pair_config(name); });");
    const std::size_t pass_at = position_of(
        engine, "const auto moved = execution::make_requests_unique( "
                "*pair_cfg, fee_filtered_tiers, requested,");
    const std::size_t post_at = position_of(
        engine, "int posted = co_await offer_mgr_->post_quotes( "
                "*pair_cfg, fee_filtered_tiers, block_height, fee_override);");
    const std::size_t record_at = position_of(
        engine, "orec.requested_asset = in_state.requested_asset; "
                "orec.requested_mojos = in_state.requested_mojos;");
    ASSERT_NE(taken_at, std::string::npos);
    ASSERT_NE(pass_at, std::string::npos);
    ASSERT_NE(post_at, std::string::npos);
    ASSERT_NE(record_at, std::string::npos);
    // Every resting offer's request is read, the tiers are moved, the same
    // list is posted, and each row is then written with the request State
    // holds for its offer -- the one the offer manager took from the payload
    // it sent, which for a merged offer is not the tier's own amount.
    EXPECT_LT(taken_at, pass_at);
    EXPECT_LT(pass_at, post_at);
    EXPECT_LT(post_at, record_at);
    EXPECT_EQ(count_of(engine, "execution::make_requests_unique("), 1u);
    EXPECT_NE(position_of(engine, "const PendingOffer in_state = state_->get_offer(orec.offer_id); "
                                  "orec.status = db_status_for(in_state);"),
              std::string::npos);
    EXPECT_EQ(count_of(engine, "orec.requested_mojos ="), 1u) << "never recomputed from the tier";

    // What a bid may pay for extra mojos is what the exposure check found
    // spare for this pair's bid side, read before the pass and handed to it.
    const std::size_t spare_declared_at = position_of(engine, "Mojo bid_spare_quote_mojos = 0;");
    const std::size_t spare_set_at = position_of(
        engine, "bid_spare_quote_mojos = execution::spare_above_reserve( "
                "exposure_unified, bid_in, bid_dec);");
    ASSERT_NE(spare_declared_at, std::string::npos);
    ASSERT_NE(spare_set_at, std::string::npos);
    EXPECT_LT(spare_declared_at, spare_set_at);
    EXPECT_LT(spare_set_at, pass_at);
    EXPECT_EQ(count_of(engine, "bid_spare_quote_mojos ="), 2u) << "declared at 0, set by the check";
    EXPECT_NE(position_of(engine, "&still_shared, &bid_spare_quote_mojos);"), std::string::npos);

    // The bounds: a pace-managed pair's bids never grow, and a bid's floor is
    // its tier's minimum or, without one, the pair's own minimum size.
    EXPECT_NE(position_of(engine, "const bool bids_may_grow = !pcs.pace.managed;"), std::string::npos);
    EXPECT_NE(position_of(engine,
                          "bounds.min_bid_size = execution::min_bid_size_mojos( "
                          "*pair_cfg, config_.strategy.min_offer_size_units, "
                          "min_it != pcs.tier_min_mojos.end() ? std::optional<Mojo>{min_it->second} "
                          ": std::nullopt);"),
              std::string::npos);

    // An offer adopted at boot or mirrored from State is persisted with the
    // request State holds for it.
    EXPECT_EQ(count_of(engine, "rec.requested_asset = po.requested_asset;"), 2u);
    EXPECT_EQ(count_of(engine, "rec.requested_mojos = po.requested_mojos;"), 2u);
}

TEST(UniqueRequestWiring, OffersReadBackFromTheWalletKeepWhatTheyAskFor)
{
    const std::string manager = squeeze(read_offer_manager_cpp());
    ASSERT_FALSE(manager.empty()) << XOP_TESTS_OFFER_MANAGER_CPP;
    // try_parse_wallet_offer, from the summary's requested amounts.
    EXPECT_EQ(count_of(manager,
                       "note_request(po, request_from_summary(pc_it->second, po.side, requested));"),
              1u);
    // evaluate_orphan, and the orphan adopted from its verdict.
    EXPECT_EQ(count_of(manager,
                       "if (const auto asked = request_from_summary(pc_it->second, eval.side, requested)) { "
                       "eval.requested_asset = asked->first; eval.requested_mojos = asked->second; }"),
              1u);
    EXPECT_EQ(count_of(manager, "po.requested_asset = eval.requested_asset;"), 1u);
    EXPECT_EQ(count_of(manager, "po.requested_mojos = eval.requested_mojos;"), 1u);
}

TEST(UniqueRequestWiring, EveryOfferTheManagerCreatesRecordsItsRequest)
{
    const std::string manager = squeeze(read_offer_manager_cpp());
    ASSERT_FALSE(manager.empty()) << XOP_TESTS_OFFER_MANAGER_CPP;
    const std::string of_its_tier = "requested_amount(pair, tier.side, tier.price, tier.size));";
    // One wallet offer per tier: post_quotes, a create that landed after the
    // stop, and the batch fallback's tiers, each also when its expiry did
    // not take and it is adopted only to be retired.
    EXPECT_EQ(count_of(manager, "note_request(pending, " + of_its_tier), 3u);
    EXPECT_EQ(count_of(manager, "note_request(adopt, " + of_its_tier), 2u);
    EXPECT_EQ(count_of(manager, "note_request(po, " + of_its_tier), 1u);

    // One wallet offer for a whole side: the total the merged payload asks
    // for, on every record of that trade.
    EXPECT_EQ(count_of(manager,
                       "const std::optional<RequestedAmount> merged_asks_for = merged_request( "
                       "pair, tiers.front().side, "
                       "merged_dict.contains(asked_key) ? merged_dict[asked_key].get<std::int64_t>() : 0);"),
              1u);
    EXPECT_EQ(count_of(manager, "note_request(pending, merged_asks_for);"), 2u);
    EXPECT_EQ(count_of(manager, "note_request(adopt, merged_asks_for);"), 1u);

    // As many requests noted as records built: a new posting path that
    // builds a `pending` or an `adopt` and forgets its request fails here.
    EXPECT_EQ(count_of(manager, "note_request(pending, "), count_of(manager, "PendingOffer pending;"));
    EXPECT_EQ(count_of(manager, "note_request(adopt, "), count_of(manager, "PendingOffer adopt;"));
}
