// test_synthetic_price.cpp -- [SYNTH 2026-10-22] the synthetic quoting price
// for revive pairs with an unlisted leg (xop/strategy/synthetic_price.hpp),
// Dexie's trade-history parsing, and the market-data rule that a synthetic
// reference moves the published mid but never counts as evidence.

#include <gtest/gtest.h>

#include "xop/execution/market_data.hpp"
#include "xop/rpc/dexie_client.hpp"
#include "xop/state.hpp"
#include "xop/strategy/synthetic_price.hpp"
#include "xop/types.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using xop::synth::PriceSource;
using xop::synth::TapeTrade;
using Clock = std::chrono::system_clock;

const Clock::time_point kNow = Clock::time_point{} + 480'000h;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

}  // namespace

// ---------------------------------------------------------------------------
// blend_sources
// ---------------------------------------------------------------------------

TEST(SynthBlend, OneSourceIsItself)
{
    const auto b = xop::synth::blend_sources({{1.835, 300.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->price, 1.835, 1e-12);
    EXPECT_NEAR(b->sigma_bps, 300.0, 1e-9);
}

TEST(SynthBlend, AgreeingSourcesTightenTheErrorBar)
{
    const auto b = xop::synth::blend_sources({{1.835, 300.0}, {1.835, 300.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->price, 1.835, 1e-12);
    EXPECT_NEAR(b->sigma_bps, 300.0 / std::sqrt(2.0), 1e-6);
}

TEST(SynthBlend, DisagreeingSourcesWidenToTheirSpread)
{
    // XCH/BYC on 2026-10-22: BYC's $1 par puts it at 1.426, the tape at
    // 1.835 -- 25% apart. Two "confident" sources must not average into a
    // confident middle.
    const auto b = xop::synth::blend_sources({{1.426, 160.0}, {1.835, 250.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_GT(b->price, 1.426);
    EXPECT_LT(b->price, 1.835);
    EXPECT_GT(b->sigma_bps, 1000.0)
        << "the blend's error bar must reflect the 25% disagreement";
}

TEST(SynthBlend, TheTighterSourceCarriesMoreWeight)
{
    const auto b = xop::synth::blend_sources({{1.0, 100.0}, {2.0, 1000.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_LT(b->price, 1.1);
}

TEST(SynthBlend, UnusableSourcesAreSkippedAndNoneIsNothing)
{
    EXPECT_FALSE(xop::synth::blend_sources({}).has_value());
    EXPECT_FALSE(xop::synth::blend_sources({{0.0, 100.0}, {kNaN, 100.0}})
                     .has_value());
    const auto b = xop::synth::blend_sources({{-1.0, 100.0}, {2.0, 100.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->price, 2.0, 1e-12);
}

TEST(SynthBlend, TheErrorBarIsCapped)
{
    const auto b = xop::synth::blend_sources({{1.0, 100.0}, {100.0, 100.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_LE(b->sigma_bps, xop::synth::kMaxSigmaBps);
}

// ---------------------------------------------------------------------------
// cross_source / aged_source
// ---------------------------------------------------------------------------

TEST(SynthCross, XchOverACatIsTheCatsInverse)
{
    // XCH/BYC = XCH-in-XCH / BYC-in-XCH = 1 / 0.545 BYC per XCH.
    const auto c = xop::synth::cross_source({1.0, 0.0}, {0.545, 300.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->price, 1.0 / 0.545, 1e-12);
    EXPECT_NEAR(c->sigma_bps, 300.0, 1e-9);
}

TEST(SynthCross, TwoCatsAddTheirErrorsInQuadrature)
{
    // BYC/wUSDC.b from BYC 0.545 XCH and wUSDC.b 0.70 XCH.
    const auto c = xop::synth::cross_source({0.545, 300.0}, {0.70, 400.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->price, 0.545 / 0.70, 1e-12);
    EXPECT_NEAR(c->sigma_bps, 500.0, 1e-9);
}

TEST(SynthCross, AnUnusableLegIsNothing)
{
    EXPECT_FALSE(xop::synth::cross_source({1.0, 0.0}, {0.0, 100.0}).has_value());
    EXPECT_FALSE(xop::synth::cross_source({kNaN, 0.0}, {1.0, 100.0}).has_value());
}

TEST(SynthAged, TheErrorBarGrowsWithAgeAndIsCapped)
{
    const PriceSource last{1.835, 300.0};
    EXPECT_NEAR(xop::synth::aged_source(last, 0.0, 25.0).sigma_bps, 300.0, 1e-9);
    EXPECT_NEAR(xop::synth::aged_source(last, 10.0, 25.0).sigma_bps, 550.0, 1e-9);
    EXPECT_NEAR(xop::synth::aged_source(last, 10.0, 25.0).price, 1.835, 1e-12);
    EXPECT_NEAR(xop::synth::aged_source(last, 1e6, 25.0).sigma_bps,
                xop::synth::kMaxSigmaBps, 1e-9);
    EXPECT_NEAR(xop::synth::aged_source(last, -5.0, 25.0).sigma_bps, 300.0, 1e-9);
}

// ---------------------------------------------------------------------------
// tape_source
// ---------------------------------------------------------------------------

TEST(SynthTape, TheLiveBycTapeReadsAboutPointFiveFour)
{
    // BYC_XCH on Dexie, 2026-10-21/22: four trades, the newest ~5h old.
    const std::vector<TapeTrade> t{
        {0.545454545455, 1.1,   kNow - 5h},
        {0.547445255474, 5.48,  kNow - 20h},
        {0.536193029491, 5.595, kNow - 23h},
        {0.545454545455, 5.5,   kNow - 23h},
    };
    const auto s = xop::synth::tape_source(t, kNow);
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(s->price, 0.5431, 0.0005);
    // hypot(200 base, ~89 dispersion) + 5h x 25 = ~344.
    EXPECT_NEAR(s->sigma_bps, 343.75, 0.5);
}

TEST(SynthTape, TradesOutsideTheWindowOrInTheFutureAreIgnored)
{
    const std::vector<TapeTrade> t{
        {0.50, 10.0, kNow - 100h},   // outside 72h
        {0.60, 10.0, kNow + 1h},     // clock skew
        {0.55, 1.0,  kNow - 1h},
    };
    const auto s = xop::synth::tape_source(t, kNow);
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(s->price, 0.55, 1e-12);
}

TEST(SynthTape, NoUsableTradeIsNothing)
{
    EXPECT_FALSE(xop::synth::tape_source({}, kNow).has_value());
    EXPECT_FALSE(xop::synth::tape_source({{0.0, 1.0, kNow - 1h},
                                          {0.5, 0.0, kNow - 1h},
                                          {0.5, 1.0, kNow - 200h}},
                                         kNow)
                     .has_value());
}

TEST(SynthTape, ScatteredTradesWidenTheErrorBar)
{
    const std::vector<TapeTrade> tight{{0.54, 1.0, kNow}, {0.55, 1.0, kNow}};
    const std::vector<TapeTrade> wide{{0.40, 1.0, kNow}, {0.70, 1.0, kNow}};
    EXPECT_GT(xop::synth::tape_source(wide, kNow)->sigma_bps,
              xop::synth::tape_source(tight, kNow)->sigma_bps + 1000.0);
}

// ---------------------------------------------------------------------------
// half_spread_for_sigma
// ---------------------------------------------------------------------------

TEST(SynthSpread, CoversTwoErrorBarsBetweenTheFloorAndTheCap)
{
    EXPECT_DOUBLE_EQ(xop::synth::half_spread_for_sigma(300.0, 100.0), 300.0);
    EXPECT_DOUBLE_EQ(xop::synth::half_spread_for_sigma(300.0, 600.0), 1200.0);
    EXPECT_DOUBLE_EQ(xop::synth::half_spread_for_sigma(300.0, 4000.0), 5000.0);
    EXPECT_DOUBLE_EQ(xop::synth::half_spread_for_sigma(300.0, kNaN), 300.0);
}

// ---------------------------------------------------------------------------
// Dexie historical_trades parsing
// ---------------------------------------------------------------------------

TEST(DexieHistoricalTrades, ParsesTheLiveShape)
{
    const auto j = nlohmann::json::parse(R"({
        "success": true, "ticker_id": "ae15..._xch",
        "trades": [
            {"trade_id": "a", "price": "0.545454545455", "base_volume": "1.1",
             "target_volume": "0.6", "trade_timestamp": 1792635467000, "type": "buy"},
            {"trade_id": "b", "price": "0.536193029491", "base_volume": "5.595",
             "target_volume": "3", "trade_timestamp": 1792569992000, "type": "sell"}
        ]})");
    const auto t = xop::rpc::parse_historical_trades(j);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_NEAR(t[0].price, 0.545454545455, 1e-12);
    EXPECT_NEAR(t[0].base_volume, 1.1, 1e-12);
    EXPECT_NEAR(t[0].target_volume, 0.6, 1e-12);
    EXPECT_EQ(t[0].timestamp_ms, 1792635467000LL);
}

TEST(DexieHistoricalTrades, ANonSuccessOrBadEntriesYieldNothing)
{
    EXPECT_TRUE(xop::rpc::parse_historical_trades(nlohmann::json::parse(
        R"({"success": false, "error_message": "invalid ticker_id"})")).empty());
    EXPECT_TRUE(xop::rpc::parse_historical_trades(nlohmann::json::parse(
        R"({"success": true})")).empty());
    const auto t = xop::rpc::parse_historical_trades(nlohmann::json::parse(R"({
        "success": true, "trades": [
            {"price": "0", "base_volume": "1", "trade_timestamp": 1},
            {"price": "0.5", "base_volume": "-1", "trade_timestamp": 1},
            {"price": "abc", "base_volume": "1", "trade_timestamp": 1},
            {"price": 0.5, "base_volume": 2, "trade_timestamp": 0},
            {"price": 0.5, "base_volume": 2, "trade_timestamp": 5}
        ]})"));
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].timestamp_ms, 5);
}

TEST(DexieHistoricalTrades, TheV2RootFollowsTheConfiguredV1Base)
{
    EXPECT_EQ(xop::rpc::dexie_v2_base_url("https://api.dexie.space/v1"),
              "https://api.dexie.space/v2");
    EXPECT_EQ(xop::rpc::dexie_v2_base_url("https://api.dexie.space/v1/"),
              "https://api.dexie.space/v2");
    EXPECT_EQ(xop::rpc::dexie_v2_base_url("https://api-testnet.dexie.space"),
              "https://api-testnet.dexie.space/v2");
}

// ---------------------------------------------------------------------------
// A synthetic reference moves the published mid and is never evidence
// ---------------------------------------------------------------------------

namespace {
xop::MarketDataConfig synth_cfg()
{
    xop::MarketDataConfig cfg;
    cfg.min_competitor_offer_size = 0;
    return cfg;
}
}  // namespace

TEST(SyntheticReference, AnEmptyBookPublishesItsMidButNoValuationGrade)
{
    xop::State state;
    xop::MarketDataFeed feed(synth_cfg(), state);
    feed.ingest_block_height(100);
    feed.ingest_cex_reference("XCH/BYC", 1.835, Clock::now(), /*synthetic=*/true);
    feed.refresh({"XCH/BYC"});

    const auto snap = state.get_market("XCH/BYC");
    EXPECT_NEAR(static_cast<double>(snap.mid_price)
                    / static_cast<double>(xop::kMojosPerXch), 1.835, 1e-6)
        << "the pair must have a mid to quote from";
    EXPECT_FALSE(snap.mid_valuation_grade)
        << "a synthetic price must never mark equity";
    EXPECT_FALSE(feed.get_cex_reference("XCH/BYC").has_value())
        << "nor stand in for a CEX reference";
}

TEST(SyntheticReference, ARealReferenceStillGrantsGrade)
{
    // The counterfactual: the same price as a real CEX leg is graded.
    xop::State state;
    xop::MarketDataFeed feed(synth_cfg(), state);
    feed.ingest_block_height(100);
    feed.ingest_cex_reference("XCH/BYC", 1.835, Clock::now());
    feed.refresh({"XCH/BYC"});

    EXPECT_TRUE(state.get_market("XCH/BYC").mid_valuation_grade);
    EXPECT_TRUE(feed.get_cex_reference("XCH/BYC").has_value());
}

TEST(SyntheticReference, ARealReferenceClearsTheSyntheticMark)
{
    xop::State state;
    xop::MarketDataFeed feed(synth_cfg(), state);
    feed.ingest_block_height(100);
    feed.ingest_cex_reference("XCH/BYC", 1.835, Clock::now(), true);
    feed.ingest_cex_reference("XCH/BYC", 1.835, Clock::now(), false);
    feed.refresh({"XCH/BYC"});
    EXPECT_TRUE(feed.get_cex_reference("XCH/BYC").has_value());
}
