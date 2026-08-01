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
//  The previous estimator was a Stoikov micro-price and nothing else:
//
//      mid = (ask_depth * bid_vwap + bid_depth * ask_vwap)
//            / (bid_depth + ask_depth)
//
//  Each side's VWAP is weighted by the OPPOSITE side's depth, so the estimate
//  leans toward the THIN side -- the side that will move first.  On a tight,
//  roughly two-sided book that is exactly right and it is why the formula is
//  kept here at all.
//
//  It is pathological on a wide, depth-asymmetric book.  Note that each side's
//  VWAP runs over the top N levels, so bid_vwap <= best_bid and
//  ask_vwap >= best_ask by construction.  Now let bid depth dominate:
//
//      bid_depth >> ask_depth   =>   mid -> ask_vwap   >=  best_ask
//
//  The estimate does not merely lean toward the ask, it leaves the book
//  entirely.  Measured on BYC/wUSDC.b (~65 deep bids against ~9 thin asks),
//  block 9087661, 2026-08-01 10:30:59 UTC:
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
//     Non-negotiable, enforced last, logged at warning level when it binds --
//     a binding clamp means the weighting produced a number outside the book
//     and we want to see every one of them.
//
//  2. DEGRADE WITH WIDTH.  The micro-price answers "which side is thinner",
//     and that question stops carrying information about fair value as the
//     spread grows: on a 1259 bps book, depth asymmetry says essentially
//     nothing about where the next print lands.  So blend continuously from
//     the micro-price toward the plain BBO midpoint as relative spread rises.
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

    /// The raw Stoikov micro-price before Layer 2 blending and Layer 1
    /// clamping.  0.0 if it could not be formed.  Diagnostic only.
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
