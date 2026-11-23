// [ADOPT-PRICE 2026-11-23] An offer read back from the wallet is priced with
// both assets' denominations.
//
// OfferManager::evaluate_orphan and try_parse_wallet_offer priced it as
// quote_mojos / base_mojos * kMojosPerXch, which is the engine's price only
// where the two assets have the same decimals: 1e9 times too small on every
// XCH/CAT pair, 1e9 times too large on a CAT/XCH one.  An adopted offer was
// then judged against the market at that price and cancelled for it.  See
// summary_offer.hpp, which also has the live cases these tests use.

#include <xop/execution/offer_manager.hpp>
#include <xop/execution/summary_offer.hpp>
#include <xop/execution/unique_request.hpp>
#include <xop/types.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#include "engine_source_scan.hpp"

using json = nlohmann::json;
using xop::kMojosPerXch;
using xop::Mojo;
using xop::PairConfig;
using xop::quote_mojos_for;
using xop::Side;
using xop::execution::offer_from_summary;
using xop::execution::OfferManager;
using xop::execution::price_from_legs;
using xop::execution::price_standing;
using xop::execution::PriceStanding;
using xop::execution::requested_amount;
using xop::execution::RequestedAmount;
using xop::execution::SummaryOffer;

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

const PairConfig kXchUsdc = pair_of("XCH/wUSDC.b", "xch", kXch, "usdc", kCat);
const PairConfig kXchDbx  = pair_of("XCH/DBX", "xch", kXch, "dbx", kCat);
const PairConfig kBycUsdc = pair_of("BYC/wUSDC.b", "byc", kCat, "usdc", kCat);
const PairConfig kEthXch  = pair_of("wmilliETH.b/XCH", "eth", kCat, "xch", kXch);

// The configured pairs, in a container with a fixed order.
std::map<std::string, PairConfig> pairs()
{
    return {{kBycUsdc.name, kBycUsdc}, {kXchDbx.name, kXchDbx}, {kXchUsdc.name, kXchUsdc},
            {kEthXch.name, kEthXch}};
}

// The ask the engine re-adopted at its start of 2026-11-20 22:34Z:
// 22.807092182709 XCH at 1.011 wUSDC.b, so 23,057 mojos of wUSDC.b.
constexpr Mojo kLiveAskBase  = 22'807'092'182'709;
constexpr Mojo kLiveAskQuote = 23'057;
// And the XCH/DBX bid of that start: 4.6 XCH at 88.274 DBX.
constexpr Mojo kLiveBidBase  = 4'600'000'000'000;
constexpr Mojo kLiveBidQuote = 406'060;

// What both readers used to compute.
Mojo raw_ratio_price(Mojo base_mojos, Mojo quote_mojos)
{
    return static_cast<Mojo>(std::llround(static_cast<double>(quote_mojos)
                                          / static_cast<double>(base_mojos)
                                          * static_cast<double>(kMojosPerXch)));
}

}  // namespace

TEST(SummaryOffer, ThePriceUsesBothAssetsDenominations)
{
    // XCH/wUSDC.b: 1 XCH for 997 mojos of wUSDC.b is 0.997 a coin.  The
    // raw-mojo ratio said 997: a billionth of it.
    EXPECT_EQ(price_from_legs(kXch, 997, kXch, kCat), std::optional<Mojo>{997'000'000'000});
    EXPECT_EQ(raw_ratio_price(kXch, 997), 997);

    // The live ask: 1.011 a coin, adopted at "1011".
    const auto live_ask = price_from_legs(kLiveAskBase, kLiveAskQuote, kXch, kCat);
    ASSERT_TRUE(live_ask.has_value());
    EXPECT_NEAR(static_cast<double>(*live_ask), 1.011e12, 1e9);
    EXPECT_EQ(raw_ratio_price(kLiveAskBase, kLiveAskQuote), 1'011);

    // The live XCH/DBX bid: 88.274 a coin, adopted at "88274".
    const auto live_bid = price_from_legs(kLiveBidBase, kLiveBidQuote, kXch, kCat);
    ASSERT_TRUE(live_bid.has_value());
    EXPECT_NEAR(static_cast<double>(*live_bid), 88.274e12, 1e9);
    EXPECT_EQ(raw_ratio_price(kLiveBidBase, kLiveBidQuote), 88'274);

    // wmilliETH.b/XCH: 1 wmilliETH.b for 1.85 XCH is 1.85.  The raw ratio is
    // 1.85e21, which no Mojo holds.
    EXPECT_EQ(price_from_legs(kCat, 1'850'000'000'000, kCat, kXch),
              std::optional<Mojo>{1'850'000'000'000});

    // BYC/wUSDC.b: the same decimals on both sides, so nothing changes.
    EXPECT_EQ(price_from_legs(kCat, 581, kCat, kCat), std::optional<Mojo>{581'000'000'000});
    EXPECT_EQ(raw_ratio_price(kCat, 581), 581'000'000'000);
}

TEST(SummaryOffer, ThePriceRunsBackIntoTheLegs)
{
    // price_from_legs is the inverse of quote_mojos_for, so what an adopted
    // offer pays or is paid comes back from its price and size: the exposure
    // sums count an adopted bid's spend, and no longer a billionth of it.
    struct Case {
        const PairConfig* pair;
        Mojo base;
        Mojo quote;
    };
    const Case cases[] = {{&kXchUsdc, kXch, 997},
                          {&kXchUsdc, kLiveAskBase, kLiveAskQuote},
                          {&kXchDbx, kLiveBidBase, kLiveBidQuote},
                          {&kEthXch, kCat, 1'850'000'000'000},
                          {&kEthXch, 500, 927'431'250'000},
                          {&kBycUsdc, kCat, 581}};
    for (const Case& c : cases) {
        const auto price = price_from_legs(c.base, c.quote, c.pair->base_mojos_per_unit,
                                           c.pair->quote_mojos_per_unit);
        ASSERT_TRUE(price.has_value()) << c.pair->name;
        const double back = quote_mojos_for(static_cast<double>(c.base), static_cast<double>(*price),
                                            static_cast<double>(c.pair->base_mojos_per_unit),
                                            static_cast<double>(c.pair->quote_mojos_per_unit));
        EXPECT_LE(std::abs(back - static_cast<double>(c.quote)), 1.0)
            << c.pair->name << ": " << back << " for " << c.quote;
    }

    // So the formula gives an adopted ask's request back to within a mojo
    // (rounding may land one off, which is why the summary's own figure is
    // kept with the offer), where it used to give nothing at all.
    const auto price = price_from_legs(kXch, 997, kXch, kCat);
    ASSERT_TRUE(price.has_value());
    const RequestedAmount asked = requested_amount(kXchUsdc, Side::Ask, *price, kXch);
    EXPECT_EQ(asked.first, "usdc");
    EXPECT_LE(std::llabs(asked.second - 997), 1);
    EXPECT_EQ(requested_amount(kXchUsdc, Side::Ask, raw_ratio_price(kXch, 997), kXch).second, 0)
        << "what it used to give";
}

TEST(SummaryOffer, APriceThatCannotBeReckonedIsNone)
{
    EXPECT_FALSE(price_from_legs(0, 997, kXch, kCat).has_value());
    EXPECT_FALSE(price_from_legs(-kXch, 997, kXch, kCat).has_value());
    EXPECT_FALSE(price_from_legs(kXch, 0, kXch, kCat).has_value());
    EXPECT_FALSE(price_from_legs(kXch, 997, 0, kCat).has_value());
    EXPECT_FALSE(price_from_legs(kXch, 997, kXch, 0).has_value());
    // Past what a Mojo holds: one mojo of a CAT for nine million XCH.
    EXPECT_FALSE(price_from_legs(1, 9'000'000'000'000'000'000, kCat, kXch).has_value());
    // And one that rounds to nothing: a million CAT units for one mojo of XCH.
    EXPECT_FALSE(price_from_legs(1'000'000 * kCat, 1, kCat, kXch).has_value());
}

TEST(SummaryOffer, ReadsAnAskAndABidFromTheSummary)
{
    // An ask: we give the base and get the quote.
    const auto ask = offer_from_summary({{"xch", kLiveAskBase}}, {{"usdc", kLiveAskQuote}}, pairs());
    ASSERT_TRUE(ask.has_value());
    EXPECT_EQ(ask->pair_name, "XCH/wUSDC.b");
    EXPECT_EQ(ask->side, Side::Ask);
    EXPECT_EQ(ask->size, kLiveAskBase);
    EXPECT_NEAR(static_cast<double>(ask->price), 1.011e12, 1e9);
    ASSERT_TRUE(ask->request.has_value());
    EXPECT_EQ(*ask->request, (RequestedAmount{"usdc", kLiveAskQuote}));

    // A bid: we give the quote and get the base.
    const auto bid = offer_from_summary({{"dbx", kLiveBidQuote}}, {{"xch", kLiveBidBase}}, pairs());
    ASSERT_TRUE(bid.has_value());
    EXPECT_EQ(bid->pair_name, "XCH/DBX");
    EXPECT_EQ(bid->side, Side::Bid);
    EXPECT_EQ(bid->size, kLiveBidBase);
    EXPECT_NEAR(static_cast<double>(bid->price), 88.274e12, 1e9);
    ASSERT_TRUE(bid->request.has_value());
    EXPECT_EQ(*bid->request, (RequestedAmount{"xch", kLiveBidBase}));

    // A pair whose base is the CAT and whose quote is XCH, both ways round.
    const auto eth_ask = offer_from_summary({{"eth", 500}}, {{"xch", 927'431'250'000}}, pairs());
    ASSERT_TRUE(eth_ask.has_value());
    EXPECT_EQ(eth_ask->pair_name, "wmilliETH.b/XCH");
    EXPECT_EQ(eth_ask->side, Side::Ask);
    EXPECT_EQ(eth_ask->price, 1'854'862'500'000);
    const auto eth_bid = offer_from_summary({{"xch", 927'431'250'000}}, {{"eth", 500}}, pairs());
    ASSERT_TRUE(eth_bid.has_value());
    EXPECT_EQ(eth_bid->side, Side::Bid);
    EXPECT_EQ(eth_bid->size, 500);
    EXPECT_EQ(eth_bid->price, 1'854'862'500'000);

    // The container the manager holds its pairs in works the same.
    const std::unordered_map<std::string, PairConfig> unordered{{kXchUsdc.name, kXchUsdc}};
    const auto again = offer_from_summary({{"xch", kXch}}, {{"usdc", 997}}, unordered);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->price, 997'000'000'000);
}

TEST(SummaryOffer, AnOfferOfNoConfiguredPairIsNotRead)
{
    // An option contract sold for XCH, two assets of different pairs, and
    // an offer of the right assets on the same side.
    EXPECT_FALSE(offer_from_summary({{"0xoption", 1}}, {{"xch", kXch}}, pairs()).has_value());
    EXPECT_FALSE(offer_from_summary({{"dbx", 5'000}}, {{"usdc", 5'000}}, pairs()).has_value());
    EXPECT_FALSE(offer_from_summary({{"xch", kXch}, {"usdc", 997}}, {{"0xother", 1}}, pairs()).has_value());
    EXPECT_FALSE(offer_from_summary({}, {}, pairs()).has_value());

    // A leg that is not positive still names the offer, with no price and no
    // request: it has to be tracked, and it cannot be judged.
    const auto empty_leg = offer_from_summary({{"xch", kXch}}, {{"usdc", 0}}, pairs());
    ASSERT_TRUE(empty_leg.has_value());
    EXPECT_EQ(empty_leg->pair_name, "XCH/wUSDC.b");
    EXPECT_EQ(empty_leg->price, 0);
    EXPECT_FALSE(empty_leg->request.has_value());
}

TEST(SummaryOffer, AnOrphanIsJudgedAtItsRealPrice)
{
    // The live ask, 1.1% above a mid of 1.00: an ask above the mid is the
    // side that does not hurt, and boot keeps it.
    const Mojo mid = kMojosPerXch;  // 1.00 wUSDC.b a coin
    const auto ask = offer_from_summary({{"xch", kLiveAskBase}}, {{"usdc", kLiveAskQuote}}, pairs());
    ASSERT_TRUE(ask.has_value());
    const PriceStanding now = price_standing(ask->side, ask->price, mid);
    EXPECT_FALSE(now.adverse);
    EXPECT_NEAR(now.deviation, 0.011, 0.0005);

    // At the price it was adopted at, it stood 100% under the mid:
    // "underselling", past any threshold, and it was cancelled.
    const PriceStanding then = price_standing(Side::Ask, raw_ratio_price(kLiveAskBase, kLiveAskQuote), mid);
    EXPECT_TRUE(then.adverse);
    EXPECT_NEAR(then.deviation, 1.0, 1e-6);

    // And a bid that overpaid by half was "100% under the mid" and kept:
    // 1 XCH for 1.5 wUSDC.b.
    const auto dear = offer_from_summary({{"usdc", 1'500}}, {{"xch", kXch}}, pairs());
    ASSERT_TRUE(dear.has_value());
    const PriceStanding dear_now = price_standing(dear->side, dear->price, mid);
    EXPECT_TRUE(dear_now.adverse);
    EXPECT_NEAR(dear_now.deviation, 0.5, 1e-9);
    EXPECT_FALSE(price_standing(Side::Bid, raw_ratio_price(kXch, 1'500), mid).adverse)
        << "what it used to say";
}

TEST(SummaryOffer, PriceStandingIsTheSideThatHurts)
{
    const Mojo mid = 2 * kMojosPerXch;
    // A bid above the mid overpays; one below it does not.
    EXPECT_TRUE(price_standing(Side::Bid, mid + mid / 50, mid).adverse);
    EXPECT_NEAR(price_standing(Side::Bid, mid + mid / 50, mid).deviation, 0.02, 1e-12);
    EXPECT_FALSE(price_standing(Side::Bid, mid - mid / 50, mid).adverse);
    EXPECT_NEAR(price_standing(Side::Bid, mid - mid / 50, mid).deviation, 0.02, 1e-12);
    // An ask below the mid undersells; one above it does not.
    EXPECT_TRUE(price_standing(Side::Ask, mid - mid / 50, mid).adverse);
    EXPECT_FALSE(price_standing(Side::Ask, mid + mid / 50, mid).adverse);
    // At the mid, and with no mid: no deviation, nothing adverse.
    EXPECT_FALSE(price_standing(Side::Bid, mid, mid).adverse);
    EXPECT_EQ(price_standing(Side::Ask, mid, mid).deviation, 0.0);
    EXPECT_FALSE(price_standing(Side::Ask, mid, 0).adverse);
    EXPECT_EQ(price_standing(Side::Bid, mid, 0).deviation, 0.0);
}

TEST(SummaryOffer, ASettledFillIsPricedByTheSameFormula)
{
    // parse_settled_fill had the formula right; it now shares the one copy.
    json summary = json::object();
    summary["offered"] = json::object({{"xch", kLiveAskBase}});
    summary["requested"] = json::object({{"usdc", kLiveAskQuote}});
    json rec = json::object();
    rec["trade_id"] = "0xabc";
    rec["status"] = "CONFIRMED";
    rec["summary"] = summary;
    const auto settled = OfferManager::parse_settled_fill(rec, Side::Ask, kXchUsdc);
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->size, kLiveAskBase);
    EXPECT_EQ(std::optional<Mojo>{settled->price},
              price_from_legs(kLiveAskBase, kLiveAskQuote, kXch, kCat));

    // A price no Mojo holds is unparseable, not whatever a cast leaves.
    json absurd_summary = json::object();
    absurd_summary["offered"] = json::object({{"eth", 1}});
    absurd_summary["requested"] = json::object({{"xch", Mojo{9'000'000'000'000'000'000}}});
    json absurd = json::object();
    absurd["summary"] = absurd_summary;
    EXPECT_FALSE(OfferManager::parse_settled_fill(absurd, Side::Ask, kEthXch).has_value());
}

// ---------------------------------------------------------------------------
// The wiring, by source scan: no unit test can construct an OfferManager
// (engine_source_scan.hpp).
// ---------------------------------------------------------------------------

TEST(SummaryOfferWiring, BothReadersAndTheFillParserShareTheOnePrice)
{
    using xop_tests::engine_scan::count_of;
    using xop_tests::engine_scan::squeeze;
    std::ifstream in(XOP_TESTS_OFFER_MANAGER_CPP, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string manager = squeeze(buffer.str());
    ASSERT_FALSE(manager.empty()) << XOP_TESTS_OFFER_MANAGER_CPP;

    // evaluate_orphan and try_parse_wallet_offer read the offer's shape, its
    // price included, through offer_from_summary...
    EXPECT_EQ(count_of(manager,
                       "const std::optional<SummaryOffer> shape = "
                       "offer_from_summary(offered, requested, pair_config_map_);"),
              2u);
    EXPECT_EQ(count_of(manager, "eval.price = shape->price;"), 1u);
    EXPECT_EQ(count_of(manager, "po.price = shape->price;"), 1u);
    // ...the orphan is judged by price_standing...
    EXPECT_EQ(count_of(manager,
                       "const PriceStanding standing = "
                       "price_standing(eval.side, eval.price, mid_it->second);"),
              1u);
    // ...parse_settled_fill uses the same formula...
    EXPECT_EQ(count_of(manager,
                       "const std::optional<Mojo> price = price_from_legs( "
                       "base_mojos, quote_mojos, pair.base_mojos_per_unit, pair.quote_mojos_per_unit);"),
              1u);
    // ...and no copy of the raw-mojo ratio is left.
    EXPECT_EQ(count_of(manager, "quote_d / base_d"), 0u);
}
