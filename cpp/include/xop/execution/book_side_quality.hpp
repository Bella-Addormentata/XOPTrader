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

/// WHY `ref` IS ZERO -- A SENTINEL THAT USED TO MEAN TWO THINGS.
///
/// [REGRESSION, INTRODUCED ON THIS BRANCH BY b45c30f, FIXED 2026-09-02]
///
/// b45c30f made the per-side verdicts gate valuation trust, which was
/// correct: a poisoned 3.24975 mid was reaching the P&L callback and the
/// drawdown breaker.  Its witness that a classification had actually
/// happened was `ref > 0.0` (market_data.cpp, `sides_examined`).  But
/// classify_sides returns ref = 0 from ONE early exit reached for TWO
/// unrelated reasons, and only one of them is evidence of anything:
///
///   NoAnchor      No usable independent anchor this cycle.  A genuine
///                 DATA GAP.  The book is unscreened and unscreenable, so
///                 withholding valuation grade is CORRECT and is the whole
///                 point of b45c30f.
///   BandDisabled  band_ratio <= 1.0 -- the operator threw the DOCUMENTED
///                 off-switch (config.hpp:2063, "<= 1.0 disables"; the
///                 parser accepts it and config.cpp exempts it from the
///                 coherence warning).  Evidence was not gathered because
///                 it was not ASKED for.  That is not a data gap.
///
/// Conflating them meant setting the documented off-switch withheld
/// valuation grade on EVERY two-sided book, including a perfect one.
/// Measured on a clean book -- bid 1.4000 / ask 1.4200, 142 bps, both
/// ratios far inside any band, anchor 1.41022765 present:
///
///   band 3.0 -> ref=1.410228 examined=1 -> grade GRANTED
///   band 1.0 -> ref=0.000000 examined=0 -> grade WITHHELD  <- regression
///   band 0.0 -> ref=0.000000 examined=0 -> grade WITHHELD  <- regression
///
/// Valuation then fell to the S20 carry and, past
/// valuation_carry_ttl_blocks, to a DEGRADED cycle: an operator who turned
/// the band off watched the bot degrade for no visible reason.  Fail-CLOSED,
/// so nothing unsafe happened -- but a documented switch silently did
/// something other than what it documents.
///
/// WHY RESTORING GRADE HERE DOES NOT RE-OPEN b45c30f -- AND THE CONDITION IT
/// DEPENDS ON, WHICH AN EARLIER REVISION OF THIS COMMENT ASSERTED WAS FREE.
///
/// Turning the band off makes bid_ok/ask_ok trivially true, so `book_sides_ok`
/// stops carrying information.  The coherence test (`book_agrees_with_itself`)
/// is independent of the band and is then the ONLY screen left.  Measured on
/// the incident book itself, bid 1.5000 / ask 4.9995, at the default
/// agree_max of 5,000 bps:
///
///   band 3.0 -> spread 10,769 bps, sides_ok=0, agrees=0 -> WITHHELD
///   band 1.0 -> spread 10,769 bps, sides_ok=1, agrees=0 -> WITHHELD
///   band 0.0 -> spread 10,769 bps, sides_ok=1, agrees=0 -> WITHHELD
///
/// [review round 9] THAT TABLE IS TRUE AND ITS CONCLUSION WAS NOT.  This
/// comment used to end "So the off-switch costs the per-side band and nothing
/// else.  It cannot hand valuation grade to the dislocated book that started
/// all this."  Every row above holds agree_max at 5,000 and uses an UNCROSSED
/// book -- the one cell where the coherence conjunct actually bites.  It has
/// two documented stand-downs of its own, and in either of them the sentence
/// is false.  Both were measured on the real feed:
///
///   band 1.0, CROSSED bid 4.9995 / ask 1.5000, agree 5,000
///       -> compute_spread_bps returns 0 for a crossed book, 0 <= 5,000,
///          agrees=1 -> GRANTED, marking equity off mid 3.24975
///   band 1.0, uncrossed incident book, agree_max 0 ("bypass off")
///       -> the conjunct stands down, agrees=1 -> GRANTED, same 3.24975
///
/// Each stand-down is individually correct and justified, in prose, by the
/// sentence "the per-side band governs alone" (market_data.cpp, and the two
/// tests that pin them).  The band's own off-switch was justified by the
/// coherence test being independent.  Each guard was licensed by the other,
/// nothing checked that both were live, and that is the exact shape of the
/// documented close_out fail-open family.
///
/// So BandDisabled is NOT sufficient on its own.  `book_was_examined` takes a
/// second input -- whether the coherence test can still bite THIS book -- and
/// the off-switch restores grade only while some screen remains standing.
///
/// `ref` KEEPS ITS OLD MEANING and is deliberately NOT set in the
/// BandDisabled case: it is a PRICE consumers substitute for a junk side,
/// and nothing was screened, so there is no price to substitute.  This enum
/// is a separate channel precisely so that no consumer of `ref` changes
/// behaviour -- see step8_references, which reads `ref` only under
/// `!bid_ok`/`!ask_ok`, both unreachable when the band is off.
enum class ScreenOutcome {
    /// MUST BE THE ZERO ENUMERATOR AND MUST STAY FIRST.  This is the
    /// withholding value, so zero-initialisation -- `ScreenOutcome{}`, a
    /// value-initialised aggregate, a memset struct -- lands fail-CLOSED.
    /// Reordering these makes silence mean "the operator opted out", which
    /// is the fail-open direction and re-opens b45c30f.
    NoAnchor = 0,
    /// The operator disabled the per-side band.  Not a data gap.
    BandDisabled,
    /// classify_sides ran the band against a real anchor; `ref` holds it.
    Screened,
};

/// Can the valuation gate's coherence conjunct still REFUSE a book, or is it
/// standing down?  `book_agrees_with_itself` is
///
///     agree_max_bps <= 0.0 || book_spread_bps <= agree_max_bps
///
/// which is capable of returning false only when BOTH inputs are positive:
///
///   * agree_max_bps <= 0  is the documented "bypass off" setting (and the
///     value effective_agree_max_spread_bps returns for an unusable gate
///     knob).  The conjunct is switched off and passes every book.
///   * book_spread_bps == 0 is compute_spread_bps's convention for a CROSSED
///     or one-sided book.  0 satisfies any positive ceiling, so the conjunct
///     passes every crossed book.  It is not blind by accident: crossed books
///     are normal on Dexie and an unmeasurable spread is deliberately not
///     treated as a contradiction.
///
/// Neither is a bug on its own -- each is a documented, tested escape, and
/// while the per-side band is live each is safe.  This predicate exists so a
/// caller can ask whether the OTHER guard is still standing before leaning on
/// it, which is the check whose absence made both escapes fatal together.
///
/// NaN lands on false (NaN > 0.0 is false), i.e. it withholds.
[[nodiscard]] constexpr bool coherence_can_bite(
    double agree_max_bps,
    double book_spread_bps) noexcept
{
    return agree_max_bps > 0.0 && book_spread_bps > 0.0;
}

/// Did a classification actually have the chance to run, and is this book
/// still screened by SOMETHING?  The valuation gate's `sides_examined`
/// witness, decided HERE rather than by re-deriving band_ratio at the call
/// site -- nothing in cpp/tests constructs an Engine (TODO S36), so a
/// decision made in this header is testable and one made in market_data.cpp
/// is not.  That is exactly how b45c30f shipped.
///
/// @param coherence_live  Whether `book_agrees_with_itself` can still refuse
///                        THIS book -- see coherence_can_bite.  Only consulted
///                        for BandDisabled, where it is the last guard left.
///
/// WHITELIST, NOT `!= NoAnchor`.  A future enumerator must default to
/// WITHHOLDING.  `!= NoAnchor` would silently grant grade to any state added
/// later; this refuses it until someone names it here on purpose.
///
/// NO DEFAULT ARGUMENT ON `coherence_live`, DELIBERATELY.  A default would
/// have to be `true` to be ergonomic, and `true` is the fail-OPEN value; a
/// future call site that forgot the argument would silently reinstate exactly
/// the two fail-opens round 9 closed.  Every caller states it.
[[nodiscard]] constexpr bool book_was_examined(ScreenOutcome o,
                                               bool coherence_live) noexcept {
    // Screened: the band actually ran against a real anchor. That IS the
    // screen, so it stands whatever the coherence test is doing -- this is
    // what keeps the two documented escapes working while the band is on.
    return o == ScreenOutcome::Screened
        // BandDisabled: the operator withdrew the band. An opt-out is not a
        // data gap, so grade is restored -- but only while a screen remains.
        || (o == ScreenOutcome::BandDisabled && coherence_live);
}

static_assert(ScreenOutcome{} == ScreenOutcome::NoAnchor,
              "zero-initialisation must mean NoAnchor");
static_assert(!book_was_examined(ScreenOutcome{}, true),
              "the default outcome must WITHHOLD valuation grade");
static_assert(!book_was_examined(ScreenOutcome::NoAnchor, true),
              "a genuine data gap must still withhold EVEN WITH the coherence "
              "test live -- this is what b45c30f closed and what must not "
              "re-open. The second argument must never rescue NoAnchor");
static_assert(book_was_examined(ScreenOutcome::BandDisabled, true),
              "the documented off-switch must not withhold while the "
              "coherence test can still refuse the book");
static_assert(!book_was_examined(ScreenOutcome::BandDisabled, false),
              "band off AND coherence standing down leaves NO screen at all; "
              "granting grade there re-opens b45c30f on a crossed book or at "
              "agree_max 0");
static_assert(book_was_examined(ScreenOutcome::Screened, false),
              "a book the band actually screened is examined regardless of "
              "the coherence test -- otherwise crossed books and the "
              "documented agree_max 0 setting black out valuation bot-wide");
static_assert(book_was_examined(ScreenOutcome::Screened, true), "");

static_assert(coherence_can_bite(5000.0, 141.8),
              "a live ceiling and a measurable spread: the conjunct bites");
static_assert(!coherence_can_bite(0.0, 141.8),
              "agree_max 0 is the documented bypass-off: it stands down");
static_assert(!coherence_can_bite(5000.0, 0.0),
              "spread 0 is compute_spread_bps's crossed/one-sided sentinel: "
              "0 satisfies every ceiling, so the conjunct cannot refuse");

/// Verdict for one dust-filtered book.  Both sides default to trusted:
/// absence of evidence against a side is not evidence against it, and the
/// no-anchor case must leave every consumer exactly where it was.
///
/// The "trusted" states are distinguishable, and so are the two reasons a
/// book went unexamined.  There are THREE ways to be trusted, not two:
///
///   trusted because VERIFIED    outcome == Screened, ref > 0,
///                               bypassed == false -- both sides were
///                               individually compared to the anchor.
///   trusted because COHERENT    outcome == Screened, ref > 0,
///                               bypassed == true -- the two-sides-agree
///                               escape fired and returned BEFORE in_band
///                               ever ran.  The anchor was accepted, so this
///                               is a screened book, but the trust came from
///                               the book agreeing with ITSELF, not from any
///                               comparison against the anchor.
///   trusted because UNEXAMINED  ref == 0, and `outcome` says WHY --
///                               NoAnchor (no evidence available) or
///                               BandDisabled (no evidence requested).
///
/// An earlier revision of this comment claimed only the first distinction
/// and asserted consumers could tell what they needed to.  They could not:
/// the "why" is what the valuation gate turns on, and it was not recorded.
/// A later revision listed two trusted states where there are three, reading
/// `Screened` as "both sides were checked against the anchor" -- which is not
/// what a bypassed book got.  The information was always recoverable from
/// `bypassed`; the comment just did not say so.
struct SideQuality {
    bool   bid_ok{true};
    bool   ask_ok{true};
    double ref{0.0};        ///< anchor actually used; 0 = nothing screened
    bool   bypassed{false}; ///< the two-sides-agree escape fired
    /// Why `ref` is what it is.  DEFAULT IS THE FAIL-CLOSED ONE: a
    /// default-constructed SideQuality is "nothing screened", and nothing
    /// screened must never read as "the operator opted out".
    ScreenOutcome outcome{ScreenOutcome::NoAnchor};
};

static_assert(SideQuality{}.outcome == ScreenOutcome::NoAnchor,
              "SideQuality{} is used as 'nothing screened' -- it must "
              "behave as NoAnchor, never as BandDisabled");
// Asserted with coherence_live TRUE -- the permissive argument -- so this
// pins that the DEFAULT withholds on its own merits and is not merely being
// rescued by a coincidentally standing-down coherence test.
static_assert(!book_was_examined(SideQuality{}.outcome, true),
              "a default-constructed SideQuality must WITHHOLD");
static_assert(SideQuality{}.ref == 0.0, "");

/// Classify a filtered book against an independent anchor.
///
/// @param best_bid  Best dust-filtered third-party bid, quote per base.
///                  <= 0 means "no third-party offer on this side".
/// @param best_ask  Best dust-filtered third-party ask, same units.
/// @param anchor    Independent reference (NEVER this pair's own book).
///                  <= 0 or non-finite means nothing screened this cycle.
/// @param band_ratio            Multiplicative band; finite and <= 1.0
///                  disables.  A disabled band yields outcome BandDisabled,
///                  NOT NoAnchor: it is an opt-out, not a data gap.  A
///                  NON-FINITE or negative band is garbage rather than a
///                  setting and degrades to NoAnchor -- on EITHER side of 1.0,
///                  which is why that test is hoisted above the comparison.
/// @param agree_max_spread_bps  Two-sides-agree bypass threshold.
///
/// On return, `outcome` always says why `ref` holds what it does, and it is
/// the ONLY field that distinguishes an operator opt-out from a data gap.
///
/// BandDisabled is NOT by itself a licence to grade: see book_was_examined,
/// which additionally requires that the coherence test can still refuse the
/// book.  This function reports WHAT HAPPENED; it does not decide trust.
[[nodiscard]] inline SideQuality classify_sides(
    double best_bid,
    double best_ask,
    double anchor,
    double band_ratio,
    double agree_max_spread_bps) noexcept
{
    SideQuality q{};

    // No usable anchor, or the test is switched off: nothing may be
    // disqualified.  ref stays 0 in BOTH cases so consumers can tell
    // "trusted because verified" from "trusted because unexamined" -- they
    // are not the same claim, and a consumer that re-references on a junk
    // side must not silently re-reference onto a zero.
    //
    // These were ONE early exit until 2026-09-02.  They are now two,
    // because `q.outcome` has to say WHICH -- see the ScreenOutcome note.
    //
    // ORDER IS LOAD-BEARING AND MUST STAY THIS WAY.  When BOTH hold (no
    // anchor AND the band disabled) the answer must be NoAnchor: a missing
    // anchor is a data gap whatever the band says, and reporting
    // BandDisabled there would let an operator's off-switch grant valuation
    // grade to a book nothing screened.  That is the fail-open direction and
    // it is precisely what b45c30f closed.  Anchor first, therefore.
    if (!(anchor > 0.0) || !std::isfinite(anchor)) {
        q.outcome = ScreenOutcome::NoAnchor;
        return q;
    }
    // A NON-FINITE BAND IS GARBAGE WHICHEVER SIDE OF 1.0 IT FALLS, and this
    // test is hoisted ABOVE the `> 1.0` comparison for that reason.
    //
    // [review round 9] It used to sit inside the disabled branch below, so it
    // only caught garbage that compared <= 1.0.  NaN and -inf land there and
    // were handled; +inf does NOT -- `!(inf > 1.0)` is false, so it fell
    // through to the band as though it were a setting, reported Screened with
    // `ref` set, and then screened NOTHING: in_band's `ratio <= inf` and
    // `ratio >= 1.0/inf == 0.0` both hold for every price.  That is the
    // maximally permissive outcome wearing the label of the verified one --
    // a book arbitrarily far from the anchor reported as positively screened.
    //
    // config.cpp:3181 throws on non-finite or negative, so no config can
    // reach this today.  It is written anyway, and written to fail CLOSED,
    // because "unreachable today" is how each of the twelve close_out
    // fail-opens started.
    if (!std::isfinite(band_ratio) || band_ratio < 0.0) {
        q.outcome = ScreenOutcome::NoAnchor;
        return q;
    }
    if (!(band_ratio > 1.0)) {
        // Everything reaching here is finite and >= 0, so this IS the range
        // config.cpp accepts and documents as "off": [0.0, 1.0].  A genuine
        // operator opt-out, not garbage.
        q.outcome = ScreenOutcome::BandDisabled;
        return q;
    }
    q.outcome = ScreenOutcome::Screened;
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
