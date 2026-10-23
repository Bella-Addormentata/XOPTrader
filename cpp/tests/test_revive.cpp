// test_revive.cpp -- [REVIVE2 2026-10-22] revive_market decisions for thin
// markets (xop/strategy/revive.hpp) and the feed-listing CEX leg that lets a
// revive pair price an empty or one-sided book (xop/feed_listings.hpp).

#include <gtest/gtest.h>

#include "xop/feed_listings.hpp"
#include "xop/strategy/revive.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono_literals;
using xop::revive::LossBudget;

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

// The same three states as xop::FlashCrashState, without pulling in the
// engine header.
enum class S { Normal, Crash, Recovery };

}  // namespace

// ---------------------------------------------------------------------------
// book_is_thin
// ---------------------------------------------------------------------------

TEST(ReviveBook, ATwoSidedThirdPartyBookIsNotThin)
{
    EXPECT_FALSE(xop::revive::book_is_thin(1.584, 2.000, true));
}

TEST(ReviveBook, AMissingSideIsThin)
{
    // wmilliETH.b/XCH on 2026-10-22: asks only.
    EXPECT_TRUE(xop::revive::book_is_thin(0.0, 1.8277, true));
    EXPECT_TRUE(xop::revive::book_is_thin(1.4850, 0.0, true));
    EXPECT_TRUE(xop::revive::book_is_thin(0.0, 0.0, true));
}

TEST(ReviveBook, ACrossedOrLockedBookIsThin)
{
    EXPECT_TRUE(xop::revive::book_is_thin(2.0, 1.5, true));
    EXPECT_TRUE(xop::revive::book_is_thin(1.5, 1.5, true));
}

TEST(ReviveBook, TouchesThatMayBeOursAreThin)
{
    EXPECT_TRUE(xop::revive::book_is_thin(1.584, 2.000, false))
        << "a raw ticker can be our own offers: it is no third-party book";
}

TEST(ReviveBook, NonFinitePricesAreThin)
{
    EXPECT_TRUE(xop::revive::book_is_thin(kNaN, 2.0, true));
    EXPECT_TRUE(xop::revive::book_is_thin(1.5, kInf, true));
}

// ---------------------------------------------------------------------------
// half_spread_floor_bps
// ---------------------------------------------------------------------------

TEST(ReviveSpread, AThinBookRaisesTheFloor)
{
    EXPECT_DOUBLE_EQ(xop::revive::half_spread_floor_bps(141.0, true, 300.0),
                     300.0);
}

TEST(ReviveSpread, AWiderNormalFloorIsKept)
{
    EXPECT_DOUBLE_EQ(xop::revive::half_spread_floor_bps(350.0, true, 300.0),
                     350.0);
}

TEST(ReviveSpread, ATwoSidedBookOrAZeroFloorChangesNothing)
{
    EXPECT_DOUBLE_EQ(xop::revive::half_spread_floor_bps(141.0, false, 300.0),
                     141.0);
    EXPECT_DOUBLE_EQ(xop::revive::half_spread_floor_bps(141.0, true, 0.0),
                     141.0);
    EXPECT_DOUBLE_EQ(xop::revive::half_spread_floor_bps(141.0, true, kNaN),
                     141.0);
}

// ---------------------------------------------------------------------------
// capped_tier_size
// ---------------------------------------------------------------------------

TEST(ReviveSize, ALargerTierIsCapped)
{
    EXPECT_EQ(xop::revive::capped_tier_size(5'000, 1'000, 1'000), 1'000);
}

TEST(ReviveSize, TheCapNeverGoesBelowTheMinimumSize)
{
    EXPECT_EQ(xop::revive::capped_tier_size(5'000, 500, 1'000), 1'000)
        << "a sub-minimum cap would turn every tier back into dust";
}

TEST(ReviveSize, ASmallerTierOrNoCapIsUnchanged)
{
    EXPECT_EQ(xop::revive::capped_tier_size(800, 1'000, 500), 800);
    EXPECT_EQ(xop::revive::capped_tier_size(5'000, 0, 1'000), 5'000);
}

// ---------------------------------------------------------------------------
// next_crash_state -- the global machine's transitions, for one pair
// ---------------------------------------------------------------------------

TEST(ReviveCrashState, NormalEntersCrashOnlyOnACrash)
{
    using xop::revive::next_crash_state;
    EXPECT_EQ(next_crash_state(S::Normal, true, false, false), S::Crash);
    EXPECT_EQ(next_crash_state(S::Normal, false, false, false), S::Normal);
}

TEST(ReviveCrashState, CrashNeedsPhaseOneStabilityToRecover)
{
    using xop::revive::next_crash_state;
    EXPECT_EQ(next_crash_state(S::Crash, true, true, true), S::Crash);
    EXPECT_EQ(next_crash_state(S::Crash, false, false, false), S::Crash);
    EXPECT_EQ(next_crash_state(S::Crash, false, true, false), S::Recovery);
}

TEST(ReviveCrashState, RecoveryNeedsPhaseTwoAndFallsBackOnARecrash)
{
    using xop::revive::next_crash_state;
    EXPECT_EQ(next_crash_state(S::Recovery, false, true, false), S::Recovery);
    EXPECT_EQ(next_crash_state(S::Recovery, false, true, true), S::Normal);
    EXPECT_EQ(next_crash_state(S::Recovery, true, true, true), S::Crash);
}

static_assert(xop::revive::next_crash_state(S::Normal, true, true, true)
              == S::Crash);

// ---------------------------------------------------------------------------
// LossBudget
// ---------------------------------------------------------------------------

namespace {
const LossBudget::TimePoint kT0 = LossBudget::TimePoint{} + 1000h;
}  // namespace

TEST(ReviveLossBudget, NoSamplesIsNoDrawdown)
{
    LossBudget b;
    EXPECT_DOUBLE_EQ(b.drawdown_usd("wmilliETH.b/XCH"), 0.0);
    EXPECT_FALSE(b.exceeded("wmilliETH.b/XCH", 10.0));
}

TEST(ReviveLossBudget, DrawdownIsPeakToLatest)
{
    LossBudget b;
    b.record("p", kT0, 2.0);
    b.record("p", kT0 + 2min, 6.0);      // peak
    b.record("p", kT0 + 4min, -5.0);     // now 11 below the peak
    EXPECT_DOUBLE_EQ(b.drawdown_usd("p"), 11.0);
    EXPECT_TRUE(b.exceeded("p", 10.0));
    EXPECT_FALSE(b.exceeded("p", 12.0));
}

TEST(ReviveLossBudget, ExactlyTheBudgetIsNotOver)
{
    LossBudget b;
    b.record("p", kT0, 10.0);
    b.record("p", kT0 + 2min, 0.0);
    EXPECT_FALSE(b.exceeded("p", 10.0));
}

TEST(ReviveLossBudget, ARecoveryBackTowardThePeakShrinksTheDrawdown)
{
    LossBudget b;
    b.record("p", kT0, 0.0);
    b.record("p", kT0 + 2min, -12.0);
    ASSERT_TRUE(b.exceeded("p", 10.0));
    b.record("p", kT0 + 4min, -4.0);
    EXPECT_FALSE(b.exceeded("p", 10.0));
}

TEST(ReviveLossBudget, ALossOlderThanTheWindowStopsCounting)
{
    LossBudget b(24h, 60s);
    b.record("p", kT0, 20.0);                  // the old peak
    b.record("p", kT0 + 1h, 0.0);
    ASSERT_TRUE(b.exceeded("p", 10.0));
    b.record("p", kT0 + 25h, 0.0);             // the peak has aged out
    EXPECT_FALSE(b.exceeded("p", 10.0));
    EXPECT_DOUBLE_EQ(b.drawdown_usd("p"), 0.0);
}

TEST(ReviveLossBudget, CloseSamplesShareABucketButKeepItsPeak)
{
    LossBudget b(24h, 60s);
    b.record("p", kT0, 10.0);
    b.record("p", kT0 + 10s, -5.0);   // same bucket: latest -5, peak stays 10
    EXPECT_DOUBLE_EQ(b.drawdown_usd("p"), 15.0)
        << "coalescing must not erase the peak it replaced";
}

TEST(ReviveLossBudget, ALossInsideTheFirstIntervalIsCaught)
{
    // [review, PR #179] With the latest-only coalescing, 0 then -20 inside
    // one interval left a single -20 sample, and another -20 a minute later
    // still read zero drawdown, so the $10 budget missed a $20 loss.
    LossBudget b(24h, 60s);
    b.record("p", kT0, 0.0);
    b.record("p", kT0 + 20s, -20.0);
    b.record("p", kT0 + 60s, -20.0);
    EXPECT_DOUBLE_EQ(b.drawdown_usd("p"), 20.0);
    EXPECT_TRUE(b.exceeded("p", 10.0));
}

// ---------------------------------------------------------------------------
// PairFlows -- the pair's own trading P&L
// ---------------------------------------------------------------------------

TEST(RevivePairFlows, NoFillsIsZeroWithoutNeedingAnyPrice)
{
    const xop::revive::PairFlows f;
    const auto v = f.value_usd(0.0, 1.43, 0.0);
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 0.0);
}

TEST(RevivePairFlows, ABidIsMarkedOnBothLegs)
{
    // wmilliETH.b/XCH: buy 2 milliETH at 1.50 XCH, XCH at $1.40.
    xop::revive::PairFlows f;
    f.apply_fill(/*bid=*/true, 2.0, 1.50, 0.0);
    EXPECT_DOUBLE_EQ(f.base_units, 2.0);
    EXPECT_DOUBLE_EQ(f.quote_units, -3.0);
    // Mid falls to 1.40 XCH: 2 x 1.40 - 3.0 = -0.2 XCH = -$0.28.
    EXPECT_NEAR(*f.value_usd(1.40, 1.40, 1.40), -0.28, 1e-12);
    // Mid rises to 1.60: +0.2 XCH = +$0.28.
    EXPECT_NEAR(*f.value_usd(1.60, 1.40, 1.40), 0.28, 1e-12);
}

TEST(RevivePairFlows, ARoundTripRealizesTheSpreadAndPaysFees)
{
    xop::revive::PairFlows f;
    f.apply_fill(true, 1.0, 1.50, 0.005);    // buy at 1.50
    f.apply_fill(false, 1.0, 1.56, 0.005);   // sell at 1.56
    EXPECT_DOUBLE_EQ(f.base_units, 0.0);
    // +0.06 XCH spread - 0.01 XCH fees = 0.05 XCH at $1.40, at any mid.
    EXPECT_NEAR(*f.value_usd(0.0, 1.40, 1.40), 0.07, 1e-12);
}

TEST(RevivePairFlows, AnUnknownPriceIsNotALoss)
{
    xop::revive::PairFlows f;
    f.apply_fill(true, 2.0, 1.50, 0.005);
    EXPECT_FALSE(f.value_usd(0.0, 1.40, 1.40).has_value()) << "no mid";
    EXPECT_FALSE(f.value_usd(1.50, 0.0, 1.40).has_value()) << "no quote USD";
    EXPECT_FALSE(f.value_usd(1.50, 1.40, kNaN).has_value()) << "no XCH USD";
}

TEST(RevivePairFlows, BadFillsAreIgnored)
{
    xop::revive::PairFlows f;
    f.apply_fill(true, 0.0, 1.5, 0.0);
    f.apply_fill(true, 1.0, -1.5, 0.0);
    f.apply_fill(true, kNaN, 1.5, 0.0);
    EXPECT_DOUBLE_EQ(f.base_units, 0.0);
    EXPECT_DOUBLE_EQ(f.quote_units, 0.0);
}

// ---------------------------------------------------------------------------
// cap_units_to_mojos
// ---------------------------------------------------------------------------

TEST(ReviveSizeCap, ConvertsUnitsToMojos)
{
    EXPECT_EQ(xop::revive::cap_units_to_mojos(1.0, 1e12), 1'000'000'000'000LL);
    EXPECT_EQ(xop::revive::cap_units_to_mojos(2.5, 1000.0), 2'500);
}

TEST(ReviveSizeCap, ATinyPositiveCapNeverBecomesTheNoCapSentinel)
{
    // [review, PR #179] 0.0001 units of a 1000-mojo CAT is 0.1 mojo, which
    // rounded to 0 -- the "no cap" value -- and left a 5-unit tier uncapped.
    EXPECT_EQ(xop::revive::cap_units_to_mojos(0.0001, 1000.0), 1);
    EXPECT_EQ(xop::revive::capped_tier_size(5'000, 1, 1'000), 1'000)
        << "a one-mojo cap still caps to the minimum size";
}

TEST(ReviveSizeCap, SaturatesInsteadOfOverflowing)
{
    EXPECT_EQ(xop::revive::cap_units_to_mojos(1e6, 1e15), 9'000'000'000'000'000'000LL);
}

TEST(ReviveSizeCap, NonPositiveOrNonFiniteIsNoCap)
{
    EXPECT_EQ(xop::revive::cap_units_to_mojos(0.0, 1e12), 0);
    EXPECT_EQ(xop::revive::cap_units_to_mojos(-1.0, 1e12), 0);
    EXPECT_EQ(xop::revive::cap_units_to_mojos(kNaN, 1e12), 0);
    EXPECT_EQ(xop::revive::cap_units_to_mojos(1.0, 0.0), 0);
}

TEST(ReviveLossBudget, PairsAreIndependent)
{
    LossBudget b;
    b.record("a", kT0, 0.0);
    b.record("a", kT0 + 2min, -20.0);
    b.record("b", kT0, 0.0);
    EXPECT_TRUE(b.exceeded("a", 10.0));
    EXPECT_FALSE(b.exceeded("b", 10.0));
}

TEST(ReviveLossBudget, ZeroOrNonFiniteBudgetDisablesAndNaNSamplesAreIgnored)
{
    LossBudget b;
    b.record("p", kT0, 0.0);
    b.record("p", kT0 + 2min, -50.0);
    EXPECT_FALSE(b.exceeded("p", 0.0));
    EXPECT_FALSE(b.exceeded("p", kNaN));

    LossBudget c;
    c.record("p", kT0, 0.0);
    c.record("p", kT0 + 2min, kNaN);
    EXPECT_DOUBLE_EQ(c.drawdown_usd("p"), 0.0);
}

// ---------------------------------------------------------------------------
// listed_cross_price -- the CEX leg a revive pair gets from feed listings
// ---------------------------------------------------------------------------

TEST(ListedCrossPrice, MatchesTheHardcodedMilliEthLeg)
{
    const std::map<std::string, double> prices{
        {"ethereum", 2'480.0}, {"chia", 1.60}};
    const auto px = xop::listed_cross_price("wmilliETH.b/XCH", prices);
    ASSERT_TRUE(px.has_value());
    // Step 1's hardcoded formula: (ethereum / 1000) / chia.
    EXPECT_NEAR(*px, (2'480.0 / 1000.0) / 1.60, 1e-12);
}

TEST(ListedCrossPrice, PricesAnyPairWithBothLegsListed)
{
    const std::map<std::string, double> prices{
        {"chia", 1.60}, {"dexie-bucks", 0.0172}};
    const auto px = xop::listed_cross_price("XCH/DBX", prices);
    ASSERT_TRUE(px.has_value());
    EXPECT_NEAR(*px, 1.60 / 0.0172, 1e-9);
}

TEST(ListedCrossPrice, AnUnlistedLegHasNoPrice)
{
    const std::map<std::string, double> prices{{"chia", 1.60}};
    EXPECT_FALSE(xop::listed_cross_price("XCH/BYC", prices).has_value())
        << "BYC has no feed listing";
    EXPECT_FALSE(xop::listed_cross_price("BYC/wUSDC.b", prices).has_value());
}

TEST(ListedCrossPrice, AMissingOrNonPositiveQuoteHasNoPrice)
{
    EXPECT_FALSE(xop::listed_cross_price(
        "XCH/DBX", std::map<std::string, double>{{"chia", 1.60}}));
    EXPECT_FALSE(xop::listed_cross_price(
        "XCH/DBX",
        std::map<std::string, double>{{"chia", 1.60}, {"dexie-bucks", 0.0}}));
    EXPECT_FALSE(xop::listed_cross_price(
        "XCH/DBX",
        std::map<std::string, double>{{"chia", kInf}, {"dexie-bucks", 0.02}}));
    EXPECT_FALSE(xop::listed_cross_price(
        "not-a-pair", std::map<std::string, double>{{"chia", 1.60}}));
}

// ---------------------------------------------------------------------------
// quote_usd_price -- the quote leg's USD price for the loss budget
// ---------------------------------------------------------------------------

TEST(ReviveQuoteUsd, AnXchQuoteUsesXchsPrice)
{
    EXPECT_DOUBLE_EQ(xop::revive::quote_usd_price(true, 1.43, 99.0, 99.0), 1.43);
    EXPECT_DOUBLE_EQ(xop::revive::quote_usd_price(true, 0.0, 99.0, 99.0), 0.0);
}

TEST(ReviveQuoteUsd, AListedNonParQuoteIsPricedFromItsListing)
{
    // [review, PR #179] wmilliETH.b/DBX: the accounting factor is 0 for a
    // non-par, non-XCH quote, so the listed price must come first or the
    // budget never sees a loss.
    EXPECT_DOUBLE_EQ(xop::revive::quote_usd_price(false, 1.43, 0.0172, 0.0),
                     0.0172);
}

TEST(ReviveQuoteUsd, TheCarriedFactorIsTheLastResort)
{
    EXPECT_DOUBLE_EQ(xop::revive::quote_usd_price(false, 1.43, 0.0, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(xop::revive::quote_usd_price(false, 1.43, kNaN, 0.0), 0.0);
}

TEST(ReviveLossBudget, AZeroBaselineCatchesAFirstHeartbeatLoss)
{
    // [review, PR #179] Step 2 books a fill before Step 11's first sample.
    // Without a baseline the first sample is already -20 and the window
    // reads no drawdown; the engine records 0 before the first fill.
    LossBudget b;
    EXPECT_FALSE(b.has_samples("p"));
    b.record("p", kT0, 0.0);                 // the baseline
    EXPECT_TRUE(b.has_samples("p"));
    b.record("p", kT0 + 2min, -20.0);
    EXPECT_TRUE(b.exceeded("p", 10.0));

    LossBudget no_baseline;
    no_baseline.record("p", kT0 + 2min, -20.0);
    no_baseline.record("p", kT0 + 4min, -20.0);
    EXPECT_FALSE(no_baseline.exceeded("p", 10.0))
        << "the counterfactual the baseline exists for";
}

TEST(RevivePairFlows, IsEmptyUntilTheFirstFill)
{
    xop::revive::PairFlows f;
    EXPECT_TRUE(f.is_empty());
    f.apply_fill(true, 1.0, 1.5, 0.0);
    EXPECT_FALSE(f.is_empty());
}

// ---------------------------------------------------------------------------
// select_resting_cancels -- revive maintenance, independent of quoting
// ---------------------------------------------------------------------------

namespace {
std::vector<xop::revive::RestingOffer> sample_book()
{
    return {
        {"a1", "wmilliETH.b/XCH", 1'000, false},
        {"a2", "wmilliETH.b/XCH", 1'500, false},   // above a 1-unit cap
        {"a3", "wmilliETH.b/XCH", 2'000, true},    // already cancelling
        {"x1", "XCH/DBX", 9'000'000'000'000, false},
    };
}
}  // namespace

TEST(ReviveMaintenance, AThinBookCancelsOnlyOffersAboveTheLimit)
{
    // [review, PR #179] The cap must bind resting offers even when the pair
    // cannot quote (empty ladder, invalid quote): the selection reads only
    // the book's thinness and the limit, never quote eligibility.
    const std::unordered_map<std::string, xop::revive::RestingRule> rules{
        {"wmilliETH.b/XCH", {false, 1'000}}};
    const auto picks = xop::revive::select_resting_cancels(sample_book(), rules);
    ASSERT_EQ(picks.size(), 1u);
    EXPECT_EQ(picks[0].offer_id, "a2");
    EXPECT_FALSE(picks[0].over_budget);
}

TEST(ReviveMaintenance, OverBudgetCancelsEverythingRestingOnThePair)
{
    // [review, PR #179] Read from the budget directly, so a breach during
    // the pair's own crash still takes its offers down.
    const std::unordered_map<std::string, xop::revive::RestingRule> rules{
        {"wmilliETH.b/XCH", {true, 0}}};
    const auto picks = xop::revive::select_resting_cancels(sample_book(), rules);
    ASSERT_EQ(picks.size(), 2u);
    for (const auto& p : picks) {
        EXPECT_TRUE(p.over_budget);
        EXPECT_NE(p.offer_id, "a3") << "an offer already cancelling is left alone";
    }
}

TEST(ReviveMaintenance, PairsWithoutARuleAreNeverTouched)
{
    EXPECT_TRUE(xop::revive::select_resting_cancels(sample_book(), {}).empty());
    const std::unordered_map<std::string, xop::revive::RestingRule> rules{
        {"wmilliETH.b/XCH", {false, 0}}};
    EXPECT_TRUE(xop::revive::select_resting_cancels(sample_book(), rules).empty())
        << "no budget breach and no size limit: nothing to do";
}
