// =============================================================================
//  orderbook_mid.hpp -- order-book mid-price estimator
// =============================================================================
//
//  A single function, `compute_orderbook_mid`, that turns a set of resting
//  third-party offers into one number: where the market is.  It is separated
//  from MarketDataFeed so it can be tested directly against adversarial books
//  without standing up a feed, a config and a lock hierarchy.
//
//  WHY THIS FILE EXISTS AT ALL
//  ---------------------------
//  The original estimator claimed to be a Stoikov micro-price.  It was not.
//  It read:
//
//      mid = (ask_depth * bid_vwap + bid_depth * ask_vwap)
//            / (bid_depth + ask_depth)              <-- WRONG, see below
//
//  Each side's price is weighted by the OPPOSITE side's depth, so the estimate
//  leans toward the THIN side -- the side that will move first.  That instinct
//  is right and is why the formula is kept here at all.
//
//  THE DEFECT WAS THE PRICE, NOT THE WEIGHTING.  Stoikov weights the TOUCH
//  PRICES:
//
//      micro = (Q_ask * best_bid + Q_bid * best_ask) / (Q_bid + Q_ask)
//
//  which is a CONVEX COMBINATION of best_bid and best_ask, and is therefore
//  bounded in [best_bid, best_ask] for EVERY depth ratio and EVERY spread.
//  Boundedness is not an extra property to be enforced afterwards; it is
//  intrinsic to the formula.
//
//  Substituting each side's top-N VWAP for its touch price threw that away.
//  Each side's VWAP runs over the top N levels, so
//
//      bid_vwap <= best_bid   and   ask_vwap >= best_ask   by construction
//
//  and a convex combination of bid_vwap and ask_vwap is bounded in
//  [bid_vwap, ask_vwap] -- an interval that STRICTLY CONTAINS the book.  So
//  the estimate could, and routinely did, leave the book entirely:
//
//      bid_depth >> ask_depth   =>   mid -> ask_vwap   >=  best_ask
//
//  DO NOT RE-DERIVE THE OLD CONCLUSION FROM THAT PREMISE.  The premise
//  "bid_vwap <= best_bid, ask_vwap >= best_ask" is correct, but the remedy is
//  NOT "add a clamp and taper the weighting off as the spread widens" -- it is
//  "the estimator is unbounded; fix the estimator".  With d_b = best_bid -
//  bid_vwap, d_a = ask_vwap - best_ask and S = the spread, the old form left
//  the book below the bid whenever
//
//      ask_depth / bid_depth  >  (S + d_a) / d_b
//
//  in which S is ADDITIVE IN THE NUMERATOR -- so a NARROWER spread made the
//  violation EASIER.  A spread taper is therefore the wrong instrument
//  entirely: it disengages precisely where the defect is worst.  Live XCH/DBX
//  over ~39h bore that out exactly -- 1,849 clamp firings, binding on 46.9%
//  of ingests, 98.38% below the bid, and the taper fully disengaged at 95.3%
//  of them.  Corrected 2026-09-02 by restoring the touch-price form.
//
//  WHAT TO EXPECT AFTER THE CORRECTION -- AND WHAT NOT TO
//  ------------------------------------------------------
//  Two acceptance numbers, one sound and one that was wrong in the original
//  write-up and is corrected here so nobody measures against it again.
//
//    SOUND:  clamp firings go to ZERO.  The micro-price and the midpoint it
//            blends with are both convex combinations of best_bid and
//            best_ask, so Layer 1 has nothing to do, analytically, ever.
//
//    WRONG:  "the ladder centre moves by spread/2".  It does not.  spread/2
//            assumes the corrected centre lands on the MIDPOINT.  It lands on
//            the MICRO-PRICE, which leans to the thin side.  With
//            R = ask_depth / bid_depth and w_micro = 1,
//
//                micro = (R*B + A) / (1 + R) = B + S/(1 + R)
//
//            so the shift off a formerly-clamped-to-bid centre is S/(1+R),
//            NOT S/2, and it DECAYS TO ZERO exactly in the deep-asymmetry
//            cases where the old clamp bound hardest.  On the live modal
//            XCH/DBX ladder a below-bid escape required R > 2.00, which caps
//            the entire below-bid population (98.38% of firings) at S/3 --
//            ~2.7 bps against the modal 8 bps spread, ~2.1 bps at the
//            observed p50 R = 2.85, 0.73 bps at R = 10, 0.008 bps at
//            R = 1000.  The maximum shift is attained AT the clamp
//            threshold, not beyond it.
//
//            Direction also reverses for the ~1.62% that clamped ABOVE the
//            ask (R < ~0.4995): there the centre FALLS.  An operator holding
//            "4.0 bps p50" as the acceptance number will see ~2 bps and
//            wrongly conclude the fix did not deploy.  See
//            OrderbookMidAcceptance.* in test_orderbook_mid.cpp, which pins
//            S/(1+R) against the estimator itself.
//
//  The case that first exposed it, BYC/wUSDC.b (~65 deep bids against ~9 thin
//  asks), block 9087661, 2026-08-01 10:30:59 UTC:
//
//      third-party book   bid 1.009156   ask 1.144728   (1259 bps wide)
//      plain midpoint     1.076942
//      published "mid"    1.144728       == the best ask, exactly
//
//  A mid sitting exactly on the best ask is not a price estimate; it is the
//  worst offer anyone is showing.  BYC's true value was ~$1.01, corroborated
//  five independent ways.  That inflated mark propagated into the XCH/BYC
//  cross and six ask tiers were lifted 1.2704-1.2979 against a true 1.4143.
//
//  THREE LAYERS, SMALLEST FIRST
//  ----------------------------
//  1. INVARIANT.  best_bid <= mid <= best_ask whenever both sides exist.
//     Non-negotiable, enforced last, logged at warning level when it binds.
//     Since the 2026-09-02 correction this clamp is a BACKSTOP THAT SHOULD
//     NEVER BIND: the touch-price micro-price and the midpoint it blends with
//     are both interior by construction, so the invariant holds analytically.
//     A binding clamp is now unambiguously a regression signal.
//
//  2. DEGRADE WITH WIDTH.  The micro-price answers "which side is thinner",
//     and that question stops carrying information about fair value as the
//     spread grows: on a 1259 bps book, depth asymmetry says essentially
//     nothing about where the next print lands.  So blend continuously from
//     the micro-price toward the plain BBO midpoint as relative spread rises.
//     NOTE this taper is a statement about the INFORMATION CONTENT of depth
//     asymmetry on a wide book.  It is NOT, and never was, a boundedness
//     mechanism -- see above for why it could not be one.
//
//  3. DEGENERATE AND ONE-SIDED BOOKS.  bid == ask, a missing side, zero or
//     negative depth.  See `OrderbookMid::mid` for exactly what each returns
//     and why.  The governing rule: prefer reporting NO mid over reporting a
//     fabricated one.
//
// =============================================================================

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "xop/types.hpp"

namespace xop {

// ---------------------------------------------------------------------------
// OrderbookMidParams -- the Layer 2 blend schedule.
//
// w_micro falls linearly from 1 to 0 as the relative spread travels from
// `narrow_bps` to `wide_bps`:
//
//     w_micro = clamp(1 - (spread_bps - narrow_bps)
//                         / (wide_bps - narrow_bps), 0, 1)
//     mid     = w_micro * microprice + (1 - w_micro) * midpoint
//
// Defaults are justified against MEASURED per-pair spread distributions --
// see StrategyConfig::microprice_narrow_bps for the numbers.  They must work
// unedited: an operator who never touches config.yaml gets the fix.
// ---------------------------------------------------------------------------
struct OrderbookMidParams {
    /// Levels per side included in each side's VWAP.
    std::size_t depth{5};

    /// At or below this relative spread (bps) the micro-price is used whole.
    double narrow_bps{200.0};

    /// At or above this relative spread (bps) the micro-price is discarded
    /// entirely and the plain BBO midpoint is used.
    double wide_bps{800.0};

    /// Mojos per whole unit of the pair's BASE asset (XCH = 1e12, CAT = 1e3)
    /// and QUOTE asset.  CompetingOffer::size is denominated in the OFFERED
    /// asset -- base mojos on asks, quote mojos on bids -- so on an XCH/CAT
    /// pair the two sides' raw sizes differ by ~1e9 and cannot be compared
    /// directly.  compute_orderbook_mid() uses these to value both sides'
    /// depths in a common numeraire (quote units) before the micro-price
    /// depth weighting; see the dimensional analysis at the normalization
    /// site.  Non-positive values fall back to kMojosPerXch.
    std::int64_t base_mojos_per_unit{kMojosPerXch};
    std::int64_t quote_mojos_per_unit{kMojosPerXch};
};

// ---------------------------------------------------------------------------
// OrderbookMid -- the estimate plus everything needed to audit it.
// ---------------------------------------------------------------------------
struct OrderbookMid {
    /// The mid, or 0.0 meaning NO USABLE MID.  Zero is returned when:
    ///   - there are no usable offers at all; or
    ///   - only one side of the book exists.
    ///
    /// The one-sided case is a deliberate refusal, not an oversight.  A lone
    /// resting bid at 1.00 bounds fair value from below and says nothing else;
    /// publishing 1.00 as "the mid" asserts a location the observation does
    /// not contain.  That fabrication is how self-referential pricing crept in
    /// before -- when every offer on a side was OURS, the filtered side went
    /// empty, the surviving side became "the market", and the bot quoted
    /// against its own print (fixed for the BBO in 5e1ceb4; this closes the
    /// same door for the order-book mid).  Callers that genuinely want a
    /// one-sided bound can read best_bid / best_ask, which are always
    /// populated with whatever the book actually showed.
    double mid{0.0};

    /// Best prices actually observed.  0.0 means that side had no usable
    /// offer.  Populated even when `mid` is 0.
    double best_bid{0.0};
    double best_ask{0.0};

    /// The Stoikov micro-price before Layer 2 blending and Layer 1 clamping:
    /// (ask_depth * best_bid + bid_depth * best_ask) / (bid_depth+ask_depth).
    /// 0.0 if it could not be formed.  Diagnostic only.
    ///
    /// Being a convex combination of best_bid and best_ask, this is ALWAYS in
    /// [best_bid, best_ask].  Before 2026-09-02 it used each side's top-N
    /// VWAP in place of its touch price and was unbounded; tests that assert
    /// `microprice` outside the book are asserting the old defect.
    double microprice{0.0};

    /// The plain (best_bid + best_ask) / 2.  0.0 if not two-sided.
    double midpoint{0.0};

    /// Relative spread, (ask - bid) / midpoint * 10000.  Negative on a
    /// crossed book.  0.0 if not two-sided.
    double spread_bps{0.0};

    /// Layer 2 weight actually applied to `microprice`, in [0, 1].
    double w_micro{0.0};

    /// Layer 1 fired: the blended estimate fell outside [best_bid, best_ask]
    /// and was clamped back in.  Always a signal that the weighting misbehaved.
    bool clamped{false};

    /// Exactly one side of the book existed.  `mid` is 0 in this case.
    bool one_sided{false};

    /// best_ask <= best_bid.  Dexie has no matching engine, so a touching or
    /// crossed book is normal, not corrupt -- 14 of 44 XCH/BYC observations in
    /// the week to 2026-08-01 had bid == ask.  Depth weighting is meaningless
    /// there (there is no interior to lean within), so the midpoint is used
    /// and w_micro is 0.  When bid == ask the midpoint IS that price, and the
    /// invariant holds trivially.
    bool degenerate{false};

    /// Number of usable levels that survived validation, per side.
    std::size_t bid_levels{0};
    std::size_t ask_levels{0};
};

// ---------------------------------------------------------------------------
// compute_orderbook_mid -- Layers 1-3, applied in that order.
//
// `pair_name` is used only for logging; pass "" to stay quiet about identity.
// Offers with a non-finite or non-positive price, or a non-positive size, are
// dropped before anything is computed: a zero-depth side cannot be weighted,
// and a negative one is not a book.
// ---------------------------------------------------------------------------
OrderbookMid compute_orderbook_mid(const std::vector<CompetingOffer>& offers,
                                   const OrderbookMidParams&          params,
                                   const std::string& pair_name = {});

}  // namespace xop
