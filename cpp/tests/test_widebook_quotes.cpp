// [WIDEBOOK-NOQUOTE / WIDEBOOK-ISOLATE 2026-11-21] A book too wide to price.
//
// On 2026-11-21 XCH/BYC's book went 158% wide (bid 0.10, ask 0.90) with its
// fair value Unavailable, the ladder centred near the book's midpoint, and
// four asks at 0.49 to 0.63 against a fair price of about 0.87 were taken.

#include <gtest/gtest.h>

#include <limits>
#include <optional>
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
