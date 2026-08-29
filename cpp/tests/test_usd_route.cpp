// ---------------------------------------------------------------------------
// [S32/S27 review] Tests for the "can this asset reach USD at all" decision.
//
// This decision could not be tested before: it lived in an Engine method, and
// no test in this suite constructs an Engine.  Review said so directly --
// nothing invoked quote_usd_factor(), usd_per_xch() or
// asset_usd_pseudo_price() -- which is how a predicate that decides whether
// to write a holding down to $0 went unpinned.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "xop/risk/usd_route.hpp"

using xop::risk::ExternalXchFeed;
using xop::risk::RoutePair;
using xop::risk::ParLookups;
using xop::risk::asset_is_routable_to_usd;
using xop::risk::xch_is_anchored;
using xop::risk::xch_is_valuable;

namespace {

/// Par lookups over fixed sets. `declared` is what carries a par at all;
/// `anchors` is the narrower set usd_per_xch() will accept as an XCH anchor,
/// defaulting to the same thing when the distinction does not matter.
xop::risk::ParLookups pars(std::set<std::string> declared,
                           std::set<std::string> anchors = {},
                           bool anchors_given = false) {
    auto in = [](std::set<std::string> s) {
        return [t = std::move(s)](const std::string& id) {
            return t.count(id) > 0;
        };
    };
    return xop::risk::ParLookups{
        in(declared), in(anchors_given ? anchors : declared)};
}

ExternalXchFeed live_feed() {
    return ExternalXchFeed{true, true, 120.0};
}

const auto kNoPars = pars({});

}  // namespace

// ---------------------------------------------------------------------------
// The external XCH feed, and the threshold that silently disables it
// ---------------------------------------------------------------------------

TEST(ExternalXchFeedUsable, AConfiguredAndFreshFeedIsAnAnchor) {
    EXPECT_TRUE(live_feed().usable());
}

TEST(ExternalXchFeedUsable, AZeroThresholdIsNotAnAnchor) {
    // usd_per_xch() gates the cached price through a freshness check that
    // treats a non-positive threshold as permanently stale, so every tick
    // rejects the value. Startup validation used to certify that config as
    // anchored; it now declines the CoinGecko exemption and warns, which is
    // the agreement this constant exists to keep.
    ExternalXchFeed f = live_feed();
    f.freshness_threshold_sec = 0.0;
    EXPECT_FALSE(f.usable());
}

TEST(ExternalXchFeedUsable, ANegativeOrNonFiniteThresholdIsNotAnAnchor) {
    for (const double bad : {-1.0, std::nan(""),
                             std::numeric_limits<double>::infinity()}) {
        ExternalXchFeed f = live_feed();
        f.freshness_threshold_sec = bad;
        EXPECT_FALSE(f.usable()) << "threshold " << bad;
    }
}

TEST(ExternalXchFeedUsable, AFeedThatDoesNotQuoteChiaIsNotAnAnchor) {
    ExternalXchFeed f = live_feed();
    f.quotes_chia = false;
    EXPECT_FALSE(f.usable());
}

// ---------------------------------------------------------------------------
// XCH's own anchor
// ---------------------------------------------------------------------------

TEST(XchIsAnchored, TheExternalFeedAloneIsEnough) {
    // No pairs at all -- the 2026-08-25 state, where an operator disabled
    // every XCH market and XCH promptly priced at $0.
    EXPECT_TRUE(xch_is_anchored(live_feed(), {}, kNoPars));
}

TEST(XchIsAnchored, AnEnabledPairAgainstADeclaredParIsEnough) {
    const std::vector<RoutePair> p{{"xch", "wusdc.b"}};
    EXPECT_TRUE(xch_is_anchored(ExternalXchFeed{}, p, pars({"wusdc.b"})));
}

TEST(XchIsAnchored, AWrapperWhosePegIsNoLongerEnforcedIsNotAnAnchor) {
    // enforce:false is how an operator disowns a compromised wrapper. It
    // must stop being an anchor, not silently keep valuing at par.
    const std::vector<RoutePair> p{{"xch", "wusdc.b"}};
    EXPECT_FALSE(xch_is_anchored(ExternalXchFeed{}, p, kNoPars));
}

TEST(XchIsAnchored, AnXchQuotedPairDoesNotAnchorXch) {
    // Only XCH as BASE against a par wrapper is an anchor; CAT/XCH prices
    // the CAT, not XCH.
    const std::vector<RoutePair> p{{"dbx", "xch"}};
    EXPECT_FALSE(xch_is_anchored(ExternalXchFeed{}, p, pars({"dbx"})));
}

// ---------------------------------------------------------------------------
// The finding: pair membership is not a route
// ---------------------------------------------------------------------------

TEST(AssetRoute, TwoCatsWithNoParAndNoXchLegIsNotARoute) {
    // The reviewer's case, and the reason this file exists. An enabled
    // CAT_A/CAT_B pair with no XCH leg and no enforced par gives
    // quote_usd_factor() nothing -- 0.0 on every tick, forever. Calling that
    // a pricing path withholds the write-off, degrades instead, and never
    // lifts, because the cause is configuration and not a feed outage.
    const std::vector<RoutePair> p{{"cat_a", "cat_b"}};
    EXPECT_FALSE(asset_is_routable_to_usd("cat_a", ExternalXchFeed{}, p,
                                          kNoPars));
}

TEST(AssetRoute, TheSamePairBecomesARouteOnceTheQuoteHasAPar) {
    const std::vector<RoutePair> p{{"cat_a", "cat_b"}};
    EXPECT_TRUE(asset_is_routable_to_usd("cat_a", ExternalXchFeed{}, p,
                                         pars({"cat_b"})));
}

TEST(AssetRoute, AnXchLegIsARouteOnlyWhileXchIsAnchored) {
    const std::vector<RoutePair> p{{"dbx", "xch"}};
    EXPECT_TRUE(asset_is_routable_to_usd("dbx", live_feed(), p, kNoPars));
    // Unanchored XCH turns the whole chain into 0 * mid.
    EXPECT_FALSE(asset_is_routable_to_usd("dbx", ExternalXchFeed{}, p,
                                          kNoPars));
}

TEST(AssetRoute, AnAssetWithItsOwnParNeedsNoMarket) {
    EXPECT_TRUE(asset_is_routable_to_usd("byc", ExternalXchFeed{}, {},
                                         pars({"byc"})));
}

TEST(AssetRoute, APricedBaseDoesNotMakeItsQuoteRoutable) {
    // [review] This asserted the opposite and was wrong. quote_usd_factor()
    // yields USD per unit of the QUOTE, which prices the BASE through the
    // mid; the mirror image does not exist, because it cannot invert an
    // arbitrary par-valued base to price the quote. So WRAP/CAT_A reported a
    // route for CAT_A while the runtime returned 0 -- and CAT_A degraded
    // permanently instead of taking the structural write-off, which is the
    // failure this predicate exists to prevent.
    const std::vector<RoutePair> p{{"wusdc.b", "cat_a"}};
    EXPECT_FALSE(asset_is_routable_to_usd("cat_a", ExternalXchFeed{}, p,
                                          pars({"wusdc.b"})));
    // The base of that same pair IS routable -- that direction works.
    EXPECT_TRUE(asset_is_routable_to_usd("wusdc.b", ExternalXchFeed{}, p,
                                         pars({"wusdc.b"})));
}

TEST(AssetRoute, AnUnheldUnpairedAssetHasNoRoute) {
    const std::vector<RoutePair> p{{"xch", "wusdc.b"}};
    EXPECT_FALSE(asset_is_routable_to_usd("wmillieth.b", live_feed(), p,
                                          kNoPars));
}

TEST(AssetRoute, OnlyOneOfSeveralPairsNeedsToOfferARoute) {
    const std::vector<RoutePair> p{
        {"cat_a", "cat_b"},        // dead end
        {"cat_a", "wusdc.b"},      // route
    };
    EXPECT_TRUE(asset_is_routable_to_usd("cat_a", ExternalXchFeed{}, p,
                                         pars({"wusdc.b"})));
}

TEST(AssetRoute, XchDelegatesToItsOwnAnchorRatherThanToPairMembership) {
    // XCH appears in an enabled pair, but against a wrapper with no par and
    // with no external feed -- so XCH is NOT routable despite being paired.
    const std::vector<RoutePair> p{{"xch", "wusdc.b"}};
    EXPECT_FALSE(asset_is_routable_to_usd("xch", ExternalXchFeed{}, p,
                                          kNoPars));
}

TEST(AssetRoute, TheZeroThresholdHoleReachesEveryXchRoutedAsset) {
    // The whole point of threading the threshold through: a config that
    // looks anchored strands every asset whose only route is via XCH.
    ExternalXchFeed f = live_feed();
    f.freshness_threshold_sec = 0.0;
    const std::vector<RoutePair> p{{"dbx", "xch"}};
    EXPECT_FALSE(asset_is_routable_to_usd("dbx", f, p, kNoPars));
    EXPECT_FALSE(asset_is_routable_to_usd("xch", f, p, kNoPars));
}


// ---------------------------------------------------------------------------
// [review] "has a par" and "can anchor XCH" are different questions.
//
// BYC declares an ENFORCED par and also prefers its market cross, so
// declared_usd_par("byc") returns a value while is_par_wrapper_quote()
// rejects it. Using one lookup for both made an XCH/BYC pair report XCH --
// and therefore every XCH-routed asset -- as routable, when usd_per_xch()
// would have skipped that pair and returned 0.
// ---------------------------------------------------------------------------

TEST(XchIsAnchored, APreferMarketCrossAssetDoesNotAnchorXch) {
    const std::vector<RoutePair> p{{"xch", "byc"}};
    // Declared par: yes. Accepted by usd_per_xch() as an anchor: no.
    const auto pars_byc = pars({"byc"}, /*anchors=*/{}, /*anchors_given=*/true);
    EXPECT_FALSE(xch_is_anchored(ExternalXchFeed{}, p, pars_byc));
}

TEST(XchIsValuable, APreferMarketCrossQuoteCanStillPriceXch) {
    // [review] The two questions differ, and conflating them wrote XCH off.
    // asset_usd_pseudo_price("xch") walks XCH's base pairs and calls
    // quote_usd_factor(), which values a BYC quote through its MARKET CROSS
    // -- so XCH/BYC prices XCH even though BYC is not an acceptable par
    // anchor for usd_per_xch(). Using the narrow test here classified XCH as
    // routeless before that snapshot arrived, valued it at $0, and let a
    // partial book seed the drawdown high-water mark.
    const std::vector<RoutePair> p{{"xch", "byc"}};
    const auto pars_byc = pars({"byc"}, /*anchors=*/{}, /*anchors_given=*/true);
    EXPECT_TRUE(xch_is_valuable(ExternalXchFeed{}, p, pars_byc));
    EXPECT_TRUE(asset_is_routable_to_usd("xch", ExternalXchFeed{}, p,
                                         pars_byc));
}

TEST(AssetRoute, APreferMarketCrossQuoteStillPricesTheBaseDirectly) {
    // The narrower lookup applies only to the XCH-ANCHOR question. A pair
    // quoted in BYC can still price its own base against BYC's declared par.
    const std::vector<RoutePair> p{{"cat_a", "byc"}};
    const auto pars_byc = pars({"byc"}, /*anchors=*/{}, /*anchors_given=*/true);
    EXPECT_TRUE(asset_is_routable_to_usd("cat_a", ExternalXchFeed{}, p,
                                         pars_byc));
}
