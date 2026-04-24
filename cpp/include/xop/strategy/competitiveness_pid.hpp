// SPDX-License-Identifier: MIT
//
// CompetitivenessPid -- adaptive control of the competitiveness gate threshold
// based on observed fill rate.  Companion to the existing spread PID
// (`SpreadPidState` in engine.hpp): where the spread PID tightens or widens
// the half-spread, this controller raises or lowers the *competitiveness
// score* required for a tier to clear Step 8 of the engine pipeline.
//
// Motivation:
//   v0.7.45-46 audits showed long no-fill streaks where every posted tier
//   was already near top-of-book (competitiveness 8/10) yet still got
//   cancelled before filling.  Static thresholds cannot react to drift in
//   real DEX taker flow.  The PID watches the rolling fill rate and, when
//   below target, *lowers* the gate so more tiers post; when above target
//   (over-trading -> we are leaving spread on the table), it *raises* the
//   gate so we keep only the most aggressive tiers.
//
// The controller is intentionally **integer-output**: the engine gate is
// an integer in [0, 10], and floating-point hysteresis around an integer
// boundary would cause flapping.  The PID accumulates internally in
// double precision and rounds to the nearest integer on read.
//
// Unit-tested in cpp/tests/test_competitiveness_pid.cpp.

#pragma once

#include <algorithm>
#include <cstdint>

namespace xop::strategy {

/// Configuration for the competitiveness-threshold PID controller.
/// Values are typically supplied from xop::StrategyConfig.
struct CompetitivenessPidConfig {
    bool   enabled{true};

    /// Target fill rate as a fraction of blocks where at least one fill
    /// is observed (binary signal per block).  0.05 = 1 fill per ~20
    /// blocks (~17 minutes at 52s blocks).
    double target_fill_rate{0.05};

    /// PID gains.  Keep small: each 1.0 unit of output moves the gate by
    /// one integer step.
    double kp{8.0};
    double ki{0.5};
    double kd{2.0};

    /// EMA smoothing alpha for the fill-rate signal (lower = smoother,
    /// longer effective window).  0.02 ~= 50-block window.
    double ema_alpha{0.02};

    /// Anti-windup clamp on the integral accumulator.
    double integral_max{4.0};

    /// Number of blocks before the controller leaves warm-up and begins
    /// emitting non-zero offsets.  During warm-up the EMA still updates.
    std::uint32_t warmup_blocks{50};

    /// Output bounds (integer offset added to the base gate threshold).
    /// Negative = more aggressive (lower gate, post more tiers).
    /// Positive = more conservative (higher gate, suppress weaker tiers).
    int min_offset{-3};
    int max_offset{+3};
};

/// Per-pair PID state + tick-and-read API.
///
/// Usage from the engine:
///
///   // once per heartbeat block, before the competitiveness gate runs:
///   pid.observe_block(had_fill_this_block);
///   const int offset = pid.current_offset();
///   const int eff_threshold = std::clamp(base_threshold + offset, 0, 10);
///
/// `had_fill_this_block` is the binary "did at least one of our offers
/// fill in this block?" signal, identical to what feeds SpreadPidState.
class CompetitivenessPid {
public:
    explicit CompetitivenessPid(CompetitivenessPidConfig cfg = {})
        : cfg_{cfg} {}

    /// Update the EMA + PID state with one block's fill signal.
    /// Must be called exactly once per heartbeat for the owning pair.
    void observe_block(bool had_fill) noexcept
    {
        ++blocks_active_;

        const double fill_signal = had_fill ? 1.0 : 0.0;
        ema_fill_rate_ = cfg_.ema_alpha * fill_signal
                       + (1.0 - cfg_.ema_alpha) * ema_fill_rate_;

        if (blocks_active_ <= cfg_.warmup_blocks || !cfg_.enabled) {
            // During warm-up, the EMA is allowed to settle but we do not
            // adjust the offset.  Leaves the engine running on its
            // configured base threshold.
            return;
        }

        // Error sign convention:
        //   ema below target  -> error > 0 -> we want to LOWER the gate
        //                        (more aggressive)
        //   ema above target  -> error < 0 -> we want to RAISE the gate
        //                        (more conservative)
        // The integer offset is therefore the *negation* of the PID sum.
        const double error = cfg_.target_fill_rate - ema_fill_rate_;

        const double p_term = cfg_.kp * error;
        const double i_term = cfg_.ki * integral_error_;
        const double d_term = cfg_.kd * (error - prev_error_);

        const double output = p_term + i_term + d_term;

        // Negate so positive PID-sum (underfilling) yields a negative
        // offset (lower gate).
        const double raw_offset_double = -output;

        // Anti-windup: clamp accumulator BEFORE update would push the
        // offset past the bounds for sustained periods.
        integral_error_ = std::clamp(
            integral_error_ + error,
            -cfg_.integral_max,
            cfg_.integral_max);

        prev_error_ = error;

        // Round-to-nearest-int and clamp to configured offset bounds.
        const int rounded = static_cast<int>(
            raw_offset_double + (raw_offset_double >= 0.0 ? 0.5 : -0.5));
        current_offset_ = std::clamp(rounded, cfg_.min_offset, cfg_.max_offset);
    }

    /// Most recent integer offset to apply to the base competitiveness
    /// threshold.  Defaults to 0 during warm-up or when disabled.
    [[nodiscard]] int current_offset() const noexcept { return current_offset_; }

    /// Most recent EMA fill-rate (for telemetry/UI).
    [[nodiscard]] double ema_fill_rate() const noexcept { return ema_fill_rate_; }

    /// Number of observe_block() calls received.
    [[nodiscard]] std::uint32_t blocks_active() const noexcept { return blocks_active_; }

    /// Whether the controller is past its warm-up window.
    [[nodiscard]] bool is_warm() const noexcept
    {
        return cfg_.enabled && blocks_active_ > cfg_.warmup_blocks;
    }

    /// Read-only accessor for the active configuration.
    [[nodiscard]] const CompetitivenessPidConfig& config() const noexcept { return cfg_; }

    /// Swap in a new configuration.  Resets the warm-up counter so the
    /// new gains are not applied to a stale EMA.
    void reconfigure(CompetitivenessPidConfig new_cfg) noexcept
    {
        cfg_              = new_cfg;
        blocks_active_    = 0;
        integral_error_   = 0.0;
        prev_error_       = 0.0;
        current_offset_   = 0;
        // ema_fill_rate_ is intentionally retained -- the observed flow
        // is a property of the market, not of our control gains.
    }

private:
    CompetitivenessPidConfig cfg_;
    double                   ema_fill_rate_{0.0};
    double                   integral_error_{0.0};
    double                   prev_error_{0.0};
    int                      current_offset_{0};
    std::uint32_t            blocks_active_{0};
};

}  // namespace xop::strategy
