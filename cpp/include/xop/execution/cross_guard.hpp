#ifndef XOP_EXECUTION_CROSS_GUARD_HPP
#define XOP_EXECUTION_CROSS_GUARD_HPP
// ---------------------------------------------------------------------------
// cross_guard.hpp -- the pre-post crossing predicate, both versions of it.
//
// [CROSSGUARD 2026-09-03] PROMOTED TO ACTIVE DECISION (S33 RESOLVED).
// Step 8 evaluates classify_cross_bbo to ensure offers do not cross opposite-side
// BBO (bids >= best_ask, asks <= best_bid), aligning pre-post suppression with
// OfferManager::classify_tier_staleness. On wide or asymmetric order books,
// this allows valid non-crossing quotes to be posted rather than erroneously
// suppressing all asks below the published midpoint.
//
// WHY BOTH EXIST
// --------------
// Step 8's legacy crossed-mid guard dropped a bid above the PUBLISHED MID and
// an ask below it. Its stated purpose was to pre-empt the canceller: an offer
// that crosses gets cancelled next cycle by classify_tier_staleness, wasting
// the fee and carrying one block of adverse selection.
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
// In PR #148 / S33, classify_cross_bbo was promoted to the active gate in
// Step 8, with classify_cross_published_mid retained as a fallback and
// for regression testing.
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
