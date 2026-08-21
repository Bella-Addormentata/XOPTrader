// =============================================================================
//  test_published_mid.cpp -- the PUBLISHED mid's BBO-band invariant
// =============================================================================
//
//  compute_orderbook_mid() clamps the ORDER-BOOK mid to its own BBO, but
//  MarketDataFeed::compute_mid() then blends that number with the CEX (30%)
//  and optionally AMM references -- AFTER the clamp.  Unbounded, that blend
//  can carry the published mid back out of the book, which is the exact
//  mechanism by which a broken external reference (the BYC $1.1447 artifact,
//  13% over its corroborated $1.01 truth) could drag a healthy pair's
//  published mid out of its own executable interval.
//
//  The invariant under test: while the dex book is two-sided and fresh, the
//  published mid stays within the BBO widened by
//
//      band_bps = max(published_mid_band_floor_bps,          // default 150
//                     published_mid_band_spread_frac         // default 0.25
//                       * book_spread_bps)
//
//  and the clamp deliberately does NOT apply to one-sided books (no interval
//  to claim) or stale books (history must not pin fresh external data).
//
//  All expected values below are computed by hand from the 70/30 dex/cex
//  blend with freshness decay disabled.
// =============================================================================

#include <gtest/gtest.h>

#include <xop/execution/market_data.hpp>
#include <xop/state.hpp>

#include <chrono>
#include <thread>
#include <cmath>

namespace {

using namespace xop;

/// A feed with CEX freshness decay disabled so the 70/30 weights are exact,
/// and AMM off (its default).  The published-mid band keeps its defaults
/// (floor 150 bps, spread fraction 0.25) unless a test overrides them.
MarketDataConfig band_cfg() {
    MarketDataConfig cfg;
    cfg.cex_freshness_threshold_sec = 0.0;  // exact 70/30 blend
    cfg.amm_blend_weight            = 0.0;
    return cfg;
}

// ---------------------------------------------------------------------------
// Tight fresh book, CEX far above: the blend must be pulled back to the band.
//
//   book: bid 1.00 / ask 1.01  (spread ~99.5 bps -> floor band, 150 bps)
//   cex:  1.20 (a broken reference, ~19% above the book)
//   raw blend: 0.7 * 1.005 + 0.3 * 1.20 = 1.0635
//   ceiling:   1.01 * 1.015 = 1.02515
// ---------------------------------------------------------------------------
TEST(PublishedMidBandTest, CexFarAboveTightBook_ClampedToBand) {
    State state;
    MarketDataFeed feed(band_cfg(), state);

    feed.ingest_dexie("XCH/wUSDC.b", 1.00, 1.01, 1.005, 100.0);
    feed.ingest_cex_reference("XCH/wUSDC.b", 1.20);
    feed.refresh({"XCH/wUSDC.b"});

    const double mid = feed.get_mid_price("XCH/wUSDC.b");
    EXPECT_NEAR(mid, 1.02515, 1e-9);
    // And strictly below the unclamped blend.
    EXPECT_LT(mid, 1.0635);
}

// ---------------------------------------------------------------------------
// Same book, CEX far BELOW: the lower edge binds symmetrically.
//
//   raw blend: 0.7 * 1.005 + 0.3 * 0.50 = 0.8535
//   floor:     1.00 * (1 - 0.015) = 0.985
// ---------------------------------------------------------------------------
TEST(PublishedMidBandTest, CexFarBelowTightBook_ClampedToLowerBand) {
    State state;
    MarketDataFeed feed(band_cfg(), state);

    feed.ingest_dexie("XCH/wUSDC.b", 1.00, 1.01, 1.005, 100.0);
    feed.ingest_cex_reference("XCH/wUSDC.b", 0.50);
    feed.refresh({"XCH/wUSDC.b"});

    EXPECT_NEAR(feed.get_mid_price("XCH/wUSDC.b"), 0.985, 1e-9);
}

// ---------------------------------------------------------------------------
// A one-sigma disagreement must pass untouched.  XCH/wUSDC.b's measured
// CexDirect sigma is ~133 bps; the floor (150 bps) exists precisely so that
// an honest disagreement of that size is never mistaken for corruption.
//
//   cex 1.02 is ~149 bps above the 1.005 midpoint.
//   raw blend: 0.7 * 1.005 + 0.3 * 1.02 = 1.0095, inside 1.02515.
// ---------------------------------------------------------------------------
TEST(PublishedMidBandTest, CexWithinBand_BlendUntouched) {
    State state;
    MarketDataFeed feed(band_cfg(), state);

    feed.ingest_dexie("XCH/wUSDC.b", 1.00, 1.01, 1.005, 100.0);
    feed.ingest_cex_reference("XCH/wUSDC.b", 1.02);
    feed.refresh({"XCH/wUSDC.b"});

    EXPECT_NEAR(feed.get_mid_price("XCH/wUSDC.b"), 1.0095, 1e-9);
}

// ---------------------------------------------------------------------------
// Wide book: the band scales with the book's own spread, because a wide book
// is a weak claim about location (at the 2026-08-01 sweep, truth sat 93 bps
// ABOVE XCH/BYC's best ask -- a hard BBO clamp could never reach it).
//
//   book: bid 1.00 / ask 1.30  (midpoint 1.15, spread 2608.7 bps)
//   band: max(150, 0.25 * 2608.7) = 652.2 bps
//   raw blend: 0.7 * 1.15 + 0.3 * 2.00 = 1.405
//   ceiling:   1.30 * (1 + 0.06522) = 1.384783
//
// The result must be clamped to the SCALED ceiling -- above the floor-only
// ceiling of 1.30 * 1.015 = 1.3195, proving the proportional term is live.
// ---------------------------------------------------------------------------
TEST(PublishedMidBandTest, WideBook_BandScalesWithSpread) {
    State state;
    MarketDataFeed feed(band_cfg(), state);

    feed.ingest_dexie("XCH/BYC", 1.00, 1.30, 1.15, 100.0);
    feed.ingest_cex_reference("XCH/BYC", 2.00);
    feed.refresh({"XCH/BYC"});

    const double mid = feed.get_mid_price("XCH/BYC");
    const double spread_bps = (1.30 - 1.00) / 1.15 * 10000.0;
    const double ceiling = 1.30 * (1.0 + 0.25 * spread_bps / 10000.0);
    EXPECT_NEAR(mid, ceiling, 1e-9);
    EXPECT_GT(mid, 1.30 * 1.015);  // wider than the floor-only band
    EXPECT_LT(mid, 1.405);         // but still clamped below the raw blend
}

// ---------------------------------------------------------------------------
// One-sided book: no clamp.  A lone side bounds fair value from one
// direction only; it asserts no interval, so the invariant claims no
// authority and the external reference governs.
//
//   dex: ask-only book, last trade 1.00 -> dex_mid = 1.00 (last-trade path)
//   raw blend: 0.7 * 1.00 + 0.3 * 1.50 = 1.15, published unclamped.
// ---------------------------------------------------------------------------
TEST(PublishedMidBandTest, OneSidedBook_NoClamp) {
    State state;
    MarketDataFeed feed(band_cfg(), state);

    feed.ingest_dexie("XCH/DBX", 0.0, 1.01, 1.00, 100.0);
    feed.ingest_cex_reference("XCH/DBX", 1.50);
    feed.refresh({"XCH/DBX"});

    EXPECT_NEAR(feed.get_mid_price("XCH/DBX"), 1.15, 1e-9);
}

// ---------------------------------------------------------------------------
// Stale book: no clamp.  A stale book is history, not "now"; pinning fresh
// CEX data to it would preserve exactly the frozen-price failure the
// staleness machinery exists to expose.  stale_threshold = 0 makes any
// positive age stale.
// ---------------------------------------------------------------------------
TEST(PublishedMidBandTest, StaleBook_NoClamp) {
    State state;
    MarketDataConfig cfg = band_cfg();
    cfg.stale_threshold = std::chrono::minutes{0};
    MarketDataFeed feed(cfg, state);

    feed.ingest_dexie("XCH/wUSDC.b", 1.00, 1.01, 1.005, 100.0);
    feed.ingest_cex_reference("XCH/wUSDC.b", 1.20);
    feed.refresh({"XCH/wUSDC.b"});

    // Unclamped blend: 0.7 * 1.005 + 0.3 * 1.20 = 1.0635.
    EXPECT_NEAR(feed.get_mid_price("XCH/wUSDC.b"), 1.0635, 1e-9);
}

// ---------------------------------------------------------------------------
// Crossed book (normal on Dexie -- no matching engine): the interval is
// inverted, so the clamp must order its bounds instead of producing an
// empty range.
//
//   book: bid 1.05 / ask 1.00 -> executable interval [1.00, 1.05]
//   spread (ordered): 0.05 / 1.025 = 487.8 bps -> band max(150, 122) = 150
//   raw blend: 0.7 * 1.025 + 0.3 * 1.50 = 1.1675
//   ceiling:   1.05 * 1.015 = 1.06575
// ---------------------------------------------------------------------------
TEST(PublishedMidBandTest, CrossedBook_BoundsAreOrdered) {
    State state;
    MarketDataFeed feed(band_cfg(), state);

    feed.ingest_dexie("XCH/BYC", 1.05, 1.00, 1.02, 100.0);
    feed.ingest_cex_reference("XCH/BYC", 1.50);
    feed.refresh({"XCH/BYC"});

    const double mid = feed.get_mid_price("XCH/BYC");
    EXPECT_TRUE(std::isfinite(mid));
    EXPECT_NEAR(mid, 1.06575, 1e-9);
}

// ---------------------------------------------------------------------------
// No CEX and no AMM: pure dex mid is inside its own book by construction,
// so the band must never perturb the ordinary single-source path.
// ---------------------------------------------------------------------------
TEST(PublishedMidBandTest, DexOnly_Unchanged) {
    State state;
    MarketDataFeed feed(band_cfg(), state);

    feed.ingest_dexie("XCH/wUSDC.b", 1.00, 1.01, 1.005, 100.0);
    feed.refresh({"XCH/wUSDC.b"});

    EXPECT_NEAR(feed.get_mid_price("XCH/wUSDC.b"), 1.005, 1e-9);
}


// ---------------------------------------------------------------------------
// S5: the last-trade fallback must be aged.
//
// It is the only leg of the blend that is a historical print rather than a
// live quote, and it enters at the full DEX weight, so an unaged fallback
// anchors the mid to whenever the pair last traded.  The fallback only opens
// when the third-party book is empty -- exactly the state a thin or bid-only
// pair sits in, which is where a 13-day-old print was measured dragging a
// mid 8%+ below fair.
// ---------------------------------------------------------------------------
TEST(LastTradeStalenessTest, FirstSightingHasUnknownAgeAndIsRefused) {
    State state;
    MarketDataFeed feed(band_cfg(), state);

    // Empty book -> Case 3; a print we have never watched move.
    feed.ingest_dexie("XCH/wUSDC.b", 0.0, 0.0, 1.005, 100.0);
    feed.ingest_cex_reference("XCH/wUSDC.b", 1.20);
    feed.refresh({"XCH/wUSDC.b"});

    // The print is refused, so the mid is the CEX leg alone rather than a
    // 70% weighting of an undateable print.
    EXPECT_NEAR(feed.get_mid_price("XCH/wUSDC.b"), 1.20, 1e-9);
}

TEST(LastTradeStalenessTest, APrintObservedToMoveIsUsable) {
    State state;
    MarketDataFeed feed(band_cfg(), state);

    // Two different prints: the second gives the change-clock a real time.
    feed.ingest_dexie("XCH/wUSDC.b", 0.0, 0.0, 1.000, 100.0);
    feed.ingest_dexie("XCH/wUSDC.b", 0.0, 0.0, 1.005, 100.0);
    feed.ingest_cex_reference("XCH/wUSDC.b", 1.20);
    feed.refresh({"XCH/wUSDC.b"});

    // 0.7 * 1.005 + 0.3 * 1.20 = 1.0635, then band-clamped as usual.
    EXPECT_GT(feed.get_mid_price("XCH/wUSDC.b"), 1.005);
    EXPECT_LT(feed.get_mid_price("XCH/wUSDC.b"), 1.20);
}

TEST(LastTradeStalenessTest, AnAgedPrintIsRefusedEvenAfterMoving) {
    State state;
    auto cfg = band_cfg();
    cfg.dex_last_trade_max_age_sec = 0.5;   // anything older than half a second
    MarketDataFeed feed(cfg, state);

    feed.ingest_dexie("XCH/wUSDC.b", 0.0, 0.0, 1.000, 100.0);
    feed.ingest_dexie("XCH/wUSDC.b", 0.0, 0.0, 1.005, 100.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    feed.ingest_cex_reference("XCH/wUSDC.b", 1.20);
    feed.refresh({"XCH/wUSDC.b"});

    EXPECT_NEAR(feed.get_mid_price("XCH/wUSDC.b"), 1.20, 1e-9);
}

TEST(LastTradeStalenessTest, TheGateCanBeDisabled) {
    State state;
    auto cfg = band_cfg();
    cfg.dex_last_trade_max_age_sec = 0.0;   // <= 0 disables, as with the tapers
    MarketDataFeed feed(cfg, state);

    feed.ingest_dexie("XCH/wUSDC.b", 0.0, 0.0, 1.005, 100.0);
    feed.ingest_cex_reference("XCH/wUSDC.b", 1.20);
    feed.refresh({"XCH/wUSDC.b"});

    // Disabled: even the never-moved print is blended, as before this change.
    EXPECT_LT(feed.get_mid_price("XCH/wUSDC.b"), 1.20);
}


}  // namespace
