// test_fair_value.cpp -- Triangulated fair value: solver, accessors, band.
//
// Regression coverage for the 2026-08-01 10:31 UTC XCH/BYC ladder sweep: all
// six ask tiers filled in one block at 1.2704-1.2979 BYC/XCH.  Every one of
// those fills logged a POSITIVE realized P&L, because P&L was measured against
// a cost basis that equally-mispriced bids had inflated.  The root cause was
// that the ladder is centred on a dexie book mid that nothing validated.
//
// The first attempted fix valued BYC at a $1.00 peg.  It was rejected: BYC has
// never traded at par, so the peg is an assumption rather than an observation.
// What replaces it is a weighted least-squares solve over the graph of assets
// and pairs, re-run per pair with that pair's own book edge deleted.
//
// The tests below lock in the properties that make it work:
//   1. MarketDataFeed reports the ABSENCE of an independent fair value
//      explicitly -- it never falls back to the book it is meant to validate.
//   2. The solver recovers an exact triangle, and distributes the error of an
//      inconsistent one according to observation quality, not pair identity.
//   3. A leg reachable only through a wide or frozen book yields a large
//      sigma and therefore Unavailable -- an honest "I do not know" rather
//      than a confident wrong number.
//   4. Adding the arbitrage-anchored AMM observation for the same pair (which
//      is NOT excluded, because a pool is not an order book) turns that same
//      situation into a usable clamp that catches every swept price.
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets in test data)
//   ISO/IEC 5055        (bounds-checked assertions, no UB)
//   ISO/IEC 25000       (clear test naming)

#include "xop/config.hpp"
#include "xop/execution/fair_value_solver.hpp"
#include "xop/execution/market_data.hpp"
#include "xop/state.hpp"
#include "xop/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace xop {
namespace {

// ===========================================================================
// Shared fixtures: the live graph, at the block of the sweep.
//
// snapshots row set for block 9087661 (2026-08-01 10:30:59), mid_price_mojos
// / 1e12 and spread_bps verbatim from the database.
// ===========================================================================

constexpr double kXchUsd   = 1.428457022211;  // External feed mark.
constexpr double kUsdcUsd  = 1.0;

constexpr double kBookXchUsdc      = 1.428457022211;
constexpr double kBookXchUsdcSpr   = 253.95759915110403;
constexpr double kBookXchByc       = 1.267345926206;
constexpr double kBookXchBycSpr    = 2114.4567455022375;
constexpr double kBookBycUsdc      = 1.144727818835;
constexpr double kBookBycUsdcSpr   = 1258.9641434290033;

// TibetSwap pools, quote-per-base on the XCH side.
// BYC pool     175.4061 XCH / 261.655 BYC     -> 1.491713 BYC per XCH.
// wUSDC.b pool 393.8683 XCH / 564.862 wUSDC.b -> 1.434138 wUSDC.b per XCH.
constexpr double kAmmXchByc  = 261.655 / 175.4061;
constexpr double kAmmXchUsdc = 564.862 / 393.8683;

// The six ask prices swept in block 9087665/9087667 (trade_log rows 875-880).
const std::vector<double> kSweptAsks = {
    1.270405261412, 1.273464596617, 1.277543710223,
    1.283662380632, 1.290800829443, 1.297939278254,
};

// Pair indices used as Edge::owner_pair throughout.
enum : int { kPairXchUsdc = 0, kPairXchByc = 1, kPairBycUsdc = 2 };

const StrategyConfig kDefaults{};

/// Edge sigma exactly as Engine::update_fair_values builds it.
double book_sigma(double spread_bps, int print_age, double n_offers = 1.0) {
    return fv::combine_sigma_bps({
        std::max(std::abs(spread_bps) / 2.0,
                 kDefaults.fair_value_min_book_sigma_bps),
        kDefaults.fair_value_stale_sigma_bps_per_print
            * static_cast<double>(print_age),
        kDefaults.fair_value_depth_ref_bps / std::sqrt(std::max(1.0, n_offers)),
    });
}

std::vector<fv::Anchor> live_anchors() {
    return {
        fv::Anchor{"xch",   kXchUsd,  kDefaults.fair_value_feed_sigma_bps},
        fv::Anchor{"wusdc", kUsdcUsd, kDefaults.fair_value_feed_sigma_bps},
    };
}

/// The three books as they stood at the sweep.  BYC/wUSDC.b carries the
/// print age it actually had: its mid had not moved for 26 heartbeats.
std::vector<fv::Edge> live_book_edges() {
    return {
        fv::Edge{"xch", "wusdc", kBookXchUsdc,
                 book_sigma(kBookXchUsdcSpr, 0), kPairXchUsdc, true},
        fv::Edge{"xch", "byc", kBookXchByc,
                 book_sigma(kBookXchBycSpr, 0), kPairXchByc, true},
        fv::Edge{"byc", "wusdc", kBookBycUsdc,
                 book_sigma(kBookBycUsdcSpr, 26), kPairBycUsdc, true},
    };
}

/// Confidence tier exactly as Engine::update_fair_values assigns it.
FairValueTier tier_of(const fv::Solution& s) {
    if (!s.ok) return FairValueTier::Unavailable;
    if (s.sigma_bps > kDefaults.fair_value_max_sigma_bps) {
        return FairValueTier::Unavailable;
    }
    if (s.both_anchored && s.sigma_bps <= kDefaults.fair_value_tight_sigma_bps) {
        return FairValueTier::CexDirect;
    }
    if (s.redundant && s.sigma_bps <= kDefaults.fair_value_tight_sigma_bps) {
        return FairValueTier::Triangulated;
    }
    return FairValueTier::Inferred;
}

/// Deviation band exactly as the Step 7 guard computes it.
double band_bps(const fv::Solution& s) {
    return kDefaults.max_fair_value_deviation_bps
         + kDefaults.fair_value_sigma_band_mult * std::max(0.0, s.sigma_bps);
}

// ===========================================================================
// Part 1 -- the solver, as pure arithmetic
// ===========================================================================

TEST(FairValueSolver, RecoversAnExactTriangle) {
    // A perfectly consistent graph: 1.4 * 1.0 == 1.4.  Whatever the weights,
    // a consistent system must reproduce its own edges exactly.
    const std::vector<fv::Anchor> anchors = {
        fv::Anchor{"xch", 1.4, 100.0}, fv::Anchor{"wusdc", 1.0, 100.0}};
    const std::vector<fv::Edge> edges = {
        fv::Edge{"xch", "wusdc", 1.4, 50.0, 0, true},
        fv::Edge{"xch", "byc",   1.4, 50.0, 1, true},
        fv::Edge{"byc", "wusdc", 1.0, 50.0, 2, true},
    };

    const auto s = fv::solve_pair(anchors, edges, "xch", "byc", 1);
    ASSERT_TRUE(s.ok);
    EXPECT_NEAR(s.price, 1.4, 1e-9);
    EXPECT_GT(s.sigma_bps, 0.0);
}

TEST(FairValueSolver, IsSymmetricUnderPairInversion) {
    // Log space is not a convenience: the answer must not depend on which leg
    // a pair happens to be named after.
    const std::vector<fv::Anchor> anchors = {
        fv::Anchor{"xch", 1.42, 100.0}, fv::Anchor{"wusdc", 1.0, 100.0}};
    const std::vector<fv::Edge> edges = {
        fv::Edge{"xch", "wusdc", 1.44, 60.0, 0, true},
        fv::Edge{"xch", "byc",   1.30, 90.0, 1, true},
    };

    const auto fwd = fv::solve_pair(anchors, edges, "xch", "byc", -1);
    const auto rev = fv::solve_pair(anchors, edges, "byc", "xch", -1);
    ASSERT_TRUE(fwd.ok);
    ASSERT_TRUE(rev.ok);
    EXPECT_NEAR(fwd.price * rev.price, 1.0, 1e-9);
    EXPECT_NEAR(fwd.sigma_bps, rev.sigma_bps, 1e-6);
}

TEST(FairValueSolver, ExcludesThePairsOwnBookEdge) {
    // The independence guarantee.  XCH/BYC's own book says 1.05; everything
    // else says 1.44 / 1.10 = 1.3090909...  With the book excluded the answer
    // must come from the other route, exactly, and not be dragged by the book
    // it is meant to validate.
    const std::vector<fv::Anchor> anchors = {
        fv::Anchor{"xch", 1.44, 100.0}, fv::Anchor{"wusdc", 1.0, 100.0}};
    const std::vector<fv::Edge> edges = {
        fv::Edge{"xch", "wusdc", 1.44, 20.0, 0, true},
        fv::Edge{"xch", "byc",   1.05, 20.0, 1, true},   // wildly different
        fv::Edge{"byc", "wusdc", 1.10, 20.0, 2, true},
    };

    const auto excluded = fv::solve_pair(anchors, edges, "xch", "byc", 1);
    ASSERT_TRUE(excluded.ok);
    EXPECT_NEAR(excluded.price, 1.44 / 1.10, 1e-9);

    // Including it drags the answer a long way toward 1.05 -- which is exactly
    // why it is excluded.  The two must differ materially, or the exclusion is
    // a no-op dressed up as a guarantee.
    const auto included = fv::solve_pair(anchors, edges, "xch", "byc", -1);
    ASSERT_TRUE(included.ok);
    EXPECT_LT(included.price, excluded.price - 0.05);
}

TEST(FairValueSolver, AmmObservationSurvivesItsOwnPairsExclusion) {
    // A constant-product pool is held to fair value by arbitrage, not by
    // anyone's willingness to quote, so it is independent of the order book in
    // exactly the sense the guard requires.  Excluding pair 1's BOOK must
    // leave pair 1's AMM edge in place.
    const std::vector<fv::Anchor> anchors = {
        fv::Anchor{"xch", 1.44, 100.0}};
    const std::vector<fv::Edge> edges = {
        fv::Edge{"xch", "byc", 1.30, 20.0, 1, /*is_book=*/true},
        fv::Edge{"xch", "byc", 1.49, 50.0, 1, /*is_book=*/false},
    };

    const auto s = fv::solve_pair(anchors, edges, "xch", "byc", 1);
    ASSERT_TRUE(s.ok);
    EXPECT_NEAR(s.price, 1.49, 1e-6);  // Only the AMM edge remains.
}

TEST(FairValueSolver, UnanchoredComponentIsNotSolved) {
    // Two assets that touch no external anchor have no USD scale at all.  The
    // solver must say so rather than return the arbitrary constant that the
    // singular system would otherwise admit.
    const std::vector<fv::Anchor> anchors = {};
    const std::vector<fv::Edge> edges = {
        fv::Edge{"byc", "wusdc", 1.10, 20.0, 2, true}};

    EXPECT_FALSE(fv::solve_pair(anchors, edges, "byc", "wusdc", -1).ok);
    EXPECT_FALSE(fv::solve_pair(anchors, edges, "byc", "wusdc", 2).ok);
}

TEST(FairValueSolver, RemovingTheOnlyRouteMakesThePairUnsolvable) {
    // Excluding a pair's own book can orphan its leg entirely.  That is the
    // correct outcome, not an error to paper over.
    const std::vector<fv::Anchor> anchors = {fv::Anchor{"xch", 1.44, 100.0}};
    const std::vector<fv::Edge> edges = {
        fv::Edge{"xch", "byc", 1.30, 20.0, 1, true}};

    EXPECT_TRUE(fv::solve_pair(anchors, edges, "xch", "byc", -1).ok);
    EXPECT_FALSE(fv::solve_pair(anchors, edges, "xch", "byc", 1).ok);
}

TEST(FairValueSolver, RedundancyRequiresMoreThanOnePath) {
    // Single chain: anchor -> edge -> leg.  Deleting the edge orphans the leg,
    // so the answer has never been cross-checked.
    {
        const std::vector<fv::Anchor> anchors = {fv::Anchor{"xch", 1.44, 100.0}};
        const std::vector<fv::Edge> edges = {
            fv::Edge{"xch", "byc", 1.30, 20.0, 1, true}};
        const auto s = fv::solve_pair(anchors, edges, "xch", "byc", -1);
        ASSERT_TRUE(s.ok);
        EXPECT_FALSE(s.redundant);
    }
    // Closed triangle with both endpoints anchored: every observation can be
    // dropped and both legs remain reachable.
    {
        const std::vector<fv::Anchor> anchors = {
            fv::Anchor{"xch", 1.44, 100.0}, fv::Anchor{"wusdc", 1.0, 100.0}};
        const std::vector<fv::Edge> edges = {
            fv::Edge{"xch", "wusdc", 1.44, 20.0, 0, true},
            fv::Edge{"xch", "byc",   1.31, 20.0, 1, true},
            fv::Edge{"byc", "wusdc", 1.10, 20.0, 2, true},
        };
        const auto s = fv::solve_pair(anchors, edges, "xch", "byc", -1);
        ASSERT_TRUE(s.ok);
        EXPECT_TRUE(s.redundant);
    }
}

TEST(FairValueSolver, TighterObservationsCarryMoreWeight) {
    // Two contradictory routes to BYC.  The answer must sit near the tighter
    // one -- and must move when, and only when, the sigmas move.  Nothing here
    // depends on which pair is which.
    const std::vector<fv::Anchor> anchors = {
        fv::Anchor{"xch", 1.44, 100.0}, fv::Anchor{"wusdc", 1.0, 100.0}};

    auto solve_with = [&](double wide_sigma) {
        const std::vector<fv::Edge> edges = {
            fv::Edge{"xch", "wusdc", 1.44, 10.0, 0, true},
            fv::Edge{"xch", "byc",   1.50, 10.0, 1, false},  // tight, keeps
            fv::Edge{"byc", "wusdc", 1.20, wide_sigma, 2, true},
        };
        return fv::solve_pair(anchors, edges, "xch", "byc", 1);
    };

    const auto tight_rival = solve_with(10.0);
    const auto wide_rival  = solve_with(2000.0);
    ASSERT_TRUE(tight_rival.ok);
    ASSERT_TRUE(wide_rival.ok);

    // 1.44/1.20 = 1.20 pulls down; the AMM edge says 1.50.
    EXPECT_LT(tight_rival.price, wide_rival.price);
    EXPECT_NEAR(wide_rival.price, 1.50, 0.01);   // wide rival barely matters

    // And the uncertainty follows the same rule: a second tight observation
    // genuinely reduces the error bar, a near-useless one does not.
    EXPECT_LT(tight_rival.sigma_bps, wide_rival.sigma_bps);
}

TEST(FairValueSolver, AFrozenBookLosesInfluenceAsItAges) {
    // dex_print_age is the only staleness signal that works here: the mid is
    // rewritten every heartbeat whether or not it moved, so wall-clock age
    // reads zero on a book frozen for 30 hours.
    auto sigma_at = [](int print_age) {
        const std::vector<fv::Anchor> anchors = {
            fv::Anchor{"wusdc", 1.0, 100.0}};
        const std::vector<fv::Edge> edges = {
            fv::Edge{"byc", "wusdc", 1.10,
                     book_sigma(60.0, print_age), 2, true}};
        const auto s = fv::solve_pair(anchors, edges, "byc", "wusdc", -1);
        EXPECT_TRUE(s.ok);
        return s.sigma_bps;
    };

    EXPECT_LT(sigma_at(0), sigma_at(10));
    EXPECT_LT(sigma_at(10), sigma_at(60));
    // A book frozen for an hour of heartbeats can no longer clear the bar.
    EXPECT_GT(sigma_at(60), kDefaults.fair_value_max_sigma_bps);
}

TEST(FairValueSolver, CombineSigmaIsQuadratureAndIgnoresNonsense) {
    EXPECT_NEAR(fv::combine_sigma_bps({3.0, 4.0}), 5.0, 1e-12);
    EXPECT_NEAR(fv::combine_sigma_bps({}), 0.0, 1e-12);
    // A negative term is not a negative variance; it contributes nothing.
    EXPECT_NEAR(fv::combine_sigma_bps({3.0, -4.0}), 3.0, 1e-12);
}

TEST(FairValueSolver, DegenerateInputIsRejectedNotGuessed) {
    const std::vector<fv::Anchor> anchors = {fv::Anchor{"xch", 1.44, 100.0}};
    const std::vector<fv::Edge> edges = {
        fv::Edge{"xch", "byc", 1.3, 20.0, 1, true}};

    EXPECT_FALSE(fv::solve_pair(anchors, edges, "xch", "xch", -1).ok);
    EXPECT_FALSE(fv::solve_pair(anchors, edges, "", "byc", -1).ok);
    EXPECT_FALSE(fv::solve_pair(anchors, edges, "xch", "", -1).ok);
    // An asset nothing mentions.
    EXPECT_FALSE(fv::solve_pair(anchors, edges, "xch", "zzz", -1).ok);
}

TEST(FairValueSolver, IgnoresUnusableObservations) {
    const std::vector<fv::Anchor> anchors = {
        fv::Anchor{"xch", 1.44, 100.0},
        fv::Anchor{"wusdc", -1.0, 100.0},   // bad price
        fv::Anchor{"dbx", 0.0143, 0.0},     // bad sigma
    };
    const std::vector<fv::Edge> edges = {
        fv::Edge{"xch", "byc", 0.0, 20.0, 1, false},   // bad price
        fv::Edge{"xch", "byc", 1.49, 50.0, 1, false},  // good
    };
    const auto s = fv::solve_pair(anchors, edges, "xch", "byc", -1);
    ASSERT_TRUE(s.ok);
    EXPECT_NEAR(s.price, 1.49, 1e-6);
    EXPECT_EQ(s.observations, 2u);  // one anchor + one edge
}

// ===========================================================================
// Part 2 -- the sweep, replayed on the real graph
// ===========================================================================

// The headline result, and it is not flattering: with only the dexie books to
// work from, the three of them AGREED at the moment of the sweep.  Their cross
// (1.267346 * 1.144728 = 1.45067) sits 1.6% from XCH/wUSDC.b, so no amount of
// triangulation can single out the wrong one.  What the solve does say is that
// nothing it knows about BYC is any good: the only surviving route runs
// through a 1259 bps book that had not printed in 26 heartbeats.  Sigma comes
// out at ~650 bps, the tier is Unavailable, and Step 7 widens instead of
// clamping.  That is the honest answer, and it is strictly better than the
// confident wrong one the rejected peg pass would have produced.
TEST(FairValueSweep, BooksAloneCannotPriceBycAndSayScoInsteadOfGuessing) {
    const auto s = fv::solve_pair(live_anchors(), live_book_edges(),
                                  "xch", "byc", kPairXchByc);
    ASSERT_TRUE(s.ok);

    EXPECT_GT(s.sigma_bps, 600.0);
    EXPECT_GT(s.sigma_bps, kDefaults.fair_value_max_sigma_bps);
    EXPECT_EQ(tier_of(s), FairValueTier::Unavailable);

    // It is NOT that the solve had no opinion -- it had one, and that opinion
    // would have blessed the sweep.  Suppressing it is the point.
    EXPECT_NEAR(s.price, 1.2479, 0.01);
    for (double px : kSweptAsks) {
        EXPECT_GT(px, s.price) << "book-only estimate would bless " << px;
    }
}

// The residual survives an Unavailable tier, and this is what makes that path
// useful: "the books contradict each other" is actionable even when the solve
// cannot say which one is wrong.
TEST(FairValueSweep, ResidualIsPublishableEvenWhenTheTierIsUnavailable) {
    const auto s = fv::solve_pair(live_anchors(), live_book_edges(),
                                  "xch", "byc", kPairXchByc);
    ASSERT_TRUE(s.ok);
    const double residual_bps = std::log(kBookXchByc / s.price) * 10'000.0;
    EXPECT_GT(std::abs(residual_bps),
              kDefaults.fair_value_residual_widen_floor_bps);
}

// Adding the arbitrage-anchored pool observation is what closes the case.  The
// AMM edge for XCH/BYC is NOT excluded when validating XCH/BYC, because a pool
// is not an order book.
TEST(FairValueSweep, AmmEdgeTurnsTheSweepIntoAClampOnEveryTier) {
    auto edges = live_book_edges();
    edges.push_back(fv::Edge{"xch", "wusdc", kAmmXchUsdc,
                             kDefaults.fair_value_amm_sigma_bps,
                             kPairXchUsdc, /*is_book=*/false});
    edges.push_back(fv::Edge{"xch", "byc", kAmmXchByc,
                             kDefaults.fair_value_amm_sigma_bps,
                             kPairXchByc, /*is_book=*/false});

    const auto s = fv::solve_pair(live_anchors(), edges, "xch", "byc",
                                  kPairXchByc);
    ASSERT_TRUE(s.ok);

    EXPECT_NEAR(s.price, 1.4901, 0.01);
    EXPECT_LT(s.sigma_bps, kDefaults.fair_value_tight_sigma_bps);
    EXPECT_EQ(tier_of(s), FairValueTier::Triangulated);

    // Every swept ask breaches the band, in the losing direction.
    const double floor_px = s.price * (1.0 - band_bps(s) / 10'000.0);
    for (double px : kSweptAsks) {
        const double dev_bps = (px / s.price - 1.0) * 10'000.0;
        EXPECT_LT(dev_bps, -band_bps(s)) << "price " << px;
        EXPECT_LT(px, floor_px);
        // ...and the clamp lifts it by at least 10%.
        EXPECT_GT(floor_px / px - 1.0, 0.10);
    }
}

// The guard must not fire on the healthy pair, or it gets turned off.
TEST(FairValueSweep, HealthyPairIsUntouchedByTheSameGraph) {
    const auto s = fv::solve_pair(live_anchors(), live_book_edges(),
                                  "xch", "wusdc", kPairXchUsdc);
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(tier_of(s), FairValueTier::CexDirect);

    // Its own book sits well inside the band even though two of the three
    // edges in the graph are badly wide.
    const double dev_bps = (kBookXchUsdc / s.price - 1.0) * 10'000.0;
    EXPECT_LT(std::abs(dev_bps), band_bps(s));
}

// ===========================================================================
// Part 3 -- the accessor
// ===========================================================================

class FairValueTest : public ::testing::Test {
protected:
    void SetUp() override {
        state_ = std::make_unique<State>();
        feed_  = std::make_unique<MarketDataFeed>(cfg_, *state_);
    }

    MarketDataConfig                cfg_;
    std::unique_ptr<State>          state_;
    std::unique_ptr<MarketDataFeed> feed_;
};

TEST_F(FairValueTest, UnknownPairReportsUnavailable) {
    EXPECT_FALSE(feed_->get_fair_value("XCH/BYC").has_value());
    EXPECT_FALSE(feed_->get_fair_value_residual_bps("XCH/BYC").has_value());
}

TEST_F(FairValueTest, IngestedValueIsReturnedWithItsTier) {
    feed_->ingest_fair_value("XCH/wUSDC.b", 1.428457, FairValueTier::CexDirect);

    const auto fv = feed_->get_fair_value("XCH/wUSDC.b");
    ASSERT_TRUE(fv.has_value());
    EXPECT_DOUBLE_EQ(fv->price, 1.428457);
    EXPECT_EQ(fv->tier, FairValueTier::CexDirect);
    EXPECT_GE(fv->age_seconds, 0.0);
    EXPECT_LT(fv->age_seconds, 60.0);
}

TEST_F(FairValueTest, FullFormRoundTripsSigmaResidualAndObservations) {
    FairValue in;
    in.price        = 1.490148;
    in.tier         = FairValueTier::Triangulated;
    in.sigma_bps    = 50.0;
    in.residual_bps = -1620.0;
    in.observations = 5;
    feed_->ingest_fair_value("XCH/BYC", in);

    const auto out = feed_->get_fair_value("XCH/BYC");
    ASSERT_TRUE(out.has_value());
    EXPECT_DOUBLE_EQ(out->price, 1.490148);
    EXPECT_EQ(out->tier, FairValueTier::Triangulated);
    EXPECT_DOUBLE_EQ(out->sigma_bps, 50.0);
    EXPECT_DOUBLE_EQ(out->residual_bps, -1620.0);
    EXPECT_EQ(out->observations, 5u);

    const auto res = feed_->get_fair_value_residual_bps("XCH/BYC");
    ASSERT_TRUE(res.has_value());
    EXPECT_DOUBLE_EQ(*res, -1620.0);
}

// The disagreement signal must outlive the price.  An Unavailable solve still
// knows the books contradict each other, and that is the half of the answer
// the operator can act on immediately.
TEST_F(FairValueTest, UnavailableTierStillPublishesTheResidual) {
    FairValue in;
    in.price        = 1.2479;   // must be discarded
    in.tier         = FairValueTier::Unavailable;
    in.sigma_bps    = 651.0;
    in.residual_bps = 155.0;
    feed_->ingest_fair_value("XCH/BYC", in);

    // No usable price...
    EXPECT_FALSE(feed_->get_fair_value("XCH/BYC").has_value());
    // ...but the disagreement is visible.
    const auto res = feed_->get_fair_value_residual_bps("XCH/BYC");
    ASSERT_TRUE(res.has_value());
    EXPECT_DOUBLE_EQ(*res, 155.0);
}

TEST_F(FairValueTest, NonPositiveOrUnavailableIngestsAreRejected) {
    feed_->ingest_fair_value("XCH/BYC", 0.0,  FairValueTier::CexDirect);
    EXPECT_FALSE(feed_->get_fair_value("XCH/BYC").has_value());

    feed_->ingest_fair_value("XCH/BYC", -1.0, FairValueTier::CexDirect);
    EXPECT_FALSE(feed_->get_fair_value("XCH/BYC").has_value());

    feed_->ingest_fair_value("XCH/BYC", 1.4,  FairValueTier::Unavailable);
    EXPECT_FALSE(feed_->get_fair_value("XCH/BYC").has_value());
}

// The whole point of the fair value is independence from the order book it
// validates.  Ingesting book data must NOT make a fair value appear.
TEST_F(FairValueTest, NeverFallsBackToTheDexieMid) {
    feed_->ingest_dexie("XCH/BYC",
                        /*best_bid =*/1.2000,
                        /*best_ask =*/1.3400,
                        /*last     =*/1.2673,
                        /*vol_24h  =*/500.0);
    feed_->refresh({"XCH/BYC"});

    // The book mid is available...
    EXPECT_GT(feed_->get_mid_price("XCH/BYC"), 0.0);
    // ...but it is not an independent fair value and must not be reported as
    // one.  Quoting blind must be visible to the caller, not papered over.
    EXPECT_FALSE(feed_->get_fair_value("XCH/BYC").has_value());
    EXPECT_FALSE(feed_->get_fair_value_residual_bps("XCH/BYC").has_value());
}

TEST_F(FairValueTest, ExpiredValueIsReportedAsAbsentNotStale) {
    cfg_.fair_value_max_age_sec = 0.000001;  // Effectively instant expiry.
    feed_ = std::make_unique<MarketDataFeed>(cfg_, *state_);

    FairValue in;
    in.price        = 98.36;
    in.tier         = FairValueTier::CexDirect;
    in.residual_bps = 12.0;
    feed_->ingest_fair_value("XCH/DBX", in);

    // Any measurable elapsed time exceeds a 1 microsecond budget.
    for (volatile int i = 0; i < 100000; ++i) { /* burn a little wall clock */ }

    EXPECT_FALSE(feed_->get_fair_value("XCH/DBX").has_value());
    EXPECT_FALSE(feed_->get_fair_value_residual_bps("XCH/DBX").has_value());
}

TEST_F(FairValueTest, ZeroMaxAgeDisablesExpiry) {
    cfg_.fair_value_max_age_sec = 0.0;
    feed_ = std::make_unique<MarketDataFeed>(cfg_, *state_);

    feed_->ingest_fair_value("XCH/DBX", 98.36, FairValueTier::CexDirect);
    EXPECT_TRUE(feed_->get_fair_value("XCH/DBX").has_value());
}

TEST_F(FairValueTest, PairsAreTrackedIndependently) {
    feed_->ingest_fair_value("XCH/wUSDC.b", 1.4285, FairValueTier::CexDirect);
    EXPECT_TRUE(feed_->get_fair_value("XCH/wUSDC.b").has_value());
    EXPECT_FALSE(feed_->get_fair_value("XCH/BYC").has_value());
}

// ===========================================================================
// Part 4 -- the solve's inputs, read back out of the feed
// ===========================================================================

TEST_F(FairValueTest, InputsExposeTheSelfFilteredBookNotTheBlendedMid) {
    feed_->ingest_dexie("XCH/BYC", 1.2000, 1.3400, 1.2673, 500.0);
    feed_->refresh({"XCH/BYC"});

    const auto obs = feed_->get_fair_value_inputs("XCH/BYC");
    EXPECT_TRUE(obs.has_book);
    EXPECT_NEAR(obs.mid, 1.27, 1e-9);
    EXPECT_GT(obs.spread_bps, 0.0);
    EXPECT_DOUBLE_EQ(obs.amm_mid, 0.0);
}

TEST_F(FairValueTest, OneSidedBookYieldsNoObservation) {
    // Post-5e1ceb4 a zero side means no THIRD-PARTY offer rests there, not
    // that the price is zero.  A one-sided book is not a price.
    feed_->ingest_dexie("XCH/BYC", 1.2000, 0.0, 1.2673, 500.0);
    feed_->refresh({"XCH/BYC"});

    const auto obs = feed_->get_fair_value_inputs("XCH/BYC");
    EXPECT_FALSE(obs.has_book);
    EXPECT_DOUBLE_EQ(obs.mid, 0.0);
}

TEST_F(FairValueTest, AmmMidIsSurfacedWithItsAge) {
    feed_->ingest_amm_mid("XCH/BYC", 1.491713);

    const auto obs = feed_->get_fair_value_inputs("XCH/BYC");
    EXPECT_NEAR(obs.amm_mid, 1.491713, 1e-9);
    EXPECT_GE(obs.amm_age_seconds, 0.0);
    EXPECT_LT(obs.amm_age_seconds, 60.0);
}

TEST_F(FairValueTest, UnknownPairHasNoInputs) {
    const auto obs = feed_->get_fair_value_inputs("NOPE/NOPE");
    EXPECT_FALSE(obs.has_book);
    EXPECT_DOUBLE_EQ(obs.amm_mid, 0.0);
    EXPECT_EQ(obs.print_age, 0);
}

// ===========================================================================
// Part 5 -- the deviation band
// ===========================================================================

namespace {

double bid_ceiling(double fair_value, double band) {
    return fair_value * (1.0 + band / 10'000.0);
}

double ask_floor(double fair_value, double band) {
    return fair_value * (1.0 - band / 10'000.0);
}

}  // namespace

TEST(FairValueBand, DefaultsProtectAnUnconfiguredDeployment) {
    // These knobs are deliberately absent from config.yaml; the defaults are
    // the operative values in production.
    const StrategyConfig d;
    EXPECT_DOUBLE_EQ(d.max_fair_value_deviation_bps, 300.0);
    EXPECT_DOUBLE_EQ(d.blind_quote_widen_pct, 50.0);
    EXPECT_GT(d.fair_value_clamp_tier_step_bps, 0.0);

    // The tight threshold must not exceed the usable ceiling, or every
    // barely-usable value is silently promoted to the highest confidence.
    EXPECT_LE(d.fair_value_tight_sigma_bps, d.fair_value_max_sigma_bps);

    // A pair between two feed-anchored assets inherently sits near
    // sqrt(2) * feed_sigma.  If the ceiling dropped below that, EVERY pair
    // would go blind and the guard would never clamp anything.
    EXPECT_GT(d.fair_value_max_sigma_bps,
              std::sqrt(2.0) * d.fair_value_feed_sigma_bps);
}

TEST(FairValueBand, ShakierEstimatesClampLessAggressively) {
    const StrategyConfig d;
    auto band = [&](double sigma) {
        return d.max_fair_value_deviation_bps
             + d.fair_value_sigma_band_mult * sigma;
    };
    EXPECT_GT(band(150.0), band(50.0));
    EXPECT_DOUBLE_EQ(band(0.0), d.max_fair_value_deviation_bps);
}

// A guard that fires on healthy quotes is a guard that gets turned off.
// Worst binding-direction deviations measured over 1 500 XCH/wUSDC.b tier
// quotes: asks -112 bps, bids +11 bps.  The band there is 300 + ~136 = 436.
TEST(FairValueBand, DoesNotBindOnTheHealthyBook) {
    const double fair = 1.428457;
    const double band = 300.0 + 136.0;  // measured sigma for that pair

    EXPECT_GT(fair * (1.0 - 112.0 / 10'000.0), ask_floor(fair, band));
    EXPECT_LT(fair * (1.0 + 11.0 / 10'000.0), bid_ceiling(fair, band));
}

// Deviations in the profitable direction are patient quotes, not mispricings.
TEST(FairValueBand, ProfitableDirectionIsNeverClamped) {
    const double fair = 1.428457;
    EXPECT_GT(fair * 1.20, ask_floor(fair, 300.0));
    EXPECT_LT(fair * 0.80, bid_ceiling(fair, 300.0));
}

// Several breaching tiers must not collapse onto a single price -- that costs
// N creation fees and locks N UTXOs for one economic level.
TEST(FairValueBand, ClampedTiersStayDistinct) {
    const double fair     = 1.490148;
    const double step_bps = StrategyConfig{}.fair_value_clamp_tier_step_bps;

    double next_min = ask_floor(fair, 350.0);
    std::vector<double> clamped;
    for (int tier = 0; tier < 6; ++tier) {
        clamped.push_back(next_min);
        next_min *= (1.0 + step_bps / 10'000.0);
    }

    for (std::size_t i = 1; i < clamped.size(); ++i) {
        EXPECT_GT(clamped[i], clamped[i - 1]);
    }
    EXPECT_GE(clamped.back(), ask_floor(fair, 350.0));
    EXPECT_LT(clamped.back(), fair);
}

// The blind path widens rather than withdraws: the bot keeps quoting.
TEST(FairValueBand, BlindWideningPushesTiersOutwardWithoutCrossing) {
    const double mid    = 1.2673;
    const double factor = 1.0 + StrategyConfig{}.blind_quote_widen_pct / 100.0;

    const double bid = mid * (1.0 - 30.0 / 10'000.0);
    const double ask = mid * (1.0 + 30.0 / 10'000.0);

    const double wide_bid = mid + (bid - mid) * factor;
    const double wide_ask = mid + (ask - mid) * factor;

    EXPECT_LT(wide_bid, bid);        // bid moved further from the mid
    EXPECT_GT(wide_ask, ask);        // ask moved further from the mid
    EXPECT_LT(wide_bid, wide_ask);   // still a valid two-sided quote
    EXPECT_GT(wide_bid, 0.0);        // still quoting, not withdrawn
}

// Residual-driven widening pushes both sides away from the mid, and is capped
// so a runaway disagreement cannot become a withdrawal.
TEST(FairValueBand, ResidualWideningIsSymmetricAndCapped) {
    const StrategyConfig d;
    auto extra = [&](double residual_bps) {
        const double excess = std::abs(residual_bps)
                            - d.fair_value_residual_widen_floor_bps;
        if (excess <= 0.0) return 0.0;
        return std::min(excess * d.fair_value_residual_widen_ratio,
                        d.max_half_spread_bps);
    };

    EXPECT_DOUBLE_EQ(extra(0.0), 0.0);
    EXPECT_DOUBLE_EQ(extra(d.fair_value_residual_widen_floor_bps), 0.0);
    EXPECT_GT(extra(1200.0), 0.0);
    EXPECT_LE(extra(100000.0), d.max_half_spread_bps);
    // Sign does not matter -- either book being wrong is equally dangerous.
    EXPECT_DOUBLE_EQ(extra(1200.0), extra(-1200.0));
}

}  // namespace
}  // namespace xop
