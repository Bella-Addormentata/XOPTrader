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
// detail: UP, always. These numbers are what we must be able to pay and what
// must fit under a cap. Rounding a cost down invents affordability.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "xop/execution/take_sizing.hpp"

using xop::kMojosPerXch;
using xop::Mojo;
using xop::execution::base_size_for_bid;
using xop::execution::ceil_to_mojo;
using xop::execution::quote_cost_for_ask;

namespace {

// The live denominations (config.cpp:638): XCH bases are 1e12 mojos/unit,
// CATs are 1000.
constexpr std::int64_t kXchMpu = 1'000'000'000'000LL;
constexpr std::int64_t kCatMpu = 1'000LL;

}  // namespace

// -- ceil_to_mojo -----------------------------------------------------------

TEST(TakeSizing, CeilRoundsUpAndRefusesNonsense)
{
    EXPECT_EQ(ceil_to_mojo(1.0L), 1);
    EXPECT_EQ(ceil_to_mojo(1.000001L), 2) << "costs must round UP";
    EXPECT_EQ(ceil_to_mojo(41.5L), 42);

    // Anything that is not a positive finite number is "no size", not an
    // arbitrary integer. A negative or NaN intermediate must decline.
    EXPECT_EQ(ceil_to_mojo(0.0L), 0);
    EXPECT_EQ(ceil_to_mojo(-1.0L), 0);
    EXPECT_EQ(ceil_to_mojo(
                  static_cast<long double>(
                      std::numeric_limits<double>::quiet_NaN())),
              0);
    // Infinity is caught by the finiteness test BEFORE the saturation
    // branch, so it declines outright rather than saturating. That ordering
    // is deliberate: an infinite intermediate means the inputs were nonsense,
    // and the safe response to nonsense is "no size", not "the largest size
    // representable".
    EXPECT_EQ(ceil_to_mojo(
                  static_cast<long double>(
                      std::numeric_limits<double>::infinity())),
              0);
}

TEST(TakeSizing, CeilSaturatesRatherThanNarrowingOutOfRange)
{
    // Narrowing a floating value that does not fit the destination integer
    // type is UNDEFINED. A recent bug in this repo (a saturation clamp that
    // rounded to 2^64) passed MSVC and failed GCC, so the guard matters more
    // than the platform's happened-to-work answer. Saturation is safe here
    // because every caller compares the result against a cap it cannot
    // exceed, so a saturated value declines.
    EXPECT_EQ(ceil_to_mojo(1.0e30L), std::numeric_limits<Mojo>::max());
    EXPECT_EQ(ceil_to_mojo(9.3e18L), std::numeric_limits<Mojo>::max());
}

// -- quote_cost_for_ask -----------------------------------------------------

TEST(TakeSizing, AskCostOnAnXchBaseCatQuotePair)
{
    // XCH/wUSDC.b: base 1e12 mpu, quote 1000 mpu. Price is carried scaled by
    // kMojosPerXch, so price 1.5e12 means 1.5 wUSDC per XCH.
    //   1 XCH = 1e12 base mojos, at 1.5 -> 1.5 wUSDC = 1500 quote mojos.
    EXPECT_EQ(quote_cost_for_ask(kXchMpu, 1'500'000'000'000LL,
                                 kXchMpu, kCatMpu),
              1500);

    // Half an XCH costs half as much.
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
    EXPECT_EQ(base_size_for_bid(3000, 1'500'000'000'000LL, kXchMpu, kCatMpu),
              2 * kXchMpu);
}

TEST(TakeSizing, BidSizeIsTheInverseOfAskCost)
{
    // Round-tripping must not lose the base size. Both directions round up,
    // so the inverse is >= the original; it must not come back smaller.
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
