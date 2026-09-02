// ---------------------------------------------------------------------------
// [S39] What the PID gains can actually produce, and what they cannot.
//
// Measured on the live rotation 2026-08-30T23:50 -> 2026-09-01T15:37 (~39.8h,
// XCH/DBX effectively the only pair quoting): the spread PID reported
// mult=0.820 in 4,935 of 5,287 ticks and NEVER went below it; the
// competitiveness PID reported offset=-3 in 5,147 of 5,287. config.yaml:216
// sets pid_min_mult: 0.7, which the gains cannot reach -- it is dead config
// that reads as a tuned safety bound.
//
// THE ERROR THESE TESTS EXIST TO PREVENT
// --------------------------------------
// The first revision of TODO.md S39 computed the authority as
//     kp*target + ki*integral_max + kd*ema_alpha
// and reported a floor of 0.816. That is a LOOSE bound, not a reachable value:
// earning the derivative term costs more in the proportional term than it
// returns, so the true floor is 0.820 and the logs agree -- 0.816-0.819 never
// appear in 5,287 ticks. Two of four research agents independently made the
// same mistake. DerivativeCannotExtendTheTighteningBound is here to make it
// impossible to reintroduce quietly.
//
// The simulation tests deliberately re-implement the controller's literal
// recursion instead of calling the closed form, so a wrong closed form cannot
// validate itself.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "xop/strategy/pid_reachability.hpp"

using xop::strategy::PidGains;
using xop::strategy::PidReachabilityFinding;
using xop::strategy::comp_pid_min_offset_is_unreachable;
using xop::strategy::comp_pid_reachable_max_offset;
using xop::strategy::comp_pid_reachable_min_offset;
using xop::strategy::effective_comp_pid_min_offset;
using xop::strategy::effective_pid_min_mult;
using xop::strategy::pid_output_bounds;
using xop::strategy::pid_round_half_away_from_zero;
using xop::strategy::pid_steady_state_bounds;
using xop::strategy::pid_transient_bounds;
using xop::strategy::spread_pid_min_mult_is_unreachable;
using xop::strategy::spread_pid_reachable_max_mult;
using xop::strategy::spread_pid_reachable_min_mult;

namespace {

/// config.yaml:210-218 as shipped on 2026-09-01. pid_integral_max is ABSENT
/// from config.yaml, so the config.hpp default 2.0 binds -- that omission is
/// itself part of S39.
constexpr PidGains kShippedSpread{
    /*kp*/ 0.8, /*ki*/ 0.05, /*kd*/ 0.2, /*target*/ 0.10,
    /*ema_alpha*/ 0.02, /*integral_max*/ 2.0, /*warmup_blocks*/ 50};

/// config.yaml:219-228 as shipped. Note target 0.15, three times the
/// config.hpp default of 0.05.
constexpr PidGains kShippedComp{
    /*kp*/ 8.0, /*ki*/ 0.5, /*kd*/ 2.0, /*target*/ 0.15,
    /*ema_alpha*/ 0.02, /*integral_max*/ 4.0, /*warmup_blocks*/ 5};

/// config.hpp:1212-1238 defaults, i.e. what binds for a deployment with no
/// pid_* keys at all -- including config.example.yaml, which has none.
constexpr PidGains kDefaultSpread{
    /*kp*/ 0.8, /*ki*/ 0.05, /*kd*/ 0.2, /*target*/ 0.10,
    /*ema_alpha*/ 0.02, /*integral_max*/ 2.0, /*warmup_blocks*/ 50};

/// config.hpp:1263-1277 defaults.
constexpr PidGains kDefaultComp{
    /*kp*/ 8.0, /*ki*/ 0.5, /*kd*/ 2.0, /*target*/ 0.05,
    /*ema_alpha*/ 0.02, /*integral_max*/ 4.0, /*warmup_blocks*/ 50};

/// The controller's LITERAL recursion, transcribed from engine.cpp:5090-5132
/// and competitiveness_pid.hpp::observe_block. Deliberately not expressed in
/// terms of anything in pid_reachability.hpp -- these tests must be able to
/// contradict the closed form.
///
/// Note the two orderings that matter and are easy to "tidy" away:
///   * i_term reads the integral from BEFORE this tick's update;
///   * prev_error is initialised to 0 and assigned only inside the
///     post-warm-up branch, so the first live tick sees a full-size step.
std::vector<double> simulate_outputs(const PidGains& g,
                                     const std::vector<int>& fills)
{
    std::vector<double> outputs;
    outputs.reserve(fills.size());

    double        ema        = 0.0;
    double        integral   = 0.0;
    double        prev_error = 0.0;
    std::uint32_t blocks     = 0;

    for (const int s : fills) {
        ++blocks;
        ema = g.ema_alpha * static_cast<double>(s) + (1.0 - g.ema_alpha) * ema;
        if (blocks <= g.warmup_blocks) {
            continue;
        }
        const double error  = g.target - ema;
        const double p_term = g.kp * error;
        const double i_term = g.ki * integral;               // BEFORE update
        const double d_term = g.kd * (error - prev_error);
        outputs.push_back(p_term + i_term + d_term);

        integral = std::clamp(integral + error, -g.integral_max, g.integral_max);
        prev_error = error;
    }
    return outputs;
}

std::vector<int> never_fill(std::size_t n) { return std::vector<int>(n, 0); }

}  // namespace

// -- The shipped floor ------------------------------------------------------

TEST(PidReachability, ShippedSpreadGainsFloorIsExactly820)
{
    const auto b = pid_steady_state_bounds(kShippedSpread);
    EXPECT_NEAR(b.max_output, 0.18, 1e-12)
        << "authority is kp*target + ki*integral_max = 0.8*0.10 + 0.05*2.0";
    EXPECT_NEAR(spread_pid_reachable_min_mult(kShippedSpread), 0.820, 1e-12)
        << "0.820, not 0.816 -- the derivative term is not free";
}

TEST(PidReachability, DerivativeCannotExtendTheTighteningBound)
{
    // THE REGRESSION GUARD. Adding kd*ema_alpha to the authority would make
    // this vary with kd. It must not: d(output)/d(ema_prev) is negative for
    // every configuration shipped here, so the maximum sits at ema = 0 where
    // the derivative term is exactly zero.
    for (const double kd : {0.0, 0.2, 2.0, 20.0}) {
        PidGains g = kShippedSpread;
        g.kd = kd;
        EXPECT_NEAR(pid_steady_state_bounds(g).max_output, 0.18, 1e-12)
            << "steady-state authority moved when kd = " << kd;
    }
}

TEST(PidReachability, TheTransientIsWhereKdActuallyShowsUp)
{
    // The counterpart to the test above, and the reason the kd invariance is
    // stated for the STEADY STATE only. On the first post-warm-up tick the
    // integrator is empty and prev_error is a fictitious zero, so the output
    // is (kp + kd) * error -- which does scale with kd, and at kd = 20 exceeds
    // the steady-state ceiling by an order of magnitude.
    PidGains g = kShippedSpread;
    g.kd = 20.0;
    EXPECT_NEAR(pid_transient_bounds(g).max_output, 2.08, 1e-12);
    EXPECT_GT(pid_output_bounds(g).max_output,
              pid_steady_state_bounds(g).max_output)
        << "the union must carry the transient, not discard it";

    // At the shipped kd the transient is comfortably inside the steady state,
    // which is why the live log's floor is 0.820 and not something else.
    EXPECT_NEAR(pid_transient_bounds(kShippedSpread).max_output, 0.10, 1e-12);
    EXPECT_LT(pid_transient_bounds(kShippedSpread).max_output,
              pid_steady_state_bounds(kShippedSpread).max_output);
}

// -- Simulation: the closed form must not be able to validate itself --------

TEST(PidReachability, MaxOutputIsAttainedOnTheNeverFillTrajectory)
{
    // Not merely bounded -- ATTAINED. The bot has been sitting on this exact
    // value for 40 hours because it is not filling.
    const auto outputs = simulate_outputs(kShippedSpread, never_fill(3000));
    ASSERT_FALSE(outputs.empty());

    double sim_max = -std::numeric_limits<double>::infinity();
    for (const double o : outputs) {
        sim_max = std::max(sim_max, o);
    }
    EXPECT_NEAR(sim_max, pid_steady_state_bounds(kShippedSpread).max_output,
                1e-12)
        << "the closed form and the literal recursion disagree";
    EXPECT_NEAR(1.0 - sim_max, 0.820, 1e-12)
        << "which is the mult=0.820 in 4,935 of 5,287 live ticks";
}

TEST(PidReachability, FirstLiveTickReproducesTheLoggedTransient)
{
    // The live log holds eight `mult=0.900` lines. They are not noise: they
    // are the first post-warm-up tick, where the integrator is still empty and
    // prev_error is a fictitious zero, giving (kp + kd) * target = 0.10.
    const auto outputs = simulate_outputs(kShippedSpread, never_fill(60));
    ASSERT_FALSE(outputs.empty());
    EXPECT_NEAR(outputs.front(), 0.10, 1e-12);
    EXPECT_NEAR(1.0 - outputs.front(), 0.900, 1e-12);
}

TEST(PidReachability, BoundsContainEveryRandomTrajectory)
{
    const auto bounds = pid_output_bounds(kShippedSpread);
    std::mt19937 rng{20260901u};

    for (int trial = 0; trial < 200; ++trial) {
        // Fill probability swept across the range so some trajectories drive
        // the EMA far above target (the widening corner) and some never fill.
        const double p = static_cast<double>(trial) / 199.0;
        const auto threshold =
            static_cast<std::uint32_t>(p * 4294967295.0);

        std::vector<int> fills(800);
        for (int& s : fills) {
            s = (rng() <= threshold) ? 1 : 0;
        }

        for (const double o : simulate_outputs(kShippedSpread, fills)) {
            ASSERT_GE(o, bounds.min_output - 1e-12)
                << "trajectory escaped the lower bound at p = " << p;
            ASSERT_LE(o, bounds.max_output + 1e-12)
                << "trajectory escaped the upper bound at p = " << p;
        }
    }
}

TEST(PidReachability, LongWarmupLetsTheTransientEscapeTheSteadyState)
{
    // At the shipped warm-up of 50 the transient is inside the envelope, so
    // dropping the union would be invisible. At 500 the EMA has time to reach
    // ~1.0, the first live tick differences against a fictitious zero, and the
    // output falls BELOW the steady-state floor. This is why the union is not
    // optional.
    PidGains shallow = kShippedSpread;
    PidGains deep    = kShippedSpread;
    deep.warmup_blocks = 500;

    EXPECT_NEAR(pid_output_bounds(shallow).min_output,
                pid_steady_state_bounds(shallow).min_output, 1e-12)
        << "at warmup 50 the transient adds nothing";
    EXPECT_LT(pid_output_bounds(deep).min_output,
              pid_steady_state_bounds(deep).min_output)
        << "at warmup 500 the transient escapes and must be carried";
}

// -- Rounding ---------------------------------------------------------------

TEST(PidReachability, RoundHalfAwayFromZeroMatchesTheController)
{
    // Mirrors competitiveness_pid.hpp exactly. std::lround would be
    // half-to-even; static_cast<int>(x + 0.5) would be wrong for every
    // negative value -- which is the only sign the live controller emits.
    EXPECT_EQ(pid_round_half_away_from_zero(-2.4999), -2);
    EXPECT_EQ(pid_round_half_away_from_zero(-2.5), -3);
    EXPECT_EQ(pid_round_half_away_from_zero(-2.50001), -3);
    EXPECT_EQ(pid_round_half_away_from_zero(-0.4999), 0);
    EXPECT_EQ(pid_round_half_away_from_zero(-0.5), -1);
    EXPECT_EQ(pid_round_half_away_from_zero(-0.0), 0);
    EXPECT_EQ(pid_round_half_away_from_zero(0.0), 0);
    EXPECT_EQ(pid_round_half_away_from_zero(2.4999), 2);
    EXPECT_EQ(pid_round_half_away_from_zero(2.5), 3);
}

// -- The competitiveness rail ----------------------------------------------

TEST(PidReachability, CompMinOffsetIsReachedByRoundingNotByTheClamp)
{
    // The distinction is the whole point: widening comp_pid_min_offset would
    // change NOTHING, because the clamp is the identity here. The floor is the
    // gain budget.
    EXPECT_NEAR(pid_steady_state_bounds(kShippedComp).max_output, 3.20, 1e-12);
    EXPECT_EQ(comp_pid_reachable_min_offset(kShippedComp), -3);

    // Configured at -10, the reachable floor is still -3.
    EXPECT_EQ(effective_comp_pid_min_offset(-10, kShippedComp), -3);
    EXPECT_EQ(effective_comp_pid_min_offset(-3, kShippedComp), -3)
        << "no-op on the shipped config";

    // And the widening direction is genuinely reachable, so +3 is a real bound.
    EXPECT_GE(comp_pid_reachable_max_offset(kShippedComp), 3);
}

TEST(PidReachability, CompDefaultTargetMakesMinOffsetUnreachable)
{
    // The latent second instance. With the config.hpp default target of 0.05
    // the authority is 2.40, so the controller can only reach -2 and the
    // configured -3 is decoration.
    EXPECT_NEAR(pid_steady_state_bounds(kDefaultComp).max_output, 2.40, 1e-12);
    EXPECT_EQ(comp_pid_reachable_min_offset(kDefaultComp), -2);

    PidReachabilityFinding f{};
    EXPECT_TRUE(comp_pid_min_offset_is_unreachable(-3, kDefaultComp, f));
    EXPECT_STREQ(f.key, "comp_pid_min_offset");
    EXPECT_DOUBLE_EQ(f.configured, -3.0);
    EXPECT_DOUBLE_EQ(f.reachable, -2.0);

    // Silent on the shipped config -- this rule must not cry wolf.
    PidReachabilityFinding g{};
    EXPECT_FALSE(comp_pid_min_offset_is_unreachable(-3, kShippedComp, g));
}

// -- The derived clamp ------------------------------------------------------

TEST(PidReachability, EffectiveMinMultIsOneDirectional)
{
    // BOTH directions asserted. A one-sided test passes with max/min swapped.
    EXPECT_NEAR(effective_pid_min_mult(0.70, kShippedSpread), 0.820, 1e-12)
        << "an unreachable floor is raised to what the gains can do";
    EXPECT_NEAR(effective_pid_min_mult(0.95, kShippedSpread), 0.95, 1e-12)
        << "a floor ABOVE the reachable one is a real bound and is honoured";
}

TEST(PidReachability, EffectiveMinMultIsANoOpOnCoherentConfig)
{
    for (const double configured : {0.82, 0.85, 0.90, 0.99}) {
        EXPECT_NEAR(effective_pid_min_mult(configured, kShippedSpread),
                    configured, 1e-12);
    }
}

TEST(PidReachability, ShippedAndDefaultConfigsAreBothUnreachable)
{
    // The shipped value is the visible bug; the DEFAULT carrying it too is
    // what makes a boot-time throw unsafe -- a bare config would not start,
    // and test_config.cpp's kMinimalValidYaml has no pid_* keys at all.
    PidReachabilityFinding shipped{};
    ASSERT_TRUE(spread_pid_min_mult_is_unreachable(0.70, kShippedSpread, shipped));
    EXPECT_STREQ(shipped.key, "pid_min_mult");
    EXPECT_NEAR(shipped.reachable, 0.820, 1e-12);
    EXPECT_NEAR(shipped.authority, 0.18, 1e-12);
    EXPECT_NEAR(shipped.reachable - shipped.configured, 0.120, 1e-12)
        << "short by 0.120, not the 0.116 the loose bound implied";

    PidReachabilityFinding dflt{};
    EXPECT_TRUE(spread_pid_min_mult_is_unreachable(0.70, kDefaultSpread, dflt))
        << "config.example.yaml has no pid_* keys and inherits this";
}

TEST(PidReachability, CoherentConfigProducesNoFinding)
{
    PidReachabilityFinding f{};
    EXPECT_FALSE(spread_pid_min_mult_is_unreachable(0.82, kShippedSpread, f));
    EXPECT_FALSE(spread_pid_min_mult_is_unreachable(0.95, kShippedSpread, f));
}

// -- Refusals ---------------------------------------------------------------

TEST(PidReachability, NonFiniteGainsReturnConfiguredUnchanged)
{
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double kInf = std::numeric_limits<double>::infinity();

    PidGains bad_kp = kShippedSpread;
    bad_kp.kp = kNaN;
    EXPECT_NEAR(effective_pid_min_mult(0.70, bad_kp), 0.70, 1e-12);
    EXPECT_EQ(effective_comp_pid_min_offset(-3, bad_kp), -3);

    PidGains bad_imax = kShippedSpread;
    bad_imax.integral_max = kInf;
    EXPECT_NEAR(effective_pid_min_mult(0.70, bad_imax), 0.70, 1e-12);

    PidReachabilityFinding f{};
    EXPECT_FALSE(spread_pid_min_mult_is_unreachable(0.70, bad_kp, f));
    EXPECT_FALSE(spread_pid_min_mult_is_unreachable(kNaN, kShippedSpread, f));
}

TEST(PidReachability, ZeroGainsDoNotProduceNonsense)
{
    // A fully disabled controller has zero authority, so its reachable floor
    // is mult = 1.0 and any configured floor below that is unreachable.
    constexpr PidGains kZero{0.0, 0.0, 0.0, 0.0, 0.02, 0.0, 50};
    EXPECT_NEAR(pid_steady_state_bounds(kZero).max_output, 0.0, 1e-12);
    EXPECT_NEAR(spread_pid_reachable_min_mult(kZero), 1.0, 1e-12);
    EXPECT_NEAR(spread_pid_reachable_max_mult(kZero), 1.0, 1e-12);
}
