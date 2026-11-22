// [WIDEBOOK-NOQUOTE / WIDEBOOK-ISOLATE 2026-11-21] A book too wide to price.
//
// On 2026-11-21 XCH/BYC's book went 158% wide (bid 0.10, ask 0.90) with its
// fair value Unavailable, the ladder centred near the book's midpoint, and
// four asks at 0.49 to 0.63 against a fair price of about 0.87 were taken.

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "xop/execution/market_data.hpp"
#include "xop/risk/limits.hpp"

using xop::Mojo;
using xop::PreTradeCheck;

namespace {

struct Sample {
    double price;
    double spread_bps;
};

// A price-history sample as the quote gate reads it.
struct Tick {
    xop::BlockHeight block_height;
    double           price;
    double           spread_bps;
};

// The flash-crash states, as Engine names them.
enum class Phase { Normal, Crash, Recovery };

using Step = PreTradeCheck::WideCrashStep<Phase>;

Step route(std::optional<Phase> held, bool crashing, bool counted,
           bool stable1 = false, bool stable2 = false)
{
    return PreTradeCheck::wide_crash_step(held, crashing, counted, stable1, stable2);
}

// What the engine's loop makes of one heartbeat's pairs: whether any pair the
// global guard judges is crashing.
bool global_guard_sees_a_crash(const std::vector<std::pair<Step, bool>>& routed)
{
    for (const auto& [step, crashing] : routed) {
        if (step.global && crashing) return true;
    }
    return false;
}

PreTradeCheck::CrashSeries series_of(const std::vector<Sample>& samples, double max_spread)
{
    return PreTradeCheck::crash_series(samples, max_spread);
}

}  // namespace

TEST(WideBookQuotes, NothingIsPostedOnATooWideBookWithoutAFairValue)
{
    EXPECT_TRUE(PreTradeCheck::book_too_wide_to_quote(15'871.0, 2'000.0, true, false))
        << "the 2026-11-21 XCH/BYC book";
    EXPECT_FALSE(PreTradeCheck::book_too_wide_to_quote(15'871.0, 2'000.0, true, true))
        << "a usable fair value prices it";
    EXPECT_FALSE(PreTradeCheck::book_too_wide_to_quote(1'500.0, 2'000.0, true, false))
        << "narrow enough to price";
    EXPECT_FALSE(PreTradeCheck::book_too_wide_to_quote(15'871.0, 2'000.0, false, false))
        << "one-sided books belong to the thin-book rules";
    EXPECT_FALSE(PreTradeCheck::book_too_wide_to_quote(15'871.0, 0.0, true, false))
        << "0 switches it off";
    EXPECT_FALSE(PreTradeCheck::book_too_wide_to_quote(
        std::numeric_limits<double>::quiet_NaN(), 2'000.0, true, false))
        << "an unmeasured spread never holds a pair";
}

TEST(WideBookQuotes, ADropOnlyTheWideBooksShowIsTheirPairsAlone)
{
    // Narrow samples near 0.87; the near bid vanishes and the mid reads 0.50
    // from a 15,871 bps book.
    std::vector<Sample> s(10, Sample{0.87, 150.0});
    s.push_back({0.50, 15'871.0});
    const auto series = series_of(s, 2'000.0);
    EXPECT_TRUE(PreTradeCheck::series_crashing(series, 0.20, 100))
        << "the wide low still counts as a drop";
    EXPECT_FALSE(PreTradeCheck::series_crashing_counted(series, 0.20, 100))
        << "the narrow books show none: only this pair is held back";
}

TEST(WideBookQuotes, ADropTheNarrowBooksConfirmIsEveryonesCrash)
{
    std::vector<Sample> s(10, Sample{0.87, 150.0});
    s.push_back({0.60, 300.0});
    const auto series = series_of(s, 2'000.0);
    EXPECT_TRUE(PreTradeCheck::series_crashing(series, 0.20, 100));
    EXPECT_TRUE(PreTradeCheck::series_crashing_counted(series, 0.20, 100));
}

TEST(WideBookQuotes, TheCountedCheckKeepsItsWindow)
{
    std::vector<Sample> s{{0.90, 100.0}};
    for (int i = 0; i < 5; ++i) s.push_back({0.60, 100.0});
    const auto series = series_of(s, 2'000.0);
    EXPECT_TRUE(PreTradeCheck::series_crashing_counted(series, 0.20, 0));
    EXPECT_FALSE(PreTradeCheck::series_crashing_counted(series, 0.20, 5))
        << "the high has left the window";
}

TEST(WideBookQuotes, AFairValueOnAFrozenCoinGeckoPriceDoesNotPriceIt)
{
    // [review #252] During a CoinGecko outage the solve keeps its last
    // anchors and the result is stamped fresh every heartbeat.
    using xop::FairValue;
    using xop::FairValueTier;
    using xop::fair_value_prices_wide_book;
    const auto with = [](FairValueTier tier) {
        FairValue fv{};
        fv.price = 0.87;
        fv.tier = tier;
        return std::optional<FairValue>{fv};
    };
    EXPECT_FALSE(fair_value_prices_wide_book(std::nullopt, true));
    EXPECT_FALSE(fair_value_prices_wide_book(with(FairValueTier::Unavailable), true));
    for (const auto tier : {FairValueTier::CexDirect, FairValueTier::Inferred,
                            FairValueTier::Triangulated}) {
        EXPECT_TRUE(fair_value_prices_wide_book(with(tier), true)) << xop::to_string(tier);
    }
    EXPECT_FALSE(fair_value_prices_wide_book(with(FairValueTier::CexDirect), false));
    EXPECT_FALSE(fair_value_prices_wide_book(with(FairValueTier::Inferred), false));
    EXPECT_TRUE(fair_value_prices_wide_book(with(FairValueTier::Triangulated), false))
        << "cross-checked by other paths";
}

// ---------------------------------------------------------------------------
// [review #252] The routing: which pair is held on its own, which goes to the
// global guard, and how a hold ends.
// ---------------------------------------------------------------------------

TEST(WideBookRouting, ADropOnlyAWideBookShowsHoldsThatPairAndNotTheOthers)
{
    // XCH/BYC's wide book shows a drop; XCH/wUSDC.b is quiet.
    const Step byc  = route(std::nullopt, /*crashing=*/true, /*counted=*/false);
    const Step usdc = route(std::nullopt, /*crashing=*/false, /*counted=*/false);

    EXPECT_EQ(byc.held, Phase::Crash);
    EXPECT_FALSE(byc.global) << "left out of the global guard while it is held";
    EXPECT_EQ(usdc.held, std::nullopt);
    EXPECT_TRUE(usdc.global);
    EXPECT_FALSE(global_guard_sees_a_crash({{byc, true}, {usdc, false}}))
        << "the other pairs keep quoting";
}

TEST(WideBookRouting, ADropTheNarrowBooksConfirmIsTheGlobalGuardsWhetherHeldOrNot)
{
    const Step fresh = route(std::nullopt, true, /*counted=*/true);
    EXPECT_EQ(fresh.held, std::nullopt);
    EXPECT_TRUE(fresh.global);

    // Held on its own until now: the hold ends and the drop is everyone's.
    for (const Phase was : {Phase::Crash, Phase::Recovery}) {
        const Step escalated = route(was, true, /*counted=*/true);
        EXPECT_EQ(escalated.held, std::nullopt);
        EXPECT_TRUE(escalated.global);
        EXPECT_TRUE(global_guard_sees_a_crash({{escalated, true}}));
    }
}

TEST(WideBookRouting, AHoldEndsThroughRecoveryAsTheGlobalGuardDoes)
{
    // Still dropping: Crash. Stable for phase 1: Recovery. For phase 2: free.
    Step step = route(Phase::Crash, /*crashing=*/true, false);
    EXPECT_EQ(step.held, Phase::Crash);
    step = route(Phase::Crash, false, false, /*stable1=*/false);
    EXPECT_EQ(step.held, Phase::Crash) << "not stable yet";
    step = route(Phase::Crash, false, false, /*stable1=*/true);
    EXPECT_EQ(step.held, Phase::Recovery);
    EXPECT_FALSE(step.global);
    step = route(Phase::Recovery, false, false, true, /*stable2=*/false);
    EXPECT_EQ(step.held, Phase::Recovery);
    step = route(Phase::Recovery, false, false, true, /*stable2=*/true);
    EXPECT_EQ(step.held, std::nullopt) << "released";
    EXPECT_FALSE(step.global) << "its release does not move the global state that heartbeat";

    // From the next heartbeat it is an ordinary pair again.
    step = route(std::nullopt, false, false, true, true);
    EXPECT_EQ(step.held, std::nullopt);
    EXPECT_TRUE(step.global);
}

TEST(WideBookRouting, AWideOnlyDropDuringRecoveryIsItsOwnCrashAgain)
{
    const Step step = route(Phase::Recovery, /*crashing=*/true, /*counted=*/false, true, true);
    EXPECT_EQ(step.held, Phase::Crash);
    EXPECT_FALSE(step.global);
}

TEST(WideBookRouting, AHeldStateIsNeverNormal)
{
    // Engine keeps an entry only while a pair is held, and suppresses the
    // quotes of every pair that has one.
    for (const auto held : {std::optional<Phase>{}, std::optional<Phase>{Phase::Crash},
                            std::optional<Phase>{Phase::Recovery}}) {
        for (int bits = 0; bits < 16; ++bits) {
            const Step step = route(held, (bits & 1) != 0, (bits & 2) != 0,
                                    (bits & 4) != 0, (bits & 8) != 0);
            EXPECT_NE(step.held, Phase::Normal);
            if (step.held.has_value()) {
                EXPECT_FALSE(step.global) << "a held pair is never also the global guard's";
            }
        }
    }
}

// ---------------------------------------------------------------------------
// [review #252] The width the quote gate judges.
// ---------------------------------------------------------------------------

TEST(WideBookQuotes, TheGateReadsThePricingBooksWidthWhenThePublishedSpreadIsZero)
{
    // After a failed offers fetch the published spread is 0 (raw ticker)
    // while the mid still comes from the cached, wide book.
    const std::vector<Tick> history{{100, 0.87, 150.0}, {101, 0.50, 15'871.0}};
    const double width = PreTradeCheck::quote_gate_spread_bps(0.0, history, 101);
    EXPECT_DOUBLE_EQ(width, 15'871.0);
    EXPECT_TRUE(PreTradeCheck::book_too_wide_to_quote(width, 2'000.0, true, false));
    EXPECT_FALSE(PreTradeCheck::book_too_wide_to_quote(0.0, 2'000.0, true, false))
        << "the published spread alone would let it through";
}

TEST(WideBookQuotes, TheGatesWidthIsTheWiderOfTheTwoAndOnlyThisBlocksSample)
{
    const std::vector<Tick> history{{100, 0.87, 15'871.0}, {101, 0.87, 150.0}};
    EXPECT_DOUBLE_EQ(PreTradeCheck::quote_gate_spread_bps(2'500.0, history, 101), 2'500.0);
    EXPECT_DOUBLE_EQ(PreTradeCheck::quote_gate_spread_bps(120.0, history, 101), 150.0);
    EXPECT_DOUBLE_EQ(PreTradeCheck::quote_gate_spread_bps(120.0, history, 102), 120.0)
        << "no sample from this block: an older book's width says nothing";
    EXPECT_DOUBLE_EQ(PreTradeCheck::quote_gate_spread_bps(120.0, std::vector<Tick>{}, 101), 120.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_DOUBLE_EQ(PreTradeCheck::quote_gate_spread_bps(nan, history, 101), 150.0);
    const std::vector<Tick> unmeasured{{101, 0.87, nan}};
    EXPECT_DOUBLE_EQ(PreTradeCheck::quote_gate_spread_bps(120.0, unmeasured, 101), 120.0);
}
