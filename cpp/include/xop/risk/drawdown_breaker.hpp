// drawdown_breaker.hpp -- Pure decision math for the P&L/equity circuit
// breakers in Engine::step_check_alerts.
//
// [DRAWDOWN-USD 2026-08-02] First pass: the breakers were moved off the raw
// cross-pair quote-mojo sum onto USD-normalized figures (a DBX mojo is
// ~1/73rd of a wUSDC.b mojo in value; ratios only cancel units when both
// sides share one).
//
// [DRAWDOWN-EQUITY 2026-08-04] Second pass, after a live false trip: the
// max-drawdown breaker measured drawdown against the P&L HIGH-WATER MARK,
// not portfolio equity.  Measured on 2026-08-04 04:14: XCH retraced ~5%
// overnight ($1.575 -> $1.498) with ~54 XCH held; the unrealized mark move
// (~-$8) was ~5% of the ~$158 portfolio -- but 32-60% of the ~$25 P&L
// peak, so the breaker read "drawdown 60%", PAUSED the engine, and
// re-alerted every cycle, while the inventory basis ($1.4711) was BELOW
// the mid ($1.4984), i.e. nothing was actually wrong.  A P&L-peak
// denominator punishes the bot for having been only modestly profitable:
// the same dollar wiggle reads N times larger the smaller the accumulated
// profit.  Industry-standard drawdown semantics divide by PORTFOLIO
// EQUITY, so the breaker now does too:
//
//     drawdown = (peak_equity_usd - equity_usd) / peak_equity_usd
//
// with equity = sum over assets of holdings x USD price, and the peak
// tracked as an in-memory high-water mark exactly like the old P&L peak
// (re-seeded from the first cycle after every restart -- a restart
// re-anchors the peak; accepted and documented at the engine member).
// The 5% threshold that fired predates functional breaker math (the old
// mixed-unit breaker could never fire meaningfully) and was never
// calibrated; the recalibrated default is 10% OF EQUITY under the renamed
// key risk.max_drawdown_frac.
//
// Compliant with:
//   ISO/IEC 5055  -- pure, NaN-guarded, division-by-zero-guarded
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
//                    (tests/test_drawdown_breaker.cpp)

#ifndef XOP_RISK_DRAWDOWN_BREAKER_HPP
#define XOP_RISK_DRAWDOWN_BREAKER_HPP

#include <chrono>
#include <vector>

namespace xop::risk {

/// Equity drawdown fraction, USD in / dimensionless out:
///
///     peak > 0 :  (peak - equity) / peak
///     peak <= 0:  0.0 -- equity is a sum of holdings x non-negative
///                 prices, so a non-positive peak means nothing has been
///                 valued yet and there is no measurable drawdown.  (This
///                 differs from the retired P&L variant, whose "losing
///                 from a never-profitable baseline" branch existed
///                 because P&L can be genuinely negative.)
///
/// The caller compares against risk.max_drawdown_frac and applies the
/// startup grace window; this function is just the unit-critical
/// arithmetic.  NaN inputs fail every comparison and yield 0.0 (no trip).
// ---------------------------------------------------------------------------
// [S27 2026-08-27] Fail-closed decision -- a SEPARATE documentation block.
// Without this break the comment below continues the preceding
// equity_drawdown_frac Doxygen block, so generated docs attach that
// function's arithmetic and NaN-return prose to this bool-returning API.
// ---------------------------------------------------------------------------

/// Should the engine stop trading because it cannot value
/// its own book?
///
/// TWO DISTINCT FAIL-OPEN STATES, both of which leave the drawdown breaker
/// unable to fire however bad things get.
///
/// 1. NO PEAK EVER ESTABLISHED.  Marking a cycle degraded freezes the peak,
///    which is right when a peak exists -- a suspect number must not ratchet
///    the high-water mark.  On a fresh process there is nothing to freeze,
///    so the peak stays 0 and `equity_drawdown_frac` returns 0.0 for a
///    non-positive peak.
///
/// 2. EVERY HELD ASSET UNPRICED, EVEN WITH A PEAK.  This one refutes the
///    first version of this helper, which assumed "a frozen peak is a real
///    reference and the ordinary comparison still protects us".  It does
///    not: `effective_usd_per_unit` carries the last known price with NO
///    expiry check -- expiry only raises the degraded flag, it does not stop
///    the carry being summed.  So when nothing is live, equity holds at its
///    carried value, which is the same number the peak was frozen at, and
///    the drawdown sits at 0 indefinitely.  Comparing a frozen equity to a
///    frozen peak cannot detect anything.
///
/// Either way the answer is to stop: an engine that cannot measure its
/// exposure has no business adding to it.
///
/// BOTH cases additionally require `valuation_degraded`, and that guard is
/// load-bearing rather than belt-and-braces.  "Every asset unpriced THIS
/// HEARTBEAT" is not the same as "we cannot value the book": a momentary
/// feed gap with every carry still inside `valuation_carry_ttl_blocks` is
/// precisely the transient the carry mechanism exists to bridge, and
/// config.hpp says so -- "a data gap must not read as a crash".  Without
/// this guard a single bad tick would permanently latch the breaker, and a
/// configured TTL of 0 ("never expire") would be ignored outright.
/// Degradation is what distinguishes a gap from an outage.
///
/// PARTIAL degradation with a valid peak is deliberately NOT a trigger --
/// there at least one asset is still live, equity still moves, and the
/// ordinary comparison genuinely does work.
/// Whether a held asset with no live price should be WRITTEN OFF at $0
/// rather than degrading the valuation cycle.
///
/// [S32 2026-08-27] "No price available" hides two conditions that must not
/// share a response:
///
/// 1. NO PRICING PATH EXISTS.  No enabled pair names the asset, so nothing
///    in the configuration can ever produce a price for it.  This is a
///    permanent, deterministic property of the config, known at startup and
///    unchanged by any market event.  wUSDC.b after the warp.green
///    compromise is the live example: both its pairs are disabled, so it is
///    structurally unpriceable and will stay so until the operator changes
///    the config.  Treating this as a data gap is what makes the engine
///    unable to resume at all -- every cycle degrades from cycle 0, the
///    authority gate never re-arms, the peak never seeds, and the drawdown
///    breaker sits inert against a $0 peak.  The honest reading is that the
///    operator has declared this asset unpriceable, so value it at $0 and
///    let the rest of the book carry the valuation.
///
/// 2. A PATH EXISTS BUT IS QUIET.  The feed is down, the book is junk this
///    heartbeat, or the mid failed its valuation grade.  This is exactly
///    the transient the carry mechanism exists to bridge, and marking it to
///    zero would be catastrophic: a momentary CoinGecko outage would write
///    the whole book to nothing, and on a fresh process -- before the first
///    fetch lands -- it would seed the peak from a near-zero equity and
///    leave the breaker under-protective for the rest of the run.
///
/// So the write-off is gated on the ABSENCE OF A CONFIGURED ROUTE, never on
/// the absence of a quote. `has_carry` is required to be false as well:
/// a carry can only exist if the asset was priced earlier in this run, so
/// its presence contradicts "no path" and the carried value is the better
/// number.
///
/// A written-off asset contributes $0 to equity and does NOT set the
/// degraded flag -- there is nothing degraded about a number the operator
/// has effectively declared. It is still excluded from the live count, so
/// a book consisting ENTIRELY of written-off assets leaves no live price,
/// no peak, and fails closed through the ordinary path below.
[[nodiscard]] constexpr bool unpriced_asset_is_written_off(
    bool has_pricing_path,
    bool has_carry) noexcept
{
    return !has_pricing_path && !has_carry;
}

[[nodiscard]] constexpr bool unvaluable_book_must_fail_closed(
    bool   grace_elapsed,
    bool   valuation_degraded,
    bool   all_held_assets_unpriced,
    double peak_equity_usd) noexcept
{
    if (!grace_elapsed || !valuation_degraded) {
        return false;
    }
    if (all_held_assets_unpriced) {
        return true;
    }
    return !(peak_equity_usd > 0.0);
}

[[nodiscard]] constexpr double equity_drawdown_frac(
    double peak_equity_usd, double equity_usd) noexcept
{
    if (peak_equity_usd > 0.0) {
        return (peak_equity_usd - equity_usd) / peak_equity_usd;
    }
    return 0.0;
}

/// Rolling-window loss threshold in USD:
///
///     anchor = equity_usd              when the portfolio has a valuation,
///              anchor_fallback_usd     otherwise (first cycles only).
///     threshold = anchor * max_window_loss_bps / 10,000
///
/// [DRAWDOWN-EQUITY 2026-08-04] The anchor is CURRENT PORTFOLIO EQUITY:
/// 250 bps of the measured ~$150 book is ~$3.75, where the retired
/// |P&L-HWM| anchor produced $1.09 and tripped spuriously on the 08-02
/// overnight mark wiggle.  The window's FLOW series stays P&L
/// (front_pnl_usd - current_pnl_usd); only the threshold scale changes.
/// Non-positive inputs yield a 0 threshold, which the caller treats as
/// "check disabled this cycle".
[[nodiscard]] constexpr double window_loss_threshold_usd(
    double equity_usd,
    double anchor_fallback_usd,
    double max_window_loss_bps) noexcept
{
    const double anchor =
        (equity_usd > 0.0)          ? equity_usd
      : (anchor_fallback_usd > 0.0) ? anchor_fallback_usd
                                    : 0.0;
    if (!(max_window_loss_bps > 0.0)) return 0.0;   // NaN-safe
    return anchor * max_window_loss_bps / 10'000.0;
}

/// Fixed nominal for the window-loss anchor when NOTHING can be valued
/// yet (no equity, no live 1-XCH USD price): $1.50, near the 2026-08 spot
/// (~$1.39-1.58).  A LOWER anchor means a LOWER loss threshold -- the
/// breaker fires earlier, the safe failure direction for a nominal that
/// only matters in the first cycles of a run.
inline constexpr double kWindowAnchorFallbackUsd = 1.50;

// ---------------------------------------------------------------------------
// Portfolio equity valuation ([DRAWDOWN-EQUITY 2026-08-04]).
// ---------------------------------------------------------------------------

/// One asset's inputs to the equity sum.  `units` is the display-unit
/// holding (mojos / mojos_per_unit).  `live_usd_per_unit` is this cycle's
/// price (0 = no conversion available this cycle); `last_usd_per_unit` is
/// the most recent successful valuation (0 = never valued).
struct AssetValuationInput {
    double units{0.0};
    double live_usd_per_unit{0.0};
    double last_usd_per_unit{0.0};
};

/// The price actually used for an asset this cycle: live when available,
/// otherwise the LAST KNOWN price.  A conversion that vanishes for a
/// cycle (empty book, cold feed) must carry the asset at its last
/// valuation -- dropping it would delete the asset's full value from
/// equity and read as an instant crash, firing the breaker on a DATA GAP
/// rather than a market move.  An asset that has never been valued
/// contributes 0 (it never contributed to the peak either, so the ratio
/// stays coherent).
[[nodiscard]] constexpr double effective_usd_per_unit(
    double live_usd_per_unit, double last_usd_per_unit) noexcept
{
    if (live_usd_per_unit > 0.0) return live_usd_per_unit;   // NaN-safe
    if (last_usd_per_unit > 0.0) return last_usd_per_unit;
    return 0.0;
}

/// Total portfolio equity in USD: sum of units x effective price.
/// Non-positive or NaN unit counts contribute 0 (holdings cannot be
/// negative in spot; a poisoned input must not drag equity down).
[[nodiscard]] inline double portfolio_equity_usd(
    const std::vector<AssetValuationInput>& assets) noexcept
{
    double total = 0.0;
    for (const auto& a : assets) {
        if (!(a.units > 0.0)) continue;
        total += a.units * effective_usd_per_unit(a.live_usd_per_unit,
                                                  a.last_usd_per_unit);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Breaker re-alert gate ([DRAWDOWN-EQUITY 2026-08-04] item 6).
//
// While the engine sat Paused with the breaker condition still true, the
// engine re-sent the CRITICAL alert every cycle and AlertManager's 60 s
// per-rule cooldown still let one through every minute (measured spam
// every ~10-30 s across the interleaved rules on 2026-08-04).  The gate
// lets the FIRST alert through immediately, then suppresses repeats until
// `interval` has elapsed; the caller logs the recurring condition at info
// level in between and calls clear() when the condition (or the pause)
// goes away so the next episode alerts immediately again.
// ---------------------------------------------------------------------------
class BreakerRealertGate {
public:
    /// True when an alert should be sent now.  Marks the gate as fired at
    /// `now` when returning true.
    [[nodiscard]] bool should_alert(
        std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::duration   interval) noexcept
    {
        if (!fired_ || now - last_ >= interval) {
            fired_ = true;
            last_  = now;
            return true;
        }
        return false;
    }

    /// Re-arm: the next should_alert() fires immediately.
    void clear() noexcept { fired_ = false; }

    [[nodiscard]] bool fired() const noexcept { return fired_; }

private:
    bool fired_{false};
    std::chrono::steady_clock::time_point last_{};
};

}  // namespace xop::risk

#endif  // XOP_RISK_DRAWDOWN_BREAKER_HPP
