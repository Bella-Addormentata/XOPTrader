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
//   * [CORRECTED in review round 5] the model-mid sanity check was
//     originally described here as measuring 1.41141912 against bbo_mid
//     3.24975 for 56.6%. It does not: its `mid` is the PUBLISHED mid,
//     which for a pair with no CEX or AMM leg IS the BBO midpoint, so its
//     deviation is identically zero and it never fired. That check is now
//     SKIPPED on a disqualified side rather than re-pointed -- re-pointing
//     it at the surviving side produced 116.7% and cleared every tier.
//     Both its operands come from the book being judged, so no
//     substitution makes it meaningful;
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
// still a default an operator can move.
//
// [review round 6] The derivation used to take the LARGER of the two, to
// guarantee the bypass could never be stricter than the gate.  That was
// backwards and is now max()-> min().  A bypass WIDER than the gate does
// not "trust a book the gate would also have accepted" -- above the gate's
// threshold the gate accepts nothing, so the extra width is trust the gate
// never granted.  The cost of the safe direction is stated honestly at
// effective_agree_max_spread_bps: a bypass stricter than the gate CAN
// disqualify both sides of a book the gate confirmed, and Step 8 then
// takes its both-sides-disqualified path.  That is quoting conservatively
// through a genuine repricing -- a fail-CLOSED cost -- and this repo trades
// that for a fail-open risk in exactly this direction, never the reverse.
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
// WHICH DIRECTION IS PERMISSIVE.  READ THIS BEFORE CHANGING THE OPERATOR.
// ----------------------------------------------------------------------
// Both knobs are CEILINGS ON AN ACCEPT CONDITION, applied to the SAME
// scalar computed by the SAME formula on the SAME two prices:
//
//   here                    spread_bps <= agree_max_spread_bps
//                             -> bypassed, book trusted WHOLE
//   mid_gate::book_confirms  book_spread_bps <= book_confirm_max_spread_bps
//                             -> book accepted as confirmation
//
// Raising EITHER admits strictly more books.  So the more permissive of
// the two is the LARGER, and the effective value must be the SMALLER.
//
// [review round 6, PR #134] This returned max() through five review
// rounds, justified by: "A bypass MORE permissive than the gate is
// harmless: it trusts a book whole that the gate would also have
// accepted."  There is no "also".  Above the gate's threshold the gate
// accepts NOTHING, so every basis point of extra width is trust nothing
// granted.  Worked case, configured 8000 / gate 5000, anchor 1.41022765,
// both bands 3.0:
//
//   bid 2.764048  ask 5.133232  bbo_mid 3.948640   spread 6000.0 bps
//     mid/anchor 2.80  -> INSIDE the 3x band, so gate_mid returns Accept
//                         at its early exit and never calls book_confirms
//     ask/anchor 3.64  -> OUT of band, and must be disqualified
//   max(): 6000 <= 8000 -> bypass fires -> ask_ok = true.  Step 8 runs
//     Check 1, measures ask tiers against the junk 5.133232 touch, and the
//     liquidity bid cap is pinned to the poisoned 3.948640 midpoint.
//   min(): 6000 > 5000 -> no bypass -> ask_ok = false, tiers re-anchored.
//
// That is the poisoned-BBO behaviour this whole header exists to stop,
// re-enabled by a knob.  Note the second route in as well: LOWERING
// mid_gate_book_confirm_max_spread_bps and touching nothing else used to
// leave max(5000, gate) = 5000 -- the operator hardened the gate and
// silently widened the bypass past it.  Under min() that edit now tightens
// both, which is what "hardening the gate" is supposed to mean.
//
// WHAT min() COSTS, STATED RATHER THAN DENIED
// -------------------------------------------
// It re-opens the case max() was introduced to close: gate confirming at
// 5000, this knob at 750, and a far-from-anchor 4000 bps book.  The gate
// accepts it as "the whole market repriced" while this classifier
// disqualifies both sides, so Step 8 takes its both-sides-disqualified
// path on a book the gate just blessed.  Traced through: Check 1 is
// skipped (it never fires anyway -- both its operands are book-derived),
// both tier sides re-reference to the anchor, and the liquidity bid cap
// tightens.  The bot quotes conservatively, or not at all, through a
// genuine market-wide repricing.
//
// That is a FAIL-CLOSED cost.  max() bought it off with a FAIL-OPEN risk.
// This repo's documented rule (the close_out fail-open family, twelve of
// one shape) makes that trade backwards, so the cost is accepted here and
// the operator is told about it in config.hpp instead of being promised it
// cannot arise.
//
// SANITISING CONTRACT -- THE TWO OPERANDS ARE NOT INTERCHANGEABLE
// ---------------------------------------------------------------
// A non-finite or NEGATIVE input is not a setting -- it is garbage the
// parser already refuses at startup (config.cpp:3163 and :3186 both throw
// ConfigError), so every branch below is defence in depth.  It is still
// written to fail CLOSED, because "unreachable today" is how the twelve
// close_out fail-open bugs each started.
//
// [review round 7] An earlier revision returned "the other value governs
// alone" for BOTH single-garbage cases, describing each as contributing
// "no constraint".  That is true for only one of them, and under min() the
// other is the maximally fail-open branch there is:
//
//   * GATE side garbage (a_ok && !b_ok).  The gate's threshold is what
//     mid_gate::book_confirms compares against, and `spread <= NaN` is
//     false for EVERY book, as is `spread <= -1`.  So an unusable gate
//     confirms NOTHING -- and returning `configured` (say 8000) would make
//     the bypass infinitely wider than a gate that grants nothing, which
//     is exactly the "trust the gate never granted" shape the direction
//     argument above exists to close.  Under min() the neutral element is
//     +infinity, NOT zero, so the other operand is not neutral here.
//     A gate you cannot trust must not license trust: return 0 (bypass
//     off), matching the both-garbage branch.
//   * OPERATOR side garbage (b_ok && !a_ok).  Falling back to the gate's
//     own ceiling is genuinely neutral -- it makes the bypass exactly as
//     wide as the gate, never wider -- so the gate value governs alone.
//
// An explicit 0 is different from garbage: it is the documented "off"
// setting, and under min() it BINDS -- 0 on either knob disables the
// bypass entirely.  That asymmetry, plus the operand asymmetry above, is
// the whole reason the branches are written out instead of a bare
// std::min: no single std:: call implements this table.
[[nodiscard]] inline double effective_agree_max_spread_bps(
    double configured,
    double gate_confirm_max_spread_bps) noexcept
{
    const bool a_ok = std::isfinite(configured) && configured >= 0.0;
    const bool b_ok = std::isfinite(gate_confirm_max_spread_bps)
                   && gate_confirm_max_spread_bps >= 0.0;
    if (a_ok && b_ok) {
        return configured < gate_confirm_max_spread_bps
                   ? configured : gate_confirm_max_spread_bps;
    }
    // An unusable GATE confirms nothing, so the bypass may grant nothing.
    if (a_ok) return 0.0;
    // An unusable OPERATOR knob defers to the gate's own ceiling.
    if (b_ok) return gate_confirm_max_spread_bps;
    return 0.0;
}

/// What Step 8's two BBO sanity guards may reference, given a per-side
/// verdict.  Pure, so the branch table can be driven by a test instead of
/// by an Engine -- which nothing in cpp/tests constructs.
///
/// [review round 5] THIS EXISTS BECAUSE A REGRESSION SHIPPED HERE THROUGH
/// FOUR REVIEW ROUNDS AND 1000 GREEN TESTS.
///
/// Check 1 compares the PUBLISHED mid against the BBO midpoint.  For a
/// pair with no CEX or AMM leg those are the SAME NUMBER, so its deviation
/// is identically zero and it never fires.  An earlier revision re-pointed
/// it at the surviving side when the other was disqualified, on the false
/// premise that its input was Step 7's fair-value centre.  On the live
/// XCH/BYC book that turned 0% into |3.24975 - 1.5|/1.5 = 116.7% and
/// cleared every tier on every block.
///
/// The distinction that matters, and the reason these two guards must be
/// treated differently:
///
///   * CHECK 1 reads a BOOK-DERIVED AGGREGATE on both sides of the
///     comparison.  Both operands move together with the book, so no
///     substitution makes the deviation meaningful.  It can only be RUN
///     or SKIPPED.
///   * CHECK 2 reads OUR OWN TIER PRICE against a book reference.  The
///     tier price comes from Step 7's centre, so re-anchoring the
///     reference to that same centre compares like with like.  It CAN be
///     re-referenced, and should be.
struct Step8References {
    bool run_mid_check{true};   ///< Check 1: run it, or skip entirely.
    double bid_tier_ref{0.0};   ///< Check 2 reference for bid tiers.
    double ask_tier_ref{0.0};   ///< Check 2 reference for ask tiers.
    double effective_mid{0.0};  ///< midpoint classify_tier should use.
};

/// @param bid_ok/ask_ok  Per-side verdicts from classify_sides.
/// @param side_ref       book_side_ref -- the anchor the verdict used.
///                       0 when nothing screened the book.
/// @param bbo_mid        The BBO midpoint (Check 1's only valid reference).
/// @param best_bid/best_ask  Same-side touches, > 0.
[[nodiscard]] inline Step8References step8_references(
    bool   bid_ok,
    bool   ask_ok,
    double side_ref,
    double bbo_mid,
    double best_bid,
    double best_ask) noexcept
{
    Step8References r{};
    const bool healthy   = bid_ok && ask_ok;
    const bool has_anchor = side_ref > 0.0 && std::isfinite(side_ref);

    // Check 1 runs ONLY on a book whose sides both stand. Never re-pointed:
    // see the note above -- re-pointing is what produced the 116.7%.
    r.run_mid_check = healthy;

    // The midpoint classify_tier uses for its bid passive rule. A midpoint
    // built from a disqualified side would read any bid up to that poisoned
    // value as a safe passive rest.
    r.effective_mid = (healthy || !has_anchor) ? bbo_mid : side_ref;

    // Per-tier references: a tier whose OWN side is disqualified is judged
    // against the independent anchor instead.
    r.bid_tier_ref = (!bid_ok && has_anchor) ? side_ref : best_bid;
    r.ask_tier_ref = (!ask_ok && has_anchor) ? side_ref : best_ask;
    return r;
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
