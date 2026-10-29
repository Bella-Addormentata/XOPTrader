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
//         ALockedRawTickerBookIsNotAFairValueObservation,
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

#include <chrono>
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

// [round 3] LOCKED is the heaviest case, not a corner.  A locked raw book
// (bid == ask) passes the ask >= bid test with compute_spread_bps' 0, so it
// entered the solve at the MINIMUM book sigma -- the most weight any book can
// carry -- before this PR, and after the relabel alone.  It is our own quotes.
// Provenance excludes it like any other raw book.  (The published spread is 0
// for a locked book with or without Gate 2, so only Gate 1 is asserted.)
TEST(RawTickerProvenance, ALockedRawTickerBookIsNotAFairValueObservation)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);
    feed.ingest_dexie(kPair, kAsk, kAsk, /*last=*/kAsk, /*vol_24h=*/0.21);
    feed.refresh({kPair});

    const auto bbo = feed.get_dex_bbo(kPair);
    ASSERT_GT(bbo.first, 0.0);
    ASSERT_DOUBLE_EQ(bbo.first, bbo.second) << "precondition: locked";

    const auto obs = feed.get_fair_value_inputs(kPair);
    EXPECT_FALSE(obs.has_book)
        << "a locked raw book passes ask >= bid at spread 0 -- the minimum "
           "sigma, so the heaviest weight -- and it is our own quotes";
    EXPECT_DOUBLE_EQ(obs.mid, 0.0);
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

// ===========================================================================
// Gate 3 -- [PEGEDGE 2026-10-16] MarketSnapshot::touches_third_party: the
// asset-level peg watcher judges the book edge only when best_bid/best_ask
// came from the filtered third-party book. A raw ticker includes our own
// resting offers, and our own bid near par must never read as proof of par.
// ===========================================================================

TEST(RawTickerProvenance, AFilteredBookPublishesThirdPartyTouches)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);
    ingest_filtered(feed);
    feed.refresh({kPair});

    const auto snap = state.get_market(kPair);
    ASSERT_GT(snap.best_bid, 0);
    EXPECT_TRUE(snap.touches_third_party);
}

TEST(RawTickerProvenance, ARawTickerBookPublishesNoThirdPartyTouches)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);
    ingest_raw(feed);
    feed.refresh({kPair});

    const auto snap = state.get_market(kPair);
    ASSERT_GT(snap.best_bid, 0)
        << "precondition: the raw touches really are published";
    EXPECT_FALSE(snap.touches_third_party)
        << "the raw ticker includes our own offers";
}

TEST(RawTickerProvenance, ARawTickerIngestWithdrawsThirdPartyTouches)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);
    ingest_filtered(feed);
    feed.refresh({kPair});
    ASSERT_TRUE(state.get_market(kPair).touches_third_party);

    // The next heartbeat's offers fetch fails: only the ticker arrives.
    ingest_raw(feed);
    feed.refresh({kPair});
    EXPECT_FALSE(state.get_market(kPair).touches_third_party);
}

TEST(RawTickerProvenance, AStaleFilteredBookPublishesNoThirdPartyTouches)
{
    MarketDataConfig cfg = provenance_cfg();
    // Any filtered ingest is older than a negative threshold.
    cfg.stale_threshold = std::chrono::minutes(-1);
    State state;
    MarketDataFeed feed(cfg, state);
    feed.ingest_block_height(100);
    ingest_filtered(feed);
    feed.refresh({kPair});

    EXPECT_FALSE(state.get_market(kPair).touches_third_party)
        << "offers last seen longer ago than stale_threshold may be gone";
}

TEST(RawTickerProvenance, WithCompetitorTrackingOffNoTouchesAreThirdParty)
{
    MarketDataConfig cfg = provenance_cfg();
    cfg.enable_competitor_tracking = false;
    State state;
    MarketDataFeed feed(cfg, state);
    feed.ingest_block_height(100);
    ingest_raw(feed);
    ingest_filtered(feed);   // a no-op with tracking off
    feed.refresh({kPair});

    EXPECT_FALSE(state.get_market(kPair).touches_third_party);
}

// ===========================================================================
// [WIDEBOOK 2026-10-29] The price history the flash-crash guard reads.
//
// A mid from a book wider than risk.flash_crash_max_spread_bps cannot set the
// high a crash is measured from.  On a heartbeat whose offers fetch failed the
// published spread is 0 (raw ticker, above) while compute_mid still serves the
// cached orderbook_mid for up to stale_threshold.  Recorded at 0 bps, a fake
// high cached from a wide book would count again and the return to a normal
// price would trip the global guard.
// ===========================================================================

TEST(RawTickerProvenance, AFailedFetchKeepsTheCachedMidsBookWidthInHistory)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);

    // XCH/BYC's book of 2026-10-29 with its near ask gone, in miniature:
    // 83.40467 / 108.0 is ~2,571 bps wide.
    feed.ingest_block_height(100);
    feed.ingest_competing_offers(
        kPair,
        {third_party("tp-bid", Side::Bid, kBid),
         third_party("tp-ask", Side::Ask, 108.0)},
        {}, xop::kMojosPerXch, 1'000);
    feed.refresh({kPair});

    // The next heartbeat's offers fetch fails: only the ticker arrives. With
    // none of our own offers resting (they expire unreplaced while the guard
    // holds) it is the same wide book, so nothing clamps the cached mid.
    feed.ingest_block_height(101);
    feed.ingest_dexie(kPair, kBid, 108.0, /*last=*/83.40, /*vol_24h=*/0.21);
    feed.refresh({kPair});

    const auto h = feed.get_price_history(kPair);
    ASSERT_EQ(h.size(), 2u);
    ASSERT_GT(h[0].spread_bps, 2000.0);
    ASSERT_DOUBLE_EQ(h[1].price, h[0].price)
        << "precondition: block 101 served the cached orderbook mid";
    EXPECT_DOUBLE_EQ(state.get_market(kPair).spread_bps, 0.0)
        << "the published spread stays 0 for the raw ticker (PR #159)";
    EXPECT_DOUBLE_EQ(h[1].spread_bps, h[0].spread_bps)
        << "the cached mid keeps the width of the book it came from";
}

TEST(RawTickerProvenance, AFilteredBookRecordsItsSpreadInHistory)
{
    State state;
    MarketDataFeed feed(provenance_cfg(), state);
    feed.ingest_block_height(100);
    ingest_filtered(feed);
    feed.refresh({kPair});

    const auto h = feed.get_price_history(kPair);
    ASSERT_EQ(h.size(), 1u);
    EXPECT_NEAR(h[0].spread_bps, kSpreadBps, 1e-6);
}
