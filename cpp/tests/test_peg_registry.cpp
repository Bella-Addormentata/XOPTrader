// test_peg_registry.cpp -- [PEG 2026-08-26]
//
// The registry decides what a pegged asset is worth, so these pin the
// answers that would otherwise be wrong silently: a missing FX rate, a
// missing observation, and a peg that is declared but no longer enforced.

#include <gtest/gtest.h>

#include <limits>

#include <xop/peg_registry.hpp>

using xop::PeggedAsset;
using xop::PegRegistry;
using xop::PegStatus;

namespace {

PeggedAsset usd_coin(std::string id = "wusdc_tail", std::string sym = "wUSDC.b") {
    PeggedAsset a;
    a.asset_id = std::move(id);
    a.symbol = std::move(sym);
    a.peg_currency = "USD";
    a.peg_target = 1.0;
    a.warn_pct = 2.0;
    a.bail_pct = 10.0;
    return a;
}

PeggedAsset euro_coin() {
    PeggedAsset a;
    a.asset_id = "eur_tail";
    a.symbol = "wEURC";
    a.peg_currency = "EUR";
    a.peg_target = 1.0;
    return a;
}

}  // namespace

// ---------------------------------------------------------------------------
// Declaration hygiene
// ---------------------------------------------------------------------------

TEST(PegRegistry, RejectsIncoherentDeclarationsRatherThanStoringThem) {
    PegRegistry reg;

    PeggedAsset no_id = usd_coin();
    no_id.asset_id.clear();
    EXPECT_FALSE(reg.add(no_id)) << "an asset with no id cannot be looked up";

    PeggedAsset no_currency = usd_coin();
    no_currency.peg_currency.clear();
    EXPECT_FALSE(reg.add(no_currency)) << "pegged to WHAT is the whole point";

    PeggedAsset zero_target = usd_coin();
    zero_target.peg_target = 0.0;
    EXPECT_FALSE(reg.add(zero_target));

    // bail must exceed warn, or the warning can never fire first and the
    // operator goes straight from "fine" to "broken" with no notice.
    PeggedAsset inverted = usd_coin();
    inverted.warn_pct = 10.0;
    inverted.bail_pct = 2.0;
    EXPECT_FALSE(reg.add(inverted));

    EXPECT_TRUE(reg.empty()) << "a rejected declaration must not be stored";
}

TEST(PegRegistry, LooksUpByAssetIdNotSymbol) {
    // Symbols collide and get reused -- CoinMarketCap serves two coins
    // called XCH, one of them dead since 2016.  Valuation must key on the
    // identity that cannot be renamed.
    PegRegistry reg({usd_coin("tail_a", "SAME"), usd_coin("tail_b", "SAME")});
    EXPECT_EQ(reg.size(), 2u);
    EXPECT_NE(reg.find("tail_a"), nullptr);
    EXPECT_NE(reg.find("tail_b"), nullptr);
    EXPECT_EQ(reg.find("SAME"), nullptr) << "symbol is not a valuation key";
}

// ---------------------------------------------------------------------------
// USD par value
// ---------------------------------------------------------------------------

TEST(PegRegistry, UsdPeggedAssetNeedsNoFxRate) {
    PegRegistry reg({usd_coin()});
    auto v = reg.usd_par_value("wusdc_tail");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1.0);
}

TEST(PegRegistry, NonUsdPegConvertsThroughTheSuppliedRate) {
    PegRegistry reg({euro_coin()});
    auto v = reg.usd_par_value("eur_tail", 1.09);
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1.09);
}

TEST(PegRegistry, NonUsdPegWithoutAnFxRateIsUnvalued) {
    // THE point of the currency field.  Before it, a EUR-pegged coin would
    // have been marked at $1.00 because "pegged" was hardcoded to mean
    // "a dollar".  A missing rate must produce no valuation, never a 1:1
    // substitution.
    PegRegistry reg({euro_coin()});
    EXPECT_FALSE(reg.usd_par_value("eur_tail").has_value());
    EXPECT_FALSE(reg.usd_par_value("eur_tail", 0.0).has_value());
    EXPECT_FALSE(reg.usd_par_value("eur_tail", -1.0).has_value());
}

TEST(PegRegistry, NonUnitTargetScales) {
    PeggedAsset yen100;
    yen100.asset_id = "jpy_tail";
    yen100.peg_currency = "JPY";
    yen100.peg_target = 100.0;
    PegRegistry reg({yen100});
    auto v = reg.usd_par_value("jpy_tail", 0.0064);  // JPY->USD
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 0.64);
}

TEST(PegRegistry, UndeclaredAssetIsUnvalued) {
    PegRegistry reg({usd_coin()});
    EXPECT_FALSE(reg.usd_par_value("something_else").has_value());
}

TEST(PegRegistry, AnUnenforcedPegDoesNotMarkTheBookAtPar) {
    // The BYC case: the issuer announced the protocol was being sunset, so
    // the declaration should stay on record for provenance while ceasing
    // to value anything.  Under the old hardcoded branch this asset kept
    // returning 1.0 and marked the whole portfolio.
    PeggedAsset winding_down = usd_coin("byc_tail", "BYC");
    winding_down.enforce = false;
    PegRegistry reg({winding_down});

    EXPECT_NE(reg.find("byc_tail"), nullptr) << "declaration is retained";
    EXPECT_FALSE(reg.is_pegged("byc_tail"));
    EXPECT_FALSE(reg.usd_par_value("byc_tail").has_value());
    EXPECT_EQ(reg.classify("byc_tail", 1.0), PegStatus::NotPegged);
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

TEST(PegRegistry, ClassifiesAgainstTheDeclaredThresholds) {
    PegRegistry reg({usd_coin()});
    EXPECT_EQ(reg.classify("wusdc_tail", 1.00), PegStatus::Holding);
    EXPECT_EQ(reg.classify("wusdc_tail", 1.01), PegStatus::Holding);
    EXPECT_EQ(reg.classify("wusdc_tail", 1.05), PegStatus::Warn);
    EXPECT_EQ(reg.classify("wusdc_tail", 0.95), PegStatus::Warn);
    EXPECT_EQ(reg.classify("wusdc_tail", 0.74), PegStatus::Broken);
}

TEST(PegRegistry, DeviationIsSignedSoBelowPegIsDistinguishable) {
    PegRegistry reg({usd_coin()});
    auto below = reg.deviation_pct("wusdc_tail", 0.74);
    ASSERT_TRUE(below.has_value());
    EXPECT_LT(*below, 0.0) << "trading below par must be visibly negative";
    EXPECT_NEAR(*below, -26.0, 0.001);

    auto above = reg.deviation_pct("wusdc_tail", 1.26);
    ASSERT_TRUE(above.has_value());
    EXPECT_GT(*above, 0.0);
}

TEST(PegRegistry, MissingObservationIsUnobservedNotHolding) {
    // The failure of 2026-08-25 in one assertion.  Disabling the wUSDC.b
    // pairs stopped the ticker ingest that fed the peg monitor, and 144
    // readings became 0.  If absence had been reported as health, nothing
    // would ever have said so.
    PegRegistry reg({usd_coin()});
    EXPECT_EQ(reg.classify("wusdc_tail", std::nullopt), PegStatus::Unobserved);
    EXPECT_EQ(reg.classify("wusdc_tail", 0.0), PegStatus::Unobserved);
    EXPECT_NE(reg.classify("wusdc_tail", std::nullopt), PegStatus::Holding);
}

TEST(PegRegistry, UndeclaredAssetClassifiesAsNotPeggedNotUnobserved) {
    // An asset nobody claimed is pegged is not a monitoring gap.
    PegRegistry reg({usd_coin()});
    EXPECT_EQ(reg.classify("xch", 1.42), PegStatus::NotPegged);
    EXPECT_EQ(reg.classify("xch", std::nullopt), PegStatus::NotPegged);
}

TEST(PegRegistry, EachBandIsEnteredOnceItsThresholdIsPassed) {
    // Bands, not exact boundaries: 1.02 - 1.0 is 0.020000000000000018 in
    // double, so "exactly 2%" is not a state the arithmetic can represent
    // and pinning it would pin the representation error.  What IS pinned
    // (below) is that the comparison is INCLUSIVE, matching DepegDetector.
    PeggedAsset a = usd_coin();
    a.warn_pct = 2.0;
    a.bail_pct = 10.0;
    PegRegistry reg({a});

    EXPECT_EQ(reg.classify("wusdc_tail", 1.015), PegStatus::Holding);
    EXPECT_EQ(reg.classify("wusdc_tail", 0.985), PegStatus::Holding);

    EXPECT_EQ(reg.classify("wusdc_tail", 1.03), PegStatus::Warn);
    EXPECT_EQ(reg.classify("wusdc_tail", 0.97), PegStatus::Warn);

    EXPECT_EQ(reg.classify("wusdc_tail", 1.15), PegStatus::Broken);
    EXPECT_EQ(reg.classify("wusdc_tail", 0.85), PegStatus::Broken);
}

TEST(PegRegistry, BandsAreSymmetricAboveAndBelowPar) {
    // A peg can break in either direction and both must be caught.  An
    // implementation that forgot the absolute value would pass every
    // above-par case and silently ignore the below-par ones -- which is
    // the direction that actually happened.
    PegRegistry reg({usd_coin()});
    for (double delta : {0.03, 0.05, 0.09}) {
        EXPECT_EQ(reg.classify("wusdc_tail", 1.0 + delta),
                  reg.classify("wusdc_tail", 1.0 - delta))
            << "asymmetric handling at delta " << delta;
    }
}

// ---------------------------------------------------------------------------
// The live numbers, as a regression
// ---------------------------------------------------------------------------

TEST(PegRegistry, TheLiveWusdcReadingClassifiesAsBroken) {
    // Implied wUSDC.b sat at $0.737-$0.746 for hours on 2026-08-25 while
    // accounting valued it at exactly $1.00.  With a declaration in place
    // that reading is Broken, not a curiosity in a log line.
    PegRegistry reg({usd_coin()});
    EXPECT_EQ(reg.classify("wusdc_tail", 0.737140), PegStatus::Broken);
    auto dev = reg.deviation_pct("wusdc_tail", 0.737140);
    ASSERT_TRUE(dev.has_value());
    EXPECT_NEAR(*dev, -26.286, 0.01);
}

TEST(PegRegistry, AllReturnsAStableOrder) {
    PegRegistry reg({usd_coin("c_tail"), usd_coin("a_tail"), usd_coin("b_tail")});
    auto all = reg.all();
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0]->asset_id, "a_tail");
    EXPECT_EQ(all[1]->asset_id, "b_tail");
    EXPECT_EQ(all[2]->asset_id, "c_tail");
}

// ---------------------------------------------------------------------------
// [2026-08-27] Review findings 2.3 / 2.4
// ---------------------------------------------------------------------------

TEST(PegRegistry, DuplicateDeclarationIsRefusedRatherThanOverwriting) {
    // Last-write-wins would let a second entry silently replace the first
    // asset's SAFETY POLICY -- its enforce flag, its bail threshold -- with
    // no way for an operator to see which was in effect.
    PeggedAsset first = usd_coin("dup", "FIRST");
    first.bail_pct = 10.0;
    first.enforce  = true;

    PeggedAsset second = usd_coin("dup", "SECOND");
    second.bail_pct = 99.0;
    second.enforce  = false;

    PegRegistry reg;
    EXPECT_TRUE(reg.add(first));
    EXPECT_FALSE(reg.add(second)) << "a duplicate is a config error, not an update";

    const auto* kept = reg.find("dup");
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->symbol, "FIRST") << "the first declaration must survive";
    EXPECT_DOUBLE_EQ(kept->bail_pct, 10.0);
    EXPECT_TRUE(kept->enforce);
}

TEST(PegRegistry, DeviationAgreesWithClassifyAboutWhatIsPegged) {
    // If classify() says NotPegged, deviation_pct must not hand back a
    // number as though the asset had a peg to deviate from.
    PeggedAsset retired = usd_coin("gone", "GONE");
    retired.enforce = false;
    PegRegistry reg({retired});

    EXPECT_EQ(reg.classify("gone", 0.74), PegStatus::NotPegged);
    EXPECT_FALSE(reg.deviation_pct("gone", 0.74).has_value())
        << "an unenforced peg has no deviation to report";
}

TEST(PegRegistry, DeviationRejectsNonFiniteObservationsLikeClassifyDoes) {
    PegRegistry reg({usd_coin()});
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    EXPECT_EQ(reg.classify("wusdc_tail", inf), PegStatus::Unobserved);
    EXPECT_FALSE(reg.deviation_pct("wusdc_tail", inf).has_value());

    EXPECT_EQ(reg.classify("wusdc_tail", nan), PegStatus::Unobserved);
    EXPECT_FALSE(reg.deviation_pct("wusdc_tail", nan).has_value());
}

TEST(PegRegistry, ThresholdsAreInclusiveLikeDepegDetector) {
    // depeg_detector.hpp:121,131 both use >=.  If classify used > instead,
    // a deviation sitting on a configured limit would be Broken to one
    // component and merely Warn to the other -- two views of the same
    // number disagreeing about whether the limit was breached.
    //
    // Uses a target of 100.0 so the deviations land on exactly
    // representable values (2.0 and 10.0 percent) and the assertion is
    // about the operator, not about float representation.
    PeggedAsset a;
    a.asset_id     = "exact";
    a.peg_currency = "USD";
    a.peg_target   = 100.0;
    a.warn_pct     = 2.0;
    a.bail_pct     = 10.0;
    PegRegistry reg({a});

    EXPECT_EQ(reg.classify("exact", 102.0), PegStatus::Warn)
        << "exactly at warn_pct must BE the warn band, not below it";
    EXPECT_EQ(reg.classify("exact", 110.0), PegStatus::Broken)
        << "exactly at bail_pct must BE broken, not merely warn";
    EXPECT_EQ(reg.classify("exact", 101.0), PegStatus::Holding);
}

TEST(PegRegistry, ZeroWarnPctIsRejectedBecauseTheBoundaryIsInclusive) {
    // classify() uses >= so it agrees with DepegDetector.  With warn_pct
    // of 0 that makes an observation sitting exactly ON the peg classify
    // as Warn, and no healthy peg could ever reach Holding.  The per-pair
    // parser already rejects this; the registry now matches.
    PeggedAsset a = usd_coin();
    a.warn_pct = 0.0;
    PegRegistry reg;
    EXPECT_FALSE(reg.add(a))
        << "a zero warn band cannot coexist with an inclusive comparison";
}

TEST(PegRegistry, AnObservationExactlyOnPegIsHolding) {
    // The property the rejection above protects.
    PegRegistry reg({usd_coin()});
    EXPECT_EQ(reg.classify("wusdc_tail", 1.0), PegStatus::Holding);
}
