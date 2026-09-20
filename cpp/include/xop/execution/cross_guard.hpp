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
#include <string>

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

// ---------------------------------------------------------------------------
// [S72 2026-09-20] strategy.price_cancel_mode: margin -- cancel for price
// only when the FILL would be a bad one.
//
// classify_tier_refresh above asks how far the tier's NEW optimal price has
// moved from the resting one.  That is a question about the ladder, not about
// the offer: 282 of the cancels in the 14 days to 2026-09-20 were this rule,
// at a median deviation of 1.30%, on books where our quotes rest 140-240 bps
// from the touch and nothing trades for days.  At the block they were
// cancelled, 126 of the 248 with a recorded ladder rested no closer to the
// centre than the ladder's own innermost tier (test_price_cancel_replay.cpp).
// The XCH/DBX tier-0 ask of 2026-09-10 went 78.69 -> 79.52 -> 78.70 -> 79.94
// in three hours, each leg a ~1% "adverse" drift of the tier's optimal price:
// that is chasing the engine's own fair-value noise, one fee per flip.
//
// The margin rule asks the offer's own question: if this were taken right now
// at its resting price, would it still earn the edge Step 7 demands of a NEW
// offer -- max(min_profit_margin, quote_width_sigma_mult x combined_sigma,
// tibetswap fee) -- against the CURRENT centre?  Both numbers are the ones
// Step 7 threaded to Step 8 (PairCycleState::quote_mid_mojos and
// quote_min_half_spread_bps), so the canceller cannot disagree with the
// pricer about where the floor is; two rules for one decision drifting apart
// is the bug class this file documents.
//
// WHY THE RESTING FLOOR IS A FRACTION OF THE POSTING FLOOR.  Step 7's
// width-floor pass pushes tiers out to EXACTLY centre x (1 +/- floor), so an
// offer is routinely born with edge == floor.  Were the cancel threshold the
// same number, the first adverse tick after kMinRefreshAgeBlocks would refresh
// it, and the rule would cancel MORE than the one it replaces.  A post
// threshold and a cancel threshold that coincide are a flapping switch;
// edge_retain is the gap between them.
// ---------------------------------------------------------------------------

/// The edge, in bps of the centre, a fill at @p price would earn: positive
/// when an ask rests ABOVE the centre or a bid BELOW it.  0 for an unusable
/// input; callers gate on margin_reference_usable first.
[[nodiscard]] inline double resting_edge_bps(bool   is_ask,
                                             double price,
                                             double centre) noexcept
{
    if (!(price > 0.0) || !std::isfinite(price)
        || !(centre > 0.0) || !std::isfinite(centre)) {
        return 0.0;
    }
    const double signed_gap = is_ask ? (price - centre) : (centre - price);
    return signed_gap / centre * 10'000.0;
}

/// Whether Step 7 handed over a centre, a floor and a retain fraction the
/// margin rule can reason from.  quote_mid_mojos and the floor stay 0 until
/// Step 7 reaches ladder generation for the pair this cycle.
[[nodiscard]] inline bool margin_reference_usable(double centre,
                                                  double min_edge_bps,
                                                  double edge_retain) noexcept
{
    return centre > 0.0 && std::isfinite(centre)
        && min_edge_bps > 0.0 && std::isfinite(min_edge_bps)
        && edge_retain > 0.0 && edge_retain <= 1.0;
}

enum class MarginRefresh {
    Fresh,        ///< keep the offer live
    Stale,        ///< crossed, or a fill would earn less than the resting floor
    NoReference,  ///< no usable centre/floor -- the caller falls back to
                  ///< classify_tier_refresh, i.e. to today's rule, rather than
                  ///< resting an offer nothing is pricing
};

/// The margin-mode zone selection, in its own order.  As above, the order is
/// the contract: a cross outranks everything, the minimum-age guard outranks
/// the edge test, and direction is not an input at all -- a favourable drift
/// can only ADD edge, so it can never cancel.
///
/// @param crossed       from classify_cross_bbo, above.
/// @param below_min_age age < kMinRefreshAgeBlocks.
/// @param is_ask        side of the resting offer.
/// @param price         its resting price.
/// @param centre        Step 7's ladder centre for the pair, THIS cycle.
/// @param min_edge_bps  Step 7's minimum half-spread for the pair, THIS cycle.
/// @param edge_retain   fraction of min_edge_bps a resting offer must keep.
[[nodiscard]] inline MarginRefresh classify_tier_refresh_margin(
    bool   crossed,
    bool   below_min_age,
    bool   is_ask,
    double price,
    double centre,
    double min_edge_bps,
    double edge_retain) noexcept
{
    // (1) Crossed: urgent, ahead of every guard -- and decidable with no
    //     centre at all, so it is tested before the reference is.
    if (crossed) {
        return MarginRefresh::Stale;
    }
    if (!margin_reference_usable(centre, min_edge_bps, edge_retain)
        || !(price > 0.0) || !std::isfinite(price)) {
        return MarginRefresh::NoReference;
    }
    // (2) Too young: same guard, same reason, as the deviation rule.
    if (below_min_age) {
        return MarginRefresh::Fresh;
    }
    // (3) The edge test.  Strictly below: an offer resting exactly on the
    //     resting floor is still acceptable, as one exactly on the posting
    //     floor is to Step 7.
    return resting_edge_bps(is_ask, price, centre) < min_edge_bps * edge_retain
        ? MarginRefresh::Stale
        : MarginRefresh::Fresh;
}

/// The offer_log cancel_reason of a margin cancel: the edge the fill would
/// have earned over the edge a resting offer must keep, e.g.
/// "margin_breach(41.7/61.5bps)".  A negative edge is an offer resting on the
/// wrong side of the centre.  The "margin_breach(" prefix is what reports
/// group on, as they do on "price_adverse(".
[[nodiscard]] inline std::string margin_breach_reason(double edge_bps,
                                                      double required_edge_bps)
{
    // One decimal, by integer arithmetic: no locale, no "-0.0", and no
    // printf for -Wformat-truncation to reason about.  A non-finite input
    // prints as 0.0 rather than putting "nan" in a column people filter.
    const auto tenths = [](double v) {
        if (!std::isfinite(v)) v = 0.0;
        if (v >  1.0e12) v =  1.0e12;
        if (v < -1.0e12) v = -1.0e12;
        const long long t = std::llround(v * 10.0);
        const long long a = (t < 0) ? -t : t;
        return std::string(t < 0 ? "-" : "") + std::to_string(a / 10) + "."
             + std::to_string(a % 10);
    };
    return "margin_breach(" + tenths(edge_bps) + "/"
         + tenths(required_edge_bps) + "bps)";
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_CROSS_GUARD_HPP
