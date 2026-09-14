// ---------------------------------------------------------------------------
// Raw ticker provenance -- PR #159, round 2.
//
// dex_best_bid/ask have two writers: ingest_dexie (the raw Dexie ticker, which
// includes OUR OWN resting offers and is never screened offer by offer) and
// ingest_competing_offers (the dust- and outlier-filtered third-party book).
// PairState::bbo_from_filtered_book records which one wrote the values there.
//
// THE HAZARD #159 CREATED.  Before it, the ticker's sides were read backwards,
// so an ordinary raw book LOOKED CROSSED -- and that alone kept it out of two
// sources that never asked where the prices came from:
//   * get_fair_value_inputs (its ask >= bid test), which feeds the fair-value
//     solve and the quote-centre blend;
//   * refresh()'s published spread_bps (compute_spread_bps returns 0 for a
//     crossed book), read by metrics and the snapshots table, the spread-
//     widening alert, the startup analyzer, the market allocator, market-cross
//     eligibility and the cross-stable arb.
// With the sides read correctly the raw book is uncrossed, so on any heartbeat
// whose offers fetch failed -- and on every heartbeat with competitor tracking
// off -- both would have taken our own quotes as market evidence.  Both now
// require provenance.  The fixture is the live XCH/DBX ticker read correctly,
// bid 83.40467 / ask 84.494: ~130 bps, the tightest of the four books, so the
// one that would have carried the most weight had it leaked.
//
// MUTATION CHECK -- red sets predicted before the run, each from a fresh build:
//   M5 get_fair_value_inputs without the provenance conjunct (the code as it
//      was before this gate)
//      -> ARawTickerBookIsNotAFairValueObservation,
//         ARawTickerIngestWithdrawsTheObservation,
//         WithCompetitorTrackingOffNothingIsObserved
//   M6 refresh publishes compute_spread_bps unconditionally (likewise)
//      -> ARawTickerBookPublishesNoSpread,
//         ARawTickerIngestWithdrawsThePublishedSpread,
//         WithCompetitorTrackingOffNoSpreadIsPublished
//   AFilteredBookIsAFairValueObservation and AFilteredBookPublishesItsSpread
//   stay green under both: they pin that each gate excludes by provenance
//   rather than by closing the filtered path as well.
//
// NOT REACHABLE FROM HERE: the Engine consumers behind these two sources --
// nothing in cpp/tests constructs an Engine.  What is pinned is the source
// every one of them reads through.
// ---------------------------------------------------------------------------

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "xop/execution/market_data.hpp"
#include "xop/state.hpp"
#include "xop/types.hpp"

namespace {

using xop::CompetingOffer;
using xop::MarketDataConfig;
using xop::MarketDataFeed;
using xop::Mojo;
using xop::Side;
using xop::State;

const std::string kPair = "XCH/DBX";

// The live XCH/DBX ticker on 2026-09-13, read correctly.
constexpr double kBid = 83.40467;
constexpr double kAsk = 84.494;
// (ask - bid) / ((bid + ask) / 2) * 1e4, evaluated offline in double.
constexpr double kSpreadBps = 129.76040846541596;
constexpr double kMid       = 83.949335;

MarketDataConfig provenance_cfg() {
    MarketDataConfig cfg;
    cfg.min_competitor_offer_size = 0;   // dust sizing is not under test
    return cfg;
}

CompetingOffer third_party(const char* id, Side side, double px) {
    CompetingOffer o;
    o.offer_id = id;
    o.side     = side;
    o.price    = static_cast<Mojo>(
        std::llround(px * static_cast<double>(xop::kMojosPerXch)));
    o.size     = 5 * xop::kMojosPerXch;
    return o;
}

// The ticker's own two prices -- but arriving as filtered third-party offers.
void ingest_filtered(MarketDataFeed& feed) {
    feed.ingest_competing_offers(
        kPair,
        {third_party("tp-bid", Side::Bid, kBid),
         third_party("tp-ask", Side::Ask, kAsk)},
        {}, xop::kMojosPerXch, 1'000);
}

// The heartbeat whose offers fetch failed: only the ticker arrives.
void ingest_raw(MarketDataFeed& feed) {
    feed.ingest_dexie(kPair, kBid, kAsk, /*last=*/83.40, /*vol_24h=*/0.21);
}

}  // namespace

// ===========================================================================
// Gate 1 -- get_fair_value_inputs: the fair-value solve and the centre blend.
// ===========================================================================

TEST(RawTickerProvenance, ARawTickerBookIsNotAFairValueObservation)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);
    ingest_raw(feed);
    feed.refresh({kPair});

    // Precondition: a two-sided, UNCROSSED book really is sitting there, so
    // the only thing left to exclude it is where it came from.
    const auto bbo = feed.get_dex_bbo(kPair);
    ASSERT_DOUBLE_EQ(bbo.first, kBid);
    ASSERT_DOUBLE_EQ(bbo.second, kAsk);

    const auto obs = feed.get_fair_value_inputs(kPair);
    EXPECT_FALSE(obs.has_book)
        << "the raw ticker includes our own offers: reading it back as the "
           "market is the bot confirming itself";
    EXPECT_DOUBLE_EQ(obs.mid, 0.0);
    EXPECT_DOUBLE_EQ(obs.spread_bps, 0.0);
}

TEST(RawTickerProvenance, AFilteredBookIsAFairValueObservation)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);
    ingest_filtered(feed);
    feed.refresh({kPair});

    const auto obs = feed.get_fair_value_inputs(kPair);
    ASSERT_TRUE(obs.has_book)
        << "the same two prices from the filtered book ARE an observation";
    EXPECT_NEAR(obs.mid, kMid, 1e-9);
    EXPECT_NEAR(obs.spread_bps, kSpreadBps, 1e-6);
}

TEST(RawTickerProvenance, ARawTickerIngestWithdrawsTheObservation)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);

    ingest_filtered(feed);
    ASSERT_TRUE(feed.get_fair_value_inputs(kPair).has_book);

    // Next heartbeat the offers fetch throws: ingest_dexie overwrites the
    // prices and clears their provenance in the same breath.
    ingest_raw(feed);
    EXPECT_FALSE(feed.get_fair_value_inputs(kPair).has_book);

    // A successful filtered ingest restores it.
    ingest_filtered(feed);
    EXPECT_TRUE(feed.get_fair_value_inputs(kPair).has_book);
}

TEST(RawTickerProvenance, WithCompetitorTrackingOffNothingIsObserved)
{
    MarketDataConfig cfg = provenance_cfg();
    cfg.enable_competitor_tracking = false;   // the ticker is all there is
    State state;
    MarketDataFeed feed(cfg, state);
    feed.ingest_block_height(100);

    ingest_raw(feed);
    ingest_filtered(feed);   // a no-op with tracking off
    feed.refresh({kPair});

    ASSERT_DOUBLE_EQ(feed.get_dex_bbo(kPair).first, kBid);
    EXPECT_FALSE(feed.get_fair_value_inputs(kPair).has_book)
        << "with tracking off every heartbeat is a raw-ticker heartbeat";
}

// ===========================================================================
// Gate 2 -- refresh()'s published spread_bps: every spread_bps reader.
// ===========================================================================

TEST(RawTickerProvenance, ARawTickerBookPublishesNoSpread)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);
    ingest_raw(feed);
    feed.refresh({kPair});

    const auto snap = state.get_market(kPair);
    // Precondition: the snapshot still carries the uncrossed raw touches --
    // the guards that read them are meant to see the correct sides --
    ASSERT_GT(snap.best_bid, 0);
    ASSERT_GT(snap.best_ask, snap.best_bid);
    // -- but publishes no width for them.
    EXPECT_DOUBLE_EQ(snap.spread_bps, 0.0)
        << "metrics, the snapshots table, the spread alert, the startup "
           "analyzer, the allocator and market-cross eligibility would all "
           "take our own quotes' width as the market's";
    EXPECT_DOUBLE_EQ(feed.get_spread_bps(kPair), 0.0);
}

TEST(RawTickerProvenance, AFilteredBookPublishesItsSpread)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);
    ingest_filtered(feed);
    feed.refresh({kPair});

    EXPECT_NEAR(state.get_market(kPair).spread_bps, kSpreadBps, 1e-6);
    EXPECT_NEAR(feed.get_spread_bps(kPair), kSpreadBps, 1e-6);
}

TEST(RawTickerProvenance, ARawTickerIngestWithdrawsThePublishedSpread)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);

    ingest_filtered(feed);
    feed.refresh({kPair});
    ASSERT_NEAR(feed.get_spread_bps(kPair), kSpreadBps, 1e-6);

    ingest_raw(feed);
    feed.refresh({kPair});
    EXPECT_DOUBLE_EQ(feed.get_spread_bps(kPair), 0.0);
    EXPECT_DOUBLE_EQ(state.get_market(kPair).spread_bps, 0.0);

    ingest_filtered(feed);
    feed.refresh({kPair});
    EXPECT_NEAR(feed.get_spread_bps(kPair), kSpreadBps, 1e-6);
}

TEST(RawTickerProvenance, WithCompetitorTrackingOffNoSpreadIsPublished)
{
    MarketDataConfig cfg = provenance_cfg();
    cfg.enable_competitor_tracking = false;
    State state;
    MarketDataFeed feed(cfg, state);
    feed.ingest_block_height(100);

    ingest_raw(feed);
    ingest_filtered(feed);   // a no-op with tracking off
    feed.refresh({kPair});

    ASSERT_GT(state.get_market(kPair).best_bid, 0);
    EXPECT_DOUBLE_EQ(feed.get_spread_bps(kPair), 0.0);
}
