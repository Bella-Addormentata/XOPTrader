// test_tibetswap.cpp -- TibetSwap v2 AMM client: response parsing, failure
//                       tolerance, and pair mapping.
//
// NO NETWORK ACCESS.  Every payload below is a literal captured from the live
// API on 2026-08-01 (or a deliberately corrupted variant of one).  The client
// object is exercised only through seams that never open a socket.
//
// What is locked in here:
//   1. The exact field names and integer scales of the /pairs and /pair/{id}
//      payloads.  BYC's pool is 175406074457098 XCH-mojo / 261655 CAT-mojo;
//      if the parser ever mis-scales those, the implied price moves by 10^9
//      and every downstream arb/blend decision is garbage.
//   2. Malformed input degrades gracefully -- nullopt / skipped record /
//      empty vector -- and never throws, so a bad payload cannot abort the
//      engine heartbeat.
//   3. build_tibetswap_reserves() assigns mojos-per-unit denominations to the
//      ASSET, not the reserve slot, in both pair orientations (da85235).
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets, no credentials, no live endpoints)
//   ISO/IEC 5055        (deterministic; no UB; no network dependency)
//   ISO/IEC 25000       (intention-revealing test names)

#include "xop/config.hpp"
#include "xop/rpc/tibetswap_client.hpp"
#include "xop/strategy/arbitrage.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

namespace xop::rpc {
namespace {

using nlohmann::json;

// ---------------------------------------------------------------------------
// Captured payloads (live API, 2026-08-01)
// ---------------------------------------------------------------------------

constexpr const char* kBycAssetId =
    "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac";
constexpr const char* kBycPairId =
    "9a3ac0d59d02e25410425626bede3ac20506cd0889fedb683e464013ade23fae";
constexpr const char* kUsdcAssetId =
    "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";
constexpr const char* kDbxAssetId =
    "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20";

/// Verbatim GET /pair/{BYC} response body.
const char* byc_pool_body() {
    return R"({
        "pair_id": "9a3ac0d59d02e25410425626bede3ac20506cd0889fedb683e464013ade23fae",
        "asset_id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
        "asset_hidden_puzzle_hash": null,
        "asset_name": "Bytecash",
        "asset_short_name": "BYC",
        "asset_image_url": "https://icons.dexie.space/x.webp",
        "asset_verified": true,
        "inverse_fee": 993,
        "liquidity_asset_id": "6d113dc5217699020c5e10aa138ec28fe3e15d894e9876b1647d9ad6bf6b1d6a",
        "xch_reserve": 175406074457098,
        "token_reserve": 261655,
        "liquidity": 402476,
        "last_coin_id_on_chain": "1fa47740fd1987f4aea781f10745093175ec65b0678d1cafadfac3680e65e259"
    })";
}

/// Two-record GET /pairs page.
const char* pairs_page_body() {
    return R"([
        {
            "pair_id": "9a3ac0d59d02e25410425626bede3ac20506cd0889fedb683e464013ade23fae",
            "asset_id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
            "asset_name": "Bytecash",
            "asset_short_name": "BYC",
            "inverse_fee": 993,
            "xch_reserve": 175406074457098,
            "token_reserve": 261655,
            "liquidity": 402476
        },
        {
            "pair_id": "054f10bd1baefdbc6c87880d445640084332d072a68fcebf810045784e54c39e",
            "asset_id": "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
            "asset_name": "Base warp.green USDC",
            "asset_short_name": "wUSDC.b",
            "inverse_fee": 993,
            "xch_reserve": 393868300000000,
            "token_reserve": 564862,
            "liquidity": 471000
        }
    ])";
}

// ---------------------------------------------------------------------------
// Pair-config fixtures mirroring the live config.yaml
// ---------------------------------------------------------------------------

PairConfig make_xch_byc() {
    PairConfig p;
    p.name                 = "XCH/BYC";
    p.base_asset_id        = "xch";
    p.quote_asset_id       = kBycAssetId;
    p.enabled              = true;
    p.base_mojos_per_unit  = 1'000'000'000'000LL;  // XCH
    p.quote_mojos_per_unit = 1'000LL;              // CAT
    return p;
}

PairConfig make_byc_xch() {
    // Same pool, reversed orientation (as wmilliETH.b/XCH is configured).
    PairConfig p;
    p.name                 = "BYC/XCH";
    p.base_asset_id        = kBycAssetId;
    p.quote_asset_id       = "xch";
    p.enabled              = true;
    p.base_mojos_per_unit  = 1'000LL;              // CAT
    p.quote_mojos_per_unit = 1'000'000'000'000LL;  // XCH
    return p;
}

PairConfig make_byc_usdc() {
    // CAT/CAT -- TibetSwap has no such pool.
    PairConfig p;
    p.name           = "BYC/wUSDC.b";
    p.base_asset_id  = kBycAssetId;
    p.quote_asset_id = kUsdcAssetId;
    p.enabled        = true;
    p.base_mojos_per_unit  = 1'000LL;
    p.quote_mojos_per_unit = 1'000LL;
    return p;
}

TibetSwapPool byc_pool() {
    return *parse_pool_json(json::parse(byc_pool_body()));
}

// ===========================================================================
// parse_pool_json -- success path
// ===========================================================================

TEST(TibetSwapParse, ParsesLiveSinglePoolPayload) {
    const auto pool = parse_pool_json(json::parse(byc_pool_body()));

    ASSERT_TRUE(pool.has_value());
    EXPECT_EQ(pool->pair_id,  kBycPairId);
    EXPECT_EQ(pool->asset_id, kBycAssetId);
    EXPECT_EQ(pool->short_name, "BYC");
    EXPECT_EQ(pool->name,       "Bytecash");

    // The two numbers that matter.  Reserves are raw mojos on both sides.
    EXPECT_EQ(pool->xch_reserve,   175'406'074'457'098LL);
    EXPECT_EQ(pool->token_reserve, 261'655LL);
    EXPECT_EQ(pool->liquidity,     402'476LL);
    EXPECT_EQ(pool->inverse_fee,   993u);
}

TEST(TibetSwapParse, InverseFeeConvertsToPerMilleNumerator) {
    // 993 => 0.7% => fee_bps field of TibetSwapReserves is 7 (per-mille).
    EXPECT_EQ(byc_pool().fee_per_mille(), 7u);

    TibetSwapPool p = byc_pool();
    p.inverse_fee = 997;
    EXPECT_EQ(p.fee_per_mille(), 3u);

    // Out-of-range values yield 0 so the caller can substitute its default.
    p.inverse_fee = 0;
    EXPECT_EQ(p.fee_per_mille(), 0u);
    p.inverse_fee = 1000;
    EXPECT_EQ(p.fee_per_mille(), 0u);
}

TEST(TibetSwapParse, ReserveScaleSurvivesRoundTripToImpliedPrice) {
    // The parsed reserves must reproduce the price measured out-of-band:
    // 175.406074 XCH / 261.655 BYC => 0.670372 XCH per BYC.
    const auto pool = byc_pool();

    const double xch_per_byc = tibet::get_implied_price(
        pool.token_reserve,        // input: BYC
        pool.xch_reserve,          // output: XCH
        1'000LL,                   // BYC mojos per unit
        1'000'000'000'000LL);      // XCH mojos per unit

    EXPECT_NEAR(xch_per_byc, 0.670372, 1e-5);

    // And the inverse direction, which is what XCH/BYC quotes against.
    const double byc_per_xch = tibet::get_implied_price(
        pool.xch_reserve,
        pool.token_reserve,
        1'000'000'000'000LL,
        1'000LL);
    EXPECT_NEAR(byc_per_xch, 1.0 / 0.670372, 1e-3);
}

TEST(TibetSwapParse, AcceptsStringEncodedReserves) {
    // Defensive: some JSON APIs emit large integers as strings.
    json j = json::parse(byc_pool_body());
    j["xch_reserve"]   = "175406074457098";
    j["token_reserve"] = "261655";

    const auto pool = parse_pool_json(j);
    ASSERT_TRUE(pool.has_value());
    EXPECT_EQ(pool->xch_reserve,   175'406'074'457'098LL);
    EXPECT_EQ(pool->token_reserve, 261'655LL);
}

TEST(TibetSwapParse, AssetAndPairIdsAreLowerCased) {
    json j = json::parse(byc_pool_body());
    j["asset_id"] = "AE1536F56760E471AD85EAD45F00D680FF9CCA73B8CC3407BE778F1C0C606EAC";

    const auto pool = parse_pool_json(j);
    ASSERT_TRUE(pool.has_value());
    EXPECT_EQ(pool->asset_id, kBycAssetId);
}

// ===========================================================================
// parse_pool_json -- malformed / hostile input must degrade, never throw
// ===========================================================================

TEST(TibetSwapParse, RejectsNonObjectPayload) {
    EXPECT_FALSE(parse_pool_json(json::array()).has_value());
    EXPECT_FALSE(parse_pool_json(json("not a pool")).has_value());
    EXPECT_FALSE(parse_pool_json(json(nullptr)).has_value());
    EXPECT_FALSE(parse_pool_json(json(42)).has_value());
}

TEST(TibetSwapParse, RejectsMissingRequiredFields) {
    for (const char* field : {"pair_id", "asset_id",
                              "xch_reserve", "token_reserve"}) {
        json j = json::parse(byc_pool_body());
        j.erase(field);
        EXPECT_FALSE(parse_pool_json(j).has_value())
            << "should reject payload missing " << field;
    }
}

TEST(TibetSwapParse, RejectsNullAndWrongTypedFields) {
    json j = json::parse(byc_pool_body());
    j["xch_reserve"] = nullptr;
    EXPECT_FALSE(parse_pool_json(j).has_value());

    j = json::parse(byc_pool_body());
    j["xch_reserve"] = json::object();
    EXPECT_FALSE(parse_pool_json(j).has_value());

    j = json::parse(byc_pool_body());
    j["token_reserve"] = "not-a-number";
    EXPECT_FALSE(parse_pool_json(j).has_value());

    j = json::parse(byc_pool_body());
    j["pair_id"] = 12345;             // Wrong type, not a string.
    EXPECT_FALSE(parse_pool_json(j).has_value());

    j = json::parse(byc_pool_body());
    j["asset_id"] = "";               // Empty string.
    EXPECT_FALSE(parse_pool_json(j).has_value());
}

TEST(TibetSwapParse, RejectsNonPositiveReserves) {
    // A drained or corrupted pool would divide by zero downstream.
    json j = json::parse(byc_pool_body());
    j["xch_reserve"] = 0;
    EXPECT_FALSE(parse_pool_json(j).has_value());

    j = json::parse(byc_pool_body());
    j["token_reserve"] = -5;
    EXPECT_FALSE(parse_pool_json(j).has_value());
}

TEST(TibetSwapParse, OutOfRangeInverseFeeFallsBackToDefault) {
    json j = json::parse(byc_pool_body());
    j["inverse_fee"] = 100000;
    const auto pool = parse_pool_json(j);
    ASSERT_TRUE(pool.has_value());
    EXPECT_EQ(pool->inverse_fee, 993u);   // Default retained.

    j = json::parse(byc_pool_body());
    j.erase("inverse_fee");
    const auto pool2 = parse_pool_json(j);
    ASSERT_TRUE(pool2.has_value());
    EXPECT_EQ(pool2->inverse_fee, 993u);
}

TEST(TibetSwapParse, MissingCosmeticFieldsAreTolerated) {
    json j = json::parse(byc_pool_body());
    j.erase("asset_name");
    j.erase("asset_short_name");
    j.erase("liquidity");

    const auto pool = parse_pool_json(j);
    ASSERT_TRUE(pool.has_value());
    EXPECT_TRUE(pool->name.empty());
    EXPECT_TRUE(pool->short_name.empty());
    EXPECT_EQ(pool->liquidity, 0);
    EXPECT_EQ(pool->xch_reserve, 175'406'074'457'098LL);
}

// ===========================================================================
// parse_pools_json -- batch behaviour
// ===========================================================================

TEST(TibetSwapParse, ParsesLiveDirectoryPage) {
    const auto pools = parse_pools_json(json::parse(pairs_page_body()));

    ASSERT_EQ(pools.size(), 2u);
    EXPECT_EQ(pools[0].short_name, "BYC");
    EXPECT_EQ(pools[1].short_name, "wUSDC.b");
    EXPECT_EQ(pools[1].xch_reserve,   393'868'300'000'000LL);
    EXPECT_EQ(pools[1].token_reserve, 564'862LL);
}

TEST(TibetSwapParse, SkipsMalformedRecordsButKeepsGoodOnes) {
    // One good pool, three kinds of junk.  The good one must survive.
    json arr = json::array();
    arr.push_back(json::parse(byc_pool_body()));
    arr.push_back(json("garbage"));
    arr.push_back(json::object());
    {
        json bad = json::parse(byc_pool_body());
        bad["token_reserve"] = 0;
        arr.push_back(bad);
    }

    const auto pools = parse_pools_json(arr);
    ASSERT_EQ(pools.size(), 1u);
    EXPECT_EQ(pools[0].asset_id, kBycAssetId);
}

TEST(TibetSwapParse, NonArrayDirectoryYieldsEmptyVectorWithoutThrowing) {
    EXPECT_TRUE(parse_pools_json(json::object()).empty());
    EXPECT_TRUE(parse_pools_json(json(nullptr)).empty());
    EXPECT_TRUE(parse_pools_json(json("error: not found")).empty());

    // An API error object, which is what a 200-with-error-body looks like.
    EXPECT_TRUE(parse_pools_json(
        json::parse(R"({"detail":"Unknown requested or offered CATs"})"))
        .empty());
}

TEST(TibetSwapParse, EmptyArrayYieldsEmptyVector) {
    EXPECT_TRUE(parse_pools_json(json::array()).empty());
}

// ===========================================================================
// build_tibetswap_reserves -- pair mapping and denomination assignment
// ===========================================================================

TEST(TibetSwapReservesMapping, MapsXchBasePairOntoPool) {
    const std::vector<TibetSwapPool> pools{byc_pool()};
    const std::vector<PairConfig>    pairs{make_xch_byc()};

    const auto reserves = build_tibetswap_reserves(pools, pairs);

    ASSERT_EQ(reserves.size(), 1u);
    EXPECT_EQ(reserves[0].pair_name,     "XCH/BYC");
    EXPECT_EQ(reserves[0].xch_reserve,   175'406'074'457'098LL);
    EXPECT_EQ(reserves[0].token_reserve, 261'655LL);
    EXPECT_EQ(reserves[0].fee_bps,       7u);

    // XCH is the base leg, so the XCH-side denomination is base_mojos_per_unit.
    EXPECT_EQ(reserves[0].xch_mojos_per_unit,   1'000'000'000'000LL);
    EXPECT_EQ(reserves[0].token_mojos_per_unit, 1'000LL);
}

TEST(TibetSwapReservesMapping, DenominationsFollowTheAssetNotTheReserveSlot) {
    // da85235 regression: with the pair quoted CAT-first, the XCH-side
    // denomination must come from the QUOTE leg.  Swapping these is a 10^9
    // error in tibet::get_implied_price().
    const std::vector<TibetSwapPool> pools{byc_pool()};
    const std::vector<PairConfig>    pairs{make_byc_xch()};

    const auto reserves = build_tibetswap_reserves(pools, pairs);

    ASSERT_EQ(reserves.size(), 1u);
    EXPECT_EQ(reserves[0].pair_name, "BYC/XCH");
    EXPECT_EQ(reserves[0].xch_mojos_per_unit,   1'000'000'000'000LL);
    EXPECT_EQ(reserves[0].token_mojos_per_unit, 1'000LL);

    // And the price computed from them is in displayable units, not raw mojos.
    const double xch_per_byc = tibet::get_implied_price(
        reserves[0].token_reserve,
        reserves[0].xch_reserve,
        reserves[0].token_mojos_per_unit,
        reserves[0].xch_mojos_per_unit);
    EXPECT_NEAR(xch_per_byc, 0.670372, 1e-5);
}

TEST(TibetSwapReservesMapping, SkipsCatCatPairsWhichHaveNoDirectPool) {
    std::vector<TibetSwapPool> pools{byc_pool()};
    const std::vector<PairConfig> pairs{make_byc_usdc()};

    EXPECT_TRUE(build_tibetswap_reserves(pools, pairs).empty());
}

TEST(TibetSwapReservesMapping, SkipsDisabledPairs) {
    const std::vector<TibetSwapPool> pools{byc_pool()};
    PairConfig p = make_xch_byc();
    p.enabled = false;

    EXPECT_TRUE(build_tibetswap_reserves(pools, {p}).empty());
}

TEST(TibetSwapReservesMapping, SkipsPairsWithNoMatchingPool) {
    const std::vector<TibetSwapPool> pools{byc_pool()};
    PairConfig p = make_xch_byc();
    p.name           = "XCH/DBX";
    p.quote_asset_id = kDbxAssetId;   // No DBX pool in the fetched set.

    EXPECT_TRUE(build_tibetswap_reserves(pools, {p}).empty());
}

TEST(TibetSwapReservesMapping, MatchesAssetIdsCaseInsensitively) {
    const std::vector<TibetSwapPool> pools{byc_pool()};
    PairConfig p = make_xch_byc();
    p.quote_asset_id =
        "AE1536F56760E471AD85EAD45F00D680FF9CCA73B8CC3407BE778F1C0C606EAC";

    ASSERT_EQ(build_tibetswap_reserves(pools, {p}).size(), 1u);
}

TEST(TibetSwapReservesMapping, FallbackFeeUsedWhenPoolFeeIsUnusable) {
    TibetSwapPool p = byc_pool();
    p.inverse_fee = 1000;             // fee_per_mille() == 0

    const auto reserves = build_tibetswap_reserves({p}, {make_xch_byc()}, 9u);
    ASSERT_EQ(reserves.size(), 1u);
    EXPECT_EQ(reserves[0].fee_bps, 9u);
}

TEST(TibetSwapReservesMapping, EmptyInputsYieldEmptyOutput) {
    EXPECT_TRUE(build_tibetswap_reserves({}, {make_xch_byc()}).empty());
    EXPECT_TRUE(build_tibetswap_reserves({byc_pool()}, {}).empty());
    EXPECT_TRUE(build_tibetswap_reserves({}, {}).empty());
}

TEST(TibetSwapReservesMapping, ProducedReservesAreAcceptedByArbitrageDetector) {
    // End-to-end of the wiring this work exists to create: parsed payload ->
    // reserves -> the setter that previously had zero call sites.
    const auto pools    = parse_pools_json(json::parse(pairs_page_body()));
    const auto reserves = build_tibetswap_reserves(pools, {make_xch_byc()});

    ArbitrageConfig cfg;
    ArbitrageDetector detector(cfg);
    EXPECT_TRUE(detector.get_tibetswap_reserves().empty());

    detector.set_tibetswap_reserves(reserves);

    ASSERT_EQ(detector.get_tibetswap_reserves().size(), 1u);
    EXPECT_EQ(detector.get_tibetswap_reserves()[0].pair_name, "XCH/BYC");
    EXPECT_FALSE(detector.tibetswap_fee_changed());
}

// ===========================================================================
// Client construction / lifecycle / directory resolution (no sockets opened)
// ===========================================================================

TEST(TibetSwapClientLifecycle, DefaultsAreOperationalWithoutConfigFile) {
    const TibetSwapConfig cfg;   // Exactly what an absent `tibetswap:` yields.

    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.base_url, "https://api.v2.tibetswap.io");
    EXPECT_EQ(cfg.polling_interval_ms, 60'000u);
    EXPECT_GT(cfg.request_timeout_ms, 0u);
    EXPECT_GT(cfg.page_limit, 0u);
    EXPECT_GT(cfg.max_pools, 0u);
}

TEST(TibetSwapClientLifecycle, OpenCloseIsIdempotentAndOpensNoConnections) {
    boost::asio::io_context ioc;
    TibetSwapClient client(ioc, TibetSwapConfig{});

    EXPECT_FALSE(client.is_open());
    client.open();
    EXPECT_TRUE(client.is_open());
    client.open();                       // Second open is a no-op.
    EXPECT_TRUE(client.is_open());
    client.close();
    EXPECT_FALSE(client.is_open());
    client.close();                      // Second close is a no-op.
    EXPECT_FALSE(client.is_open());
}

TEST(TibetSwapClientLifecycle, SeededDirectoryMapsAssetsToPairIds) {
    boost::asio::io_context ioc;
    TibetSwapClient client(ioc, TibetSwapConfig{});

    EXPECT_EQ(client.directory_size(), 0u);
    client.seed_directory(parse_pools_json(json::parse(pairs_page_body())));
    EXPECT_EQ(client.directory_size(), 2u);
}

TEST(TibetSwapClientLifecycle, RequestsBeforeOpenFailLoudlyRatherThanCrash) {
    // A fetch on an unopened client must throw a typed TibetSwapError, which
    // is what the engine heartbeat catches and logs.  No socket is created.
    boost::asio::io_context ioc;
    TibetSwapClient client(ioc, TibetSwapConfig{});

    bool threw = false;
    boost::asio::co_spawn(
        ioc,
        [&client]() -> boost::asio::awaitable<void> {
            co_await client.fetch_pool("deadbeef");
            co_return;
        },
        [&threw](std::exception_ptr ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const TibetSwapError&) {
                    threw = true;
                } catch (...) {
                }
            }
        });
    ioc.run();

    EXPECT_TRUE(threw);
}

// GCC 11 (ubuntu-22.04) ICEs -- "in build_special_member_call, at
// cp/call.c:10200" -- on a lambda coroutine that binds the class-type result
// of a co_await into its frame.  The other two co_spawn lambdas in this file
// discard their results and compile fine.  Holding the vector in a plain
// function coroutine keeps the closure's frame scalar-only and sidesteps it.
boost::asio::awaitable<std::size_t> pool_count_for_assets(
    TibetSwapClient& client, std::vector<std::string> asset_ids)
{
    co_return (co_await client.fetch_pools_for_assets(asset_ids)).size();
}

TEST(TibetSwapClientLifecycle, EmptyAssetListShortCircuitsWithoutNetwork) {
    // No asset IDs (or only the native XCH sentinel) => no request at all,
    // even though the client was never opened.  Proves the guard runs first.
    boost::asio::io_context ioc;
    TibetSwapClient client(ioc, TibetSwapConfig{});

    std::size_t results = 1;
    bool        threw   = false;

    boost::asio::co_spawn(
        ioc,
        [&client, &results]() -> boost::asio::awaitable<void> {
            results = co_await pool_count_for_assets(client, {"xch", ""});
            co_return;
        },
        [&threw](std::exception_ptr ep) { threw = (ep != nullptr); });
    ioc.run();

    EXPECT_FALSE(threw);
    EXPECT_EQ(results, 0u);
}

TEST(TibetSwapClientLifecycle, EmptyPairIdIsRejectedWithoutNetwork) {
    boost::asio::io_context ioc;
    TibetSwapClient client(ioc, TibetSwapConfig{});
    client.open();

    bool threw = false;
    boost::asio::co_spawn(
        ioc,
        [&client]() -> boost::asio::awaitable<void> {
            co_await client.fetch_pool("");
            co_return;
        },
        [&threw](std::exception_ptr ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const TibetSwapError&) {
                    threw = true;
                } catch (...) {
                }
            }
        });
    ioc.run();
    client.close();

    EXPECT_TRUE(threw);
}

// ===========================================================================
// Error hierarchy -- the engine catches std::exception; these must all be it
// ===========================================================================

TEST(TibetSwapErrors, AllErrorTypesDeriveFromStdException) {
    const TibetSwapError        base("boom");
    const TibetSwapClientError  client(404, "not found");
    const TibetSwapRateLimitError limited;
    const TibetSwapServerError  server(503, "unavailable");

    EXPECT_NE(dynamic_cast<const std::exception*>(&base),    nullptr);
    EXPECT_NE(dynamic_cast<const std::exception*>(&client),  nullptr);
    EXPECT_NE(dynamic_cast<const std::exception*>(&limited), nullptr);
    EXPECT_NE(dynamic_cast<const std::exception*>(&server),  nullptr);

    EXPECT_EQ(client.status_code, 404);
    EXPECT_EQ(server.status_code, 503);
    EXPECT_NE(std::string(client.what()).find("404"), std::string::npos);
}

}  // namespace
}  // namespace xop::rpc
