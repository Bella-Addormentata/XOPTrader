#ifndef XOP_EXECUTION_CROSS_GUARD_HPP
#define XOP_EXECUTION_CROSS_GUARD_HPP
// ---------------------------------------------------------------------------
// cross_guard.hpp -- the pre-post crossing predicate, both versions of it.
//
// [CROSSGUARD 2026-09-01] SHADOW ONLY. Nothing here changes what is
// suppressed. Step 8 still suppresses on the published-mid verdict exactly
// as it has since 2026-04-12; the BBO verdict is computed alongside it and
// only the DISAGREEMENT is logged. Measure first.
//
// WHY BOTH EXIST
// --------------
// Step 8's crossed-mid guard drops a bid above the PUBLISHED MID and an ask
// below it. Its stated purpose is to pre-empt the canceller: an offer that
// crosses gets cancelled next cycle by classify_tier_staleness, wasting the
// fee and carrying one block of adverse selection.
//
// It was a bit-exact predictor of that canceller when it was written, in
// 4d3f30d (2026-04-12), because the canceller then used the model mid too.
// ONE DAY LATER a932a5d replaced the canceller's test with a BBO test and
// did not touch the guard, recording the reason in offer_manager.cpp:
//
//     "Using model mid as the threshold is too conservative -- a bid
//      between mid and best_ask is a valid competitive bid, not a crossed
//      offer."
//
// git log -L over the guard returns 4d3f30d alone: it has never been
// modified. So for about four and a half months the guard has been strictly
// STRICTER than the thing it claims to pre-empt. On an uncrossed book
// best_bid <= mid <= best_ask, so it removes every ask in (best_bid, mid]
// and every bid in [mid, best_ask) that the canceller would call Fresh --
// which is precisely the profitable half-spread on each side.
//
// The divergence got worse, not better: since 2026-08-01 the ladder centre
// is a fair-value blend that deliberately leaves the published mid behind
// (measured p50 99 bps, p99 737 bps on XCH/DBX), so the tier prices the
// guard judges are no longer in the same price frame as its reference.
//
// WHY THIS IS A SHADOW AND NOT A FIX
// ----------------------------------
// Because the numbers say there is nothing to recover and everything to
// risk. The guard is INERT on the only enabled pair: zero firings across
// six live log rotations, one firing in the entire retained corpus
// (2026-08-19, one bid tier). Every pair with material guard activity is
// enabled:false. And every "would have suppressed" figure available today
// is a RECONSTRUCTION from persisted ladders, not an observation -- the
// guard has never been instrumented.
//
// Meanwhile a change in this exact Step 8 family shipped a regression
// through four review rounds and a thousand green tests, because nothing in
// cpp/tests constructs an Engine. So: extract the predicate, pin it, count
// the disagreement in production, and decide from data rather than from a
// reading. If the counter stays at zero the question answers itself.
//
// Pure header, no engine types, so both predicates are driven directly by
// cpp/tests/test_cross_guard.cpp.
// ---------------------------------------------------------------------------

#include <cmath>

namespace xop::execution {

enum class CrossVerdict {
    Ok,             ///< safe to post
    Crossed,        ///< would cross; suppress
    Indeterminate,  ///< no usable reference -- decide nothing
};

/// WHAT STEP 8 DOES TODAY. Bid above the published mid, or ask below it.
///
/// @param is_ask  side of the tier.
/// @param price   tier price.
/// @param published_mid  MarketDataFeed::get_mid_price, mojo-scaled.
[[nodiscard]] inline CrossVerdict classify_cross_published_mid(
    bool is_ask, double price, double published_mid) noexcept
{
    if (!(published_mid > 0.0) || !std::isfinite(published_mid)
        || !(price > 0.0) || !std::isfinite(price)) {
        return CrossVerdict::Indeterminate;
    }
    if (is_ask) {
        return price < published_mid ? CrossVerdict::Crossed
                                     : CrossVerdict::Ok;
    }
    return price > published_mid ? CrossVerdict::Crossed : CrossVerdict::Ok;
}

/// Result of the BBO predicate, with the provenance the log needs.
struct BboCrossCheck {
    CrossVerdict verdict{CrossVerdict::Indeterminate};
    /// best_bid >= best_ask. Routine on dexie, which has no matching
    /// engine: an uncrossed book is a coincidence of who has not yet taken
    /// what, not an invariant. Reported rather than special-cased, because
    /// the CANCELLER does not special-case it either and this predicate
    /// exists to predict the canceller.
    bool book_inverted{false};
    /// The BBO was unavailable and the +/-5% mid buffer was used instead,
    /// mirroring the canceller's own fallback.
    bool used_mid_fallback{false};
};

/// WHAT THE CANCELLER DOES -- offer_manager.cpp classify_tier_staleness.
/// Mirrored deliberately, including its inequalities and its fallback, so
/// a disagreement counted against it is a real prediction of a real cancel
/// rather than an artifact of two nearly-similar rules.
///
/// Bid crosses iff price >= best_ask; ask crosses iff price <= best_bid.
/// With no BBO, fall back to the published mid with a 5% buffer.
[[nodiscard]] inline BboCrossCheck classify_cross_bbo(
    bool   is_ask,
    double price,
    double best_bid,
    double best_ask,
    double published_mid) noexcept
{
    BboCrossCheck r{};
    if (!(price > 0.0) || !std::isfinite(price)) {
        return r;
    }
    const bool have_bbo = best_bid > 0.0 && best_ask > 0.0
                       && std::isfinite(best_bid) && std::isfinite(best_ask);
    if (have_bbo) {
        r.book_inverted = best_bid >= best_ask;
        r.verdict = is_ask ? (price <= best_bid ? CrossVerdict::Crossed
                                                : CrossVerdict::Ok)
                           : (price >= best_ask ? CrossVerdict::Crossed
                                                : CrossVerdict::Ok);
        return r;
    }
    if (published_mid > 0.0 && std::isfinite(published_mid)) {
        constexpr double kCrossBuffer = 0.05;
        r.used_mid_fallback = true;
        r.verdict = is_ask
            ? (price < published_mid * (1.0 - kCrossBuffer)
                   ? CrossVerdict::Crossed : CrossVerdict::Ok)
            : (price > published_mid * (1.0 + kCrossBuffer)
                   ? CrossVerdict::Crossed : CrossVerdict::Ok);
        return r;
    }
    return r;  // Indeterminate
}

[[nodiscard]] inline const char* cross_verdict_name(CrossVerdict v) noexcept
{
    switch (v) {
        case CrossVerdict::Ok:            return "ok";
        case CrossVerdict::Crossed:       return "crossed";
        case CrossVerdict::Indeterminate: break;
    }
    return "indeterminate";
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_CROSS_GUARD_HPP
