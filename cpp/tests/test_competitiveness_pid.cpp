// test_competitiveness_pid.cpp -- Regression tests for the
// xop::strategy::CompetitivenessPid controller introduced in v0.7.48.
//
// The controller is integer-output, per-pair, and intended to nudge the
// engine's competitiveness gate (Step 8) up or down based on observed
// fill rate.  These tests pin the contract:
//
//   1. Warm-up: emits zero offset until blocks_active > warmup_blocks.
//   2. Underfilling drives the offset NEGATIVE (lower the gate).
//   3. Overfilling drives the offset POSITIVE (raise the gate).
//   4. Output is clamped to [min_offset, max_offset].
//   5. EMA decays toward the observed signal.
//   6. Disabling the controller pins the offset to 0.
//
// ISO/IEC 27001:2022 -- pure numerical verification, no secrets.
// ISO/IEC 5055       -- deterministic tests, no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/strategy/competitiveness_pid.hpp>

namespace {

using xop::strategy::CompetitivenessPid;
using xop::strategy::CompetitivenessPidConfig;

CompetitivenessPidConfig fast_cfg()
{
    // Aggressive gains + short warm-up so tests converge in a few hundred
    // iterations rather than thousands.
    CompetitivenessPidConfig cfg;
    cfg.enabled          = true;
    cfg.target_fill_rate = 0.10;
    cfg.kp               = 20.0;
    cfg.ki               = 1.0;
    cfg.kd               = 0.0;
    cfg.ema_alpha        = 0.10;
    cfg.integral_max     = 5.0;
    cfg.warmup_blocks    = 10;
    cfg.min_offset       = -3;
    cfg.max_offset       = +3;
    return cfg;
}

}  // namespace

// --------------------------------------------------------------------------
// Warm-up: offset stays at 0 while blocks_active <= warmup_blocks even if
// the fill rate is far from target.
// --------------------------------------------------------------------------
TEST(CompetitivenessPid, WarmupEmitsZeroOffset) {
    CompetitivenessPid pid{fast_cfg()};

    for (uint32_t i = 0; i < 10; ++i) {
        pid.observe_block(false);  // zero fills -> would normally drive negative
        EXPECT_EQ(pid.current_offset(), 0);
        EXPECT_FALSE(pid.is_warm());
    }
}

// --------------------------------------------------------------------------
// Underfilling (no fills observed) should produce a NEGATIVE offset
// (lower the gate -> more aggressive posting).
// --------------------------------------------------------------------------
TEST(CompetitivenessPid, UnderfillingDrivesOffsetNegative) {
    CompetitivenessPid pid{fast_cfg()};

    // Run well past warm-up with zero fills.
    for (int i = 0; i < 200; ++i) {
        pid.observe_block(false);
    }
    EXPECT_TRUE(pid.is_warm());
    EXPECT_LT(pid.current_offset(), 0);
    EXPECT_NEAR(pid.ema_fill_rate(), 0.0, 1e-6);
}

// --------------------------------------------------------------------------
// Overfilling (every block fills) should produce a POSITIVE offset
// (raise the gate -> more selective posting).
// --------------------------------------------------------------------------
TEST(CompetitivenessPid, OverfillingDrivesOffsetPositive) {
    CompetitivenessPid pid{fast_cfg()};

    for (int i = 0; i < 200; ++i) {
        pid.observe_block(true);
    }
    EXPECT_TRUE(pid.is_warm());
    EXPECT_GT(pid.current_offset(), 0);
    EXPECT_NEAR(pid.ema_fill_rate(), 1.0, 1e-3);
}

// --------------------------------------------------------------------------
// Output bounds: starvation must not push past min_offset; saturation
// must not push past max_offset.
// --------------------------------------------------------------------------
TEST(CompetitivenessPid, OutputClampedToConfiguredBounds) {
    auto cfg = fast_cfg();
    cfg.min_offset = -2;
    cfg.max_offset = +2;
    CompetitivenessPid pid_low{cfg};
    CompetitivenessPid pid_high{cfg};

    for (int i = 0; i < 1000; ++i) pid_low.observe_block(false);
    for (int i = 0; i < 1000; ++i) pid_high.observe_block(true);

    EXPECT_GE(pid_low.current_offset(), cfg.min_offset);
    EXPECT_LE(pid_low.current_offset(), cfg.max_offset);
    EXPECT_GE(pid_high.current_offset(), cfg.min_offset);
    EXPECT_LE(pid_high.current_offset(), cfg.max_offset);

    // Sanity: extremes should actually saturate at the bounds.
    EXPECT_EQ(pid_low.current_offset(), cfg.min_offset);
    EXPECT_EQ(pid_high.current_offset(), cfg.max_offset);
}

// --------------------------------------------------------------------------
// At target fill rate the controller should converge to ~0 offset.
// --------------------------------------------------------------------------
TEST(CompetitivenessPid, OnTargetConvergesNearZero) {
    auto cfg = fast_cfg();
    cfg.target_fill_rate = 0.10;
    CompetitivenessPid pid{cfg};

    // Deliver one fill every 10 blocks for a long horizon.
    for (int i = 0; i < 5000; ++i) {
        const bool had_fill = (i % 10 == 0);
        pid.observe_block(had_fill);
    }
    EXPECT_NEAR(pid.ema_fill_rate(), 0.10, 0.05);
    EXPECT_LE(std::abs(pid.current_offset()), 1);
}

// --------------------------------------------------------------------------
// Disabling the controller pins offset to 0 regardless of inputs.
// --------------------------------------------------------------------------
TEST(CompetitivenessPid, DisabledPinsOffsetToZero) {
    auto cfg = fast_cfg();
    cfg.enabled = false;
    CompetitivenessPid pid{cfg};

    for (int i = 0; i < 500; ++i) {
        pid.observe_block(false);
        EXPECT_EQ(pid.current_offset(), 0);
    }
}

// --------------------------------------------------------------------------
// reconfigure() resets warm-up and clears integral history but keeps the
// observed EMA (a property of the market, not the controller).
// --------------------------------------------------------------------------
TEST(CompetitivenessPid, ReconfigureResetsControllerStateButKeepsEma) {
    CompetitivenessPid pid{fast_cfg()};
    for (int i = 0; i < 200; ++i) pid.observe_block(false);
    const double ema_before = pid.ema_fill_rate();
    EXPECT_LT(pid.current_offset(), 0);

    auto cfg2 = fast_cfg();
    cfg2.warmup_blocks = 50;
    pid.reconfigure(cfg2);

    EXPECT_EQ(pid.current_offset(), 0);
    EXPECT_FALSE(pid.is_warm());
    EXPECT_DOUBLE_EQ(pid.ema_fill_rate(), ema_before);
    EXPECT_EQ(pid.blocks_active(), 0u);
}
