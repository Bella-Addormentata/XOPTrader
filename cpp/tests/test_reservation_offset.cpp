// test_reservation_offset.cpp -- Unit tests for the Avellaneda-Stoikov
// reservation offset applied to the Step 7 ladder centre ([AS-RES]).
//
// The acceptance arithmetic is written out in full so every number can be
// checked by hand:
//
//   tau   = 1140 s / 31,536,000 s  = 3.61492e-5 yr   (19-min heartbeat)
//   sigma = 1.11 annualized -> sigma^2 * tau
//         = 1.2321 * 3.61492e-5   = 4.45414e-5       (variance over 19 min)
//   q     = (116 - 80) / 80       = 0.45             (measured 2026-07-31)
//   raw   = 0.45 * 1000 * 4.45414e-5 = 2.00436e-2    (200.4 bps)
//   rate  = capped at 100 bps -> 0.01
//   centre' = centre * (1 - 0.01): bids AND asks shift DOWN 1% -- the bot
//   leans to sell its excess base, the direction it was observed failing
//   to move.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/config.hpp>
#include <xop/strategy/reservation_offset.hpp>

#include <cmath>
#include <limits>

namespace {

using xop::strategy::normalized_imbalance;
using xop::strategy::reservation_offset;

// Shared realistic constants (measured 2026-07-31 state).
constexpr double kTauYears  = 1140.0 / 31'536'000.0;  // 19-min heartbeat
constexpr double kSigmaHigh = 1.11;                    // measured annualized
constexpr double kGamma     = 1000.0;                  // config default
constexpr double kCapBps    = 100.0;                   // config default

// ============================================================================
// Acceptance (A): balanced inventory -> zero shift, ladder identical
// ============================================================================

TEST(ReservationOffsetTest, BalancedInventoryZeroShift) {
    // q ~ 0 at the target for every per-pair target in the live config.
    for (const double target : {0.5, 0.5882352941176471,
                                0.2222222222222222, 0.9090909090909091}) {
        const auto ro = reservation_offset(
            /*inv_ratio=*/target, /*ratio_target=*/target,
            kGamma, kSigmaHigh, kTauYears, kCapBps);
        EXPECT_DOUBLE_EQ(ro.q, 0.0) << "target=" << target;
        EXPECT_DOUBLE_EQ(ro.rate, 0.0) << "target=" << target;
        EXPECT_FALSE(ro.capped);
        // centre' = centre * (1 - 0) == centre: ladders identical.
        const double centre = 12.19;
        EXPECT_DOUBLE_EQ(centre * (1.0 - ro.rate), centre);
    }
}

// ============================================================================
// Acceptance (B): measured 2026-07-31 state -- capped selling lean
// ============================================================================

TEST(ReservationOffsetTest, MeasuredLongStateCappedSellingLean) {
    // ~116 XCH held vs ~80 target: q = (116-80)/80 = 0.45.  With a 0.5
    // ratio target that is inv_ratio = 0.725: (0.725-0.5)/0.5 = 0.45.
    const double inv_ratio = 0.725;
    const double target    = 0.50;
    EXPECT_NEAR(normalized_imbalance(inv_ratio, target), 0.45, 1e-12);

    const auto ro = reservation_offset(
        inv_ratio, target, kGamma, kSigmaHigh, kTauYears, kCapBps);

    // raw = 0.45 * 1000 * 1.11^2 * (1140/31536000)
    //     = 0.45 * 1000 * 1.2321 * 3.61492e-5 = 2.00436e-2 (200.4 bps).
    const double expected_raw =
        0.45 * kGamma * kSigmaHigh * kSigmaHigh * kTauYears;
    EXPECT_NEAR(ro.raw_rate, expected_raw, 1e-9);
    EXPECT_NEAR(ro.raw_rate, 2.00436e-2, 5e-6);
    EXPECT_GT(ro.raw_rate, kCapBps / 10'000.0);  // 200.4 bps > 100 bps rail

    // Capped at the 100 bps rail.
    EXPECT_TRUE(ro.capped);
    EXPECT_DOUBLE_EQ(ro.rate, 0.01);

    // Direction: q > 0 (long base) -> centre shifts DOWN, so bids shift
    // down AND asks shift down -- selling pressure that reduces inventory.
    const double centre      = 12.19;               // e.g. XCH/wUSDC.b mid
    const double half_spread = centre * 0.005;      // any half-spread
    const double centre_s    = centre * (1.0 - ro.rate);
    EXPECT_LT(centre_s, centre);
    EXPECT_NEAR(centre_s, 12.0681, 1e-4);           // 12.19 * 0.99
    EXPECT_LT(centre_s - half_spread, centre - half_spread);  // bid down
    EXPECT_LT(centre_s + half_spread, centre + half_spread);  // ask down
}

// ============================================================================
// Acceptance (C): sigma-floor pair (no history) -> offset ~ 0, no cliff
// ============================================================================

TEST(ReservationOffsetTest, SigmaFloorDegradesToNoShift) {
    // Cold pair: sigma = the 0.001 floor.  Same strong imbalance as (B).
    const auto ro = reservation_offset(
        0.725, 0.50, kGamma, /*sigma=*/0.001, kTauYears, kCapBps);

    // raw = 0.45 * 1000 * 1e-6 * 3.61492e-5 = 1.627e-8 (0.00016 bps).
    EXPECT_NEAR(ro.raw_rate, 1.627e-8, 1e-10);
    EXPECT_FALSE(ro.capped);
    // Below the engine's 1e-6 (0.01 bps) apply-threshold: today's
    // behaviour, no cliff.
    EXPECT_LT(std::abs(ro.rate), 1e-6);
}

// ============================================================================
// Short inventory leans the centre UP (buying pressure)
// ============================================================================

TEST(ReservationOffsetTest, ShortInventoryBuyingLean) {
    // inv_ratio 0.25 vs target 0.5: q = -0.5 (holding half the target).
    const auto ro = reservation_offset(
        0.25, 0.50, kGamma, kSigmaHigh, kTauYears, kCapBps);
    EXPECT_NEAR(ro.q, -0.5, 1e-12);
    // raw = -0.5 * 1000 * 4.45414e-5 = -2.227e-2 -> capped at -100 bps.
    EXPECT_TRUE(ro.capped);
    EXPECT_DOUBLE_EQ(ro.rate, -0.01);
    const double centre = 1.0;
    EXPECT_GT(centre * (1.0 - ro.rate), centre);  // centre shifts UP
}

// ============================================================================
// Moderate volatility: smooth, uncapped scaling
// ============================================================================

TEST(ReservationOffsetTest, ModerateVolUncapped) {
    // sigma = 0.5: raw = 0.45 * 1000 * 0.25 * 3.61492e-5 = 4.067e-3
    // (40.7 bps) -- inside the rail, applied as-is.
    const auto ro = reservation_offset(
        0.725, 0.50, kGamma, 0.5, kTauYears, kCapBps);
    EXPECT_FALSE(ro.capped);
    EXPECT_NEAR(ro.rate * 10'000.0, 40.67, 0.05);
}

// ============================================================================
// Guards: degenerate inputs yield exactly zero offset
// ============================================================================

TEST(ReservationOffsetTest, DegenerateInputsYieldZero) {
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // Disabled-by-zero knobs.
    EXPECT_DOUBLE_EQ(reservation_offset(0.9, 0.5, 0.0, 1.0, kTauYears, 100).rate, 0.0);
    EXPECT_DOUBLE_EQ(reservation_offset(0.9, 0.5, kGamma, 0.0, kTauYears, 100).rate, 0.0);
    EXPECT_DOUBLE_EQ(reservation_offset(0.9, 0.5, kGamma, 1.0, 0.0, 100).rate, 0.0);
    EXPECT_DOUBLE_EQ(reservation_offset(0.9, 0.5, kGamma, 1.0, kTauYears, 0.0).rate, 0.0);

    // NaN poisoning is absorbed, never propagated.
    EXPECT_DOUBLE_EQ(reservation_offset(nan, 0.5, kGamma, 1.0, kTauYears, 100).rate, 0.0);
    EXPECT_DOUBLE_EQ(reservation_offset(0.9, 0.5, kGamma, nan, kTauYears, 100).rate, 0.0);

    // Degenerate targets: no target, or a target that admits no imbalance.
    EXPECT_DOUBLE_EQ(normalized_imbalance(0.9, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(normalized_imbalance(0.9, 1.0), 0.0);
}

// ============================================================================
// Rails: |q| capped at 4, inv_ratio clamped to [0, 1]
// ============================================================================

TEST(ReservationOffsetTest, ImbalanceRails) {
    // Tiny target: fully long would be q = (1 - 0.1)/0.1 = 9 -> railed to 4.
    EXPECT_DOUBLE_EQ(normalized_imbalance(1.0, 0.1), 4.0);
    // Fully short of any target is exactly -1, never below.
    EXPECT_DOUBLE_EQ(normalized_imbalance(0.0, 0.5), -1.0);
    // Out-of-range ratios clamp instead of extrapolating.
    EXPECT_DOUBLE_EQ(normalized_imbalance(1.7, 0.5),
                     normalized_imbalance(1.0, 0.5));
    EXPECT_DOUBLE_EQ(normalized_imbalance(-0.3, 0.5),
                     normalized_imbalance(0.0, 0.5));
}

// ============================================================================
// Config defaults: the mechanism ships ON with the approved rail
// ============================================================================

TEST(ReservationOffsetTest, ConfigDefaults) {
    const xop::StrategyConfig d{};
    EXPECT_TRUE(d.as_reservation_enabled);
    EXPECT_DOUBLE_EQ(d.as_reservation_gamma, 1000.0);
    EXPECT_DOUBLE_EQ(d.as_reservation_max_offset_bps, 100.0);
}

}  // namespace
