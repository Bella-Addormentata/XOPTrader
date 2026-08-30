// ---------------------------------------------------------------------------
// [PARANCHOR] The pure half of the CoinGecko fetch: vs_currencies
// derivation from declared pegs, and FX extraction from the response.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "xop/rpc/coingecko_parse.hpp"

using nlohmann::json;
using xop::PegRegistry;
using xop::PeggedAsset;
using xop::rpc::parse_simple_price;
using xop::rpc::vs_currencies_for;

namespace {

PeggedAsset pegged(const char* id, const char* cur) {
    PeggedAsset a;
    a.asset_id = id;
    a.symbol = id;
    a.peg_currency = cur;
    a.peg_target = 1.0;
    return a;
}

}  // namespace

TEST(VsCurrencies, usd_only_worlds_request_only_usd)
{
    EXPECT_EQ(vs_currencies_for(PegRegistry{}),
              (std::vector<std::string>{"usd"}));

    PegRegistry reg;
    ASSERT_TRUE(reg.add(pegged("a1", "USD")));
    EXPECT_EQ(vs_currencies_for(reg), (std::vector<std::string>{"usd"}));
}

TEST(VsCurrencies, non_usd_pegs_append_their_currencies_sorted_deduped)
{
    PegRegistry reg;
    ASSERT_TRUE(reg.add(pegged("a1", "JPY")));
    ASSERT_TRUE(reg.add(pegged("a2", "EUR")));
    ASSERT_TRUE(reg.add(pegged("a3", "EUR")));   // duplicate currency
    ASSERT_TRUE(reg.add(pegged("a4", "USD")));
    EXPECT_EQ(vs_currencies_for(reg),
              (std::vector<std::string>{"usd", "eur", "jpy"}));
}

TEST(Parse, usd_prices_extracted_and_junk_skipped)
{
    const json r = {
        {"chia", {{"usd", 1.43}}},
        {"junk-zero", {{"usd", 0.0}}},
        {"junk-neg", {{"usd", -3.0}}},
        {"junk-str", {{"usd", "1.43"}}},
        {"junk-shape", 7},
    };
    const auto out = parse_simple_price(
        r, {"chia", "junk-zero", "junk-neg", "junk-str", "junk-shape",
            "absent"},
        {"usd"});
    ASSERT_EQ(out.usd.size(), 1u);
    EXPECT_DOUBLE_EQ(out.usd.at("chia"), 1.43);
    EXPECT_TRUE(out.fx_usd_per.empty());
}

TEST(Parse, fx_cross_is_usd_over_currency_from_the_first_coin)
{
    // chia: 1.43 USD, 1.31 EUR -> EURUSD = 1.43/1.31.
    const json r = {
        {"chia", {{"usd", 1.43}, {"eur", 1.31}}},
        {"ethereum", {{"usd", 2450.0}, {"eur", 2000.0}}},  // would say 1.225
    };
    const auto out =
        parse_simple_price(r, {"chia", "ethereum"}, {"usd", "eur"});
    ASSERT_EQ(out.fx_usd_per.count("EUR"), 1u);
    // Deterministically the FIRST coin's ratio, keyed UPPERCASE.
    EXPECT_NEAR(out.fx_usd_per.at("EUR"), 1.43 / 1.31, 1e-12);
}

TEST(Parse, a_coin_without_the_currency_defers_to_the_next)
{
    const json r = {
        {"chia", {{"usd", 1.43}}},                          // no jpy
        {"ethereum", {{"usd", 2450.0}, {"jpy", 365'000.0}}},
    };
    const auto out =
        parse_simple_price(r, {"chia", "ethereum"}, {"usd", "jpy"});
    ASSERT_EQ(out.fx_usd_per.count("JPY"), 1u);
    EXPECT_NEAR(out.fx_usd_per.at("JPY"), 2450.0 / 365'000.0, 1e-15);
}

TEST(Parse, a_missing_currency_is_absent_never_one)
{
    const json r = {{"chia", {{"usd", 1.43}}}};
    const auto out = parse_simple_price(r, {"chia"}, {"usd", "eur"});
    EXPECT_EQ(out.fx_usd_per.count("EUR"), 0u);
}

TEST(Parse, junk_currency_quotes_are_skipped)
{
    const json r = {
        {"chia", {{"usd", 1.43}, {"eur", 0.0}}},
        {"ethereum", {{"usd", 2450.0}, {"eur", -1.0}}},
    };
    const auto out =
        parse_simple_price(r, {"chia", "ethereum"}, {"usd", "eur"});
    EXPECT_EQ(out.fx_usd_per.count("EUR"), 0u);
}

TEST(Parse, a_coin_with_junk_usd_contributes_no_fx_either)
{
    // The cross needs BOTH quotes from the same coin; a junk USD price
    // disqualifies that coin entirely rather than borrowing another's.
    const json r = {
        {"chia", {{"usd", 0.0}, {"eur", 1.31}}},
        {"ethereum", {{"usd", 2450.0}, {"eur", 2000.0}}},
    };
    const auto out =
        parse_simple_price(r, {"chia", "ethereum"}, {"usd", "eur"});
    ASSERT_EQ(out.fx_usd_per.count("EUR"), 1u);
    EXPECT_NEAR(out.fx_usd_per.at("EUR"), 2450.0 / 2000.0, 1e-12);
}
