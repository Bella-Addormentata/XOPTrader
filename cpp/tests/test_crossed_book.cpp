// ---------------------------------------------------------------------------
// [S40] The crossed-book taker computed a size cap and threw it away.
//
// Step 9c ended its candidate selection with
//
//     const Mojo take_size = std::min(best_ask_size, max_take_mojos);
//
// and then called wallet_->take_offer(offer_bech32, fee). That signature has
// NO SIZE PARAMETER -- a Chia offer is an atomic swap of exactly the coins the
// maker committed, with no partial fill -- so the clamp bounded a LOG FIELD
// and nothing else. arbitrage.crossed_book_max_take_xch: 5.0 was inert, and
// crossed-book exposure was unbounded.
//
// What was measured before the fix:
//   * 208 attempts logged size=5000000000000, exactly the 5.0 XCH cap. A
//     logged size that lands exactly on the cap means the counterparty offer
//     was LARGER and only the log was clamped.
//   * 3 attempts below the cap, and exactly one success ever:
//     2026-08-31T13:47:43 XCH/DBX TOOK edge=14.4bps size=1289726060241 --
//     the sub-cap one.
//   * 186 "insufficient funds" errors on the at-cap attempts. Being broke is
//     the only thing that has ever bounded this path. Funding the wallet
//     without this fix unmasks the bug, which is why the two must land in
//     this order.
//   * Of the 23 crossed_book rows in taker_fills, nineteen settled strictly
//     below their pair's cap and FOUR sit exactly on it -- the BYC/wUSDC.b
//     rows of 2026-08-06 21:38 to 22:45, base_delta 5000 against a
//     cap_mojos_for(5.0, 1000) of exactly 5000. Exactly-on-cap is the clamp
//     signature, so those four are indistinguishable from clamped records and
//     their remediation is OPEN, not closed. See crossed_book.hpp. An earlier
//     version of this comment claimed all 23 were clean; it had not queried
//     the database.
//
// The clamp also reached the books: record_taker_fill() derives the quote leg
// as quote_mojos_for(base_mojos, ...), so a clamped base size would have
// synthesised the taker_fills row and BOTH double-entry ledger legs from a
// number the RPC never saw -- coherently, leaving the books internally
// consistent and wrong, with nothing downstream able to detect it.
//
// These tests pin the fix: the cap is honoured by FILTERING, exactly as Step
// 9d has always done, and no code path returns a size that is not the
// counterparty's size verbatim. They are the entire regression surface,
// because nothing in cpp/tests can construct an Engine (TODO S36) and the
// call site is otherwise unreachable from a test.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "xop/execution/crossed_book.hpp"

using xop::CompetingOffer;
using xop::Mojo;
using xop::Side;
using xop::execution::CrossedBookVerdict;

namespace {

// [2026-09-02] TEST-LOCAL UNTYPED ADAPTERS. cap_mojos_for now takes a BaseMpu
// and returns BaseMojos, and evaluate_crossed_book takes a BaseMojos cap (see
// cpp/include/xop/util/denom.hpp). Adapting at one point keeps all 20 cap
// assertions below byte-identical, so `git diff` on this file shows no changed
// NUMBERS -- which is the property that proves the retype did not move the
// arithmetic. The denomination guarantee itself is asserted at compile time in
// crossed_book.hpp (CapCallable) and exercised in cpp/tests/test_denom.cpp.
//
// The decision FIELDS are read through `.v` at the assertion sites rather than
// adapted here, deliberately: the biconditional this file exists to defend
// (take_size != 0 <=> verdict == Take) is about those fields specifically, and
// hiding them behind a converting wrapper would make the regression test for
// the std::min clamp read against a copy instead of the real struct.

[[nodiscard]] Mojo cap_mojos_for(double units, std::int64_t base_mpu) noexcept
{
    return xop::execution::cap_mojos_for(units, xop::BaseMpu{base_mpu}).v;
}

[[nodiscard]] xop::execution::CrossedBookDecision evaluate_crossed_book(
    const std::vector<CompetingOffer>& offers, double min_edge_bps, Mojo cap)
{
    return xop::execution::evaluate_crossed_book(offers, min_edge_bps,
                                                 xop::BaseMojos{cap});
}

// The live denominations. config.cpp:638 gives base_mojos_per_unit = 1e12 for
// an XCH base and 1000 for a CAT, so the SAME crossed_book_max_take_xch: 5.0
// means 5e12 mojos on XCH/DBX and 5000 mojos on BYC/wUSDC.b.
constexpr std::int64_t kXchDenom = 1'000'000'000'000LL;
constexpr std::int64_t kCatDenom = 1000LL;
constexpr double       kMaxTake  = 5.0;    // config.yaml:295
constexpr Mojo         kXchCap   = 5'000'000'000'000LL;
constexpr Mojo         kCatCap   = 5000LL;

CompetingOffer mk(Side side, Mojo price, Mojo size, std::string id = "offer")
{
    CompetingOffer co;
    co.offer_id  = std::move(id);
    co.pair_name = "XCH/DBX";
    co.side      = side;
    co.price     = price;
    co.size      = size;
    return co;
}

// A crossed XCH/DBX book: bid 101, ask 100 -> 100 bps of edge.
constexpr Mojo kAsk = 100'000'000'000'000LL;
constexpr Mojo kBid = 101'000'000'000'000LL;

std::vector<CompetingOffer> crossed_book(Mojo ask_size)
{
    return {mk(Side::Bid, kBid, ask_size), mk(Side::Ask, kAsk, ask_size, "cheap")};
}

}  // namespace

// -- The cap derivation, and the 9c/9d denomination difference --------------

TEST(CrossedBook, CapIsPairDenominatedNotAlwaysXch)
{
    // This is the assertion that fails the moment someone "unifies" 9c's cap
    // with 9d's kMojosPerXch. Under kMojosPerXch the BYC cap would become
    // 5e12 instead of 5000 -- a billion-fold widening of a live money-path
    // limit, on the pair that produced 1,116 of the at-cap detections.
    EXPECT_EQ(cap_mojos_for(kMaxTake, kXchDenom), kXchCap);
    EXPECT_EQ(cap_mojos_for(kMaxTake, kCatDenom), kCatCap);
}

TEST(CrossedBook, CapIsTheSameNumberOnEveryAbi)
{
    // [2026-09-01] cap_mojos_for used to compute this product in
    // `long double`, which is 53-bit on MSVC and 64-bit x87 on GCC/x86-64,
    // and then TRUNCATE it. So the cap VALUE was ABI-divergent even though
    // the narrowing was well-defined -- in the very file whose comment is
    // cited elsewhere as the exemplar of getting ABI divergence right.
    //
    // Every case below is a non-dyadic unit count, which is what makes the
    // two precisions land on different integers. Exact products, and what
    // each ABI produced BEFORE the fix:
    //
    //   0.15 * 1e12 = 149999999999.999994...   MSVC 150000000000 / GCC 149999999999
    //   0.03 * 1e12 =  29999999999.999999...   MSVC  30000000000 / GCC  29999999999
    //   0.3  * 1e12 = 299999999999.999989...   MSVC 300000000000 / GCC 299999999999
    //
    // THIS TEST WOULD HAVE BEEN RED ON CI AND GREEN LOCALLY, which is the
    // exact failure shape the whole exercise exists to close: it is not
    // reproducible on the developer machine, so it has to be argued from the
    // type rather than observed. In `double` both toolchains are IEEE
    // binary64 under SSE2 (FLT_EVAL_METHOD 0 on x86-64, no x87 excess
    // precision), so these are now identical by construction.
    //
    // Latent rather than live: no current config value is non-dyadic
    // (5.0, 0.25, 50, 2.0 all agreed on both ABIs), so nothing in production
    // moved. The point is that the next operator to type 0.15 does not
    // reopen this.
    EXPECT_EQ(cap_mojos_for(0.15, kXchDenom), 150'000'000'000LL);
    EXPECT_EQ(cap_mojos_for(0.03, kXchDenom), 30'000'000'000LL);
    EXPECT_EQ(cap_mojos_for(0.3, kXchDenom), 300'000'000'000LL);

    // The dyadic config values in use today, pinned so the change is visibly
    // a no-op for them.
    EXPECT_EQ(cap_mojos_for(5.0, kXchDenom), 5'000'000'000'000LL);
    EXPECT_EQ(cap_mojos_for(0.25, kXchDenom), 250'000'000'000LL);
    EXPECT_EQ(cap_mojos_for(2.0, kXchDenom), 2'000'000'000'000LL);
    EXPECT_EQ(cap_mojos_for(50.0, kXchDenom), 50'000'000'000'000LL);
}

TEST(CrossedBook, UnusableCapMeansTakeNothingNotTakeAnything)
{
    // An unrepresentable cap is an UNBOUNDED cap, and unbounded is the bug.
    // Every one of these must decline rather than fall back to "no limit".
    EXPECT_EQ(cap_mojos_for(0.0, kXchDenom), 0);
    EXPECT_EQ(cap_mojos_for(-5.0, kXchDenom), 0);
    EXPECT_EQ(cap_mojos_for(std::numeric_limits<double>::quiet_NaN(), kXchDenom), 0);
    EXPECT_EQ(cap_mojos_for(std::numeric_limits<double>::infinity(), kXchDenom), 0);
    EXPECT_EQ(cap_mojos_for(kMaxTake, 0), 0);
    EXPECT_EQ(cap_mojos_for(kMaxTake, -1), 0);

    // Overflow: 1e9 XCH at 1e12 mojos/unit is 1e21, far past int64. Narrowing
    // that would be UB, and a previous saturation clamp in this repo rounded
    // to 2^63, passed MSVC and failed GCC in CI.
    EXPECT_EQ(cap_mojos_for(1.0e9, kXchDenom), 0);

    // And a zero cap must reach the caller as a distinct verdict, not as a
    // take of size zero.
    const auto d = evaluate_crossed_book(crossed_book(10), 1.0, 0);
    EXPECT_EQ(d.verdict, CrossedBookVerdict::CapUnusable);
    EXPECT_EQ(d.take_size.v, 0);
}

// -- Book shapes ------------------------------------------------------------

TEST(CrossedBook, EmptyBookIsNoBook)
{
    const auto d = evaluate_crossed_book({}, 1.0, kXchCap);
    EXPECT_EQ(d.verdict, CrossedBookVerdict::NoBook);
    EXPECT_EQ(d.take_size.v, 0);
    EXPECT_EQ(d.best_ask_size.v, 0);
    EXPECT_TRUE(d.best_ask_offer_id.empty());
}

TEST(CrossedBook, OneSidedBookIsNoBook)
{
    // The asks-only case is the one that used to leak: selection populates
    // best_ask_offer_id and best_ask_size before the NoBook exit, while
    // best_ask_price is assigned only after it. A declined decision carrying
    // a real size beside a price of 0 is a trap for the detection log, which
    // prints best_ask_size. All three ask fields must be returned as a unit.
    const std::vector<CompetingOffer> asks_only{mk(Side::Ask, kAsk, 10)};
    const auto a = evaluate_crossed_book(asks_only, 1.0, kXchCap);
    EXPECT_EQ(a.verdict, CrossedBookVerdict::NoBook);
    EXPECT_EQ(a.take_size.v, 0);
    EXPECT_EQ(a.best_ask_price, 0);
    EXPECT_EQ(a.best_ask_size.v, 0);
    EXPECT_TRUE(a.best_ask_offer_id.empty());

    const std::vector<CompetingOffer> bids_only{mk(Side::Bid, kBid, 10)};
    const auto b = evaluate_crossed_book(bids_only, 1.0, kXchCap);
    EXPECT_EQ(b.verdict, CrossedBookVerdict::NoBook);
    EXPECT_EQ(b.take_size.v, 0);
    EXPECT_EQ(b.best_ask_size.v, 0);
    EXPECT_TRUE(b.best_ask_offer_id.empty());
}

// A NoBook decision must never describe an offer. Stated as its own test
// because the biconditional at the top of crossed_book.hpp constrains
// take_size only, and the property test's field assertion accepts either the
// counterparty size or 0 -- so neither of them can see this.
TEST(CrossedBook, ADeclinedDecisionWithNoUsableCrossDescribesNoOffer)
{
    // Several asks, no bid at all: selection runs to completion and picks a
    // cheapest ask, then the book shape rejects it.
    const std::vector<CompetingOffer> asks_only{
        mk(Side::Ask, kAsk + 5, 7),
        mk(Side::Ask, kAsk, 9),
        mk(Side::Ask, kAsk + 1, 11),
    };
    const auto d = evaluate_crossed_book(asks_only, 1.0, kXchCap);
    EXPECT_EQ(d.verdict, CrossedBookVerdict::NoBook);
    EXPECT_EQ(d.best_ask_price, 0);
    EXPECT_EQ(d.best_ask_size.v, 0)
        << "a NoBook decision reported a counterparty size next to a zero "
           "price -- the three ask fields must be written as a unit";
    EXPECT_TRUE(d.best_ask_offer_id.empty());
    EXPECT_EQ(d.take_size.v, 0);
    EXPECT_EQ(d.cap_mojos.v, 0);
}

TEST(CrossedBook, UncrossedBookIsNotCrossed)
{
    const std::vector<CompetingOffer> book{
        mk(Side::Bid, kAsk - 1, 10),   // bid strictly below ask
        mk(Side::Ask, kAsk, 10),
    };
    const auto d = evaluate_crossed_book(book, 1.0, kXchCap);
    EXPECT_EQ(d.verdict, CrossedBookVerdict::NotCrossed);
    EXPECT_EQ(d.take_size.v, 0);
    EXPECT_EQ(d.best_bid_price, kAsk - 1);
    EXPECT_EQ(d.best_ask_price, kAsk);
}

TEST(CrossedBook, CrossedButThinEdgeDeclines)
{
    // 100 bps of edge against a 250 bps minimum.
    const auto d = evaluate_crossed_book(crossed_book(10), 250.0, kXchCap);
    EXPECT_EQ(d.verdict, CrossedBookVerdict::EdgeTooThin);
    EXPECT_EQ(d.take_size.v, 0);
    EXPECT_NEAR(d.edge_bps, 100.0, 1e-6);
}

TEST(CrossedBook, TouchingBookIsCrossedWithZeroEdge)
{
    // bid == ask is "crossed" by 9c's >= test, and carries exactly 0 bps.
    // It survives only a zero minimum -- which is what the old code did too.
    const std::vector<CompetingOffer> book{
        mk(Side::Bid, kAsk, 10), mk(Side::Ask, kAsk, 10)};
    EXPECT_EQ(evaluate_crossed_book(book, 1.0, kXchCap).verdict,
              CrossedBookVerdict::EdgeTooThin);
    EXPECT_EQ(evaluate_crossed_book(book, 0.0, kXchCap).verdict,
              CrossedBookVerdict::Take);
}

// -- The cap boundary -------------------------------------------------------

TEST(CrossedBook, ExactlyAtCapIsTakenBecause9dsFilterIsInclusive)
{
    // 9d's filter is `best_ask_a_size <= max_take_mojos`. An offer of exactly
    // the cap fits the cap. This is not cosmetic: 104 of the recent XCH/DBX
    // detections logged exactly the cap, so if any of those offers is exactly
    // 5.0 XCH this boundary decides whether we trade at all.
    const auto d = evaluate_crossed_book(crossed_book(kXchCap), 1.0, kXchCap);
    EXPECT_EQ(d.verdict, CrossedBookVerdict::Take);
    EXPECT_EQ(d.take_size.v, kXchCap);
    EXPECT_EQ(d.take_size.v, d.best_ask_size.v);
}

TEST(CrossedBook, OneMojoOverCapIsSkippedNotClamped)
{
    // THE REGRESSION TEST. Reinstate `std::min(best_ask_size, cap)` and this
    // fails twice over: the verdict becomes Take, and take_size becomes the
    // cap rather than 0.
    const Mojo oversized = kXchCap + 1;
    const auto d = evaluate_crossed_book(crossed_book(oversized), 1.0, kXchCap);

    EXPECT_EQ(d.verdict, CrossedBookVerdict::SizeExceedsCap);
    EXPECT_EQ(d.take_size.v, 0);
    EXPECT_NE(d.take_size.v, kXchCap);       // the clamped value, explicitly
    EXPECT_EQ(d.best_ask_size.v, oversized); // the TRUE size, for the log
    EXPECT_EQ(d.cap_mojos.v, kXchCap);
}

TEST(CrossedBook, TheLiveAtCapCaseIsDeclinedRatherThanTakenAtTheCap)
{
    // The 208 real detections: the log said size=5000000000000 because the
    // clamp had already happened. Reconstruct one -- a 12 XCH offer against
    // the 5 XCH cap -- and assert that what reaches the caller is a refusal
    // carrying the true 12 XCH, not a take of 5.
    const Mojo real_offer = 12'000'000'000'000LL;
    const auto d = evaluate_crossed_book(crossed_book(real_offer), 1.0, kXchCap);

    EXPECT_EQ(d.verdict, CrossedBookVerdict::SizeExceedsCap);
    EXPECT_EQ(d.take_size.v, 0);
    EXPECT_EQ(d.best_ask_size.v, real_offer);
}

TEST(CrossedBook, TheOneHistoricalSuccessIsUnchanged)
{
    // 2026-08-31T13:47:43 XCH/DBX TOOK edge=14.4bps size=1289726060241. Below
    // the cap, so min(size, cap) == size and the recorded size was already
    // exact. Every surviving take must be bit-identical to the old code.
    const Mojo settled = 1'289'726'060'241LL;
    const auto d = evaluate_crossed_book(crossed_book(settled), 1.0, kXchCap);

    EXPECT_EQ(d.verdict, CrossedBookVerdict::Take);
    EXPECT_EQ(d.take_size.v, settled);
    EXPECT_EQ(d.take_size.v, d.best_ask_size.v);
}

// -- Zero size, which 9c never guarded --------------------------------------

TEST(CrossedBook, ZeroSizeOfferIsRefusedBecauseTheFillWouldNotBeRecorded)
{
    // 9c had no size > 0 test, so best_ask_size == 0 produced take_size == 0,
    // the take FIRED, and record_taker_fill() returned early on
    // base_mojos <= 0 -- a settled on-chain take with no taker_fills row and
    // no ledger legs at all. 9d guards this; now so does 9c.
    const auto d = evaluate_crossed_book(crossed_book(0), 1.0, kXchCap);
    EXPECT_EQ(d.verdict, CrossedBookVerdict::ZeroSizeOffer);
    EXPECT_EQ(d.take_size.v, 0);

    const auto neg = evaluate_crossed_book(crossed_book(-1), 1.0, kXchCap);
    EXPECT_EQ(neg.verdict, CrossedBookVerdict::ZeroSizeOffer);
    EXPECT_EQ(neg.take_size.v, 0);
}

TEST(CrossedBook, ZeroPricedAskCannotManufactureInfiniteEdge)
{
    // A zero-priced ask would have given edge = (bid - 0) / 0 = +inf, cleared
    // any minimum, and been taken. Excluding it strictly reduces action.
    const std::vector<CompetingOffer> book{
        mk(Side::Bid, kBid, 10),
        mk(Side::Ask, 0, 10, "free-money"),
        mk(Side::Ask, kAsk, 10, "real"),
    };
    const auto d = evaluate_crossed_book(book, 1.0, kXchCap);
    EXPECT_EQ(d.best_ask_offer_id, "real");
    EXPECT_EQ(d.best_ask_price, kAsk);
    EXPECT_EQ(d.verdict, CrossedBookVerdict::Take);
}

// -- We skip; we do not re-select -------------------------------------------

TEST(CrossedBook, OversizedCheapestAskMakesUsSkipTheWholePairNotPickTheNextAsk)
{
    // The name is the specification. Re-selecting a smaller ask inside the
    // cross would capture more edge, but it would make us lift offers we do
    // not lift today -- which breaks the property that makes this change safe
    // to ship on a live money path: the set of offers we take must be a
    // strict SUBSET of what the old code took. 9d skips (engine.cpp Step 9d
    // size filter) and 9c now matches it. Widening this is a follow-up with
    // its own risk argument, not a tidy-up.
    const std::vector<CompetingOffer> book{
        mk(Side::Bid, kBid, kXchCap),
        mk(Side::Ask, kAsk, kXchCap + 1, "cheapest-but-huge"),
        mk(Side::Ask, kAsk + 1, 1000, "pricier-but-would-fit"),
    };
    const auto d = evaluate_crossed_book(book, 1.0, kXchCap);

    EXPECT_EQ(d.verdict, CrossedBookVerdict::SizeExceedsCap);
    EXPECT_EQ(d.take_size.v, 0);
    EXPECT_EQ(d.best_ask_offer_id, "cheapest-but-huge");
    EXPECT_NE(d.best_ask_offer_id, "pricier-but-would-fit");
}

TEST(CrossedBook, SelectionPicksTheCheapestAskAndTheHighestBid)
{
    const std::vector<CompetingOffer> book{
        mk(Side::Bid, kBid - 5, 10, "weak-bid"),
        mk(Side::Bid, kBid, 10, "best-bid"),
        mk(Side::Ask, kAsk + 7, 10, "dear-ask"),
        mk(Side::Ask, kAsk, 10, "cheap-ask"),
    };
    const auto d = evaluate_crossed_book(book, 1.0, kXchCap);
    EXPECT_EQ(d.best_bid_price, kBid);
    EXPECT_EQ(d.best_ask_price, kAsk);
    EXPECT_EQ(d.best_ask_offer_id, "cheap-ask");
}

// -- The CAT denomination, end to end ---------------------------------------

TEST(CrossedBook, BycOfferOfFiveThousandAndOneMojosExceedsTheFiveThousandCap)
{
    // BYC/wUSDC.b: 1,116 of 1,219 detections ever logged sat exactly on the
    // 5000-mojo cap. Under a kMojosPerXch cap this same call would return
    // Take, so this test is red the instant the denominations are unified.
    const Mojo cap = cap_mojos_for(kMaxTake, kCatDenom);
    ASSERT_EQ(cap, kCatCap);

    EXPECT_EQ(evaluate_crossed_book(crossed_book(5001), 1.0, cap).verdict,
              CrossedBookVerdict::SizeExceedsCap);
    EXPECT_EQ(evaluate_crossed_book(crossed_book(5000), 1.0, cap).verdict,
              CrossedBookVerdict::Take);
}

// -- The invariant, over many synthetic books -------------------------------

TEST(CrossedBook, TakeInvariantHoldsOverEverySyntheticBook)
{
    // The property, as a biconditional. One direction stops the clamp; the
    // other stops the same bug wearing different clothes -- a caller reading
    // take_size off a DECLINED decision and handing it to record_taker_fill.
    //
    //     take_size != 0  <=>  verdict == Take
    //     verdict == Take  =>  take_size == best_ask_size <= cap_mojos
    //
    // Sizes straddle every boundary that matters: zero, one, the cap minus
    // one, the cap, the cap plus one, and absurd. Caps include the unusable
    // ones. Prices cover uncrossed, touching and crossed books.
    const Mojo sizes[] = {
        -1, 0, 1, 999, kCatCap - 1, kCatCap, kCatCap + 1,
        kXchCap - 1, kXchCap, kXchCap + 1,
        std::numeric_limits<Mojo>::max(),
    };
    const Mojo caps[] = {0, -1, 1, kCatCap, kXchCap,
                         std::numeric_limits<Mojo>::max()};
    const Mojo bids[] = {kAsk - 1, kAsk, kBid, kAsk * 2};
    const double mins[] = {-10.0, 0.0, 1.0, 100.0, 10000.0};

    int takes = 0;
    int declines = 0;

    for (const Mojo size : sizes) {
        for (const Mojo cap : caps) {
            for (const Mojo bid : bids) {
                for (const double min_edge : mins) {
                    const std::vector<CompetingOffer> book{
                        mk(Side::Bid, bid, size),
                        mk(Side::Ask, kAsk, size),
                    };
                    const auto d = evaluate_crossed_book(book, min_edge, cap);

                    if (d.verdict == CrossedBookVerdict::Take) {
                        ++takes;
                        // The clamp can never appear here.
                        ASSERT_EQ(d.take_size.v, d.best_ask_size.v)
                            << "size=" << size << " cap=" << cap;
                        ASSERT_GT(d.take_size.v, 0);
                        ASSERT_LE(d.take_size.v, d.cap_mojos.v)
                            << "size=" << size << " cap=" << cap;
                        ASSERT_GT(d.cap_mojos.v, 0);
                    } else {
                        ++declines;
                        ASSERT_EQ(d.take_size.v, 0)
                            << "a declined decision must carry no size: "
                            << "size=" << size << " cap=" << cap;
                    }

                    // The reported size is ALWAYS the counterparty's, never a
                    // derived one -- the log rule, enforced at the source.
                    ASSERT_TRUE(d.best_ask_size.v == size || d.best_ask_size.v == 0)
                        << "size=" << size;
                }
            }
        }
    }

    // Guard against a vacuous pass: a helper that returned Skip for
    // everything would satisfy every assertion above.
    EXPECT_GT(takes, 0);
    EXPECT_GT(declines, 0);
}

TEST(CrossedBook, ADefaultConstructedDecisionDeclines)
{
    // A forgotten initialisation must fail CLOSED.
    const xop::execution::CrossedBookDecision d;
    EXPECT_EQ(d.verdict, CrossedBookVerdict::NoBook);
    EXPECT_EQ(d.take_size.v, 0);
    EXPECT_EQ(d.cap_mojos.v, 0);
}
