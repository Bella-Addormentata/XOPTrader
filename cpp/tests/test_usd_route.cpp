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
using xop::risk::asset_is_routable_to_usd;
using xop::risk::xch_is_anchored;

namespace {

/// Par lookup over a fixed set, matching the engine's
/// declared_usd_par().has_value().
xop::risk::ParLookup pars(std::set<std::string> declared) {
    return [d = std::move(declared)](const std::string& id) {
        return d.count(id) > 0;
    };
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
    // Legal, silent, and anchorless: usd_per_xch() gates the cached price
    // through a freshness check that treats a non-positive threshold as
    // permanently stale, so every tick rejects the value while startup
    // validation reported a healthy anchor.
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

TEST(AssetRoute, AssetWorksAsEitherLegOfThePair) {
    const std::vector<RoutePair> p{{"wusdc.b", "cat_a"}};
    EXPECT_TRUE(asset_is_routable_to_usd("cat_a", ExternalXchFeed{}, p,
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
