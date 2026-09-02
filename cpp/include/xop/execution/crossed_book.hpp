#ifndef XOP_EXECUTION_CROSSED_BOOK_HPP
#define XOP_EXECUTION_CROSSED_BOOK_HPP
// ---------------------------------------------------------------------------
// crossed_book.hpp -- the Step 9c crossed-book taker decision, extracted.
//
// [S40 2026-09-01] WHY std::min IS THE BUG AND NOT THE FIX
// --------------------------------------------------------
// Step 9c used to end its candidate selection with
//
//     const Mojo take_size = std::min(best_ask_size, max_take_mojos);
//
// and then took the offer with
//
//     co_await wallet_->take_offer(offer_status.offer.offer_bech32, fee);
//
// The signature is take_offer(const std::string& offer_text, uint64_t fee)
// -- chia_rpc.hpp:499. THERE IS NO SIZE PARAMETER. A Chia offer is an atomic
// swap of exactly the coins the maker committed to it; there is no partial
// fill and no way to lift half of one. So the clamp could not, even in
// principle, bound what we spent. It bounded a LOG FIELD. Every consumer of
// take_size was a log line, an alert string, or record_taker_fill() -- never
// the RPC. arbitrage.crossed_book_max_take_xch was inert.
//
// The measured consequence: 208 attempts logged size=5000000000000, exactly
// the 5.0 XCH cap, which means the counterparty offer was LARGER and only
// the log was clamped. Every one of those failed in the wallet on funding.
// Being broke is the only thing that has ever bounded this path. Funding the
// wallet without this fix unmasks an unbounded take.
//
// The second-order damage is worse than the log. record_taker_fill() derives
// the quote leg as quote_mojos_for(base_mojos, price_mojos, ...), so a
// clamped base size synthesises BOTH double-entry ledger legs and the
// taker_fills row from a number the RPC never saw -- coherently, so the
// books stay internally consistent while being wrong about reality, and
// nothing downstream can detect it.
//
// HISTORICAL ROWS: 19 CLEAN, 4 UNDECIDED. An earlier draft of this header
// said "all 23 crossed_book rows settled below the cap, so no remediation is
// owed". That was asserted without querying the database and it is wrong.
// Re-read read-only on 2026-09-01, taker_fills WHERE strategy='crossed_book'
// has 23 rows; nineteen are strictly below their pair's cap, and four are
// EXACTLY ON IT:
//
//     2026-08-06T21:38:01.800Z  BYC/wUSDC.b  base_delta=5000  quote_delta=-5005
//     2026-08-06T22:00:26.487Z  BYC/wUSDC.b  base_delta=5000  quote_delta=-5005
//     2026-08-06T22:23:07.326Z  BYC/wUSDC.b  base_delta=5000  quote_delta=-5005
//     2026-08-06T22:45:05.796Z  BYC/wUSDC.b  base_delta=5000  quote_delta=-5005
//
// BYC is a CAT, so base_mojos_per_unit is 1000 and cap_mojos_for(5.0, 1000)
// is exactly 5000. Landing exactly on the cap is the CLAMP SIGNATURE -- it is
// what std::min(size, 5000) returns for every counterparty offer of 5000 or
// more -- so those four rows are indistinguishable from clamped records. The
// quote leg is derived, not observed, so if the base was clamped both
// double-entry legs are wrong by the same construction. Logs older than
// 2026-08-31 have rotated away, so this cannot now be settled from logs; it
// needs the four counterparty offer ids adjudicated on chain or on Dexie.
// REMEDIATION FOR THOSE FOUR ROWS IS OPEN. Do not restate it as closed
// without the adjudication.
//
// THE FIX IS A FILTER, NOT A CLAMP. If the whole offer does not fit under
// the cap, we do not take it. Step 9d has done this correctly since it was
// written -- see Step 9d's `size_ok` filter and its "takes are all-or-nothing"
// skip log -- and Step 9f likewise; this header is 9d's rule made testable,
// because nothing in cpp/tests can construct an Engine (TODO S36) and the
// call site is otherwise unpinnable. (Line numbers are deliberately not cited:
// this step moved ~40 lines in the commit that added this header, and the
// citations it shipped with were already stale.)
//
// WE SKIP; WE DO NOT RE-SELECT. When the cheapest ask is oversized we
// decline the pair for this cycle rather than looking for a smaller ask
// inside the cross. That is 9d's semantics and it is deliberately the
// conservative direction. Selecting the best ask WITHIN the cap would
// capture more edge, but it would make us lift offers we do not lift today.
// That is a follow-up, deliberately not done here.
//
// THE SUBSET PROPERTY, STATED ACCURATELY. For every book whose cheapest ask
// carries a positive price, the set of offers this header takes is a subset
// of what the old code took. There is exactly ONE exception and it is worth
// naming rather than hiding: a zero- or negative-priced cheapest ask. The old
// selection loop had no price guard, so such an ask won selection, produced
// edge = (bid - 0) / 0 = +inf, cleared any minimum, and WAS TAKEN. This
// header excludes it, which promotes the next-cheapest ask -- a real,
// finite-priced, edge-checked, cap-checked offer the old code never reached.
// So in that one case we take a DIFFERENT offer, not a superset of them:
// one phantom take is replaced by one ordinary take. A price of 0 is
// reachable, not theoretical -- ingest computes price by llround()ing a
// market ratio scaled by kMojosPerXch, which rounds to 0 below 5e-13. The
// redirection is the safer behaviour; the blanket claim that no take can
// happen that would not have happened before is simply not true, and
// CrossedBook.ZeroPricedAskCannotManufactureInfiniteEdge encodes the
// exception.
//
// THE INVARIANT, stated as a biconditional because one direction is not
// enough:
//
//     decision.take_size != 0  <=>  decision.verdict == Take
//     and when it is Take:  take_size == best_ask_size <= cap_mojos
//
// The forward direction stops the clamp. The reverse direction stops the
// other shape of the same bug -- a caller reading take_size off a DECLINED
// decision and handing it to record_taker_fill(). No code path in this
// header produces a size that is not the counterparty's size verbatim.
// There is no std::min here, and there must never be one.
//
// Pure header: plain data plus xop::types, no engine types, no asio, no
// spdlog, driven directly by cpp/tests/test_crossed_book.cpp.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "xop/types.hpp"

namespace xop::execution {

// ---------------------------------------------------------------------------
// cap_mojos_for -- the cap in base mojos, from config units and the PAIR's
//                  denomination.
//
// Step 9c is denominated per pair: PairConfig::base_mojos_per_unit is 1e12
// for an XCH base and 1000 for a CAT (config.cpp:638), so the same
// crossed_book_max_take_xch: 5.0 means 5e12 mojos on XCH/DBX and 5000 mojos
// on BYC/wUSDC.b. Step 9d uses kMojosPerXch instead. That difference is
// REAL and is preserved on purpose -- this argument exists so that anyone
// "unifying" the two turns a test red rather than silently multiplying the
// BYC cap by a billion.
//
// Returns 0 for any cap that cannot be represented or trusted: a
// non-positive or non-finite unit count, a non-positive denomination, or a
// product that would not fit in a Mojo. Zero means TAKE NOTHING. An
// unrepresentable cap is an unbounded cap, and unbounded is the bug -- so it
// declines rather than falling back to "no limit".
// ---------------------------------------------------------------------------
[[nodiscard]] inline Mojo cap_mojos_for(double       max_take_units,
                                        std::int64_t base_mojos_per_unit) noexcept
{
    if (base_mojos_per_unit <= 0) return 0;
    if (!(max_take_units > 0.0) || !std::isfinite(max_take_units)) return 0;

    // 9.0e18 is exactly representable as a double (9e18 = 2^18 * 34332275390625,
    // a 46-bit odd part) and is strictly below 2^63, so both the comparison
    // and the subsequent narrowing are exact and well-defined on every ABI we
    // build. A previous saturation clamp in this repo rounded to 2^63 and
    // passed MSVC while failing GCC; do not replace this with
    // (long double)INT64_MAX.
    //
    // NO TEST GUARDS THIS LINE, and that was checked rather than assumed:
    // deleting it leaves the whole suite green on MSVC. Narrowing an
    // out-of-range floating value to an integer is UNDEFINED, so no portable
    // test can pin it -- MSVC's cvttsd2si happens to yield INT64_MIN, which
    // the `cap > 0` line below then absorbs into a safe 0. That is luck, and
    // it is ABI-specific. THIS BOUND is what makes the conversion defined;
    // the `cap > 0` below is a second line of defence, not the first. Do not
    // delete it on the grounds that the tests still pass.
    //
    // [2026-09-01] THE TYPE HERE IS `double`, DELIBERATELY, AND IT USED TO BE
    // `long double`. The comment above was true of the NARROWING and false of
    // the VALUE, which is a distinction worth spelling out because this
    // function is cited elsewhere as the exemplar of getting this right.
    //
    // `long double` is 53-bit on MSVC and 64-bit x87 on GCC/x86-64, so
    // `units * base_mojos_per_unit` was computed to different precisions and
    // then TRUNCATED -- the cap VALUE was ABI-divergent, in the same file
    // that warns about ABI divergence. For max_take_units = 0.15 and
    // base_mojos_per_unit = 1e12 the exact product is 149999999999.999994...,
    // which the 53-bit rounding lifts to exactly 150000000000 while the
    // 64-bit rounding leaves below it: MSVC 150000000000, GCC 149999999999.
    // Same for 0.03 and 0.3. This is the cap that `base_sz > max_mojos` is
    // tested against at Step 9e and Step 9f -- the same guard the exact
    // base_size_for_bid result feeds.
    //
    // It was latent, not live: every current config value
    // (crossed_book_max_take_xch 5.0, midpoint_recycling_max_take_xch 0.25,
    // peg_arb_max_take_units 50, drift_corrector_max_take_units 2.0) is
    // dyadic-exact and agrees on both ABIs. Any operator writing 0.15, 0.03
    // or 0.3 would have made CI and production disagree by one mojo again.
    //
    // `double` fixes it because binary64 arithmetic is correctly rounded and
    // identical on both toolchains here: GCC targets x86-64 with SSE2, where
    // FLT_EVAL_METHOD is 0, so there is no x87 excess precision to leak in.
    // MSVC is bit-identical to before (its long double IS double), so the
    // DEPLOYED build's cap values do not move at all.
    //
    // Note this is portable rounding, NOT exact-rational evaluation, and that
    // is the intended reading: 0.15 * 1e12 in exact rational over the double
    // 0.15 is 149999999999, whereas round-to-nearest recovers the 150000000000
    // the operator meant when they typed 0.15. Truncation afterwards stays --
    // it only ever narrows the cap, which is the conservative direction.
    constexpr double kMaxSafe = 9.0e18;

    const double cap_ld = static_cast<double>(max_take_units)
                        * static_cast<double>(base_mojos_per_unit);
    if (cap_ld > kMaxSafe) return 0;

    const Mojo cap = static_cast<Mojo>(cap_ld);  // truncates down -- conservative
    return cap > 0 ? cap : 0;
}

// ---------------------------------------------------------------------------
// The verdict. Exactly one of these describes any book.
// ---------------------------------------------------------------------------
enum class CrossedBookVerdict {
    NoBook,          ///< empty book, or no usable price on one side
    NotCrossed,      ///< best_bid < best_ask -- no opportunity
    EdgeTooThin,     ///< crossed, but edge below min_edge_bps
    CapUnusable,     ///< the cap itself is not a usable number -- take nothing
    ZeroSizeOffer,   ///< cheapest ask carries no size -- nothing to take or record
    SizeExceedsCap,  ///< whole offer will not fit; takes are all-or-nothing
    Take,            ///< lift it, in full, at exactly best_ask_size
};

// ---------------------------------------------------------------------------
// The decision. Defaults to NoBook with a zero size: a default-constructed
// or forgotten decision declines.
// ---------------------------------------------------------------------------
struct CrossedBookDecision {
    CrossedBookVerdict verdict{CrossedBookVerdict::NoBook};

    Mojo        best_bid_price{0};   ///< highest competing bid, 0 if none
    Mojo        best_ask_price{0};   ///< lowest competing ask, 0 if none
    std::string best_ask_offer_id{}; ///< id of that ask, empty if none
    Mojo        best_ask_size{0};    ///< the counterparty's size, VERBATIM
    double      edge_bps{0.0};       ///< (bid - ask) / ask * 10000, 0 if not crossed

    /// Non-zero IFF verdict == Take, and then exactly best_ask_size.
    /// Never a clamped value. See the biconditional at the top of this file.
    Mojo take_size{0};

    /// The cap this decision was judged against, for the log. 0 when unusable.
    Mojo cap_mojos{0};
};

// ---------------------------------------------------------------------------
// evaluate_crossed_book
//
// @param offers        one pair's competing book, own offers already excluded
//                      by ingest (engine.cpp:3559-3563). Mixing pairs is a
//                      precondition violation, not a case handled here.
// @param min_edge_bps  arbitrage.crossed_book_min_edge_bps.
// @param cap_mojos     the per-pair cap in base mojos -- from cap_mojos_for().
//
// Order of judgement is deliberate. Book shape, then cross, then edge, then
// the cap. A bad cap is only reported when it would actually have gated a
// take, which keeps it out of the per-block log of every uncrossed pair
// while still surfacing it at the exact moment it costs us something.
// ---------------------------------------------------------------------------
[[nodiscard]] inline CrossedBookDecision evaluate_crossed_book(
    const std::vector<CompetingOffer>& offers,
    double                             min_edge_bps,
    Mojo                               cap_mojos)
{
    CrossedBookDecision d;

    // -- Selection. Cheapest ask wins, REGARDLESS OF SIZE. -------------------
    // Size is not a selection criterion, only a veto: an oversized cheapest
    // ask must make us skip the pair, not quietly promote the second-cheapest
    // ask into a trade we would not otherwise have made.
    //
    // Both sides require price > 0. Step 9c did not, and a zero-priced ask
    // would have produced edge = (bid - 0) / 0 = +inf, cleared any minimum,
    // and been taken. Excluding it strictly reduces action.
    Mojo best_bid = 0;
    Mojo best_ask = std::numeric_limits<Mojo>::max();
    bool have_ask = false;

    for (const auto& co : offers) {
        if (co.price <= 0) continue;

        if (co.side == Side::Bid && co.price > best_bid) {
            best_bid = co.price;
        }
        if (co.side == Side::Ask && co.price < best_ask) {
            best_ask            = co.price;
            d.best_ask_offer_id = co.offer_id;
            d.best_ask_size     = co.size;
            have_ask            = true;
        }
    }

    if (best_bid == 0 || !have_ask) {
        // NoBook. Return the ask fields to their defaults as well, so a
        // declined decision never carries a populated offer id and size
        // beside a best_ask_price of 0 -- the three are written and returned
        // as a unit or not at all. The biconditional below constrains only
        // take_size; this closes the gap it leaves, because the detection log
        // prints best_ask_size and a future edit that moves that log above
        // the quiet-verdict filter must not be able to print a real size
        // against a zero price.
        d.best_ask_offer_id.clear();
        d.best_ask_size = 0;
        return d;  // take_size stays 0
    }

    d.best_bid_price = best_bid;
    d.best_ask_price = best_ask;

    if (best_bid < best_ask) {
        d.verdict = CrossedBookVerdict::NotCrossed;
        return d;
    }

    // Same arithmetic Step 9c has always used, so the edge reported by this
    // header is the number the live logs have been printing.
    const double ask_d = static_cast<double>(best_ask);
    const double bid_d = static_cast<double>(best_bid);
    d.edge_bps = (bid_d - ask_d) / ask_d * 10000.0;

    // Written as !(>=) rather than (<) so a NaN on either side DECLINES.
    // For finite inputs this is exactly the old edge_bps < min_edge_bps.
    if (!(d.edge_bps >= min_edge_bps)) {
        d.verdict = CrossedBookVerdict::EdgeTooThin;
        return d;
    }

    if (cap_mojos <= 0) {
        d.verdict = CrossedBookVerdict::CapUnusable;
        return d;
    }
    d.cap_mojos = cap_mojos;

    // 9d guards this (best_ask_a_size > 0). Without it a zero-size offer is
    // taken on chain and record_taker_fill() returns early on its
    // `base_mojos <= 0` guard, leaving a settled take with NO taker_fills row
    // and NO ledger legs at all.
    if (d.best_ask_size <= 0) {
        d.verdict = CrossedBookVerdict::ZeroSizeOffer;
        return d;
    }

    // Inclusive, matching 9d's best_ask_a_size <= max_take_mojos. An offer of
    // exactly the cap fits the cap.
    if (d.best_ask_size > cap_mojos) {
        d.verdict = CrossedBookVerdict::SizeExceedsCap;
        return d;
    }

    d.verdict   = CrossedBookVerdict::Take;
    d.take_size = d.best_ask_size;   // NOT std::min. Ever.
    return d;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_CROSSED_BOOK_HPP
