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
// Step 8. classify_cross_published_mid now has NO production caller: the
// fallback for a missing BBO is the +/-5% mid band inside classify_cross_bbo
// itself. The legacy predicate is kept only so the tests can pin what the
// old rule did and measure the S33 change against it.
//
// It also carries classify_tier_refresh, the canceller's zone selection,
// for the reason given above that function.
//
// Pure header, no engine types, so every predicate here is driven directly
// by cpp/tests/test_cross_guard.cpp.
// ---------------------------------------------------------------------------

#include <cmath>

namespace xop::execution {

enum class CrossVerdict {
    Ok,             ///< safe to post
    Crossed,        ///< would cross; suppress
    Indeterminate,  ///< no usable reference -- decide nothing
};

/// THE LEGACY RULE, kept as a test reference only. Bid above the published
/// mid, or ask below it. This is what Step 8 inlined from 4d3f30d until S33
/// promoted classify_cross_bbo below; it has no production caller now.
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
/// Each side is judged against the OPPOSITE touch ALONE: a bid needs only
/// best_ask, an ask needs only best_bid. The +/-5% published-mid band is
/// the fallback for a missing RELEVANT touch, not for a missing full BBO.
///
/// [S33 2026-09-12] It used to demand BOTH touches before it would reach
/// either verdict, so a one-sided book fell straight through to the mid
/// band -- which is far too loose to catch a cross. An ask at 99 against a
/// standing bid of 100, with no best_ask at all, is plainly liftable, yet
/// the band asked 99 < 100*0.95 = 95, answered no, and the ask got posted
/// into the bid. One-sided and asymmetric books are the exact condition
/// this gate exists for (it is the live suppression gate at engine.cpp
/// Step 8 since PR #148), so the missing half of the book must not
/// disable the half that is present.
///
/// book_inverted still requires both touches: it is a claim about the
/// book, not about one quote, and with one touch there is nothing to
/// invert. It stays reported-not-special-cased for the reason above.
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
    const bool have_bid = best_bid > 0.0 && std::isfinite(best_bid);
    const bool have_ask = best_ask > 0.0 && std::isfinite(best_ask);
    r.book_inverted = have_bid && have_ask && best_bid >= best_ask;

    // The touch on the other side of the book -- the one that could take
    // this quote the moment it is posted. Only that one gates the verdict.
    const bool have_opposite = is_ask ? have_bid : have_ask;
    if (have_opposite) {
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

// ---------------------------------------------------------------------------
// [S33 2026-09-12] THE CANCELLER'S REFRESH ZONES, lifted out so they can be
// driven from a test.
//
// classify_tier_staleness is a member of OfferManager, which cannot be
// constructed without an io_context, a wallet RPC client, a Dexie client and
// the shared State -- i.e. not from xop_tests. Its zone selection is
// nonetheless pure arithmetic on numbers the caller already has, so it lives
// here and offer_manager.cpp calls it. Same move the repo already made for
// select_repost_keys and shift_schedule_to_floor in engine.hpp.
//
// It sits in this header rather than a new one because it is the OTHER half
// of the same decision: classify_tier_staleness computes `crossed` with the
// predicate above and then picks a zone with the predicate below, and the
// two drifting apart is the bug class this whole file documents.
// ---------------------------------------------------------------------------

/// How much further a FAVORABLE drift is tolerated than an adverse one
/// before the offer is refreshed. A quote that drifted in our favour is
/// still earning; only a gross disconnect is worth the cancel+repost fee.
inline constexpr double kFavorableDriftMultiplier = 3.0;

/// Mirrors TierStaleness (offer_manager.hpp). Restated rather than included
/// so this header stays free of engine types and the tests stay pure.
enum class TierRefresh {
    Fresh,    ///< keep the offer live
    Stale,    ///< cancel and repost at the new price
    Expired,  ///< aged out; cancel regardless of price
};

/// The zone selection of classify_tier_staleness, in its own order. The
/// order is the contract: a cross outranks every age guard, and the soft-TTL
/// zone outranks the minimum-age guard.
///
/// @param crossed             from classify_cross_bbo, above.
/// @param past_soft_ttl       age >= ttl_blocks.
/// @param below_min_age       age < kMinRefreshAgeBlocks.
/// @param adverse             the drift makes our quote more generous.
/// @param price_deviation     ABSOLUTE fractional deviation from optimal.
/// @param tier_threshold      kSelectiveRefreshThreshold x (1 + tier x scale).
/// @param soft_ttl_threshold  kSoftTtlAdverseThreshold.
[[nodiscard]] inline TierRefresh classify_tier_refresh(
    bool   crossed,
    bool   past_soft_ttl,
    bool   below_min_age,
    bool   adverse,
    double price_deviation,
    double tier_threshold,
    double soft_ttl_threshold) noexcept
{
    // (1) Crossed: urgent, ahead of every age guard.
    if (crossed) {
        return TierRefresh::Stale;
    }
    // (2) Soft TTL zone: an aged offer expires -- rather than going merely
    //     stale -- on ADVERSE drift past the gentler soft-TTL threshold.
    //     Direction still decides here: a favorably drifted quote is more
    //     conservative than the one we would post now, so it is left to rest
    //     until the hard TTL (kHardTtlMultiplier, offer_manager.hpp:821-826)
    //     expires it unconditionally.
    if (past_soft_ttl) {
        return (adverse && price_deviation > soft_ttl_threshold)
            ? TierRefresh::Expired
            : TierRefresh::Fresh;
    }
    // (3) Too young: the cancel+recreate round trip costs more than the
    //     adverse selection it would avoid.
    if (below_min_age) {
        return TierRefresh::Fresh;
    }
    // (4) Normal zone: tier-scaled threshold, widened for favorable drift.
    const double limit = adverse
        ? tier_threshold
        : tier_threshold * kFavorableDriftMultiplier;
    return price_deviation > limit ? TierRefresh::Stale : TierRefresh::Fresh;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_CROSS_GUARD_HPP
