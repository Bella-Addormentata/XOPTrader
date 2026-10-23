// test_synthetic_price.cpp -- [SYNTH 2026-10-22] the synthetic quoting price
// for revive pairs with an unlisted leg (xop/strategy/synthetic_price.hpp),
// Dexie's trade-history parsing, and the market-data rule that a synthetic
// reference moves the published mid but never counts as evidence.

#include <gtest/gtest.h>

#include "xop/execution/fair_value_solver.hpp"
#include "xop/execution/market_data.hpp"
#include "xop/feed_listings.hpp"
#include "xop/rpc/dexie_client.hpp"
#include "xop/state.hpp"
#include "xop/strategy/revive.hpp"
#include "xop/strategy/synthetic_price.hpp"
#include "xop/types.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using xop::synth::PriceSource;
using xop::synth::TapeTrade;
using Clock = std::chrono::system_clock;

const Clock::time_point kNow = Clock::time_point{} + 480'000h;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

}  // namespace

// ---------------------------------------------------------------------------
// blend_sources
// ---------------------------------------------------------------------------

TEST(SynthBlend, OneSourceIsItself)
{
    const auto b = xop::synth::blend_sources({{1.835, 300.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->price, 1.835, 1e-12);
    EXPECT_NEAR(b->sigma_bps, 300.0, 1e-9);
}

TEST(SynthBlend, AgreeingSourcesTightenTheErrorBar)
{
    const auto b = xop::synth::blend_sources({{1.835, 300.0}, {1.835, 300.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->price, 1.835, 1e-12);
    EXPECT_NEAR(b->sigma_bps, 300.0 / std::sqrt(2.0), 1e-6);
}

TEST(SynthBlend, DisagreeingSourcesWidenToTheirSpread)
{
    // XCH/BYC on 2026-10-22: BYC's $1 par puts it at 1.426, the tape at
    // 1.835 -- 25% apart. Two "confident" sources must not average into a
    // confident middle.
    const auto b = xop::synth::blend_sources({{1.426, 160.0}, {1.835, 250.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_GT(b->price, 1.426);
    EXPECT_LT(b->price, 1.835);
    EXPECT_GT(b->sigma_bps, 1000.0)
        << "the blend's error bar must reflect the 25% disagreement";
}

TEST(SynthBlend, TheTighterSourceCarriesMoreWeight)
{
    const auto b = xop::synth::blend_sources({{1.0, 100.0}, {2.0, 1000.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_LT(b->price, 1.1);
}

TEST(SynthBlend, UnusableSourcesAreSkippedAndNoneIsNothing)
{
    EXPECT_FALSE(xop::synth::blend_sources({}).has_value());
    EXPECT_FALSE(xop::synth::blend_sources({{0.0, 100.0}, {kNaN, 100.0}})
                     .has_value());
    const auto b = xop::synth::blend_sources({{-1.0, 100.0}, {2.0, 100.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_NEAR(b->price, 2.0, 1e-12);
}

TEST(SynthBlend, TheErrorBarIsCapped)
{
    const auto b = xop::synth::blend_sources({{1.0, 100.0}, {100.0, 100.0}});
    ASSERT_TRUE(b.has_value());
    EXPECT_LE(b->sigma_bps, xop::synth::kMaxSigmaBps);
}

// ---------------------------------------------------------------------------
// cross_source / aged_source
// ---------------------------------------------------------------------------

TEST(SynthCross, XchOverACatIsTheCatsInverse)
{
    // XCH/BYC = XCH-in-XCH / BYC-in-XCH = 1 / 0.545 BYC per XCH.
    const auto c = xop::synth::cross_source({1.0, 0.0}, {0.545, 300.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->price, 1.0 / 0.545, 1e-12);
    EXPECT_NEAR(c->sigma_bps, 300.0, 1e-9);
}

TEST(SynthCross, TwoCatsAddTheirErrorsInQuadrature)
{
    // BYC/wUSDC.b from BYC 0.545 XCH and wUSDC.b 0.70 XCH.
    const auto c = xop::synth::cross_source({0.545, 300.0}, {0.70, 400.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->price, 0.545 / 0.70, 1e-12);
    EXPECT_NEAR(c->sigma_bps, 500.0, 1e-9);
}

TEST(SynthCross, AnUnusableLegIsNothing)
{
    EXPECT_FALSE(xop::synth::cross_source({1.0, 0.0}, {0.0, 100.0}).has_value());
    EXPECT_FALSE(xop::synth::cross_source({kNaN, 0.0}, {1.0, 100.0}).has_value());
}

TEST(SynthAged, TheErrorBarGrowsWithAgeAndIsCapped)
{
    const PriceSource last{1.835, 300.0};
    EXPECT_NEAR(xop::synth::aged_source(last, 0.0, 25.0).sigma_bps, 300.0, 1e-9);
    EXPECT_NEAR(xop::synth::aged_source(last, 10.0, 25.0).sigma_bps, 550.0, 1e-9);
    EXPECT_NEAR(xop::synth::aged_source(last, 10.0, 25.0).price, 1.835, 1e-12);
    EXPECT_NEAR(xop::synth::aged_source(last, 1e6, 25.0).sigma_bps,
                xop::synth::kMaxSigmaBps, 1e-9);
    EXPECT_NEAR(xop::synth::aged_source(last, -5.0, 25.0).sigma_bps, 300.0, 1e-9);
}

// ---------------------------------------------------------------------------
// tape_source
// ---------------------------------------------------------------------------

TEST(SynthTape, TheLiveBycTapeReadsAboutPointFiveFour)
{
    // BYC_XCH on Dexie, 2026-10-21/22: four trades, the newest ~5h old.
    const std::vector<TapeTrade> t{
        {0.545454545455, 1.1,   kNow - 5h},
        {0.547445255474, 5.48,  kNow - 20h},
        {0.536193029491, 5.595, kNow - 23h},
        {0.545454545455, 5.5,   kNow - 23h},
    };
    const auto s = xop::synth::tape_source(t, kNow);
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(s->price, 0.5431, 0.0005);
    // hypot(200 base, ~89 dispersion) + 5h x 25 = ~344.
    EXPECT_NEAR(s->sigma_bps, 343.75, 0.5);
}

TEST(SynthTape, TradesOutsideTheWindowOrInTheFutureAreIgnored)
{
    const std::vector<TapeTrade> t{
        {0.50, 10.0, kNow - 100h},   // outside 72h
        {0.60, 10.0, kNow + 1h},     // clock skew
        {0.55, 1.0,  kNow - 1h},
    };
    const auto s = xop::synth::tape_source(t, kNow);
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(s->price, 0.55, 1e-12);
}

TEST(SynthTape, NoUsableTradeIsNothing)
{
    EXPECT_FALSE(xop::synth::tape_source({}, kNow).has_value());
    EXPECT_FALSE(xop::synth::tape_source({{0.0, 1.0, kNow - 1h},
                                          {0.5, 0.0, kNow - 1h},
                                          {0.5, 1.0, kNow - 200h}},
                                         kNow)
                     .has_value());
}

TEST(SynthTape, ScatteredTradesWidenTheErrorBar)
{
    const std::vector<TapeTrade> tight{{0.54, 1.0, kNow}, {0.55, 1.0, kNow}};
    const std::vector<TapeTrade> wide{{0.40, 1.0, kNow}, {0.70, 1.0, kNow}};
    EXPECT_GT(xop::synth::tape_source(wide, kNow)->sigma_bps,
              xop::synth::tape_source(tight, kNow)->sigma_bps + 1000.0);
}

// ---------------------------------------------------------------------------
// half_spread_for_sigma
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// conservative_mark -- the revive loss budget's mark for a synthetic price
// ---------------------------------------------------------------------------

TEST(SynthMark, MarksOneErrorBarAgainstThePosition)
{
    const PriceSource p{2.0, 500.0};  // +-5%
    EXPECT_DOUBLE_EQ(xop::synth::conservative_mark(p, 3.0), 1.9)
        << "long base: the lower edge";
    EXPECT_DOUBLE_EQ(xop::synth::conservative_mark(p, -3.0), 2.1)
        << "short base: the upper edge";
    EXPECT_DOUBLE_EQ(xop::synth::conservative_mark(p, 0.0), 2.0);
}

TEST(SynthMark, TheErrorBarIsCappedAndUnusableIsNothing)
{
    EXPECT_DOUBLE_EQ(xop::synth::conservative_mark({2.0, 9000.0}, 1.0), 1.0)
        << "clipped to kMaxSigmaBps (50%), never a zero or negative mark";
    EXPECT_DOUBLE_EQ(xop::synth::conservative_mark({0.0, 100.0}, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(xop::synth::conservative_mark({kNaN, 100.0}, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(xop::synth::conservative_mark({2.0, kNaN}, 1.0), 0.0);
}

// ---------------------------------------------------------------------------
// The revive loss budget for a pair with an unlisted leg: leg-by-leg marks
// (PairFlows::value_usd_by_leg), the unlisted leg at its synthetic price's
// unfavourable edge (conservative_mark) -- as Engine::step_update_pnl does.
// ---------------------------------------------------------------------------

namespace {
// The engine's mark for an unlisted leg: its synthetic USD price, one error
// bar against the position.
double unlisted_mark(double usd, double sigma_bps, double units)
{
    return xop::synth::conservative_mark(PriceSource{usd, sigma_bps}, units);
}
const auto kBudgetT0 = Clock::time_point{} + 1000h;
}  // namespace

TEST(SynthBudget, AnUnlistedQuoteLegIsMarkedFromItsOwnUsdPrice)
{
    // Second review: XCH/BYC, net -40 XCH / +80 BYC, XCH steady at $1.43,
    // BYC (tape-priced) falls from $0.78 to $0.50. The real loss is
    // 80 x 0.28 = $22.40, over the default $10 budget.
    xop::revive::PairFlows flows;
    flows.base_units  = -40.0;
    flows.quote_units = 80.0;
    const double xch_usd = 1.43;
    auto value = [&](double byc_usd) {
        return flows.value_usd_by_leg(
            xch_usd, unlisted_mark(byc_usd, 0.0, flows.quote_units), xch_usd);
    };
    xop::revive::LossBudget budget;
    budget.record("XCH/BYC", kBudgetT0, *value(0.78));
    budget.record("XCH/BYC", kBudgetT0 + 2min, *value(0.50));
    EXPECT_NEAR(budget.drawdown_usd("XCH/BYC"), 22.40, 1e-9);
    EXPECT_TRUE(budget.exceeded("XCH/BYC", 10.0));

    // The defect this replaces: a par-backed mid (1.43 BYC per XCH) times
    // the tape-priced BYC marks XCH at $0.715 and sees only $6.384.
    const double par_mid = 1.43;
    const double stale_loss = *flows.value_usd(par_mid, 0.78, xch_usd)
                            - *flows.value_usd(par_mid, 0.50, xch_usd);
    EXPECT_NEAR(stale_loss, 6.384, 1e-9);
    EXPECT_FALSE(stale_loss > 10.0) << "which is why it missed the budget";
}

TEST(SynthBudget, AnUnlistedBaseLegIsMarkedFromItsOwnUsdPrice)
{
    // Third review: BYC/wUSDC.b, net +80 BYC / -62.40 wUSDC.b (listed, $1).
    // BYC's tape falls from $0.78 to $0.50: a $22.40 loss.
    xop::revive::PairFlows flows;
    flows.base_units  = 80.0;
    flows.quote_units = -62.40;
    auto value = [&](double byc_usd) {
        return flows.value_usd_by_leg(
            unlisted_mark(byc_usd, 0.0, flows.base_units), 1.0, 1.43);
    };
    xop::revive::LossBudget budget;
    budget.record("BYC/wUSDC.b", kBudgetT0, *value(0.78));
    budget.record("BYC/wUSDC.b", kBudgetT0 + 2min, *value(0.50));
    EXPECT_NEAR(budget.drawdown_usd("BYC/wUSDC.b"), 22.40, 1e-9);
    EXPECT_TRUE(budget.exceeded("BYC/wUSDC.b", 10.0));

    // The defect this replaces: a par-backed fair value (1.0 wUSDC.b per
    // BYC) records $17.60 before and after -- no loss at all.
    EXPECT_NEAR(*flows.value_usd(1.0, 1.0, 1.43), 17.60, 1e-9);
}

TEST(SynthBudget, EachLegIsMarkedAgainstThePosition)
{
    // Holding BYC marks it at its lower edge, owing it at its upper edge.
    EXPECT_DOUBLE_EQ(unlisted_mark(0.80, 500.0, 80.0), 0.76);
    EXPECT_DOUBLE_EQ(unlisted_mark(0.80, 500.0, -80.0), 0.84);
}

TEST(SynthBudget, OnlyAHeldLegNeedsAMark)
{
    xop::revive::PairFlows quote_only;
    quote_only.quote_units = 50.0;
    EXPECT_NEAR(*quote_only.value_usd_by_leg(0.0, 0.8, 1.43), 40.0, 1e-12)
        << "quote-only exposure needs no base mark (and no mid)";
    EXPECT_FALSE(quote_only.value_usd_by_leg(1.43, 0.0, 1.43).has_value())
        << "but cannot be valued without its own";

    xop::revive::PairFlows base_only;
    base_only.base_units = 2.0;
    EXPECT_NEAR(*base_only.value_usd_by_leg(1.43, 0.0, 1.43), 2.86, 1e-12);
    EXPECT_FALSE(base_only.value_usd_by_leg(kNaN, 0.8, 1.43).has_value());

    xop::revive::PairFlows fees;
    fees.fee_xch = 0.001;
    EXPECT_FALSE(fees.value_usd_by_leg(1.0, 1.0, 0.0).has_value());
    EXPECT_NEAR(*fees.value_usd_by_leg(0.0, 0.0, 2.0), -0.002, 1e-15);
}

// Fourth review: the peak is taken at the central marks and the latest
// value at the adverse ones. Discounting both lets the discount hide a fall.
namespace {
// +units of an unlisted coin against a listed $1 quote: central and adverse
// valuations at `price` with `sigma_bps`, as Step 11 computes them.
struct Valuation { double central; double adverse; };
Valuation coin_position(double units, double quote_units, double price,
                        double sigma_bps)
{
    xop::revive::PairFlows f;
    f.base_units  = units;
    f.quote_units = quote_units;
    return {*f.value_usd_by_leg(price, 1.0, 1.43),
            *f.value_usd_by_leg(unlisted_mark(price, sigma_bps, units), 1.0, 1.43)};
}
}  // namespace

TEST(SynthBudget, ANarrowingErrorBarCannotHideAFall)
{
    // +100 coins / -$50. The coin falls from $1 (1,000 bps) to a fresh
    // $0.85 trade (200 bps): a central loss of $15.
    const auto before = coin_position(100.0, -50.0, 1.00, 1000.0);
    const auto after  = coin_position(100.0, -50.0, 0.85, 200.0);
    EXPECT_NEAR(before.central, 50.0, 1e-9);
    EXPECT_NEAR(after.adverse, 33.3, 1e-9);

    xop::revive::LossBudget budget;
    budget.record("p", kBudgetT0, before.central, before.adverse);
    budget.record("p", kBudgetT0 + 2min, after.central, after.adverse);
    EXPECT_NEAR(budget.drawdown_usd("p"), 16.7, 1e-9)
        << "at least the $15 central loss";
    EXPECT_TRUE(budget.exceeded("p", 10.0));

    // The defect: both discounted, the budget saw $6.70.
    xop::revive::LossBudget both_discounted;
    both_discounted.record("p", kBudgetT0, before.adverse);
    both_discounted.record("p", kBudgetT0 + 2min, after.adverse);
    EXPECT_NEAR(both_discounted.drawdown_usd("p"), 6.7, 1e-9);
    EXPECT_FALSE(both_discounted.exceeded("p", 10.0));
}

TEST(SynthBudget, AConstantErrorBarCannotHideAFallEither)
{
    // +80 coins / -$40, $0.78 -> $0.65 at a constant 500 bps: a central
    // loss of $10.40, over the $10 budget.
    const auto before = coin_position(80.0, -40.0, 0.78, 500.0);
    const auto after  = coin_position(80.0, -40.0, 0.65, 500.0);
    xop::revive::LossBudget budget;
    budget.record("p", kBudgetT0, before.central, before.adverse);
    budget.record("p", kBudgetT0 + 2min, after.central, after.adverse);
    EXPECT_NEAR(budget.drawdown_usd("p"), 13.0, 1e-9);
    EXPECT_TRUE(budget.exceeded("p", 10.0));

    xop::revive::LossBudget both_discounted;
    both_discounted.record("p", kBudgetT0, before.adverse);
    both_discounted.record("p", kBudgetT0 + 2min, after.adverse);
    EXPECT_NEAR(both_discounted.drawdown_usd("p"), 9.88, 1e-9)
        << "the defect: $9.88, under the budget";
}

TEST(SynthBudget, UncertaintyItselfCountsAgainstTheBudget)
{
    // Disclosed behaviour, not an accident: with the price unchanged the
    // drawdown is the error bar on the position, so a large position in a
    // coin the bot cannot price closely stops quoting before it can lose
    // more than the budget unseen.
    const auto v = coin_position(80.0, -40.0, 0.78, 500.0);
    xop::revive::LossBudget budget;
    budget.record("p", kBudgetT0, v.central, v.adverse);
    EXPECT_NEAR(budget.drawdown_usd("p"), 80.0 * 0.78 * 0.05, 1e-9);
}

TEST(SynthBudget, TheTwoValueRecordNeverLetsAdverseExceedCentral)
{
    xop::revive::LossBudget budget;
    budget.record("p", kBudgetT0, 10.0, 12.0);   // adverse above central
    EXPECT_DOUBLE_EQ(budget.drawdown_usd("p"), 0.0);
    budget.record("p", kBudgetT0 + 2min, 10.0, kNaN);  // ignored
    EXPECT_DOUBLE_EQ(budget.drawdown_usd("p"), 0.0);
}

TEST(SynthBudget, AListedLegIsNotHeldStillByAStaleFeed)
{
    // Sixth review: +100 BYC (unlisted, $1 +-200 bps) / -100 DBX (listed).
    // CoinGecko is down; DBX's Dexie trades rise from $1 to $1.20 while its
    // cached feed price stays at $1. The real loss is at least $20.
    xop::revive::PairFlows flows;
    flows.base_units  = 100.0;
    flows.quote_units = -100.0;
    const double xch_usd = 1.43;
    auto record = [&](xop::revive::LossBudget& b, Clock::time_point t,
                      double dbx_usd, double dbx_sigma) {
        const double byc_c = 1.0;
        const double byc_a = unlisted_mark(1.0, 200.0, flows.base_units);
        const double dbx_a = unlisted_mark(dbx_usd, dbx_sigma, flows.quote_units);
        b.record("BYC/DBX", t, *flows.value_usd_by_leg(byc_c, dbx_usd, xch_usd),
                 *flows.value_usd_by_leg(byc_a, dbx_a, xch_usd));
    };

    // Stale: the cached $1 both times (a listed price carries no error bar).
    xop::revive::LossBudget stale;
    record(stale, kBudgetT0, 1.00, 0.0);
    record(stale, kBudgetT0 + 2min, 1.00, 0.0);
    EXPECT_NEAR(stale.drawdown_usd("BYC/DBX"), 2.0, 1e-9)
        << "the defect: only BYC's error bar shows";
    EXPECT_FALSE(stale.exceeded("BYC/DBX", 10.0));

    // Fixed: during the outage DBX is marked from its own trades.
    xop::revive::LossBudget fresh;
    record(fresh, kBudgetT0, 1.00, 200.0);
    record(fresh, kBudgetT0 + 2min, 1.20, 200.0);
    EXPECT_GE(fresh.drawdown_usd("BYC/DBX"), 22.0);
    EXPECT_TRUE(fresh.exceeded("BYC/DBX", 10.0));
}

TEST(SynthBudget, ExpiredTradesAgeTheMarkInsteadOfDroppingIt)
{
    // Third review: once a coin's trades leave the 72 h window the tape has
    // nothing, and the budget must neither go blind (stopping revival for a
    // quiet coin) nor fall back to a carried par factor. The engine keeps
    // the asset's last synthetic price and grows its error bar.
    const std::vector<TapeTrade> old = {
        {0.545, 1.0, kNow - 80h}, {0.540, 2.0, kNow - 79h}};
    EXPECT_FALSE(xop::synth::tape_source(old, kNow).has_value())
        << "precondition: every trade is outside the window";

    const PriceSource last{0.545, 600.0};     // as last computed
    const auto aged = xop::synth::aged_source(
        last, /*age_h=*/10.0, xop::synth::kAgedSigmaBpsPerHour);
    EXPECT_DOUBLE_EQ(aged.price, 0.545);
    EXPECT_DOUBLE_EQ(aged.sigma_bps, 850.0);

    // Held BYC is then marked lower the longer it goes unobserved: the
    // budget sees a growing loss, never a missing one.
    const double xch_usd = 1.43;
    const double fresh_mark = unlisted_mark(last.price * xch_usd, last.sigma_bps, 80.0);
    const double aged_mark = unlisted_mark(aged.price * xch_usd, aged.sigma_bps, 80.0);
    EXPECT_LT(aged_mark, fresh_mark);
    xop::revive::PairFlows flows;
    flows.quote_units = 80.0;
    EXPECT_TRUE(flows.value_usd_by_leg(0.0, aged_mark, xch_usd).has_value());
}

// ---------------------------------------------------------------------------
// Trade timestamps -> time points
// ---------------------------------------------------------------------------

TEST(SynthTime, AnInRangeTimestampConvertsExactly)
{
    const auto at = xop::synth::time_from_epoch_ms(1792635467000LL);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::milliseconds>(
                  at->time_since_epoch()).count(),
              1792635467000LL);
}

TEST(SynthTime, TimestampsBeyondTheClockAreRefusedBeforeConversion)
{
    // 1e15 ms fits int64 but overflows a nanosecond (Linux) or 100 ns
    // (MSVC) system_clock duration.
    EXPECT_FALSE(xop::synth::time_from_epoch_ms(1'000'000'000'000'000LL));
    EXPECT_FALSE(xop::synth::time_from_epoch_ms(
        std::numeric_limits<std::int64_t>::max()));
    EXPECT_FALSE(xop::synth::time_from_epoch_ms(0));
    EXPECT_FALSE(xop::synth::time_from_epoch_ms(-5));
    // The boundary itself: the clock's largest whole millisecond converts,
    // one past it does not.
    const std::int64_t max_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::duration::max()).count();
    EXPECT_TRUE(xop::synth::time_from_epoch_ms(max_ms).has_value());
    EXPECT_FALSE(xop::synth::time_from_epoch_ms(max_ms + 1));
}

// ---------------------------------------------------------------------------
// Which revive pairs take the synthetic price
// ---------------------------------------------------------------------------

TEST(SynthEligibility, OnlyAMissingListingSelectsTheSyntheticPrice)
{
    EXPECT_TRUE(xop::pair_has_unlisted_leg("XCH/BYC"));
    EXPECT_TRUE(xop::pair_has_unlisted_leg("BYC/wUSDC.b"));
    EXPECT_FALSE(xop::pair_has_unlisted_leg("XCH/wUSDC.b"));
    EXPECT_FALSE(xop::pair_has_unlisted_leg("wmilliETH.b/DBX"));
    EXPECT_FALSE(xop::pair_has_unlisted_leg("not-a-pair"));
}

TEST(SynthEligibility, AnEmptyFeedCacheDoesNotMakeAListedPairUnlisted)
{
    // The startup-outage case: both legs listed, no prices fetched yet.
    // listed_cross_price has nothing, but the pair must keep waiting for the
    // live feed rather than fall back to the tape.
    const std::map<std::string, double> no_prices;
    ASSERT_FALSE(xop::listed_cross_price("XCH/wUSDC.b", no_prices).has_value());
    EXPECT_FALSE(xop::pair_has_unlisted_leg("XCH/wUSDC.b"));
}

// ---------------------------------------------------------------------------
// The ladder centres on the synthetic price, not the raw estimate
// ---------------------------------------------------------------------------

TEST(SynthCentre, AnEmptyBookCentresOnTheSyntheticPriceNotTheEstimate)
{
    // Step 1: the tape (1.835 +-250 bps) and the estimate (1.426 +-160 bps)
    // blend to the synthetic price. Step 7 hands that price -- in place of
    // the estimate -- to the centre blend, with no book opinion.
    const auto synth = xop::synth::blend_sources(
        {PriceSource{1.835, 250.0}, PriceSource{1.426, 160.0}});
    ASSERT_TRUE(synth.has_value());
    EXPECT_NEAR(synth->price, 1.5344, 5e-4);

    const auto c = xop::fv::blend_quote_center(0.0, 0.0, synth->price,
                                               synth->sigma_bps);
    ASSERT_TRUE(c.ok);
    EXPECT_NEAR(c.center, synth->price, 1e-9)
        << "the tape must move the centre, not only the spread";
    EXPECT_DOUBLE_EQ(c.sigma_bps, synth->sigma_bps);
}

TEST(SynthSpread, CoversTwoErrorBarsBetweenTheFloorAndTheCap)
{
    EXPECT_DOUBLE_EQ(xop::synth::half_spread_for_sigma(300.0, 100.0), 300.0);
    EXPECT_DOUBLE_EQ(xop::synth::half_spread_for_sigma(300.0, 600.0), 1200.0);
    EXPECT_DOUBLE_EQ(xop::synth::half_spread_for_sigma(300.0, 4000.0), 5000.0);
    EXPECT_DOUBLE_EQ(xop::synth::half_spread_for_sigma(300.0, kNaN), 300.0);
}

// ---------------------------------------------------------------------------
// Dexie historical_trades parsing
// ---------------------------------------------------------------------------

TEST(DexieHistoricalTrades, ParsesTheLiveShape)
{
    const auto j = nlohmann::json::parse(R"({
        "success": true, "ticker_id": "ae15..._xch",
        "trades": [
            {"trade_id": "a", "price": "0.545454545455", "base_volume": "1.1",
             "target_volume": "0.6", "trade_timestamp": 1792635467000, "type": "buy"},
            {"trade_id": "b", "price": "0.536193029491", "base_volume": "5.595",
             "target_volume": "3", "trade_timestamp": 1792569992000, "type": "sell"}
        ]})");
    const auto t = xop::rpc::parse_historical_trades(j);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_NEAR(t[0].price, 0.545454545455, 1e-12);
    EXPECT_NEAR(t[0].base_volume, 1.1, 1e-12);
    EXPECT_NEAR(t[0].target_volume, 0.6, 1e-12);
    EXPECT_EQ(t[0].timestamp_ms, 1792635467000LL);
}

TEST(DexieHistoricalTrades, ANonSuccessOrBadEntriesYieldNothing)
{
    EXPECT_TRUE(xop::rpc::parse_historical_trades(nlohmann::json::parse(
        R"({"success": false, "error_message": "invalid ticker_id"})")).empty());
    EXPECT_TRUE(xop::rpc::parse_historical_trades(nlohmann::json::parse(
        R"({"success": true})")).empty());
    const auto t = xop::rpc::parse_historical_trades(nlohmann::json::parse(R"({
        "success": true, "trades": [
            {"price": "0", "base_volume": "1", "trade_timestamp": 1},
            {"price": "0.5", "base_volume": "-1", "trade_timestamp": 1},
            {"price": "abc", "base_volume": "1", "trade_timestamp": 1},
            {"price": 0.5, "base_volume": 2, "trade_timestamp": 0},
            {"price": 0.5, "base_volume": 2, "trade_timestamp": 5}
        ]})"));
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].timestamp_ms, 5);
}

TEST(DexieHistoricalTrades, AnOversizedTimestampIsRejectedNotCast)
{
    // 1e30 and 2^63 are finite and positive but outside int64: they must be
    // dropped before the cast, never converted.
    const auto t = xop::rpc::parse_historical_trades(nlohmann::json::parse(R"({
        "success": true, "trades": [
            {"price": 0.5, "base_volume": 2, "trade_timestamp": 1e30},
            {"price": 0.5, "base_volume": 2, "trade_timestamp": 9223372036854775808.0},
            {"price": 0.5, "base_volume": 2, "trade_timestamp": 1792635467000}
        ]})"));
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].timestamp_ms, 1792635467000LL);
}

TEST(DexieHistoricalTrades, TheV2RootFollowsTheConfiguredV1Base)
{
    EXPECT_EQ(xop::rpc::dexie_v2_base_url("https://api.dexie.space/v1"),
              "https://api.dexie.space/v2");
    EXPECT_EQ(xop::rpc::dexie_v2_base_url("https://api.dexie.space/v1/"),
              "https://api.dexie.space/v2");
    EXPECT_EQ(xop::rpc::dexie_v2_base_url("https://api-testnet.dexie.space"),
              "https://api-testnet.dexie.space/v2");
}

// ---------------------------------------------------------------------------
// A synthetic reference moves the published mid and is never evidence
// ---------------------------------------------------------------------------

namespace {
xop::MarketDataConfig synth_cfg()
{
    xop::MarketDataConfig cfg;
    cfg.min_competitor_offer_size = 0;
    return cfg;
}
}  // namespace

TEST(SyntheticReference, AnEmptyBookPublishesItsMidButNoValuationGrade)
{
    xop::State state;
    xop::MarketDataFeed feed(synth_cfg(), state);
    feed.ingest_block_height(100);
    feed.ingest_cex_reference("XCH/BYC", 1.835, Clock::now(), /*synthetic=*/true);
    feed.refresh({"XCH/BYC"});

    const auto snap = state.get_market("XCH/BYC");
    EXPECT_NEAR(static_cast<double>(snap.mid_price)
                    / static_cast<double>(xop::kMojosPerXch), 1.835, 1e-6)
        << "the pair must have a mid to quote from";
    EXPECT_FALSE(snap.mid_valuation_grade)
        << "a synthetic price must never mark equity";
    EXPECT_FALSE(feed.get_cex_reference("XCH/BYC").has_value())
        << "nor stand in for a CEX reference";
}

TEST(SyntheticReference, ARealReferenceStillGrantsGrade)
{
    // The counterfactual: the same price as a real CEX leg is graded.
    xop::State state;
    xop::MarketDataFeed feed(synth_cfg(), state);
    feed.ingest_block_height(100);
    feed.ingest_cex_reference("XCH/BYC", 1.835, Clock::now());
    feed.refresh({"XCH/BYC"});

    EXPECT_TRUE(state.get_market("XCH/BYC").mid_valuation_grade);
    EXPECT_TRUE(feed.get_cex_reference("XCH/BYC").has_value());
}

TEST(SyntheticReference, ATwoSidedBookKeepsItsOwnGradedMid)
{
    // A healthy two-sided book with a synthetic leg inside its spread. The
    // synthetic leg must not enter the published mid (it would otherwise
    // be graded through the book branch and reach equity and P&L): the mid
    // and grade must equal those of the same book with no synthetic leg.
    // Inside the spread on purpose: a leg outside it would be blended and
    // then clamped back to the book edge by the published-mid band, which
    // can hide a blend.
    auto offer = [](const std::string& id, xop::Side side, double price) {
        xop::CompetingOffer o;
        o.offer_id = id;
        o.side     = side;
        o.price    = static_cast<xop::Mojo>(
            std::llround(price * static_cast<double>(xop::kMojosPerXch)));
        o.size     = 5'000'000'000'000LL;
        return o;
    };
    const std::string pair = "XCH/BYC";
    auto run = [&](bool with_synthetic) {
        xop::MarketDataConfig cfg = synth_cfg();
        cfg.amm_blend_weight = 0.0;
        xop::State state;
        xop::MarketDataFeed feed(cfg, state);
        feed.ingest_block_height(100);
        feed.ingest_reference_anchor(pair, /*implied_cross=*/1.41,
                                     /*peg_target=*/0.0);
        feed.ingest_competing_offers(
            pair,
            {offer("b1", xop::Side::Bid, 1.38), offer("a1", xop::Side::Ask, 1.44)},
            {}, xop::kMojosPerXch, 1'000);
        if (with_synthetic) {
            feed.ingest_cex_reference(pair, 1.39, Clock::now(), /*synthetic=*/true);
        }
        feed.refresh({pair});
        return state.get_market(pair);
    };
    const auto control = run(false);
    ASSERT_GT(control.mid_price, 0);
    ASSERT_TRUE(control.mid_valuation_grade)
        << "precondition: the book alone must earn grade, or the assertion "
           "below proves nothing";

    const auto snap = run(true);
    EXPECT_EQ(snap.mid_price, control.mid_price)
        << "the synthetic leg moved a two-sided book's mid (blended 70/30)";
    EXPECT_TRUE(snap.mid_valuation_grade);
    EXPECT_FALSE(snap.mid_synthetic)
        << "a synthetic leg that contributed nothing is not provenance";
}

TEST(SyntheticReference, AnEmptyBookPublishesItsSyntheticProvenance)
{
    // The ungraded readers (USD anchor fallback, arbitrage scans, drift
    // corrector) need to know the mid is synthetic, not just ungraded.
    xop::State state;
    xop::MarketDataFeed feed(synth_cfg(), state);
    feed.ingest_block_height(100);
    feed.ingest_cex_reference("XCH/BYC", 1.835, Clock::now(), /*synthetic=*/true);
    feed.refresh({"XCH/BYC"});
    const auto snap = state.get_market("XCH/BYC");
    ASSERT_GT(snap.mid_price, 0);
    EXPECT_TRUE(snap.mid_synthetic);

    // The same price as a real reference is not synthetic.
    xop::State state2;
    xop::MarketDataFeed feed2(synth_cfg(), state2);
    feed2.ingest_block_height(100);
    feed2.ingest_cex_reference("XCH/BYC", 1.835, Clock::now());
    feed2.refresh({"XCH/BYC"});
    EXPECT_FALSE(state2.get_market("XCH/BYC").mid_synthetic);
}

TEST(SyntheticReference, AnUnrepresentablePriceIsRefused)
{
    // Sixth review: a tape-only price of 1e-8 XCH per coin crosses to 1e8
    // coins per XCH -- 1e20 in mojos, outside int64. It must not be stored
    // (the snapshot would convert it with undefined behaviour).
    xop::State state;
    xop::MarketDataFeed feed(synth_cfg(), state);
    feed.ingest_block_height(100);
    feed.ingest_cex_reference("XCH/CHEAP", 1e8, Clock::now(), /*synthetic=*/true);
    feed.refresh({"XCH/CHEAP"});
    const auto snap = state.get_market("XCH/CHEAP");
    EXPECT_EQ(snap.mid_price, 0);
    EXPECT_EQ(snap.cex_mid, 0);
    EXPECT_FALSE(snap.mid_synthetic);

    // Just inside the range still publishes.
    xop::State state2;
    xop::MarketDataFeed feed2(synth_cfg(), state2);
    feed2.ingest_block_height(100);
    feed2.ingest_cex_reference("XCH/CHEAP", 9e6, Clock::now(), true);
    feed2.refresh({"XCH/CHEAP"});
    EXPECT_EQ(state2.get_market("XCH/CHEAP").mid_price,
              static_cast<xop::Mojo>(9e6 * 1e12));
}

TEST(SyntheticReference, ARealReferenceClearsTheSyntheticMark)
{
    xop::State state;
    xop::MarketDataFeed feed(synth_cfg(), state);
    feed.ingest_block_height(100);
    feed.ingest_cex_reference("XCH/BYC", 1.835, Clock::now(), true);
    feed.ingest_cex_reference("XCH/BYC", 1.835, Clock::now(), false);
    feed.refresh({"XCH/BYC"});
    EXPECT_TRUE(feed.get_cex_reference("XCH/BYC").has_value());
}
