// ---------------------------------------------------------------------------
// [PARANCHOR] A declared peg as a fair-value anchor -- universal over
// assets and currencies, gated by suspension, and never inventing a
// missing FX rate.  The BYC blindness of 2026-08-30 is the USD case; the
// EUR and JPY cases pin that nothing here quietly assumes dollars.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/execution/par_anchor.hpp"

using xop::PegRegistry;
using xop::PeggedAsset;
using xop::par_anchor;

namespace {

constexpr double kWrapperSigma = 100.0;
constexpr double kMarketSigma  = 150.0;

PegRegistry with(PeggedAsset a) {
    PegRegistry reg;
    EXPECT_TRUE(reg.add(std::move(a)));
    return reg;
}

PeggedAsset byc() {
    PeggedAsset a;
    a.asset_id = "ae15";
    a.symbol = "BYC";
    a.peg_currency = "USD";
    a.peg_target = 1.0;
    a.prefer_market_cross = true;
    return a;
}

PeggedAsset eur_wrapper() {
    PeggedAsset a;
    a.asset_id = "ee01";
    a.symbol = "wEURC";
    a.peg_currency = "EUR";
    a.peg_target = 1.0;
    a.prefer_market_cross = false;
    return a;
}

}  // namespace

TEST(ParAnchor, a_usd_peg_anchors_at_target_with_no_fx_needed)
{
    const auto reg = with(byc());
    const auto a = par_anchor(reg, "ae15", "byc", /*suspended=*/false,
                              std::nullopt, kWrapperSigma, kMarketSigma);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->asset, "byc");
    EXPECT_DOUBLE_EQ(a->usd_price, 1.0);
}

TEST(ParAnchor, sigma_follows_the_declarations_kind)
{
    // BYC is market-determined -> the wider bar.
    const auto market = par_anchor(with(byc()), "ae15", "byc", false,
                                   std::nullopt, kWrapperSigma,
                                   kMarketSigma);
    ASSERT_TRUE(market.has_value());
    EXPECT_DOUBLE_EQ(market->sigma_bps, kMarketSigma);

    // A wrapper is par by construction -> the tighter bar.
    auto wrapper_decl = byc();
    wrapper_decl.prefer_market_cross = false;
    const auto wrapper = par_anchor(with(wrapper_decl), "ae15", "byc",
                                    false, std::nullopt, kWrapperSigma,
                                    kMarketSigma);
    ASSERT_TRUE(wrapper.has_value());
    EXPECT_DOUBLE_EQ(wrapper->sigma_bps, kWrapperSigma);
}

TEST(ParAnchor, a_suspended_par_contributes_nothing)
{
    // wUSDC.b's lesson: the declaration survives a depeg; the anchor must
    // not.
    const auto a = par_anchor(with(byc()), "ae15", "byc",
                              /*suspended=*/true, std::nullopt,
                              kWrapperSigma, kMarketSigma);
    EXPECT_FALSE(a.has_value());
}

TEST(ParAnchor, an_unenforced_or_undeclared_peg_is_no_anchor)
{
    auto wound_down = byc();
    wound_down.enforce = false;
    EXPECT_FALSE(par_anchor(with(wound_down), "ae15", "byc", false,
                            std::nullopt, kWrapperSigma, kMarketSigma)
                     .has_value());

    EXPECT_FALSE(par_anchor(PegRegistry{}, "ae15", "byc", false,
                            std::nullopt, kWrapperSigma, kMarketSigma)
                     .has_value());
}

TEST(ParAnchor, a_eur_peg_needs_the_fx_rate_and_uses_it)
{
    const auto reg = with(eur_wrapper());

    // No EURUSD -> no anchor.  NEVER a silent 1:1.
    EXPECT_FALSE(par_anchor(reg, "ee01", "weurc", false, std::nullopt,
                            kWrapperSigma, kMarketSigma)
                     .has_value());

    // With the rate, the par converts.
    const auto a = par_anchor(reg, "ee01", "weurc", false, 1.09,
                              kWrapperSigma, kMarketSigma);
    ASSERT_TRUE(a.has_value());
    EXPECT_DOUBLE_EQ(a->usd_price, 1.09);
}

TEST(ParAnchor, a_100_jpy_peg_is_expressible_and_converts)
{
    PeggedAsset a;
    a.asset_id = "aa77";
    a.symbol = "wJPYx";
    a.peg_currency = "JPY";
    a.peg_target = 100.0;                     // pegged to ONE HUNDRED yen
    const auto anch = par_anchor(with(a), "aa77", "wjpyx", false,
                                 /*usd per yen*/ 0.0067, kWrapperSigma,
                                 kMarketSigma);
    ASSERT_TRUE(anch.has_value());
    EXPECT_NEAR(anch->usd_price, 0.67, 1e-12);
}

TEST(ParAnchor, junk_fx_rates_are_refused)
{
    const auto reg = with(eur_wrapper());
    for (const double junk : {0.0, -1.09,
                              std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_FALSE(par_anchor(reg, "ee01", "weurc", false, junk,
                                kWrapperSigma, kMarketSigma)
                         .has_value())
            << "fx=" << junk;
    }
}

TEST(ParAnchor, a_nonpositive_sigma_is_refused_not_propagated)
{
    // The solver treats sigma <= 0 as unusable; producing such an anchor
    // would silently drop it there.  Refuse HERE, where the config error
    // is attributable.
    EXPECT_FALSE(par_anchor(with(byc()), "ae15", "byc", false,
                            std::nullopt, kWrapperSigma, 0.0)
                     .has_value());
    EXPECT_FALSE(par_anchor(with(byc()), "ae15", "byc", false,
                            std::nullopt, kWrapperSigma, -5.0)
                     .has_value());
}

TEST(ParAnchorConsensus, agreeing_siblings_anchor_at_the_widest_sigma)
{
    // wUSDC.b and wUSDC: one graph node, two declarations.  Both healthy
    // and agreeing -> anchored, but at the LESS confident bar.
    PegRegistry reg;
    auto wrapper = byc();
    wrapper.asset_id = "fa4a";
    wrapper.prefer_market_cross = false;   // 100bps
    ASSERT_TRUE(reg.add(wrapper));
    auto cdp = byc();
    cdp.asset_id = "bbb5";                 // market-determined: 150bps
    ASSERT_TRUE(reg.add(cdp));

    const auto a = xop::par_anchor_consensus(
        reg, "wusdc",
        {xop::ParLegInput{"fa4a"}, xop::ParLegInput{"bbb5"}},
        kWrapperSigma, kMarketSigma);
    ASSERT_TRUE(a.has_value());
    EXPECT_DOUBLE_EQ(a->sigma_bps, kMarketSigma);
}

TEST(ParAnchorConsensus, one_suspended_sibling_kills_the_node)
{
    // The review's bypass: wUSDC.b suspended mid-depeg, wUSDC healthy,
    // one shared leg.  The healthy sibling must NOT anchor the node.
    PegRegistry reg;
    auto broken = byc();
    broken.asset_id = "fa4a";
    ASSERT_TRUE(reg.add(broken));
    auto healthy = byc();
    healthy.asset_id = "bbb5";
    ASSERT_TRUE(reg.add(healthy));

    xop::ParLegInput suspended{"fa4a"};
    suspended.suspended = true;
    EXPECT_FALSE(xop::par_anchor_consensus(
                     reg, "wusdc", {suspended, xop::ParLegInput{"bbb5"}},
                     kWrapperSigma, kMarketSigma)
                     .has_value());
    // Order must not matter.
    EXPECT_FALSE(xop::par_anchor_consensus(
                     reg, "wusdc", {xop::ParLegInput{"bbb5"}, suspended},
                     kWrapperSigma, kMarketSigma)
                     .has_value());
}

TEST(ParAnchorConsensus, an_undeclared_sibling_kills_the_node)
{
    PegRegistry reg;
    ASSERT_TRUE(reg.add(byc()));   // "ae15" declared; "zz99" is not
    EXPECT_FALSE(xop::par_anchor_consensus(
                     reg, "byc",
                     {xop::ParLegInput{"ae15"}, xop::ParLegInput{"zz99"}},
                     kWrapperSigma, kMarketSigma)
                     .has_value());
}

TEST(ParAnchorConsensus, disagreeing_pars_are_refused)
{
    PegRegistry reg;
    auto one = byc();
    one.asset_id = "aa11";
    ASSERT_TRUE(reg.add(one));
    auto two = byc();
    two.asset_id = "bb22";
    two.peg_target = 1.05;         // a DIFFERENT declared par
    ASSERT_TRUE(reg.add(two));
    EXPECT_FALSE(xop::par_anchor_consensus(
                     reg, "byc",
                     {xop::ParLegInput{"aa11"}, xop::ParLegInput{"bb22"}},
                     kWrapperSigma, kMarketSigma)
                     .has_value());
}

TEST(ParAnchorConsensus, an_empty_input_set_is_no_anchor)
{
    EXPECT_FALSE(xop::par_anchor_consensus(with(byc()), "byc", {},
                                           kWrapperSigma, kMarketSigma)
                     .has_value());
}

TEST(ParAnchor, the_anchor_is_named_for_the_graph_leg_not_the_asset_id)
{
    const auto a = par_anchor(with(byc()), "ae15", "byc", false,
                              std::nullopt, kWrapperSigma, kMarketSigma);
    ASSERT_TRUE(a.has_value());
    // The solver joins anchors to edges by LEG symbol; an anchor named by
    // asset id would never join anything and read as "anchored" in logs.
    EXPECT_EQ(a->asset, "byc");
    EXPECT_NE(a->asset, "ae15");
}
