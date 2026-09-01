#ifndef XOP_EXECUTION_BOOK_SIDE_QUALITY_HPP
#define XOP_EXECUTION_BOOK_SIDE_QUALITY_HPP
// ---------------------------------------------------------------------------
// book_side_quality.hpp -- per-SIDE agreement with an independent anchor.
//
// [SIDEQUALITY 2026-09-01]
//
// WHY THIS EXISTS
// ---------------
// Every book consumer in this tree read best_bid and best_ask as an atomic
// pair, so none of them could express the state XCH/BYC was actually in:
//
//   price is BYC per XCH, anchor (solved, no book input) = 1.41022765
//     bid stack   1.5000  1.4283  1.4066  1.3793  1.3699  1.3514
//     ask stack   4.9995  5.0000  9.7500 10.0000 10.0000 10.0000
//
// Every bid is within 6.4% of the anchor.  Every ask is 3.5x to 7.1x it.
// The 10,769 bps "spread" that produces is not a spread -- it is an ABSENT
// ask side wearing the costume of one.  Three consumers then did the wrong
// thing with it, all correctly, all from the same bad premise:
//
//   * the model-mid sanity check measured a correct 1.41141912 against
//     bbo_mid 3.24975 (the mean of an honest bid and a junk ask), got
//     56.6% deviation, and suppressed EVERY tier -- the pair went silent
//     precisely BECAUSE the solver was right;
//   * the per-tier check measured our correctly-priced ask against
//     best_ask 4.9995 and called it 71.8% "aggressive";
//   * the competitive anchor capped bids at that same poisoned midpoint,
//     parking a bid at 1.5001 against a 1.4102 fair value.
//
// WHAT THIS DOES NOT DO
// ---------------------
// It does not remove offers.  That was the tempting version and it is
// wrong: stripping the ask side takes dex_best_ask to 0, compute_mid
// Case 2 refuses to publish a lone bid as a mid, Case 3 needs a fresh
// last trade a silent pair does not have, so the pair publishes NO mid,
// Step 4 marks the quote invalid, and the correct 1.41 never reaches the
// ladder.  Silence instead of a mispriced quote is not an improvement
// when the alternative is quoting correctly.
//
// So the book stays intact and each consumer decides what to reference.
// Flagging is the reversible half of the idea; removal is not.
//
// THE BYPASS IS THE IMPORTANT PART
// --------------------------------
// A genuine market-wide repricing moves BOTH sides together and leaves
// the book internally coherent.  That is the same evidence
// mid_gate::book_confirms() accepts to override an anchor breach, and
// disqualifying sides in that state would strip the confirmation before
// the gate could read it -- the two halves of one feature contradicting
// each other, which is the defect class mid_gate.hpp's
// offer_absurdity_ratio comment was written about.  Hence: a two-sided
// book whose own spread is tight is trusted WHOLE, however far it sits
// from the anchor.  Dislocation is one side moving ALONE, and that always
// leaves a wide spread behind.
//
// "Tight" is DERIVED from the gate's own confirmation threshold rather
// than merely defaulted to it -- see effective_agree_max_spread_bps.  An
// earlier revision of this comment claimed the two were "pinned to the
// same default", which was true and insufficient: a default they share is
// still a default an operator can move, and moving this one below the
// gate's threshold recreates exactly the contradiction described above.
//
// Pure header, no feed or engine types, so the branches are driven
// directly by cpp/tests/test_book_side_quality.cpp rather than through a
// MarketDataFeed.
// ---------------------------------------------------------------------------

#include <cmath>

namespace xop::bookside {

// The bypass threshold this classifier must ACTUALLY use, DERIVED from the
// published-mid gate's own confirmation threshold so the two can never be
// configured into conflict.  Same shape, and for the same reason, as
// mid_gate::offer_absurdity_ratio.
//
// [review, PR #134] The header above says the bypass and
// mid_gate::book_confirms() are "pinned to the same default".  Sharing a
// DEFAULT is not the same as being unable to disagree, and an operator can
// set them apart.  The reviewer's case: leave the gate confirming at
// 5000 bps and set this to 750.  A far-from-anchor book at 4000 bps is then
// accepted by book_confirms() as "the whole market repriced" -- while this
// classifier disqualifies BOTH its sides, because 4000 > 750.  Step 8 then
// takes its both-sides-disqualified path instead of honouring the
// confirmation, which is precisely the contradiction the header promises
// cannot happen.
//
// The constraint is one-directional.  A bypass MORE permissive than the
// gate is harmless: it trusts a book whole that the gate would also have
// accepted.  A bypass LESS permissive strips the evidence the gate is
// waiting for.  So the effective value is the larger of the two, and an
// operator who lowers this knob below the gate's threshold gets the gate's
// threshold rather than a silent contradiction.
//
// Non-finite or negative inputs contribute nothing rather than poisoning
// the result; if both are unusable the bypass is simply off, which is the
// safe direction (it only ever ADDS trust).
[[nodiscard]] inline double effective_agree_max_spread_bps(
    double configured,
    double gate_confirm_max_spread_bps) noexcept
{
    const double a = (std::isfinite(configured) && configured > 0.0)
                         ? configured : 0.0;
    const double b = (std::isfinite(gate_confirm_max_spread_bps)
                      && gate_confirm_max_spread_bps > 0.0)
                         ? gate_confirm_max_spread_bps : 0.0;
    return a > b ? a : b;
}

/// Verdict for one dust-filtered book.  Both sides default to trusted:
/// absence of evidence against a side is not evidence against it, and the
/// no-anchor case must leave every consumer exactly where it was.
struct SideQuality {
    bool   bid_ok{true};
    bool   ask_ok{true};
    double ref{0.0};        ///< anchor actually used; 0 = nothing screened
    bool   bypassed{false}; ///< the two-sides-agree escape fired
};

/// Classify a filtered book against an independent anchor.
///
/// @param best_bid  Best dust-filtered third-party bid, quote per base.
///                  <= 0 means "no third-party offer on this side".
/// @param best_ask  Best dust-filtered third-party ask, same units.
/// @param anchor    Independent reference (NEVER this pair's own book).
///                  <= 0 or non-finite means nothing screened this cycle.
/// @param band_ratio            Multiplicative band; <= 1.0 disables.
/// @param agree_max_spread_bps  Two-sides-agree bypass threshold.
[[nodiscard]] inline SideQuality classify_sides(
    double best_bid,
    double best_ask,
    double anchor,
    double band_ratio,
    double agree_max_spread_bps) noexcept
{
    SideQuality q{};

    // No usable anchor, or the test is switched off: nothing may be
    // disqualified.  ref stays 0 so consumers can tell "trusted because
    // verified" from "trusted because unexamined" -- they are not the
    // same claim, and a consumer that re-references on a junk side must
    // not silently re-reference onto a zero.
    if (!(anchor > 0.0) || !std::isfinite(anchor) || !(band_ratio > 1.0)) {
        return q;
    }
    q.ref = anchor;

    // The bypass, tested BEFORE the per-side band so a coherent book can
    // never be dismantled by it.  Requires a genuinely two-sided,
    // uncrossed book: a crossed book (ask <= bid) is not a market that
    // agrees with itself, and a one-sided one has no spread to measure --
    // both fall through to the per-side test, matching
    // compute_spread_bps's convention that 0 means one-sided or crossed.
    if (best_bid > 0.0 && best_ask > best_bid
        && std::isfinite(best_bid) && std::isfinite(best_ask)
        && agree_max_spread_bps > 0.0) {
        const double mid = (best_bid + best_ask) / 2.0;
        const double spread_bps = (best_ask - best_bid) / mid * 10000.0;
        if (spread_bps <= agree_max_spread_bps) {
            q.bypassed = true;
            return q;
        }
    }

    // Per-side band.  An ABSENT side (<= 0) stays ok: "no third-party
    // offer here" is already reported as 0 and every consumer handles it,
    // and marking it not-ok would conflate two different failures.
    auto in_band = [&](double px) noexcept {
        if (!(px > 0.0) || !std::isfinite(px)) return true;
        const double ratio = px / anchor;
        return ratio <= band_ratio && ratio >= 1.0 / band_ratio;
    };
    q.bid_ok = in_band(best_bid);
    q.ask_ok = in_band(best_ask);
    return q;
}

}  // namespace xop::bookside

#endif  // XOP_EXECUTION_BOOK_SIDE_QUALITY_HPP
