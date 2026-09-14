// test_maker_fill_legs.cpp -- Unit tests for booking EVERY leg of a confirmed
// maker fill into the InventoryTracker ([FILL-LEGS 2026-09-13]).
//
// Step 2 used to book the base leg only.  These tests drive the exact
// functions the engine now calls (xop/accounting/maker_fill_legs.hpp) with a
// real Fill, a real PairConfig and a real InventoryTracker built the way the
// engine builds it (no_loss_constraint on).  Nothing here re-derives the quote
// formula: every expected number is a literal, computed independently by an
// IEEE-double emulation of the same arithmetic.
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/accounting/maker_fill_legs.hpp>
#include <xop/config.hpp>
#include <xop/risk/drawdown_breaker.hpp>
#include <xop/risk/inventory.hpp>
#include <xop/types.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>

namespace {

using xop::Mojo;
using xop::Side;
using xop::accounting::apply_maker_fill_legs;
using xop::accounting::maker_fill_inventory_legs;
using xop::accounting::MakerFillApplyResult;
using xop::accounting::MakerFillInventoryLegs;
using xop::accounting::rejected_fill_legs;

// Asset ids as configured live.
constexpr const char* kXch = "xch";
constexpr const char* kDbx =
    "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20";
constexpr const char* kByc =
    "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac";
constexpr const char* kWusdc =
    "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";
constexpr const char* kWmilliEthB =
    "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6";

constexpr Mojo kXchDenom = 1'000'000'000'000;  // mojos per XCH
constexpr Mojo kCatDenom = 1'000;              // mojos per CAT unit
constexpr xop::BlockHeight kBlock = 9'276'808;

xop::PairConfig pair_config(const char* base, const char* quote,
                            Mojo base_denom, Mojo quote_denom) {
    xop::PairConfig pc;
    pc.base_asset_id        = base;
    pc.quote_asset_id       = quote;
    pc.name                 = "TEST/PAIR";
    pc.base_mojos_per_unit  = base_denom;
    pc.quote_mojos_per_unit = quote_denom;
    return pc;
}

xop::PairConfig xch_dbx() {
    return pair_config(kXch, kDbx, kXchDenom, kCatDenom);
}

xop::Fill maker_fill(Side side, Mojo size, Mojo price, Mojo fee) {
    xop::Fill f{};
    f.offer_id     = "0xtestfill";
    f.pair_name    = "TEST/PAIR";
    f.side         = side;
    f.price        = price;
    f.size         = size;
    f.block_height = kBlock;
    f.fee_mojos    = fee;
    return f;
}

MakerFillInventoryLegs legs_for(const xop::PairConfig& pc, Side side,
                                Mojo size, Mojo price, Mojo base_usd,
                                Mojo fee, bool trusted = true) {
    return maker_fill_inventory_legs(maker_fill(side, size, price, fee), pc,
                                     base_usd, trusted);
}

// A record restored the way the engine restores inventory_state.
void restore(xop::InventoryTracker& inv, const char* asset, Mojo qty,
             double basis) {
    inv.restore_record(asset, qty, static_cast<double>(qty) * basis, false);
}

// Equity exactly as compute_portfolio_equity_usd builds it: units are
// net_inventory / 1e12 for xch and / 1e3 for CATs, marked at live prices.
double equity_usd(const xop::InventoryTracker& inv, double xch_usd,
                  double dbx_usd) {
    return xop::risk::portfolio_equity_usd({
        {static_cast<double>(inv.net_inventory(kXch)) / 1e12, xch_usd, 0.0},
        {static_cast<double>(inv.net_inventory(kDbx)) / 1e3, dbx_usd, 0.0},
    });
}

class MakerFillInventoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        risk_cfg_.soft_limit_pct           = 0.60;
        risk_cfg_.hard_limit_pct           = 0.80;
        risk_cfg_.single_cat_cap_pct       = 0.12;
        risk_cfg_.kelly_fraction           = 0.50;
        risk_cfg_.max_capital_per_pair_pct = 0.20;
    }

    xop::RiskConfig risk_cfg_;
    xop::Timestamp  now_ = std::chrono::system_clock::now();
};

// ============================================================================
// The legs: what gets booked, in which direction, at what price
// ============================================================================

// An ask on XCH/DBX: 1 XCH sold at 80 DBX.  The quote leg CREDITS 80 DBX
// (80,000 mojos) at the implied $0.02 (1.6e12 * 1e12 / 80e12 = 2e10).
TEST(MakerFillLegsTest, AskOnXchDbxCreditsDbxAtImpliedUsdPrice) {
    const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                               80'000'000'000'000, 1'600'000'000'000, 18'987);
    EXPECT_TRUE(legs.base.applies);
    EXPECT_FALSE(legs.base.is_buy);
    EXPECT_EQ(legs.base.qty_mojos, 1'000'000'000'000);
    EXPECT_EQ(legs.base.usd_pseudo_price, 1'600'000'000'000);

    EXPECT_TRUE(legs.quote.applies);
    EXPECT_TRUE(legs.quote.is_buy);
    EXPECT_EQ(legs.quote.qty_mojos, 80'000);
    EXPECT_EQ(legs.quote.usd_pseudo_price, 20'000'000'000);

    EXPECT_TRUE(legs.fee.applies);
    EXPECT_FALSE(legs.fee.is_buy);
    EXPECT_EQ(legs.fee.qty_mojos, 18'987);
    EXPECT_EQ(legs.fee.usd_pseudo_price, 0);

    EXPECT_FALSE(legs.quote_invalid);
}

// A bid on XCH/BYC at par: 2 XCH bought at 1.5 BYC.  The quote leg DEBITS
// 3 BYC (3,000 mojos) at 1.5e12 * 1e12 / 1.5e12 = $1.00.
TEST(MakerFillLegsTest, BidOnXchBycDebitsBycAtImpliedUsdPrice) {
    const auto legs = legs_for(pair_config(kXch, kByc, kXchDenom, kCatDenom),
                               Side::Bid, 2'000'000'000'000,
                               1'500'000'000'000, 1'500'000'000'000, 0);
    EXPECT_TRUE(legs.base.applies);
    EXPECT_TRUE(legs.base.is_buy);
    EXPECT_EQ(legs.base.qty_mojos, 2'000'000'000'000);
    EXPECT_EQ(legs.base.usd_pseudo_price, 1'500'000'000'000);

    EXPECT_TRUE(legs.quote.applies);
    EXPECT_FALSE(legs.quote.is_buy);
    EXPECT_EQ(legs.quote.qty_mojos, 3'000);
    EXPECT_EQ(legs.quote.usd_pseudo_price, 1'000'000'000'000);

    EXPECT_FALSE(legs.fee.applies);
    EXPECT_EQ(legs.fee.qty_mojos, 0);
}

// wmilliETH.b/XCH shape: a CAT base and an XCH quote.  5 units sold at
// 0.002 XCH each.  The XCH quote leg is booked -- State's XCH-quote
// exclusion is deliberately not copied.
TEST(MakerFillLegsTest, CatBaseXchQuotePairBooksTheXchLeg) {
    const auto legs = legs_for(pair_config(kWmilliEthB, kXch, kCatDenom,
                                           kXchDenom),
                               Side::Ask, 5'000, 2'000'000'000,
                               3'000'000'000, 0);
    EXPECT_TRUE(legs.base.applies);
    EXPECT_FALSE(legs.base.is_buy);
    EXPECT_EQ(legs.base.qty_mojos, 5'000);
    EXPECT_EQ(legs.base.usd_pseudo_price, 3'000'000'000);

    EXPECT_TRUE(legs.quote.applies);
    EXPECT_TRUE(legs.quote.is_buy);
    EXPECT_EQ(legs.quote.qty_mojos, 10'000'000'000);
    EXPECT_EQ(legs.quote.usd_pseudo_price, 1'500'000'000'000);
}

// No USD valuation for the base means none for the quote either, and never a
// placeholder: both quantities book, both prices stay 0.
TEST(MakerFillLegsTest, UnpricedFillBooksBothQuantitiesWithoutAPrice) {
    const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                               80'000'000'000'000, 0, 18'987);
    EXPECT_TRUE(legs.base.applies);
    EXPECT_FALSE(legs.base.is_buy);
    EXPECT_EQ(legs.base.qty_mojos, 1'000'000'000'000);
    EXPECT_EQ(legs.base.usd_pseudo_price, 0);

    EXPECT_TRUE(legs.quote.applies);
    EXPECT_TRUE(legs.quote.is_buy);
    EXPECT_EQ(legs.quote.qty_mojos, 80'000);
    EXPECT_EQ(legs.quote.usd_pseudo_price, 0);

    EXPECT_TRUE(legs.fee.applies);
    EXPECT_FALSE(legs.fee.is_buy);
    EXPECT_EQ(legs.fee.qty_mojos, 18'987);
    EXPECT_EQ(legs.fee.usd_pseudo_price, 0);

    const auto negative = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                                   80'000'000'000'000, -5, 18'987);
    EXPECT_EQ(negative.base.usd_pseudo_price, 0);
    EXPECT_EQ(negative.quote.usd_pseudo_price, 0);
}

// Both legs enter at equal USD COST.  This is about basis only -- equity
// marks holdings at live prices (see AskFillEquityMatchesPostTradeHoldings).
TEST(MakerFillLegsTest, BothLegsCarryEqualUsdCostBasisAtBooking) {
    struct Vector {
        xop::PairConfig pc{};
        Side side{Side::Ask};
        Mojo size{0};
        Mojo price{0};
        Mojo base_usd{0};
    };
    const std::array<Vector, 4> vectors{{
        {xch_dbx(), Side::Ask, 1'000'000'000'000, 80'000'000'000'000,
         1'600'000'000'000},
        {pair_config(kXch, kByc, kXchDenom, kCatDenom), Side::Bid,
         2'000'000'000'000, 1'500'000'000'000, 1'500'000'000'000},
        {pair_config(kWmilliEthB, kXch, kCatDenom, kXchDenom), Side::Ask,
         5'000, 2'000'000'000, 3'000'000'000},
        // Fill 14 of 2026-09-11, XCH/DBX at block 9276808.
        {xch_dbx(), Side::Ask, 1'000'000'000'000, 83'783'000'000'000,
         1'479'182'728'954},
    }};
    for (const auto& v : vectors) {
        const auto legs = legs_for(v.pc, v.side, v.size, v.price, v.base_usd, 0);
        EXPECT_TRUE(legs.quote.applies);
        const double base_value =
            (static_cast<double>(legs.base.qty_mojos)
             / static_cast<double>(v.pc.base_mojos_per_unit))
            * (static_cast<double>(legs.base.usd_pseudo_price) / 1e12);
        const double quote_value =
            (static_cast<double>(legs.quote.qty_mojos)
             / static_cast<double>(v.pc.quote_mojos_per_unit))
            * (static_cast<double>(legs.quote.usd_pseudo_price) / 1e12);
        EXPECT_LE(std::abs(base_value - quote_value), 1e-9 * base_value);
    }

    // Fill 14's implied DBX price: 1,479,182,728,954 * 1e12 / 83.783e12
    // = 17,654,926,762.64 -> 17,654,926,763.
    const auto live = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                               83'783'000'000'000, 1'479'182'728'954, 0);
    EXPECT_EQ(live.quote.usd_pseudo_price, 17'654'926'763);
}

TEST(MakerFillLegsTest, DegenerateInputsNeverBookAGarbageQuoteLeg) {
    const MakerFillApplyResult all_ok{};

    // (a) Nothing filled: no legs, nothing to reject.
    const auto a = legs_for(xch_dbx(), Side::Ask, 0, 80'000'000'000'000,
                            1'600'000'000'000, 0);
    EXPECT_FALSE(a.base.applies);
    EXPECT_FALSE(a.quote.applies);
    EXPECT_FALSE(a.quote_invalid);
    EXPECT_EQ(std::string(rejected_fill_legs(a, all_ok)), "");

    // (b) A zero price: the base still books, the quote cannot be derived.
    const auto b = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000, 0,
                            1'600'000'000'000, 0);
    EXPECT_TRUE(b.base.applies);
    EXPECT_FALSE(b.quote.applies);
    EXPECT_TRUE(b.quote_invalid);
    EXPECT_EQ(std::string(rejected_fill_legs(b, all_ok)), "quote");

    // (c) A zero quote denomination.
    const auto c = legs_for(pair_config(kXch, kDbx, kXchDenom, 0), Side::Ask,
                            1'000'000'000'000, 80'000'000'000'000,
                            1'600'000'000'000, 0);
    EXPECT_TRUE(c.quote_invalid);
    EXPECT_FALSE(c.quote.applies);
    EXPECT_EQ(std::string(rejected_fill_legs(c, all_ok)), "quote");

    // (d) 1 mojo of XCH at 80 DBX is 0.00008 DBX mojos: it rounds to 0, which
    // is no leg -- and not a failure.
    const auto d = legs_for(xch_dbx(), Side::Ask, 1, 80'000'000'000'000,
                            1'600'000'000'000, 0);
    EXPECT_FALSE(d.quote.applies);
    EXPECT_FALSE(d.quote_invalid);
    EXPECT_EQ(d.quote.qty_mojos, 0);
    EXPECT_EQ(std::string(rejected_fill_legs(d, all_ok)), "");

    // (e) A quote quantity of 1.6e34 mojos is outside Mojo: checked, never
    // passed to llround (whose result there is unspecified).
    const auto e = legs_for(pair_config(kWmilliEthB, kXch, kCatDenom,
                                        kXchDenom),
                            Side::Ask, Mojo{4'000'000'000'000'000'000},
                            Mojo{4'000'000'000'000'000'000},
                            1'600'000'000'000, 0);
    EXPECT_TRUE(e.quote_invalid);
    EXPECT_FALSE(e.quote.applies);
    EXPECT_TRUE(e.base.applies);
    EXPECT_EQ(e.quote.qty_mojos, 0);
    EXPECT_EQ(std::string(rejected_fill_legs(e, all_ok)), "quote");

    // (f) No fee, or a nonsensical negative one: no fee leg.
    const auto f0 = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                             80'000'000'000'000, 1'600'000'000'000, 0);
    EXPECT_FALSE(f0.fee.applies);
    const auto f1 = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                             80'000'000'000'000, 1'600'000'000'000, -1);
    EXPECT_FALSE(f1.fee.applies);
}

// ============================================================================
// The tracker: what the legs do to real InventoryTracker records
// ============================================================================

TEST_F(MakerFillInventoryTest, AskFillEquityMatchesPostTradeHoldings) {
    xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
    restore(inv, kXch, 25'000'000'000'000, 1'500'000'000'000.0);
    restore(inv, kDbx, 600'000, 18'000'000'000.0);

    const double before = equity_usd(inv, 1.60, 0.02);
    const double before_other_mark = equity_usd(inv, 1.60, 0.021);
    EXPECT_NEAR(before, 52.0, 1e-9);

    const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                               80'000'000'000'000, 1'600'000'000'000, 18'987);
    const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
    EXPECT_TRUE(r.base_ok);
    EXPECT_TRUE(r.quote_ok);
    EXPECT_TRUE(r.fee_ok);

    EXPECT_EQ(inv.net_inventory(kXch), 23'999'999'981'013);
    EXPECT_EQ(inv.net_inventory(kDbx), 680'000);
    // (600,000 * 1.8e10 + 80,000 * 2e10) / 680,000 = 18,235,294,117.6
    EXPECT_EQ(inv.get_record(kDbx).weighted_avg_cost_basis, 18'235'294'118);
    EXPECT_EQ(inv.get_record(kXch).weighted_avg_cost_basis, 1'500'000'000'000);

    // At marks equal to the fill's own ratio ($1.60 / $0.02 = 80) the only
    // equity change is the fee, 18,987 mojos x $1.60.  Base-only booking read
    // $50.40 here: -$1.60, -3.08%.
    const double after = equity_usd(inv, 1.60, 0.02);
    EXPECT_NEAR(after, 51.9999999696208, 1e-9);
    EXPECT_NEAR(before - after, 3.03792e-8, 1e-10);

    // At any OTHER mark the booked fill moves equity by its edge against the
    // marks, because equity reads holdings and never basis:
    //   80 DBX x $0.021 - 1 XCH x $1.60 - 18,987e-12 XCH x $1.60
    //   = +$0.0799999696208
    EXPECT_NEAR(equity_usd(inv, 1.60, 0.021) - before_other_mark,
                0.07999996962080047, 1e-9);
}

// The fourteen XCH/DBX asks of 2026-09-11 as trade_log recorded them (1 XCH
// each, fee 18,987 mojos), with the USD price the engine would have given each
// base leg.  The STARTING BOOK IS SYNTHETIC.  DBX 601,057 mojos is the
// tracker's pre-fill quantity carried over from the prior investigation
// (inventory_state now holds 1,740,296: the 1,137,997 of quote legs plus
// 1,242 of non-trade flows).  XCH is sized so that base-only booking of all
// fourteen lands near the ratio the snapshots showed; it is about 1 XCH above
// the real book, and the snapshot at the breach block reflected thirteen
// booked asks, not fourteen.  The literals therefore pin the ARITHMETIC of a
// fourteen-ask replay, not the incident's exact figures.
TEST_F(MakerFillInventoryTest, Replay20260911FourteenDbxAsks) {
    struct ReplayFill {
        Mojo price{0};
        Mojo base_usd{0};
    };
    constexpr std::array<ReplayFill, 14> kFills{{
        {79'760'000'000'000, 1'408'156'958'588},
        {79'920'000'000'000, 1'410'981'746'870},
        {79'773'000'000'000, 1'408'386'472'636},
        {79'840'000'000'000, 1'409'569'352'729},
        {80'559'000'000'000, 1'422'263'245'071},
        {80'694'000'000'000, 1'424'646'660'184},
        {81'692'000'000'000, 1'442'266'277'093},
        {81'692'000'000'000, 1'442'266'277'093},
        {81'692'000'000'000, 1'442'266'277'093},
        {81'692'000'000'000, 1'442'266'277'093},
        {81'692'000'000'000, 1'442'266'277'093},
        {81'791'000'000'000, 1'444'014'114'843},
        {83'417'000'000'000, 1'472'721'025'759},
        {83'783'000'000'000, 1'479'182'728'954},
    }};
    // Marks at block 9276808: snapshots.xch_usd_rate 1.47, and DBX at
    // 1.47 / 83.262877255953 (that block's XCH/DBX mid).
    constexpr double kXchUsd = 1.47;
    constexpr double kDbxUsd = 0.017654926762633586;

    xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
    restore(inv, kXch, 38'640'000'000'000, 1'450'000'000'000.0);
    restore(inv, kDbx, 601'057, 17'500'000'000.0);
    const double before = equity_usd(inv, kXchUsd, kDbxUsd);

    for (const auto& fill : kFills) {
        const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                                   fill.price, fill.base_usd, 18'987);
        const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
        EXPECT_TRUE(r.base_ok);
        EXPECT_TRUE(r.quote_ok);
        EXPECT_TRUE(r.fee_ok);
    }

    EXPECT_EQ(inv.net_inventory(kXch), 24'639'999'734'182);
    EXPECT_EQ(inv.net_inventory(kDbx), 1'739'054);
    EXPECT_NEAR(static_cast<double>(inv.get_record(kDbx).weighted_avg_cost_basis),
                17'601'380'516.0, 2.0);
    EXPECT_NEAR(static_cast<double>(inv.get_record(kXch).weighted_avg_cost_basis),
                1'450'000'000'000.0, 2.0);

    // Base-only booking (the shipped defect: no quote legs, no fees) leaves
    // DBX at 601,057 and reads 0.773412992035939 here.
    EXPECT_NEAR(inv.inventory_ratio(kXch, kDbx, 83'262'877'255'953, kXchDenom,
                                    kCatDenom),
                0.5412255376328949, 1e-9);

    // Base-only booking moved equity by -$20.58 at this mark.  With every leg
    // booked the move is the fourteen fills' edge against the mark.
    EXPECT_NEAR(equity_usd(inv, kXchUsd, kDbxUsd) - before,
                -0.48874669965573503, 1e-6);
}

TEST_F(MakerFillInventoryTest, CatXchAskCreditsTheXchQuoteLeg) {
    xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
    restore(inv, kWmilliEthB, 20'000, 3'000'000'000.0);
    restore(inv, kXch, 10'000'000'000'000, 1'500'000'000'000.0);

    const auto pc = pair_config(kWmilliEthB, kXch, kCatDenom, kXchDenom);
    const auto legs = legs_for(pc, Side::Ask, 5'000, 2'000'000'000,
                               3'000'000'000, 18'987);
    const auto r = apply_maker_fill_legs(inv, pc, legs, kBlock, now_);
    EXPECT_TRUE(r.base_ok);
    EXPECT_TRUE(r.quote_ok);
    EXPECT_TRUE(r.fee_ok);

    EXPECT_EQ(inv.net_inventory(kWmilliEthB), 15'000);
    EXPECT_EQ(inv.get_record(kWmilliEthB).weighted_avg_cost_basis,
              3'000'000'000);
    // +10 XCH of proceeds, -18,987 mojos of fee.
    EXPECT_EQ(inv.net_inventory(kXch), 10'009'999'981'013);
    EXPECT_EQ(inv.get_record(kXch).weighted_avg_cost_basis, 1'500'000'000'000);
}

// A bid spends DBX at an implied $0.02, below its $0.025 basis.  The tracker
// enforces never-sell-at-loss, and the quote sale must book anyway: the fill
// already happened.
TEST_F(MakerFillInventoryTest, BidQuoteLegBypassesNoLossBelowBasis) {
    xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
    ASSERT_TRUE(inv.no_loss_constraint_enabled());
    restore(inv, kXch, 10'000'000'000'000, 1'500'000'000'000.0);
    restore(inv, kDbx, 600'000, 25'000'000'000.0);

    const auto legs = legs_for(xch_dbx(), Side::Bid, 1'000'000'000'000,
                               80'000'000'000'000, 1'600'000'000'000, 0);
    // The scenario really is a sale below basis.
    EXPECT_LT(legs.quote.usd_pseudo_price,
              inv.get_record(kDbx).weighted_avg_cost_basis);

    const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
    EXPECT_TRUE(r.quote_ok);
    EXPECT_EQ(inv.net_inventory(kDbx), 520'000);
    EXPECT_EQ(inv.get_record(kDbx).weighted_avg_cost_basis, 25'000'000'000);
    EXPECT_EQ(inv.net_inventory(kXch), 11'000'000'000'000);
    // (10 * 1.5e12 + 1 * 1.6e12) / 11 = 1,509,090,909,090.9
    EXPECT_EQ(inv.get_record(kXch).weighted_avg_cost_basis, 1'509'090'909'091);
}

// A priced ask on a wallet-seeded (sentinel) quote record replaces the whole
// holding's basis with the implied price and clears the flag.
TEST_F(MakerFillInventoryTest, PricedAskReplacesQuoteSentinelBasis) {
    xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
    inv.seed_position(kDbx, 600'000, Mojo{1});
    ASSERT_TRUE(inv.get_record(kDbx).basis_is_seed_sentinel);
    restore(inv, kXch, 25'000'000'000'000, 1'500'000'000'000.0);

    const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                               80'000'000'000'000, 1'600'000'000'000, 18'987,
                               /*trusted=*/true);
    const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
    EXPECT_TRUE(r.quote_ok);

    const auto dbx = inv.get_record(kDbx);
    EXPECT_EQ(dbx.total_quantity, 680'000);
    EXPECT_EQ(dbx.weighted_avg_cost_basis, 20'000'000'000);
    EXPECT_FALSE(dbx.basis_is_seed_sentinel);
}

TEST_F(MakerFillInventoryTest, SentinelQuoteStaysRepairableWhenUnpricedOrSold) {
    // (a) An UNPRICED ask on a sentinel quote record keeps basis and flag, so
    // Step 11's mark-at-first-observation can still repair the whole holding.
    {
        xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
        inv.seed_position(kDbx, 600'000, Mojo{1});
        restore(inv, kXch, 25'000'000'000'000, 1'500'000'000'000.0);

        const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                                   80'000'000'000'000, /*base_usd=*/0, 18'987);
        const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
        EXPECT_TRUE(r.quote_ok);

        auto dbx = inv.get_record(kDbx);
        EXPECT_EQ(dbx.total_quantity, 680'000);
        EXPECT_EQ(dbx.weighted_avg_cost_basis, 1);
        EXPECT_TRUE(dbx.basis_is_seed_sentinel);

        inv.reseed_basis(kDbx, 20'000'000'000);
        dbx = inv.get_record(kDbx);
        EXPECT_EQ(dbx.weighted_avg_cost_basis, 20'000'000'000);
        EXPECT_FALSE(dbx.basis_is_seed_sentinel);
    }
    // (b) A priced BID spends from the sentinel record: a sale never touches
    // the flag, so the remainder stays repairable.
    {
        xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
        inv.seed_position(kDbx, 600'000, Mojo{1});

        const auto legs = legs_for(xch_dbx(), Side::Bid, 1'000'000'000'000,
                                   80'000'000'000'000, 1'600'000'000'000, 0);
        const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
        EXPECT_TRUE(r.quote_ok);

        const auto dbx = inv.get_record(kDbx);
        EXPECT_EQ(dbx.total_quantity, 520'000);
        EXPECT_EQ(dbx.weighted_avg_cost_basis, 1);
        EXPECT_TRUE(dbx.basis_is_seed_sentinel);
    }
}

TEST_F(MakerFillInventoryTest, LegFailuresAreReportedPerLeg) {
    // (i) A bid spends 80 DBX with 50 tracked: the quote leg is refused and
    // reported; the base and fee legs still book.
    {
        xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
        restore(inv, kXch, 10'000'000'000'000, 1'500'000'000'000.0);
        restore(inv, kDbx, 50'000, 20'000'000'000.0);

        const auto legs = legs_for(xch_dbx(), Side::Bid, 1'000'000'000'000,
                                   80'000'000'000'000, 1'600'000'000'000,
                                   18'987);
        const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
        EXPECT_TRUE(r.base_ok);
        EXPECT_FALSE(r.quote_ok);
        EXPECT_TRUE(r.fee_ok);
        EXPECT_EQ(inv.net_inventory(kDbx), 50'000);
        EXPECT_EQ(inv.net_inventory(kXch), 10'999'999'981'013);
        EXPECT_EQ(std::string(rejected_fill_legs(legs, r)), "quote");
        EXPECT_EQ(std::string(rejected_fill_legs(
                      legs, MakerFillApplyResult{false, false, true})),
                  "base+quote");
    }
    // (ii) A CAT/CAT bid with no XCH tracked: only the fee leg is refused,
    // and a refused fee is never a rejection.
    {
        xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
        restore(inv, kByc, 100'000, 1'000'000'000'000.0);
        restore(inv, kWusdc, 50'000, 1'000'000'000'000.0);

        const auto pc = pair_config(kByc, kWusdc, kCatDenom, kCatDenom);
        const auto legs = legs_for(pc, Side::Bid, 10'000, 1'001'000'000'000,
                                   1'001'000'000'000, 5'000);
        const auto r = apply_maker_fill_legs(inv, pc, legs, kBlock, now_);
        EXPECT_TRUE(r.base_ok);
        EXPECT_TRUE(r.quote_ok);
        EXPECT_FALSE(r.fee_ok);
        EXPECT_EQ(inv.net_inventory(kByc), 110'000);
        EXPECT_EQ(inv.net_inventory(kWusdc), 39'990);
        EXPECT_EQ(inv.net_inventory(kXch), 0);
        EXPECT_EQ(std::string(rejected_fill_legs(legs, r)), "");
    }
    // (iii) An ask of 1 XCH with 0.5 tracked: the base leg is refused and
    // reported; the proceeds still book.
    {
        xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
        restore(inv, kXch, 500'000'000'000, 1'500'000'000'000.0);
        restore(inv, kDbx, 600'000, 20'000'000'000.0);

        const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                                   80'000'000'000'000, 1'600'000'000'000, 0);
        const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
        EXPECT_FALSE(r.base_ok);
        EXPECT_TRUE(r.quote_ok);
        EXPECT_TRUE(r.fee_ok);
        EXPECT_EQ(inv.net_inventory(kXch), 500'000'000'000);
        EXPECT_EQ(inv.net_inventory(kDbx), 680'000);
        EXPECT_EQ(std::string(rejected_fill_legs(legs, r)), "base");

        // An underivable quote beside a refused base names both.
        const auto invalid = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                                      0, 1'600'000'000'000, 0);
        EXPECT_EQ(std::string(rejected_fill_legs(
                      invalid, MakerFillApplyResult{false, true, true})),
                  "base+quote");
    }
}

// When the pair's quote-USD factor is not trusted, the quote leg books
// UNPRICED: a sentinel quote record keeps its flag for Step 11 to repair from
// a graded mark, and a real basis is left intact.  The base leg is unaffected.
TEST_F(MakerFillInventoryTest, UntrustedQuoteFactorLeavesQuoteBasisRepairable) {
    // (a) Sentinel quote record.
    {
        xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
        inv.seed_position(kDbx, 600'000, Mojo{1});
        restore(inv, kXch, 25'000'000'000'000, 1'500'000'000'000.0);

        const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                                   80'000'000'000'000, 1'600'000'000'000,
                                   18'987, /*trusted=*/false);
        EXPECT_EQ(legs.base.usd_pseudo_price, 1'600'000'000'000);
        EXPECT_TRUE(legs.quote.applies);
        EXPECT_EQ(legs.quote.qty_mojos, 80'000);
        EXPECT_EQ(legs.quote.usd_pseudo_price, 0);

        const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
        EXPECT_TRUE(r.base_ok);
        EXPECT_TRUE(r.quote_ok);
        EXPECT_TRUE(r.fee_ok);

        auto dbx = inv.get_record(kDbx);
        EXPECT_EQ(dbx.total_quantity, 680'000);
        EXPECT_EQ(dbx.weighted_avg_cost_basis, 1);
        EXPECT_TRUE(dbx.basis_is_seed_sentinel);
        EXPECT_EQ(inv.net_inventory(kXch), 23'999'999'981'013);
        EXPECT_EQ(inv.get_record(kXch).weighted_avg_cost_basis,
                  1'500'000'000'000);

        inv.reseed_basis(kDbx, 20'000'000'000);
        dbx = inv.get_record(kDbx);
        EXPECT_EQ(dbx.weighted_avg_cost_basis, 20'000'000'000);
        EXPECT_FALSE(dbx.basis_is_seed_sentinel);
    }
    // (b) A real quote basis: the lot is costed at it, so it does not move.
    {
        xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
        restore(inv, kDbx, 600'000, 18'000'000'000.0);
        restore(inv, kXch, 25'000'000'000'000, 1'500'000'000'000.0);

        const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                                   80'000'000'000'000, 1'600'000'000'000,
                                   18'987, /*trusted=*/false);
        const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
        EXPECT_TRUE(r.quote_ok);

        const auto dbx = inv.get_record(kDbx);
        EXPECT_EQ(dbx.total_quantity, 680'000);
        EXPECT_EQ(dbx.weighted_avg_cost_basis, 18'000'000'000);
        EXPECT_FALSE(dbx.basis_is_seed_sentinel);
    }
}

// A refused PRICED buy must reach the result flags.  record_buy used to return
// void, so apply_fill_leg reported success for a buy the tracker had refused
// and the engine neither logged nor alerted.  The only refusal a priced buy
// leg can meet is an overflow of the holding: legs never carry a quantity or a
// price <= 0.  Neither test has a fee leg, so the refused holding can move
// only through the refused leg.
TEST_F(MakerFillInventoryTest, PricedBidBaseLegOverflowIsRejectedNotSwallowed) {
    constexpr Mojo kNearMax = std::numeric_limits<Mojo>::max() - 1;
    xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
    restore(inv, kXch, kNearMax, 1'500'000'000'000.0);
    restore(inv, kDbx, 600'000, 18'000'000'000.0);

    const auto legs = legs_for(xch_dbx(), Side::Bid, 1'000'000'000'000,
                               80'000'000'000'000, 1'600'000'000'000,
                               /*fee=*/0);
    ASSERT_TRUE(legs.base.applies);
    ASSERT_TRUE(legs.base.is_buy);
    ASSERT_GT(legs.base.usd_pseudo_price, 0);  // priced: record_buy

    const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
    EXPECT_FALSE(r.base_ok);
    EXPECT_TRUE(r.quote_ok);
    EXPECT_TRUE(r.fee_ok);
    EXPECT_EQ(std::string(rejected_fill_legs(legs, r)), "base");

    EXPECT_EQ(inv.net_inventory(kXch), kNearMax);
    EXPECT_EQ(inv.get_record(kXch).weighted_avg_cost_basis, 1'500'000'000'000);
    // The refusal does not stop the next leg: the 80 DBX spend still books.
    EXPECT_EQ(inv.net_inventory(kDbx), 520'000);
}

TEST_F(MakerFillInventoryTest, PricedAskQuoteLegOverflowIsRejectedNotSwallowed) {
    constexpr Mojo kNearMax = std::numeric_limits<Mojo>::max() - 1;
    xop::InventoryTracker inv(risk_cfg_, 0, /*no_loss_constraint=*/true);
    restore(inv, kXch, 25'000'000'000'000, 1'500'000'000'000.0);
    restore(inv, kDbx, kNearMax, 18'000'000'000.0);

    const auto legs = legs_for(xch_dbx(), Side::Ask, 1'000'000'000'000,
                               80'000'000'000'000, 1'600'000'000'000,
                               /*fee=*/0, /*trusted=*/true);
    ASSERT_TRUE(legs.quote.applies);
    ASSERT_TRUE(legs.quote.is_buy);
    ASSERT_GT(legs.quote.usd_pseudo_price, 0);  // priced: record_buy

    const auto r = apply_maker_fill_legs(inv, xch_dbx(), legs, kBlock, now_);
    EXPECT_TRUE(r.base_ok);
    EXPECT_FALSE(r.quote_ok);
    EXPECT_TRUE(r.fee_ok);
    EXPECT_EQ(std::string(rejected_fill_legs(legs, r)), "quote");

    EXPECT_EQ(inv.net_inventory(kDbx), kNearMax);
    EXPECT_EQ(inv.get_record(kDbx).weighted_avg_cost_basis, 18'000'000'000);
    // The base sale still books.
    EXPECT_EQ(inv.net_inventory(kXch), 24'000'000'000'000);
}

}  // namespace
