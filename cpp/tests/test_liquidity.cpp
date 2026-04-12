// test_liquidity.cpp -- Unit tests for the multi-tier liquidity engine.
//
// Tests cover:
//   1. analyse_order_book_gaps() — gap detection from competing offers
//   2. Adverse-selection-aware tier sizing — inverse-decay weight redistribution
//   3. Gap-aware dynamic tier spacing — blend toward detected gap centres
//   4. AMM-aware mid-price blending in MarketDataFeed::compute_mid()
//   5. Edge cases (empty inputs, single tier, extreme volatility)

#include <gtest/gtest.h>

#include <xop/strategy/liquidity.hpp>
#include <xop/execution/market_data.hpp>
#include <xop/state.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace {

using namespace xop;

// Helper: create a CompetingOffer at a specific price.
CompetingOffer make_offer(Side side, Mojo price, Mojo size = 1'000'000) {
    CompetingOffer o;
    o.offer_id  = "test";
    o.pair_name = "XCH/wUSDC.b";
    o.side      = side;
    o.price     = price;
    o.size      = size;
    o.first_seen_block = 100;
    o.last_seen_block  = 100;
    o.last_seen_ts     = std::chrono::system_clock::now();
    return o;
}

// ============================================================================
// 1. analyse_order_book_gaps
// ============================================================================

TEST(GapDetectionTest, EmptyOffers_NoGaps) {
    auto gaps = analyse_order_book_gaps({}, 1'000'000'000'000LL);
    // Early-return: no offers means nothing to scan, returns empty.
    EXPECT_TRUE(gaps.empty());
}

TEST(GapDetectionTest, ZeroMid_NoGaps) {
    std::vector<CompetingOffer> offers = {make_offer(Side::Bid, 900)};
    auto gaps = analyse_order_book_gaps(offers, 0);
    EXPECT_TRUE(gaps.empty());
}

TEST(GapDetectionTest, SingleBidOffer_GapAboveAndBelow) {
    // Mid = 1,000,000 mojos.  One bid at 995,000 (50 bps from mid).
    const Mojo mid = 1'000'000;
    const Mojo bid_price = 995'000;  // 50 bps below mid
    std::vector<CompetingOffer> offers = {make_offer(Side::Bid, bid_price)};

    auto gaps = analyse_order_book_gaps(offers, mid, 50.0, 1500.0);

    // Should find gap(s) on bid side and one big gap on ask side.
    bool found_ask_gap = false;
    for (const auto& g : gaps) {
        if (g.side == Side::Ask) {
            // Full ask side is empty: 0 to 1500 bps.
            found_ask_gap = true;
            EXPECT_NEAR(g.low_bps, 0.0, 1.0);
            EXPECT_NEAR(g.high_bps, 1500.0, 1.0);
        }
    }
    EXPECT_TRUE(found_ask_gap);
}

TEST(GapDetectionTest, TwoOffersSamePrice_DeduplicateAndDetectGaps) {
    const Mojo mid = 1'000'000;
    // Two bids at same level (~200 bps from mid).
    const Mojo bid_price = 980'000;
    std::vector<CompetingOffer> offers = {
        make_offer(Side::Bid, bid_price),
        make_offer(Side::Bid, bid_price),
    };

    auto gaps = analyse_order_book_gaps(offers, mid, 50.0, 1500.0);

    // After dedup there's one level at ~200 bps.
    // Gaps: [0,200) and [200,1500), plus full ask side.
    bool found_bid_gap_inner = false;
    bool found_bid_gap_outer = false;
    for (const auto& g : gaps) {
        if (g.side == Side::Bid && g.low_bps < 100.0) {
            found_bid_gap_inner = true;
            EXPECT_GT(g.width_bps, 50.0);
        }
        if (g.side == Side::Bid && g.low_bps > 100.0) {
            found_bid_gap_outer = true;
        }
    }
    EXPECT_TRUE(found_bid_gap_inner);
    EXPECT_TRUE(found_bid_gap_outer);
}

TEST(GapDetectionTest, DenseOffers_NoGaps) {
    const Mojo mid = 1'000'000;
    // Place offers at every 20 bps from 10 to 1500 bps.
    std::vector<CompetingOffer> offers;
    for (double bps = 10.0; bps <= 1500.0; bps += 20.0) {
        Mojo price = static_cast<Mojo>(
            static_cast<double>(mid) * (1.0 - bps / 10000.0));
        offers.push_back(make_offer(Side::Bid, price));
    }

    auto gaps = analyse_order_book_gaps(offers, mid, 50.0, 1500.0);

    // With offers every 20 bps, no gap should be >= 50 bps on bid side.
    int bid_gaps = 0;
    for (const auto& g : gaps) {
        if (g.side == Side::Bid) ++bid_gaps;
    }
    EXPECT_EQ(bid_gaps, 0);
}

TEST(GapDetectionTest, SortedByWidthDescending) {
    const Mojo mid = 1'000'000;
    // Bid side: offers at 100 bps and 600 bps → gaps at [0,100] and [100,600]
    std::vector<CompetingOffer> offers = {
        make_offer(Side::Bid, static_cast<Mojo>(mid * (1.0 - 100.0/10000.0))),
        make_offer(Side::Bid, static_cast<Mojo>(mid * (1.0 - 600.0/10000.0))),
    };

    auto gaps = analyse_order_book_gaps(offers, mid, 50.0, 1500.0);

    // Verify widest gap comes first.
    for (std::size_t i = 1; i < gaps.size(); ++i) {
        EXPECT_GE(gaps[i - 1].width_bps, gaps[i].width_bps);
    }
}

// ============================================================================
// 2. Adverse-selection-aware tier sizing
// ============================================================================

class AdverseSelectionSizingTest : public ::testing::Test {
protected:
    LiquidityConfig make_config(double decay, bool enabled = true) {
        LiquidityConfig cfg;
        cfg.num_tiers = 4;
        cfg.tier_spacing_bps = {60.0, 200.0, 500.0, 1000.0};
        cfg.tier_size_pct = {0.30, 0.25, 0.25, 0.20};
        cfg.adverse_selection_sizing = enabled;
        cfg.adverse_selection_decay = decay;
        cfg.adverse_selection_sigma_threshold = 0.05;
        cfg.gap_aware_spacing = false;  // isolate sizing test
        return cfg;
    }
};

TEST_F(AdverseSelectionSizingTest, Enabled_TierZeroShrinks) {
    LiquidityConfig cfg = make_config(0.7);
    LiquidityEngine engine("TEST/PAIR", cfg);

    const Mojo mid = 1'000'000'000'000LL;
    const Mojo capital = 10'000'000'000'000LL;
    const Mojo inventory = 10'000'000'000'000LL;

    auto ladder = engine.compute_ladder(
        mid, 0.03, 0.5, capital, inventory,
        {}, cfg);

    // With adverse selection sizing, tier 0 should get less than the
    // default 30%.  Find tier 0 bid.
    Mojo tier0_bid_size = 0;
    Mojo tier3_bid_size = 0;
    for (const auto& tq : ladder) {
        if (tq.side == Side::Bid && tq.tier_index == 0) tier0_bid_size = tq.size;
        if (tq.side == Side::Bid && tq.tier_index == 3) tier3_bid_size = tq.size;
    }

    // Tier 0 should be smaller than tier 3.
    EXPECT_GT(tier3_bid_size, tier0_bid_size);

    // Tier 0 should be significantly less than 30% of capital.
    const double tier0_frac = static_cast<double>(tier0_bid_size)
                            / static_cast<double>(capital);
    EXPECT_LT(tier0_frac, 0.20);
}

TEST_F(AdverseSelectionSizingTest, Disabled_DefaultSizes) {
    LiquidityConfig cfg = make_config(0.7, /*enabled=*/false);
    LiquidityEngine engine("TEST/PAIR", cfg);

    const Mojo mid = 1'000'000'000'000LL;
    const Mojo capital = 10'000'000'000'000LL;
    const Mojo inventory = 10'000'000'000'000LL;

    auto ladder = engine.compute_ladder(
        mid, 0.03, 0.5, capital, inventory,
        {}, cfg);

    // With sizing disabled, tier 0 should get 30% of capital.
    Mojo tier0_bid_size = 0;
    for (const auto& tq : ladder) {
        if (tq.side == Side::Bid && tq.tier_index == 0) {
            tier0_bid_size = tq.size;
            break;
        }
    }

    const double tier0_frac = static_cast<double>(tier0_bid_size)
                            / static_cast<double>(capital);
    EXPECT_NEAR(tier0_frac, 0.30, 0.01);
}

TEST_F(AdverseSelectionSizingTest, HighVolatility_MoreAggressive) {
    LiquidityConfig cfg = make_config(0.7);
    LiquidityEngine engine("TEST/PAIR", cfg);

    const Mojo mid = 1'000'000'000'000LL;
    const Mojo capital = 10'000'000'000'000LL;
    const Mojo inventory = 10'000'000'000'000LL;

    // Low vol: tier 0 gets some fraction.
    auto ladder_low = engine.compute_ladder(
        mid, 0.03, 0.5, capital, inventory, {}, cfg);

    // High vol (above threshold): decay halved → even less on tier 0.
    auto ladder_high = engine.compute_ladder(
        mid, 0.10, 0.5, capital, inventory, {}, cfg);

    Mojo t0_low = 0, t0_high = 0;
    for (const auto& tq : ladder_low) {
        if (tq.side == Side::Bid && tq.tier_index == 0) { t0_low = tq.size; break; }
    }
    for (const auto& tq : ladder_high) {
        if (tq.side == Side::Bid && tq.tier_index == 0) { t0_high = tq.size; break; }
    }

    // High vol should give tier 0 even less capital.
    EXPECT_LT(t0_high, t0_low);
}

TEST_F(AdverseSelectionSizingTest, SizeSumPreserved) {
    // Total allocated capital should be the same regardless of decay.
    LiquidityConfig cfg = make_config(0.7);
    LiquidityEngine engine("TEST/PAIR", cfg);

    const Mojo mid = 1'000'000'000'000LL;
    const Mojo capital = 10'000'000'000'000LL;
    const Mojo inventory = 10'000'000'000'000LL;

    auto ladder = engine.compute_ladder(
        mid, 0.03, 0.5, capital, inventory, {}, cfg);

    Mojo total_bid = 0;
    for (const auto& tq : ladder) {
        if (tq.side == Side::Bid) total_bid += tq.size;
    }

    // Total bid sizes should sum to approximately capital
    // (within rounding tolerance).
    const double total_frac = static_cast<double>(total_bid)
                            / static_cast<double>(capital);
    EXPECT_NEAR(total_frac, 1.0, 0.02);
}

// ============================================================================
// 3. Gap-aware dynamic tier spacing
// ============================================================================

class GapAwareSpacingTest : public ::testing::Test {
protected:
    LiquidityConfig make_config() {
        LiquidityConfig cfg;
        cfg.num_tiers = 4;
        cfg.tier_spacing_bps = {60.0, 200.0, 500.0, 1000.0};
        cfg.tier_size_pct = {0.30, 0.25, 0.25, 0.20};
        cfg.gap_aware_spacing = true;
        cfg.min_gap_bps = 50.0;
        cfg.max_gap_scan_bps = 1500.0;
        cfg.gap_blend_factor = 0.6;
        cfg.adverse_selection_sizing = false;  // isolate spacing test
        return cfg;
    }
};

TEST_F(GapAwareSpacingTest, NoCompetitors_BaselineSpacing) {
    LiquidityConfig cfg = make_config();
    LiquidityEngine engine("TEST/PAIR", cfg);

    const Mojo mid = 1'000'000'000'000LL;

    // With no competing offers, should fall back to baseline.
    auto ladder_no_comp = engine.compute_ladder(
        mid, 0.03, 0.5, 10'000'000'000'000LL, 10'000'000'000'000LL);

    auto ladder_with_comp = engine.compute_ladder(
        mid, 0.03, 0.5, 10'000'000'000'000LL, 10'000'000'000'000LL,
        {}, cfg);

    // Both should produce the same result.
    ASSERT_EQ(ladder_no_comp.size(), ladder_with_comp.size());
    for (std::size_t i = 0; i < ladder_no_comp.size(); ++i) {
        EXPECT_EQ(ladder_no_comp[i].price, ladder_with_comp[i].price);
    }
}

TEST_F(GapAwareSpacingTest, GapPresent_ShiftsTierToward) {
    LiquidityConfig cfg = make_config();
    LiquidityEngine engine("TEST/PAIR", cfg);

    const Mojo mid = 1'000'000'000'000LL;

    // Create a gap: bid offers at 30 bps and 400 bps, leaving a
    // ~370 bps gap between them.  Baseline tier 1 is at 200 bps;
    // it should shift toward the gap centre (~215 bps).
    std::vector<CompetingOffer> offers = {
        make_offer(Side::Bid, static_cast<Mojo>(
            static_cast<double>(mid) * (1.0 - 30.0/10000.0))),
        make_offer(Side::Bid, static_cast<Mojo>(
            static_cast<double>(mid) * (1.0 - 400.0/10000.0))),
    };

    auto ladder_gap = engine.compute_ladder(
        mid, 0.03, 0.5, 10'000'000'000'000LL, 10'000'000'000'000LL,
        offers, cfg);

    auto ladder_base = engine.compute_ladder(
        mid, 0.03, 0.5, 10'000'000'000'000LL, 10'000'000'000'000LL,
        {}, cfg);

    // The gap-aware ladder should differ from baseline.
    bool any_different = false;
    for (std::size_t i = 0; i < std::min(ladder_gap.size(), ladder_base.size()); ++i) {
        if (ladder_gap[i].price != ladder_base[i].price) {
            any_different = true;
            break;
        }
    }
    EXPECT_TRUE(any_different);
}

TEST_F(GapAwareSpacingTest, SpacingRemainsAscending) {
    LiquidityConfig cfg = make_config();
    LiquidityEngine engine("TEST/PAIR", cfg);

    const Mojo mid = 1'000'000'000'000LL;

    // Place offers to create gaps at irregular positions.
    std::vector<CompetingOffer> offers = {
        make_offer(Side::Bid, static_cast<Mojo>(mid * 0.999)),   // ~10 bps
        make_offer(Side::Bid, static_cast<Mojo>(mid * 0.990)),   // ~100 bps
        make_offer(Side::Bid, static_cast<Mojo>(mid * 0.970)),   // ~300 bps
        make_offer(Side::Ask, static_cast<Mojo>(mid * 1.001)),   // ~10 bps
        make_offer(Side::Ask, static_cast<Mojo>(mid * 1.008)),   // ~80 bps
    };

    auto ladder = engine.compute_ladder(
        mid, 0.03, 0.5, 10'000'000'000'000LL, 10'000'000'000'000LL,
        offers, cfg);

    // Extract bid tier spreads and verify ascending.
    std::vector<double> bid_spreads;
    for (const auto& tq : ladder) {
        if (tq.side == Side::Bid) {
            bid_spreads.push_back(tq.spread_bps);
        }
    }
    for (std::size_t i = 1; i < bid_spreads.size(); ++i) {
        EXPECT_GT(bid_spreads[i], bid_spreads[i - 1])
            << "Bid tier " << i << " spread must be > tier " << (i-1);
    }
}

TEST_F(GapAwareSpacingTest, BlendFactorZero_BaselineUnchanged) {
    LiquidityConfig cfg = make_config();
    cfg.gap_blend_factor = 0.0;  // No blending.
    LiquidityEngine engine("TEST/PAIR", cfg);

    const Mojo mid = 1'000'000'000'000LL;
    std::vector<CompetingOffer> offers = {
        make_offer(Side::Bid, static_cast<Mojo>(mid * 0.99)),
    };

    auto ladder_blend0 = engine.compute_ladder(
        mid, 0.03, 0.5, 10'000'000'000'000LL, 10'000'000'000'000LL,
        offers, cfg);

    auto ladder_baseline = engine.compute_ladder(
        mid, 0.03, 0.5, 10'000'000'000'000LL, 10'000'000'000'000LL);

    // With blend=0, gap-aware should produce identical results to baseline.
    ASSERT_EQ(ladder_blend0.size(), ladder_baseline.size());
    for (std::size_t i = 0; i < ladder_blend0.size(); ++i) {
        EXPECT_EQ(ladder_blend0[i].price, ladder_baseline[i].price);
    }
}

// ============================================================================
// 4. AMM-aware mid-price blending
// ============================================================================

TEST(AmmMidPriceTest, IngestAndBlend) {
    // Create a MarketDataFeed and verify AMM blending.
    State state;
    MarketDataConfig md_cfg;
    md_cfg.amm_blend_weight = 0.25;
    md_cfg.amm_freshness_threshold_sec = 300.0;
    md_cfg.cex_freshness_threshold_sec = 0.0;  // Disable CEX freshness decay
    MarketDataFeed feed(md_cfg, state);

    const std::string pair = "XCH/wUSDC.b";

    // Ingest DEX data: bid=0.40, ask=0.42 → DEX mid = 0.41
    feed.ingest_dexie(pair, 0.40, 0.42, 0.41, 100.0);

    // Ingest AMM mid = 0.45 (2-way divergent from DEX).
    feed.ingest_amm_mid(pair, 0.45);

    // Refresh to compute mid.
    feed.refresh({pair});

    double mid = feed.get_mid_price(pair);

    // Without CEX:
    //   w_dex = 0.70, w_amm = 0.25, w_cex = 0.0
    //   normalised: w_dex = 0.70/0.95 ≈ 0.7368, w_amm = 0.25/0.95 ≈ 0.2632
    //   mid = 0.7368 * 0.41 + 0.2632 * 0.45 ≈ 0.3021 + 0.1184 ≈ 0.4205
    EXPECT_GT(mid, 0.41);   // Pulled up toward AMM price.
    EXPECT_LT(mid, 0.45);   // But not fully.
    EXPECT_NEAR(mid, 0.4205, 0.005);
}

TEST(AmmMidPriceTest, AmmWeightZero_DexOnly) {
    State state;
    MarketDataConfig md_cfg;
    md_cfg.amm_blend_weight = 0.0;  // Disabled.
    md_cfg.cex_freshness_threshold_sec = 0.0;
    MarketDataFeed feed(md_cfg, state);

    feed.ingest_dexie("P", 0.40, 0.42, 0.41, 100.0);
    feed.ingest_amm_mid("P", 0.90);  // Should be ignored.
    feed.refresh({"P"});

    double mid = feed.get_mid_price("P");
    EXPECT_NEAR(mid, 0.41, 0.001);  // Pure DEX mid.
}

TEST(AmmMidPriceTest, InvalidAmmMid_Ignored) {
    State state;
    MarketDataConfig md_cfg;
    md_cfg.amm_blend_weight = 0.25;
    MarketDataFeed feed(md_cfg, state);

    feed.ingest_dexie("P", 0.40, 0.42, 0.41, 100.0);
    feed.ingest_amm_mid("P", -1.0);   // Invalid — should be ignored.
    feed.refresh({"P"});

    double mid = feed.get_mid_price("P");
    EXPECT_NEAR(mid, 0.41, 0.001);
}

// ============================================================================
// 5. Edge cases
// ============================================================================

TEST(LiquidityEdgeTest, SingleTier_AdverseSelectionNoEffect) {
    LiquidityConfig cfg;
    cfg.num_tiers = 1;
    cfg.tier_spacing_bps = {100.0};
    cfg.tier_size_pct = {1.0};
    cfg.adverse_selection_sizing = true;
    cfg.adverse_selection_decay = 0.7;
    cfg.gap_aware_spacing = false;
    LiquidityEngine engine("T/P", cfg);

    auto ladder = engine.compute_ladder(
        1'000'000'000'000LL, 0.03, 0.5,
        5'000'000'000'000LL, 5'000'000'000'000LL,
        {}, cfg);

    // With 1 tier, adverse selection sizing has no effect (only 1 weight).
    // Should produce 2 quotes: 1 bid + 1 ask.
    EXPECT_EQ(ladder.size(), 2u);

    Mojo bid_size = 0, ask_size = 0;
    for (const auto& tq : ladder) {
        if (tq.side == Side::Bid) bid_size = tq.size;
        if (tq.side == Side::Ask) ask_size = tq.size;
    }
    EXPECT_GT(bid_size, 0);
    EXPECT_GT(ask_size, 0);
}

TEST(LiquidityEdgeTest, ZeroMid_EmptyLadder) {
    LiquidityConfig cfg;
    cfg.num_tiers = 4;
    cfg.tier_spacing_bps = {60.0, 200.0, 500.0, 1000.0};
    cfg.tier_size_pct = {0.30, 0.25, 0.25, 0.20};
    LiquidityEngine engine("T/P", cfg);

    auto ladder = engine.compute_ladder(
        0, 0.03, 0.5,
        5'000'000'000'000LL, 5'000'000'000'000LL,
        {}, cfg);

    EXPECT_TRUE(ladder.empty());
}

TEST(LiquidityEdgeTest, GetCompetingOffers_EmptyWhenNone) {
    State state;
    MarketDataConfig md_cfg;
    MarketDataFeed feed(md_cfg, state);

    auto offers = feed.get_competing_offers("NONEXISTENT");
    EXPECT_TRUE(offers.empty());
}

// ============================================================================
// 6. Competitive Anchor Pricing
// ============================================================================

// Helper: make a LiquidityConfig suitable for competitive anchor tests.
LiquidityConfig make_anchor_config(uint32_t tiers = 3) {
    LiquidityConfig cfg;
    cfg.num_tiers = tiers;
    cfg.tier_spacing_bps.resize(tiers);
    cfg.tier_size_pct.resize(tiers);
    for (uint32_t i = 0; i < tiers; ++i) {
        cfg.tier_spacing_bps[i] = 50.0 + i * 50.0;
        cfg.tier_size_pct[i] = 1.0 / static_cast<double>(tiers);
    }
    cfg.competitive_anchor_enabled = true;
    cfg.competitive_anchor_stride_bps = 65.0;
    cfg.competitive_anchor_max_distance_bps = 500.0;
    cfg.gap_aware_spacing = false;
    cfg.adverse_selection_sizing = false;
    cfg.fill_rate_sizing = false;
    return cfg;
}

TEST(CompetitiveAnchorTest, BothSidesAnchored) {
    // Setup: mid=2.0, best_comp_bid=1.99, best_comp_ask=2.01
    constexpr Mojo mid = 2'000'000'000'000LL;
    constexpr Mojo comp_bid = 1'990'000'000'000LL;
    constexpr Mojo comp_ask = 2'010'000'000'000LL;

    auto cfg = make_anchor_config(3);
    LiquidityEngine engine("T/Q", cfg);

    std::vector<CompetingOffer> offers;
    offers.push_back(make_offer(Side::Bid, comp_bid, 1'000'000'000'000LL));
    offers.push_back(make_offer(Side::Ask, comp_ask, 1'000'000'000'000LL));

    auto ladder = engine.compute_ladder(
        mid, 0.03, 0.5,
        10'000'000'000'000LL, 10'000'000'000'000LL,
        offers, cfg);

    ASSERT_FALSE(ladder.empty());

    // Tier 0 bid should be 1 tick above comp_bid (i.e., closer to mid).
    auto bid_t0 = std::find_if(ladder.begin(), ladder.end(),
        [](const TierQuote& tq) { return tq.side == Side::Bid && tq.tier_index == 0; });
    ASSERT_NE(bid_t0, ladder.end());
    EXPECT_GT(bid_t0->price, comp_bid);
    EXPECT_LE(bid_t0->price, mid);

    // Tier 0 ask should be 1 tick below comp_ask (i.e., closer to mid).
    auto ask_t0 = std::find_if(ladder.begin(), ladder.end(),
        [](const TierQuote& tq) { return tq.side == Side::Ask && tq.tier_index == 0; });
    ASSERT_NE(ask_t0, ladder.end());
    EXPECT_LT(ask_t0->price, comp_ask);
    EXPECT_GE(ask_t0->price, mid);
}

TEST(CompetitiveAnchorTest, AskSide_MidAboveBestAsk) {
    // Scenario: model mid (2.30) sits above best competing ask (2.28).
    // This is the exact bug scenario -- with the fix, ask-side anchoring
    // should still work because the BBO reference is used for safety.
    constexpr Mojo mid       = 2'300'000'000'000LL;
    constexpr Mojo comp_bid  = 2'270'000'000'000LL;
    constexpr Mojo comp_ask  = 2'280'000'000'000LL;
    // BBO ref = (2.27 + 2.28) / 2 = 2.275

    auto cfg = make_anchor_config(3);
    LiquidityEngine engine("T/Q", cfg);

    std::vector<CompetingOffer> offers;
    offers.push_back(make_offer(Side::Bid, comp_bid, 2'000'000'000'000LL));
    offers.push_back(make_offer(Side::Ask, comp_ask, 2'000'000'000'000LL));

    auto ladder = engine.compute_ladder(
        mid, 0.03, 0.5,
        10'000'000'000'000LL, 10'000'000'000'000LL,
        offers, cfg);

    ASSERT_FALSE(ladder.empty());

    // Ask Tier 0: anchor = comp_ask - 1tick = ~2.2798
    // Safety: new_price >= bbo_ref (2.275), which 2.2798 satisfies.
    auto ask_t0 = std::find_if(ladder.begin(), ladder.end(),
        [](const TierQuote& tq) { return tq.side == Side::Ask && tq.tier_index == 0; });
    ASSERT_NE(ask_t0, ladder.end());
    // The ask should be near 2.28, NOT stuck at 2.30+ (the old bug).
    EXPECT_LT(ask_t0->price, mid);
    EXPECT_GE(ask_t0->price, comp_bid);  // never below best bid
}

TEST(CompetitiveAnchorTest, DistanceTooFar_FallsBack) {
    // When the best competing offer is > max_distance_bps from mid,
    // anchoring should be skipped (offers remain at original positions).
    constexpr Mojo mid      = 2'000'000'000'000LL;
    constexpr Mojo far_bid  = 1'800'000'000'000LL;  // 1000bps from mid
    constexpr Mojo far_ask  = 2'200'000'000'000LL;  // 1000bps from mid

    auto cfg = make_anchor_config(2);
    cfg.competitive_anchor_max_distance_bps = 500.0;
    LiquidityEngine engine("T/Q", cfg);

    std::vector<CompetingOffer> offers;
    offers.push_back(make_offer(Side::Bid, far_bid, 1'000'000'000'000LL));
    offers.push_back(make_offer(Side::Ask, far_ask, 1'000'000'000'000LL));

    auto ladder = engine.compute_ladder(
        mid, 0.03, 0.5,
        10'000'000'000'000LL, 10'000'000'000'000LL,
        offers, cfg);

    ASSERT_FALSE(ladder.empty());

    // Tier 0 bid should NOT be near far_bid (not anchored).
    auto bid_t0 = std::find_if(ladder.begin(), ladder.end(),
        [](const TierQuote& tq) { return tq.side == Side::Bid && tq.tier_index == 0; });
    ASSERT_NE(bid_t0, ladder.end());
    // Un-anchored bid should be closer to mid than far_bid.
    EXPECT_GT(bid_t0->price, far_bid + 100'000'000'000LL);
}

TEST(CompetitiveAnchorTest, OneSidedCompetition_BidOnly) {
    // Only bid-side competing offers exist; ask side should fall back.
    constexpr Mojo mid      = 2'000'000'000'000LL;
    constexpr Mojo comp_bid = 1'990'000'000'000LL;

    auto cfg = make_anchor_config(2);
    LiquidityEngine engine("T/Q", cfg);

    std::vector<CompetingOffer> offers;
    offers.push_back(make_offer(Side::Bid, comp_bid, 1'000'000'000'000LL));

    auto ladder = engine.compute_ladder(
        mid, 0.03, 0.5,
        10'000'000'000'000LL, 10'000'000'000'000LL,
        offers, cfg);

    ASSERT_FALSE(ladder.empty());

    // Bid should be anchored (near comp_bid + tick).
    auto bid_t0 = std::find_if(ladder.begin(), ladder.end(),
        [](const TierQuote& tq) { return tq.side == Side::Bid && tq.tier_index == 0; });
    ASSERT_NE(bid_t0, ladder.end());
    EXPECT_GT(bid_t0->price, comp_bid);
    EXPECT_LT(bid_t0->price, comp_bid + 1'000'000'000LL);

    // Ask should NOT be anchored (no competing asks).
    // It should be at the model-derived position (above mid).
    auto ask_t0 = std::find_if(ladder.begin(), ladder.end(),
        [](const TierQuote& tq) { return tq.side == Side::Ask && tq.tier_index == 0; });
    ASSERT_NE(ask_t0, ladder.end());
    EXPECT_GE(ask_t0->price, mid);
}

TEST(CompetitiveAnchorTest, TierStride_Descends) {
    // Verify that bid tiers step DOWN from the anchor by stride_bps.
    constexpr Mojo mid      = 2'000'000'000'000LL;
    constexpr Mojo comp_bid = 1'990'000'000'000LL;
    constexpr Mojo comp_ask = 2'010'000'000'000LL;

    auto cfg = make_anchor_config(3);
    cfg.competitive_anchor_stride_bps = 100.0;  // 100bps per tier
    LiquidityEngine engine("T/Q", cfg);

    std::vector<CompetingOffer> offers;
    offers.push_back(make_offer(Side::Bid, comp_bid, 1'000'000'000'000LL));
    offers.push_back(make_offer(Side::Ask, comp_ask, 1'000'000'000'000LL));

    auto ladder = engine.compute_ladder(
        mid, 0.03, 0.5,
        10'000'000'000'000LL, 10'000'000'000'000LL,
        offers, cfg);

    // Collect bid tier prices sorted by tier_index.
    std::vector<Mojo> bid_prices;
    for (const auto& tq : ladder) {
        if (tq.side == Side::Bid) bid_prices.push_back(tq.price);
    }
    std::sort(bid_prices.begin(), bid_prices.end(), std::greater<>());

    ASSERT_GE(bid_prices.size(), 2u);
    // Each subsequent bid tier should be lower (further from mid).
    for (size_t i = 1; i < bid_prices.size(); ++i) {
        EXPECT_LT(bid_prices[i], bid_prices[i - 1])
            << "Bid tier " << i << " should be lower than tier " << (i - 1);
    }
}

// ============================================================================
// 7. Dust Filter Denomination Awareness
// ============================================================================

TEST(DustFilterTest, BidSideNotFilteredWithCorrectDenom) {
    // Verify that bid-side competing offers denominated in a small-mpu
    // quote asset (e.g. wUSDC with 1e3 mojos/unit) are NOT filtered
    // when quote_mojos_per_unit is provided correctly.
    State state;
    MarketDataConfig md_cfg;
    md_cfg.enable_competitor_tracking = true;
    md_cfg.min_competitor_offer_size = 100'000'000'000LL;  // 0.1 XCH
    MarketDataFeed feed(md_cfg, state);

    constexpr Mojo xch_mpu  = 1'000'000'000'000LL;
    constexpr Mojo usdc_mpu = 1'000LL;  // wUSDC.b

    std::vector<CompetingOffer> offers;
    // A bid offering 2.28 wUSDC (= 2280 mojos in wUSDC denomination).
    // This is ~1 XCH worth — a legitimate offer, not dust.
    CompetingOffer bid;
    bid.offer_id = "bid1";
    bid.pair_name = "XCH/wUSDC.b";
    bid.side = Side::Bid;
    bid.price = 2'280'000'000'000LL;
    bid.size = 2280;  // 2.28 wUSDC * 1000 mojos/wUSDC
    bid.first_seen_block = 100;
    bid.last_seen_block = 100;
    bid.last_seen_ts = std::chrono::system_clock::now();
    offers.push_back(bid);

    // An ask offering 1 XCH (= 1e12 mojos).
    CompetingOffer ask;
    ask.offer_id = "ask1";
    ask.pair_name = "XCH/wUSDC.b";
    ask.side = Side::Ask;
    ask.price = 2'280'000'000'000LL;
    ask.size = 1'000'000'000'000LL;
    ask.first_seen_block = 100;
    ask.last_seen_block = 100;
    ask.last_seen_ts = std::chrono::system_clock::now();
    offers.push_back(ask);

    std::unordered_set<std::string> own_ids;

    // With correct denomination: both offers should survive dust filter.
    feed.ingest_competing_offers("XCH/wUSDC.b", offers, own_ids,
                                  xch_mpu, usdc_mpu);
    auto result = feed.get_competing_offers("XCH/wUSDC.b");
    EXPECT_EQ(result.size(), 2u)
        << "Both bid and ask should survive with correct denomination";

    // Verify bid is present.
    bool has_bid = false;
    bool has_ask = false;
    for (const auto& o : result) {
        if (o.side == Side::Bid) has_bid = true;
        if (o.side == Side::Ask) has_ask = true;
    }
    EXPECT_TRUE(has_bid) << "Bid should NOT be filtered as dust";
    EXPECT_TRUE(has_ask) << "Ask should NOT be filtered as dust";
}

TEST(DustFilterTest, BidSideFilteredWithOldDefault) {
    // Demonstrate the OLD bug: with default base_mojos_per_unit only,
    // the bid-side offer (2280 mojos) would be filtered as dust
    // against the 1e12 threshold.
    State state;
    MarketDataConfig md_cfg;
    md_cfg.enable_competitor_tracking = true;
    md_cfg.min_competitor_offer_size = 100'000'000'000LL;
    MarketDataFeed feed(md_cfg, state);

    std::vector<CompetingOffer> offers;
    CompetingOffer bid;
    bid.offer_id = "bid1";
    bid.pair_name = "XCH/wUSDC.b";
    bid.side = Side::Bid;
    bid.price = 2'280'000'000'000LL;
    bid.size = 2280;  // 2.28 wUSDC in cat mojos — tiny vs 1e12 threshold
    bid.first_seen_block = 100;
    bid.last_seen_block = 100;
    bid.last_seen_ts = std::chrono::system_clock::now();
    offers.push_back(bid);

    std::unordered_set<std::string> own_ids;

    // With both params defaulting to 1e12 — bid size 2280 < 1e12 → filtered.
    feed.ingest_competing_offers("XCH/wUSDC.b", offers, own_ids);
    auto result = feed.get_competing_offers("XCH/wUSDC.b");
    EXPECT_EQ(result.size(), 0u)
        << "With default denomination, tiny bid should be filtered";
}

}  // namespace
