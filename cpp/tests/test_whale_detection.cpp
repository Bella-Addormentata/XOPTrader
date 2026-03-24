// test_whale_detection.cpp -- Unit tests for whale-trader detection and
//                             response logic in MarketDataFeed.
//
// Tests the following scenarios:
//   1.  Non-whale trade: small trade does not trigger whale detection.
//   2.  Absolute threshold: trade >= whale_trade_threshold is classified.
//   3.  Volume-fraction threshold: trade >= whale_volume_fraction * vol_24h.
//   4.  Both thresholds independently sufficient (OR logic).
//   5.  Spread multiplier at zero events returns 1.0.
//   6.  Spread multiplier scales linearly to max at full window.
//   7.  Spread multiplier is clamped at max (never exceeds it).
//   8.  is_whale_active returns false with no events.
//   9.  is_whale_active returns true after a whale trade.
//  10.  Window expiry: whale goes inactive after whale_window_blocks.
//  11.  Multiple pairs tracked independently.
//  12.  dominant_side reflects the most-recent whale event's direction.
//  13.  largest_trade_size records the biggest trade in the window.
//  14.  events_in_window accumulates multiple whale trades correctly.
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets in test data)
//   ISO/IEC 5055        (bounds-checked assertions, no UB)
//   ISO/IEC 25000       (clear test naming, comprehensive coverage)

#include "xop/execution/market_data.hpp"
#include "xop/state.hpp"
#include "xop/types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace xop {
namespace {

// ===========================================================================
// Test Fixture
// ===========================================================================

class WhaleDetectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Whale threshold: 50 XCH absolute, 5 % of 24h volume.
        cfg_.whale_trade_threshold     = 50LL * kMojosPerXch;
        cfg_.whale_volume_fraction     = 0.05;   // 5 %
        cfg_.whale_window_blocks       = 10;
        cfg_.whale_max_spread_multiplier = 3.0;

        state_ = std::make_unique<State>();
        feed_  = std::make_unique<MarketDataFeed>(cfg_, *state_);
    }

    /// Helper: Inject dexie mid-price and volume so that volume-fraction tests
    /// have a valid denominator.
    void prime_pair(const std::string& pair,
                    double bid,
                    double ask,
                    double vol_24h,
                    BlockHeight block = 100)
    {
        feed_->ingest_dexie(pair, bid, ask, (bid + ask) / 2.0, vol_24h);
        feed_->ingest_block_height(block);
    }

    /// Convenience: convert XCH to mojos.
    static Mojo xch(double v) {
        return static_cast<Mojo>(v * static_cast<double>(kMojosPerXch));
    }

    MarketDataConfig             cfg_;
    std::unique_ptr<State>       state_;
    std::unique_ptr<MarketDataFeed> feed_;
};

// ===========================================================================
// Test Cases
// ===========================================================================

// ---------------------------------------------------------------------------
// TEST 1: Non-whale trade is ignored
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, SmallTrade_NotClassifiedAsWhale) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1000.0, 100);

    // 1 XCH << 50 XCH threshold; 1/1000 = 0.1 % << 5 % fraction threshold.
    feed_->ingest_trade(pair, Side::Bid, xch(1.0), 100);

    EXPECT_FALSE(feed_->is_whale_active(pair));
    EXPECT_FALSE(feed_->get_whale_metrics(pair).has_value());
    EXPECT_DOUBLE_EQ(1.0, feed_->get_whale_spread_multiplier(pair));
}

// ---------------------------------------------------------------------------
// TEST 2: Absolute-threshold trade is classified as whale
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, AbsoluteThreshold_TradeClassifiedAsWhale) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);  // huge volume → fraction won't trigger

    // Exactly at threshold: 50 XCH.
    feed_->ingest_trade(pair, Side::Bid, xch(50.0), 100);

    EXPECT_TRUE(feed_->is_whale_active(pair));
    auto wm = feed_->get_whale_metrics(pair);
    ASSERT_TRUE(wm.has_value());
    EXPECT_EQ(1u, wm->events_in_window);
    EXPECT_EQ(xch(50.0), wm->largest_trade_size);
}

// ---------------------------------------------------------------------------
// TEST 3: Volume-fraction threshold triggers even below absolute threshold
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, VolumeFractionThreshold_SmallVolumeMarket) {
    const std::string pair = "XCH/wUSDC";
    // 24h vol = 100 XCH; whale_volume_fraction = 5 % → threshold = 5 XCH.
    prime_pair(pair, 2.70, 2.75, 100.0, 100);

    // 10 XCH = 10 % of 100 XCH vol → above 5 % fraction; below 50 XCH absolute.
    feed_->ingest_trade(pair, Side::Ask, xch(10.0), 100);

    EXPECT_TRUE(feed_->is_whale_active(pair));
    auto wm = feed_->get_whale_metrics(pair);
    ASSERT_TRUE(wm.has_value());
    EXPECT_EQ(1u, wm->events_in_window);
    // The trade (10 XCH) is at least 5 % of 100 XCH daily volume -- confirmed
    // by the fact that detection triggered despite being below the 50 XCH
    // absolute threshold.
}

// ---------------------------------------------------------------------------
// TEST 4: Absolute threshold alone is sufficient (high-volume pair)
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, AbsoluteThreshold_SufficientOnHighVolumePair) {
    const std::string pair = "XCH/wUSDC";
    // vol_24h = 10,000 XCH → 50 XCH = 0.5 % < 5 % fraction threshold.
    prime_pair(pair, 2.70, 2.75, 10'000.0, 100);

    // 50 XCH: fraction = 0.5 %, but absolute threshold = 50 XCH → should trigger.
    feed_->ingest_trade(pair, Side::Bid, xch(50.0), 100);

    EXPECT_TRUE(feed_->is_whale_active(pair));
}

// ---------------------------------------------------------------------------
// TEST 5: Spread multiplier = 1.0 when no whale events
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, SpreadMultiplier_OneWhenNoEvents) {
    const std::string pair = "XCH/wUSDC";
    EXPECT_DOUBLE_EQ(1.0, feed_->get_whale_spread_multiplier(pair));
}

// ---------------------------------------------------------------------------
// TEST 6: Spread multiplier scales linearly with event count
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, SpreadMultiplier_LinearScaling) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    // With window = 10 and max multiplier = 3.0:
    //   5 events → fraction = 5/10 = 0.5 → multiplier = 1.0 + 0.5 * 2.0 = 2.0.
    for (int i = 0; i < 5; ++i) {
        feed_->ingest_trade(pair, Side::Bid, xch(100.0), static_cast<BlockHeight>(100 + i));
    }

    const double mult = feed_->get_whale_spread_multiplier(pair);
    EXPECT_NEAR(2.0, mult, 0.01);
}

// ---------------------------------------------------------------------------
// TEST 7: Spread multiplier is clamped at max
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, SpreadMultiplier_ClampedAtMax) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    // 20 events >> window of 10 → fraction clamped at 1.0 → multiplier = 3.0.
    for (int i = 0; i < 20; ++i) {
        feed_->ingest_trade(pair, Side::Bid, xch(100.0), static_cast<BlockHeight>(100 + i));
    }

    const double mult = feed_->get_whale_spread_multiplier(pair);
    EXPECT_NEAR(cfg_.whale_max_spread_multiplier, mult, 0.01);
    EXPECT_LE(mult, cfg_.whale_max_spread_multiplier + 1e-9);
}

// ---------------------------------------------------------------------------
// TEST 8: is_whale_active false with no events
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, IsWhaleActive_FalseWithNoEvents) {
    EXPECT_FALSE(feed_->is_whale_active("XCH/wUSDC"));
}

// ---------------------------------------------------------------------------
// TEST 9: is_whale_active true after whale trade
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, IsWhaleActive_TrueAfterWhaleTrade) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    feed_->ingest_trade(pair, Side::Ask, xch(200.0), 100);

    EXPECT_TRUE(feed_->is_whale_active(pair));
}

// ---------------------------------------------------------------------------
// TEST 10: Window expiry -- whale goes inactive after whale_window_blocks
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, WindowExpiry_WhaleGoesInactive) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    // Inject whale event at block 100.
    feed_->ingest_trade(pair, Side::Bid, xch(100.0), 100);
    ASSERT_TRUE(feed_->is_whale_active(pair));

    // Advance block height past the window (window = 10 blocks → block 110+).
    feed_->ingest_block_height(110);

    // Now the whale window has expired.
    EXPECT_FALSE(feed_->is_whale_active(pair));
    EXPECT_DOUBLE_EQ(1.0, feed_->get_whale_spread_multiplier(pair));
}

// ---------------------------------------------------------------------------
// TEST 11: Multiple pairs tracked independently
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, MultiplePairs_IndependentTracking) {
    const std::string pair1 = "XCH/wUSDC";
    const std::string pair2 = "XCH/SBX";

    prime_pair(pair1, 2.70, 2.75, 1'000'000.0, 100);
    prime_pair(pair2, 0.0001, 0.0002, 5000.0, 100);

    // Whale only on pair1.
    feed_->ingest_trade(pair1, Side::Bid, xch(50.0), 100);

    EXPECT_TRUE(feed_->is_whale_active(pair1));
    EXPECT_FALSE(feed_->is_whale_active(pair2));

    // Whale only on pair2.
    feed_->ingest_trade(pair2, Side::Ask, xch(50.0), 100);

    EXPECT_TRUE(feed_->is_whale_active(pair1));
    EXPECT_TRUE(feed_->is_whale_active(pair2));

    auto wm2 = feed_->get_whale_metrics(pair2);
    ASSERT_TRUE(wm2.has_value());
    EXPECT_EQ(pair2, wm2->pair_name);
    EXPECT_EQ(Side::Ask, wm2->dominant_side);
}

// ---------------------------------------------------------------------------
// TEST 12: dominant_side reflects the most-recent whale event
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, DominantSide_ReflectsMostRecentEvent) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    feed_->ingest_trade(pair, Side::Bid, xch(50.0), 100);
    feed_->ingest_trade(pair, Side::Ask, xch(75.0), 101);  // Most recent

    auto wm = feed_->get_whale_metrics(pair);
    ASSERT_TRUE(wm.has_value());
    EXPECT_EQ(Side::Ask, wm->dominant_side);
}

// ---------------------------------------------------------------------------
// TEST 13: largest_trade_size reflects the biggest trade in the window
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, LargestTradeSize_RecordsMax) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    feed_->ingest_trade(pair, Side::Bid, xch(50.0),  100);
    feed_->ingest_trade(pair, Side::Bid, xch(200.0), 101);  // Largest
    feed_->ingest_trade(pair, Side::Ask, xch(75.0),  102);

    auto wm = feed_->get_whale_metrics(pair);
    ASSERT_TRUE(wm.has_value());
    EXPECT_EQ(xch(200.0), wm->largest_trade_size);
}

// ---------------------------------------------------------------------------
// TEST 14: events_in_window accumulates multiple trades correctly
// ---------------------------------------------------------------------------

TEST_F(WhaleDetectionTest, EventsInWindow_Accumulates) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    feed_->ingest_trade(pair, Side::Bid, xch(50.0), 100);
    feed_->ingest_trade(pair, Side::Bid, xch(60.0), 101);
    feed_->ingest_trade(pair, Side::Ask, xch(70.0), 102);

    auto wm = feed_->get_whale_metrics(pair);
    ASSERT_TRUE(wm.has_value());
    EXPECT_EQ(3u, wm->events_in_window);
}

}  // namespace
}  // namespace xop
