// test_liquidity.cpp -- Unit tests for the multi-tier liquidity engine.
//
// Tests cover:
//   1. analyse_order_book_gaps() — gap detection from competing offers
//   2. Adverse-selection-aware tier sizing — inverse-decay weight redistribution
//   3. Gap-aware dynamic tier spacing — blend toward detected gap centres
//   4. AMM-aware mid-price blending in MarketDataFeed::compute_mid()
//   5. Edge cases (empty inputs, single tier, extreme volatility)

#include <gtest/gtest.h>

#include <xop/config.hpp>
#include <xop/strategy/liquidity.hpp>
#include <xop/execution/market_data.hpp>
#include <xop/state.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
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
// [AS-WARM recalibration 2026-08-01] sigma-threshold units regression
// ============================================================================
//
// The decay-halving branch compares ANNUALIZED sigma against
// adverse_selection_sigma_threshold.  The old defaults (0.05 in code, 0.005
// in config.yaml) were tuned while the volatility estimator was never ready
// and sigma sat at the 0.001 floor, so the branch never fired in production.
// The warm-start made sigma honest (measured 0.4-1.9 annualized across
// pairs); an unrecalibrated threshold would fire PERMANENTLY and silently
// replace the configured tier profile with an outer-heavy one.  These tests
// pin the new 2.0 default and the production tier-weight arithmetic on both
// sides of it.
//
// Production profile (config.yaml): decay = 0.85, tier_size_pct =
// [10, 12, 15, 18, 22, 23]%.
//
// Below threshold (base decay 0.85), weights_i = 1/0.85^i:
//   [1, 1.1765, 1.3841, 1.6283, 1.9157, 2.2537], sum = 9.3583
//   -> [10.69, 12.57, 14.79, 17.40, 20.47, 24.08]%
//   i.e. within ~1.2 pp of the configured sizes (they were evidently
//   calibrated to this profile).
//
// Above threshold (halved decay 0.425), weights_i = 1/0.425^i:
//   [1, 2.3529, 5.5363, 13.0267, 30.6510, 72.1200], sum = 124.687
//   -> [0.80, 1.89, 4.44, 10.45, 24.58, 57.84]%
//   ~82% of ladder capital in the 230-300 bps outer tiers.  Nobody chose
//   that as the everyday regime; it must be reserved for genuinely extreme
//   volatility (> 200% annualized).

class SigmaThresholdRecalibrationTest : public ::testing::Test {
protected:
    // Production-shaped config; adverse_selection_sigma_threshold is left
    // at the compiled DEFAULT on purpose -- that default is what this test
    // pins.
    LiquidityConfig make_production_config() {
        LiquidityConfig cfg;
        cfg.num_tiers = 6;
        cfg.tier_spacing_bps = {30.0, 60.0, 90.0, 160.0, 230.0, 300.0};
        cfg.tier_size_pct = {0.10, 0.12, 0.15, 0.18, 0.22, 0.23};
        cfg.adverse_selection_sizing = true;
        cfg.adverse_selection_decay = 0.85;
        cfg.gap_aware_spacing = false;  // isolate sizing
        return cfg;
    }

    static double bid_frac(const std::vector<TierQuote>& ladder,
                           std::uint32_t tier, Mojo capital) {
        for (const auto& tq : ladder) {
            if (tq.side == Side::Bid && tq.tier_index == tier) {
                return static_cast<double>(tq.size)
                     / static_cast<double>(capital);
            }
        }
        return -1.0;
    }
};

TEST_F(SigmaThresholdRecalibrationTest, DefaultIsAnnualizedTwoPointZero) {
    EXPECT_DOUBLE_EQ(LiquidityConfig{}.adverse_selection_sigma_threshold, 2.0);
    EXPECT_DOUBLE_EQ(
        xop::StrategyConfig{}.adverse_selection_sigma_threshold, 2.0);
}

TEST_F(SigmaThresholdRecalibrationTest,
       MeasuredBaselineVolKeepsConfiguredProfile) {
    LiquidityConfig cfg = make_production_config();
    LiquidityEngine engine("TEST/PAIR", cfg);

    // Capital large enough that no tier hits build_raw_ladder's 1.0-XCH
    // tier-0 minimum (which would mask the weight profile under test).
    const Mojo mid       = 1'000'000'000'000LL;
    const Mojo capital   = 1'000'000'000'000'000LL;  // 1000 XCH
    const Mojo inventory = 1'000'000'000'000'000LL;

    // 1.9 annualized: the TOP of the measured baseline range (XCH/BYC,
    // itself partly inflated by pre-fix self-priced mids in the warm-start
    // history).  Must NOT trip the halving.
    auto ladder = engine.compute_ladder(
        mid, /*sigma=*/1.9, 0.5, capital, inventory, {}, cfg);

    // Unhalved decay-0.85 profile, ~= the configured [10..23]% sizes.
    EXPECT_NEAR(bid_frac(ladder, 0, capital), 0.1069, 0.005);
    EXPECT_NEAR(bid_frac(ladder, 5, capital), 0.2408, 0.005);
    // The outer-heavy signature of the halved branch (tier5 = 57.8%) must
    // be nowhere in sight at normal volatility.
    EXPECT_LT(bid_frac(ladder, 5, capital), 0.30);
    EXPECT_GT(bid_frac(ladder, 0, capital), 0.08);
}

TEST_F(SigmaThresholdRecalibrationTest, ExtremeVolStillHalvesDecay) {
    LiquidityConfig cfg = make_production_config();
    LiquidityEngine engine("TEST/PAIR", cfg);

    // Same large-capital note as above: the halved tier-0 weight (0.80%)
    // must clear the 1.0-XCH tier-0 minimum to be observable.
    const Mojo mid       = 1'000'000'000'000LL;
    const Mojo capital   = 1'000'000'000'000'000LL;  // 1000 XCH
    const Mojo inventory = 1'000'000'000'000'000LL;

    // 2.5 annualized (> 2.0): genuinely extreme regime -- the conservative
    // outer-heavy reallocation is exactly what we want here.
    auto ladder = engine.compute_ladder(
        mid, /*sigma=*/2.5, 0.5, capital, inventory, {}, cfg);

    // Halved decay 0.425 -> [0.80, 1.89, 4.44, 10.45, 24.58, 57.84]%.
    EXPECT_NEAR(bid_frac(ladder, 0, capital), 0.0080, 0.002);
    EXPECT_NEAR(bid_frac(ladder, 5, capital), 0.5784, 0.01);
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

TEST(LiquidityEdgeTest, ZeroAskPoolStillGeneratesBids) {
    LiquidityConfig cfg;
    cfg.num_tiers = 2;
    cfg.tier_spacing_bps = {50.0, 100.0};
    cfg.tier_size_pct = {0.40, 0.60};
    cfg.gap_aware_spacing = false;
    cfg.adverse_selection_sizing = false;
    cfg.fill_rate_sizing = false;
    LiquidityEngine engine("T/P", cfg);

    const Mojo capital = 10'000'000'000'000LL;
    auto ladder = engine.compute_ladder(
        1'000'000'000'000LL, 0.03, 0.5,
        capital, 0,
        {}, cfg);

    std::size_t bid_count = 0;
    std::size_t ask_count = 0;
    Mojo total_bid = 0;
    for (const auto& quote : ladder) {
        if (quote.side == Side::Bid) {
            ++bid_count;
            total_bid += quote.size;
        } else {
            ++ask_count;
        }
    }

    EXPECT_EQ(bid_count, 2u);
    EXPECT_EQ(ask_count, 0u);
    EXPECT_EQ(total_bid, capital);
}

TEST(LiquidityEdgeTest, ZeroBidPoolStillGeneratesAsks) {
    LiquidityConfig cfg;
    cfg.num_tiers = 2;
    cfg.tier_spacing_bps = {50.0, 100.0};
    cfg.tier_size_pct = {0.40, 0.60};
    cfg.gap_aware_spacing = false;
    cfg.adverse_selection_sizing = false;
    cfg.fill_rate_sizing = false;
    LiquidityEngine engine("T/P", cfg);

    const Mojo inventory = 10'000'000'000'000LL;
    auto ladder = engine.compute_ladder(
        1'000'000'000'000LL, 0.03, 0.5,
        0, inventory,
        {}, cfg);

    std::size_t bid_count = 0;
    std::size_t ask_count = 0;
    Mojo total_ask = 0;
    for (const auto& quote : ladder) {
        if (quote.side == Side::Bid) {
            ++bid_count;
        } else {
            ++ask_count;
            total_ask += quote.size;
        }
    }

    EXPECT_EQ(bid_count, 0u);
    EXPECT_EQ(ask_count, 2u);
    EXPECT_EQ(total_ask, inventory);
}

TEST(LiquidityEdgeTest, BothSidesUseSmallerSymmetricPool) {
    LiquidityConfig cfg;
    cfg.num_tiers = 2;
    cfg.tier_spacing_bps = {50.0, 100.0};
    cfg.tier_size_pct = {0.40, 0.60};
    cfg.gap_aware_spacing = false;
    cfg.adverse_selection_sizing = false;
    cfg.fill_rate_sizing = false;
    LiquidityEngine engine("T/P", cfg);

    const Mojo capital = 10'000'000'000'000LL;
    const Mojo inventory = 4'000'000'000'000LL;
    auto ladder = engine.compute_ladder(
        1'000'000'000'000LL, 0.03, 0.5,
        capital, inventory,
        {}, cfg);

    Mojo total_bid = 0;
    Mojo total_ask = 0;
    for (const auto& quote : ladder) {
        if (quote.side == Side::Bid) {
            total_bid += quote.size;
        } else {
            total_ask += quote.size;
        }
    }

    EXPECT_EQ(total_bid, inventory);
    EXPECT_EQ(total_ask, inventory);
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

// ============================================================================
// floor_recovery_ask_price -- finding 1 of the 2026-08-01 adversarial review.
//
// Step 8's quote-recovery repricing (best_ask * (1 - undercut)) runs AFTER
// every Step 7 guard and used to carry no floor; these tests pin the sigma
// width floor that now bounds it.  Prices are pseudo-price mojos.
// ============================================================================

TEST(QuoteRecoveryFloorTest, UndercutAboveFloorPassesThroughUnchanged) {
    // best_ask 1.10, undercut 5 bps -> 1.09945.  Floor: mid 1.00 + 50 bps
    // = 1.005, far below.  The plain undercut survives.
    const Mojo best_ask = 1'100'000'000'000LL;
    const Mojo mid      = 1'000'000'000'000LL;
    const auto r = floor_recovery_ask_price(best_ask, 5.0, mid, 50.0);

    ASSERT_TRUE(r.apply);
    EXPECT_FALSE(r.floored);
    EXPECT_EQ(r.price, static_cast<Mojo>(std::llround(1.1e12 * (1.0 - 5.0 / 10'000.0))));
    EXPECT_LT(r.price, best_ask);
}

TEST(QuoteRecoveryFloorTest, UndercutBelowFloorIsLiftedToTheFloor) {
    // The reviewer's failure mode in miniature: a mispriced-low third-party
    // ask (1.01, ~1.0% above mid) with a 427 bps sigma floor.  The undercut
    // wants 1.00949...; the floor demands mid * 1.0427 = 1.0427.  1.0427 >
    // best_ask, so recovery must SKIP, not price below the floor.
    const Mojo best_ask = 1'010'000'000'000LL;
    const Mojo mid      = 1'000'000'000'000LL;
    const auto r = floor_recovery_ask_price(best_ask, 5.0, mid, 427.0);
    EXPECT_FALSE(r.apply);

    // With a floor that binds but still fits under the best ask (98 bps:
    // 1.0098, above the 5 bps undercut at 1.009495 and below the 1.01 ask),
    // the price is lifted TO the floor rather than skipped.
    const auto r2 = floor_recovery_ask_price(best_ask, 5.0, mid, 98.0);
    ASSERT_TRUE(r2.apply);
    EXPECT_TRUE(r2.floored);
    EXPECT_EQ(r2.price,
              static_cast<Mojo>(std::llround(1.0e12 * (1.0 + 98.0 / 10'000.0))));
    EXPECT_LE(r2.price, best_ask);
}

TEST(QuoteRecoveryFloorTest, FloorExactlyAtBestAskStillApplies) {
    // floor == best_ask: cannot undercut, but repricing TO the best ask does
    // not violate the floor.  Only floor > best_ask skips.
    const Mojo mid      = 1'000'000'000'000LL;
    const Mojo best_ask = static_cast<Mojo>(
        std::llround(1.0e12 * (1.0 + 100.0 / 10'000.0)));
    const auto r = floor_recovery_ask_price(best_ask, 5.0, mid, 100.0);
    ASSERT_TRUE(r.apply);
    EXPECT_TRUE(r.floored);
    EXPECT_EQ(r.price, best_ask);
}

TEST(QuoteRecoveryFloorTest, MissingInputsSkipRatherThanRunUnfloored) {
    // No book reference.
    EXPECT_FALSE(floor_recovery_ask_price(0, 5.0, 1'000'000'000'000LL, 50.0).apply);
    EXPECT_FALSE(floor_recovery_ask_price(-1, 5.0, 1'000'000'000'000LL, 50.0).apply);
    // No Step 7 floor available: the old behaviour (unfloored undercut) must
    // NOT be the fallback.
    EXPECT_FALSE(floor_recovery_ask_price(1'100'000'000'000LL, 5.0, 0, 50.0).apply);
    EXPECT_FALSE(floor_recovery_ask_price(1'100'000'000'000LL, 5.0, -5, 50.0).apply);
}

TEST(QuoteRecoveryFloorTest, ZeroHalfSpreadFloorsAtTheMidItself) {
    // min_half_spread 0 (all config floors zero) degrades to "never below
    // mid", not "no floor".  Negative bps are treated as zero.
    const Mojo mid      = 1'000'000'000'000LL;
    const Mojo best_ask = 1'000'500'000'000LL;   // 5 bps above mid
    // Undercut of 20 bps would land 1.0004999e12 * ... below mid; floor lifts
    // to exactly mid.
    const auto r = floor_recovery_ask_price(best_ask, 20.0, mid, 0.0);
    ASSERT_TRUE(r.apply);
    EXPECT_TRUE(r.floored);
    EXPECT_EQ(r.price, mid);

    const auto rn = floor_recovery_ask_price(best_ask, 20.0, mid, -10.0);
    ASSERT_TRUE(rn.apply);
    EXPECT_EQ(rn.price, mid);
}

// ============================================================================
// [2026-08-01 dark-pair fix] width_floor_exempts_competitiveness
// ============================================================================
//
// Observed live: a pair with a tight (~8 bps) but ~90% stale book gets an
// uncertainty width floor of ~150 bps, Step 7 correctly forces every tier
// ~150 bps from centre, and the Step 8 competitiveness guard then suppresses
// all 12 tiers against the 8 bps BBO -- the pair posts nothing, every
// heartbeat.  The exemption: a tier whose distance from centre is within
// floor + its ladder shape offset (assigned spacing minus the side's
// innermost assigned spacing) is AT its mandated width and must survive the
// uncompetitiveness suppression.  Everything else behaves exactly as today.
//
// Numbers used throughout: centre = 100e12 mojos, raw spacing schedule
// [30, 60, 90] bps.  With a 150 bps floor, Step 7 shifts the schedule by
// delta = 150 - 30 = 120 to [150, 180, 210]; without a binding floor the
// schedule stays put.

TEST(WidthFloorCompetitivenessTest, TierAtMandatedWidthSurvives) {
    const Mojo centre = 100'000'000'000'000LL;
    const double floor_bps = 150.0;   // sigma-driven, tight-but-stale book
    const double innermost = 150.0;   // shifted schedule front

    // Ask tier 0 at exactly the floor: dist 150 <= 150 + 0.
    const Mojo ask0 = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 + 150.0 / 10'000.0)));
    EXPECT_TRUE(width_floor_exempts_competitiveness(
        ask0, 150.0, innermost, centre, floor_bps));

    // Ask tier 2 at floor + shape offset: dist 210 <= 150 + (210 - 150).
    const Mojo ask2 = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 + 210.0 / 10'000.0)));
    EXPECT_TRUE(width_floor_exempts_competitiveness(
        ask2, 210.0, innermost, centre, floor_bps));

    // Bid side is symmetric: dist 150 below centre.
    const Mojo bid0 = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 - 150.0 / 10'000.0)));
    EXPECT_TRUE(width_floor_exempts_competitiveness(
        bid0, 150.0, innermost, centre, floor_bps));

    // Ladder-build rounding (ceil on asks) must not defeat the exemption:
    // one mojo past the exact edge is ~1e-8 bps, inside the epsilon.
    EXPECT_TRUE(width_floor_exempts_competitiveness(
        ask0 + 1, 150.0, innermost, centre, floor_bps));
}

TEST(WidthFloorCompetitivenessTest, TierWellBeyondFloorPlusSpacingSuppressed) {
    const Mojo centre = 100'000'000'000'000LL;
    const double floor_bps = 150.0;
    const double innermost = 150.0;

    // A tier repriced out to 400 bps while its assigned spacing is 210:
    // bound = 150 + (210 - 150) = 210 < 400 -> still suppressible.
    const Mojo far_ask = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 + 400.0 / 10'000.0)));
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        far_ask, 210.0, innermost, centre, floor_bps));

    // Same distance on the bid side.
    const Mojo far_bid = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 - 400.0 / 10'000.0)));
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        far_bid, 210.0, innermost, centre, floor_bps));

    // Even one full bps beyond the bound is out (epsilon is 0.01 bps, for
    // mojo rounding only -- not a soft margin).
    const Mojo just_out = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 + 211.0 / 10'000.0)));
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        just_out, 210.0, innermost, centre, floor_bps));
}

TEST(WidthFloorCompetitivenessTest, HealthyPairFloorBelowSpacingNeverExempt) {
    // The healthy-book case: floor (30 bps) below the innermost configured
    // spacing (100 bps), so Step 7 never shifts the schedule and tiers sit
    // at their configured [100, 200] positions.  The exemption must never
    // fire -- the guard behaves byte-identically to before the fix.
    const Mojo centre = 100'000'000'000'000LL;
    const double floor_bps = 30.0;
    const double innermost = 100.0;

    const Mojo ask0 = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 + 100.0 / 10'000.0)));
    // dist 100 vs bound 30 + (100 - 100) = 30 -> not exempt.
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        ask0, 100.0, innermost, centre, floor_bps));

    const Mojo ask1 = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 + 200.0 / 10'000.0)));
    // dist 200 vs bound 30 + (200 - 100) = 130 -> not exempt.
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        ask1, 200.0, innermost, centre, floor_bps));

    const Mojo bid0 = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 - 100.0 / 10'000.0)));
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        bid0, 100.0, innermost, centre, floor_bps));

    const Mojo bid1 = static_cast<Mojo>(std::llround(
        static_cast<double>(centre) * (1.0 - 200.0 / 10'000.0)));
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        bid1, 200.0, innermost, centre, floor_bps));
}

TEST(WidthFloorCompetitivenessTest, DegenerateInputsNeverExempt) {
    const Mojo centre = 100'000'000'000'000LL;
    const Mojo price  = 101'500'000'000'000LL;

    // No centre / no price / no floor -> never exempt.
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        price, 150.0, 150.0, 0, 150.0));
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        0, 150.0, 150.0, centre, 150.0));
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        price, 150.0, 150.0, centre, 0.0));
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        price, 150.0, 150.0, centre, -25.0));

    // NaN floor or spacing fails closed (not exempt), never throws.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        price, 150.0, 150.0, centre, nan));
    EXPECT_FALSE(width_floor_exempts_competitiveness(
        price, nan, 150.0, centre, 150.0));
}

// ============================================================================
// [SIDEQUALITY 2026-09-01] Per-side book quality and the bid cap.
//
// The live XCH/BYC book of 2026-09-01, in the pair's own orientation
// (BYC per XCH), against the Step-7 fair-value centre:
//
//     model mid      1.41141912   (solved from CoinGecko XCH/USD and BYC par)
//     best comp bid  1.50000000   honest side, 6.3% above the centre
//     best comp ask  4.99950000   junk side, 3.5x the centre
//     bbo midpoint   3.24975000   half honest, half junk
//
// bid_cap == bbo_ref therefore did not bind, and the anchor parked a bid at
// best_comp_bid + tick against a fair value 6.3% below it -- an overpay on
// every XCH bought, every cycle.
// ============================================================================

namespace {

// Production uses 8000 bps (config.yaml), not the 500 default, and the gate
// matters here: |1.50 - 1.41141912| / 1.41141912 = 628 bps, which the 500
// default would reject outright and mask the behaviour under test.
LiquidityConfig make_sidequality_config() {
    auto cfg = make_anchor_config(3);
    cfg.competitive_anchor_max_distance_bps = 8000.0;
    return cfg;
}

constexpr Mojo kSqMid     = 1'411'419'120LL * 1000LL;  // 1.41141912
constexpr Mojo kSqCompBid = 1'500'000'000'000LL;       // 1.50
constexpr Mojo kSqCompAsk = 4'999'500'000'000LL;       // 4.9995

std::vector<CompetingOffer> sidequality_offers() {
    std::vector<CompetingOffer> offers;
    offers.push_back(make_offer(Side::Bid, kSqCompBid, 1'000'000'000'000LL));
    offers.push_back(make_offer(Side::Ask, kSqCompAsk, 1'000'000'000'000LL));
    return offers;
}

}  // namespace

TEST(BookSideQualityLadder, UnexaminedBookSelfCrossesAndLosesTheWholeLadder) {
    // THE BUG, reproduced. With both sides unexamined (the flags' defaults,
    // i.e. exactly the pre-change code path) bbo_ref is the poisoned 3.24975
    // midpoint, so bid_cap does not bind and all three bids anchor at
    // best_comp_bid + tick = 1.500141141912 -- above the ask tiers, which
    // sit around the model mid of 1.41141912. The post-adjustment cross
    // check then deletes EVERYTHING:
    //
    //   [Liquidity] XCH/BYC competitive anchor: anchored 3 bids
    //       (comp_best=1500000000000) 0 asks bbo_ref=3249750000000
    //       bid_cap=3249750000000
    //   [Liquidity] XCH/BYC post-adjustment cross: max_bid=1500141141912
    //       >= min_ask=1418476215600 -- dropped 6/6 tiers
    //
    // which is the 2026-08-30 production log line, verbatim in shape. The
    // outcome is not "a slightly expensive bid" -- it is NO QUOTES AT ALL,
    // on a pair whose fair value the engine had already solved correctly.
    auto cfg = make_sidequality_config();
    LiquidityEngine engine("XCH/BYC", cfg);

    auto ladder = engine.compute_ladder(
        kSqMid, 0.03, 0.5,
        100'000'000'000'000LL, 100'000'000'000'000LL,
        sidequality_offers(), cfg);

    EXPECT_TRUE(ladder.empty())
        << "the legacy path is expected to self-cross and drop every tier; "
           "if this now survives, the healthy-book cap has been changed and "
           "that decision needs its own review";
}

TEST(BookSideQualityLadder, DisqualifiedAskSideStopsTheOverpay) {
    // The fix. Once the ask side is disqualified it may no longer help build
    // bbo_ref, which falls back to the model mid; bid_cap then tightens to
    // min(bbo_ref, mid) and no bid may be priced above fair value.
    auto cfg = make_sidequality_config();
    cfg.book_ask_side_anchor_ok = false;
    LiquidityEngine engine("XCH/BYC", cfg);

    auto ladder = engine.compute_ladder(
        kSqMid, 0.03, 0.5,
        100'000'000'000'000LL, 100'000'000'000'000LL,
        sidequality_offers(), cfg);

    ASSERT_FALSE(ladder.empty());
    for (const auto& tq : ladder) {
        if (tq.side != Side::Bid) continue;
        EXPECT_LE(tq.price, kSqMid)
            << "tier " << tq.tier_index << " bids " << tq.price
            << " above the model mid " << kSqMid
            << " -- this is the ~6.3% overpay the cap exists to stop";
    }
}

TEST(BookSideQualityLadder, DisqualifiedAskSideLeavesTheAskLadderIntact) {
    // Disqualifying a side must not empty the ladder. The ask tiers keep
    // their raw prices around the model mid rather than being anchored to
    // the junk 4.9995 stack, which is the whole point: quote correctly
    // rather than not at all.
    auto cfg = make_sidequality_config();
    cfg.book_ask_side_anchor_ok = false;
    LiquidityEngine engine("XCH/BYC", cfg);

    auto ladder = engine.compute_ladder(
        kSqMid, 0.03, 0.5,
        100'000'000'000'000LL, 100'000'000'000'000LL,
        sidequality_offers(), cfg);

    const bool has_ask = std::any_of(
        ladder.begin(), ladder.end(),
        [](const TierQuote& tq) { return tq.side == Side::Ask; });
    EXPECT_TRUE(has_ask) << "the ask side must still be quoted";

    for (const auto& tq : ladder) {
        if (tq.side != Side::Ask) continue;
        EXPECT_LT(tq.price, kSqCompAsk)
            << "no ask tier should be dragged out to the junk stack";
    }
}

namespace {

// ---------------------------------------------------------------------------
// Healthy-book fixture.
//
// Deliberately NOT symmetric about the mid, and that asymmetry is the whole
// point.  The first version of the guard below used comp_bid 1.99 / comp_ask
// 2.01 against mid 2.00, where the BBO midpoint IS the model mid.  On that
// book the expression under test -- bbo_ref falling back to mid_f, and
// bid_cap tightening to min(bbo_ref, mid) -- is a no-op, so the flags cannot
// move the ladder and the guard compared the new code against itself.
//
// Here the book leans hard to the buy side:
//
//     model mid       2.00
//     best comp bid   2.02        ABOVE the mid
//     best comp ask   2.10
//     bbo midpoint    2.06        300 bps above the mid
//     bid anchor      2.0202      comp_bid + 1 tick, BETWEEN the two
//     stride           65 bps     0.013 per tier at this mid
//
// so the bid cap is what decides the bid ladder:
//
//   both flags true    bid_cap = bbo_ref = 2.06.  Bid tiers 0 and 1
//                      (2.0202 and 2.0072) clear it and anchor above the mid.
//   either flag false  bbo_ref falls back to 2.00 and bid_cap tightens to
//                      min(2.00, 2.00) = 2.00.  Those two tiers fail the cap
//                      and keep their raw prices; only tier 2 (1.9942) fits.
//
// HealthyBookFixtureIsSensitiveToTheFlags asserts that second bullet.  It has
// to exist: the flags DEFAULT to true, so "explicitly true" and "default" are
// the same config by construction and the equality assertion in the guard
// underneath is worth nothing unless the fixture is one where the flags are
// known to be load-bearing.
// ---------------------------------------------------------------------------
constexpr Mojo kHbMid     = 2'000'000'000'000LL;  // 2.00
constexpr Mojo kHbCompBid = 2'020'000'000'000LL;  // 2.02
constexpr Mojo kHbCompAsk = 2'100'000'000'000LL;  // 2.10

std::vector<CompetingOffer> healthy_book_offers() {
    std::vector<CompetingOffer> offers;
    offers.push_back(make_offer(Side::Bid, kHbCompBid, 1'000'000'000'000LL));
    offers.push_back(make_offer(Side::Ask, kHbCompAsk, 1'000'000'000'000LL));
    return offers;
}

std::vector<TierQuote> healthy_book_ladder(const LiquidityConfig& cfg) {
    LiquidityEngine engine("T/Q", cfg);
    return engine.compute_ladder(
        kHbMid, 0.03, 0.5, 10'000'000'000'000LL, 10'000'000'000'000LL,
        healthy_book_offers(), cfg);
}

// Everything the flags are able to move: side, tier and price, in order.
std::string ladder_shape(const std::vector<TierQuote>& ladder) {
    std::string out;
    for (const auto& tq : ladder) {
        out += to_string(tq.side);
        out += std::to_string(static_cast<int>(tq.tier_index));
        out += '@';
        out += std::to_string(tq.price);
        out += ' ';
    }
    return out;
}

}  // namespace

TEST(BookSideQualityLadder, HealthyBookFixtureIsSensitiveToTheFlags) {
    // The teeth behind HealthyTwoSidedBookIsUnaffectedByTheNewFlags.  On THIS
    // fixture the flags must be observable, otherwise the "unaffected" guard
    // is measuring nothing.  Flip either side and the bid cap drops from the
    // 2.06 BBO midpoint to the 2.00 model mid, which unanchors the tiers that
    // sit between them.
    const auto cfg = make_sidequality_config();
    const auto healthy = healthy_book_ladder(cfg);
    ASSERT_FALSE(healthy.empty());

    // The fixture only bites if the healthy path actually anchors bids above
    // the model mid -- that is the region the tightened cap removes.
    const bool anchored_above_mid = std::any_of(
        healthy.begin(), healthy.end(), [](const TierQuote& tq) {
            return tq.side == Side::Bid && tq.price > kHbMid;
        });
    ASSERT_TRUE(anchored_above_mid)
        << "degenerate fixture: the healthy path put no bid above the model "
           "mid, so tightening bid_cap to the mid cannot change anything. "
        << ladder_shape(healthy);

    auto bid_flipped = cfg;
    bid_flipped.book_bid_side_anchor_ok = false;
    auto ask_flipped = cfg;
    ask_flipped.book_ask_side_anchor_ok = false;

    EXPECT_NE(ladder_shape(healthy), ladder_shape(healthy_book_ladder(bid_flipped)))
        << "disqualifying the bid side left the ladder untouched -- the "
           "fixture is degenerate and the healthy-book guard is vacuous";
    EXPECT_NE(ladder_shape(healthy), ladder_shape(healthy_book_ladder(ask_flipped)))
        << "disqualifying the ask side left the ladder untouched -- the "
           "fixture is degenerate and the healthy-book guard is vacuous";
}

TEST(BookSideQualityLadder, HealthyTwoSidedBookIsUnaffectedByTheNewFlags) {
    // Regression guard for the pairs that are actually earning.  On a book
    // where the flags demonstrably CAN move the ladder (see
    // HealthyBookFixtureIsSensitiveToTheFlags, same fixture), leaving them at
    // their defaults must reproduce the healthy path exactly -- same tier
    // count, same sides, same prices.
    const auto cfg = make_sidequality_config();
    const auto by_default = healthy_book_ladder(cfg);

    auto explicit_cfg = cfg;
    explicit_cfg.book_bid_side_anchor_ok = true;
    explicit_cfg.book_ask_side_anchor_ok = true;
    const auto healthy = healthy_book_ladder(explicit_cfg);

    ASSERT_FALSE(by_default.empty());
    ASSERT_EQ(by_default.size(), healthy.size());
    for (std::size_t i = 0; i < by_default.size(); ++i) {
        EXPECT_EQ(by_default[i].price, healthy[i].price)
            << "tier " << i << " moved on a healthy book";
        EXPECT_EQ(by_default[i].side, healthy[i].side);
    }

    // ...and the healthy path is the pre-change path: the BBO midpoint cap
    // does not bind, so every bid tier still anchors off best_comp_bid + 1
    // tick, stepping outward by one stride.  Asserting the values rather than
    // just self-consistency is what stops this from drifting into another
    // comparison of the new code with itself.
    constexpr Mojo kTick   = kHbMid / 10'000;                    // 0.0002
    constexpr Mojo kAnchor = kHbCompBid + kTick;                 // 2.0202
    constexpr Mojo kStride = 65LL * kHbMid / 10'000;             // 65 bps
    for (const auto& tq : healthy) {
        if (tq.side != Side::Bid) continue;
        EXPECT_EQ(tq.price,
                  kAnchor - static_cast<Mojo>(tq.tier_index) * kStride)
            << "bid tier " << static_cast<int>(tq.tier_index)
            << " is no longer anchored off the competing best bid on a "
               "healthy book";
    }
}

}  // namespace
