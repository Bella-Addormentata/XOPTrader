// ---------------------------------------------------------------------------
// [S40 follow-up] Step 9e clamped a take size and mis-read a bid's units.
//
// The clamp was S40 verbatim, in a second place:
//
//     const Mojo take_sz = std::min(c.size, max_mojos);
//
// take_offer(offer_text, fee) has no size parameter and a Chia offer cannot
// be partially filled, so this bounded nothing. Step 9e was WORSE than the
// Step 9c instance the original S40 report covered, because the clamped value
// did not only mislead a log -- it fed the pre-balance guard. For any offer
// larger than peg_arb_max_take_units the guard priced a fraction of the take,
// passed, and let take_offer() lift the whole offer we could not fund: a
// fail-open inside a guard whose only job is to stop unfundable takes. It
// then fed record_taker_fill(), synthesising both double-entry ledger legs
// from a size the RPC never saw.
//
// The second defect was independent of the clamp. Ingest denominates a BID's
// size in the QUOTE asset and an ASK's in the BASE asset
// (market_data.cpp:1933-1937). Step 9e applied one base-denominated formula
// to both sides, so on any pair whose base and quote denominations differ,
// the cap comparison, the funding estimate and the ledger entry were all in
// the wrong unit. On BYC/wUSDC.b it was invisible: base and quote
// mojos-per-unit are coincidentally both 1000.
//
// Step 9f had this right, inline, in lambdas no test could reach -- the only
// correct copy in the file, twenty lines from the wrong one. take_sizing.hpp
// is that maths extracted; 9e and 9f now share it and these tests hold it.
//
// The rounding direction is part of the contract, not an implementation
// detail: UP, always. Both functions compute an OUTFLOW -- what we must be
// able to pay, and what we must hand over -- in different assets. Rounding a
// cost down invents affordability.
//
// ===========================================================================
// [2026-09-01] AND THE ARITHMETIC ITSELF IS NOW EXACT INTEGER ARITHMETIC.
// ===========================================================================
// take_sizing.hpp used to do both conversions in `long double`, which is
// 64-bit (== double) on MSVC and 80-bit x87 on GCC/x86-64. That made this
// file's own first assertion platform-dependent: the exact answer below is
// 1500, MSVC produced 1500 and GCC produced 1501, and CI was red on GCC only.
// MSVC was right BY LUCK -- the intermediate needs 65 significand bits and
// neither format has them; MSVC's two rounding errors happened to cancel.
//
// Nothing in the expectations below was changed to accommodate that. Every
// literal here was already the mathematically exact answer; the code moved to
// the tests, not the other way round. Three assertions were red on GCC and
// all three are unedited: AskCostOnAnXchBaseCatQuotePair (both of them) and
// BidSizeIsConvertedFromQuoteMojosToBaseMojos.
//
// The suite below adds what the old one could not have caught:
//
//   * a DIFFERENTIAL sweep against a reference implemented by a deliberately
//     DIFFERENT method -- base-10 schoolbook arithmetic with repeated-
//     subtraction long division -- so the implementation cannot validate
//     itself. The reference also divides by the FULL combined denominator in
//     one pass, where the implementation splits it into two 64-bit divisions,
//     so the decomposition and its remainder logic are independently checked.
//
//   * a FLOATING-POINT TRIPWIRE whose exact answer no floating format on
//     either ABI can reach. See TakeSizingExact.FloatingPointWouldFailThis.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "xop/execution/take_sizing.hpp"

using xop::kMojosPerXch;
using xop::Mojo;

// ===========================================================================
// [2026-09-02] TEST-LOCAL UNTYPED ADAPTERS, AND WHY THEY ARE THE RIGHT CHOICE
// HERE RATHER THAN A DODGE.
//
// quote_cost_for_ask and base_size_for_bid now take BaseMojos/QuoteMojos and
// BaseMpu/QuoteMpu (see cpp/include/xop/util/denom.hpp). Roughly 60 assertions
// in this file pass bare integer literals and compare against bare integer
// literals. There were two ways to absorb that:
//
//   (a) wrap all ~60 call sites and all ~60 expected values by hand, or
//   (b) adapt at ONE point and leave every assertion byte-identical.
//
// (b) IS CHOSEN, for three reasons, and the third is the decisive one:
//
//   1. THE ACCEPTANCE CRITERION FOR THE RETYPE IS "NO VALUE MOVED". The strong
//      typedefs are constexpr wrappers that compile away, so the arithmetic must
//      be bit-identical; if it is not, the type change broke the maths. Leaving
//      the assertions untouched makes that property auditable by `git diff` --
//      the diff for this file shows ZERO changed numbers. Hand-wrapping 60 sites
//      would bury that signal in 120 lines of mechanical churn and give a typo
//      the chance to mask a real change.
//
//   2. THIS FILE'S JOB IS ARITHMETIC, NOT DENOMINATION. It is the exact-integer
//      kernel's differential test against an independent decimal long-division
//      reference. The denomination guarantee is proven where it belongs: by the
//      concept-based static_asserts in take_sizing.hpp's ACCEPTANCE block
//      (which fail the BUILD, on both toolchains, not a test run) and by
//      cpp/tests/test_denom.cpp.
//
//   3. IT PRESERVES THE MIDDLE-LIMB CARRY WITNESS. The differential and seeded
//      sweeps below feed ONE loop variable to BOTH functions, which cannot be
//      done with strong types -- a single value cannot be BaseMojos and
//      QuoteMojos at once. Splitting those loops into two typed variables is
//      exactly the "harmless test edit" that take_sizing.hpp:596-621 warns
//      silently drops the only runtime coverage of the mul3_u64 carry. Adapting
//      instead of splitting keeps that coverage intact.
//
// These adapters take bare integers ON PURPOSE. They are the boundary; nothing
// below them is denomination-aware, and nothing below them needs to be.
// ===========================================================================
namespace {

[[nodiscard]] constexpr Mojo quote_cost_for_ask(Mojo         base_size,
                                                Mojo         price,
                                                std::int64_t base_mpu,
                                                std::int64_t quote_mpu) noexcept
{
    return xop::execution::quote_cost_for_ask(xop::BaseMojos{base_size}, price,
                                              xop::BaseMpu{base_mpu},
                                              xop::QuoteMpu{quote_mpu}).v;
}

[[nodiscard]] constexpr Mojo base_size_for_bid(Mojo         quote_size,
                                               Mojo         price,
                                               std::int64_t base_mpu,
                                               std::int64_t quote_mpu) noexcept
{
    return xop::execution::base_size_for_bid(xop::QuoteMojos{quote_size}, price,
                                             xop::BaseMpu{base_mpu},
                                             xop::QuoteMpu{quote_mpu}).v;
}


// The live denominations (config.cpp:638): XCH bases are 1e12 mojos/unit,
// CATs are 1000.
constexpr std::int64_t kXchMpu = 1'000'000'000'000LL;
constexpr std::int64_t kCatMpu = 1'000LL;

constexpr Mojo kMojoMax = std::numeric_limits<Mojo>::max();

// ===========================================================================
// AN INDEPENDENT REFERENCE, BY A DIFFERENT METHOD ON PURPOSE.
//
// take_sizing.hpp computes in binary: 32-bit limbs, 64x64->128 multiplies,
// and restoring binary long division applied TWICE (once per denominator
// factor), inferring exactness from two separate remainders.
//
// Everything below is base TEN: schoolbook digit multiplication and classical
// long division by repeated subtraction, dividing by the SINGLE combined
// denominator d1*d2. It shares no code, no radix, no algorithm and no
// exactness argument with the implementation. If both agree on a value, they
// agree for a reason.
//
// Digits are little-endian (index 0 is the units digit) with no leading
// zeros, except that zero itself is {0}.
// ===========================================================================
using Dec = std::vector<int>;

Dec dec_trim(Dec v)
{
    while (v.size() > 1 && v.back() == 0) v.pop_back();
    return v;
}

Dec dec_from_u64(std::uint64_t x)
{
    if (x == 0) return Dec{0};
    Dec v;
    while (x != 0) {
        v.push_back(static_cast<int>(x % 10U));
        x /= 10U;
    }
    return v;
}

Dec dec_mul(const Dec& a, const Dec& b)
{
    Dec r(a.size() + b.size(), 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        int carry = 0;
        for (std::size_t j = 0; j < b.size(); ++j) {
            const int cur = r[i + j] + a[i] * b[j] + carry;
            r[i + j]      = cur % 10;
            carry         = cur / 10;
        }
        std::size_t k = i + b.size();
        while (carry != 0) {
            const int cur = r[k] + carry;
            r[k]          = cur % 10;
            carry         = cur / 10;
            ++k;
        }
    }
    return dec_trim(r);
}

// -1 / 0 / +1
int dec_cmp(const Dec& a, const Dec& b)
{
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (std::size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

// a -= b, requires a >= b.
void dec_sub_inplace(Dec& a, const Dec& b)
{
    int borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        int cur = a[i] - borrow - (i < b.size() ? b[i] : 0);
        if (cur < 0) {
            cur += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        a[i] = cur;
    }
    while (a.size() > 1 && a.back() == 0) a.pop_back();
}

// Classical long division, one quotient digit at a time, each digit found by
// repeated subtraction. Sets `remainder_nonzero` -- that single flag is the
// reference's whole notion of inexactness, where the implementation has to
// combine two separate remainders to reach the same conclusion.
Dec dec_divmod(const Dec& n, const Dec& d, bool& remainder_nonzero)
{
    Dec q(n.size(), 0);
    Dec rem{0};
    for (std::size_t i = n.size(); i-- > 0;) {
        rem.insert(rem.begin(), n[i]);
        while (rem.size() > 1 && rem.back() == 0) rem.pop_back();
        int digit = 0;
        while (dec_cmp(rem, d) >= 0) {
            dec_sub_inplace(rem, d);
            ++digit;
        }
        q[i] = digit;
    }
    remainder_nonzero = !(rem.size() == 1 && rem[0] == 0);
    return dec_trim(q);
}

// The reference reports not just the value but WHETHER the ceiling actually
// fired and whether the bound was hit. The sweeps below use those two flags to
// prove they are not vacuous: a sweep that only ever produced exact, in-range
// quotients would agree with a truncating or non-saturating implementation
// just as happily as with a correct one.
struct RefResult {
    Mojo value{0};
    bool inexact{false};     // a real ceiling was applied
    bool saturated{false};   // the true quotient exceeded Mojo max
};

// Narrow a decimal quotient to a Mojo, applying the ceiling and saturating at
// Mojo max -- the same contract the implementation promises, reached by
// digit-wise accumulation rather than by inspecting 64-bit limbs.
RefResult dec_to_mojo_ceil(const Dec& q, bool inexact)
{
    constexpr std::uint64_t kMax = static_cast<std::uint64_t>(kMojoMax);
    RefResult               out;
    out.inexact = inexact;

    std::uint64_t v = 0;
    for (std::size_t i = q.size(); i-- > 0;) {
        if (v > kMax / 10U) {
            out.value     = kMojoMax;
            out.saturated = true;
            return out;
        }
        v *= 10U;
        const std::uint64_t digit = static_cast<std::uint64_t>(q[i]);
        if (v > kMax - digit) {
            out.value     = kMojoMax;
            out.saturated = true;
            return out;
        }
        v += digit;
    }
    if (v >= kMax) {
        out.value     = kMojoMax;
        out.saturated = true;
        return out;
    }
    out.value = static_cast<Mojo>(v) + (inexact ? 1 : 0);
    return out;
}

std::uint64_t ref_mpu_or_one(std::int64_t mpu)
{
    return static_cast<std::uint64_t>(mpu > 0 ? mpu : 1);
}

RefResult ref_ask(Mojo         base_size,
                  Mojo         price,
                  std::int64_t base_mpu,
                  std::int64_t quote_mpu)
{
    if (base_size <= 0 || price <= 0) return RefResult{};
    const Dec num =
        dec_mul(dec_mul(dec_from_u64(static_cast<std::uint64_t>(base_size)),
                        dec_from_u64(static_cast<std::uint64_t>(price))),
                dec_from_u64(ref_mpu_or_one(quote_mpu)));
    const Dec den = dec_mul(dec_from_u64(ref_mpu_or_one(base_mpu)),
                            dec_from_u64(static_cast<std::uint64_t>(kMojosPerXch)));
    bool      inexact = false;
    const Dec quo     = dec_divmod(num, den, inexact);
    return dec_to_mojo_ceil(quo, inexact);
}

RefResult ref_bid(Mojo         quote_size,
                  Mojo         price,
                  std::int64_t base_mpu,
                  std::int64_t quote_mpu)
{
    if (quote_size <= 0 || price <= 0) return RefResult{};
    const Dec num =
        dec_mul(dec_mul(dec_from_u64(static_cast<std::uint64_t>(quote_size)),
                        dec_from_u64(static_cast<std::uint64_t>(kMojosPerXch))),
                dec_from_u64(ref_mpu_or_one(base_mpu)));
    const Dec den = dec_mul(dec_from_u64(static_cast<std::uint64_t>(price)),
                            dec_from_u64(ref_mpu_or_one(quote_mpu)));
    bool      inexact = false;
    const Dec quo     = dec_divmod(num, den, inexact);
    return dec_to_mojo_ceil(quo, inexact);
}

Mojo ref_quote_cost_for_ask(Mojo         base_size,
                            Mojo         price,
                            std::int64_t base_mpu,
                            std::int64_t quote_mpu)
{
    return ref_ask(base_size, price, base_mpu, quote_mpu).value;
}

Mojo ref_base_size_for_bid(Mojo         quote_size,
                           Mojo         price,
                           std::int64_t base_mpu,
                           std::int64_t quote_mpu)
{
    return ref_bid(quote_size, price, base_mpu, quote_mpu).value;
}

// Deterministic PRNG (splitmix64). A unit test that cannot be replayed from
// its own source is not evidence.
class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept
    {
        state_ += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    // Uniform in [lo, hi], both inclusive, lo <= hi.
    Mojo in_range(Mojo lo, Mojo hi) noexcept
    {
        const std::uint64_t span =
            static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo) + 1U;
        if (span == 0) return static_cast<Mojo>(next() & 0x7FFF'FFFF'FFFF'FFFFULL);
        return lo + static_cast<Mojo>(next() % span);
    }

private:
    std::uint64_t state_;
};

}  // namespace

// -- the three assertions that were red on GCC, unedited ---------------------

TEST(TakeSizing, AskCostOnAnXchBaseCatQuotePair)
{
    // XCH/wUSDC.b: base 1e12 mpu, quote 1000 mpu. Price is carried scaled by
    // kMojosPerXch, so price 1.5e12 means 1.5 wUSDC per XCH.
    //   1 XCH = 1e12 base mojos, at 1.5 -> 1.5 wUSDC = 1500 quote mojos.
    //
    // 1500 is EXACT: 1e12 * 1.5e12 * 1e3 / (1e12 * 1e12) has no fractional
    // part at all. In long double GCC returned 1501 here and MSVC returned
    // 1500; the expectation was always right and neither compiler was
    // trustworthy. Do not loosen this to a band.
    EXPECT_EQ(quote_cost_for_ask(kXchMpu, 1'500'000'000'000LL,
                                 kXchMpu, kCatMpu),
              1500);

    // Half an XCH costs half as much. GCC returned 751 here in long double.
    EXPECT_EQ(quote_cost_for_ask(kXchMpu / 2, 1'500'000'000'000LL,
                                 kXchMpu, kCatMpu),
              750);
}

TEST(TakeSizing, AskCostOnACatCatPairMatchesTheLedger)
{
    // BYC/wUSDC.b: both 1000 mpu, price 1.001 -> 1.001e12 scaled.
    // 5000 base mojos (5 BYC) costs 5005 quote mojos -- exactly the
    // quote_delta the four 2026-08-06 taker_fills rows carry.
    EXPECT_EQ(quote_cost_for_ask(5000, 1'001'000'000'000LL, kCatMpu, kCatMpu),
              5005);
}

TEST(TakeSizing, AskCostRoundsUpNeverDown)
{
    // 1 base mojo at 1.5 on a CAT/CAT pair is 1.5 quote mojos. Rounding that
    // down to 1 would tell the pre-balance guard we can afford something we
    // cannot.
    EXPECT_EQ(quote_cost_for_ask(1, 1'500'000'000'000LL, kCatMpu, kCatMpu), 2);
}

TEST(TakeSizing, AskCostDeclinesOnUnusableInputs)
{
    // Zero is DO NOT TAKE, not "free". Every call site treats a zero cost as
    // a reason to skip.
    EXPECT_EQ(quote_cost_for_ask(0, 1'500'000'000'000LL, kXchMpu, kCatMpu), 0);
    EXPECT_EQ(quote_cost_for_ask(kXchMpu, 0, kXchMpu, kCatMpu), 0);
    EXPECT_EQ(quote_cost_for_ask(-5, 1'500'000'000'000LL, kXchMpu, kCatMpu), 0);
    EXPECT_EQ(quote_cost_for_ask(kXchMpu, -1, kXchMpu, kCatMpu), 0);
}

// -- base_size_for_bid ------------------------------------------------------

TEST(TakeSizing, BidSizeIsConvertedFromQuoteMojosToBaseMojos)
{
    // THE BUG THIS FUNCTION EXISTS FOR. On XCH/wUSDC.b a bid advertising
    // "3000" is advertising 3000 QUOTE mojos (3 wUSDC), not 3000 base mojos.
    // At 1.5 wUSDC per XCH that is 2 XCH = 2e12 base mojos.
    //
    // Step 9e used the raw 3000 as base mojos: against a 50-unit cap of
    // 5e13 base mojos it looked like a rounding error, when the real
    // delivery was 2e12. Nine orders of magnitude, in the number that is
    // compared to the cap, charged to the wallet, and written to both
    // ledger legs.
    //
    // 2e12 is EXACT. In long double GCC returned 2000000000001 -- the third
    // divergent assertion, and the one the CI report did not separate out.
    EXPECT_EQ(base_size_for_bid(3000, 1'500'000'000'000LL, kXchMpu, kCatMpu),
              2 * kXchMpu);
}

TEST(TakeSizing, BidSizeIsTheInverseOfAskCost)
{
    // Round-tripping must not lose the base size. Both directions round up,
    // so the inverse is >= the original; it must not come back smaller.
    //
    // Under exact arithmetic this now holds BY PROOF rather than by luck:
    // cost >= base*P*Q/(B*M) implies back >= cost*M*B/(P*Q) >= base.
    const Mojo base  = 7 * kXchMpu;
    const Mojo price = 1'234'000'000'000LL;

    const Mojo quote = quote_cost_for_ask(base, price, kXchMpu, kCatMpu);
    ASSERT_GT(quote, 0);

    const Mojo back = base_size_for_bid(quote, price, kXchMpu, kCatMpu);
    EXPECT_GE(back, base);
    EXPECT_LT(back - base, kXchMpu / 1000)
        << "the round trip drifted by more than the rounding can explain";
}

TEST(TakeSizing, BidSizeOnACatCatPairIsUnchangedWhenDenominationsMatch)
{
    // Why the defect was invisible in production: BYC/wUSDC.b has base mpu ==
    // quote mpu == 1000, so at price 1.0 the conversion is the identity and
    // the wrong formula gave the right answer. This test exists so nobody
    // "confirms" the old behaviour from this pair alone.
    EXPECT_EQ(base_size_for_bid(5000, kMojosPerXch, kCatMpu, kCatMpu), 5000);
}

TEST(TakeSizing, BidSizeDeclinesOnUnusableInputs)
{
    EXPECT_EQ(base_size_for_bid(0, 1'500'000'000'000LL, kXchMpu, kCatMpu), 0);
    EXPECT_EQ(base_size_for_bid(3000, 0, kXchMpu, kCatMpu), 0)
        << "a zero price must not divide";
    EXPECT_EQ(base_size_for_bid(3000, -1, kXchMpu, kCatMpu), 0);
    EXPECT_EQ(base_size_for_bid(-3000, 1'500'000'000'000LL, kXchMpu, kCatMpu), 0);
}

TEST(TakeSizing, ABrokenPairDenominationDoesNotProduceGarbage)
{
    // A non-positive mojos-per-unit is a broken pair config. It must not
    // divide by zero or produce a negative size; it normalises to 1, which is
    // what Step 9f has always done.
    EXPECT_GT(quote_cost_for_ask(1000, kMojosPerXch, 0, kCatMpu), 0);
    EXPECT_GT(base_size_for_bid(1000, kMojosPerXch, kXchMpu, 0), 0);
    EXPECT_GT(quote_cost_for_ask(1000, kMojosPerXch, -5, -5), 0);

    // The normalisation is to 1, not to "decline" and not to some other
    // constant. Pin the actual values so a future edit cannot quietly change
    // which constant a broken config falls back to.
    EXPECT_EQ(quote_cost_for_ask(1000, kMojosPerXch, 0, kCatMpu),
              ref_quote_cost_for_ask(1000, kMojosPerXch, 0, kCatMpu));
    EXPECT_EQ(base_size_for_bid(1000, kMojosPerXch, kXchMpu, 0),
              ref_base_size_for_bid(1000, kMojosPerXch, kXchMpu, 0));
    EXPECT_EQ(quote_cost_for_ask(1000, kMojosPerXch, -5, -5),
              ref_quote_cost_for_ask(1000, kMojosPerXch, -5, -5));
}

// -- the property that makes the cap a filter -------------------------------

TEST(TakeSizing, TheConvertedBidSizeIsWhatACapMustBeComparedAgainst)
{
    // A synthetic sweep standing in for the 9e call site: whatever the
    // denominations, the number handed to the cap comparison must be base
    // mojos. The old code compared the raw quote size, which is a different
    // quantity whenever the denominations differ -- so the cap either barely
    // bound anything or bound far too much, depending on which way the ratio
    // went.
    const std::int64_t mpus[]   = {kXchMpu, kCatMpu};
    const Mojo         prices[] = {kMojosPerXch / 2, kMojosPerXch,
                                   2 * kMojosPerXch};
    const Mojo         sizes[]  = {1, 1000, 5'000'000LL};

    int differed = 0;
    for (const std::int64_t base_mpu : mpus) {
        for (const std::int64_t quote_mpu : mpus) {
            for (const Mojo price : prices) {
                for (const Mojo quote_size : sizes) {
                    const Mojo base = base_size_for_bid(quote_size, price,
                                                        base_mpu, quote_mpu);
                    ASSERT_GT(base, 0)
                        << "base_mpu=" << base_mpu << " quote_mpu=" << quote_mpu
                        << " price=" << price << " size=" << quote_size;
                    if (base != quote_size) ++differed;
                }
            }
        }
    }

    // Guard against a vacuous pass: if the conversion were the identity in
    // every case, this test would prove nothing about the unit error.
    EXPECT_GT(differed, 0);
}

// ===========================================================================
// EXACTNESS. Everything below was added on 2026-09-01 with the integer
// rewrite. The old suite had two tests of a `ceil_to_mojo(long double)`
// helper; that helper is gone, and the properties it stood for -- round up,
// saturate rather than narrow out of range, refuse nonsense -- are pinned
// here in the integer domain instead. They are NOT deleted, they are moved:
// a tested-but-dead float helper on this header would be an invitation to
// wire it back in.
// ===========================================================================

TEST(TakeSizingExact, FloatingPointWouldFailThis)
{
    // THE TRIPWIRE. This is the test to look at first if you are about to
    // "simplify" take_sizing.hpp back to a floating expression.
    //
    //     10000001 * 99999990000001 == 10^21 + 1, EXACTLY.
    //
    // So the true value of the ask conversion is
    //     (10^21 + 1) * 1e3 / (1e12 * 1e12) = 1.000000000000000000001
    // and the true ceiling is 2, not 1. Distinguishing that from 1.0 needs
    // about 70 bits of significand. MSVC's long double has 53. GCC's x87
    // long double has 64. IEEE binary128, were it available, would have 113
    // and would get this right -- but neither of our compilers offers it on
    // this target.
    //
    // So unlike the 1500 case, which is wrong on GCC only, this one is wrong
    // on BOTH ABIs: any float implementation returns 1 everywhere. The guard
    // is therefore not itself platform-dependent, which is the whole point --
    // it fails on the developer's machine, not only on the CI runner nobody
    // can reproduce locally.
    EXPECT_EQ(quote_cost_for_ask(10'000'001LL, 99'999'990'000'001LL,
                                 kXchMpu, kCatMpu),
              2)
        << "a floating-point intermediate has been reintroduced: this returns "
           "1 in double AND in x87 long double";

    // Note that take_sizing.hpp also pins this as a static_assert, so a float
    // rewrite fails to COMPILE before it ever reaches this test. This runtime
    // copy exists so the failure has a message attached to it.

    // A bid-side witness, also wrong on BOTH ABIs. This one could not be
    // constructed the way the ask witness was, and the reason is worth
    // recording: the ask's denominator is base_mpu * kMojosPerXch, which can
    // be 1e24, whereas the bid's is price * quote_mpu with price <= 2^63-1.
    // Writing N = k*D + r, the smallest r that keeps N a multiple of
    // kMojosPerXch * base_mpu forces r/(k*D) >= 2^-63 -- exactly a factor of
    // two too coarse to hide from x87's 64-bit significand. So there is no
    // SMALL both-ABI bid witness at all; this one was found by search over
    // large operands instead, on the live XCH/CAT geometry.
    //
    //   exact  7160404810233565666
    //   MSVC   7160404810233565184   (482 low)
    //   GCC    7160404810233565665   (1 low)
    //
    // Note the direction: BOTH platforms come in UNDER the true ceiling. That
    // is the fail-open direction -- a delivery obligation understated, or a
    // cost we are short of but believe we can pay.
    EXPECT_EQ(base_size_for_bid(55'307'012'408'681'172LL,
                                7'724'006'375'957'549'991LL,
                                kXchMpu, kCatMpu),
              7'160'404'810'233'565'666LL)
        << "a floating-point intermediate has been reintroduced on the bid "
           "path";

    // And the two CI witnesses again, stated as exactness rather than as
    // "what the old code happened to produce".
    EXPECT_EQ(quote_cost_for_ask(kXchMpu, 1'500'000'000'000LL, kXchMpu, kCatMpu),
              1500);
    EXPECT_EQ(base_size_for_bid(3000, 1'500'000'000'000LL, kXchMpu, kCatMpu),
              2'000'000'000'000LL);
}

TEST(TakeSizingExact, IntermediatesExceedOneHundredAndTwentyEightBits)
{
    // This is why the fix could not just be `unsigned __int128` behind an
    // #ifdef, quite apart from portability.
    //
    //   numerator   = 1e18 * 1e12 * 1e12 = 1e42  -> 140 bits
    //   denominator = 1e12 * 1e12        = 1e24  ->  80 bits
    //   quotient    = 1e18                       -> comfortably in range
    //
    // A 128-bit intermediate wraps on the numerator and cannot produce this
    // answer. So does narrowing the division loop to 128 iterations, which
    // would skip the high limb entirely.
    //
    // CORRECTION, measured rather than assumed: this case does NOT cover
    // dropping the carry out of the middle limb, which an earlier version of
    // this comment claimed it did. Reinstate that mutation
    // (`result.hi = high.hi;` in mul3_u64) and this assertion still passes --
    // as does every other assertion in this file except one iteration of the
    // seeded random sweep below. See CarryOutOfTheMiddleLimbIsPinned.
    EXPECT_EQ(base_size_for_bid(1'000'000'000'000'000'000LL, kXchMpu,
                                kXchMpu, kXchMpu),
              1'000'000'000'000'000'000LL);

    // Same on the ask side: 1e18 * 1e18 * 1e3 = 1e39 (130 bits), over 1e24.
    EXPECT_EQ(quote_cost_for_ask(1'000'000'000'000'000'000LL,
                                 1'000'000'000'000'000'000LL,
                                 kXchMpu, kCatMpu),
              1'000'000'000'000'000LL);

    // And the widest thing the domain admits: three factors just under 2^63
    // is a ~189-bit product. It must saturate cleanly rather than wrap into
    // something small and affordable-looking.
    EXPECT_EQ(quote_cost_for_ask(kMojoMax, kMojoMax, 1, kMojoMax), kMojoMax);
}

TEST(TakeSizingExact, CarryOutOfTheMiddleLimbIsPinned)
{
    // mul3_u64 adds the carry out of the middle limb into the high limb:
    //
    //     result.mid = low.hi + high.lo;
    //     result.hi  = high.hi + ((result.mid < low.hi) ? 1ULL : 0ULL);
    //
    // That second line is the whole "three limbs always suffice" argument,
    // which is the stated reason __int128 was rejected. Before this test it
    // was pinned by NOTHING deterministic. Verified by mutation: delete the
    // carry term and the entire suite stays green except one iteration of
    // MatchesAnIndependentDecimalReferenceUnderRandomSweep -- so re-seeding
    // that sweep, or dropping kMojoMax from its mpus[] array, would have
    // removed the last coverage of this line without looking like it.
    //
    // The witness needs an adversarial denomination. Structurally
    // low.hi < c, so with production mpus in {1, kCatMpu, kXchMpu} the carry
    // requires high.lo within kXchMpu of 2^64 -- roughly a 5e-8 chance, and
    // 400,000 samples on the live ask geometry produce none. This line is
    // defensive, not live; that is exactly why it needs a pin rather than a
    // sweep.
    //
    //   N = 789718602219600096 * 1e12 * (2^63-1)   -- 163 bits
    //   D = 4600337460146979069 * (2^63-1)
    //   exact ceiling      171665363479
    //   carry dropped      171665363471   (8 LOW -- the fail-open direction)
    EXPECT_EQ(base_size_for_bid(789'718'602'219'600'096LL,
                                4'600'337'460'146'979'069LL,
                                kMojoMax, kMojoMax),
              171'665'363'479LL)
        << "the carry out of mul3_u64's middle limb has been dropped";

    // take_sizing.hpp pins the same value as a static_assert, so the carry
    // bug fails the BUILD on both toolchains. This runtime copy exists so the
    // failure carries a message.
}

TEST(TakeSizingExact, DivisorsWiderThanTheMojoBoundDecline)
{
    // divmod_u192 is correct only for d < 2^63: its loop invariant is
    // rem < d, so a divisor at or above 2^63 admits rem up to 2^64-2 and
    // (rem << 1) wraps, yielding a garbage quotient. ceil_mul3_div2 now
    // enforces that precondition instead of only documenting it.
    //
    // Unreachable through the two public functions -- every divisor they pass
    // is a positive Mojo, kMojosPerXch, or mpu_or_one() of an int64_t, all
    // <= 2^63-1 -- so this exercises the kernel directly. The contract under
    // test is that a violating divisor DECLINES (0 = unpriceable) rather than
    // returning a wrapped number that a caller would spend against. Zero is
    // never "free"; decide_funding() turns it into Unknown.
    using xop::execution::detail::ceil_mul3_div2;

    constexpr std::uint64_t kJustOverMojoMax = 1ULL << 63;

    EXPECT_EQ(ceil_mul3_div2(1000, 1000, 1000, kJustOverMojoMax, 1), 0)
        << "a divisor >= 2^63 must decline, not wrap divmod_u192's remainder";
    EXPECT_EQ(ceil_mul3_div2(1000, 1000, 1000, 1, kJustOverMojoMax), 0);
    EXPECT_EQ(ceil_mul3_div2(1000, 1000, 1000, ~0ULL, 1), 0);

    // The boundary itself is still a live, ordinary divisor -- the guard is
    // > kMojoMax, not >= -- so kMojoMax must keep computing normally.
    EXPECT_EQ(ceil_mul3_div2(static_cast<std::uint64_t>(kMojoMax), 1, 1,
                             static_cast<std::uint64_t>(kMojoMax), 1),
              1);
}

TEST(TakeSizingExact, SaturatesAtTheMojoBoundInsteadOfNarrowingOutOfRange)
{
    // Replaces the old CeilSaturatesRatherThanNarrowingOutOfRange, which
    // tested the deleted ceil_to_mojo(long double) directly.
    //
    // The prior bug this guards was a clamp compared against a floating
    // literal that ROUNDED ABOVE UINT64_MAX, making the narrowing cast
    // undefined -- it passed MSVC and failed GCC. There is no floating
    // literal on this path any more, so the bound is an exact integer and the
    // failure class is structurally unreachable. Saturation is safe because
    // every caller compares the result against a cap it cannot exceed, so a
    // saturated value declines.

    // Vastly over: 2^63-1 * 1e12 * 1e12 / 1 is about 9.2e42.
    EXPECT_EQ(base_size_for_bid(kMojoMax, 1, kXchMpu, 1), kMojoMax);
    EXPECT_EQ(quote_cost_for_ask(kMojoMax, kMojoMax, 1, kXchMpu), kMojoMax);

    // Exactly ON the bound, from below and from above. base_size_for_bid with
    // price == kMojosPerXch and matched mpus is the identity, so this asks for
    // precisely Mojo max and precisely Mojo max - 1.
    EXPECT_EQ(base_size_for_bid(kMojoMax, kMojosPerXch, kCatMpu, kCatMpu),
              kMojoMax);
    EXPECT_EQ(base_size_for_bid(kMojoMax - 1, kMojosPerXch, kCatMpu, kCatMpu),
              kMojoMax - 1);

    // THE ONE THAT MAKES THE BOUND TEST `>=` AND NOT `>`. Here the floor of
    // the true quotient is EXACTLY Mojo max and the remainder is non-zero, so
    // the ceiling wants Mojo max + 1 -- a value that does not exist. If the
    // saturation check were `> kMojoMax` instead of `>=`, this would fall
    // through to `static_cast<Mojo>(q) + 1`, which is signed overflow: UB,
    // and in practice a large negative number that reads as affordable.
    //
    // These two are hard to find and worth keeping. The window is a razor's
    // edge: the numerator is always a multiple of kMojosPerXch * mpu, so a
    // multiple of it has to land inside an interval of width D < that same
    // multiple. Both were located by search, not by construction.
    //   bid: 9223372036845552435 * 1e12 / 999999999999
    //        floor = 9223372036854775807, remainder = 36854775807
    EXPECT_EQ(base_size_for_bid(9'223'372'036'845'552'435LL, 999'999'999'999LL,
                                1, 1),
              kMojoMax);
    //   ask: 9223372036845552435 * 1000000000001 * 1e12 / (1e12 * 1e12)
    //        floor = 9223372036854775807, remainder non-zero
    EXPECT_EQ(quote_cost_for_ask(9'223'372'036'845'552'435LL,
                                 1'000'000'000'001LL, kXchMpu, kXchMpu),
              kMojoMax);
    EXPECT_GT(base_size_for_bid(9'223'372'036'845'552'435LL, 999'999'999'999LL,
                                1, 1),
              0)
        << "a wrapped saturation reads as affordable";

    // Everything the implementation can return is a legal Mojo.
    EXPECT_LE(quote_cost_for_ask(kMojoMax, kMojoMax, 1, kMojoMax), kMojoMax);
    EXPECT_GE(quote_cost_for_ask(kMojoMax, kMojoMax, 1, kMojoMax), 0);
}

TEST(TakeSizingExact, ZeroMeansUnpriceableAndPositiveInputsNeverProduceIt)
{
    // Replaces the old CeilRoundsUpAndRefusesNonsense. take_retry.hpp's
    // decide_funding() reads cost <= 0 as Unknown and DECLINES; this is the
    // contract that makes an unpriceable take a refusal rather than a free
    // one, and it is the most dangerous thing in this file to weaken.

    // Non-positive inputs decline, on both functions, in every argument
    // position that can carry one.
    EXPECT_EQ(quote_cost_for_ask(0, kMojosPerXch, kXchMpu, kCatMpu), 0);
    EXPECT_EQ(quote_cost_for_ask(kXchMpu, 0, kXchMpu, kCatMpu), 0);
    EXPECT_EQ(quote_cost_for_ask(kMojoMax * -1, kMojosPerXch, kXchMpu, kCatMpu), 0);
    EXPECT_EQ(base_size_for_bid(0, kMojosPerXch, kXchMpu, kCatMpu), 0);
    EXPECT_EQ(base_size_for_bid(kXchMpu, 0, kXchMpu, kCatMpu), 0);

    // The most negative Mojo has no positive counterpart; it must still
    // decline rather than casting into a huge unsigned value.
    EXPECT_EQ(quote_cost_for_ask(std::numeric_limits<Mojo>::min(),
                                 kMojosPerXch, kXchMpu, kCatMpu),
              0);
    EXPECT_EQ(base_size_for_bid(std::numeric_limits<Mojo>::min(),
                                kMojosPerXch, kXchMpu, kCatMpu),
              0);
    EXPECT_EQ(base_size_for_bid(kXchMpu, std::numeric_limits<Mojo>::min(),
                                kXchMpu, kCatMpu),
              0);

    // THE STRENGTHENED HALF, which the float version could not promise: with
    // every input positive the result is the ceiling of a strictly positive
    // rational, so it is ALWAYS >= 1. A returned 0 can now only mean "an
    // input was non-positive" -- it can no longer mean "the intermediate
    // underflowed" or "the intermediate was NaN". Smallest possible ratios in
    // both directions:
    EXPECT_EQ(quote_cost_for_ask(1, 1, kMojoMax, 1), 1);
    EXPECT_EQ(base_size_for_bid(1, kMojoMax, 1, kMojoMax), 1);
    EXPECT_EQ(quote_cost_for_ask(1, 1, kXchMpu, kCatMpu), 1);
}

// ---------------------------------------------------------------------------
// THE DIFFERENTIAL SWEEP.
//
// Every case is checked against ref_*, which is base-10 schoolbook arithmetic
// with repeated-subtraction long division by the single combined denominator.
// The implementation is base-2 limb arithmetic with the denominator split in
// two. Neither can validate the other by construction.
//
// Each sweep counts how many cases were inexact (a real ceiling was applied)
// and how many saturated, and asserts both are non-zero -- a sweep that only
// ever hit exact, in-range quotients would pass against almost any broken
// implementation.
// ---------------------------------------------------------------------------

TEST(TakeSizingExact, MatchesAnIndependentDecimalReferenceOnLiveGeometries)
{
    // The four (base_mpu, quote_mpu) geometries reachable from config.cpp:638
    // across the six configured pairs, plus the 1 that mpu_or_one produces
    // from a malformed config.
    struct Geometry {
        std::int64_t base_mpu;
        std::int64_t quote_mpu;
    };
    const Geometry geometries[] = {
        {kXchMpu, kCatMpu},   // XCH/wUSDC.b, XCH/BYC, XCH/DBX  (DBX is LIVE)
        {kCatMpu, kCatMpu},   // BYC/wUSDC.b
        {kCatMpu, kXchMpu},   // wmilliETH.b/XCH, wmilliETH/XCH
        {kXchMpu, kXchMpu},   // not configured, but reachable arithmetic
        {1, 1},               // mpu_or_one() fallback on a broken config
        {0, -5},              // the broken config itself
    };

    // Live XCH/DBX price band (taker_fills / offer_log), the wmilliETH.b/XCH
    // band, and the round numbers the older tests use.
    const Mojo prices[] = {
        1LL,
        kMojosPerXch / 2,
        kMojosPerXch,
        1'001'000'000'000LL,
        1'234'000'000'000LL,
        1'500'000'000'000LL,
        1'740'000'000'000LL,
        84'570'142'857'143LL,
        97'181'600'000'000LL,
        115'470'000'000'000LL,
        119'453'015'427'770LL,
        160'000'000'000'000LL,
    };

    const Mojo sizes[] = {
        1LL,
        999LL,
        1000LL,
        5000LL,
        142'600'000'000LL,
        279'900'000'000LL,
        1'289'726'060'241LL,
        kXchMpu,
        5'000'000'000'000LL,
        50'000'000'000'000LL,
    };

    int checked     = 0;
    int ask_inexact = 0;
    int bid_inexact = 0;
    int differed    = 0;
    for (const Geometry& g : geometries) {
        for (const Mojo price : prices) {
            for (const Mojo size : sizes) {
                const RefResult ask_ref = ref_ask(size, price,
                                                  g.base_mpu, g.quote_mpu);
                const Mojo      ask     = quote_cost_for_ask(size, price,
                                                             g.base_mpu, g.quote_mpu);
                ASSERT_EQ(ask, ask_ref.value)
                    << "quote_cost_for_ask(" << size << ", " << price << ", "
                    << g.base_mpu << ", " << g.quote_mpu << ")";

                const RefResult bid_ref = ref_bid(size, price,
                                                  g.base_mpu, g.quote_mpu);
                const Mojo      bid     = base_size_for_bid(size, price,
                                                            g.base_mpu, g.quote_mpu);
                ASSERT_EQ(bid, bid_ref.value)
                    << "base_size_for_bid(" << size << ", " << price << ", "
                    << g.base_mpu << ", " << g.quote_mpu << ")";

                // Positive inputs must never price to zero.
                ASSERT_GT(ask, 0);
                ASSERT_GT(bid, 0);

                checked += 2;
                if (ask_ref.inexact) ++ask_inexact;
                if (bid_ref.inexact) ++bid_inexact;
                if (bid != size) ++differed;
            }
        }
    }

    // Anti-vacuity. The sweep must have run to completion, must have produced
    // conversions that are not the identity, and must have exercised the
    // CEILING on both functions -- otherwise a truncating implementation would
    // agree with a truncating reference and this test would prove nothing.
    EXPECT_EQ(checked, 2 * 6 * 12 * 10);
    EXPECT_GT(differed, 0);
    EXPECT_GT(ask_inexact, 0) << "no ask case rounded up: the sweep is vacuous";
    EXPECT_GT(bid_inexact, 0) << "no bid case rounded up: the sweep is vacuous";
}

TEST(TakeSizingExact, MatchesAnIndependentDecimalReferenceUnderRandomSweep)
{
    SplitMix rng(0xDEAD'BEEF'0000'1500ULL);

    const std::int64_t mpus[] = {1, kCatMpu, kXchMpu, kMojoMax};

    int ask_inexact = 0;
    int bid_inexact = 0;
    int saturated   = 0;
    int cases       = 0;

    constexpr int kCases = 20'000;
    for (int i = 0; i < kCases; ++i) {
        const std::int64_t base_mpu  = mpus[rng.next() % 4U];
        const std::int64_t quote_mpu = mpus[rng.next() % 4U];

        // A mix of magnitudes: live-sized, tiny, and full-range adversarial.
        // Ingest applies no maximum-size filter (market_data.cpp:1954-2021
        // only filters a minimum size and, conditionally, outlier prices), and
        // the per-take cap is applied AFTER this conversion, so the whole
        // positive Mojo range really is reachable here.
        Mojo size  = 0;
        Mojo price = 0;
        switch (i % 3) {
            case 0:  // live XCH/DBX magnitudes
                size  = rng.in_range(100'000'000'000LL, 50'000'000'000'000LL);
                price = rng.in_range(84'000'000'000'000LL, 160'000'000'000'000LL);
                break;
            case 1:  // small values, where the ceiling dominates
                size  = rng.in_range(1, 1'000'000LL);
                price = rng.in_range(1, 2 * kMojosPerXch);
                break;
            default:  // full range
                size  = rng.in_range(1, kMojoMax);
                price = rng.in_range(1, kMojoMax);
                break;
        }

        const RefResult ask_ref = ref_ask(size, price, base_mpu, quote_mpu);
        const Mojo      ask     = quote_cost_for_ask(size, price, base_mpu, quote_mpu);
        ASSERT_EQ(ask, ask_ref.value)
            << "quote_cost_for_ask(" << size << ", " << price << ", "
            << base_mpu << ", " << quote_mpu << ")  [i=" << i << "]";

        const RefResult bid_ref = ref_bid(size, price, base_mpu, quote_mpu);
        const Mojo      bid     = base_size_for_bid(size, price, base_mpu, quote_mpu);
        ASSERT_EQ(bid, bid_ref.value)
            << "base_size_for_bid(" << size << ", " << price << ", "
            << base_mpu << ", " << quote_mpu << ")  [i=" << i << "]";

        // The contract, on every single case: positive in, positive out.
        ASSERT_GT(ask, 0);
        ASSERT_GT(bid, 0);

        if (ask_ref.inexact && !ask_ref.saturated) ++ask_inexact;
        if (bid_ref.inexact && !bid_ref.saturated) ++bid_inexact;
        if (ask_ref.saturated || bid_ref.saturated) ++saturated;
        ++cases;
    }

    EXPECT_EQ(cases, kCases);
    // Anti-vacuity: the sweep must have exercised the rounding in BOTH
    // directions of the conversion, and must have reached saturation. A sweep
    // that only produced exact, in-range quotients would agree with a
    // truncating or non-saturating implementation too.
    EXPECT_GT(ask_inexact, 0) << "the ask sweep never rounded up: it is vacuous";
    EXPECT_GT(bid_inexact, 0) << "the bid sweep never rounded up: it is vacuous";
    EXPECT_GT(saturated, 0) << "the sweep never reached saturation";
}
