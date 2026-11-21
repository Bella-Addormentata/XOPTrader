// [UNIQUE-REQUEST 2026-11-21] No two live offers ask for the same coin.
//
// A taker combining offers pays every request for an asset from one coin,
// and two identical outputs of one spend are invalid (DuplicateOutput); the
// wallet reuses an offer's payment address. See unique_request.hpp.

#include <gtest/gtest.h>

#include <set>

#include "xop/execution/unique_request.hpp"

using xop::kMojosPerXch;
using xop::Mojo;
using xop::PairConfig;
using xop::Side;
using xop::TierQuote;
using xop::execution::make_request_unique;
using xop::execution::requested_amount;
using xop::execution::RequestedAmount;

namespace {

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

TierQuote tier_of(Side side, double price, Mojo size)
{
    TierQuote t{};
    t.tier_index = 0;
    t.side = side;
    t.price = static_cast<Mojo>(price * static_cast<double>(kMojosPerXch));
    t.size = size;
    return t;
}

const PairConfig kXchUsdc = pair_of("XCH/wUSDC.b", "xch", 1'000'000'000'000, "usdc", 1'000);
const PairConfig kXchByc = pair_of("XCH/BYC", "xch", 1'000'000'000'000, "byc", 1'000);
const PairConfig kBycUsdc = pair_of("BYC/wUSDC.b", "byc", 1'000, "usdc", 1'000);
const PairConfig kEthXch = pair_of("wmilliETH.b/XCH", "eth", 1'000, "xch", 1'000'000'000'000);

}  // namespace

TEST(UniqueRequest, ReadsTheRequestAsTheOfferIsBuilt)
{
    // A bid asks for its size of the base; an ask for the floor of its quote.
    const TierQuote bid = tier_of(Side::Bid, 0.95, 4'600'000'000'000);
    EXPECT_EQ(requested_amount(kXchUsdc, bid.side, bid.price, bid.size),
              (RequestedAmount{"xch", 4'600'000'000'000}));
    const TierQuote ask = tier_of(Side::Ask, 0.9976, 1'000'000'000'000);
    EXPECT_EQ(requested_amount(kXchUsdc, ask.side, ask.price, ask.size),
              (RequestedAmount{"usdc", 997}));
}

TEST(UniqueRequest, AnUnsharedRequestIsLeftAlone)
{
    TierQuote t = tier_of(Side::Bid, 0.86, 1'000'000'000'000);
    const TierQuote before = t;
    EXPECT_TRUE(make_request_unique(kXchByc, t, {{"xch", 2'800'000'000'000}}));
    EXPECT_EQ(t.size, before.size);
    EXPECT_EQ(t.price, before.price);
}

TEST(UniqueRequest, ABidAsksForOneMoreBaseMojo)
{
    // Same-tier XCH bids on two pairs: 1 XCH each, at the same address.
    TierQuote t = tier_of(Side::Bid, 0.86, 1'000'000'000'000);
    std::set<RequestedAmount> taken{{"xch", 1'000'000'000'000}};
    ASSERT_TRUE(make_request_unique(kXchByc, t, taken));
    EXPECT_EQ(t.size, 1'000'000'000'001) << "never under the tier's minimum size";

    taken.insert({"xch", 1'000'000'000'001});
    TierQuote again = tier_of(Side::Bid, 0.86, 1'000'000'000'000);
    ASSERT_TRUE(make_request_unique(kXchByc, again, taken));
    EXPECT_EQ(again.size, 1'000'000'000'002);
}

TEST(UniqueRequest, The1BycBidsEachAskForADifferentAmount)
{
    std::set<RequestedAmount> taken;
    for (int i = 0; i < 4; ++i) {
        TierQuote t = tier_of(Side::Bid, 0.58, 1'000);
        ASSERT_TRUE(make_request_unique(kBycUsdc, t, taken));
        taken.insert(requested_amount(kBycUsdc, t.side, t.price, t.size));
    }
    EXPECT_EQ(taken, (std::set<RequestedAmount>{{"byc", 1'000}, {"byc", 1'001},
                                                 {"byc", 1'002}, {"byc", 1'003}}));
}

TEST(UniqueRequest, AnAskAsksForOneQuoteMojoLessAtALowerPrice)
{
    TierQuote t = tier_of(Side::Ask, 0.9976, 1'000'000'000'000);
    const Mojo price_before = t.price;
    std::set<RequestedAmount> taken{{"usdc", 997}};
    ASSERT_TRUE(make_request_unique(kXchUsdc, t, taken));
    EXPECT_EQ(requested_amount(kXchUsdc, t.side, t.price, t.size).second, 996);
    EXPECT_LT(t.price, price_before) << "in the taker's favour";
    EXPECT_EQ(t.size, 1'000'000'000'000) << "the size is untouched";

    taken.insert({"usdc", 996});
    TierQuote again = tier_of(Side::Ask, 0.9976, 1'000'000'000'000);
    ASSERT_TRUE(make_request_unique(kXchUsdc, again, taken));
    EXPECT_EQ(requested_amount(kXchUsdc, again.side, again.price, again.size).second, 995);
}

TEST(UniqueRequest, AnAskForXchFindsItsPriceToo)
{
    // wmilliETH.b/XCH: one price unit moves the request by half a mojo.
    TierQuote t = tier_of(Side::Ask, 1.8549, 500);
    const RequestedAmount want = requested_amount(kEthXch, t.side, t.price, t.size);
    ASSERT_TRUE(make_request_unique(kEthXch, t, {want}));
    EXPECT_EQ(requested_amount(kEthXch, t.side, t.price, t.size),
              (RequestedAmount{"xch", want.second - 1}));
}

TEST(UniqueRequest, OutOfStepsTheTierIsUnchanged)
{
    TierQuote t = tier_of(Side::Bid, 0.86, 1'000);
    std::set<RequestedAmount> taken;
    for (Mojo m = 1'000; m <= 1'003; ++m) taken.insert({"byc", m});
    const TierQuote before = t;
    EXPECT_FALSE(make_request_unique(kBycUsdc, t, taken, /*max_steps=*/3));
    EXPECT_EQ(t.size, before.size);
    EXPECT_EQ(t.price, before.price);
}
