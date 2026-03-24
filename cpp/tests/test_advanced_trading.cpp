// test_advanced_trading.cpp -- Unit tests for VPIN, OFI, and asymmetric
//                              spread widening in MarketDataFeed.
//
// Tests the following advanced trading methods:
//
// VPIN (Volume-Synchronized Probability of Informed Trading):
//   1.  No trades → no VPIN metrics available.
//   2.  Balanced flow → VPIN near 0.
//   3.  Fully one-sided flow → VPIN near 1.
//   4.  Mixed flow → VPIN between 0 and 1.
//   5.  Bucket completion → correct bucket count.
//   6.  Window trimming → oldest buckets evicted.
//   7.  Buy/sell volume percentages tracked correctly.
//
// OFI (Order Flow Imbalance):
//   8.  Single snapshot → no OFI metrics.
//   9.  Bid strengthening → positive OFI.
//  10.  Ask strengthening → negative OFI.
//  11.  Stable book → zero OFI.
//  12.  Window trimming → oldest snapshots evicted.
//  13.  Normalised OFI is bounded in [-1, 1].
//
// Asymmetric Spread Widening:
//  14.  No whale → symmetric multipliers {1, 1}.
//  15.  Whale buying → ask multiplier > bid multiplier.
//  16.  Whale selling → bid multiplier > ask multiplier.
//  17.  Skew factor 0 → symmetric despite whale activity.
//  18.  Average of asymmetric multipliers matches symmetric multiplier.
//
// References:
//   Easley, López de Prado & O'Hara (2012). "Flow Toxicity and Liquidity
//     in a High-frequency World."
//   Cont, Kukanov & Stoikov (2014). "The Price Impact of Order Book Events."
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets in test data)
//   ISO/IEC 5055        (bounds-checked assertions, no UB)
//   ISO/IEC 25000       (clear test naming, comprehensive coverage)

#include "xop/execution/market_data.hpp"
#include "xop/state.hpp"
#include "xop/types.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace xop {
namespace {

// ===========================================================================
// Test Fixture
// ===========================================================================

class AdvancedTradingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // VPIN: 10 XCH per bucket, 50-bucket window.
        cfg_.vpin_bucket_size    = 10.0;
        cfg_.vpin_window_buckets = 50;

        // OFI: 20-observation window.
        cfg_.ofi_window_size     = 20;

        // Asymmetric: 50 % skew factor.
        cfg_.asymmetric_skew_factor = 0.5;

        // Whale config for asymmetric tests.
        cfg_.whale_trade_threshold     = 50LL * kMojosPerXch;
        cfg_.whale_volume_fraction     = 0.05;
        cfg_.whale_window_blocks       = 10;
        cfg_.whale_max_spread_multiplier = 3.0;

        state_ = std::make_unique<State>();
        feed_  = std::make_unique<MarketDataFeed>(cfg_, *state_);
    }

    void prime_pair(const std::string& pair,
                    double bid,
                    double ask,
                    double vol_24h,
                    BlockHeight block = 100)
    {
        feed_->ingest_dexie(pair, bid, ask, (bid + ask) / 2.0, vol_24h);
        feed_->ingest_block_height(block);
    }

    static Mojo xch(double v) {
        return static_cast<Mojo>(v * static_cast<double>(kMojosPerXch));
    }

    MarketDataConfig                cfg_;
    std::unique_ptr<State>          state_;
    std::unique_ptr<MarketDataFeed> feed_;
};

// ===========================================================================
// VPIN Tests
// ===========================================================================

// TEST 1: No trades → no VPIN metrics.
TEST_F(AdvancedTradingTest, Vpin_NoTrades_NoMetrics) {
    EXPECT_FALSE(feed_->get_vpin_metrics("XCH/wUSDC").has_value());
    EXPECT_DOUBLE_EQ(0.0, feed_->get_vpin("XCH/wUSDC"));
}

// TEST 2: Perfectly balanced flow → VPIN near 0.
TEST_F(AdvancedTradingTest, Vpin_BalancedFlow_NearZero) {
    const std::string pair = "XCH/wUSDC";

    // Fill 10 buckets alternating buy/sell, 5 XCH each = 10 XCH per bucket.
    for (int i = 0; i < 10; ++i) {
        feed_->ingest_trade_for_vpin(pair, Side::Bid, 5.0);
        feed_->ingest_trade_for_vpin(pair, Side::Ask, 5.0);
    }

    auto vm = feed_->get_vpin_metrics(pair);
    ASSERT_TRUE(vm.has_value());
    // Each bucket: |5 - 5| / 10 = 0 → VPIN = 0.
    EXPECT_NEAR(0.0, vm->vpin, 0.01);
}

// TEST 3: Fully one-sided flow → VPIN = 1.0.
TEST_F(AdvancedTradingTest, Vpin_OneSidedFlow_One) {
    const std::string pair = "XCH/wUSDC";

    // Fill 5 buckets with only buys (10 XCH each).
    for (int i = 0; i < 5; ++i) {
        feed_->ingest_trade_for_vpin(pair, Side::Bid, 10.0);
    }

    auto vm = feed_->get_vpin_metrics(pair);
    ASSERT_TRUE(vm.has_value());
    // Each bucket: |10 - 0| / 10 = 1.0 → VPIN = 1.0.
    EXPECT_NEAR(1.0, vm->vpin, 0.01);
    EXPECT_NEAR(1.0, vm->buy_volume_pct, 0.01);
    EXPECT_NEAR(0.0, vm->sell_volume_pct, 0.01);
}

// TEST 4: Mixed flow → VPIN between 0 and 1.
TEST_F(AdvancedTradingTest, Vpin_MixedFlow_Intermediate) {
    const std::string pair = "XCH/wUSDC";

    // Fill buckets with 70/30 buy/sell split: |7 - 3| / 10 = 0.4.
    for (int i = 0; i < 10; ++i) {
        feed_->ingest_trade_for_vpin(pair, Side::Bid, 7.0);
        feed_->ingest_trade_for_vpin(pair, Side::Ask, 3.0);
    }

    auto vm = feed_->get_vpin_metrics(pair);
    ASSERT_TRUE(vm.has_value());
    EXPECT_NEAR(0.4, vm->vpin, 0.05);
    EXPECT_GT(vm->vpin, 0.0);
    EXPECT_LT(vm->vpin, 1.0);
}

// TEST 5: Bucket count matches expected completions.
TEST_F(AdvancedTradingTest, Vpin_BucketCount) {
    const std::string pair = "XCH/wUSDC";

    // 3 buckets × 10 XCH each = 30 XCH total.
    for (int i = 0; i < 3; ++i) {
        feed_->ingest_trade_for_vpin(pair, Side::Bid, 10.0);
    }

    auto vm = feed_->get_vpin_metrics(pair);
    ASSERT_TRUE(vm.has_value());
    EXPECT_EQ(3u, vm->complete_buckets);
}

// TEST 6: Window trimming evicts oldest buckets.
TEST_F(AdvancedTradingTest, Vpin_WindowTrimming) {
    // Recreate feed with a tiny window: 3 buckets.
    cfg_.vpin_window_buckets = 3;
    state_ = std::make_unique<State>();
    feed_  = std::make_unique<MarketDataFeed>(cfg_, *state_);

    const std::string pair = "XCH/wUSDC";

    // Fill 5 buckets; only last 3 should remain.
    for (int i = 0; i < 5; ++i) {
        feed_->ingest_trade_for_vpin(pair, Side::Bid, 10.0);
    }

    auto vm = feed_->get_vpin_metrics(pair);
    ASSERT_TRUE(vm.has_value());
    EXPECT_EQ(3u, vm->complete_buckets);
}

// TEST 7: Buy/sell volume percentages.
TEST_F(AdvancedTradingTest, Vpin_VolumePercentages) {
    const std::string pair = "XCH/wUSDC";

    // 80/20 split across 2 completed buckets.
    for (int i = 0; i < 2; ++i) {
        feed_->ingest_trade_for_vpin(pair, Side::Bid, 8.0);
        feed_->ingest_trade_for_vpin(pair, Side::Ask, 2.0);
    }

    auto vm = feed_->get_vpin_metrics(pair);
    ASSERT_TRUE(vm.has_value());
    EXPECT_NEAR(0.8, vm->buy_volume_pct, 0.01);
    EXPECT_NEAR(0.2, vm->sell_volume_pct, 0.01);
}

// ===========================================================================
// OFI Tests
// ===========================================================================

// TEST 8: Single snapshot → no OFI metrics.
TEST_F(AdvancedTradingTest, Ofi_SingleSnapshot_NoMetrics) {
    feed_->ingest_book_snapshot_for_ofi("XCH/wUSDC", 2.70, 100.0, 2.75, 100.0);
    EXPECT_FALSE(feed_->get_ofi_metrics("XCH/wUSDC").has_value());
    EXPECT_DOUBLE_EQ(0.0, feed_->get_normalized_ofi("XCH/wUSDC"));
}

// TEST 9: Bid price increase → positive OFI (buy pressure).
TEST_F(AdvancedTradingTest, Ofi_BidStrengthening_PositiveOfi) {
    const std::string pair = "XCH/wUSDC";

    // Snapshot 1: bid = 2.70, ask = 2.75.
    feed_->ingest_book_snapshot_for_ofi(pair, 2.70, 100.0, 2.75, 100.0);

    // Snapshot 2: bid rises to 2.72 (bid improvement → buy pressure).
    feed_->ingest_book_snapshot_for_ofi(pair, 2.72, 100.0, 2.75, 100.0);

    auto om = feed_->get_ofi_metrics(pair);
    ASSERT_TRUE(om.has_value());
    // Bid increased → e^B = +100.  Ask unchanged → e^A = 0.
    // OFI = 100 - 0 = +100 (positive = buy pressure).
    EXPECT_GT(om->ofi, 0.0);
    EXPECT_GT(om->normalized_ofi, 0.0);
}

// TEST 10: Ask price decrease → negative OFI (sell pressure).
TEST_F(AdvancedTradingTest, Ofi_AskStrengthening_NegativeOfi) {
    const std::string pair = "XCH/wUSDC";

    feed_->ingest_book_snapshot_for_ofi(pair, 2.70, 100.0, 2.75, 100.0);

    // Ask drops to 2.73 (ask improvement = sellers more aggressive → sell pressure).
    feed_->ingest_book_snapshot_for_ofi(pair, 2.70, 100.0, 2.73, 100.0);

    auto om = feed_->get_ofi_metrics(pair);
    ASSERT_TRUE(om.has_value());
    // Bid unchanged → e^B = 0.  Ask decreased → e^A = +100.
    // OFI = 0 - 100 = -100 (negative = sell pressure).
    EXPECT_LT(om->ofi, 0.0);
    EXPECT_LT(om->normalized_ofi, 0.0);
}

// TEST 11: Stable book (no changes) → zero OFI.
TEST_F(AdvancedTradingTest, Ofi_StableBook_ZeroOfi) {
    const std::string pair = "XCH/wUSDC";

    feed_->ingest_book_snapshot_for_ofi(pair, 2.70, 100.0, 2.75, 100.0);
    feed_->ingest_book_snapshot_for_ofi(pair, 2.70, 100.0, 2.75, 100.0);

    auto om = feed_->get_ofi_metrics(pair);
    ASSERT_TRUE(om.has_value());
    // No changes → delta_bid = 0, delta_ask = 0, OFI = 0.
    EXPECT_NEAR(0.0, om->ofi, 1e-12);
    EXPECT_NEAR(0.0, om->normalized_ofi, 1e-12);
}

// TEST 12: Window trimming keeps bounded snapshot count.
TEST_F(AdvancedTradingTest, Ofi_WindowTrimming) {
    const std::string pair = "XCH/wUSDC";

    // Default window = 20.  Inject 30 snapshots.
    for (int i = 0; i < 30; ++i) {
        feed_->ingest_book_snapshot_for_ofi(
            pair, 2.70 + 0.01 * i, 100.0, 2.75 + 0.01 * i, 100.0);
    }

    auto om = feed_->get_ofi_metrics(pair);
    ASSERT_TRUE(om.has_value());
    // observations = window_size + 1 = 21 (kept for deltas).
    EXPECT_LE(om->observations, cfg_.ofi_window_size + 1);
}

// TEST 13: Normalised OFI is bounded in [-1, 1].
TEST_F(AdvancedTradingTest, Ofi_NormalisedBounded) {
    const std::string pair = "XCH/wUSDC";

    // Extreme bid strengthening across 10 snapshots.
    for (int i = 0; i < 10; ++i) {
        feed_->ingest_book_snapshot_for_ofi(
            pair, 2.70 + 0.1 * i, 1000.0, 2.75 + 0.1 * i, 1000.0);
    }

    auto om = feed_->get_ofi_metrics(pair);
    ASSERT_TRUE(om.has_value());
    EXPECT_GE(om->normalized_ofi, -1.0);
    EXPECT_LE(om->normalized_ofi, 1.0);
}

// ===========================================================================
// Asymmetric Spread Widening Tests
// ===========================================================================

// TEST 14: No whale → symmetric {1.0, 1.0}.
TEST_F(AdvancedTradingTest, Asymmetric_NoWhale_Symmetric) {
    auto am = feed_->get_asymmetric_spread_multipliers("XCH/wUSDC");
    EXPECT_DOUBLE_EQ(1.0, am.bid_multiplier);
    EXPECT_DOUBLE_EQ(1.0, am.ask_multiplier);
}

// TEST 15: Whale buying → ask multiplier > bid multiplier.
TEST_F(AdvancedTradingTest, Asymmetric_WhaleBuying_AskWider) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    // Inject whale buy.
    feed_->ingest_trade(pair, Side::Bid, xch(100.0), 100);
    ASSERT_TRUE(feed_->is_whale_active(pair));

    auto am = feed_->get_asymmetric_spread_multipliers(pair);
    EXPECT_GT(am.ask_multiplier, am.bid_multiplier);
    EXPECT_GT(am.ask_multiplier, 1.0);
    EXPECT_GT(am.bid_multiplier, 0.99);  // bid should still be >= 1.0
}

// TEST 16: Whale selling → bid multiplier > ask multiplier.
TEST_F(AdvancedTradingTest, Asymmetric_WhaleSelling_BidWider) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    feed_->ingest_trade(pair, Side::Ask, xch(100.0), 100);
    ASSERT_TRUE(feed_->is_whale_active(pair));

    auto am = feed_->get_asymmetric_spread_multipliers(pair);
    EXPECT_GT(am.bid_multiplier, am.ask_multiplier);
    EXPECT_GT(am.bid_multiplier, 1.0);
}

// TEST 17: Skew factor 0 → symmetric multipliers even with whale.
TEST_F(AdvancedTradingTest, Asymmetric_SkewZero_Symmetric) {
    cfg_.asymmetric_skew_factor = 0.0;
    state_ = std::make_unique<State>();
    feed_  = std::make_unique<MarketDataFeed>(cfg_, *state_);

    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    feed_->ingest_trade(pair, Side::Bid, xch(100.0), 100);

    auto am = feed_->get_asymmetric_spread_multipliers(pair);
    EXPECT_NEAR(am.bid_multiplier, am.ask_multiplier, 1e-9);
}

// TEST 18: Average of asymmetric multipliers ≈ symmetric multiplier.
TEST_F(AdvancedTradingTest, Asymmetric_AverageEqualsSymmetric) {
    const std::string pair = "XCH/wUSDC";
    prime_pair(pair, 2.70, 2.75, 1'000'000.0, 100);

    feed_->ingest_trade(pair, Side::Bid, xch(100.0), 100);

    const double sym = feed_->get_whale_spread_multiplier(pair);
    auto am = feed_->get_asymmetric_spread_multipliers(pair);

    // The average of the two multipliers should equal the symmetric multiplier.
    const double avg = (am.bid_multiplier + am.ask_multiplier) / 2.0;
    EXPECT_NEAR(sym, avg, 1e-9);
}

}  // namespace
}  // namespace xop
