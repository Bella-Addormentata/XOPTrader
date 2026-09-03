// ---------------------------------------------------------------------------
// test_denom.cpp -- the strong denomination typedefs, and the extraction that
//                   makes them worth having.
//
// [TODO S36 increment, 2026-09-02]
//
// WHAT THIS FILE IS FOR, AND WHAT IT DELIBERATELY CANNOT DO.
//
// The headline guarantee of denom.hpp is a NEGATIVE: a transposed denomination
// does not compile. A gtest TEST() cannot assert that directly -- an ill-formed
// expression in a test body is a build failure, not a red test -- so the primary
// evidence lives as static_asserts in denom.hpp, take_sizing.hpp and
// crossed_book.hpp, which are evaluated by whichever compiler is building and
// therefore hold on CI's GCC as well as on the developer machine's MSVC.
//
// This file does three things those static_asserts cannot:
//
//   1. It re-states the rejections through CONCEPTS EVALUATED AS RUNTIME VALUES
//      (EXPECT_FALSE(CostCallable<...>)), so the guarantee appears in the test
//      count and in the test report rather than being invisible when green. A
//      concept used as a bool is still resolved at compile time; what the
//      EXPECT_FALSE buys is VISIBILITY, and it is worth being explicit that
//      that is all it buys.
//
//   2. It pins that THE ARITHMETIC DID NOT MOVE, by driving the typed API at the
//      exact witnesses take_sizing.hpp pins and by sweeping the typed path
//      against the untyped-equivalent computation.
//
//   3. It tests classify_offer_size, which is the part of this increment that
//      strong types alone could NOT deliver: the resolution of a runtime
//      denomination, extracted into a pure function so that it is reachable from
//      a test at all. Nothing in cpp/tests can construct an Engine (TODO S36),
//      so before this extraction the five open-coded side ternaries in
//      engine.cpp were unguarded lines.
//
// READ denom.hpp's THREE LIMITS before treating a green run here as proof of
// anything broader. In particular LIMIT 1: a call site can still write
// BaseMojos{co.size} on a bid and launder a wrong number through the type. No
// test in this file can detect that, and none pretends to.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <ostream>
#include <vector>

#include "xop/execution/crossed_book.hpp"
#include "xop/execution/take_sizing.hpp"
#include "xop/util/denom.hpp"

namespace xop {
// gtest prints unknown types as a raw byte dump. Denominated is a money value
// and a failed assertion on one must be readable, so teach gtest to print the
// underlying count. Test-local on purpose: denom.hpp is a PURE header and must
// not acquire an <ostream> dependency for the sake of test diagnostics.
template <class Tag>
void PrintTo(const Denominated<Tag>& d, std::ostream* os)
{
    *os << d.v;
}
}  // namespace xop

using xop::BaseMojos;
using xop::BaseMpu;
using xop::CompetingOffer;
using xop::Mojo;
using xop::OfferedMojos;
using xop::QuoteMojos;
using xop::QuoteMpu;
using xop::Side;
using xop::execution::classify_offer_size;
using xop::execution::SpendAsset;
using xop::execution::TakeLegs;

namespace {

// The live denominations. config.cpp:638 hard-assigns base/quote mojos-per-unit
// from the asset id: 1e12 for "xch", 1000 otherwise.
constexpr std::int64_t kXchMpu = 1'000'000'000'000LL;
constexpr std::int64_t kCatMpu = 1'000LL;

CompetingOffer offer(Side side, Mojo price, Mojo size)
{
    CompetingOffer co{};
    co.offer_id = "offer-1";
    co.pair_name = "XCH/DBX";
    co.side  = side;
    co.price = price;
    co.size  = size;
    return co;
}

}  // namespace

// ===========================================================================
// 1. CONSTRUCTION
// ===========================================================================

TEST(Denom, DefaultConstructsToZeroSoAForgottenValueFailsClosed)
{
    // Not cosmetic. Zero on this path is take_sizing.hpp's DO NOT TAKE, never
    // "free", so a default-constructed amount DECLINES. This is the same
    // fail-closed discipline as the documented family of twelve.
    EXPECT_EQ(BaseMojos{}.v, 0);
    EXPECT_EQ(QuoteMojos{}.v, 0);
    EXPECT_EQ(BaseMojos{}, BaseMojos{0});
}

TEST(Denom, ExplicitConstructionCarriesTheValueVerbatim)
{
    EXPECT_EQ(BaseMojos{5'000'000'000'000LL}.v, 5'000'000'000'000LL);
    EXPECT_EQ(QuoteMojos{-7}.v, -7);
    EXPECT_EQ(BaseMojos{std::numeric_limits<Mojo>::max()}.v,
              std::numeric_limits<Mojo>::max());
}

TEST(Denom, TheWrapperIsZeroOverhead)
{
    // If this ever fails, the "ZERO runtime change" claim that justified
    // choosing strong typedefs over dependency injection is false, and the
    // whole cost/benefit argument for this increment collapses.
    EXPECT_EQ(sizeof(BaseMojos), sizeof(Mojo));
    EXPECT_EQ(sizeof(QuoteMojos), sizeof(Mojo));
}

// ===========================================================================
// 2. COMPARISON AND ARITHMETIC
// ===========================================================================

TEST(Denom, SameTagComparisonOrdersByUnderlyingValue)
{
    EXPECT_TRUE(BaseMojos{5} < BaseMojos{7});
    EXPECT_TRUE(BaseMojos{7} > BaseMojos{5});
    EXPECT_TRUE(BaseMojos{5} <= BaseMojos{5});
    EXPECT_TRUE(BaseMojos{5} >= BaseMojos{5});
    EXPECT_TRUE(BaseMojos{5} == BaseMojos{5});
    EXPECT_TRUE(BaseMojos{5} != BaseMojos{6});

    // Negatives order correctly -- the guards in engine.cpp are written as
    // `<= BaseMojos{0}`, so this is load-bearing rather than decorative.
    EXPECT_TRUE(BaseMojos{-1} < BaseMojos{0});
    EXPECT_FALSE(BaseMojos{-1} > BaseMojos{0});
}

TEST(Denom, EqualityExistsAlongsideTheDefaultedSpaceship)
{
    // Worth its own test. A survey of this change asserted that a defaulted
    // operator<=> does NOT give you operator==, and that if you relied on it
    // EXPECT_EQ and static_assert(a == b) would fail to compile.
    //
    // THAT IS WRONG: [class.compare.default]/4 implicitly declares a defaulted
    // == from a defaulted <=>, and it was verified on MSVC before this comment
    // was written. denom.hpp declares == explicitly anyway, because the
    // redundancy costs nothing and removes a toolchain question -- but the
    // reason is documented rather than folded into a false claim.
    EXPECT_EQ(BaseMojos{42}, BaseMojos{42});
    static_assert(BaseMojos{42} == BaseMojos{42});
}

TEST(Denom, SameTagArithmeticStaysInTheSameDenomination)
{
    EXPECT_EQ(BaseMojos{7} + BaseMojos{5}, BaseMojos{12});
    EXPECT_EQ(BaseMojos{7} - BaseMojos{5}, BaseMojos{2});

    BaseMojos acc{};
    acc += BaseMojos{100};
    acc += BaseMojos{23};
    EXPECT_EQ(acc, BaseMojos{123});
}

// ===========================================================================
// 3. THE REJECTIONS -- surfaced as runtime rows so a green suite SHOWS them.
//
// Each concept below is resolved at compile time; EXPECT_FALSE only makes the
// result visible in the test report. The assertions that actually FAIL A BUILD
// live in the headers. Both are stated so nobody mistakes one for the other.
// ===========================================================================

namespace {

template <class Sz, class P, class B, class Q>
concept CostCallable =
    requires(Sz s, P p, B b, Q q) { xop::execution::quote_cost_for_ask(s, p, b, q); };

template <class Sz, class P, class B, class Q>
concept SizeCallable =
    requires(Sz s, P p, B b, Q q) { xop::execution::base_size_for_bid(s, p, b, q); };

template <class L, class Cap>
concept BaseComparableWith = requires(L l, Cap c) { l.base > c; };

template <class L, class Cap>
concept QuoteComparableWith = requires(L l, Cap c) { l.quote > c; };

}  // namespace

TEST(Denom, TheStep9eDefectDoesNotCompile)
{
    // THE ACCEPTANCE CRITERION FOR THIS WHOLE INCREMENT.
    //
    // The Step 9e peg-arb defect: a BID carries a QUOTE-denominated size, and it
    // was used as base mojos in the cap comparison, the pre-trade funding
    // estimate and BOTH double-entry ledger legs. On BYC/wUSDC.b the error is
    // invisible because base and quote mojos-per-unit are both 1000; on any pair
    // where they differ the cap is applied in the wrong unit.
    //
    // The defect as it was ACTUALLY written passed a bare, unclassified Mojo:
    EXPECT_FALSE((CostCallable<Mojo, Mojo, BaseMpu, QuoteMpu>));

    // The defect stated in denominational terms -- a quote size where base is
    // expected -- and its mirror image:
    EXPECT_FALSE((CostCallable<QuoteMojos, Mojo, BaseMpu, QuoteMpu>));
    EXPECT_FALSE((SizeCallable<BaseMojos, Mojo, BaseMpu, QuoteMpu>));

    // And the correct wirings still work, which is what stops all of the above
    // from being vacuously true of a function nobody can call at all.
    EXPECT_TRUE((CostCallable<BaseMojos, Mojo, BaseMpu, QuoteMpu>));
    EXPECT_TRUE((SizeCallable<QuoteMojos, Mojo, BaseMpu, QuoteMpu>));
}

TEST(Denom, TransposedDenominationArgumentsAreRejected)
{
    // The two mojos-per-unit arguments are ADJACENT and were the same type
    // (std::int64_t) before this change, so swapping them was a silent 1e9x
    // error on any pair whose denominations differ -- and there is no test that
    // could have caught it, because both orders compiled and both produced a
    // plausible number.
    EXPECT_FALSE((CostCallable<BaseMojos, Mojo, QuoteMpu, BaseMpu>));
    EXPECT_FALSE((SizeCallable<QuoteMojos, Mojo, QuoteMpu, BaseMpu>));

    // An unwrapped mpu is refused too, so the guarantee cannot be sidestepped
    // by simply not wrapping one argument.
    EXPECT_FALSE((CostCallable<BaseMojos, Mojo, std::int64_t, QuoteMpu>));
    EXPECT_FALSE((CostCallable<BaseMojos, Mojo, BaseMpu, std::int64_t>));
}

TEST(Denom, TheCapComparisonAcceptsOnlyTheBaseLeg)
{
    // This is the exact expression Step 9e got wrong: `base_sz > max_mojos`.
    // The first of these is what FORCES cap_mojos_for to return BaseMojos --
    // without that return-type change, a bare cap compiles and the guarantee is
    // worthless at the one site it was built for.
    EXPECT_FALSE((BaseComparableWith<TakeLegs, Mojo>));       // bare cap
    EXPECT_FALSE((QuoteComparableWith<TakeLegs, BaseMojos>)); // wrong leg
    EXPECT_TRUE((BaseComparableWith<TakeLegs, BaseMojos>));   // right leg

    // cap_mojos_for's return type, pinned. A future edit that "simplifies" it
    // back to a bare Mojo turns the three rows above into lies.
    static_assert(std::is_same_v<
                  decltype(xop::execution::cap_mojos_for(1.0, BaseMpu{1})),
                  BaseMojos>);
}

// ===========================================================================
// 4. THE ARITHMETIC DID NOT MOVE.
//
// The retype is supposed to be arithmetically INERT: constexpr wrappers compile
// away and every expression inside take_sizing.hpp is byte-for-byte what
// shipped. If any value below has moved, the type change broke the maths and
// this increment is a defect rather than a guard.
// ===========================================================================

TEST(Denom, TypedApiReproducesTheHeaderWitnessesExactly)
{
    using xop::execution::base_size_for_bid;
    using xop::execution::quote_cost_for_ask;

    // The two assertions CI was red on when this arithmetic was long double.
    EXPECT_EQ(quote_cost_for_ask(BaseMojos{kXchMpu}, 1'500'000'000'000LL,
                                 BaseMpu{kXchMpu}, QuoteMpu{kCatMpu}),
              QuoteMojos{1500});
    EXPECT_EQ(quote_cost_for_ask(BaseMojos{500'000'000'000LL}, 1'500'000'000'000LL,
                                 BaseMpu{kXchMpu}, QuoteMpu{kCatMpu}),
              QuoteMojos{750});

    // The third divergent one.
    EXPECT_EQ(base_size_for_bid(QuoteMojos{3000}, 1'500'000'000'000LL,
                                BaseMpu{kXchMpu}, QuoteMpu{kCatMpu}),
              BaseMojos{2'000'000'000'000LL});

    // THE FLOAT TRIPWIRE. 10000001 * 99999990000001 == 10^21 + 1 exactly, so
    // the true ceiling is 2 and every floating format on every platform answers
    // 1. Reproduced here through the TYPED path specifically, so that the
    // exactness property is pinned on the API the engine now calls.
    EXPECT_EQ(quote_cost_for_ask(BaseMojos{10'000'001LL}, 99'999'990'000'001LL,
                                 BaseMpu{kXchMpu}, QuoteMpu{kCatMpu}),
              QuoteMojos{2});

    // The mul3_u64 middle-limb carry witness.
    EXPECT_EQ(base_size_for_bid(QuoteMojos{789'718'602'219'600'096LL},
                                4'600'337'460'146'979'069LL,
                                BaseMpu{std::numeric_limits<Mojo>::max()},
                                QuoteMpu{std::numeric_limits<Mojo>::max()}),
              BaseMojos{171'665'363'479LL});

    // Saturation through the public API.
    EXPECT_EQ(base_size_for_bid(QuoteMojos{std::numeric_limits<Mojo>::max()}, 1,
                                BaseMpu{kXchMpu}, QuoteMpu{1}),
              BaseMojos{std::numeric_limits<Mojo>::max()});

    // Zero is DO NOT TAKE, never free.
    EXPECT_EQ(quote_cost_for_ask(BaseMojos{0}, 1'500'000'000'000LL,
                                 BaseMpu{kXchMpu}, QuoteMpu{kCatMpu}),
              QuoteMojos{0});
    EXPECT_EQ(base_size_for_bid(QuoteMojos{3000}, 0,
                                BaseMpu{kXchMpu}, QuoteMpu{kCatMpu}),
              BaseMojos{0});
}

TEST(Denom, CapMojosForIsUnchangedAcrossTheLiveConfigValues)
{
    using xop::execution::cap_mojos_for;

    // Every configured value today, at both live denominations.
    EXPECT_EQ(cap_mojos_for(5.0, BaseMpu{kXchMpu}), BaseMojos{5'000'000'000'000LL});
    EXPECT_EQ(cap_mojos_for(5.0, BaseMpu{kCatMpu}), BaseMojos{5000});
    EXPECT_EQ(cap_mojos_for(0.25, BaseMpu{kXchMpu}), BaseMojos{250'000'000'000LL});
    EXPECT_EQ(cap_mojos_for(2.0, BaseMpu{kXchMpu}), BaseMojos{2'000'000'000'000LL});
    EXPECT_EQ(cap_mojos_for(50.0, BaseMpu{kXchMpu}), BaseMojos{50'000'000'000'000LL});

    // The declines. An unrepresentable cap is an UNBOUNDED cap, and unbounded is
    // the defect, so every one of these must be zero and not "no limit".
    EXPECT_EQ(cap_mojos_for(0.0, BaseMpu{kXchMpu}), BaseMojos{0});
    EXPECT_EQ(cap_mojos_for(-5.0, BaseMpu{kXchMpu}), BaseMojos{0});
    EXPECT_EQ(cap_mojos_for(std::numeric_limits<double>::quiet_NaN(),
                            BaseMpu{kXchMpu}), BaseMojos{0});
    EXPECT_EQ(cap_mojos_for(std::numeric_limits<double>::infinity(),
                            BaseMpu{kXchMpu}), BaseMojos{0});
    EXPECT_EQ(cap_mojos_for(5.0, BaseMpu{0}), BaseMojos{0});
    EXPECT_EQ(cap_mojos_for(5.0, BaseMpu{-1}), BaseMojos{0});
    EXPECT_EQ(cap_mojos_for(1.0e9, BaseMpu{kXchMpu}), BaseMojos{0});
}

// ===========================================================================
// 5. classify_offer_size -- the extraction.
//
// This is the piece strong typedefs could not deliver alone. CompetingOffer::size
// is denominated in the OFFERED asset -- base for an ask, quote for a bid -- and
// that is a RUNTIME fact no static tag can express (denom.hpp LIMIT 1). Before
// this function existed, the resolution was an open-coded side ternary repeated
// at five call sites in engine.cpp and MISSING at three more, none of them
// reachable from a test.
// ===========================================================================

TEST(ClassifyOfferSize, AnAskSizeIsBaseMojosAndTheSpendIsQuote)
{
    const auto legs = classify_offer_size(Side::Ask, OfferedMojos{kXchMpu},
                                          1'500'000'000'000LL,
                                          BaseMpu{kXchMpu}, QuoteMpu{kCatMpu});
    ASSERT_TRUE(legs.usable);

    // An ask's advertised size IS base mojos -- passed through verbatim, never
    // converted. This is the half Step 9e happened to get right.
    EXPECT_EQ(legs.base, BaseMojos{kXchMpu});

    // ...and the quote leg is the cost of lifting it, which is the 1500 witness.
    EXPECT_EQ(legs.quote, QuoteMojos{1500});

    // Lifting an ask spends QUOTE.
    EXPECT_EQ(legs.spend, SpendAsset::Quote);
}

TEST(ClassifyOfferSize, ABidSizeIsQuoteMojosAndIsConvertedToBase)
{
    // THE STEP 9e BUG, EXPRESSED AS BEHAVIOUR. The advertised 3000 is QUOTE
    // mojos. Step 9e treated that 3000 as base mojos; the true base leg is
    // 2e12 -- a factor of 666,666,666 out. That is the number the cap bounds,
    // the number charged against the wallet, and the number record_taker_fill()
    // books.
    const auto legs = classify_offer_size(Side::Bid, OfferedMojos{3000},
                                          1'500'000'000'000LL,
                                          BaseMpu{kXchMpu}, QuoteMpu{kCatMpu});
    ASSERT_TRUE(legs.usable);

    EXPECT_EQ(legs.quote, QuoteMojos{3000});                    // as advertised
    EXPECT_EQ(legs.base, BaseMojos{2'000'000'000'000LL});       // what we deliver
    EXPECT_NE(legs.base.v, 3000);                               // the bug, explicitly

    // Hitting a bid DELIVERS base, so the spend is base-denominated.
    EXPECT_EQ(legs.spend, SpendAsset::Base);
}

TEST(ClassifyOfferSize, BothLegsAreAlwaysPopulatedOnEitherSide)
{
    // Neither leg is conditional, so neither can be transposed: there is no
    // branch at a call site that could pick the wrong one and still be
    // type-correct against a consumer.
    const auto ask = classify_offer_size(Side::Ask, OfferedMojos{kXchMpu},
                                         1'500'000'000'000LL,
                                         BaseMpu{kXchMpu}, QuoteMpu{kCatMpu});
    const auto bid = classify_offer_size(Side::Bid, OfferedMojos{3000},
                                         1'500'000'000'000LL,
                                         BaseMpu{kXchMpu}, QuoteMpu{kCatMpu});
    EXPECT_GT(ask.base.v, 0);
    EXPECT_GT(ask.quote.v, 0);
    EXPECT_GT(bid.base.v, 0);
    EXPECT_GT(bid.quote.v, 0);
}

TEST(ClassifyOfferSize, ADefaultConstructedResultDeclines)
{
    // Per the documented fail-open family of twelve: a forgotten or
    // default-constructed decision must DECLINE, not proceed.
    const TakeLegs forgotten{};
    EXPECT_FALSE(forgotten.usable);
    EXPECT_EQ(forgotten.base, BaseMojos{0});
    EXPECT_EQ(forgotten.quote, QuoteMojos{0});
}

TEST(ClassifyOfferSize, UnusableInputsDeclineAndCarryNoSize)
{
    const BaseMpu  b{kXchMpu};
    const QuoteMpu q{kCatMpu};

    for (const Side side : {Side::Ask, Side::Bid}) {
        // Zero and negative sizes.
        EXPECT_FALSE(classify_offer_size(side, OfferedMojos{0}, 1'500'000'000'000LL, b, q).usable);
        EXPECT_FALSE(classify_offer_size(side, OfferedMojos{-1}, 1'500'000'000'000LL, b, q).usable);
        // Zero and negative prices. Ingest can genuinely produce a zero price:
        // it llrounds a market ratio scaled by kMojosPerXch, which rounds to 0
        // below 5e-13.
        EXPECT_FALSE(classify_offer_size(side, OfferedMojos{3000}, 0, b, q).usable);
        EXPECT_FALSE(classify_offer_size(side, OfferedMojos{3000}, -1, b, q).usable);

        // And a declined result carries NO size, so a caller that ignores
        // `usable` still gets take_sizing's zero-means-decline contract.
        const auto d = classify_offer_size(side, OfferedMojos{0}, 0, b, q);
        EXPECT_EQ(d.base, BaseMojos{0});
        EXPECT_EQ(d.quote, QuoteMojos{0});
    }
}

TEST(ClassifyOfferSize, TheCompetingOfferOverloadAgreesWithTheExplicitForm)
{
    // The overload exists so a call site cannot pair one offer's size with
    // another offer's side -- the corruption a type alone cannot catch
    // (denom.hpp LIMIT 1). It must be a pure convenience over the explicit form,
    // never a second implementation.
    const BaseMpu  b{kXchMpu};
    const QuoteMpu q{kCatMpu};

    for (const Side side : {Side::Ask, Side::Bid}) {
        const CompetingOffer co = offer(side, 1'500'000'000'000LL, 3000);
        const auto viaco = classify_offer_size(co, b, q);
        const auto direct =
            classify_offer_size(co.side, OfferedMojos{co.size}, co.price, b, q);

        EXPECT_EQ(viaco.base, direct.base);
        EXPECT_EQ(viaco.quote, direct.quote);
        EXPECT_EQ(viaco.spend, direct.spend);
        EXPECT_EQ(viaco.usable, direct.usable);
    }
}

TEST(ClassifyOfferSize, MatchesTheUnderlyingConversionsAcrossASweep)
{
    // The differential check: classify_offer_size must be exactly the two
    // take_sizing conversions dispatched on side, with no arithmetic of its
    // own. Any divergence means the extraction changed a value, which is the
    // one thing it must not do.
    const std::vector<std::int64_t> mpus{1, kCatMpu, kXchMpu};
    const std::vector<Mojo> prices{1, 1'000, 1'500'000'000'000LL, kXchMpu};
    const std::vector<Mojo> sizes{1, 3'000, 1'000'000'000LL, kXchMpu};

    for (const std::int64_t bm : mpus) {
        for (const std::int64_t qm : mpus) {
            for (const Mojo price : prices) {
                for (const Mojo size : sizes) {
                    const BaseMpu  b{bm};
                    const QuoteMpu q{qm};

                    const auto ask = classify_offer_size(Side::Ask, OfferedMojos{size},
                                                         price, b, q);
                    EXPECT_EQ(ask.base, BaseMojos{size});
                    EXPECT_EQ(ask.quote,
                              xop::execution::quote_cost_for_ask(BaseMojos{size},
                                                                 price, b, q))
                        << "ask leg diverged: bm=" << bm << " qm=" << qm
                        << " price=" << price << " size=" << size;

                    const auto bid = classify_offer_size(Side::Bid, OfferedMojos{size},
                                                         price, b, q);
                    EXPECT_EQ(bid.quote, QuoteMojos{size});
                    EXPECT_EQ(bid.base,
                              xop::execution::base_size_for_bid(QuoteMojos{size},
                                                                price, b, q))
                        << "bid leg diverged: bm=" << bm << " qm=" << qm
                        << " price=" << price << " size=" << size;
                }
            }
        }
    }
}

TEST(ClassifyOfferSize, TheBidAndAskLegsDifferWhenDenominationsDiffer)
{
    // The masking condition, stated as a test so it cannot be forgotten: on
    // BYC/wUSDC.b base and quote mojos-per-unit are BOTH 1000, and the Step 9e
    // error is invisible there. It is only visible when they differ. A future
    // reader who tests only on a CAT/CAT pair will see nothing wrong.
    const auto masked = classify_offer_size(Side::Bid, OfferedMojos{5000},
                                            kXchMpu,
                                            BaseMpu{kCatMpu}, QuoteMpu{kCatMpu});
    ASSERT_TRUE(masked.usable);
    EXPECT_EQ(masked.base.v, masked.quote.v)
        << "on a CAT/CAT pair the raw size and the converted base size coincide, "
           "which is exactly why Step 9e went unnoticed on BYC/wUSDC.b";

    const auto exposed = classify_offer_size(Side::Bid, OfferedMojos{5000},
                                             kXchMpu,
                                             BaseMpu{kXchMpu}, QuoteMpu{kCatMpu});
    ASSERT_TRUE(exposed.usable);
    EXPECT_NE(exposed.base.v, exposed.quote.v)
        << "on XCH/DBX the two differ by the denomination ratio, and treating "
           "the advertised size as base mojos is the defect";
}
