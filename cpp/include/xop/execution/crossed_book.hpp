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
// nothing downstream can detect it. (Checked 2026-09-01: all 23 crossed_book
// rows settled below the cap, so no remediation is owed. That is a
// consequence of never having had the funds, not of the code being right.)
//
// THE FIX IS A FILTER, NOT A CLAMP. If the whole offer does not fit under
// the cap, we do not take it. Step 9d has done this correctly since it was
// written (engine.cpp:11771-11801) and Step 9f likewise; this header is 9d's
// rule made testable, because nothing in cpp/tests can construct an Engine
// (TODO S36) and the call site is otherwise unpinnable.
//
// WE SKIP; WE DO NOT RE-SELECT. When the cheapest ask is oversized we
// decline the pair for this cycle rather than looking for a smaller ask
// inside the cross. That is 9d's semantics and it is deliberately the
// conservative direction: it makes the set of offers we take a strict
// SUBSET of what the old code took, so this change cannot cause a take that
// would not have happened before. Selecting the best ask WITHIN the cap
// would capture more edge, but it would make us lift offers we do not lift
// today, which is the one thing that breaks the subset property. That is a
// follow-up, deliberately not done here.
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

    // 9.0e18 is exactly representable as a double (and long double is double
    // under MSVC), and is strictly below 2^63, so both the comparison and the
    // subsequent narrowing are exact and well-defined on every ABI we build.
    // A previous saturation clamp in this repo rounded to 2^63 and passed
    // MSVC while failing GCC; do not replace this with (long double)INT64_MAX.
    //
    // NO TEST GUARDS THIS LINE, and that was checked rather than assumed:
    // deleting it leaves the whole suite green on MSVC. Narrowing an
    // out-of-range floating value to an integer is UNDEFINED, so no portable
    // test can pin it -- MSVC's cvttsd2si happens to yield INT64_MIN, which
    // the `cap > 0` line below then absorbs into a safe 0. That is luck, and
    // it is ABI-specific. THIS BOUND is what makes the conversion defined;
    // the `cap > 0` below is a second line of defence, not the first. Do not
    // delete it on the grounds that the tests still pass.
    constexpr long double kMaxSafe = 9.0e18L;

    const long double cap_ld = static_cast<long double>(max_take_units)
                             * static_cast<long double>(base_mojos_per_unit);
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
    std::string best_ask_offer_id;   ///< id of that ask, empty if none
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
        return d;  // NoBook -- take_size stays 0
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
    // taken on chain and record_taker_fill() returns early at engine.cpp:14301
    // (base_mojos <= 0), leaving a settled take with NO taker_fills row and NO
    // ledger legs at all.
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
