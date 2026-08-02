/**
 * @file test_settled_fill.cpp
 * @brief Unit tests for OfferManager::parse_settled_fill -- extraction of
 *        SETTLED size/price from a confirmed wallet trade record.
 *
 * Background: detect_fills() historically copied the POSTED price/size from
 * the tracked PendingOffer into the Fill, discarding the settled amounts in
 * the wallet record's "summary".  parse_settled_fill() closes that hole.
 * The helper is static and pure (no RPC client needed), so these tests
 * exercise the exact code path detect_fills() uses to populate the Fill:
 *
 *   - parse OK      -> Fill.size/price = settled values
 *   - parse nullopt -> Fill.size/price = posted values + WARNING log
 *
 * Unit conventions under test (dimensional analysis):
 *   summary amounts are RAW MOJOS of each asset (XCH 1e12/unit, CAT 1e3/unit)
 *   Fill.size  = settled base mojos
 *   Fill.price = quote_units_per_base_unit * kMojosPerXch
 *              = quote_mojos * base_denom * kMojosPerXch
 *                / (base_mojos * quote_denom)
 * which is the exact inverse of xop::quote_mojos_for (types.hpp).
 */

#include <xop/execution/offer_manager.hpp>
#include <xop/types.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>

using json = nlohmann::json;
using xop::Mojo;
using xop::Side;
using xop::kMojosPerXch;
using xop::quote_mojos_for;
using xop::PairConfig;
using xop::execution::OfferManager;
using xop::execution::SettledFill;

namespace {

constexpr const char* kUsdcId =
    "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";

PairConfig make_xch_usdc_pair() {
    PairConfig pc;
    pc.base_asset_id       = "xch";
    pc.quote_asset_id      = kUsdcId;
    pc.name                = "XCH/wUSDC.b";
    pc.base_mojos_per_unit = 1'000'000'000'000LL;  // XCH: 1e12 mojos/unit
    pc.quote_mojos_per_unit = 1'000LL;             // CAT: 1e3  mojos/unit
    return pc;
}

/// Build a wallet trade record with a settled summary.
/// offered/requested map asset_id -> raw mojos, per the Chia wallet format.
json make_record(const json& offered, const json& requested) {
    return json{
        {"trade_id", "0xabc123"},
        {"status", "CONFIRMED"},
        {"summary", {{"offered", offered},
                     {"requested", requested},
                     {"fees", 50'000'000}}},
    };
}

// ---------------------------------------------------------------------------
// (i) settled == posted -> identical values to the posted offer (the Fill
//     detect_fills builds is bit-identical to the pre-fix behaviour).
// ---------------------------------------------------------------------------

TEST(SettledFill, AskFullSettlementMatchesPosted) {
    const auto pc = make_xch_usdc_pair();

    // Posted ask: sell 1 XCH at 20 wUSDC.b per XCH.
    const Mojo posted_size  = 1'000'000'000'000LL;         // 1 XCH in mojos
    const Mojo posted_price = 20 * kMojosPerXch;           // 20 * 1e12 pseudo

    // The posted quote leg, via the canonical helper: 20'000 CAT mojos.
    const Mojo quote_leg = static_cast<Mojo>(std::llround(quote_mojos_for(
        static_cast<double>(posted_size),
        static_cast<double>(posted_price),
        static_cast<double>(pc.base_mojos_per_unit),
        static_cast<double>(pc.quote_mojos_per_unit))));
    ASSERT_EQ(quote_leg, 20'000);

    // Ask: we GAVE base (offered), GOT quote (requested).
    const json rec = make_record(json{{"xch", posted_size}},
                                 json{{kUsdcId, quote_leg}});

    const auto settled = OfferManager::parse_settled_fill(rec, Side::Ask, pc);
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->size,  posted_size);
    EXPECT_EQ(settled->price, posted_price);
}

TEST(SettledFill, BidFullSettlementMatchesPosted) {
    const auto pc = make_xch_usdc_pair();

    // Posted bid: buy 2 XCH at 19.5 wUSDC.b per XCH.
    const Mojo posted_size  = 2'000'000'000'000LL;
    const Mojo posted_price = static_cast<Mojo>(19.5 * 1e12);
    const Mojo quote_leg    = 39'000;  // 2 * 19.5 units * 1e3 mojos/unit

    // Bid: we GAVE quote (offered), GOT base (requested).
    const json rec = make_record(json{{kUsdcId, quote_leg}},
                                 json{{"xch", posted_size}});

    const auto settled = OfferManager::parse_settled_fill(rec, Side::Bid, pc);
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->size,  posted_size);
    EXPECT_EQ(settled->price, posted_price);
}

// ---------------------------------------------------------------------------
// (ii) partial settlement -> settled size and the implied settled price.
// ---------------------------------------------------------------------------

TEST(SettledFill, AskPartialSettlementHalfSize) {
    const auto pc = make_xch_usdc_pair();

    // Posted: sell 1 XCH for 20'000 CAT mojos (20/XCH).  Settled: half.
    const Mojo settled_base  = 500'000'000'000LL;  // 0.5 XCH
    const Mojo settled_quote = 10'000;             // 10 wUSDC.b

    const json rec = make_record(json{{"xch", settled_base}},
                                 json{{kUsdcId, settled_quote}});

    const auto settled = OfferManager::parse_settled_fill(rec, Side::Ask, pc);
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->size, settled_base);
    // Implied price unchanged: 10 units / 0.5 units = 20 per XCH.
    EXPECT_EQ(settled->price, 20 * kMojosPerXch);
}

TEST(SettledFill, PartialSettlementWithDifferentImpliedPrice) {
    const auto pc = make_xch_usdc_pair();

    // Settled: gave 0.5 XCH, got 9 wUSDC.b -> implied 18 per XCH.
    const json rec = make_record(json{{"xch", 500'000'000'000LL}},
                                 json{{kUsdcId, 9'000}});

    const auto settled = OfferManager::parse_settled_fill(rec, Side::Ask, pc);
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->size, 500'000'000'000LL);
    EXPECT_EQ(settled->price, 18 * kMojosPerXch);

    // Roundtrip through the canonical helper reproduces the settled quote
    // mojos exactly -- the two formulas are inverses.
    const double roundtrip = quote_mojos_for(
        static_cast<double>(settled->size),
        static_cast<double>(settled->price),
        static_cast<double>(pc.base_mojos_per_unit),
        static_cast<double>(pc.quote_mojos_per_unit));
    EXPECT_EQ(static_cast<Mojo>(std::llround(roundtrip)), 9'000);
}

TEST(SettledFill, CatCatPairUsesBothDenominations) {
    // BYC/wUSDC.b-style pair: both legs are CATs (1e3 mojos/unit).
    PairConfig pc;
    pc.base_asset_id        = "aaaa";
    pc.quote_asset_id       = "bbbb";
    pc.name                 = "BYC/wUSDC.b";
    pc.base_mojos_per_unit  = 1'000LL;
    pc.quote_mojos_per_unit = 1'000LL;

    // Sold 100 BYC (100'000 mojos) for 99 wUSDC.b (99'000 mojos).
    const json rec = make_record(json{{"aaaa", 100'000}},
                                 json{{"bbbb", 99'000}});

    const auto settled = OfferManager::parse_settled_fill(rec, Side::Ask, pc);
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->size, 100'000);
    // 0.99 quote units per base unit * 1e12 (integer literal: 0.99 * 1e12
    // computed in double truncates to 989'999'999'999 under static_cast).
    EXPECT_EQ(settled->price, 990'000'000'000LL);
}

// ---------------------------------------------------------------------------
// (iii) summary missing / malformed -> nullopt, which detect_fills() maps to
//       the POSTED-value fallback plus a WARNING log (the warn branch is the
//       only consumer of nullopt; see detect_fills, offer_manager.cpp).
// ---------------------------------------------------------------------------

TEST(SettledFill, MissingSummaryReturnsNullopt) {
    const auto pc = make_xch_usdc_pair();
    const json rec = {{"trade_id", "0xabc"}, {"status", "CONFIRMED"}};
    EXPECT_FALSE(OfferManager::parse_settled_fill(rec, Side::Ask, pc)
                     .has_value());
}

TEST(SettledFill, NonObjectSummaryReturnsNullopt) {
    const auto pc = make_xch_usdc_pair();
    const json rec = {{"trade_id", "0xabc"}, {"summary", "corrupt"}};
    EXPECT_FALSE(OfferManager::parse_settled_fill(rec, Side::Ask, pc)
                     .has_value());
}

TEST(SettledFill, EmptyLegsReturnNullopt) {
    const auto pc = make_xch_usdc_pair();
    const json rec = make_record(json::object(), json{{kUsdcId, 20'000}});
    EXPECT_FALSE(OfferManager::parse_settled_fill(rec, Side::Ask, pc)
                     .has_value());
}

TEST(SettledFill, WrongSideAssetsReturnNullopt) {
    // Record has base on the REQUESTED side but we were the Ask (we should
    // have GIVEN base) -> legs do not match our side -> nullopt.
    const auto pc = make_xch_usdc_pair();
    const json rec = make_record(json{{kUsdcId, 20'000}},
                                 json{{"xch", 1'000'000'000'000LL}});
    EXPECT_FALSE(OfferManager::parse_settled_fill(rec, Side::Ask, pc)
                     .has_value());
    // The same record parsed as a Bid is valid.
    EXPECT_TRUE(OfferManager::parse_settled_fill(rec, Side::Bid, pc)
                    .has_value());
}

TEST(SettledFill, ZeroOrNegativeAmountsReturnNullopt) {
    const auto pc = make_xch_usdc_pair();
    const json zero_base = make_record(json{{"xch", 0}},
                                       json{{kUsdcId, 20'000}});
    EXPECT_FALSE(OfferManager::parse_settled_fill(zero_base, Side::Ask, pc)
                     .has_value());
    const json neg_quote = make_record(json{{"xch", 1'000'000'000'000LL}},
                                       json{{kUsdcId, -5}});
    EXPECT_FALSE(OfferManager::parse_settled_fill(neg_quote, Side::Ask, pc)
                     .has_value());
}

TEST(SettledFill, StringEncodedAmountsAreParsed) {
    // Some Chia wallet versions serialise amounts as strings.
    const auto pc = make_xch_usdc_pair();
    const json rec = make_record(json{{"xch", "1000000000000"}},
                                 json{{kUsdcId, "20000"}});
    const auto settled = OfferManager::parse_settled_fill(rec, Side::Ask, pc);
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->size,  1'000'000'000'000LL);
    EXPECT_EQ(settled->price, 20 * kMojosPerXch);
}

}  // namespace
