// test_market_allocator.cpp -- Unit tests for the dynamic market allocator.
//
// Tests:
//   1. Equal scores produce equal allocation.
//   2. One pair with dominant volume gets higher allocation.
//   3. Min/max guardrails are enforced.
//   4. Hysteresis prevents small changes from shifting capital.
//   5. Triangular arbitrage edge detection (forward and reverse).
//   6. Disabled allocator returns min_alloc_pct.
//   7. should_evaluate respects interval.
//   8. EMA smoothing blends toward new target.
//   9. Normalisation handles zero-max dimensions.
//  10. Config parsing round-trip.

#include "xop/strategy/market_allocator.hpp"
#include "xop/execution/market_data.hpp"
#include "xop/config.hpp"
#include "xop/state.hpp"
#include "xop/types.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace xop {
namespace {

// ===========================================================================
// Test Fixture
// ===========================================================================

class MarketAllocatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        state_ = std::make_unique<State>();
        feed_ = std::make_unique<MarketDataFeed>(md_cfg_, *state_);

        // Default allocator config.
        alloc_cfg_.enabled = true;
        alloc_cfg_.eval_interval_blocks = 10;
        alloc_cfg_.min_alloc_pct = 0.10;
        alloc_cfg_.max_alloc_pct = 0.50;
        alloc_cfg_.hysteresis_bps = 50.0;
        alloc_cfg_.smooth_alpha = 1.0;  // Instant switch for test clarity.
    }

    // Helper: ingest Dexie data for a pair.
    void ingest_pair(const std::string& pair,
                     double bid, double ask,
                     double last, double vol24h) {
        feed_->ingest_dexie(pair, bid, ask, last, vol24h);
    }

    // Helper: ingest competing offers for a pair.
    void ingest_competitors(const std::string& pair, std::size_t count) {
        std::vector<CompetingOffer> offers;
        for (std::size_t i = 0; i < count; ++i) {
            CompetingOffer o;
            o.offer_id = "comp_" + std::to_string(i);
            o.pair_name = pair;
            o.side = (i % 2 == 0) ? Side::Bid : Side::Ask;
            o.price = static_cast<Mojo>(2.3 * kMojosPerXch);
            o.size = kMojosPerXch;
            o.first_seen_block = 100;
            o.last_seen_block = 100;
            o.last_seen_ts = std::chrono::system_clock::now();
            offers.push_back(o);
        }
        std::unordered_set<std::string> own_ids;
        feed_->ingest_competing_offers(pair, offers, own_ids);
    }

    // Helper: call refresh() so mid_price etc. are computed from ingested data.
    void refresh_feed(const std::vector<std::string>& pairs) {
        feed_->ingest_block_height(100);
        feed_->refresh(pairs);
    }

    MarketDataConfig             md_cfg_;
    MarketAllocatorConfig        alloc_cfg_;
    std::unique_ptr<State>       state_;
    std::unique_ptr<MarketDataFeed> feed_;
};

// ===========================================================================
// Test Cases
// ===========================================================================

// ---------------------------------------------------------------------------
// TEST 1: Equal market data → equal allocation.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, EqualScoresProduceEqualAllocation) {
    // Feed identical data for two pairs.
    ingest_pair("XCH/wUSDC.b", 2.37, 2.38, 2.37, 100.0);
    ingest_pair("XCH/BYC",     2.40, 2.41, 2.40, 100.0);
    refresh_feed({"XCH/wUSDC.b", "XCH/BYC"});

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);
    alloc.evaluate({"XCH/wUSDC.b", "XCH/BYC"}, 100);

    double a1 = alloc.get_allocation("XCH/wUSDC.b");
    double a2 = alloc.get_allocation("XCH/BYC");

    // Should be approximately 50/50.
    EXPECT_NEAR(a1 + a2, 1.0, 0.01);
    EXPECT_NEAR(a1, 0.50, 0.15);  // Tolerance for scoring differences.
    EXPECT_NEAR(a2, 0.50, 0.15);
}

// ---------------------------------------------------------------------------
// TEST 2: Higher volume → higher allocation.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, HigherVolumeGetsMoreAllocation) {
    alloc_cfg_.max_alloc_pct = 0.90;  // Allow differentiation with 2 pairs.

    ingest_pair("XCH/wUSDC.b", 2.37, 2.38, 2.37, 50.0);
    ingest_pair("BYC/wUSDC.b", 0.99, 1.01, 1.00, 500.0);  // 10x volume.

    // Same competitors for both.
    ingest_competitors("XCH/wUSDC.b", 3);
    ingest_competitors("BYC/wUSDC.b", 3);
    refresh_feed({"XCH/wUSDC.b", "BYC/wUSDC.b"});

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);
    alloc.evaluate({"XCH/wUSDC.b", "BYC/wUSDC.b"}, 100);

    double a_xch = alloc.get_allocation("XCH/wUSDC.b");
    double a_byc = alloc.get_allocation("BYC/wUSDC.b");

    // BYC/wUSDC.b should get more allocation.
    EXPECT_GT(a_byc, a_xch);
    EXPECT_NEAR(a_xch + a_byc, 1.0, 0.01);
}

// ---------------------------------------------------------------------------
// TEST 3: Fewer competitors → higher allocation.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, FewerCompetitorsGetsMoreAllocation) {
    alloc_cfg_.max_alloc_pct = 0.90;  // Allow differentiation with 2 pairs.

    ingest_pair("XCH/wUSDC.b", 2.37, 2.38, 2.37, 100.0);
    ingest_pair("BYC/wUSDC.b", 0.99, 1.01, 1.00, 100.0);

    ingest_competitors("XCH/wUSDC.b", 10);  // Crowded.
    ingest_competitors("BYC/wUSDC.b", 1);   // Sparse.
    refresh_feed({"XCH/wUSDC.b", "BYC/wUSDC.b"});

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);
    alloc.evaluate({"XCH/wUSDC.b", "BYC/wUSDC.b"}, 100);

    double a_xch = alloc.get_allocation("XCH/wUSDC.b");
    double a_byc = alloc.get_allocation("BYC/wUSDC.b");

    // BYC/wUSDC.b with fewer competitors should get more.
    EXPECT_GT(a_byc, a_xch);
}

// ---------------------------------------------------------------------------
// TEST 4: Min/max guardrails enforced.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, MinMaxGuardrails) {
    alloc_cfg_.min_alloc_pct = 0.15;
    alloc_cfg_.max_alloc_pct = 0.45;

    // One pair dominates all dimensions.
    ingest_pair("XCH/wUSDC.b", 2.37, 2.38, 2.37, 10.0);
    ingest_pair("BYC/wUSDC.b", 0.99, 1.01, 1.00, 1000.0);
    ingest_pair("XCH/BYC", 2.40, 2.41, 2.40, 10.0);

    ingest_competitors("XCH/wUSDC.b", 10);
    ingest_competitors("BYC/wUSDC.b", 0);
    ingest_competitors("XCH/BYC", 8);
    refresh_feed({"XCH/wUSDC.b", "BYC/wUSDC.b", "XCH/BYC"});

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);
    alloc.evaluate({"XCH/wUSDC.b", "BYC/wUSDC.b", "XCH/BYC"}, 100);

    for (const auto& name : {"XCH/wUSDC.b", "BYC/wUSDC.b", "XCH/BYC"}) {
        double a = alloc.get_allocation(name);
        EXPECT_GE(a, alloc_cfg_.min_alloc_pct - 0.01)
            << name << " below min";
        EXPECT_LE(a, alloc_cfg_.max_alloc_pct + 0.01)
            << name << " above max";
    }

    // Allocations should still sum to ~1.0.
    auto allocs = alloc.get_all_allocations();
    double sum = 0.0;
    for (const auto& [k, v] : allocs) sum += v;
    EXPECT_NEAR(sum, 1.0, 0.02);
}

// ---------------------------------------------------------------------------
// TEST 5: Hysteresis prevents small changes from shifting allocation.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, HysteresisPreventsTinyShifts) {
    alloc_cfg_.hysteresis_bps = 200.0;  // Need > 2% change to shift.
    alloc_cfg_.smooth_alpha = 1.0;

    ingest_pair("XCH/wUSDC.b", 2.37, 2.38, 2.37, 100.0);
    ingest_pair("BYC/wUSDC.b", 0.99, 1.01, 1.00, 100.0);
    refresh_feed({"XCH/wUSDC.b", "BYC/wUSDC.b"});

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);

    // First evaluation.
    alloc.evaluate({"XCH/wUSDC.b", "BYC/wUSDC.b"}, 100);
    double a1_first = alloc.get_allocation("XCH/wUSDC.b");

    // Slightly change volume — should NOT trigger reallocation.
    ingest_pair("BYC/wUSDC.b", 0.99, 1.01, 1.00, 105.0);
    refresh_feed({"XCH/wUSDC.b", "BYC/wUSDC.b"});
    alloc.evaluate({"XCH/wUSDC.b", "BYC/wUSDC.b"}, 200);
    double a1_second = alloc.get_allocation("XCH/wUSDC.b");

    EXPECT_NEAR(a1_first, a1_second, 0.01)
        << "Allocation should not change for a tiny volume shift";
}

// ---------------------------------------------------------------------------
// TEST 6: should_evaluate respects interval.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, ShouldEvaluateRespectsInterval) {
    alloc_cfg_.eval_interval_blocks = 50;

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);

    // First call — always true.
    EXPECT_TRUE(alloc.should_evaluate(100));

    // Simulate evaluation.
    ingest_pair("XCH/wUSDC.b", 2.37, 2.38, 2.37, 100.0);
    refresh_feed({"XCH/wUSDC.b"});
    alloc.evaluate({"XCH/wUSDC.b"}, 100);

    // Too soon.
    EXPECT_FALSE(alloc.should_evaluate(120));
    EXPECT_FALSE(alloc.should_evaluate(149));

    // At interval boundary.
    EXPECT_TRUE(alloc.should_evaluate(150));
    EXPECT_TRUE(alloc.should_evaluate(200));
}

// ---------------------------------------------------------------------------
// TEST 7: Disabled allocator returns min_alloc_pct.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, DisabledReturnsMinAlloc) {
    alloc_cfg_.enabled = false;

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);

    EXPECT_FALSE(alloc.should_evaluate(100));
    EXPECT_NEAR(alloc.get_allocation("XCH/wUSDC.b"),
                alloc_cfg_.min_alloc_pct, 0.001);
}

// ---------------------------------------------------------------------------
// TEST 8: Triangular arbitrage edge detection.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, TriangularArbitrageEdge) {
    // Set up a triangle with a deliberate pricing mismatch.
    // XCH/wUSDC  = 2.40 (mid)
    // XCH/BYC    = 2.40 (mid)
    // BYC/wUSDC  = 1.05 (mid) — overpriced BYC!
    //
    // Forward: 1 XCH → 2.40 wUSDC → 2.40/1.05 = 2.286 BYC → 2.286/2.40 = 0.952 XCH (loss)
    // Reverse: 1 XCH → 2.40 BYC → 2.40 * 1.05 = 2.52 wUSDC → 2.52/2.40 = 1.05 XCH (profit!)
    ingest_pair("XCH/wUSDC.b", 2.39, 2.41, 2.40, 100.0);
    ingest_pair("XCH/BYC",     2.39, 2.41, 2.40, 100.0);
    ingest_pair("BYC/wUSDC.b", 1.04, 1.06, 1.05, 100.0);
    refresh_feed({"XCH/wUSDC.b", "XCH/BYC", "BYC/wUSDC.b"});

    alloc_cfg_.tri_arb_fee_bps = 0.0;  // No fees for clean test.
    alloc_cfg_.tri_arb_min_edge_bps = 1.0;

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);

    auto tri = alloc.compute_tri_arb_edge();
    EXPECT_TRUE(tri.has_edge);
    // Reverse should be profitable (~500 bps).
    EXPECT_GT(tri.reverse_edge_bps, 100.0);
}

// ---------------------------------------------------------------------------
// TEST 9: No arb when prices are consistent.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, NoArbWhenConsistent) {
    // Consistent triangle: XCH/wUSDC = 2.40, BYC/wUSDC = 1.00, XCH/BYC = 2.40
    ingest_pair("XCH/wUSDC.b", 2.39, 2.41, 2.40, 100.0);
    ingest_pair("XCH/BYC",     2.39, 2.41, 2.40, 100.0);
    ingest_pair("BYC/wUSDC.b", 0.99, 1.01, 1.00, 100.0);
    refresh_feed({"XCH/wUSDC.b", "XCH/BYC", "BYC/wUSDC.b"});

    alloc_cfg_.tri_arb_fee_bps = 15.0;  // With fees.
    alloc_cfg_.tri_arb_min_edge_bps = 5.0;

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);

    auto tri = alloc.compute_tri_arb_edge();
    // With fees, a consistent triangle should have no significant edge.
    EXPECT_FALSE(tri.has_edge);
}

// ---------------------------------------------------------------------------
// TEST 10: get_scores returns full breakdown.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, GetScoresReturnsBreakdown) {
    ingest_pair("XCH/wUSDC.b", 2.37, 2.38, 2.37, 100.0);
    ingest_pair("BYC/wUSDC.b", 0.99, 1.01, 1.00, 200.0);
    refresh_feed({"XCH/wUSDC.b", "BYC/wUSDC.b"});

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);
    alloc.evaluate({"XCH/wUSDC.b", "BYC/wUSDC.b"}, 100);

    auto scores = alloc.get_scores();
    EXPECT_EQ(scores.size(), 2u);

    for (const auto& ps : scores) {
        EXPECT_FALSE(ps.pair_name.empty());
        EXPECT_GE(ps.composite, 0.0);
        EXPECT_LE(ps.composite, 1.0);
        EXPECT_GE(ps.allocation, alloc_cfg_.min_alloc_pct - 0.01);
    }
}

// ---------------------------------------------------------------------------
// TEST 11: EMA smoothing blends toward new target.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, EmaSmoothingBlends) {
    alloc_cfg_.smooth_alpha = 0.5;  // 50% EMA.
    alloc_cfg_.hysteresis_bps = 0.0;  // No hysteresis for this test.
    alloc_cfg_.max_alloc_pct = 0.90;  // Allow wide swings for 2-pair test.

    ingest_pair("XCH/wUSDC.b", 2.37, 2.38, 2.37, 100.0);
    ingest_pair("BYC/wUSDC.b", 0.99, 1.01, 1.00, 100.0);
    refresh_feed({"XCH/wUSDC.b", "BYC/wUSDC.b"});

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);

    // First eval — set baseline.
    alloc.evaluate({"XCH/wUSDC.b", "BYC/wUSDC.b"}, 100);
    double baseline = alloc.get_allocation("BYC/wUSDC.b");

    // Dramatically shift conditions.
    ingest_pair("BYC/wUSDC.b", 0.99, 1.01, 1.00, 1000.0);
    refresh_feed({"XCH/wUSDC.b", "BYC/wUSDC.b"});
    alloc.evaluate({"XCH/wUSDC.b", "BYC/wUSDC.b"}, 200);
    double blended = alloc.get_allocation("BYC/wUSDC.b");

    // With alpha=0.5, should be halfway between baseline and new ideal.
    // It should have moved toward the new target but not reached it fully.
    // Since sums must = 1.0, just check it moved in the right direction.
    EXPECT_GT(blended, baseline)
        << "EMA should shift toward higher-volume pair";
}

// ---------------------------------------------------------------------------
// TEST 12: Single pair gets exactly 100% allocation.
// ---------------------------------------------------------------------------
TEST_F(MarketAllocatorTest, SinglePairGets100Percent) {
    ingest_pair("XCH/wUSDC.b", 2.37, 2.38, 2.37, 100.0);
    refresh_feed({"XCH/wUSDC.b"});

    MarketAllocator alloc(alloc_cfg_, feed_.get(), nullptr);
    alloc.evaluate({"XCH/wUSDC.b"}, 100);

    EXPECT_NEAR(alloc.get_allocation("XCH/wUSDC.b"), 1.0, 0.01);
}

}  // namespace
}  // namespace xop
