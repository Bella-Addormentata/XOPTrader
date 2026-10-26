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
// [CHURN 2026-10-26] WHICH LISTING: Dexie can list one market under both of
// its assets' keys.  Bid, ask, high and low come in each listing's own units,
// but `last` is the same raw number in every listing, in the units of the
// market's canonical one (TODO S53): "xch" for an XCH market; for a CAT/CAT
// market, the asset Dexie prices it in, which GET /v1/pairs names (wUSDC.b
// for BYC/wUSDC.b).  Reading BYC/wUSDC.b's BYC-keyed listing first turned its
// 0.8988 into 1.1126, and the flips between the two listings got 68 of our
// resting asks cancelled as crossed in 17 h.
//
// Everything here drives the PURE functions get_ticker() is built from, so no
// HTTP is involved.  NOT reachable from here: the Engine call sites that hand
// best_bid / best_ask / volume_base_daily to ingest_dexie and to the OFI
// snapshot -- nothing in cpp/tests constructs an Engine -- and get_ticker()'s
// glue around GET /v1/pairs: when it claims a fetch, what it stores or marks
// failed, and that it hands the cached quote to orient_market_ticker().
// DexieClient has no HTTP seam; the parse, the lookup and the cache that
// glue calls are driven below.
//
// MUTATION CHECK -- red sets predicted from the arithmetic before the run,
// then each run from a fresh build (the PR carries the table):
//   M1 the parse reads buy as the bid again (the original defect)
//      -> BuyIsTheAskAndSellIsTheBid, CaseAXchBycBidIsOurOwnRestingBid,
//         CaseAXchDbxMatchesTheEngineLogReadCorrectly,
//         CaseBBycXchIsReturnedAsListed, CaseBBycWusdcbJunkAskIsTheAsk,
//         MissingSideStaysZeroWhenInverted, MissingSideStaysZeroWhenDirect,
//         WithBothListingsPresentCaseAIsSearchedFirst,
//         CatCatWithBothListingsTheQuoteKeyedOneIsUsed,
//         CatCatListedOnlyUnderItsBaseGetsTheMarketsLast,
//         XchBasePairListedOnlyUnderTheCatKeyGetsTheMarketsLast,
//         ReversedCatCatWithDexiesPairsReadsTheCanonicalListing,
//         ReversedCatCatListedOnlyUnderItsQuoteGetsTheMarketsLast,
//         XchQuotedPairWithBothListingsTheXchKeyedOneIsUsed.
//      BothListingsOfOneMarketAgree stays GREEN: the mislabel swaps both
//      listings identically.  That is how S7's closure could look right --
//      the flip is self-consistent; only our own bid shows which side is
//      which.
//   M2 Case A takes reciprocals without exchanging the sides
//      -> CaseAXchBycBidIsOurOwnRestingBid,
//         CaseAXchDbxMatchesTheEngineLogReadCorrectly,
//         MissingSideStaysZeroWhenInverted, BothListingsOfOneMarketAgree,
//         WithBothListingsPresentCaseAIsSearchedFirst,
//         CatCatListedOnlyUnderItsBaseGetsTheMarketsLast,
//         ReversedCatCatWithDexiesPairsReadsTheCanonicalListing.
//   M3 Case B exchanges the sides
//      -> CaseBBycXchIsReturnedAsListed, CaseBBycWusdcbJunkAskIsTheAsk,
//         MissingSideStaysZeroWhenDirect,
//         CatCatWithBothListingsTheQuoteKeyedOneIsUsed,
//         XchBasePairListedOnlyUnderTheCatKeyGetsTheMarketsLast,
//         ReversedCatCatListedOnlyUnderItsQuoteGetsTheMarketsLast,
//         XchQuotedPairWithBothListingsTheXchKeyedOneIsUsed.
//   M4 Case A takes reciprocals of high/low without exchanging them
//      -> InvertedHighAndLowAreExchanged, BothListingsOfOneMarketAgree.
//   M7 the quote-keyed listing is canonical for every pair (Case B first
//      everywhere, the XCH rule gone)
//      -> CaseAXchBycBidIsOurOwnRestingBid,
//         WithBothListingsPresentCaseAIsSearchedFirst,
//         XchBasePairListedOnlyUnderTheCatKeyGetsTheMarketsLast,
//         ReversedCatCatWithDexiesPairsReadsTheCanonicalListing,
//         ReversedCatCatListedOnlyUnderItsQuoteGetsTheMarketsLast,
//         AMarketQuoteIsIgnoredForAnXchPairOrWhenItNamesNeitherAsset.
//   M8 [CHURN 2026-10-26] the base-keyed listing is canonical for every pair
//      (Case A first everywhere, the order before this round)
//      -> CaseBBycXchIsReturnedAsListed,
//         CatCatWithBothListingsTheQuoteKeyedOneIsUsed,
//         CatCatListedOnlyUnderItsBaseGetsTheMarketsLast,
//         XchQuotedPairWithBothListingsTheXchKeyedOneIsUsed,
//         AMarketQuoteIsIgnoredForAnXchPairOrWhenItNamesNeitherAsset.
//   M9 the XCH test checks the quote only, so an XCH-BASED pair is handled
//      as CAT/CAT
//      -> CaseAXchBycBidIsOurOwnRestingBid,
//         WithBothListingsPresentCaseAIsSearchedFirst,
//         XchBasePairListedOnlyUnderTheCatKeyGetsTheMarketsLast,
//         AMarketQuoteIsIgnoredForAnXchPairOrWhenItNamesNeitherAsset.
//   M10 a non-canonical listing's `last` is oriented as that listing's own
//      -> CatCatListedOnlyUnderItsBaseGetsTheMarketsLast,
//         XchBasePairListedOnlyUnderTheCatKeyGetsTheMarketsLast,
//         ReversedCatCatListedOnlyUnderItsQuoteGetsTheMarketsLast.
//   M11 `market_quote` is ignored (the configured quote is always canonical)
//      -> ReversedCatCatWithDexiesPairsReadsTheCanonicalListing,
//         ReversedCatCatListedOnlyUnderItsQuoteGetsTheMarketsLast.
//   M12 `market_quote` outranks the XCH rule
//      -> AMarketQuoteIsIgnoredForAnXchPairOrWhenItNamesNeitherAsset.
//   M13 a `market_quote` naming neither asset counts as the base
//      -> AMarketQuoteIsIgnoredForAnXchPairOrWhenItNamesNeitherAsset.
//   M14 cat_market_quote() answers for an XCH market
//      -> CatMarketQuoteIsUnknownForXchMarketsUnlistedPairsAndBothOrders.
//   M15 cat_market_quote() matches the entry in the requested order only
//      -> CatMarketQuoteIsThePairsQuoteInEitherOrder,
//         CatMarketQuoteIsUnknownForXchMarketsUnlistedPairsAndBothOrders.
//   M16 cat_market_quote() answers with the first entry when both orders are
//      listed
//      -> CatMarketQuoteIsUnknownForXchMarketsUnlistedPairsAndBothOrders.
//   M17 claim_fetch() leaves the due time where it was
//      -> PairsCacheFetchesEveryTtlAndRetriesAfterAFailure.
//   M18 fail() drops the entries
//      -> PairsCacheFetchesEveryTtlAndRetriesAfterAFailure.
//   M19 store() schedules the next fetch kRetry out instead of kTtl
//      -> PairsCacheFetchesEveryTtlAndRetriesAfterAFailure.
//   M20 parse_pairs_response() keeps an entry whose id is empty
//      -> PairsResponseParsesAndSkipsMalformedEntries.
//   M21 Case A leaves volume_base_daily as the listed token's
//      -> CaseAXchBycBidIsOurOwnRestingBid.
//   M22 the parse takes volume_base_daily from the market key's volume
//      -> CaseBBycXchIsReturnedAsListed,
//         XchQuotedPairWithBothListingsTheXchKeyedOneIsUsed.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "xop/rpc/dexie_client.hpp"

using xop::rpc::PairInfo;
using xop::rpc::PairsOrientationCache;
using xop::rpc::TickerData;
using xop::rpc::cat_market_quote;
using xop::rpc::orient_market_ticker;
using xop::rpc::parse_market_ticker;
using xop::rpc::parse_pairs_response;

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
// BYC per XCH.  This listing IS the XCH/BYC pair as listed -- except `last`,
// the "xch" listing's 0.5 XCH per BYC served again (TODO S53).
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

// [CHURN 2026-10-26] BYC/wUSDC.b as Dexie served it on 2026-10-25/26 (engine
// log: the "get_ticker -> found" and "ingest_dexie" lines), trimmed to the
// fields the parser reads.  Under wUSDC.b's key, BYC with our own 0.908 ask
// and no bid: the listing get_ticker read in 1,132 of 1,218 reads.
constexpr const char* kWusdcbKeyOct26 = R"json({
  "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d": [
    {
      "id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
      "code": "BYC", "name": "Bytecash",
      "pair_id": "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
      "prices": {
        "buy":  [{"depth": 0, "price": 0.908}],
        "sell": [{"depth": 0, "price": null}],
        "last": {"price": 0.8988},
        "high": {"daily": null},
        "low":  {"daily": null}
      }
    }
  ]
})json";

// The same market under BYC's key, which Case A found in the other 86:
// wUSDC.b, BYC per wUSDC.b, with our 0.908 ask seen from the other side
// (1/0.908, a wUSDC.b bid) and a junk 710.546 ask.  Its `last` is the same
// 0.8988 -- wUSDC.b per BYC, not BYC per wUSDC.b: our own BYC/wUSDC.b bids
// settled at 0.896 and 0.900 on 2026-10-20.
constexpr const char* kBycKeyOct26 = R"json({
  "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac": [
    {
      "id": "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
      "code": "wUSDC.b",
      "pair_id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
      "prices": {
        "buy":  [{"depth": 0, "price": 710.545999}],
        "sell": [{"depth": 0, "price": 1.1013215859030836}],
        "last": {"price": 0.8988},
        "high": {"daily": null},
        "low":  {"daily": null}
      }
    }
  ]
})json";

// [CHURN 2026-10-26] GET /v1/pairs as served on 2026-10-26: entries 1, 2, 11
// and 21 of its list, whole -- an entry is a base and a quote, nothing more.
// Dexie prices BYC/wUSDC.b in wUSDC.b, and none of the ~400 entries read
// pairs them the other way round.  It names XCH the BASE of XCH/wUSDC.b and
// XCH/BYC: the opposite of the units /v1/markets serves their `last` in.
constexpr const char* kPairsOct26 = R"json({
  "success": true,
  "pairs": [
    {"base":  {"id": "xch", "code": "XCH", "name": "Chia"},
     "quote": {"id": "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
               "code": "wUSDC.b", "name": "Base warp.green USDC"}},
    {"base":  {"id": "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20",
               "code": "DBX", "name": "dexie bucks"},
     "quote": {"id": "xch", "code": "XCH", "name": "Chia"}},
    {"base":  {"id": "xch", "code": "XCH", "name": "Chia"},
     "quote": {"id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
               "code": "BYC", "name": "Bytecash"}},
    {"base":  {"id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
               "code": "BYC", "name": "Bytecash"},
     "quote": {"id": "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
               "code": "wUSDC.b", "name": "Base warp.green USDC"}}
  ]
})json";

// Entries Dexie has not been seen to serve, one defect each.  Only the last
// is usable: a code that is not a string costs the code, not the entry.
constexpr const char* kMalformedPairs = R"json([
  42,
  {"base": {"id": "aaaa"}},
  {"base": {"id": 7}, "quote": {"id": "bbbb"}},
  {"base": {"id": ""}, "quote": {"id": "bbbb"}},
  {"base": "aaaa", "quote": {"id": "bbbb"}},
  {"base": {"id": "aaaa", "code": 5}, "quote": {"id": "bbbb", "code": "TWO"}}
])json";

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
    // Volumes stay keyed as listed: XCH under "xch", BYC for the token.  The
    // base's is the key's here.
    EXPECT_DOUBLE_EQ(t.volume_xch_daily, 80.0);
    EXPECT_DOUBLE_EQ(t.volume_quote_daily, 100.0);
    EXPECT_DOUBLE_EQ(t.volume_base_daily, 80.0) << "XCH, the pair's base";

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
    EXPECT_DOUBLE_EQ(t.volume_base_daily, 100.0) << "BYC, the pair's base";
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
    // NOT price_last AS LISTED: Dexie serves 0.5 in both listings, in the
    // "xch" listing's units (XCH per BYC).  Read as listed, the BYC-keyed 0.5
    // sits outside its own 1.25-2.0 range; oriented as the market's, it is
    // the same 2.0 -- XchBasePairListedOnlyUnderTheCatKeyGetsTheMarketsLast,
    // and TODO S53.
}

// [round 3] Dexie can list one market under BOTH keys, and the saved response
// does so for XCH/BYC.  The listing found first is taken as canonical, so the
// search order decides `last`: the xch-keyed 0.5 orients to 2.0, while the
// BYC-keyed 0.5, taken as canonical, would pass through as 0.5.  An XCH
// market's canonical listing is the "xch" one -- Case A here, where the BASE
// is XCH -- and a CAT/CAT market's is the one under the asset Dexie prices it
// in [CHURN 2026-10-26] (the tests after this one).  Bid and ask agree either
// way, which is why they cannot catch a reordering -- `inverted`, the
// listing's key and `last` can.
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
    EXPECT_TRUE(match->canonical_listing);
    EXPECT_EQ(match->listed.pair_id, "xch");
    EXPECT_DOUBLE_EQ(match->oriented.price_last, 2.0)
        << "Case B would have returned the BYC-keyed listing's 0.5 verbatim";
    EXPECT_DOUBLE_EQ(match->oriented.best_bid, 1.451);
    EXPECT_DOUBLE_EQ(match->oriented.best_ask, 3.3333333333333335);
}

// [CHURN 2026-10-26] A CAT/CAT market listed under both keys: the listing
// under the asset Dexie prices it in is read -- for BYC/wUSDC.b, configured
// the way Dexie prices it, the QUOTE's, with or without /v1/pairs' answer.
// Case A first -- the order before this -- read the BYC-keyed listing and
// inverted its `last` to 1.1126, 24% above the 0.8988 both listings served;
// each switch between the two re-stamped the print as fresh and got our
// resting BYC/wUSDC.b asks cancelled as crossed.
TEST(DexieMarketTicker, CatCatWithBothListingsTheQuoteKeyedOneIsUsed)
{
    auto m = nlohmann::json::parse(kWusdcbKeyOct26);
    m[kByc] = nlohmann::json::parse(kBycKeyOct26).at(kByc);
    ASSERT_TRUE(m.contains(kWusdcb));
    ASSERT_TRUE(m.contains(kByc)) << "precondition: both listings present";

    for (const std::string& market_quote : {std::string{}, kWusdcb}) {
        SCOPED_TRACE(market_quote.empty() ? "no /v1/pairs answer"
                                          : "Dexie prices it in wUSDC.b");
        const auto match = orient_market_ticker(m, kByc, kWusdcb, market_quote);
        ASSERT_TRUE(match.has_value());
        EXPECT_FALSE(match->inverted) << "Case B: the wUSDC.b-keyed listing";
        EXPECT_TRUE(match->canonical_listing);
        EXPECT_EQ(match->listed.pair_id, kWusdcb);
        EXPECT_DOUBLE_EQ(match->oriented.price_last, 0.8988)
            << "between our own 0.896 and 0.900 fills; Case A gave 1.1126";
        EXPECT_DOUBLE_EQ(match->oriented.best_ask, 0.908) << "our own ask";
        EXPECT_DOUBLE_EQ(match->oriented.best_bid, 0.0) << "no bid listed";
    }
}

// The same market listed under its BASE only: Case A orients bid, ask, high
// and low, but `last` stays as served -- wUSDC.b per BYC, as in every listing
// of this market.  This pins the fallback, not a price anyone has seen:
// nothing shows Dexie serving the BYC-keyed listing without the other one.
TEST(DexieMarketTicker, CatCatListedOnlyUnderItsBaseGetsTheMarketsLast)
{
    const auto m = nlohmann::json::parse(kBycKeyOct26);
    for (const std::string& market_quote : {std::string{}, kWusdcb}) {
        SCOPED_TRACE(market_quote.empty() ? "no /v1/pairs answer"
                                          : "Dexie prices it in wUSDC.b");
        const auto match = orient_market_ticker(m, kByc, kWusdcb, market_quote);
        ASSERT_TRUE(match.has_value());
        EXPECT_TRUE(match->inverted);
        EXPECT_FALSE(match->canonical_listing);
        EXPECT_EQ(match->listed.pair_id, kByc);
        EXPECT_NEAR(match->oriented.best_ask, 0.908, 1e-12)
            << "our own ask, 1 / the wUSDC.b bid";
        EXPECT_NEAR(match->oriented.best_bid, 1.0 / 710.545999, 1e-15);
        EXPECT_DOUBLE_EQ(match->oriented.price_last, 0.8988)
            << "inverting it as Case A does gives the 1.1126 of the defect";
    }
}

// An XCH/BYC ticker with only the BYC-keyed listing: bid, ask, high and low
// as listed (BYC per XCH), and `last` -- served as 0.5 XCH per BYC, the
// units of the "xch" listing -- turned into 2.0 BYC per XCH, inside the
// listing's own 1.25-2.0 range and equal to what the xch-keyed listing gives.
TEST(DexieMarketTicker, XchBasePairListedOnlyUnderTheCatKeyGetsTheMarketsLast)
{
    const auto m = nlohmann::json::parse(kBycKeyMarkets);
    const auto match = orient_market_ticker(m, "xch", kByc);
    ASSERT_TRUE(match.has_value());
    EXPECT_FALSE(match->inverted) << "Case B: the BYC-keyed listing, direct";
    EXPECT_FALSE(match->canonical_listing);
    EXPECT_EQ(match->listed.pair_id, kByc);
    EXPECT_DOUBLE_EQ(match->oriented.best_bid, 1.451);
    EXPECT_DOUBLE_EQ(match->oriented.best_ask, 3.3333333333333335);
    EXPECT_DOUBLE_EQ(match->oriented.price_high, 2.0);
    EXPECT_DOUBLE_EQ(match->oriented.price_low, 1.25);
    EXPECT_DOUBLE_EQ(match->oriented.price_last, 2.0);
}

// [CHURN 2026-10-26] A CAT/CAT pair configured against Dexie's orientation --
// wUSDC.b/BYC, a market Dexie prices in wUSDC.b -- reads the canonical
// listing once /v1/pairs has said which that is: the wUSDC.b-keyed one,
// inverted, `last` included.  Taking the configured quote as canonical would
// read the BYC-keyed listing and pass its 0.8988 wUSDC.b per BYC through as
// if it were BYC per wUSDC.b.
TEST(DexieMarketTicker, ReversedCatCatWithDexiesPairsReadsTheCanonicalListing)
{
    auto m = nlohmann::json::parse(kWusdcbKeyOct26);
    m[kByc] = nlohmann::json::parse(kBycKeyOct26).at(kByc);
    ASSERT_TRUE(m.contains(kWusdcb));
    ASSERT_TRUE(m.contains(kByc)) << "precondition: both listings present";

    const auto match = orient_market_ticker(m, kWusdcb, kByc, kWusdcb);
    ASSERT_TRUE(match.has_value());
    EXPECT_TRUE(match->inverted) << "Case A: the wUSDC.b-keyed listing";
    EXPECT_TRUE(match->canonical_listing);
    EXPECT_EQ(match->listed.pair_id, kWusdcb);
    EXPECT_NEAR(match->oriented.price_last, 1.0 / 0.8988, 1e-12)
        << "BYC per wUSDC.b";
    EXPECT_NEAR(match->oriented.best_bid, 1.0 / 0.908, 1e-12)
        << "our own 0.908 BYC ask is a bid for wUSDC.b";
    EXPECT_DOUBLE_EQ(match->oriented.best_ask, 0.0) << "no BYC bid listed";
}

// The same reversed pair with only the BYC-keyed listing: read direct, and
// `last` still turned into BYC per wUSDC.b.
TEST(DexieMarketTicker, ReversedCatCatListedOnlyUnderItsQuoteGetsTheMarketsLast)
{
    const auto m = nlohmann::json::parse(kBycKeyOct26);
    const auto match = orient_market_ticker(m, kWusdcb, kByc, kWusdcb);
    ASSERT_TRUE(match.has_value());
    EXPECT_FALSE(match->inverted) << "Case B: the BYC-keyed listing, direct";
    EXPECT_FALSE(match->canonical_listing);
    EXPECT_EQ(match->listed.pair_id, kByc);
    EXPECT_DOUBLE_EQ(match->oriented.best_bid, 1.1013215859030836);
    EXPECT_DOUBLE_EQ(match->oriented.best_ask, 710.545999);
    EXPECT_NEAR(match->oriented.price_last, 1.0 / 0.8988, 1e-12);
}

// `market_quote` only ever decides between a CAT/CAT market's own two
// listings.  An XCH market's canonical listing is the "xch" one whatever it
// says -- /v1/pairs names XCH the base of XCH/BYC, while /v1/markets serves
// that market's `last` in XCH per BYC -- and one naming neither asset is no
// answer at all.
TEST(DexieMarketTicker, AMarketQuoteIsIgnoredForAnXchPairOrWhenItNamesNeitherAsset)
{
    auto xch_byc = nlohmann::json::parse(kXchMarkets);
    xch_byc[kByc] = nlohmann::json::parse(kBycKeyMarkets).at(kByc);
    const auto xch = orient_market_ticker(xch_byc, "xch", kByc, kByc);
    ASSERT_TRUE(xch.has_value());
    EXPECT_TRUE(xch->inverted);
    EXPECT_EQ(xch->listed.pair_id, "xch");
    EXPECT_DOUBLE_EQ(xch->oriented.price_last, 2.0);

    auto byc_wusdcb = nlohmann::json::parse(kWusdcbKeyOct26);
    byc_wusdcb[kByc] = nlohmann::json::parse(kBycKeyOct26).at(kByc);
    const auto cat = orient_market_ticker(byc_wusdcb, kByc, kWusdcb, kDbx);
    ASSERT_TRUE(cat.has_value());
    EXPECT_FALSE(cat->inverted);
    EXPECT_EQ(cat->listed.pair_id, kWusdcb);
    EXPECT_DOUBLE_EQ(cat->oriented.price_last, 0.8988);
}

// An XCH-QUOTED pair listed under both keys reads the "xch" listing too --
// Case B for this pair -- and with it the right `last`.  Case A would read the
// BYC-keyed listing and invert its 0.5 to 2.0 XCH per BYC, outside even the
// xch-keyed listing's own 0.5-0.8 daily range.  The listing read also decides
// which volume field holds the base's: here the token's 100 BYC, not the 80
// XCH under the key, which is what the engine used to take for every pair.
TEST(DexieMarketTicker, XchQuotedPairWithBothListingsTheXchKeyedOneIsUsed)
{
    auto m = nlohmann::json::parse(kXchMarkets);
    m[kByc] = nlohmann::json::parse(kBycKeyMarkets).at(kByc);
    ASSERT_TRUE(m.contains("xch"));
    ASSERT_TRUE(m.contains(kByc)) << "precondition: both listings present";

    const auto match = orient_market_ticker(m, kByc, "xch");
    ASSERT_TRUE(match.has_value());
    EXPECT_FALSE(match->inverted) << "Case B: the xch-keyed listing, direct";
    EXPECT_TRUE(match->canonical_listing);
    EXPECT_EQ(match->listed.pair_id, "xch");
    EXPECT_DOUBLE_EQ(match->oriented.price_last, 0.5)
        << "Case A would have inverted the BYC-keyed listing's 0.5 to 2.0";
    EXPECT_DOUBLE_EQ(match->oriented.best_bid, 0.3);
    EXPECT_DOUBLE_EQ(match->oriented.best_ask, 0.6891798759476223);
    EXPECT_DOUBLE_EQ(match->oriented.volume_base_daily, 100.0)
        << "BYC, the pair's base";
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

// [CHURN 2026-10-26] GET /v1/pairs, parsed leniently: one bad entry costs
// only itself, never every CAT/CAT ticker its orientation.
TEST(DexieMarketTicker, PairsResponseParsesAndSkipsMalformedEntries)
{
    const auto pairs = parse_pairs_response(nlohmann::json::parse(kPairsOct26));
    ASSERT_EQ(pairs.size(), 4u);
    EXPECT_EQ(pairs[0].base.id, "xch");
    EXPECT_EQ(pairs[0].quote.id, kWusdcb);
    EXPECT_EQ(pairs[3].base.id, kByc);
    EXPECT_EQ(pairs[3].base.code, "BYC");
    EXPECT_EQ(pairs[3].base.name, "Bytecash");
    EXPECT_EQ(pairs[3].quote.id, kWusdcb);
    EXPECT_EQ(pairs[3].quote.code, "wUSDC.b");
    EXPECT_EQ(pairs[3].quote.name, "Base warp.green USDC");

    auto body = nlohmann::json::parse(kPairsOct26);
    for (const auto& bad : nlohmann::json::parse(kMalformedPairs)) {
        body["pairs"].push_back(bad);
    }
    const auto kept = parse_pairs_response(body);
    ASSERT_EQ(kept.size(), 5u) << "the four real entries and the last test one";
    EXPECT_EQ(kept[4].base.id, "aaaa");
    EXPECT_EQ(kept[4].base.code, "");
    EXPECT_EQ(kept[4].quote.id, "bbbb");
    EXPECT_EQ(kept[4].quote.code, "TWO");

    EXPECT_TRUE(parse_pairs_response(nlohmann::json::array()).empty());
    EXPECT_TRUE(parse_pairs_response(nlohmann::json::parse(R"({"success": true})"))
                    .empty());
    EXPECT_TRUE(parse_pairs_response(nlohmann::json::parse(R"({"pairs": {}})"))
                    .empty());
}

TEST(DexieMarketTicker, CatMarketQuoteIsThePairsQuoteInEitherOrder)
{
    const auto pairs = parse_pairs_response(nlohmann::json::parse(kPairsOct26));
    EXPECT_EQ(cat_market_quote(pairs, kByc, kWusdcb).value_or("none"), kWusdcb)
        << "BYC/wUSDC.b as configured, and as Dexie prices it";
    EXPECT_EQ(cat_market_quote(pairs, kWusdcb, kByc).value_or("none"), kWusdcb)
        << "asked the other way round, the same market and the same answer";

    auto twice = pairs;
    twice.push_back(pairs[3]);
    EXPECT_EQ(cat_market_quote(twice, kWusdcb, kByc).value_or("none"), kWusdcb)
        << "one orientation listed twice is still one answer";
}

TEST(DexieMarketTicker, CatMarketQuoteIsUnknownForXchMarketsUnlistedPairsAndBothOrders)
{
    const auto pairs = parse_pairs_response(nlohmann::json::parse(kPairsOct26));
    // /v1/pairs lists XCH/wUSDC.b and XCH/BYC, but its orientation is not
    // the units of an XCH market's `last`, so it is not consulted for one.
    EXPECT_FALSE(cat_market_quote(pairs, "xch", kWusdcb).has_value());
    EXPECT_FALSE(cat_market_quote(pairs, kByc, "xch").has_value());
    EXPECT_FALSE(cat_market_quote(pairs, kByc, kDbx).has_value())
        << "no entry pairs them";
    EXPECT_FALSE(cat_market_quote(pairs, kByc, kByc).has_value());
    EXPECT_FALSE(cat_market_quote(pairs, "", kWusdcb).has_value());

    // Both orders listed: no single answer, so the configured quote stands.
    auto both = pairs;
    PairInfo reversed;
    reversed.base.id  = kWusdcb;
    reversed.quote.id = kByc;
    both.push_back(reversed);
    EXPECT_FALSE(cat_market_quote(both, kByc, kWusdcb).has_value());
    EXPECT_FALSE(cat_market_quote(both, kWusdcb, kByc).has_value());
}

TEST(DexieMarketTicker, PairsCacheFetchesEveryTtlAndRetriesAfterAFailure)
{
    using namespace std::chrono_literals;
    constexpr auto kTtl   = PairsOrientationCache::kTtl;
    constexpr auto kRetry = PairsOrientationCache::kRetry;
    PairsOrientationCache cache;
    const auto t0 = PairsOrientationCache::Clock::time_point{} + 1000h;

    EXPECT_TRUE(cache.claim_fetch(t0)) << "due before the first fetch";
    EXPECT_FALSE(cache.claim_fetch(t0))
        << "claimed: a second ticker in the same cycle does not fetch too";
    EXPECT_FALSE(cache.quote_for(kByc, kWusdcb).has_value())
        << "nothing held yet: the configured quote stands";

    cache.store(parse_pairs_response(nlohmann::json::parse(kPairsOct26)), t0);
    EXPECT_EQ(cache.quote_for(kByc, kWusdcb).value_or("none"), kWusdcb);
    EXPECT_FALSE(cache.claim_fetch(t0 + kTtl - 1s)) << "a good read lasts kTtl";
    const auto t1 = t0 + kTtl;
    EXPECT_TRUE(cache.claim_fetch(t1));

    cache.fail(t1);
    EXPECT_EQ(cache.quote_for(kByc, kWusdcb).value_or("none"), kWusdcb)
        << "a failed read keeps the last good entries";
    EXPECT_FALSE(cache.claim_fetch(t1 + kRetry - 1s));
    const auto t2 = t1 + kRetry;
    EXPECT_TRUE(cache.claim_fetch(t2)) << "a failed read is retried after kRetry";

    // A claimed fetch that never reports back does not hold off the next
    // one for longer than kRetry.
    EXPECT_FALSE(cache.claim_fetch(t2 + kRetry - 1s));
    EXPECT_TRUE(cache.claim_fetch(t2 + kRetry));
}
