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
using xop::CrossCandidate;
using xop::CrossSelection;
using xop::select_market_cross;

namespace {

// [review round 4] Replaces the PegRegistry(std::vector<...>) constructor,
// which was removed because it called add() and threw the result away -- so a
// duplicate or incoherent declaration silently produced a half-built registry.
// This helper ASSERTS every add, which is strictly better for a fixture: a
// test that accidentally declares an asset the registry refuses now fails
// loudly instead of quietly exercising an emptier registry than it looks.
PegRegistry registry_of(std::initializer_list<PeggedAsset> assets) {
    PegRegistry reg;
    for (const auto& a : assets) {
        EXPECT_TRUE(reg.add(a))
            << "fixture declared an asset the registry refused: " << a.asset_id;
    }
    return reg;
}

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
    PegRegistry reg = registry_of({usd_coin("tail_a", "SAME"), usd_coin("tail_b", "SAME")});
    EXPECT_EQ(reg.size(), 2u);
    EXPECT_NE(reg.find("tail_a"), nullptr);
    EXPECT_NE(reg.find("tail_b"), nullptr);
    EXPECT_EQ(reg.find("SAME"), nullptr) << "symbol is not a valuation key";
}

// ---------------------------------------------------------------------------
// USD par value
// ---------------------------------------------------------------------------

TEST(PegRegistry, UsdPeggedAssetNeedsNoFxRate) {
    PegRegistry reg = registry_of({usd_coin()});
    auto v = reg.usd_par_value("wusdc_tail");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1.0);
}

TEST(PegRegistry, NonUsdPegConvertsThroughTheSuppliedRate) {
    PegRegistry reg = registry_of({euro_coin()});
    auto v = reg.usd_par_value("eur_tail", 1.09);
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1.09);
}

TEST(PegRegistry, NonUsdPegWithoutAnFxRateIsUnvalued) {
    // THE point of the currency field.  Before it, a EUR-pegged coin would
    // have been marked at $1.00 because "pegged" was hardcoded to mean
    // "a dollar".  A missing rate must produce no valuation, never a 1:1
    // substitution.
    PegRegistry reg = registry_of({euro_coin()});
    EXPECT_FALSE(reg.usd_par_value("eur_tail").has_value());
    EXPECT_FALSE(reg.usd_par_value("eur_tail", 0.0).has_value());
    EXPECT_FALSE(reg.usd_par_value("eur_tail", -1.0).has_value());
}

TEST(PegRegistry, NonUnitTargetScales) {
    PeggedAsset yen100;
    yen100.asset_id = "jpy_tail";
    yen100.peg_currency = "JPY";
    yen100.peg_target = 100.0;
    PegRegistry reg = registry_of({yen100});
    auto v = reg.usd_par_value("jpy_tail", 0.0064);  // JPY->USD
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 0.64);
}

TEST(PegRegistry, UndeclaredAssetIsUnvalued) {
    PegRegistry reg = registry_of({usd_coin()});
    EXPECT_FALSE(reg.usd_par_value("something_else").has_value());
}

TEST(PegRegistry, AnUnenforcedPegDoesNotMarkTheBookAtPar) {
    // The BYC case: the issuer announced the protocol was being sunset, so
    // the declaration should stay on record for provenance while ceasing
    // to value anything.  Under the old hardcoded branch this asset kept
    // returning 1.0 and marked the whole portfolio.
    PeggedAsset winding_down = usd_coin("byc_tail", "BYC");
    winding_down.enforce = false;
    PegRegistry reg = registry_of({winding_down});

    EXPECT_NE(reg.find("byc_tail"), nullptr) << "declaration is retained";
    EXPECT_FALSE(reg.is_pegged("byc_tail"));
    EXPECT_FALSE(reg.usd_par_value("byc_tail").has_value());
    EXPECT_EQ(reg.classify("byc_tail", 1.0), PegStatus::NotPegged);
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

TEST(PegRegistry, ClassifiesAgainstTheDeclaredThresholds) {
    PegRegistry reg = registry_of({usd_coin()});
    EXPECT_EQ(reg.classify("wusdc_tail", 1.00), PegStatus::Holding);
    EXPECT_EQ(reg.classify("wusdc_tail", 1.01), PegStatus::Holding);
    EXPECT_EQ(reg.classify("wusdc_tail", 1.05), PegStatus::Warn);
    EXPECT_EQ(reg.classify("wusdc_tail", 0.95), PegStatus::Warn);
    EXPECT_EQ(reg.classify("wusdc_tail", 0.74), PegStatus::Broken);
}

TEST(PegRegistry, DeviationIsSignedSoBelowPegIsDistinguishable) {
    PegRegistry reg = registry_of({usd_coin()});
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
    PegRegistry reg = registry_of({usd_coin()});
    EXPECT_EQ(reg.classify("wusdc_tail", std::nullopt), PegStatus::Unobserved);
    EXPECT_EQ(reg.classify("wusdc_tail", 0.0), PegStatus::Unobserved);
    EXPECT_NE(reg.classify("wusdc_tail", std::nullopt), PegStatus::Holding);
}

TEST(PegRegistry, UndeclaredAssetClassifiesAsNotPeggedNotUnobserved) {
    // An asset nobody claimed is pegged is not a monitoring gap.
    PegRegistry reg = registry_of({usd_coin()});
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
    PegRegistry reg = registry_of({a});

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
    PegRegistry reg = registry_of({usd_coin()});
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
    PegRegistry reg = registry_of({usd_coin()});
    EXPECT_EQ(reg.classify("wusdc_tail", 0.737140), PegStatus::Broken);
    auto dev = reg.deviation_pct("wusdc_tail", 0.737140);
    ASSERT_TRUE(dev.has_value());
    EXPECT_NEAR(*dev, -26.286, 0.01);
}

TEST(PegRegistry, AllReturnsAStableOrder) {
    PegRegistry reg = registry_of({usd_coin("c_tail"), usd_coin("a_tail"), usd_coin("b_tail")});
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
    PegRegistry reg = registry_of({retired});

    EXPECT_EQ(reg.classify("gone", 0.74), PegStatus::NotPegged);
    EXPECT_FALSE(reg.deviation_pct("gone", 0.74).has_value())
        << "an unenforced peg has no deviation to report";
}

TEST(PegRegistry, DeviationRejectsNonFiniteObservationsLikeClassifyDoes) {
    PegRegistry reg = registry_of({usd_coin()});
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
    PegRegistry reg = registry_of({a});

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
    PegRegistry reg = registry_of({usd_coin()});
    EXPECT_EQ(reg.classify("wusdc_tail", 1.0), PegStatus::Holding);
}

// ---------------------------------------------------------------------------
// Market-cross selection (review round 2, finding 115-3).
//
// None of this was reachable while it lived inside Engine::market_cross_for:
// constructing an Engine builds every subsystem, so the cross-orientation and
// declared-par fixes could have regressed with the whole suite green.
// ---------------------------------------------------------------------------

namespace {

PeggedAsset wrapper(std::string id, double target = 1.0,
                    std::string currency = "USD") {
    PeggedAsset a;
    a.asset_id     = std::move(id);
    a.symbol       = "WRAP";
    a.peg_currency = std::move(currency);
    a.peg_target   = target;
    a.warn_pct     = 2.0;
    a.bail_pct     = 10.0;
    a.enforce      = true;
    a.prefer_market_cross = false;      // a par wrapper
    return a;
}

CrossCandidate cand(std::string base, std::string quote, std::string name,
                    double mid, double spread_bps = 50.0) {
    return CrossCandidate{std::move(base), std::move(quote), std::move(name),
                          mid, spread_bps};
}

}  // namespace

TEST(MarketCross, DirectOrientationMultipliesByPar) {
    // BYC/WRAP: the mid is WRAP per BYC, so par converts it to dollars.
    auto reg = registry_of({wrapper("wrap_tail")});
    const auto sel = select_market_cross(
        "byc_tail", {cand("byc_tail", "wrap_tail", "BYC/WRAP", 1.05)},
        reg, 300.0);
    EXPECT_EQ(sel.pair_name, "BYC/WRAP");
    EXPECT_NEAR(sel.usd_per_unit, 1.05, 1e-12);
}

TEST(MarketCross, InverseOrientationDividesByTheMid) {
    // WRAP/BYC: the mid is BYC per WRAP, so a unit of BYC is par / mid.
    // Accepting only the direct form was the bug -- it produced no source, no
    // warning, and a silent fall back to par on an asset flagged
    // prefer_market_cross precisely because its peg was not trusted.
    auto reg = registry_of({wrapper("wrap_tail")});
    const auto sel = select_market_cross(
        "byc_tail", {cand("wrap_tail", "byc_tail", "WRAP/BYC", 1.25)},
        reg, 300.0);
    EXPECT_EQ(sel.pair_name, "WRAP/BYC");
    EXPECT_NEAR(sel.usd_per_unit, 0.8, 1e-12);   // 1.00 / 1.25
}

TEST(MarketCross, BothOrientationsAgreeOnValue) {
    auto reg = registry_of({wrapper("wrap_tail")});
    const auto direct = select_market_cross(
        "byc_tail", {cand("byc_tail", "wrap_tail", "D", 0.8)}, reg, 300.0);
    const auto inverse = select_market_cross(
        "byc_tail", {cand("wrap_tail", "byc_tail", "I", 1.25)}, reg, 300.0);
    EXPECT_NEAR(direct.usd_per_unit, inverse.usd_per_unit, 1e-12);
}

TEST(MarketCross, NonUnitParScalesTheResult) {
    // A wrapper pegged at 100 units of currency, not 1 -- returning the mid
    // raw would report wrapper units as dollars.
    auto reg = registry_of({wrapper("wrap_tail", 100.0)});
    const auto sel = select_market_cross(
        "byc_tail", {cand("byc_tail", "wrap_tail", "P", 2.0)}, reg, 300.0);
    EXPECT_NEAR(sel.usd_per_unit, 200.0, 1e-12);
}

TEST(MarketCross, AMissingFxRateYieldsNoCrossRatherThanOneToOne) {
    // A EUR-pegged wrapper has no USD value without an FX rate. "No
    // valuation" is the answer; a silent 1:1 is the failure this registry
    // exists to prevent.
    auto reg = registry_of({wrapper("wrap_tail", 1.0, "EUR")});
    const auto sel = select_market_cross(
        "byc_tail", {cand("byc_tail", "wrap_tail", "E", 1.05)}, reg, 300.0);
    EXPECT_TRUE(sel.pair_name.empty());
}

TEST(MarketCross, AnUnenforcedWrapperIsNotAParAnchor) {
    PeggedAsset retired = wrapper("wrap_tail");
    retired.enforce = false;
    auto reg = registry_of({retired});
    const auto sel = select_market_cross(
        "byc_tail", {cand("byc_tail", "wrap_tail", "R", 1.05)}, reg, 300.0);
    EXPECT_TRUE(sel.pair_name.empty())
        << "clearing enforce retires the par, so it cannot anchor a cross";
}

TEST(MarketCross, AWideBookIsWorseEvidenceThanTheDeclaredPar) {
    auto reg = registry_of({wrapper("wrap_tail")});
    EXPECT_TRUE(select_market_cross(
        "byc_tail", {cand("byc_tail", "wrap_tail", "W", 1.05, 301.0)},
        reg, 300.0).pair_name.empty());
    EXPECT_FALSE(select_market_cross(
        "byc_tail", {cand("byc_tail", "wrap_tail", "W", 1.05, 300.0)},
        reg, 300.0).pair_name.empty()) << "the bound is inclusive";
}

TEST(MarketCross, AOneSidedBookNeverQualifies) {
    auto reg = registry_of({wrapper("wrap_tail")});
    EXPECT_TRUE(select_market_cross(
        "byc_tail", {cand("byc_tail", "wrap_tail", "O", 1.05, 0.0)},
        reg, 300.0).pair_name.empty())
        << "spread_bps 0 means one-sided or crossed, not infinitely tight";
}

TEST(MarketCross, ANonPositiveOrNonFiniteMidIsSkipped) {
    auto reg = registry_of({wrapper("wrap_tail")});
    const double inf = std::numeric_limits<double>::infinity();
    for (double mid : {0.0, -1.0, inf}) {
        EXPECT_TRUE(select_market_cross(
            "byc_tail", {cand("byc_tail", "wrap_tail", "N", mid)},
            reg, 300.0).pair_name.empty()) << "mid=" << mid;
    }
}

TEST(MarketCross, TheFirstELIGIBLECandidateWinsNotTheFirstCandidate) {
    // A wide book earlier in the list must not shadow a tight one behind it.
    auto reg = registry_of({wrapper("wrap_tail")});
    const auto sel = select_market_cross("byc_tail", {
        cand("byc_tail", "unknown_tail", "NOT_A_WRAPPER", 1.01),
        cand("byc_tail", "wrap_tail", "TOO_WIDE", 1.02, 900.0),
        cand("byc_tail", "wrap_tail", "GOOD", 1.03, 20.0),
    }, reg, 300.0);
    EXPECT_EQ(sel.pair_name, "GOOD");
    EXPECT_NEAR(sel.usd_per_unit, 1.03, 1e-12);
}

TEST(MarketCross, NoCandidatesYieldsNoCross) {
    auto reg = registry_of({wrapper("wrap_tail")});
    EXPECT_TRUE(select_market_cross("byc_tail", {}, reg, 300.0)
                    .pair_name.empty());
}

// ---------------------------------------------------------------------------
// [review] usd_per_base_from_mid -- the multiplication that had no coverage.
//
// Review's point: nothing in the suite invoked usd_per_xch(),
// quote_usd_factor() or asset_usd_pseudo_price(), because no test constructs
// an Engine. So the single multiplication that keeps a EUR-pegged wrapper
// from being reported as dollars could have regressed with the suite green.
// ---------------------------------------------------------------------------

namespace {
constexpr double kScale = 1e12;   // kMojosPerXch
}

TEST(UsdPerBaseFromMid, AUnitParIsJustTheMid) {
    // 25 quote units per XCH, par $1.00 -> $25.
    const auto usd = xop::usd_per_base_from_mid(25.0 * kScale, kScale, 1.0);
    ASSERT_TRUE(usd.has_value());
    EXPECT_DOUBLE_EQ(*usd, 25.0);
}

TEST(UsdPerBaseFromMid, ANonUnitParIsAppliedRatherThanAssumedAway) {
    // The whole reason the par exists. A EUR wrapper at 1.08 USD/EUR must
    // not report its EUR mid as dollars.
    const auto usd = xop::usd_per_base_from_mid(25.0 * kScale, kScale, 1.08);
    ASSERT_TRUE(usd.has_value());
    EXPECT_DOUBLE_EQ(*usd, 27.0);
    EXPECT_GT(*usd, 25.0) << "the FX rate was dropped";
}

TEST(UsdPerBaseFromMid, ASubUnitParReducesTheValuation) {
    // A wrapper trading below its peg target must value BELOW the raw mid.
    const auto usd = xop::usd_per_base_from_mid(25.0 * kScale, kScale, 0.74);
    ASSERT_TRUE(usd.has_value());
    EXPECT_DOUBLE_EQ(*usd, 18.5);
}

TEST(UsdPerBaseFromMid, NoParMeansNoValuationNotOneDollar) {
    // peg_registry's contract: absent must never become 1.0.
    EXPECT_FALSE(xop::usd_per_base_from_mid(25.0 * kScale, kScale, 0.0));
    EXPECT_FALSE(xop::usd_per_base_from_mid(25.0 * kScale, kScale, -1.0));
}

TEST(UsdPerBaseFromMid, AQuietOrJunkBookYieldsNothing) {
    EXPECT_FALSE(xop::usd_per_base_from_mid(0.0, kScale, 1.0));
    EXPECT_FALSE(xop::usd_per_base_from_mid(-1.0, kScale, 1.0));
}

TEST(UsdPerBaseFromMid, NonFiniteInputsYieldNothing) {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(xop::usd_per_base_from_mid(inf, kScale, 1.0));
    EXPECT_FALSE(xop::usd_per_base_from_mid(nan, kScale, 1.0));
    EXPECT_FALSE(xop::usd_per_base_from_mid(25.0 * kScale, kScale, inf));
    EXPECT_FALSE(xop::usd_per_base_from_mid(25.0 * kScale, kScale, nan));
    EXPECT_FALSE(xop::usd_per_base_from_mid(25.0 * kScale, 0.0, 1.0));
}

TEST(UsdPerBaseFromMid, AnOverflowingProductIsRejectedNotReturned) {
    // Two individually finite values whose product is not. Returning
    // infinity here reaches llround downstream.
    const double huge = std::numeric_limits<double>::max();
    EXPECT_FALSE(xop::usd_per_base_from_mid(huge, 1.0, huge));
}

// ---------------------------------------------------------------------------
// [review] Finite is not enough. A finite-but-enormous par reaches llround.
// ---------------------------------------------------------------------------

TEST(PeggedAssetCoherence, AnAstronomicalParIsRejected) {
    // 1e308 is finite and positive. Multiplied by a Mojo -- up to 1e12 for
    // XCH -- it overflows long long inside std::llround, which raises the
    // invalid-operation condition and returns an unusable value rather than
    // an error.
    xop::PeggedAsset a;
    a.asset_id = "wusdc.b";
    a.peg_currency = "USD";
    a.peg_target = 1e308;
    EXPECT_FALSE(a.is_coherent());
}

TEST(PeggedAssetCoherence, ThePracticalRangeIsStillAccepted) {
    xop::PeggedAsset a;
    a.asset_id = "wusdc.b";
    a.peg_currency = "USD";
    for (const double par : {0.000001, 0.5, 1.0, 3.75, 1000.0,
                             xop::PeggedAsset::kMaxPegTarget}) {
        a.peg_target = par;
        EXPECT_TRUE(a.is_coherent()) << "rejected a legitimate par " << par;
    }
}

TEST(PeggedAssetCoherence, TheBoundLeavesHeadroomUnderLlround) {
    // The product that must survive: par * mojos-per-XCH.
    constexpr double kMojos = 1e12;
    const double worst = xop::PeggedAsset::kMaxPegTarget * kMojos;
    EXPECT_LT(worst, 9.0e18) << "the bound does not actually protect llround";
}
