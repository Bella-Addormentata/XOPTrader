// ---------------------------------------------------------------------------
// Asset-level peg suspension: detect, latch, re-enable.
//
// The three properties the operator was promised: a sustained depeg latches
// (one wick does not), the latch never clears itself (only the button
// does), and the button re-arms detection rather than granting amnesty.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/risk/peg_suspension.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using xop::risk::PegObservation;
using xop::risk::PegRuntime;
using xop::risk::observe_peg;
using xop::risk::reenable_peg;

namespace {

// wUSDC.b-shaped thresholds: warn 2%, bail 10%, 3 sustained observations.
PegObservation obs(PegRuntime& rt, double usd, std::uint32_t block = 100)
{
    return observe_peg(rt, usd, 1.0, 10.0, 2.0, 3, block);
}

}  // namespace

TEST(PegSuspension, HoldsInsideWarnAndWarnsPastIt)
{
    PegRuntime rt;
    EXPECT_EQ(obs(rt, 1.005), PegObservation::Holding);
    EXPECT_EQ(obs(rt, 0.97), PegObservation::Warn);   // 3% > warn, < bail
    EXPECT_FALSE(rt.suspended);
    EXPECT_EQ(rt.above_bail, 0u) << "warn is a message, not a streak";
}

TEST(PegSuspension, OneWickPastBailDoesNotLatch)
{
    PegRuntime rt;
    EXPECT_EQ(obs(rt, 0.80), PegObservation::Warn);   // 20% -- 1 of 3
    EXPECT_EQ(obs(rt, 1.00), PegObservation::Holding);
    EXPECT_EQ(rt.above_bail, 0u) << "recovery resets the streak";
    EXPECT_FALSE(rt.suspended);
}

TEST(PegSuspension, ASustainedBailLatchesExactlyAtTheThreshold)
{
    PegRuntime rt;
    EXPECT_EQ(obs(rt, 0.85, 501), PegObservation::Warn);
    EXPECT_EQ(obs(rt, 0.84, 502), PegObservation::Warn);
    EXPECT_EQ(obs(rt, 0.86, 503), PegObservation::JustSuspended);
    EXPECT_TRUE(rt.suspended);
    EXPECT_EQ(rt.suspended_at_block, 503u);

    // And only ONCE: the transition is what triggers the cancel, so a
    // second JustSuspended would cancel the same book twice.
    EXPECT_EQ(obs(rt, 0.86, 504), PegObservation::Suspended);
}

TEST(PegSuspension, TheLatchNeverClearsItself)
{
    PegRuntime rt;
    obs(rt, 0.85);
    obs(rt, 0.85);
    obs(rt, 0.85);
    ASSERT_TRUE(rt.suspended);

    // The chart heals completely. The bridge behind the wrapper may not
    // have -- that judgement is the operator's, not a counter's.
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(obs(rt, 1.0), PegObservation::Suspended);
    }
    EXPECT_TRUE(rt.suspended);
}

TEST(PegSuspension, ReenableRearmsDetectionRatherThanGrantingAmnesty)
{
    PegRuntime rt;
    obs(rt, 0.85);
    obs(rt, 0.85);
    obs(rt, 0.85);
    ASSERT_TRUE(rt.suspended);

    reenable_peg(rt);
    EXPECT_FALSE(rt.suspended);
    EXPECT_EQ(rt.above_bail, 0u);

    // Still depegged -> the next sustained run re-suspends. This is what
    // makes the button safe to offer: it means "look again", never "trust
    // it forever".
    EXPECT_EQ(obs(rt, 0.85), PegObservation::Warn);
    EXPECT_EQ(obs(rt, 0.85), PegObservation::Warn);
    EXPECT_EQ(obs(rt, 0.85), PegObservation::JustSuspended);
    EXPECT_TRUE(rt.suspended);
}

TEST(PegSuspension, ADataGapIsNotEvidenceInEitherDirection)
{
    PegRuntime rt;
    obs(rt, 0.85);
    obs(rt, 0.85);
    ASSERT_EQ(rt.above_bail, 2u);

    // NaN, zero, negative, infinite: the streak HOLDS -- an unpriceable
    // tick must neither suspend on an outage nor blind-reset a genuine run.
    for (double junk : {std::numeric_limits<double>::quiet_NaN(), 0.0, -1.0,
                        std::numeric_limits<double>::infinity()}) {
        obs(rt, junk);
        EXPECT_EQ(rt.above_bail, 2u) << junk;
        EXPECT_FALSE(rt.suspended) << junk;
    }

    // The next real observation continues the streak.
    EXPECT_EQ(obs(rt, 0.85), PegObservation::JustSuspended);
}

TEST(PegSuspension, ZeroSustainedMeansNowNotNever)
{
    PegRuntime rt;
    EXPECT_EQ(observe_peg(rt, 0.5, 1.0, 10.0, 2.0, /*sustained=*/0, 7),
              PegObservation::JustSuspended);
    EXPECT_TRUE(rt.suspended);
}

TEST(PegSuspension, TheDisplayStaysHonestWhileLatched)
{
    PegRuntime rt;
    obs(rt, 0.85);
    obs(rt, 0.85);
    obs(rt, 0.85);
    ASSERT_TRUE(rt.suspended);

    obs(rt, 0.70);
    EXPECT_NEAR(rt.last_deviation_pct, 30.0, 1e-9)
        << "the operator deciding whether to re-enable needs the CURRENT "
           "deviation, not the one that latched";
}

// ===========================================================================
// peg_usd_observation -- the route that twice suspended a healthy peg.
//
// [review, PR #134] The suspension tests all drove observe_peg() with
// ALREADY-SCALED doubles, so nothing exercised the 1e12 conversion or the
// side-quality skip that sit in front of it -- the exact path that cancelled
// every offer on every pair touching wUSDC.b (2026-08-29) and BYC
// (2026-08-30). These drive it directly.
// ===========================================================================

namespace {

// XCH/BYC on 2026-08-30: XCH at $1.43, BYC honest at par.
constexpr double kUsdXch = 1.43;
constexpr double kScale  = 1e12;          // kMojosPerXch

// A near-par cross: 1.4142 BYC per XCH => BYC = 1.43 / 1.4142 = $1.0112.
constexpr double kParMidMojos = 1.4142 * kScale;

// The dislocated published mid on the same pair: 3.25 BYC per XCH, which is
// the mean of an honest 1.50 bid and a junk 4.9995 ask.
constexpr double kJunkMidMojos = 3.25 * kScale;

using xop::risk::XchAnchorKind;
using xop::risk::XchUsdAnchor;
using xop::risk::xch_anchor_is_circular_for;

// [CIRCANCHOR 2026-09-02] The DEFAULT anchor in these tests is the
// non-circular one, deliberately: a fixture whose default were circular
// would make every unrelated test in this section depend on the new guard
// NOT firing, and one over-broad guard would then take the whole file down
// at once instead of failing where it means something.
constexpr XchUsdAnchor kExternalAnchor{kUsdXch, XchAnchorKind::ExternalFeed,
                                       {}};

// The asset under judgment in the pre-existing cases is BYC.
constexpr std::string_view kByc    = "byc";
constexpr std::string_view kWusdcB = "wusdc.b";

}  // namespace

TEST(PegUsdObservation, MojoScaledNearParCrossReadsAsPar)
{
    // THE UNIT BUG. Before the fix this route divided usd_per_xch by the RAW
    // mojo-scaled mid, giving 1.43 / 1.4142e12 = 1.01e-12 -- reported as
    // "observed $0.0000 ... 100.0% off" and latched as a depeg.
    const auto obs = xop::risk::peg_usd_observation(
        kParMidMojos, kScale, /*xch_base=*/true,
        /*mid_valuation_grade=*/true, /*bid_side_ok=*/true,
        /*ask_side_ok=*/true, kExternalAnchor, kByc);
    ASSERT_TRUE(obs.has_value());
    EXPECT_NEAR(*obs, 1.0112, 1e-3)
        << "a near-par cross must read as ~$1.01, not ~1e-12";
}

TEST(PegUsdObservation, ANearParObservationDoesNotSuspend)
{
    // End to end through observe_peg: the value this route produces must not
    // trip bail_pct, however many heartbeats it repeats for.
    const auto obs = xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true, kExternalAnchor, kByc);
    ASSERT_TRUE(obs.has_value());

    xop::risk::PegRuntime rt{};
    for (int i = 0; i < 60; ++i) {
        const auto what = xop::risk::observe_peg(
            rt, *obs, /*peg_target=*/1.0, /*bail_pct=*/10.0,
            /*warn_pct=*/2.0, /*sustained_observations=*/30,
            /*block_height=*/1000u + static_cast<std::uint32_t>(i));
        ASSERT_NE(what, xop::risk::PegObservation::JustSuspended)
            << "suspended on heartbeat " << i << " at an observation of "
            << *obs;
    }
    EXPECT_FALSE(rt.suspended);
    EXPECT_EQ(rt.above_bail, 0u);
}

TEST(PegUsdObservation, UnscaledMidWouldHaveSuspended)
{
    // The counterfactual, pinned so the regression cannot come back quietly:
    // feed observe_peg what the OLD code computed and confirm it latches.
    // If this ever stops suspending, the bail threshold moved and this whole
    // test file needs rereading.
    const double old_broken_value = kUsdXch / kParMidMojos;   // ~1.01e-12
    xop::risk::PegRuntime rt{};
    bool suspended = false;
    for (int i = 0; i < 40 && !suspended; ++i) {
        suspended = xop::risk::observe_peg(
            rt, old_broken_value, 1.0, 10.0, 2.0, 30,
            1000u + static_cast<std::uint32_t>(i))
            == xop::risk::PegObservation::JustSuspended;
    }
    EXPECT_TRUE(suspended)
        << "the pre-fix arithmetic is expected to latch a false depeg";
}

TEST(PegUsdObservation, ADisqualifiedSideYieldsNothing)
{
    // THE SOURCE BUG. Scale alone was not enough: on the dislocated book the
    // correctly scaled observation is 1.43 / 3.25 = $0.44, still 56% off par
    // and still past bail_pct 10. So a poisoned midpoint must produce NO
    // observation rather than a plausible-looking one.
    EXPECT_NEAR(kUsdXch / 3.25, 0.44, 0.01)
        << "sanity: the correctly-scaled junk observation really is ~$0.44";

    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kJunkMidMojos, kScale, true, true,
        /*bid_side_ok=*/true, /*ask_side_ok=*/false, kExternalAnchor, kByc)
            .has_value());
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kJunkMidMojos, kScale, true, true,
        /*bid_side_ok=*/false, /*ask_side_ok=*/true, kExternalAnchor, kByc)
            .has_value());
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kJunkMidMojos, kScale, true, true, false, false, kExternalAnchor, kByc)
            .has_value());
}

TEST(PegUsdObservation, ADisqualifiedSideHoldsTheStreakRatherThanResettingIt)
{
    // The data-gap contract. A skipped observation must neither advance the
    // streak toward suspension nor clear a genuine one already building --
    // absence of evidence is not evidence either way.
    xop::risk::PegRuntime rt{};

    // Three genuinely bad observations build a streak.
    for (int i = 0; i < 3; ++i) {
        static_cast<void>(xop::risk::observe_peg(
            rt, 0.50, 1.0, 10.0, 2.0, 30,
            1000u + static_cast<std::uint32_t>(i)));
    }
    ASSERT_EQ(rt.above_bail, 3u);

    // Now the book goes junk-sided: no observation at all.
    const auto gap = xop::risk::peg_usd_observation(
        kJunkMidMojos, kScale, true, true, true, /*ask_side_ok=*/false,
        kExternalAnchor, kByc);
    ASSERT_FALSE(gap.has_value());

    const double nan_obs = std::numeric_limits<double>::quiet_NaN();
    for (int i = 0; i < 10; ++i) {
        static_cast<void>(
            xop::risk::observe_peg(rt, nan_obs, 1.0, 10.0, 2.0, 30, 2000u));
    }
    EXPECT_EQ(rt.above_bail, 3u)
        << "a data gap must HOLD the streak -- neither advance nor reset";
    EXPECT_FALSE(rt.suspended);
}

TEST(PegUsdObservation, UngradedMidYieldsNothing)
{
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, /*mid_valuation_grade=*/false,
        true, true, kExternalAnchor, kByc).has_value());
}

TEST(PegUsdObservation, XchQuoteOrientationMultipliesInsteadOfDividing)
{
    // <asset>/XCH: the price is XCH per asset, so the USD value is the
    // PRODUCT. Getting this backwards yields a plausible reciprocal, which
    // is the failure mode mid_gate's orient_triangle comment warns about.
    // 0.7071 XCH per BYC * $1.43 = $1.0112.
    const auto obs = xop::risk::peg_usd_observation(
        0.7071 * kScale, kScale, /*xch_base=*/false, true, true, true,
        kExternalAnchor, kByc);
    ASSERT_TRUE(obs.has_value());
    EXPECT_NEAR(*obs, 1.0112, 1e-3);
}

TEST(PegUsdObservation, DegenerateInputsYieldNothing)
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // usd_per_xch unavailable -- the engine's own guard, restated here.
    // Kind None is what usd_per_xch_anchor() returns for its 0.0 exit.
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true,
        XchUsdAnchor{0.0, XchAnchorKind::None, {}}, kByc).has_value());
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true,
        XchUsdAnchor{nan, XchAnchorKind::ExternalFeed, {}}, kByc)
            .has_value());
    // No mid.
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        0.0, kScale, true, true, true, true, kExternalAnchor, kByc)
            .has_value());
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        inf, kScale, true, true, true, true, kExternalAnchor, kByc)
            .has_value());
    // Nonsense scale.
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kParMidMojos, 0.0, true, true, true, true, kExternalAnchor, kByc)
            .has_value());
}

// ===========================================================================
// THE CIRCULAR XCH ANCHOR -- [CIRCANCHOR 2026-09-02]
//
// WHAT WAS MEASURED. peg_suspension.hpp's own header has claimed since
// 2026-08-29 that each asset is "observed in USD through a route that DOES
// NOT pass through its own par". Following the route through the engine
// showed that was an assertion, not an enforced property:
//
//   1. Engine::usd_per_xch() prefers an external CoinGecko quote for XCH,
//      but that quote is freshness-gated and the timestamp advances only on
//      a SUCCESSFUL fetch. Four consecutive failures at the shipped 30s
//      cadence exceed the 120s default threshold and the gate flips.
//   2. It then falls back to the first enabled XCH/<par wrapper> pair and
//      returns mid(XCH/W) / 1e12 * declared_par(W).
//   3. peg_usd_observation, judging W on that SAME pair, computes
//      usd_per_xch / mid_units -- dividing the mid straight back out.
//
// So usd == declared_par(W) to floating-point rounding. And the two ways
// observe_peg can be told "nothing useful happened" are NOT symmetric:
//
//   * nullopt  -> the data-gap branch, which writes nothing. Streak HOLDS.
//   * at-par   -> the below-bail branch, which runs rt.above_bail = 0.
//
// With sustained_observations at 30, an at-par read every graded cycle means
// no streak can ever complete for the duration of a CoinGecko outage, and
// any streak built beforehand is wiped by the first cycle after it starts.
// Silent, and pointed at wUSDC.b -- the one asset whose bridge was actually
// compromised (warp.green, 2026-08-25).
//
// WHY THE DIRECTION MATTERS. Before PR #134 a mojo-scale bug made this same
// route read ~100% off, so it FALSE-SUSPENDED: loud, wrong, and impossible
// to miss. #134's scale fix is correct and it converted that false positive
// into a silent false negative. These tests pin the fix AND that asymmetry,
// because the asymmetry is the part a future "simplification" will not see.
//
// NOTE THE INVERTED FAIL-SAFE. Everywhere else here, returning nothing is
// the dangerous shortcut and producing a number is safe. In this one place
// it is reversed: REFUSING is safe (the streak holds) and OBSERVING a
// circular anchor is unsafe (it manufactures an all-clear). Each test below
// names the mutation it is meant to survive.
// ===========================================================================

TEST(PegUsdObservation, ACircularSelfAnchorYieldsNothing)
{
    // THE BUG. XCH is priced off XCH/wUSDC.b at par, and wUSDC.b is judged
    // on that same pair.
    constexpr double kPar = 1.0;
    constexpr double kMidUnits = 1.4142;      // wUSDC.b per XCH
    constexpr XchUsdAnchor kSelfAnchor{
        kMidUnits * kPar, XchAnchorKind::DeclaredParCross, kWusdcB};

    // Make the circle visible rather than asserting it from memory: the
    // anchor divided by its own mid is, identically, the declared par.
    EXPECT_NEAR(kSelfAnchor.usd / kMidUnits, kPar, 1e-12)
        << "the circle: dividing the anchor by the mid it was built from "
           "returns the declared par, so the observation cannot disagree "
           "with the declaration it is supposed to be testing";

    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kMidUnits * kScale, kScale, /*xch_base=*/true,
        /*mid_valuation_grade=*/true, /*bid_side_ok=*/true,
        /*ask_side_ok=*/true, kSelfAnchor, kWusdcB).has_value())
        << "an anchor built from the judged asset's own declared par is not "
           "an observation about that asset";

    // MUTATION: delete the guard in peg_usd_observation. Without it this
    // returns 1.0 -- exactly peg_target -- and the EXPECT_FALSE fails.
}

TEST(PegUsdObservation, ACircularAnchorIsRefusedInEitherOrientation)
{
    // The reversed pair. XCH is anchored on XCH/wUSDC.b while the watcher
    // observes through wUSDC.b/XCH: a DIFFERENT pair name, the same circle,
    // because both legs are wUSDC.b's own book against wUSDC.b's own par.
    // This is why the rule keys on the ASSET and not on the pair.
    constexpr XchUsdAnchor kSelfAnchor{
        1.4142, XchAnchorKind::DeclaredParCross, kWusdcB};

    EXPECT_FALSE(xop::risk::peg_usd_observation(
        0.7071 * kScale, kScale, /*xch_base=*/false, true, true, true,
        kSelfAnchor, kWusdcB).has_value());

    // MUTATION: key the guard on a pair name (or on xch_base) instead of on
    // the asset id and this orientation slips through.
}

TEST(PegUsdObservation, ACrossAssetParAnchorIsStillAnObservation)
{
    // THE OVER-CORRECTION GUARD, and the reason the rule is not simply
    // "refuse whenever the DEX fallback was used".
    //
    // XCH anchored on XCH/BYC, judging wUSDC.b. The observation is
    // par(BYC) * mid(XCH/BYC) / mid(XCH/wUSDC.b) -- wUSDC.b priced in BYC.
    // BYC's par is in there, wUSDC.b's is not, so this MOVES when wUSDC.b
    // depegs. Refusing it would blind every asset for the whole outage:
    // the same false negative, just wider.
    constexpr XchUsdAnchor kBycAnchor{
        kUsdXch, XchAnchorKind::DeclaredParCross, kByc};

    const auto healthy = xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true, kBycAnchor, kWusdcB);
    ASSERT_TRUE(healthy.has_value())
        << "a par anchor on a DIFFERENT asset is a real market cross";
    EXPECT_NEAR(*healthy, 1.0112, 1e-3);

    // And it must still SEE a depeg. Same anchor, dislocated mid, both
    // sides clean -- the arithmetic that produced the honest $0.44 on
    // 2026-08-30. Refusing here would silence a genuine signal.
    const auto depegged = xop::risk::peg_usd_observation(
        kJunkMidMojos, kScale, true, true, true, true, kBycAnchor, kWusdcB);
    ASSERT_TRUE(depegged.has_value())
        << "the guard must not silence a real observation";
    EXPECT_NEAR(*depegged, 0.44, 0.01);

    xop::risk::PegRuntime rt{};
    EXPECT_EQ(xop::risk::observe_peg(rt, *depegged, 1.0, 10.0, 2.0, 1, 900u),
              PegObservation::JustSuspended)
        << "56% off par, past bail, and it must still latch";

    // MUTATION: coarsen the guard to `kind == DeclaredParCross` and this
    // test fails at the first ASSERT_TRUE. This is the test that stops the
    // fix from becoming a worse version of the bug.
}

TEST(PegUsdObservation, AFreshExternalAnchorIsNeverCircular)
{
    // The normal path: CoinGecko fresh, so the dollar price of XCH consults
    // no pair and no declared par at all.
    const auto obs = xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true, kExternalAnchor,
        kWusdcB);
    ASSERT_TRUE(obs.has_value());
    EXPECT_NEAR(*obs, 1.0112, 1e-3);

    // And an anchor with NO provenance recorded must not "match" an asset
    // with no id -- an unpopulated struct is not evidence of a circle.
    EXPECT_TRUE(xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true,
        XchUsdAnchor{kUsdXch, XchAnchorKind::ExternalFeed, {}},
        std::string_view{}).has_value());

    // MUTATION: drop the `!par_asset_id.empty()` term from the predicate and
    // this second case starts refusing.
}

TEST(PegUsdObservation, AMislabelledAnchorIsRefusedRatherThanWavedThrough)
{
    // The predicate deliberately ignores `kind`. Today par_asset_id is set
    // only for DeclaredParCross, so this state cannot arise -- but if some
    // future edit records a par asset under another kind, the question is
    // which way the ambiguity resolves. It must resolve to REFUSE, because
    // refusing holds the streak and observing clears it.
    constexpr XchUsdAnchor kMislabelled{
        1.4142, XchAnchorKind::ExternalFeed, kWusdcB};
    EXPECT_TRUE(xch_anchor_is_circular_for(kMislabelled, kWusdcB));
    EXPECT_FALSE(xop::risk::peg_usd_observation(
        1.4142 * kScale, kScale, true, true, true, true, kMislabelled,
        kWusdcB).has_value());

    // MUTATION: add `kind == XchAnchorKind::DeclaredParCross` back into the
    // predicate and this fails -- which is the point: that term would be a
    // fail-OPEN clause.
}

TEST(PegUsdObservation, ACircularAnchorHoldsTheStreakInsteadOfClearingIt)
{
    // THE REGRESSION THAT WOULD HAVE CAUGHT THIS. The two arms differ only
    // in whether the circular reading reaches observe_peg, so the assertion
    // that separates them IS the bug.
    //
    // wUSDC.b thresholds as configured for the re-enable path: warn 2%,
    // bail 10%, 30 sustained observations. The asset has been genuinely
    // depegged at $0.50 for 29 heartbeats -- one short of latching.
    constexpr double kDepegged = 0.50;
    constexpr std::uint32_t kSustained = 30;

    // [review round 8] RETURNS the fixture instead of ASSERTing inside a
    // lambda. gtest's ASSERT_ macros expand to a bare `return;`, which in a
    // lambda returns only from the LAMBDA -- so a broken fixture used to let
    // both arms run on against an unexpected PegRuntime and report a spread
    // of downstream mismatches instead of the one accurate failure. Observed
    // live: 13 error blocks, with the load-bearing one buried among ten
    // identical warn mismatches. The abort semantics belong at test level.
    auto build_29_streak = []() {
        xop::risk::PegRuntime rt{};
        for (std::uint32_t i = 0; i < 29; ++i) {
            static_cast<void>(xop::risk::observe_peg(
                rt, kDepegged, 1.0, 10.0, 2.0, kSustained, 1000u + i));
        }
        return rt;
    };

    // --- ARM 1: what the circular route PRODUCES, fed in directly. -------
    // Pinned as a counterfactual in the style of UnscaledMidWouldHaveSus-
    // pended, so nobody later reasons that an at-par reading is harmless.
    {
        xop::risk::PegRuntime rt = build_29_streak();
        ASSERT_EQ(rt.above_bail, 29u) << "fixture: 29 depegged observations";
        ASSERT_FALSE(rt.suspended) << "fixture: one short of latching";

        EXPECT_EQ(xop::risk::observe_peg(rt, /*usd=*/1.0, 1.0, 10.0, 2.0,
                                         kSustained, 1029u),
                  PegObservation::Holding);
        EXPECT_EQ(rt.above_bail, 0u)
            << "an exactly-at-par observation does not merely fail to "
               "advance the streak -- it DESTROYS it, and reports 0.0% off "
               "to the operator while doing so";
        EXPECT_NEAR(rt.last_deviation_pct, 0.0, 1e-12);
    }

    // --- ARM 2: the fix. The circular anchor never becomes a number. -----
    {
        xop::risk::PegRuntime rt = build_29_streak();
        ASSERT_EQ(rt.above_bail, 29u) << "fixture: 29 depegged observations";
        ASSERT_FALSE(rt.suspended) << "fixture: one short of latching";

        // CoinGecko has gone stale; XCH is now priced off XCH/wUSDC.b at
        // the declared par, and wUSDC.b is judged through that same pair.
        constexpr XchUsdAnchor kSelfAnchor{
            1.4142, XchAnchorKind::DeclaredParCross, kWusdcB};
        const auto refused = xop::risk::peg_usd_observation(
            1.4142 * kScale, kScale, true, /*grade=*/true,
            /*bid_ok=*/true, /*ask_ok=*/true, kSelfAnchor, kWusdcB);
        EXPECT_FALSE(refused.has_value());

        // NOT `ASSERT`, and NOT a hardcoded NaN. The loop below feeds
        // observe_peg whatever the route ACTUALLY produced, converted the
        // way step_observe_asset_pegs converts it -- nullopt leaves usd_obs
        // at quiet_NaN. Asserting out here, or substituting a NaN by hand,
        // would make the streak assertion below a restatement of the
        // already-covered data-gap case instead of a test of THIS route.
        // With the guard deleted, `refused` holds 1.0 and the outage loop
        // wipes the streak -- which is the failure being pinned.
        const double usd_obs =
            refused ? *refused : std::numeric_limits<double>::quiet_NaN();

        // Ten heartbeats of outage.
        for (std::uint32_t i = 0; i < 10; ++i) {
            EXPECT_EQ(xop::risk::observe_peg(rt, usd_obs, 1.0, 10.0, 2.0,
                                             kSustained, 2000u + i),
                      PegObservation::Warn);
        }
        EXPECT_EQ(rt.above_bail, 29u)
            << "the streak must survive the entire outage: a refused "
               "observation HOLDS it, whereas the at-par number the "
               "circular route produces would have reset it to 0 on the "
               "first cycle";

        // The feed returns. One genuine observation completes the run, and
        // the suspension the live bug would have prevented FOREVER fires.
        EXPECT_EQ(xop::risk::observe_peg(rt, kDepegged, 1.0, 10.0, 2.0,
                                         kSustained, 3000u),
                  PegObservation::JustSuspended);
        EXPECT_TRUE(rt.suspended);
        EXPECT_EQ(rt.suspended_at_block, 3000u);
    }

    // MUTATION: delete the guard and arm 2 fails twice over -- at the
    // ASSERT_FALSE, and again because above_bail lands at 0 and nothing
    // ever suspends.
}

TEST(PegUsdObservation, XchAnchorCircularityIsDecidedAtCompileTime)
{
    // peg_suspension.hpp static_asserts these four, so a GCC build fails on
    // the header alone. Restated at runtime so a reader of this file sees
    // the truth table without having to go and find it -- and so gtest
    // names the case that broke rather than just a header line.
    constexpr XchUsdAnchor kSelf{1.4142, XchAnchorKind::DeclaredParCross,
                                 kWusdcB};
    constexpr XchUsdAnchor kCross{kUsdXch, XchAnchorKind::DeclaredParCross,
                                  kByc};
    constexpr XchUsdAnchor kNone{0.0, XchAnchorKind::None, {}};

    static_assert(xch_anchor_is_circular_for(kSelf, kWusdcB));
    static_assert(!xch_anchor_is_circular_for(kCross, kWusdcB));
    static_assert(!xch_anchor_is_circular_for(kExternalAnchor, kWusdcB));
    static_assert(!xch_anchor_is_circular_for(kNone, kWusdcB));

    EXPECT_TRUE(xch_anchor_is_circular_for(kSelf, kWusdcB));
    EXPECT_FALSE(xch_anchor_is_circular_for(kCross, kWusdcB));
    EXPECT_FALSE(xch_anchor_is_circular_for(kExternalAnchor, kWusdcB));
    EXPECT_FALSE(xch_anchor_is_circular_for(kNone, kWusdcB));

    // The anchor asset judged as ITSELF is refused; judged as the other
    // asset it is not. Same struct, different question.
    EXPECT_TRUE(xch_anchor_is_circular_for(kCross, kByc));
}

// ===========================================================================
// select_xch_usd_anchor -- THE WIRING THAT ARMS EVERYTHING ABOVE.
//
// [review round 8] Every test above hands peg_usd_observation a hand-built
// XchUsdAnchor. That covers the RULE and leaves the RECORDING bare, and the
// recording is what makes the rule reachable. The mutation that motivated
// this section: have the DeclaredParCross fallback return
// `ExternalFeed, {}` instead -- the same VALUE, no provenance. It reinstates
// the original defect completely (xch_anchor_is_circular_for then answers
// false for every anchor forever) and the entire suite stayed green,
// because nothing constructed the anchor from configuration.
// ===========================================================================

namespace {

// The shipped shape: wUSDC.b is a par wrapper and therefore an anchor
// candidate; BYC declares an enforced par but prefers its market cross, so
// it is NOT one. That asymmetry is why quote_anchors_xch is its own field
// rather than "has a declared par".
std::vector<xop::risk::XchAnchorCandidate> shipped_candidates()
{
    return {
        {"XCH/wUSDC.b", "xch", kWusdcB, /*quote_anchors_xch=*/true},
        {"XCH/BYC", "xch", kByc, /*quote_anchors_xch=*/false},
    };
}

// Prices every candidate at a fixed mid * par. The selector is lazy, so
// this also records WHICH candidates it was actually asked about.
xop::risk::XchAnchorPrice pricer(std::vector<std::string>& asked,
                                 std::optional<double> answer)
{
    return [&asked, answer](const xop::risk::XchAnchorCandidate& c)
               -> std::optional<double> {
        asked.emplace_back(c.pair_name);
        return answer;
    };
}

}  // namespace

TEST(SelectXchUsdAnchor, AFreshExternalFeedIsRecordedAsExternalAndNotCircular)
{
    std::vector<std::string> asked;
    const auto anchor = xop::risk::select_xch_usd_anchor(
        /*external_feed_fresh=*/true, kUsdXch, shipped_candidates(),
        pricer(asked, 1.4142));

    EXPECT_DOUBLE_EQ(anchor.usd, kUsdXch);
    EXPECT_EQ(anchor.kind, XchAnchorKind::ExternalFeed);
    EXPECT_TRUE(anchor.par_asset_id.empty());
    EXPECT_TRUE(asked.empty())
        << "the fallback must not be priced at all when the feed is good";

    // The property the whole fix turns on, asserted on a CONSTRUCTED anchor
    // rather than a literal.
    EXPECT_FALSE(xch_anchor_is_circular_for(anchor, kWusdcB));
    EXPECT_FALSE(xch_anchor_is_circular_for(anchor, kByc));
}

TEST(SelectXchUsdAnchor, AStaleFeedFallsBackAndRECORDSWhosePairItUsed)
{
    // ***THE M5 KILLER.*** CoinGecko is stale, so XCH is priced off
    // mid(XCH/wUSDC.b) * par(wUSDC.b). The VALUE is a perfectly good USD
    // rate and every consumer of usd_per_xch() is happy with it -- which is
    // exactly why dropping the provenance was invisible. The peg watcher is
    // the one caller that must be told, because it is about to divide that
    // same mid straight back out.
    std::vector<std::string> asked;
    const auto anchor = xop::risk::select_xch_usd_anchor(
        /*external_feed_fresh=*/false, 0.0, shipped_candidates(),
        pricer(asked, 1.4142));

    EXPECT_DOUBLE_EQ(anchor.usd, 1.4142);
    EXPECT_EQ(anchor.kind, XchAnchorKind::DeclaredParCross);
    EXPECT_EQ(anchor.par_asset_id, kWusdcB)
        << "the anchor must name the asset whose DECLARED par it consumed";

    // BYC is not a candidate, so it must never have been priced.
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked[0], "XCH/wUSDC.b");

    // And now the consequence, end to end: wUSDC.b judged through the very
    // pair that anchored XCH is REFUSED, while BYC through the same anchor
    // is still a real observation.
    EXPECT_TRUE(xch_anchor_is_circular_for(anchor, kWusdcB));
    EXPECT_FALSE(xch_anchor_is_circular_for(anchor, kByc));

    EXPECT_FALSE(xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true, anchor, kWusdcB)
                     .has_value())
        << "the wired-up anchor must refuse its own asset -- this is the "
           "assertion the ExternalFeed/{} mutation breaks";
    EXPECT_TRUE(xop::risk::peg_usd_observation(
        kParMidMojos, kScale, true, true, true, true, anchor, kByc)
                    .has_value());
}

TEST(SelectXchUsdAnchor, AFreshFeedWithNoUsableXchPriceStillFallsBack)
{
    // The route the operator warn used to describe wrongly as "the external
    // feed is stale". The feed is FRESH; it just carries no usable chia
    // price -- a missing key, a renamed id, a NaN, a non-positive quote.
    // All four land on the fallback, with full provenance.
    for (const double bad : {0.0, -1.0,
                             std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        std::vector<std::string> asked;
        const auto anchor = xop::risk::select_xch_usd_anchor(
            /*external_feed_fresh=*/true, bad, shipped_candidates(),
            pricer(asked, 1.4142));
        EXPECT_EQ(anchor.kind, XchAnchorKind::DeclaredParCross);
        EXPECT_EQ(anchor.par_asset_id, kWusdcB);
    }
}

TEST(SelectXchUsdAnchor, AnUnpriceableCandidateIsSkippedRatherThanTrusted)
{
    // A candidate whose par is unavailable -- no FX rate, or the peg is
    // itself SUSPENDED -- yields nullopt and must not become an anchor.
    std::vector<std::string> asked;
    const auto anchor = xop::risk::select_xch_usd_anchor(
        false, 0.0, shipped_candidates(), pricer(asked, std::nullopt));

    EXPECT_EQ(anchor.kind, XchAnchorKind::None);
    EXPECT_DOUBLE_EQ(anchor.usd, 0.0);
    EXPECT_TRUE(anchor.par_asset_id.empty());
    // Unknown, NOT a guessed rate: 0 is what callers read as "no USD
    // valuation", and a hard-coded fallback would bake a 2x error into a
    // PERSISTED cost basis.
    EXPECT_FALSE(xch_anchor_is_circular_for(anchor, kWusdcB));
}

TEST(SelectXchUsdAnchor, ANonPositiveOrNonFinitePriceIsNotAnAnchor)
{
    for (const double bad : {0.0, -2.0,
                             std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        std::vector<std::string> asked;
        const auto anchor = xop::risk::select_xch_usd_anchor(
            false, 0.0, shipped_candidates(), pricer(asked, bad));
        EXPECT_EQ(anchor.kind, XchAnchorKind::None)
            << "a junk fallback price must be 'unknown', never an anchor "
               "carrying a par asset id";
        EXPECT_TRUE(anchor.par_asset_id.empty());
    }
}

TEST(SelectXchUsdAnchor, OnlyXchBasedParWrapperPairsAreCandidates)
{
    // Neither of these may anchor XCH: BYC prefers its market cross, and
    // wUSDC.b/XCH has XCH as the QUOTE, so its mid is not USD-per-XCH at
    // all. Selecting either would produce a silently mis-scaled rate.
    const std::vector<xop::risk::XchAnchorCandidate> candidates{
        {"XCH/BYC", "xch", kByc, /*quote_anchors_xch=*/false},
        {"wUSDC.b/XCH", kWusdcB, "xch", /*quote_anchors_xch=*/false},
    };
    std::vector<std::string> asked;
    const auto anchor = xop::risk::select_xch_usd_anchor(
        false, 0.0, candidates, pricer(asked, 1.4142));

    EXPECT_EQ(anchor.kind, XchAnchorKind::None);
    EXPECT_TRUE(asked.empty());
}

TEST(SelectXchUsdAnchor, TheFirstUsableCandidateWinsAndNamesItsOwnAsset)
{
    // Two wrappers configured. The first cannot be priced, so the second
    // anchors -- and the provenance must name the one that ACTUALLY
    // supplied the par, not the one that was tried first. Re-deriving the
    // answer later ("which pair would usd_per_xch have picked") is the
    // drift bug this recording exists to avoid.
    constexpr std::string_view kUsds = "usds";
    const std::vector<xop::risk::XchAnchorCandidate> candidates{
        {"XCH/wUSDC.b", "xch", kWusdcB, true},
        {"XCH/USDS", "xch", kUsds, true},
    };

    std::vector<std::string> asked;
    const auto anchor = xop::risk::select_xch_usd_anchor(
        false, 0.0, candidates,
        [&asked](const xop::risk::XchAnchorCandidate& c)
            -> std::optional<double> {
            asked.emplace_back(c.pair_name);
            if (c.quote_asset_id == kWusdcB) return std::nullopt;
            return 1.4142;
        });

    EXPECT_EQ(anchor.kind, XchAnchorKind::DeclaredParCross);
    EXPECT_EQ(anchor.par_asset_id, kUsds);
    EXPECT_EQ(asked.size(), 2u) << "both candidates tried, in order";

    // The asset that could NOT be priced is not blinded by this anchor;
    // the one that supplied the par is.
    EXPECT_FALSE(xch_anchor_is_circular_for(anchor, kWusdcB));
    EXPECT_TRUE(xch_anchor_is_circular_for(anchor, kUsds));
}

TEST(SelectXchUsdAnchor, NoCandidatesAndNoFeedIsUnknownRatherThanAGuess)
{
    const auto anchor = xop::risk::select_xch_usd_anchor(
        false, 0.0, {}, nullptr);
    EXPECT_EQ(anchor.kind, XchAnchorKind::None);
    EXPECT_DOUBLE_EQ(anchor.usd, 0.0);
    EXPECT_TRUE(anchor.par_asset_id.empty());

    // A missing pricer must not crash the selection, and must not invent an
    // anchor either.
    const auto no_pricer = xop::risk::select_xch_usd_anchor(
        false, 0.0, shipped_candidates(), nullptr);
    EXPECT_EQ(no_pricer.kind, XchAnchorKind::None);
}
