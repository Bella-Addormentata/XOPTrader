#ifndef XOP_STRATEGY_PID_REACHABILITY_HPP
#define XOP_STRATEGY_PID_REACHABILITY_HPP
// ---------------------------------------------------------------------------
// pid_reachability.hpp -- what a PID's gains can actually produce.
//
// [S39 2026-09-01] Both fill-rate controllers have been pinned at their most
// aggressive value for the entire retained log window (~39.8h): the spread PID
// reports mult=0.820 in 4,935 of 5,287 ticks and the competitiveness PID
// reports offset=-3 in 5,147 of 5,287. Neither number is a coincidence and
// neither is the configured clamp -- both are the GAIN BUDGET, and this header
// exists so that budget is computed in exactly one place.
//
// WHY THIS IS NOT A VALIDATION HELPER
// -----------------------------------
// config.yaml:216 sets `pid_min_mult: 0.7`. The gains cannot reach it. It is
// dead config that reads as a tuned safety bound, and the operator has no way
// to know without doing this algebra by hand. The repo's answer to that shape
// of problem is DERIVE-don't-validate -- the same move as
// bookside::effective_agree_max_spread_bps
// (cpp/include/xop/execution/book_side_quality.hpp) -- so the engine asks for
// the EFFECTIVE clamp and the knob can never disagree with the controller.
//
// THE DERIVATIVE TERM DOES NOT BUY AUTHORITY. READ THIS BEFORE "IMPROVING" IT
// ---------------------------------------------------------------------------
// The obvious closed form for the tightening bound is
//     kp*target + ki*integral_max + kd*ema_alpha
// and it is WRONG. Substituting the EMA recursion
//     ema_n = alpha*s_n + (1-alpha)*ema_{n-1},   s_n in {0,1}
// into output = kp*e_n + ki*I_{n-1} + kd*(e_n - e_{n-1}) gives
//
//     output = kp*T - alpha*s*(kp + kd) + ema_prev*C_e + ki*I
//     C_e    = kd*alpha - kp*(1 - alpha)
//
// For every configuration this repo ships C_e is NEGATIVE (shipped spread
// gains: 0.2*0.02 - 0.8*0.98 = -0.780). Earning a positive derivative term
// requires a non-zero EMA, and each unit of EMA costs kp*(1-alpha) in the
// proportional term for every kd*alpha it returns -- 195x more than it gains.
// The maximum therefore sits at ema = 0, where the derivative term is exactly
// zero:
//
//     max_output = kp*target + ki*integral_max        <- no kd term. ever.
//
// This is not a theoretical nicety. The first revision of TODO.md S39 claimed
// a reachable floor of 0.816 from the loose bound; the live logs show 0.820 as
// the minimum ever observed across 5,287 ticks and nothing in 0.816-0.819.
// test_pid_reachability.cpp pins this with a simulation that re-derives the
// bound from the literal recursion rather than copying the formula.
//
// Pure header, no engine or config types, so cpp/tests/test_pid_reachability.
// cpp drives every bound directly -- nothing in cpp/tests constructs an Engine
// (S36), which is how regressions in this family have survived review before.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xop::strategy {

/// Gains for the recursion BOTH controllers share:
///   ema_n   = alpha*s_n + (1-alpha)*ema_{n-1},   s_n in {0,1}, ema_0 = 0
///   error_n = target - ema_n
///   output  = kp*error_n + ki*I_{n-1} + kd*(error_n - error_{n-1})
///   I_n     = clamp(I_{n-1} + error_n, -integral_max, +integral_max)
///
/// Note `I_{n-1}`: both implementations compute the output from the integral
/// as it stood BEFORE this tick's update. That ordering is deliberate in
/// competitiveness_pid.hpp and mirrored inline in engine.cpp; reproducing it
/// here is what makes these bounds match the live controller.
struct PidGains {
    double        kp{0.0};
    double        ki{0.0};
    double        kd{0.0};
    double        target{0.0};
    double        ema_alpha{0.0};
    double        integral_max{0.0};
    std::uint32_t warmup_blocks{0};
};

struct PidOutputBounds {
    /// Infimum of the PID sum. An infimum rather than a minimum: driving the
    /// EMA to 1 is asymptotic. Safe as a bound.
    double min_output{0.0};
    /// Supremum, and ATTAINED exactly -- the never-fill trajectory holds
    /// ema at 0 and saturates the integrator in ceil(integral_max/target)
    /// ticks. This is the value the live bot has been sitting at.
    double max_output{0.0};
};

namespace detail {

[[nodiscard]] inline bool gains_are_finite(const PidGains& g) noexcept
{
    return std::isfinite(g.kp) && std::isfinite(g.ki) && std::isfinite(g.kd)
        && std::isfinite(g.target) && std::isfinite(g.ema_alpha)
        && std::isfinite(g.integral_max);
}

/// Largest EMA reachable within `warmup_blocks` ticks starting from 0, i.e.
/// 1 - (1-alpha)^W. Needed because the transient below is bounded by how far
/// the EMA can actually have travelled by the time the controller wakes up.
[[nodiscard]] inline double max_ema_after_warmup(const PidGains& g) noexcept
{
    const double a = std::clamp(g.ema_alpha, 0.0, 1.0);
    if (!(a > 0.0)) {
        return 0.0;
    }
    const double decayed = std::pow(1.0 - a, static_cast<double>(g.warmup_blocks));
    return std::clamp(1.0 - decayed, 0.0, 1.0);
}

}  // namespace detail

/// STEADY-STATE reachable interval, i.e. every tick after the first live one.
///
/// `output` is AFFINE in (ema_prev, s, I) and each coordinate ranges over a
/// closed interval or a two-point set, so the extrema sit at corners and the
/// bound is exact rather than conservative.
///
/// `max_output` here is `kp*target + ki*integral_max` for every configuration
/// this repo ships, and is INVARIANT IN kd -- see the header note. That
/// invariance is the property test_pid_reachability.cpp pins, because it is
/// the one an "improvement" would break.
[[nodiscard]] inline PidOutputBounds pid_steady_state_bounds(const PidGains& g) noexcept
{
    PidOutputBounds b{};
    if (!detail::gains_are_finite(g)) {
        return b;
    }
    const double a    = std::clamp(g.ema_alpha, 0.0, 1.0);
    const double c_e  = g.kd * a - g.kp * (1.0 - a);   // d(output)/d(ema_prev)
    const double s_hi = -a * (g.kp + g.kd);            // the s = 1 contribution
    const double base = g.kp * g.target;
    const double i_span = g.ki * g.integral_max;

    // Maximise/minimise each independent coordinate separately:
    //   s in {0, 1}            -> 0 or s_hi
    //   ema_prev in [0, 1]     -> 0 or c_e
    //   I in [-I_max, +I_max]  -> +/- ki*I_max
    b.max_output = base + std::max(0.0, s_hi) + std::max(0.0, c_e) + i_span;
    b.min_output = base + std::min(0.0, s_hi) + std::min(0.0, c_e) - i_span;
    return b;
}

/// The FIRST-POST-WARM-UP TRANSIENT, which is a genuinely separate regime.
///
/// `prev_error` is initialised to 0 and is only assigned inside the post-warm-up
/// branch, so the first live tick differences against a fictitious zero and
/// sees a full-size step with the integrator still empty:
///     output = (kp + kd) * error,   error in [target - ema_W, target]
/// where ema_W is the furthest the EMA can have travelled during warm-up.
///
/// At the shipped spread warm-up (50 blocks) this tops out at 0.10 -> mult
/// 0.90, which is exactly the eight `mult=0.900` lines in the live log. At
/// warmup_blocks = 500 it escapes the steady-state floor entirely, and with a
/// large kd it escapes the steady-state CEILING -- which is why this is
/// unioned in below and why the kd-invariance above is stated for the steady
/// state only.
[[nodiscard]] inline PidOutputBounds pid_transient_bounds(const PidGains& g) noexcept
{
    PidOutputBounds b{};
    if (!detail::gains_are_finite(g)) {
        return b;
    }
    const double kpd    = g.kp + g.kd;
    const double err_hi = g.target;
    const double err_lo = g.target - detail::max_ema_after_warmup(g);
    const double t_a    = kpd * err_hi;
    const double t_b    = kpd * err_lo;
    b.max_output = std::max(t_a, t_b);
    b.min_output = std::min(t_a, t_b);
    return b;
}

/// Reachable interval over the controller's whole life: the steady state
/// unioned with the first-post-warm-up transient.
[[nodiscard]] inline PidOutputBounds pid_output_bounds(const PidGains& g) noexcept
{
    if (!detail::gains_are_finite(g)) {
        return PidOutputBounds{};
    }
    const PidOutputBounds s = pid_steady_state_bounds(g);
    const PidOutputBounds t = pid_transient_bounds(g);
    return PidOutputBounds{std::min(s.min_output, t.min_output),
                           std::max(s.max_output, t.max_output)};
}

// -- spread PID: mult = clamp(1 - output, min_mult, max_mult) ---------------

/// The tightest multiplier the gains can produce, IGNORING the configured
/// clamp. Shipped gains give exactly 0.820.
[[nodiscard]] inline double spread_pid_reachable_min_mult(const PidGains& g) noexcept
{
    return 1.0 - pid_output_bounds(g).max_output;
}

/// The widest multiplier the gains can produce, ignoring the configured clamp.
[[nodiscard]] inline double spread_pid_reachable_max_mult(const PidGains& g) noexcept
{
    return 1.0 - pid_output_bounds(g).min_output;
}

/// The tightening clamp the engine should ACTUALLY apply, derived from the
/// gain budget so the knob and the controller cannot disagree.
///
/// ONE-DIRECTIONAL, and the direction matters. A configured floor ABOVE the
/// reachable floor genuinely truncates the controller and must be honoured --
/// that is a real safety bound. A configured floor BELOW it is a promise the
/// arithmetic cannot keep. So the effective value is the LARGER of the two.
///
/// Behaviourally a no-op on the shipped config: max(0.70, 0.820) = 0.820, and
/// the controller never produces a multiplier below 0.820 anyway, so the clamp
/// is inert at either value. It stops being a no-op the moment someone retunes
/// the gains, which is precisely when the operator needs it to be honest.
[[nodiscard]] inline double effective_pid_min_mult(double configured_min_mult,
                                                   const PidGains& g) noexcept
{
    if (!detail::gains_are_finite(g) || !std::isfinite(configured_min_mult)) {
        return configured_min_mult;
    }
    const double reachable = spread_pid_reachable_min_mult(g);
    if (!std::isfinite(reachable)) {
        return configured_min_mult;
    }
    return std::max(configured_min_mult, reachable);
}

// -- competitiveness PID: offset = clamp(round(-output), min_off, max_off) --

/// Byte-for-byte mirror of competitiveness_pid.hpp:
///     static_cast<int>(x + (x >= 0.0 ? 0.5 : -0.5))
/// `static_cast` truncates toward zero, so this is round-half-AWAY-from-zero,
/// NOT std::lround (half-to-even) and NOT static_cast<int>(x + 0.5) (which is
/// wrong for every negative value -- the case that matters here, since the
/// live controller only ever emits negative offsets).
///
/// -0.0 >= 0.0 is true in IEEE-754, so negative zero takes the positive branch
/// and yields 0. Correct, and pinned by test.
[[nodiscard]] inline int pid_round_half_away_from_zero(double x) noexcept
{
    if (!std::isfinite(x)) {
        return 0;
    }
    return static_cast<int>(x + (x >= 0.0 ? 0.5 : -0.5));
}

/// Most negative (most aggressive) offset the gains can produce, ignoring the
/// configured clamp. Shipped comp gains give raw -3.20, which ROUNDS to -3.
[[nodiscard]] inline int comp_pid_reachable_min_offset(const PidGains& g) noexcept
{
    return pid_round_half_away_from_zero(-pid_output_bounds(g).max_output);
}

/// Most positive (most conservative) offset the gains can produce.
[[nodiscard]] inline int comp_pid_reachable_max_offset(const PidGains& g) noexcept
{
    return pid_round_half_away_from_zero(-pid_output_bounds(g).min_output);
}

/// Same one-directional rule as effective_pid_min_mult.
///
/// A no-op on the shipped config -- max(-3, -3) = -3 -- and note WHY -3 is the
/// live value: it comes out of the ROUNDING step, not out of the clamp.
/// `clamp(-3, -3, 3)` is the identity here, so `comp_pid_min_offset` never
/// fires and widening it to -5 or -10 would change nothing at all. The floor
/// is the gain budget.
[[nodiscard]] inline int effective_comp_pid_min_offset(int configured_min_offset,
                                                       const PidGains& g) noexcept
{
    if (!detail::gains_are_finite(g)) {
        return configured_min_offset;
    }
    return std::max(configured_min_offset, comp_pid_reachable_min_offset(g));
}

// -- config advisory --------------------------------------------------------

/// One unreachable-clamp finding, reported rather than thrown. See
/// `spread_pid_min_mult_is_unreachable` for why this must never be fatal.
struct PidReachabilityFinding {
    const char* key{""};        ///< e.g. "pid_min_mult"
    double      configured{0.0};///< as written, or as defaulted
    double      reachable{0.0}; ///< what the gains can actually produce
    double      authority{0.0}; ///< kp*target + ki*integral_max
};

/// True when the configured floor can NEVER bind -- i.e. it reads as a tuned
/// safety value while being decoration.
///
/// MUST NOT BE PROMOTED TO A ConfigError. The config.hpp DEFAULTS are
/// themselves unreachable (pid_target_fill_rate{0.10} and
/// pid_integral_max{2.0} give the same 0.18 authority against
/// pid_min_mult{0.70}), so a boot-time refusal would reject a bare config, a
/// fresh deployment, config.example.yaml, and every load_config case in
/// cpp/tests/test_config.cpp -- whose kMinimalValidYaml carries no pid_* keys
/// at all. Refusal is the right tool for a contradiction that cannot be
/// resolved (comp_pid_min_offset > comp_pid_max_offset, config.cpp:1780).
/// This one resolves deterministically in one line, so it advises.
[[nodiscard]] inline bool spread_pid_min_mult_is_unreachable(
    double configured_min_mult,
    const PidGains& g,
    PidReachabilityFinding& out) noexcept
{
    if (!detail::gains_are_finite(g) || !std::isfinite(configured_min_mult)) {
        return false;
    }
    const double reachable = spread_pid_reachable_min_mult(g);
    if (!std::isfinite(reachable) || !(configured_min_mult < reachable)) {
        return false;
    }
    out.key        = "pid_min_mult";
    out.configured = configured_min_mult;
    out.reachable  = reachable;
    out.authority  = g.kp * g.target + g.ki * g.integral_max;
    return true;
}

/// True when `comp_pid_min_offset` can never bind. Silent on the shipped
/// config (raw -3.20 rounds to -3, which equals the configured -3) and fires
/// on the config.hpp defaults (target 0.05 -> authority 2.40 -> -2 > -3),
/// which is the latent second instance of the same bug.
[[nodiscard]] inline bool comp_pid_min_offset_is_unreachable(
    int configured_min_offset,
    const PidGains& g,
    PidReachabilityFinding& out) noexcept
{
    if (!detail::gains_are_finite(g)) {
        return false;
    }
    const int reachable = comp_pid_reachable_min_offset(g);
    if (!(reachable > configured_min_offset)) {
        return false;
    }
    out.key        = "comp_pid_min_offset";
    out.configured = static_cast<double>(configured_min_offset);
    out.reachable  = static_cast<double>(reachable);
    out.authority  = g.kp * g.target + g.ki * g.integral_max;
    return true;
}

}  // namespace xop::strategy

#endif  // XOP_STRATEGY_PID_REACHABILITY_HPP
