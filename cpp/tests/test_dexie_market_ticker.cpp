// ---------------------------------------------------------------------------
// Dexie /v1/markets ticker: which depth array is the bid, and orientation.
//
// THE DEFECT: Dexie names its depth arrays for the TAKER.  prices.buy is what
// buying the token costs -- the token's ASK -- and prices.sell is what selling
// it fetches -- the token's BID.  The parse read buy as the bid.  The Case A
// reciprocal-and-flip after it was right all along, so every ticker came out
// with bid and ask exchanged: XCH/BYC read bid 3.333333 / ask 1.451 and logged
// "Crossed book" every heartbeat, over a book that was bid 1.451 / ask 3.333333.
//
// THE EVIDENCE (GET /v1/markets, 2026-09-13; the fixtures below are those
// listings, trimmed to the fields the parser reads):
//   * our own XCH/BYC tier-0 bid, 1451 BYC mojos for 1 XCH = 1.451, is BYC's
//     buy[0] = 0.6891798759476223 = 1/1.451 under "xch", and XCH's
//     sell[0] = 1.451 under the BYC key;
//   * the engine logged XCH/DBX as "bid=84.494000 >= ask=83.404670" -- the
//     reciprocals of that listing's sell[0] and buy[0] respectively;
//   * v0.9.21 recorded BYC/wUSDC.b as "crossed book, bid 373.97 / ask 0.95"
//     (engine.cpp) -- that listing's buy[0] and sell[0], under wUSDC.b's key.
//
// Everything here drives the PURE functions get_ticker() is built from, so no
// HTTP is involved.  NOT reachable from here: the Engine call sites that hand
// best_bid / best_ask to ingest_dexie and to the OFI snapshot -- nothing in
// cpp/tests constructs an Engine.
//
// MUTATION CHECK -- red sets predicted from the arithmetic before the run,
// then each run from a fresh build (the PR carries the table):
//   M1 the parse reads buy as the bid again (the original defect)
//      -> BuyIsTheAskAndSellIsTheBid, CaseAXchBycBidIsOurOwnRestingBid,
//         CaseAXchDbxMatchesTheEngineLogReadCorrectly,
//         CaseBBycXchIsReturnedAsListed, CaseBBycWusdcbJunkAskIsTheAsk,
//         MissingSideStaysZeroWhenInverted, MissingSideStaysZeroWhenDirect,
//         WithBothListingsPresentCaseAIsSearchedFirst.
//      BothListingsOfOneMarketAgree stays GREEN: the mislabel swaps both
//      listings identically.  That is how S7's closure could look right --
//      the flip is self-consistent; only our own bid shows which side is
//      which.
//   M2 Case A takes reciprocals without exchanging the sides
//      -> CaseAXchBycBidIsOurOwnRestingBid,
//         CaseAXchDbxMatchesTheEngineLogReadCorrectly,
//         MissingSideStaysZeroWhenInverted, BothListingsOfOneMarketAgree,
//         WithBothListingsPresentCaseAIsSearchedFirst.
//   M3 Case B exchanges the sides
//      -> CaseBBycXchIsReturnedAsListed, CaseBBycWusdcbJunkAskIsTheAsk,
//         MissingSideStaysZeroWhenDirect.
//   M4 Case A takes reciprocals of high/low without exchanging them
//      -> InvertedHighAndLowAreExchanged, BothListingsOfOneMarketAgree.
//   M7 the Case B search runs before Case A (round 3)
//      -> WithBothListingsPresentCaseAIsSearchedFirst, alone: every other
//         fixture lists each market once, so the order cannot show there.
// ---------------------------------------------------------------------------

#include <cmath>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "xop/rpc/dexie_client.hpp"

using xop::rpc::TickerData;
using xop::rpc::orient_market_ticker;
using xop::rpc::parse_market_ticker;

namespace {

const std::string kByc =
    "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac";
const std::string kDbx =
    "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20";
const std::string kWusdcb =
    "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";

// The "xch" listing of BYC and DBX, as served on 2026-09-13.
constexpr const char* kXchMarkets = R"json({
  "xch": [
    {
      "id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
      "code": "BYC", "name": "Bytecash", "pair_id": "xch", "incentives": false,
      "volume": {
        "xch": {"daily": 80},
        "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac": {"daily": 100}
      },
      "prices": {
        "buy":  [{"depth": 0, "price": 0.6891798759476223},
                 {"depth": 1, "price": 0.6891798759476223},
                 {"depth": 5, "price": 0.6918853876814318}],
        "sell": [{"depth": 0, "price": 0.3},
                 {"depth": 1, "price": 0.3},
                 {"depth": 5, "price": 0.11483803052777644}],
        "last": {"price": 0.5},
        "high": {"daily": 0.8},
        "low":  {"daily": 0.5}
      }
    },
    {
      "id": "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20",
      "code": "DBX", "name": "dexie bucks", "pair_id": "xch", "incentives": true,
      "volume": {
        "xch": {"daily": 0.210679651375},
        "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20": {"daily": 17.802}
      },
      "prices": {
        "buy":  [{"depth": 0, "price": 0.011989736322336629}],
        "sell": [{"depth": 0, "price": 0.011835159893010153}],
        "last": {"price": 0.011834605739523649},
        "high": {"daily": 0.01196581605668925},
        "low":  {"daily": 0.011834605739523649}
      }
    }
  ]
})json";

// The same XCH/BYC market listed the other way round: XCH under the BYC key,
// BYC per XCH.  This listing IS the XCH/BYC pair as listed.
constexpr const char* kBycKeyMarkets = R"json({
  "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac": [
    {
      "id": "xch", "code": "XCH", "name": "Chia",
      "pair_id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
      "incentives": false,
      "prices": {
        "buy":  [{"depth": 0, "price": 3.3333333333333335}],
        "sell": [{"depth": 0, "price": 1.451}],
        "last": {"price": 0.5},
        "high": {"daily": 2},
        "low":  {"daily": 1.25}
      }
    }
  ]
})json";

// BYC under wUSDC.b's key: the configured BYC/wUSDC.b pair, which get_ticker
// resolves through Case B.
constexpr const char* kWusdcbKeyMarkets = R"json({
  "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d": [
    {
      "id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
      "code": "BYC", "name": "Bytecash",
      "pair_id": "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
      "incentives": false,
      "prices": {
        "buy":  [{"depth": 0, "price": 373.97157816005983}],
        "sell": [{"depth": 0, "price": 0.9523809523809523}],
        "last": {"price": 0.48},
        "high": {"daily": null},
        "low":  {"daily": null}
      }
    }
  ]
})json";

// An empty side, served as a null price the way Dexie serves one, and a
// token with no prices at all.
constexpr const char* kOneSidedMarkets = R"json({
  "xch": [
    {
      "id": "aaaa", "code": "ONE", "pair_id": "xch",
      "prices": {
        "buy":  [{"depth": 0, "price": 0.5}],
        "sell": [{"depth": 0, "price": null}],
        "last": {"price": null},
        "high": {"daily": null},
        "low":  {"daily": null}
      }
    },
    {"id": "bbbb", "code": "NONE", "pair_id": "xch"}
  ]
})json";

}  // namespace

// The root convention, pinned by our own resting bid.
TEST(DexieMarketTicker, BuyIsTheAskAndSellIsTheBid)
{
    // Our XCH/BYC bid pays 1.451 BYC for 1 XCH.  In the BYC-keyed listing --
    // the XCH/BYC pair as listed -- it is on the SELL side: a taker selling
    // XCH receives our bid.
    const auto byc_key = nlohmann::json::parse(kBycKeyMarkets);
    const TickerData xch_in_byc =
        parse_market_ticker(byc_key.at(kByc).at(0), kByc);
    EXPECT_DOUBLE_EQ(xch_in_byc.best_bid, 1.451);
    EXPECT_DOUBLE_EQ(xch_in_byc.best_ask, 3.3333333333333335);

    // The same bid seen from "xch" gives BYC away, so it is BYC's ASK,
    // 1/1.451 XCH per BYC -- on the BUY side.
    const auto xch_key = nlohmann::json::parse(kXchMarkets);
    const TickerData byc_in_xch =
        parse_market_ticker(xch_key.at("xch").at(0), "xch");
    EXPECT_DOUBLE_EQ(byc_in_xch.best_ask, 0.6891798759476223);
    EXPECT_DOUBLE_EQ(byc_in_xch.best_bid, 0.3);
    EXPECT_EQ(byc_in_xch.pair_id, "xch");
}

// Case A: the market key is the pair's BASE.
TEST(DexieMarketTicker, CaseAXchBycBidIsOurOwnRestingBid)
{
    const auto m = nlohmann::json::parse(kXchMarkets);
    const auto match = orient_market_ticker(m, "xch", kByc);
    ASSERT_TRUE(match.has_value());
    EXPECT_TRUE(match->inverted);

    const TickerData& t = match->oriented;
    EXPECT_DOUBLE_EQ(t.best_bid, 1.451)
        << "the top bid is our own tier-0 bid, 1451 BYC mojos for 1 XCH";
    EXPECT_DOUBLE_EQ(t.best_ask, 3.3333333333333335)
        << "Dexie's BYC-keyed listing serves this ask as buy[0]";
    EXPECT_LT(t.best_bid, t.best_ask) << "the XCH/BYC ticker is not crossed";
    EXPECT_DOUBLE_EQ(t.price_last, 2.0);
    EXPECT_EQ(t.code, "BYC");
    EXPECT_EQ(t.pair_id, "xch");
    // Volumes stay keyed as listed: XCH under "xch", BYC for the token.
    EXPECT_DOUBLE_EQ(t.volume_xch_daily, 80.0);
    EXPECT_DOUBLE_EQ(t.volume_quote_daily, 100.0);

    // The listing is kept exactly as served.
    EXPECT_DOUBLE_EQ(match->listed.best_ask, 0.6891798759476223);
    EXPECT_DOUBLE_EQ(match->listed.best_bid, 0.3);
}

TEST(DexieMarketTicker, CaseAXchDbxMatchesTheEngineLogReadCorrectly)
{
    // engine.log on v0.10.23: "raw_buy=0.011990 raw_sell=0.011835 ->
    // buy=84.494000 sell=83.404670", then "Crossed book for XCH/DBX:
    // bid=84.494000 >= ask=83.404670".  Read correctly the same two numbers
    // are the bid 83.40467 and the ask 84.494.
    const auto m = nlohmann::json::parse(kXchMarkets);
    const auto match = orient_market_ticker(m, "xch", kDbx);
    ASSERT_TRUE(match.has_value());
    EXPECT_TRUE(match->inverted);
    EXPECT_NEAR(match->oriented.best_bid, 83.40467, 1e-6);
    EXPECT_NEAR(match->oriented.best_ask, 84.494, 1e-6);
    EXPECT_LT(match->oriented.best_bid, match->oriented.best_ask);
}

// Case B: the market key is the pair's QUOTE.
TEST(DexieMarketTicker, CaseBBycXchIsReturnedAsListed)
{
    // BYC/XCH from the same listing: XCH per BYC already, so bid = sell[0]
    // and ask = buy[0], untouched.
    const auto m = nlohmann::json::parse(kXchMarkets);
    const auto match = orient_market_ticker(m, kByc, "xch");
    ASSERT_TRUE(match.has_value());
    EXPECT_FALSE(match->inverted);

    const TickerData& t = match->oriented;
    EXPECT_DOUBLE_EQ(t.best_bid, 0.3);
    EXPECT_DOUBLE_EQ(t.best_ask, 0.6891798759476223);
    EXPECT_LT(t.best_bid, t.best_ask);
    EXPECT_DOUBLE_EQ(t.price_last, 0.5);
    EXPECT_DOUBLE_EQ(t.price_high, 0.8);
    EXPECT_DOUBLE_EQ(t.price_low, 0.5);
}

TEST(DexieMarketTicker, CaseBBycWusdcbJunkAskIsTheAsk)
{
    // v0.9.21 recorded this pair as "crossed book, bid 373.97 / ask 0.95".
    // Those are this listing's buy[0] and sell[0]: a 374-wUSDC.b BYC is an
    // offer to SELL, and the 0.95 is somebody's bid.  Not crossed -- wide.
    const auto m = nlohmann::json::parse(kWusdcbKeyMarkets);
    const auto match = orient_market_ticker(m, kByc, kWusdcb);
    ASSERT_TRUE(match.has_value());
    EXPECT_FALSE(match->inverted);
    EXPECT_DOUBLE_EQ(match->oriented.best_bid, 0.9523809523809523);
    EXPECT_DOUBLE_EQ(match->oriented.best_ask, 373.97157816005983);
    EXPECT_DOUBLE_EQ(match->oriented.price_high, 0.0)
        << "a null daily high is absent";
}

TEST(DexieMarketTicker, InvertedHighAndLowAreExchanged)
{
    // BYC traded between 0.5 and 0.8 XCH, i.e. between 1.25 and 2.0 BYC per
    // XCH: the reciprocal of the LOW is the new HIGH.
    const auto m = nlohmann::json::parse(kXchMarkets);
    const auto match = orient_market_ticker(m, "xch", kByc);
    ASSERT_TRUE(match.has_value());
    EXPECT_DOUBLE_EQ(match->oriented.price_high, 2.0);
    EXPECT_DOUBLE_EQ(match->oriented.price_low, 1.25);
    EXPECT_GT(match->oriented.price_high, match->oriented.price_low);
}

// Dexie lists one market twice; orienting one listing must reproduce the
// other.  This checks the flip, not the labels -- see M1 in the header.
TEST(DexieMarketTicker, BothListingsOfOneMarketAgree)
{
    const auto xch_key = nlohmann::json::parse(kXchMarkets);
    const auto from_xch = orient_market_ticker(xch_key, "xch", kByc);
    ASSERT_TRUE(from_xch.has_value());
    ASSERT_TRUE(from_xch->inverted);

    const auto byc_key = nlohmann::json::parse(kBycKeyMarkets);
    const TickerData as_listed =
        parse_market_ticker(byc_key.at(kByc).at(0), kByc);

    EXPECT_NEAR(from_xch->oriented.best_bid, as_listed.best_bid, 1e-12);
    EXPECT_NEAR(from_xch->oriented.best_ask, as_listed.best_ask, 1e-12);
    EXPECT_NEAR(from_xch->oriented.price_high, as_listed.price_high, 1e-12);
    EXPECT_NEAR(from_xch->oriented.price_low, as_listed.price_low, 1e-12);
    // NOT price_last: Dexie serves 0.5 in both listings, which cannot hold
    // in both units, and the BYC-keyed one sits outside its own 1.25-2.0
    // range.  Every CAT-keyed listing in the saved response does the same,
    // with 1/last inside its range, so Dexie appears to serve `last`
    // reciprocally under CAT keys -- TODO S48, not a property of ours.
}

// [round 3] Dexie can list one market under BOTH keys, and the saved response
// does so for XCH/BYC.  orient_market_ticker searches Case A under every key
// before Case B, and that order is not cosmetic: the two listings disagree on
// `last` (the xch-keyed 0.5 orients to 2.0; the BYC-keyed 0.5 would pass
// through as 0.5), so the search order alone decides the last trade the
// engine ingests.  Bid and ask agree either way, which is why they cannot
// catch a reordering -- `inverted`, the listing's key and `last` can.
TEST(DexieMarketTicker, WithBothListingsPresentCaseAIsSearchedFirst)
{
    auto m = nlohmann::json::parse(kXchMarkets);
    const auto byc_key = nlohmann::json::parse(kBycKeyMarkets);
    m[kByc] = byc_key.at(kByc);
    ASSERT_TRUE(m.contains("xch"));
    ASSERT_TRUE(m.contains(kByc)) << "precondition: both listings present";

    const auto match = orient_market_ticker(m, "xch", kByc);
    ASSERT_TRUE(match.has_value());
    EXPECT_TRUE(match->inverted) << "Case A: the xch-keyed listing, inverted";
    EXPECT_EQ(match->listed.pair_id, "xch");
    EXPECT_DOUBLE_EQ(match->oriented.price_last, 2.0)
        << "Case B would have returned the BYC-keyed listing's 0.5 verbatim";
    EXPECT_DOUBLE_EQ(match->oriented.best_bid, 1.451);
    EXPECT_DOUBLE_EQ(match->oriented.best_ask, 3.3333333333333335);
}

TEST(DexieMarketTicker, MissingSideStaysZeroWhenInverted)
{
    const auto m = nlohmann::json::parse(kOneSidedMarkets);

    const auto one = orient_market_ticker(m, "xch", "aaaa");
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(one->inverted);
    EXPECT_DOUBLE_EQ(one->oriented.best_bid, 2.0) << "1 / the token's 0.5 ask";
    EXPECT_DOUBLE_EQ(one->oriented.best_ask, 0.0)
        << "no token bid: absent, never 1/0";
    EXPECT_TRUE(std::isfinite(one->oriented.best_ask));
    EXPECT_DOUBLE_EQ(one->oriented.price_last, 0.0);
    EXPECT_DOUBLE_EQ(one->oriented.price_high, 0.0);
    EXPECT_DOUBLE_EQ(one->oriented.price_low, 0.0);

    const auto none = orient_market_ticker(m, "xch", "bbbb");
    ASSERT_TRUE(none.has_value());
    for (const double v : {none->oriented.best_bid, none->oriented.best_ask,
                           none->oriented.price_last,
                           none->oriented.price_high,
                           none->oriented.price_low}) {
        EXPECT_TRUE(std::isfinite(v));
        EXPECT_DOUBLE_EQ(v, 0.0);
    }
}

TEST(DexieMarketTicker, MissingSideStaysZeroWhenDirect)
{
    const auto m = nlohmann::json::parse(kOneSidedMarkets);
    const auto one = orient_market_ticker(m, "aaaa", "xch");
    ASSERT_TRUE(one.has_value());
    EXPECT_FALSE(one->inverted);
    EXPECT_DOUBLE_EQ(one->oriented.best_bid, 0.0) << "the sell side is null";
    EXPECT_DOUBLE_EQ(one->oriented.best_ask, 0.5);
}

TEST(DexieMarketTicker, UnmatchedOrMalformedIsNotFound)
{
    const auto m = nlohmann::json::parse(kXchMarkets);
    EXPECT_FALSE(orient_market_ticker(m, "xch", "cccc").has_value());
    EXPECT_FALSE(orient_market_ticker(m, kByc, kDbx).has_value())
        << "two tokens listed under one key are not a pair of each other";
    EXPECT_FALSE(orient_market_ticker(nlohmann::json::array(), "xch", kByc)
                     .has_value());
    EXPECT_FALSE(orient_market_ticker(
                     nlohmann::json::parse(R"({"xch": "not a list"})"),
                     "xch", kByc)
                     .has_value());
    EXPECT_FALSE(orient_market_ticker(
                     nlohmann::json::parse(R"({"xch": [42, null, "x"]})"),
                     "xch", kByc)
                     .has_value());
}
