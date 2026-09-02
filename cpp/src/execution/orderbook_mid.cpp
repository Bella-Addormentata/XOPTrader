// =============================================================================
//  orderbook_mid.cpp -- order-book mid-price estimator (Layers 1-3)
// =============================================================================
//
//  See orderbook_mid.hpp for the full rationale.  In brief: a TRUE Stoikov
//  micro-price -- opposite-side depth weighting applied to the TOUCH PRICES --
//  is a convex combination of best_bid and best_ask and is therefore bounded
//  inside the book unconditionally.  What used to live here weighted each
//  side's top-N VWAP instead, and the VWAPs lie OUTSIDE the BBO, so the
//  estimate could leave the book on any depth-asymmetric ingest.  That was an
//  estimator defect, not an inherent property of the micro-price; it was
//  corrected on 2026-09-02.  Three layers, applied in order:
//
//    3. classify the book   (two-sided / one-sided / degenerate / empty)
//    2. blend micro-price -> midpoint as the spread widens
//    1. clamp into [best_bid, best_ask], loudly -- now a never-firing
//       backstop rather than a load-bearing correction
//
// =============================================================================

#include "xop/execution/orderbook_mid.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace xop {

namespace {

struct Level {
    double price;
    double size;
};

// VWAP over the top `n` levels.  Returns {vwap, total_size}; {0, 0} when the
// side is empty or carries no size.
std::pair<double, double> vwap_top_n(const std::vector<Level>& levels,
                                     std::size_t               n) {
    double num = 0.0, den = 0.0;
    const std::size_t limit = std::min(levels.size(), n);
    for (std::size_t i = 0; i < limit; ++i) {
        num += levels[i].price * levels[i].size;
        den += levels[i].size;
    }
    if (!(den > 0.0) || !std::isfinite(num) || !std::isfinite(den)) {
        return {0.0, 0.0};
    }
    return {num / den, den};
}

}  // namespace

OrderbookMid compute_orderbook_mid(const std::vector<CompetingOffer>& offers,
                                   const OrderbookMidParams&          params,
                                   const std::string&                 pair_name)
{
    OrderbookMid out;

    const std::size_t depth = (params.depth > 0) ? params.depth : 1;

    // -- Layer 3 (part 1): admit only levels that are actually a book -------
    // A non-finite or non-positive price is not a quote, and a non-positive
    // size contributes nothing to a VWAP while being able to zero or invert
    // a depth denominator.  Drop both rather than propagate a NaN.
    //
    // -- Unit normalization: both sides' depths in QUOTE units --------------
    // [2026-08-01 adversarial review, finding 2] CompetingOffer::size is
    // denominated in the OFFERED asset:
    //
    //     Ask (offered = base):  base-asset mojos   (XCH = 1e12 per unit)
    //     Bid (offered = quote): quote-asset mojos  (CAT = 1e3  per unit)
    //
    // and CompetingOffer::price is the pseudo-price
    //
    //     price_pseudo = quote_units_per_base_unit * kMojosPerXch
    //
    // so px = price_pseudo / kMojosPerXch below is quote units per base
    // unit.  Feeding RAW mojos into the depth weighting therefore mixed
    // units across sides: on an XCH/CAT pair ask depth (1e12 mojos/unit)
    // dwarfed bid depth (1e3 mojos/unit) by ~1e9, the micro-price collapsed
    // to bid_vwap, and the Layer-1 invariant clamp pinned the mid at
    // best_bid on essentially every ingest of XCH/DBX and XCH/wUSDC.b --
    // burying real violations under routine warnings.  Normalize both sides
    // into QUOTE units before any weighting:
    //
    //     bid:  size / quote_mojos_per_unit            [quote units]
    //     ask: (size / base_mojos_per_unit) * px       [base units *
    //                                                    quote/base
    //                                                    = quote units]
    //
    // Worked example, XCH/wUSDC.b at px = 2.24 (wUSDC per XCH; wUSDC has
    // 1e3 mojos/unit, XCH has 1e12):
    //
    //     bid of 1000 wUSDC: size = 1000 * 1e3  = 1e6  quote mojos
    //                        ->  1e6 / 1e3            = 1000 quote units
    //     ask of  400 XCH:   size =  400 * 1e12 = 4e14 base mojos
    //                        -> (4e14 / 1e12) * 2.24  =  896 quote units
    //
    // Raw mojos would have weighted these sides 1e6 : 4e14 (~1 : 4e8);
    // normalized they weigh 1000 : 896 -- comparable, as the book actually
    // is.  Within one side the division is a constant factor, so bid VWAPs
    // are unchanged; ask VWAP weights become quote VALUE, which is the
    // correct measure of how much book is resting at each level.
    const double base_mpu = (params.base_mojos_per_unit > 0)
        ? static_cast<double>(params.base_mojos_per_unit)
        : static_cast<double>(kMojosPerXch);
    const double quote_mpu = (params.quote_mojos_per_unit > 0)
        ? static_cast<double>(params.quote_mojos_per_unit)
        : static_cast<double>(kMojosPerXch);

    std::vector<Level> bids, asks;
    bids.reserve(offers.size());
    asks.reserve(offers.size());

    for (const auto& o : offers) {
        const double px = static_cast<double>(o.price)
                          / static_cast<double>(kMojosPerXch);
        if (!std::isfinite(px) || px <= 0.0) continue;
        // Depth in quote units (see the dimensional analysis above);
        // non-positive sizes still cancel out of the VWAP here.
        const double sz = (o.side == Side::Bid)
            ? static_cast<double>(o.size) / quote_mpu
            : (static_cast<double>(o.size) / base_mpu) * px;
        if (!std::isfinite(sz) || sz <= 0.0) continue;
        (o.side == Side::Bid ? bids : asks).push_back({px, sz});
    }

    if (bids.empty() && asks.empty()) {
        return out;  // mid = 0: no book at all.
    }

    // Best first: bids descending, asks ascending.
    std::sort(bids.begin(), bids.end(),
              [](const Level& a, const Level& b) { return a.price > b.price; });
    std::sort(asks.begin(), asks.end(),
              [](const Level& a, const Level& b) { return a.price < b.price; });

    out.bid_levels = bids.size();
    out.ask_levels = asks.size();
    out.best_bid   = bids.empty() ? 0.0 : bids.front().price;
    out.best_ask   = asks.empty() ? 0.0 : asks.front().price;

    // -- Layer 3 (part 2): one-sided book -> NO MID -------------------------
    // A single side bounds fair value; it does not locate it.  Returning that
    // side's price as "the mid" invents information, and on the thin pairs we
    // are routinely the only offer on a side, so this is the common case, not
    // an exotic one.  best_bid / best_ask are still reported.
    if (bids.empty() || asks.empty()) {
        out.one_sided = true;
        spdlog::debug("[OrderbookMid] {}: one-sided book "
                      "(bid_levels={} ask_levels={}) -- reporting no mid "
                      "rather than publishing a single side as the market",
                      pair_name, out.bid_levels, out.ask_levels);
        return out;
    }

    // -- Layer 3 (part 3): degenerate book (touching or crossed) ------------
    // Dexie has no matching engine, so bid >= ask is normal.  There is no
    // interior for a depth weighting to lean within, so take the midpoint:
    // when bid == ask that IS the single price everyone agrees on, and when
    // the book is crossed the midpoint sits inside [ask, bid], which is the
    // only interval either side supports.
    if (out.best_ask <= out.best_bid) {
        out.degenerate = true;
        out.midpoint   = (out.best_bid + out.best_ask) / 2.0;
        out.mid        = out.midpoint;
        out.w_micro    = 0.0;
        if (out.best_ask < out.best_bid) {
            out.spread_bps = (out.best_ask - out.best_bid)
                             / out.midpoint * 10000.0;  // negative
            spdlog::debug("[OrderbookMid] {}: crossed book bid={:.6f} "
                          "ask={:.6f} -- using midpoint {:.6f}",
                          pair_name, out.best_bid, out.best_ask, out.mid);
        }
        return out;
    }

    // -- Two-sided, properly ordered book -----------------------------------
    out.midpoint   = (out.best_bid + out.best_ask) / 2.0;
    out.spread_bps = (out.best_ask - out.best_bid) / out.midpoint * 10000.0;

    const auto [bid_vwap, bid_depth] = vwap_top_n(bids, depth);
    const auto [ask_vwap, ask_depth] = vwap_top_n(asks, depth);

    // -- Layer 2: degrade the weighting as the book widens ------------------
    // Both depths must be strictly positive for the opposite-side weighting to
    // mean anything; if either is zero the micro-price is undefined and the
    // midpoint is the whole answer.
    //
    // [2026-09-02, review] THIS GUARD IS DEFENSIVE AND CANNOT CURRENTLY TAKE
    // ITS FALSE BRANCH.  Every level reaching here has already been filtered
    // to a finite price > 0 and a finite size > 0, and the one-sided early
    // return above guarantees both vectors are non-empty -- so vwap_top_n
    // always sees den > 0 and num > 0 and can never return {0, 0}.  All four
    // terms are unconditionally true and the midpoint-only fallback below is
    // unreachable.  It was equally unreachable BEFORE the touch-price
    // correction; the correction did not create this.  It is kept because it
    // costs nothing and because the filters it leans on live ~70 lines away
    // and could be relaxed by someone who never reads this function.  The
    // books that genuinely must be refused (one-sided, empty, all-zero-size,
    // touching, crossed) are refused earlier, and correctly.
    //
    // CONSEQUENCE FOR THE VWAPs: they are NOT load-bearing for the estimate.
    // Their only remaining consumer is the Layer-1 clamp diagnostic below,
    // which prints them so a (now never-expected) firing can be attributed.
    // Do not read this gate as evidence that the VWAPs still price anything.
    if (bid_vwap > 0.0 && ask_vwap > 0.0
        && bid_depth > 0.0 && ask_depth > 0.0)
    {
        // -- THE STOIKOV MICRO-PRICE, IN TOUCH-PRICE FORM -------------------
        // [2026-09-02] This line previously read
        //
        //     micro = (ask_depth * bid_vwap + bid_depth * ask_vwap)
        //             / (bid_depth + ask_depth);
        //
        // which is NOT a Stoikov micro-price.  Stoikov weights the TOUCH
        // PRICES by the opposite side's size:
        //
        //     micro = (Q_ask * B + Q_bid * A) / (Q_bid + Q_ask)
        //
        // and that is a CONVEX COMBINATION of B and A, so it is bounded in
        // [B, A] by construction, for every depth ratio and every spread.
        // Substituting each side's top-N VWAP for its touch price destroys
        // exactly that property: a convex combination of V_b and V_a is
        // bounded in [V_b, V_a], and since V_b <= B and V_a >= A on any book
        // with more than one level a side, that interval STRICTLY CONTAINS
        // [B, A].  Boundedness was discarded at the substitution, and the
        // Layer-1 clamp below has been papering over it ever since.
        //
        // With d_b = B - V_b, d_a = V_a - A and S = A - B, the old form
        // escaped below the bid whenever
        //
        //     D_a / D_b  >  (S + d_a) / d_b
        //
        // S is ADDITIVE IN THE NUMERATOR, so a NARROWER spread makes the
        // violation EASIER -- which is why the escapes concentrated on the
        // tightest pair and why the Layer-2 spread taper (which only starts
        // biting at 200 bps) was fully disengaged at 95.3% of firings.
        // Measured over ~39h of live XCH/DBX: 1,849 clamp firings, 98.38% of
        // them below the bid, binding on 46.9% of ingests, raw excursion p50
        // 12.29 bps against a modal 8 bps spread.
        //
        // The DEPTH substitution (top-N cumulative rather than touch size) is
        // sound and is retained -- cumulative depth is the better measure of
        // how much book is resting behind each touch.  Only the PRICE
        // substitution was the defect.
        //
        // Written deliberately in TOUCH-PRICE form rather than as "clamp the
        // VWAPs into the book first".  The two are algebraically identical
        // (clamp(V_b, B, A) == B always, and clamp(V_a, B, A) == A always),
        // but the touch form cannot be misread later as a redundant guard and
        // "simplified" back to the VWAPs.  The VWAPs are still computed: they
        // gate usability above and they are reported in the clamp diagnostic
        // below.
        const double micro = (ask_depth * out.best_bid
                              + bid_depth * out.best_ask)
                             / (bid_depth + ask_depth);
        if (std::isfinite(micro) && micro > 0.0) {
            out.microprice = micro;

            const double narrow = std::max(0.0, params.narrow_bps);
            const double wide   = params.wide_bps;

            double w = 1.0;
            if (wide > narrow) {
                w = 1.0 - (out.spread_bps - narrow) / (wide - narrow);
            } else {
                // Degenerate schedule (wide <= narrow): no band to interpolate
                // across, so it collapses to a step at `narrow`.  Config
                // validation rejects this, but the estimator must not divide
                // by zero if it is ever reached.
                w = (out.spread_bps <= narrow) ? 1.0 : 0.0;
            }
            out.w_micro = std::clamp(w, 0.0, 1.0);
        }
    }

    out.mid = out.w_micro * out.microprice
              + (1.0 - out.w_micro) * out.midpoint;

    // -- Layer 1: THE INVARIANT ---------------------------------------------
    // best_bid <= mid <= best_ask.  Non-negotiable.  Enforced last so that
    // nothing downstream of it can reintroduce a violation.
    //
    // A binding clamp is always a defect signal, never routine operation: it
    // means the depth weighting produced a number outside the book it was
    // computed from.  Warning level, with the pair, the raw micro-price and
    // the BBO, so the condition is greppable in production logs.
    //
    // [2026-09-02] Since the micro-price is now a convex combination of
    // best_bid and best_ask, and the midpoint it blends with is likewise
    // interior, `mid` is in [best_bid, best_ask] ANALYTICALLY on every
    // two-sided ordered book.  This clamp should therefore never bind again;
    // it is retained as the last line of defence (and against floating-point
    // dust at the boundary), NOT as a routine correction.  If it fires, the
    // estimator above has regressed -- the VWAPs are logged alongside so the
    // first question, "did someone put the VWAPs back into the numerator",
    // can be answered straight from the log line.
    if (out.mid < out.best_bid || out.mid > out.best_ask) {
        const double raw = out.mid;
        out.mid     = std::clamp(out.mid, out.best_bid, out.best_ask);
        out.clamped = true;
        spdlog::warn("[OrderbookMid] {}: INVARIANT CLAMP -- blended mid "
                     "{:.8f} outside book [bid {:.8f}, ask {:.8f}]; "
                     "raw microprice={:.8f} w_micro={:.3f} "
                     "spread={:.0f}bps depth(bid/ask)={:.0f}/{:.0f} "
                     "vwap(bid/ask)={:.8f}/{:.8f} "
                     "levels(bid/ask)={}/{} -- clamped to {:.8f}",
                     pair_name, raw, out.best_bid, out.best_ask,
                     out.microprice, out.w_micro, out.spread_bps,
                     bid_depth, ask_depth, bid_vwap, ask_vwap,
                     out.bid_levels, out.ask_levels,
                     out.mid);
    }

    return out;
}

}  // namespace xop
