#ifndef XOP_EXECUTION_TAKE_SIZING_HPP
#define XOP_EXECUTION_TAKE_SIZING_HPP
// ---------------------------------------------------------------------------
// take_sizing.hpp -- unit conversion for taker paths, extracted so it can be
//                    tested and so there is ONE copy of it.
//
// [S40 follow-up 2026-09-01] WHY THIS EXISTS
// ------------------------------------------
// Two facts about a competing offer, both easy to forget at a call site:
//
//   1. take_offer(offer_text, fee) has NO SIZE PARAMETER (chia_rpc.hpp:499).
//      A Chia offer is an atomic swap of exactly the coins the maker
//      committed; there is no partial fill. Any per-take size limit is
//      therefore a FILTER -- "does the whole offer fit?" -- and never a
//      clamp. std::min() on a take size bounds a log field and nothing else.
//      That was S40.
//
//   2. The two sides are denominated in DIFFERENT assets. The ingest says so
//      (market_data.cpp:1933-1937): "Bid-side offers are denominated in the
//      QUOTE asset ...; ask-side offers are in the BASE asset". So an ASK's
//      `size` is already base mojos, while a BID's `size` is quote mojos and
//      must be converted before it is compared against a base-denominated
//      cap, charged against a balance, or handed to record_taker_fill() --
//      which books its `base_mojos` argument as base.
//
// Step 9f got both right inline. Step 9e got both wrong: it clamped with
// std::min and treated a bid's quote size as base mojos, in the cap
// comparison, in the pre-balance cost estimate and in the ledger. On
// BYC/wUSDC.b the second error is invisible because base and quote
// denominations happen to be equal (1000 each); on any pair where they
// differ the cap is applied in the wrong unit.
//
// Rounding is UP, always, and deliberately. BOTH of these functions compute
// an OUTFLOW -- something we must hand over -- they just do it in different
// assets. quote_cost_for_ask is the quote we spend lifting an ask;
// base_size_for_bid is the base we must DELIVER to hit a bid (the maker
// offers quote and requests base, and the advertised `size` is the offered
// quote leg). So overstating is the safe direction for both: rounding a cost
// down invents affordability, and rounding a delivery down under-reports what
// we are about to hand over. Both errors point at spending more than
// intended, which is the direction this file exists to close.
//
// One accepted asymmetry, recorded so nobody "fixes" it later: engine.cpp
// also passes base_size_for_bid's result to record_taker_fill() as
// `base_mojos`, where rounding UP overstates the inventory relieved by up to
// one mojo. That is a one-mojo ledger bias in exchange for a guard that
// cannot fail open. The guard wins. Do not flip the rounding to suit the
// ledger.
//
// Pure header: plain integers plus xop::types. No engine types, no asio, no
// spdlog. Driven directly by cpp/tests/test_take_sizing.cpp.
//
// ===========================================================================
// [2026-09-01] THE ARITHMETIC HERE IS EXACT INTEGER ARITHMETIC. DO NOT
//              "SIMPLIFY" IT BACK TO A FLOATING EXPRESSION. READ THIS FIRST.
// ===========================================================================
// This file used to compute both conversions in `long double`. That is a
// PLATFORM-DEPENDENT type, and it made this header give different answers on
// different CI runners:
//
//     `long double` is 64-bit (IDENTICAL to double, 53-bit significand) on
//     MSVC, and 80-bit x87 extended (64-bit significand) on GCC/x86-64.
//
// The witness is the first assertion in test_take_sizing.cpp:
//
//     quote_cost_for_ask(1e12, 1.5e12, 1e12, 1e3)
//         = 1e12 * 1.5e12 * 1e3 / (1e12 * 1e12)
//         = EXACTLY 1500, with no fractional part whatsoever.
//
//     MSVC returned 1500.  GCC returned 1501.  CI was red on GCC.
//
// The temptation is to read that as "MSVC is right, GCC is broken". It is the
// other way round: NEITHER is reliable, and MSVC was right BY LUCK. The
// intermediate 1e12 * 1.5e12 * 1e3 = 1.5e27 has odd part 3*5^27, which needs
// 65 significand bits -- more than x87's 64, and far more than double's 53.
// GCC rounds at the third multiply and lands a hair ABOVE 1500, which ceil()
// lifts to 1501. MSVC rounds earlier, at the FIRST multiply, and its two
// errors happen to cancel. Widening the float type does not fix this: there
// is no floating type on either platform wide enough to hold the
// intermediate. The same mechanism made
// base_size_for_bid(3000, 1.5e12, 1e12, 1e3) return 2000000000001 on GCC
// where the exact answer is 2000000000000.
//
// And the divergence was never confined to CI. But state the size of it
// correctly, because an earlier draft of this comment quoted a single
// percentage and that is not what the error does.
//
// THE LAW IS: the float result is wrong by up to ONE ULP OF THE RESULT. So
// the error is not a fixed number of mojos, it scales with the answer, and
// the rate at which it bites depends entirely on where the answer sits
// relative to the format's significand.
//
//   * BID path (base_size_for_bid), live XCH/DBX, a competing bid whose base
//     leg is 0.1-100 XCH (quote_size 1e4..1e7 DBX mojos, result 1e11..1e14):
//     the deployed MSVC build disagrees with exact arithmetic on 0.33% of
//     conversions, and the error is EXACTLY ONE MOJO -- but 92% of those are
//     one mojo BELOW the true ceiling, which is precisely the fail-open this
//     header's rounding contract exists to prevent. GCC's wider format is
//     wrong on 0.005% of the same band, also by one.
//
//   * ASK path (quote_cost_for_ask), same pair, our own tier sizes
//     (1.0-5.0 XCH per config max_offer_size_units): NEITHER ABI diverged in
//     20,000 samples. At live magnitudes the ask path was clean. It is
//     fragile specifically on exact-integer results -- which is the 1500/750
//     case CI was red on -- not on the bulk of live inputs.
//
//   * AWAY FROM LIVE MAGNITUDES the ±1 characterisation collapses, so do not
//     carry it into a commit message as a bound. Once the result passes 2^53
//     the MSVC error rate goes to ~100% and grows with the ULP: at a result
//     of ~1e18 it is up to 236 mojos, and the test file's own bid witness
//     (test_take_sizing.cpp:541) is 482 mojos LOW at a result of 7.2e18.
//     There is no upper bound on it in the abstract; there is only the bound
//     the live size configuration happens to impose.
//
// (Numbers above are from exact-rational emulation of round-to-nearest-even
// at 53 and 64 significand bits in the source's operation order, validated
// against every witness in this file and in test_take_sizing.cpp before
// being trusted.)
//
// THE FIX IS THAT THERE IS NOTHING TO ROUND. Every input is an integer count
// of mojos and every output is an integer count of mojos. The conversions are
// exact rationals and the contract is a CEILING DIVISION. Floating point was
// never buying anything here except a platform dependency.
//
// The guard against a future innocent "simplification" is not this comment,
// it is the static_assert block at the bottom of this file. It pins
//
//     quote_cost_for_ask(10'000'001, 99'999'990'000'001, 1e12, 1e3) == 2
//
// because 10000001 * 99999990000001 = 10^21 + 1 EXACTLY, so the true value is
// 1.000000000000000000001 and the true ceiling is 2. Reaching that answer
// needs about 70 bits of significand. EVERY floating format on EVERY platform
// we build for computes exactly 1.0 and returns 1 -- MSVC and GCC alike. So
// any rewrite of these functions in float fails to COMPILE, on any compiler,
// rather than failing a test on one runner. That is deliberate. Do not weaken
// it to a runtime EXPECT.
//
// WHAT IS STILL FLOAT ON THIS PATH -- so that "there is ONE copy of it" above
// is read as the intent and not as a claim that the sweep is finished:
//
//   * types.hpp:59-67 quote_mojos_for() is the same rational in double. It is
//     still used by Engine::record_taker_fill (engine.cpp:14875-14879) to
//     derive the ledger's QUOTE leg from the now-exact base size, and by
//     Engine::live_offer_exposure (engine.cpp:14956-14971), which additionally
//     uses std::llround -- round to NEAREST, not the ceiling contract stated
//     below -- so an exposure reading can come in up to half a mojo light per
//     offer. Both are portable-but-inexact rather than platform-divergent:
//     double is IEEE binary64 under SSE2 on both toolchains, so MSVC and GCC
//     agree. That is why they were left alone here; they are an accuracy and
//     consistency question, not the CI-red question, and live_offer_exposure
//     feeds a live reserve reading that this change deliberately does not
//     move. Deliberate exception, recorded rather than silently tolerated.
//
//   * co.price itself -- see the SCOPE note on base_size_for_bid below.
//
// WHAT THESE TESTS DO NOT COVER -- read this before treating a green suite as
// proof. test_take_sizing.cpp pins the ARITHMETIC in this file. It cannot
// reach the CALL SITES: nothing in cpp/tests constructs an Engine (TODO S36),
// so the Step 9e filter, the Step 9f filter and the Step 9 recovery taker's
// per-block budget are all UNGUARDED LINES. Delete any of those three filters
// and the suite stays green. The reason to move the maths here was to make
// the part that CAN be pinned actually pinned, and to stop 9e and 9f keeping
// two copies of it -- not to claim the call sites are covered.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <limits>

#include "xop/types.hpp"

namespace xop::execution {

// ---------------------------------------------------------------------------
// detail -- a portable exact wide-integer kernel.
//
// WHY NOT __int128 / _umul128, given that one of the two exists on every
// compiler we actually build with:
//
//   * IT IS NOT WIDE ENOUGH. This is the decisive reason and it is easy to
//     miss. The numerators genuinely exceed 128 bits. base_size and price are
//     both llround() of a Dexie-supplied double (engine.cpp:3545-3559), there
//     is no maximum-size filter anywhere on ingest, and the per-take cap is
//     applied AFTER this conversion (engine.cpp:12449, :12999). So the domain
//     really is the whole positive Mojo range, and
//     quote_size * kMojosPerXch * base_mpu reaches
//     (2^63-1) * 1e12 * (2^63-1) ~ 2^166. Even on unremarkable data, a bid
//     advertising 1e15 quote mojos on an XCH-base pair gives
//     1e15 * 1e12 * 1e12 = 1e39, which is 130 bits and already overflows an
//     unsigned __int128. A 128-bit intermediate does not merely lack
//     portability here; it is WRONG.
//
//   * cpp/CMakeLists.txt:53-55 sets CXX_EXTENSIONS OFF, so GCC compiles with
//     -std=c++20 (strict ISO), and :82-83 adds -Wpedantic -Werror. In strict
//     ISO mode -Wpedantic diagnoses __int128 as a non-standard type. Boost in
//     this very tree wraps it in __extension__ under #ifdef __GNUC__ for
//     exactly that reason (boost/config/detail/suffix.hpp:521-528).
//
//   * MSVC's _umul128/_udiv128 are x64-only, need <intrin.h>, and _udiv128
//     raises #DE unless the quotient is separately proven to fit 64 bits.
//
//   * Two implementations behind an #if, only one of which the developer
//     machine ever compiles, is EXACTLY the failure mode that produced this
//     bug and the three other MSVC/GCC divergences on this branch.
//
// So: no intrinsic, no extension, no #if, no floating point, one code path
// everywhere. Everything below is fixed-width unsigned arithmetic, whose
// results and wrap behaviour are fully specified by the standard, with a
// proof at each step that no intermediate reaches 2^64 and no shift count
// reaches the operand width. GCC and MSVC therefore compute IDENTICAL values
// by construction rather than by coincidence -- which is what matters, since
// this bug cannot be reproduced on the developer machine.
//
// Everything is constexpr on purpose. The static_asserts at the bottom of
// this file are evaluated by THE COMPILER DOING THE BUILD, so GCC checks them
// itself; and constant evaluation forbids undefined behaviour outright, so a
// bad shift here becomes a compile error on every platform rather than a
// silent divergence on one.
// ---------------------------------------------------------------------------
namespace detail {

inline constexpr std::uint64_t kLow32Mask = 0xFFFF'FFFFULL;

struct U128 {
    std::uint64_t hi{0};
    std::uint64_t lo{0};
};

struct U192 {
    std::uint64_t hi{0};
    std::uint64_t mid{0};
    std::uint64_t lo{0};
};

// 64 x 64 -> 128, via 32-bit limbs so that no single operation can overflow
// 64 bits.
//
// Overflow proof. With a = a_high*2^32 + a_low and b = b_high*2^32 + b_low:
//   part_mid = (p00 >> 32) + (p01 & mask) + (p10 & mask)
//            <= (2^32 - 2) + (2^32 - 1) + (2^32 - 1) < 3 * 2^32 < 2^64.
//   result.hi = p11 + (p01 >> 32) + (p10 >> 32) + (part_mid >> 32)
//             <= (2^64 - 2^33 + 1) + (2^32 - 2) + (2^32 - 2) + 2 = 2^64 - 1.
// Every shift count is 32, well below the 64-bit operand width, so no shift
// is undefined. Total function: defined for all inputs.
[[nodiscard]] constexpr U128 mul_u64(std::uint64_t a, std::uint64_t b) noexcept
{
    const std::uint64_t a_low  = a & kLow32Mask;
    const std::uint64_t a_high = a >> 32;
    const std::uint64_t b_low  = b & kLow32Mask;
    const std::uint64_t b_high = b >> 32;

    const std::uint64_t p00 = a_low * b_low;
    const std::uint64_t p01 = a_low * b_high;
    const std::uint64_t p10 = a_high * b_low;
    const std::uint64_t p11 = a_high * b_high;

    const std::uint64_t part_mid =
        (p00 >> 32) + (p01 & kLow32Mask) + (p10 & kLow32Mask);

    U128 result;
    result.lo = (part_mid << 32) | (p00 & kLow32Mask);
    result.hi = p11 + (p01 >> 32) + (p10 >> 32) + (part_mid >> 32);
    return result;
}

// Exact product of three values, each required to be < 2^63.
//
// PRECONDITION: a, b, c < 2^63. Guaranteed at every call site -- each factor
// is either a positive Mojo (a signed 64-bit integer, hence <= 2^63-1),
// kMojosPerXch (1e12), or mpu_or_one() of an std::int64_t (also <= 2^63-1).
//
// Under that precondition the product is < 2^189, so three limbs ALWAYS
// suffice -- for both functions, over the entire Mojo domain, with no
// saturation needed in the multiply itself.
//
// Carry proof. ab < 2^126, so ab.hi < 2^62; hence high = ab.hi * c < 2^125
// and high.hi < 2^61. The carry out of `mid` adds at most 1, so result.hi
// cannot wrap.
[[nodiscard]] constexpr U192 mul3_u64(std::uint64_t a,
                                      std::uint64_t b,
                                      std::uint64_t c) noexcept
{
    const U128 ab   = mul_u64(a, b);
    const U128 low  = mul_u64(ab.lo, c);
    const U128 high = mul_u64(ab.hi, c);

    U192 result;
    result.lo  = low.lo;
    result.mid = low.hi + high.lo;
    result.hi  = high.hi + ((result.mid < low.hi) ? 1ULL : 0ULL);  // carry
    return result;
}

struct DivResult {
    U192          q{};
    std::uint64_t r{0};
};

// 192-bit / 64-bit, exact quotient and remainder, by restoring binary long
// division. 192 iterations, on a path that runs a handful of times per block;
// the cost is irrelevant, and the correctness is arguable line by line, which
// is the actual requirement here.
//
// PRECONDITION: d < 2^63. This is what makes the remainder shift safe, and it
// is NOT a property of division in general -- state it, do not leave it
// implicit for the next reader to rediscover.
//
// Overflow proof. The loop invariant is rem < d. With d <= 2^63-1 that gives
// rem <= 2^63-2, so (rem << 1) | 1 <= 2^64-3 and the shifted remainder never
// wraps. A general 64-bit divisor WOULD break this. Shift counts are 1 and
// 63, both < 64, so no shift is undefined. The quotient of a 192-bit value by
// d >= 1 fits in 192 bits, so no quotient bit is shifted off the top.
//
// d == 0 returns {0, 0} rather than dividing: total, not undefined. No divide
// instruction is executed, so there is no trap and no garbage quotient. The
// caller reads that as an unpriceable zero, which declines.
[[nodiscard]] constexpr DivResult divmod_u192(U192 n, std::uint64_t d) noexcept
{
    DivResult out{};
    if (d == 0) return out;

    const std::uint64_t limbs[3] = {n.hi, n.mid, n.lo};
    std::uint64_t       rem      = 0;
    U192                q{};

    for (int w = 0; w < 3; ++w) {
        for (int b = 63; b >= 0; --b) {
            rem   = (rem << 1) | ((limbs[w] >> b) & 1ULL);
            q.hi  = (q.hi << 1) | (q.mid >> 63);
            q.mid = (q.mid << 1) | (q.lo >> 63);
            q.lo  = (q.lo << 1);
            if (rem >= d) {
                rem -= d;
                q.lo |= 1ULL;
            }
        }
    }

    out.q = q;
    out.r = rem;
    return out;
}

// ceil( n1 * n2 * n3 / (d1 * d2) ), exact, saturating at Mojo max.
//
// WHY TWO SEPARATE 64-BIT DIVISIONS RATHER THAN ONE WIDE DIVISOR. The
// denominators do not fit in 64 bits either: on the live XCH/DBX pair
// base_mpu * kMojosPerXch = 1e12 * 1e12 = 1e24, which is 80 bits, and on
// wmilliETH.b/XCH price * quote_mpu reaches about 1.3e24. So a single
// mul_div() with a 64-bit divisor is impossible before overflow is even
// considered, and GCD cancellation does not rescue it (for XCH/DBX it leaves
// 1e21, still 70 bits). Splitting the divisor keeps every divide at <= 2^63-1,
// which is exactly the precondition divmod_u192 needs.
//
// Dividing twice is exact, and it also recovers the exact combined remainder:
//   floor(floor(N/d1)/d2) == floor(N/(d1*d2))  for integers; and writing
//   N = q1*d1 + r1, q1 = q2*d2 + r2 gives N = q2*d1*d2 + (r2*d1 + r1) with
//   r2*d1 + r1 <= (d2-1)*d1 + (d1-1) < d1*d2. So that bracket IS
//   N mod (d1*d2), hence
//
//       N mod (d1*d2) == 0   <=>   r1 == 0 AND r2 == 0
//       ceil(N/(d1*d2))      ==    q2 + (r1 != 0 || r2 != 0 ? 1 : 0)
//
// BOTH remainders must be tested. Dropping r1 is a real, silent fail-open:
// the 10^21+1 witness in the static_asserts below has r1 = 1000 and r2 = 0,
// so an r2-only test returns 1 where the true answer is 2.
//
// Note that swapping d1 and d2 is deliberately UNDETECTABLE -- the identity
// above is symmetric in d1 and d2, and so is the remainder disjunction. There
// is no test for that mutation because no test can exist. Do not go hunting
// for one.
[[nodiscard]] constexpr Mojo ceil_mul3_div2(std::uint64_t n1,
                                            std::uint64_t n2,
                                            std::uint64_t n3,
                                            std::uint64_t d1,
                                            std::uint64_t d2) noexcept
{
    if (n1 == 0 || n2 == 0 || n3 == 0) return 0;

    // ENFORCE divmod_u192's precondition rather than only documenting it.
    // That function is correct only for d < 2^63: its loop invariant is
    // rem < d, so a divisor >= 2^63 admits rem up to 2^64-2 and (rem << 1)
    // WRAPS, silently returning a garbage quotient that a caller would spend
    // against. Prose in a shipped header does not stop a future caller in
    // this namespace from passing a wider divisor, and there is no compiler
    // that would diagnose it.
    //
    // Both bounds are unreachable through quote_cost_for_ask /
    // base_size_for_bid today -- every divisor there is a positive Mojo
    // (int64, so <= 2^63-1), kMojosPerXch, or mpu_or_one() of an int64_t --
    // so the type system already enforces it and this costs two comparisons
    // on a path that runs a handful of times per block. Declining (rather
    // than asserting or wrapping) keeps the file's contract intact: zero
    // means UNPRICEABLE, and decide_funding() turns that into a decline.
    // It never means free.
    constexpr std::uint64_t kMaxDivisor =
        static_cast<std::uint64_t>(std::numeric_limits<Mojo>::max());
    if (d1 == 0 || d2 == 0) return 0;
    if (d1 > kMaxDivisor || d2 > kMaxDivisor) return 0;

    const U192      numerator = mul3_u64(n1, n2, n3);
    const DivResult step1     = divmod_u192(numerator, d1);
    const DivResult step2     = divmod_u192(step1.q, d2);

    const bool inexact = (step1.r != 0) || (step2.r != 0);

    // Saturation is an INTEGER comparison against an exactly-representable
    // bound. A previous bug in this repo clamped against a floating literal
    // that rounded ABOVE UINT64_MAX, making the narrowing cast undefined;
    // crossed_book.hpp:143-157 documents that rule and the old
    // ceil_to_mojo() in this very file violated it. There is no floating
    // literal anywhere here, so that entire failure class is structurally
    // unreachable rather than merely avoided.
    constexpr std::uint64_t kMojoMax =
        static_cast<std::uint64_t>(std::numeric_limits<Mojo>::max());

    if (step2.q.hi != 0 || step2.q.mid != 0) {
        return std::numeric_limits<Mojo>::max();
    }
    if (step2.q.lo >= kMojoMax) {
        // >= and not > : this also stops the +1 below from overflowing signed.
        return std::numeric_limits<Mojo>::max();
    }
    return static_cast<Mojo>(step2.q.lo) + (inexact ? 1 : 0);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Denomination normaliser. A non-positive mojos-per-unit is a broken pair
// config; treating it as 1 keeps the arithmetic defined and is what Step 9f
// has always done.
//
// BE PRECISE ABOUT WHAT PROTECTS THIS, because an earlier version of this
// comment claimed "the cap is normalised the same way at the call site
// (crossed_book.hpp:140)" and that is not what that line does. The two
// arguments are protected by two DIFFERENT mechanisms and one of them is only
// a config invariant:
//
//   * BASE mpu: a non-positive value is separately FATAL, not normalised.
//     crossed_book.hpp:140 is `if (base_mojos_per_unit <= 0) return 0;` --
//     cap_mojos_for DECLINES, so nothing is taken at all. Stronger than
//     normalisation, but a different mechanism; do not cite it as agreement.
//
//   * QUOTE mpu: NOTHING GUARDS IT. cap_mojos_for does not take a quote mpu
//     and never inspects one. With quote_mojos_per_unit <= 0, substituting 1
//     divides the true cost by the real quote mpu -- 1000x low on a CAT quote
//     -- and decide_funding() then sees an affordable price for a take we
//     cannot fund. That is the fail-open direction, with no cap compensating.
//     It is unreachable only because config.cpp:638-639 hard-assigns both
//     denominations from the asset id (1e12 for "xch", 1e3 otherwise) and can
//     never produce a non-positive one. If that derivation ever becomes
//     operator-supplied, this normaliser must become a decline.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr std::uint64_t mpu_or_one(std::int64_t mojos_per_unit) noexcept
{
    return static_cast<std::uint64_t>(mojos_per_unit > 0 ? mojos_per_unit : 1);
}

// ---------------------------------------------------------------------------
// quote_cost_for_ask -- what lifting an ASK costs us, in QUOTE mojos.
//
// We buy `base_size` base mojos at `price`. Prices are carried scaled by
// kMojosPerXch regardless of the pair's denominations, so the conversion is
//
//     quote = ceil( base_size * price * quote_mpu / (base_mpu * kMojosPerXch) )
//
// computed EXACTLY -- see the kernel above, and the banner at the top of this
// file for why it must never go back to floating point.
//
// @return 0 when the inputs cannot produce a usable cost. Zero means DO NOT
//         TAKE; it is not "free". take_retry.hpp's decide_funding() turns a
//         cost <= 0 into Unknown and declines, and that contract is now
//         STRICTLY STRONGER than it was: with all inputs positive, the
//         ceiling of a positive rational is always >= 1, so a returned 0 can
//         ONLY mean an input was non-positive. The float version could in
//         principle have reached 0 by underflow or a NaN intermediate; this
//         one structurally cannot.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr Mojo quote_cost_for_ask(Mojo         base_size,
                                                Mojo         price,
                                                std::int64_t base_mojos_per_unit,
                                                std::int64_t quote_mojos_per_unit) noexcept
{
    if (base_size <= 0 || price <= 0) return 0;
    return detail::ceil_mul3_div2(static_cast<std::uint64_t>(base_size),
                                  static_cast<std::uint64_t>(price),
                                  mpu_or_one(quote_mojos_per_unit),
                                  mpu_or_one(base_mojos_per_unit),
                                  static_cast<std::uint64_t>(kMojosPerXch));
}

// ---------------------------------------------------------------------------
// base_size_for_bid -- how much BASE we must DELIVER to hit a BID, in base
//                      mojos.
//
// The bid's advertised `size` is QUOTE mojos (the ingest convention above) --
// the maker offers quote and requests base -- so this is the inverse of
// quote_cost_for_ask:
//
//     base = ceil( quote_size * kMojosPerXch * base_mpu / (price * quote_mpu) )
//
// This is the number that must be compared against a base-denominated cap,
// charged against the base wallet, and recorded as record_taker_fill()'s
// base_mojos. Using the raw `size` there is a unit error, not a rounding one.
//
// It is an OUTFLOW, which is why it rounds UP exactly like the cost does. The
// comment here used to say a bid "delivers" this much base, which reads as an
// inflow and invites a future reader to flip the rounding on a spend. The bid
// does not deliver it to us; we deliver it to them.
//
// SCOPE, so that a commit message does not overclaim: making this conversion
// exact does not make the PATH exact.
//
// The true base owed is the offer's requested[0].amount. It is DISCARDED at
// the parse boundary: engine.cpp:3515-3523 is the only place `requested` is
// touched and it reads `.id` alone, to decide the side -- `.amount` is never
// read off the requested leg anywhere in engine.cpp. What Step 9 gets instead
// is a reconstruction through co.price, and engine.cpp:3546-3560 shows how
// lossy that is: for a bid the price is `1.0 / orec.price` (a reciprocal
// double) then llround()'d against kMojosPerXch, and the size comes from
// llround() of `orec.offered[0].amount * offered_denom`. That reconstruction
// error is larger than the one-ULP-of-the-result this exactness buys.
// Separate problem, not fixed here.
//
// And the ledger's QUOTE leg is still float: record_taker_fill derives
// quote_delta_mojos via quote_mojos_for() + llround (engine.cpp:14875-14879)
// from this now-exact base size, discarding the offer's own exact quote leg.
// Portable but inexact -- see the "WHAT IS STILL FLOAT" note in the banner.
//
// @return 0 when the inputs cannot produce a usable size. Zero means DO NOT
//         TAKE.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr Mojo base_size_for_bid(Mojo         quote_size,
                                               Mojo         price,
                                               std::int64_t base_mojos_per_unit,
                                               std::int64_t quote_mojos_per_unit) noexcept
{
    if (quote_size <= 0 || price <= 0) return 0;
    return detail::ceil_mul3_div2(static_cast<std::uint64_t>(quote_size),
                                  static_cast<std::uint64_t>(kMojosPerXch),
                                  mpu_or_one(base_mojos_per_unit),
                                  static_cast<std::uint64_t>(price),
                                  mpu_or_one(quote_mojos_per_unit));
}

// ---------------------------------------------------------------------------
// COMPILE-TIME GUARD. These are not decoration and they are not duplicates of
// the unit tests. They are evaluated by whichever compiler is BUILDING this
// header, so they hold on the GCC CI runner that this bug was only ever
// visible on, and they fail the BUILD rather than a test run.
//
// THERE ARE 9 OF THEM. That count is load-bearing and is quoted here on
// purpose: this branch's mutation harness rewrites this file in place, and
// the intermediate states it passes through have this block DELETED while
// leaving the comment headers in place. `grep -c '^static_assert'` returning
// anything other than 9 means you are looking at a partially-mutated file,
// not at the fix. Keep this number in step if you add one.
// ---------------------------------------------------------------------------

// The two assertions CI was red on (test_take_sizing.cpp:103 and :108). In
// long double GCC returned 1501 and 751 here. 1500 and 750 are exact.
static_assert(quote_cost_for_ask(1'000'000'000'000LL, 1'500'000'000'000LL,
                                 1'000'000'000'000LL, 1'000LL) == 1500);
static_assert(quote_cost_for_ask(500'000'000'000LL, 1'500'000'000'000LL,
                                 1'000'000'000'000LL, 1'000LL) == 750);

// The THIRD divergent assertion, which the CI report folded into the second
// test's name without separating out (test_take_sizing.cpp:153). In long
// double GCC returned 2000000000001 here.
static_assert(base_size_for_bid(3000LL, 1'500'000'000'000LL,
                                1'000'000'000'000LL, 1'000LL)
              == 2'000'000'000'000LL);

// THE FLOAT TRIPWIRE. 10000001 * 99999990000001 == 10^21 + 1 exactly, so the
// true value is 1.000000000000000000001 and the true ceiling is 2. Resolving
// that needs about 70 significand bits. Every floating format on every
// platform we build for -- MSVC's 53-bit long double AND GCC's 64-bit x87
// long double -- computes exactly 1.0 and answers 1. Unlike the 1500 case,
// this one is wrong on BOTH ABIs, so the guard is not itself
// platform-dependent: reintroduce a floating intermediate anywhere on this
// path and NOTHING compiles, ANYWHERE. It also kills a kernel mutation that
// tests only the second remainder (here r1 = 1000 and r2 = 0).
static_assert(quote_cost_for_ask(10'000'001LL, 99'999'990'000'001LL,
                                 1'000'000'000'000LL, 1'000LL) == 2);

// Saturation reached through the public API, and a 140-bit numerator that no
// 128-bit intermediate could carry (1e18 * 1e12 * 1e12 = 1e42, over a
// denominator of 1e24, with the result back in Mojo range).
static_assert(base_size_for_bid(9'223'372'036'854'775'807LL, 1LL,
                                1'000'000'000'000LL, 1LL)
              == std::numeric_limits<Mojo>::max());
static_assert(base_size_for_bid(1'000'000'000'000'000'000LL, 1'000'000'000'000LL,
                                1'000'000'000'000LL, 1'000'000'000'000LL)
              == 1'000'000'000'000'000'000LL);

// THE MIDDLE-LIMB CARRY IN mul3_u64. Pinned deterministically, because
// nothing else pins it and that was measured rather than assumed.
//
// Reinstate the carry bug (`result.hi = high.hi;`, dropping the
// `+ (result.mid < low.hi)` term) and EVERY other assertion in this block
// still passes, including the 1e42 case below whose comment used to claim it
// covered this. The only thing in the whole 1158-test suite that noticed was
// the seeded random sweep, at one iteration, on a geometry production cannot
// produce -- so re-seeding the sweep or dropping kMojoMax from its mpus[]
// array, both of which look like harmless test edits, would have silently
// left this line with zero coverage.
//
// It matters because this line IS the "three limbs always suffice" argument,
// which is the stated reason __int128 was rejected. Carrying it as a
// static_assert means a future compiler or a future edit fails the BUILD on
// both toolchains rather than depending on a random seed.
//
// The witness needs an adversarial denomination: structurally low.hi < c, so
// with production mpus in {1, 1e3, 1e12} the carry needs high.lo within 1e12
// of 2^64 -- about a 5e-8 chance, and 400,000 samples on the live ask
// geometry produce none. kMojoMax as an mpu is what makes it reachable.
//
//   N = 789718602219600096 * 1e12 * (2^63-1)  -- 163 bits, hi limb non-zero
//   D = 4600337460146979069 * (2^63-1)
//   exact ceiling      = 171665363479
//   with carry dropped = 171665363471   (8 low -- fail-open)
static_assert(base_size_for_bid(789'718'602'219'600'096LL,
                                4'600'337'460'146'979'069LL,
                                9'223'372'036'854'775'807LL,
                                9'223'372'036'854'775'807LL)
              == 171'665'363'479LL);

// Zero is DO NOT TAKE, never "free" -- decide_funding()'s contract.
static_assert(quote_cost_for_ask(0LL, 1'500'000'000'000LL, 1'000'000'000'000LL,
                                 1'000LL) == 0);
static_assert(base_size_for_bid(3000LL, 0LL, 1'000'000'000'000LL, 1'000LL) == 0);

}  // namespace xop::execution

#endif  // XOP_EXECUTION_TAKE_SIZING_HPP
