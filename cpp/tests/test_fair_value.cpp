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
#include "xop/strategy/liquidity.hpp"
#include "xop/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <thread>
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

// ...and the DEPTH behind those prices, which is what their weight is made of.
// Both sides of a constant-product pool hold equal value, so the pool is worth
// twice its XCH side.  These are dust: ~$500 and ~$1,125.
constexpr double kPoolXchBycUsd  = 2.0 * 175.4061 * kXchUsd;   // ~$501
constexpr double kPoolXchUsdcUsd = 2.0 * 393.8683 * kXchUsd;   // ~$1,125

/// Ground truth for BYC at the block of the sweep, established from five
/// independent sources (TibetSwap reserves 0.961, dexie v2 tickers 1.011,
/// dexie 7-day traded VWAP 1.001, executable bid depth 1.000-1.014,
/// Spacescan 0.942).  Used only to measure how wrong each behaviour is.
constexpr double kBycTrueUsd    = 1.01;
constexpr double kTrueXchByc    = kXchUsd / kBycTrueUsd;   // ~1.41431

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

/// AMM edge sigma exactly as Engine::update_fair_values builds it: derived
/// from pool depth, never a constant.
double amm_sigma(double pool_usd) {
    return fv::amm_sigma_bps(pool_usd,
                             kDefaults.fair_value_amm_sigma_bps,
                             kDefaults.fair_value_amm_depth_k_bps);
}

/// The three books plus the two pool observations, weighted by their depth.
/// This is the graph the engine actually assembles today.
std::vector<fv::Edge> live_edges_with_amm() {
    auto edges = live_book_edges();
    edges.push_back(fv::Edge{"xch", "wusdc", kAmmXchUsdc,
                             amm_sigma(kPoolXchUsdcUsd),
                             kPairXchUsdc, /*is_book=*/false});
    edges.push_back(fv::Edge{"xch", "byc", kAmmXchByc,
                             amm_sigma(kPoolXchBycUsd),
                             kPairXchByc, /*is_book=*/false});
    return edges;
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

TEST(FairValueSolver, TwoAnchorsAloneSolveAnEdgelessPair) {
    // [PARANCHOR] The BYC shape: no book edge survives own-book exclusion
    // and the AMM is down, but BOTH legs carry an external anchor -- xch
    // via CoinGecko (100bps) and byc via its declared par (150bps).  The
    // solve must succeed as both_anchored at the anchor ratio, and the
    // combined sigma must clear the 200bps usability ceiling -- this
    // inequality is the entire reason the par sigma defaults are 100/140.
    const std::vector<fv::Anchor> anchors = {
        fv::Anchor{"xch", 1.43, 100.0}, fv::Anchor{"byc", 1.0, 140.0}};

    const auto s = fv::solve_pair(anchors, {}, "xch", "byc", -1);
    ASSERT_TRUE(s.ok);
    EXPECT_TRUE(s.both_anchored);
    EXPECT_NEAR(s.price, 1.43, 1e-9);
    EXPECT_NEAR(s.sigma_bps, std::sqrt(100.0 * 100.0 + 140.0 * 140.0), 1.0);
    EXPECT_LT(s.sigma_bps, 200.0);
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

// -- WHAT THIS TEST USED TO ASSERT, AND WHY IT NO LONGER DOES ---------------
//
// It was called AmmEdgeTurnsTheSweepIntoAClampOnEveryTier, and it fed both
// pools into the solve at a FLAT 50 bps.  That produced fair = 1.4901,
// sigma = 50 bps, tier Triangulated, and a clamp that lifted all six swept
// asks by 10.8%-13.2%.  It passed, and it was measuring the wrong thing.
//
// A 50 bps sigma is a claim that the pool is accurate to half a percent.  The
// BYC pool implied 1.4917 at that very block while BYC was worth ~$1.01, i.e.
// XCH/BYC ~= 1.4143 -- the pool was wrong by 547 bps, eleven times the error
// it was declaring.  Weighted at 50 bps it carried 1/(50e-4)^2 = 4.0e4 against
// 242 for the competing book: a 166:1 majority for $500 of liquidity.  The
// "clamp on every tier" was that one number talking to itself.
//
// Depth-weighted, the same pool declares 670 bps -- the order of its actual
// error -- and the honest consequence is that XCH/BYC becomes UNPRICEABLE, not
// clamped.  That is the finding, so that is what this test now locks in.
TEST(FairValueSweep, DustPoolCannotMakeBycPriceableAndMustNotPretendTo) {
    const auto s = fv::solve_pair(live_anchors(), live_edges_with_amm(),
                                  "xch", "byc", kPairXchByc);
    ASSERT_TRUE(s.ok);

    // The estimate moves toward the truth (1.4143) and away from the book
    // (1.2673) -- the pool is real information and is not being discarded.
    EXPECT_NEAR(s.price, 1.3608, 0.01);
    EXPECT_GT(s.price, kBookXchByc);
    EXPECT_LT(s.price, kAmmXchByc);

    // ...but it arrives with an error bar that forbids acting on it.
    EXPECT_NEAR(s.sigma_bps, 466.7, 5.0);
    EXPECT_GT(s.sigma_bps, kDefaults.fair_value_max_sigma_bps);
    EXPECT_EQ(tier_of(s), FairValueTier::Unavailable);

    // NO ASK IS CLAMPED UP ANY MORE.  Every swept price sits inside the band,
    // so on a $500 pool's authority the guard says nothing at all.
    const double floor_px = s.price * (1.0 - band_bps(s) / 10'000.0);
    for (double px : kSweptAsks) {
        const double dev_bps = (px / s.price - 1.0) * 10'000.0;
        EXPECT_GT(dev_bps, -band_bps(s)) << "price " << px;
        EXPECT_GT(px, floor_px) << "price " << px;
    }
}

// The corollary, stated plainly so it cannot be lost: the sweep is NO LONGER
// caught by the clamp.  Step 7 falls to the blind path (widen 50%) plus the
// consistency-residual widening, and the widened asks are still 6.5%-10.0%
// below what BYC was actually worth.
//
// This is not a regression in the guard -- it is the guard declining to
// launder a $500 pool into a confident number.  Nothing in the system
// genuinely knows what BYC is worth: its only two witnesses are that pool and
// a 1259 bps book frozen for 26 heartbeats.  Closing the gap needs a real
// observation (a deeper pool, a CEX listing, an executable-depth mark), not a
// smaller error bar on the one we have.
TEST(FairValueSweep, SweepIsNotClampedAnyMoreAndTheGapIsQuantified) {
    const auto s = fv::solve_pair(live_anchors(), live_edges_with_amm(),
                                  "xch", "byc", kPairXchByc);
    ASSERT_TRUE(s.ok);
    ASSERT_EQ(tier_of(s), FairValueTier::Unavailable);

    // Step 7's Unavailable response, in order: consistency-residual widening
    // (capped at XCH/BYC's own 75 bps half-spread override in config.yaml),
    // then blind widening of every tier to 1.5x its distance from the mid.
    const double residual_bps = std::log(kBookXchByc / s.price) * 10'000.0;
    ASSERT_LT(residual_bps, -kDefaults.fair_value_residual_widen_floor_bps);

    const double excess = std::abs(residual_bps)
                        - kDefaults.fair_value_residual_widen_floor_bps;
    const double widen_bps = std::min(
        excess * kDefaults.fair_value_residual_widen_ratio, 75.0);
    EXPECT_NEAR(widen_bps, 75.0, 1e-9);  // the cap binds

    const double blind = 1.0 + kDefaults.blind_quote_widen_pct / 100.0;

    double worst_gap_pct = 0.0;
    for (double px : kSweptAsks) {
        const double after_residual = px * (1.0 + widen_bps / 10'000.0);
        const double posted = kBookXchByc
                            + (after_residual - kBookXchByc) * blind;
        // Still below the truth: the taker's free money is reduced, not removed.
        EXPECT_LT(posted, kTrueXchByc) << "ask " << px;
        worst_gap_pct = std::max(worst_gap_pct,
                                 (kTrueXchByc / posted - 1.0) * 100.0);
    }

    // Was ~11.3% unmitigated at the sweep; the widening takes the worst tier
    // to ~10.0%.  Recorded so any future improvement has a baseline.
    EXPECT_NEAR(worst_gap_pct, 10.0, 0.5);
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

// Requirement (b): re-weighting the AMM must not disturb the pair that pays
// for everything.  XCH/wUSDC.b has a live two-sided book, a direct feed anchor
// on both legs, and 0.0% staleness, so the pool was never carrying it -- the
// depth re-weighting should be nearly invisible here, and it is.
TEST(FairValueSweep, HealthyPairIsUnaffectedByDepthWeightingTheAmm) {
    const auto s = fv::solve_pair(live_anchors(), live_edges_with_amm(),
                                  "xch", "wusdc", kPairXchUsdc);
    ASSERT_TRUE(s.ok);

    // Flat-50 gave 1.434836 / 47.0 bps; depth-weighted gives 1.433518 /
    // 133.2 bps.  The PRICE moves 9 bps -- nothing -- because the tight book
    // and the two anchors already determined it.  Sigma rises honestly, since
    // a $1,125 pool was never worth 50 bps.
    EXPECT_NEAR(s.price, 1.4335, 0.005);
    EXPECT_NEAR(std::log(s.price / 1.434836) * 10'000.0, 0.0, 20.0);

    // The tier the pair trades under is unchanged, so nothing about its
    // quoting behaviour changes.
    EXPECT_EQ(tier_of(s), FairValueTier::CexDirect);

    // And its own book still sits comfortably inside the band: no clamp, on a
    // pair where a clamp would be a false positive.
    const double dev_bps = (kBookXchUsdc / s.price - 1.0) * 10'000.0;
    EXPECT_LT(std::abs(dev_bps), band_bps(s));
    EXPECT_LT(std::abs(dev_bps), 100.0);
}

// ===========================================================================
// Part 2b -- the AMM's weight comes from its depth
// ===========================================================================

TEST(AmmSigma, ScalesAsInverseSqrtOfPoolDepth) {
    const double floor_bps = 50.0;
    const double k         = 15'000.0;

    // Quadrupling the pool halves the sigma, which doubles its weight per
    // dollar of depth -- i.e. weight is linear in pool size, the property the
    // 1/sqrt form exists to produce.
    const double a = fv::amm_sigma_bps(10'000.0, floor_bps, k);
    const double b = fv::amm_sigma_bps(40'000.0, floor_bps, k);
    EXPECT_NEAR(a, 150.0, 1e-9);
    EXPECT_NEAR(b, 75.0, 1e-9);
    EXPECT_NEAR(a / b, 2.0, 1e-9);
}

TEST(AmmSigma, TheLivePoolsLandBesideTheBooksTheyCompeteWith) {
    // The calibration claim, checked against the real reserves.
    const double byc  = amm_sigma(kPoolXchBycUsd);
    const double usdc = amm_sigma(kPoolXchUsdcUsd);

    EXPECT_NEAR(kPoolXchBycUsd,   501.0,  2.0);
    EXPECT_NEAR(kPoolXchUsdcUsd, 1125.0,  2.0);

    EXPECT_NEAR(byc,  670.0, 2.0);
    EXPECT_NEAR(usdc, 447.0, 2.0);

    // The whole point: the $500 pool no longer outvotes the book it competes
    // with.  It used to carry 166x that book's weight; now it carries 0.92x.
    const double book_byc_usdc = book_sigma(kBookBycUsdcSpr, 26);
    EXPECT_GT(byc, book_byc_usdc);

    auto weight = [](double sigma_bps) {
        const double s = sigma_bps / 10'000.0;
        return 1.0 / (s * s);
    };
    EXPECT_NEAR(weight(byc) / weight(book_byc_usdc), 0.92, 0.02);
    EXPECT_NEAR(weight(50.0) / weight(book_byc_usdc), 165.6, 1.0);
}

TEST(AmmSigma, ADeepPoolApproachesTheFloorAndNeverBeatsIt) {
    const double floor_bps = kDefaults.fair_value_amm_sigma_bps;
    const double k         = kDefaults.fair_value_amm_depth_k_bps;

    // 100x the live BYC pool: ~$50,100 -> ~67 bps, closing on the floor.
    EXPECT_NEAR(fv::amm_sigma_bps(100.0 * kPoolXchBycUsd, floor_bps, k),
                67.0, 1.0);
    // Deeper still: pinned at the floor, never below it.  A pool cannot claim
    // more certainty than the swap fee and arbitrage band allow.
    EXPECT_DOUBLE_EQ(fv::amm_sigma_bps(90'000.0, floor_bps, k), 50.0);
    EXPECT_DOUBLE_EQ(fv::amm_sigma_bps(1e12, floor_bps, k), floor_bps);
}

TEST(AmmSigma, UnknownDepthIsUnusableRatherThanOptimistic) {
    // 0.0 means "do not use this observation".  Substituting a cheerful
    // constant for a depth we could not measure is the defect being fixed, so
    // the failure mode is refusal, not a default.
    EXPECT_DOUBLE_EQ(fv::amm_sigma_bps(0.0, 50.0, 15'000.0), 0.0);
    EXPECT_DOUBLE_EQ(fv::amm_sigma_bps(-1.0, 50.0, 15'000.0), 0.0);
    EXPECT_DOUBLE_EQ(
        fv::amm_sigma_bps(std::numeric_limits<double>::quiet_NaN(),
                          50.0, 15'000.0), 0.0);
    // A non-positive k disables the derivation entirely.
    EXPECT_DOUBLE_EQ(fv::amm_sigma_bps(500.0, 50.0, 0.0), 0.0);
}

// A pool small enough carries so little weight that the solve is left where
// the books alone put it.  No pair is named: this is a property of depth.
TEST(AmmSigma, ADustPoolBarelyMovesTheAnswer) {
    auto edges = live_book_edges();
    edges.push_back(fv::Edge{"xch", "byc", kAmmXchByc,
                             amm_sigma(/*pool_usd=*/10.0),
                             kPairXchByc, /*is_book=*/false});

    const auto with_dust = fv::solve_pair(live_anchors(), edges,
                                          "xch", "byc", kPairXchByc);
    const auto books_only = fv::solve_pair(live_anchors(), live_book_edges(),
                                           "xch", "byc", kPairXchByc);
    ASSERT_TRUE(with_dust.ok);
    ASSERT_TRUE(books_only.ok);

    // A $10 pool shifts the answer by well under 1%.
    EXPECT_NEAR(with_dust.price, books_only.price, books_only.price * 0.01);
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
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

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

TEST_F(FairValueTest, AmmMidIsSurfacedWithItsAgeAndItsDepth) {
    feed_->ingest_amm_mid("XCH/BYC", 1.491713, kPoolXchBycUsd);

    const auto obs = feed_->get_fair_value_inputs("XCH/BYC");
    EXPECT_NEAR(obs.amm_mid, 1.491713, 1e-9);
    EXPECT_NEAR(obs.amm_pool_usd, kPoolXchBycUsd, 1e-6);
    EXPECT_GE(obs.amm_age_seconds, 0.0);
    EXPECT_LT(obs.amm_age_seconds, 60.0);
}

// ---------------------------------------------------------------------------
// AMM staleness must be honest, or every freshness gate is decoration.
//
// The engine used to re-read the reserves CACHE every heartbeat and stamp the
// ingest time as the observation time.  A pool fetched once and never again
// therefore reported an age of ~0 seconds forever, and no gate keyed on
// amm_age_seconds could ever fire.  ingest_amm_mid now stores the caller's
// observation time verbatim.
// ---------------------------------------------------------------------------
TEST_F(FairValueTest, AmmAgeReflectsTheFetchTimeNotTheIngestTime) {
    const auto ten_minutes_ago =
        std::chrono::system_clock::now() - std::chrono::minutes(10);
    feed_->ingest_amm_mid("XCH/BYC", 1.491713, kPoolXchBycUsd,
                          ten_minutes_ago);

    const auto obs = feed_->get_fair_value_inputs("XCH/BYC");
    // ~600 s, NOT ~0 s.  Under the old behaviour this read as brand new.
    EXPECT_NEAR(obs.amm_age_seconds, 600.0, 5.0);
}

// The gate the honest age makes reachable: an AMM sample older than
// fair_value_amm_max_age_sec must stop contributing an edge.  Constructed
// here as the engine evaluates it, so the condition is proven to fire.
TEST_F(FairValueTest, StaleAmmSampleIsDroppedByTheFreshnessGate) {
    const double max_age = kDefaults.fair_value_amm_max_age_sec;
    ASSERT_GT(max_age, 0.0);

    auto gate_admits = [&](std::chrono::seconds age) {
        feed_ = std::make_unique<MarketDataFeed>(cfg_, *state_);
        feed_->ingest_amm_mid("XCH/BYC", 1.491713, kPoolXchBycUsd,
                              std::chrono::system_clock::now() - age);
        const auto obs = feed_->get_fair_value_inputs("XCH/BYC");
        const double sigma = amm_sigma(obs.amm_pool_usd);
        return obs.amm_mid > 0.0 && sigma > 0.0
            && obs.amm_age_seconds <= max_age;
    };

    // Fresh -> contributes.
    EXPECT_TRUE(gate_admits(std::chrono::seconds(1)));
    // Older than the budget -> dropped.  THIS IS THE BRANCH THAT WAS DEAD.
    EXPECT_FALSE(gate_admits(
        std::chrono::seconds(static_cast<long long>(max_age) + 60)));
}

// A sample whose pool depth we could not measure is not published as a
// weightable observation.
TEST_F(FairValueTest, AmmSampleWithUnknownDepthCarriesNoUsableWeight) {
    feed_->ingest_amm_mid("XCH/BYC", 1.491713, /*pool_usd=*/0.0);

    const auto obs = feed_->get_fair_value_inputs("XCH/BYC");
    EXPECT_DOUBLE_EQ(obs.amm_pool_usd, 0.0);
    // The engine's edge-construction guard: sigma 0 means "no edge".
    EXPECT_DOUBLE_EQ(amm_sigma(obs.amm_pool_usd), 0.0);
}

// ---------------------------------------------------------------------------
// FIX 1, locked in: the AMM must not be able to move the mid it validates.
// ---------------------------------------------------------------------------
TEST(FairValueBlend, AmmIsKeptOutOfTheCompositeMidByDefault) {
    // The knob is absent from config.yaml, so the DEFAULT is the operative
    // production value.  Both layers must agree, and both must be zero: the
    // same TibetSwap sample feeds the fair-value solve, and a reference that
    // sets the ladder centre cannot also be the thing that checks it.
    EXPECT_DOUBLE_EQ(MarketDataConfig{}.amm_blend_weight, 0.0);
    EXPECT_DOUBLE_EQ(StrategyConfig{}.amm_blend_weight, 0.0);
}

TEST_F(FairValueTest, AmmMidDoesNotMoveTheCompositeMid) {
    feed_->ingest_dexie("XCH/BYC", 1.2000, 1.3400, 1.2673, 500.0);
    feed_->refresh({"XCH/BYC"});
    const double mid_without_amm = feed_->get_mid_price("XCH/BYC");
    ASSERT_GT(mid_without_amm, 0.0);

    // A pool 17% away from the book -- the live BYC dislocation -- must leave
    // the mid exactly where it was.
    feed_->ingest_amm_mid("XCH/BYC", 1.491713, kPoolXchBycUsd);
    feed_->refresh({"XCH/BYC"});

    EXPECT_DOUBLE_EQ(feed_->get_mid_price("XCH/BYC"), mid_without_amm);

    // ...while still being available to the solve as an independent input.
    EXPECT_NEAR(feed_->get_fair_value_inputs("XCH/BYC").amm_mid,
                1.491713, 1e-9);
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

// ===========================================================================
// Part 6 -- the corrected order-book mid, end to end through MarketDataFeed
//
// The BYC/wUSDC.b edge above carries mid 1.144728, which is the number the OLD
// micro-price published at block 9087661.  Reconstructing the book behind it:
// the mid sat exactly on the best ask (the old code's debug-level cap binding),
// and snapshots.spread_bps = 1258.964 fixes the ratio of the two sides, so
//
//     ask = 1.144728,  bid = ask * (1 - s/2) / (1 + s/2) = 1.009145
//     plain midpoint   = 1.076937
//
// The implied best bid of 1.009 is corroborated independently: the recorded
// executable bid depth on that book was 1.000-1.014.  At 1259 bps the new
// schedule gives w_micro = 0, so the corrected mid IS that plain midpoint.
//
// These tests drive the REAL ingest path rather than editing a fixture,
// because the question the fix has to answer -- does the corrected mid feed
// through to the fair value -- is a question about the wiring, and a fixture
// cannot answer it.
// ===========================================================================

constexpr double kBycBookBid      = 1.009145;
constexpr double kBycBookAsk      = 1.144728;
constexpr double kBycFixedMid     = (kBycBookBid + kBycBookAsk) / 2.0;
constexpr double kBycOldPublished = 1.144727818835;

/// The recorded shape of that book: ~65 deep bids against ~9 thin asks.
std::vector<CompetingOffer> byc_book_at_the_sweep() {
    auto mk = [](Side side, double px, double sz) {
        CompetingOffer o;
        o.offer_id = std::to_string(px) + (side == Side::Bid ? "b" : "a")
                     + std::to_string(sz);
        o.side  = side;
        o.price = static_cast<Mojo>(px * static_cast<double>(kMojosPerXch));
        o.size  = static_cast<Mojo>(sz * static_cast<double>(kMojosPerXch));
        return o;
    };
    std::vector<CompetingOffer> book;
    for (int i = 0; i < 65; ++i) {
        book.push_back(mk(Side::Bid, kBycBookBid * (1.0 - 0.002 * i), 500.0));
    }
    for (int i = 0; i < 9; ++i) {
        book.push_back(mk(Side::Ask, kBycBookAsk * (1.0 + 0.02 * i), 5.0));
    }
    return book;
}

// (d), part 1: the published mid is corrected, all the way through the feed.
TEST_F(FairValueTest, CorrectedMidMovesTheBycMarkOffItsOwnBestAsk) {
    feed_->ingest_competing_offers("BYC/wUSDC.b", byc_book_at_the_sweep(), {});

    feed_->refresh({"BYC/wUSDC.b"});

    const auto bbo = feed_->get_dex_bbo("BYC/wUSDC.b");
    EXPECT_NEAR(bbo.first,  kBycBookBid, 1e-4);
    EXPECT_NEAR(bbo.second, kBycBookAsk, 1e-4);

    const double mid = feed_->get_mid_price("BYC/wUSDC.b");

    // 1259 bps of spread -> w_micro = 0 -> the plain midpoint.
    EXPECT_NEAR(mid, kBycFixedMid, 1e-4);

    // A long way below what was actually published, and no longer sitting on
    // the best ask.
    EXPECT_LT(mid, kBycOldPublished);
    EXPECT_LT(mid, kBycBookAsk);
    EXPECT_GT(mid, kBycBookBid);
    EXPECT_GT((kBycOldPublished / mid - 1.0) * 10'000.0, 550.0);
}

// (d), part 2: THE ANSWER TO THE FAIR-VALUE QUESTION, and it is a negative.
//
// The corrected mid does NOT feed the fair-value solve, because the solve was
// never fed the order-book mid in the first place: get_fair_value_inputs()
// publishes the plain BBO midpoint (see
// FairValueTest.InputsExposeTheSelfFilteredBookNotTheBlendedMid above).  That
// insulation is deliberate and predates this change.
//
// So the solve sees the same number before and after, and XCH/BYC's sigma and
// tier cannot move.  Asserted rather than assumed, because "the fix probably
// helps the fair value too" is exactly the kind of unearned claim that turns
// into a false sense of coverage later.
TEST_F(FairValueTest, TheFairValueSolveIsInsulatedFromTheOrderBookMid) {
    feed_->ingest_competing_offers("BYC/wUSDC.b", byc_book_at_the_sweep(), {});

    const auto obs = feed_->get_fair_value_inputs("BYC/wUSDC.b");
    ASSERT_TRUE(obs.has_book);

    // The edge fed to the solve is the BBO midpoint.  It is what it was under
    // the old code too, when the PUBLISHED mid was 1.144728 -- the two numbers
    // were already different objects.
    EXPECT_NEAR(obs.mid, kBycFixedMid, 1e-4);
    EXPECT_NEAR(obs.spread_bps, 1258.96, 5.0);

    // Therefore XCH/BYC's solve is what it was: still Unavailable, still on
    // the widen-don't-clamp path.
    const auto s = fv::solve_pair(live_anchors(), live_edges_with_amm(),
                                  "xch", "byc", kPairXchByc);
    ASSERT_TRUE(s.ok);
    EXPECT_GT(s.sigma_bps, kDefaults.fair_value_max_sigma_bps);
    EXPECT_EQ(tier_of(s), FairValueTier::Unavailable);
}

// (d), part 3: sigma is not a function of the mid, stated as arithmetic.
//
// The book edge's sigma is max(|spread|/2, floor) combined in quadrature with
// the print-age and depth terms.  WHERE INSIDE THE SPREAD THE MID SITS IS NOT
// AN INPUT.  No mid correction of any size can move it, so no mid correction
// can lift XCH/BYC under the 200 bps ceiling.  Its book has to narrow, its
// print has to move, or its pool has to deepen.
TEST(CorrectedMid, SigmaIsIndependentOfWhereTheMidSitsInTheBook) {
    // Only width, staleness and depth are inputs.
    EXPECT_LT(book_sigma(100.0, 0), book_sigma(1259.0, 0));
    EXPECT_LT(book_sigma(1259.0, 0), book_sigma(1259.0, 26));
    EXPECT_LT(book_sigma(1259.0, 0, 9.0), book_sigma(1259.0, 0, 1.0));

    // And at BYC/wUSDC.b's actual width it is already over the ceiling before
    // staleness is even counted.
    EXPECT_GT(book_sigma(kBookBycUsdcSpr, 0),
              kDefaults.fair_value_max_sigma_bps);
}

// (d), part 4: the healthy pair's solve is untouched -- its edge is the BBO
// midpoint and that is not what this change moves.
TEST(CorrectedMid, HealthyPairSolveStaysCexDirect) {
    const auto s = fv::solve_pair(live_anchors(), live_edges_with_amm(),
                                  "xch", "wusdc", kPairXchUsdc);
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(tier_of(s), FairValueTier::CexDirect);
    EXPECT_LE(s.sigma_bps, kDefaults.fair_value_tight_sigma_bps);
}

// (c): what re-centring on the corrected mark is worth, for the pair whose
// ladder IS centred on it.
//
// BYC/wUSDC.b's own ladder was centred on 1.144728 -- 13.3% above a BYC worth
// ~$1.01 -- so it was marking and quoting the asset a seventh too rich.
// Re-centred on 1.076937 that overstatement is more than halved.
//
// Note carefully what this does NOT claim.  The six XCH/BYC ask tiers swept at
// 10:31 were centred on XCH/BYC's OWN book mid of 1.267346, which is already
// the exact plain midpoint of its 1.133359 / 1.401333 book (lambda = 0.5000,
// measured from the same-block join) -- the old code's "deviates >10% from the
// BBO midpoint, clamp to it" guard had fired there and landed on the same
// value this schedule reaches.  So those six tiers do not move.  What this
// removes is the upstream BYC mark that made a 1.27 cross look consistent.
TEST(CorrectedMid, BycOwnLadderCentreMovesMostOfTheWayToTheTruth) {
    const double err_before = (kBycOldPublished / kBycTrueUsd - 1.0) * 100.0;
    const double err_after  = (kBycFixedMid     / kBycTrueUsd - 1.0) * 100.0;

    EXPECT_NEAR(err_before, 13.34, 0.05);   // +13.3% over the truth
    EXPECT_NEAR(err_after,   6.63, 0.05);   // +6.6%
    EXPECT_LT(err_after, err_before / 2.0);

    // What remains is the plain midpoint of a genuinely 1259 bps book, which
    // is the honest answer at that width rather than an artifact.  The bid
    // side of that same book is within 0.1% of the truth.
    EXPECT_NEAR(kBycBookBid, kBycTrueUsd, 0.002);
}

// ===========================================================================
// Part 6 -- uncertainty-scaled quoting (phase 3)
//
// The binary usable/Unavailable cliff treated the solve's sigma as a validity
// flag.  It is a WIDTH INSTRUCTION.  The ladder centre is now an inverse-
// variance blend of the pair's own mid and the external estimate, and the
// ladder's minimum half-spread is k_sigma * combined_sigma (floored by the
// existing margins).  These tests replay the design's acceptance cases with
// the real numbers, arithmetic shown.
// ===========================================================================

/// The engine's minimum half-spread, exactly as Step 7 computes it.
double min_half_spread_bps(double combined_sigma_bps) {
    return std::max(
        std::max(kDefaults.min_profit_margin_bps,
                 ArbitrageSettings{}.tibetswap_fee_bps),
        kDefaults.quote_width_sigma_mult * combined_sigma_bps);
}

/// Build a default ladder around `center` with the spacing schedule shifted
/// outward so the innermost tier honours the floor -- exactly the Step 7
/// pre-generation shift.
std::vector<TierQuote> ladder_with_floor(double center, double floor_bps) {
    LiquidityConfig cfg;                     // default 4 tiers [60,200,500,1000]
    if (floor_bps > cfg.tier_spacing_bps.front()) {
        const double delta = floor_bps - cfg.tier_spacing_bps.front();
        for (double& s : cfg.tier_spacing_bps) s += delta;
    }
    LiquidityEngine liq("TEST/PAIR", cfg);
    const auto mid = static_cast<std::int64_t>(
        std::llround(center * 1e12));
    return liq.compute_ladder(mid, /*sigma=*/0.05, /*inventory_ratio=*/0.5,
                              /*available_capital=*/1'000'000'000'000,
                              /*available_inventory=*/1'000'000'000'000, cfg);
}

TEST(QuoteCenterBlend, RefusesWhenNeitherSideIsUsable) {
    EXPECT_FALSE(fv::blend_quote_center(0.0, 0.0, 0.0, 0.0).ok);
    EXPECT_FALSE(fv::blend_quote_center(-1.0, 50.0, 0.0, 100.0).ok);
    EXPECT_FALSE(fv::blend_quote_center(1.0, 0.0, 1.0, 0.0).ok);  // no sigmas
}

TEST(QuoteCenterBlend, OneSidedInputDegradesToThatSideVerbatim) {
    // No external anchor: the pair quotes around its own mid with its own
    // width -- it does NOT go silent.  This is the general low-certainty-
    // market behaviour the owner's directive requires.
    const auto book_only = fv::blend_quote_center(1.2673, 1057.0, 0.0, 0.0);
    ASSERT_TRUE(book_only.ok);
    EXPECT_DOUBLE_EQ(book_only.center, 1.2673);
    EXPECT_DOUBLE_EQ(book_only.sigma_bps, 1057.0);
    EXPECT_DOUBLE_EQ(book_only.w_external, 0.0);

    // No two-sided book: the external estimate prices the pair alone.
    const auto ext_only = fv::blend_quote_center(0.0, 0.0, 1.3608, 466.7);
    ASSERT_TRUE(ext_only.ok);
    EXPECT_DOUBLE_EQ(ext_only.center, 1.3608);
    EXPECT_DOUBLE_EQ(ext_only.sigma_bps, 466.7);
    EXPECT_DOUBLE_EQ(ext_only.w_external, 1.0);
}

TEST(QuoteCenterBlend, EqualSigmasGiveTheGeometricMeanAndShrinkSigma) {
    // Two equally-uncertain estimates: the blend is their log-space midpoint
    // (geometric mean) and the combined sigma is sigma/sqrt(2) -- two honest
    // witnesses know more than one.
    const auto b = fv::blend_quote_center(1.0, 200.0, 4.0, 200.0);
    ASSERT_TRUE(b.ok);
    EXPECT_NEAR(b.center, 2.0, 1e-9);
    EXPECT_NEAR(b.sigma_bps, 200.0 / std::sqrt(2.0), 1e-9);
    EXPECT_NEAR(b.w_external, 0.5, 1e-12);
}

TEST(QuoteCenterBlend, IsSymmetricUnderPairInversion) {
    // Log space is not a convenience here either: inverting both inputs must
    // exactly invert the answer, so the blend cannot depend on which leg the
    // pair is named after.
    const auto fwd = fv::blend_quote_center(1.2673, 1057.0, 1.3608, 466.7);
    const auto rev = fv::blend_quote_center(1.0 / 1.2673, 1057.0,
                                            1.0 / 1.3608, 466.7);
    ASSERT_TRUE(fwd.ok);
    ASSERT_TRUE(rev.ok);
    EXPECT_NEAR(fwd.center * rev.center, 1.0, 1e-12);
    EXPECT_NEAR(fwd.sigma_bps, rev.sigma_bps, 1e-9);
}

TEST(QuoteCenterBlend, TheTighterWitnessDominates) {
    const auto b = fv::blend_quote_center(1.0, 100.0, 1.1, 1000.0);
    ASSERT_TRUE(b.ok);
    EXPECT_LT(b.w_external, 0.02);          // 1:100 variance ratio
    EXPECT_NEAR(b.center, 1.0, 0.002);
    EXPECT_LT(b.sigma_bps, 100.0);          // never worse than the best input
}

// ---------------------------------------------------------------------------
// ACCEPTANCE (A): sweep replay at block 9087661 conditions.
//
// Book 1.133359 / 1.401333 (mid 1.267346, spread 2114.5 bps), external solve
// estimate ~1.3608 at sigma ~466.7 bps (the real solve over the real graph,
// re-derived here rather than hardcoded).  Arithmetic:
//
//   book sigma   = sqrt(1057.23^2 + 30^2)                       = 1057.65 bps
//   w_external   = 1057.65^2 / (1057.65^2 + 466.7^2)            = 0.837
//   centre       = exp(0.837*ln(1.3608) + 0.163*ln(1.267346))   ~ 1.3451
//   combined     = 1057.65*466.7 / sqrt(1057.65^2 + 466.7^2)    ~  427 bps
//   lowest ask   = centre * (1 + 427/10000)                     ~ 1.4025
//
// Truth was 1.4143: the lowest ask sits ~0.84% below it instead of 10.4%.
// The six swept prices (1.2704-1.2979) are all far inside the floor and can
// no longer be posted.
// ---------------------------------------------------------------------------
TEST(UncertaintyQuoting, AcceptanceA_SweepLadderCanNoLongerBePosted) {
    const auto s = fv::solve_pair(live_anchors(), live_edges_with_amm(),
                                  "xch", "byc", kPairXchByc);
    ASSERT_TRUE(s.ok);
    ASSERT_EQ(tier_of(s), FairValueTier::Unavailable);  // still not clampable
    ASSERT_NEAR(s.price, 1.3608, 0.01);
    ASSERT_NEAR(s.sigma_bps, 466.7, 5.0);

    const double sigma_book = book_sigma(kBookXchBycSpr, 0);
    EXPECT_NEAR(sigma_book, 1057.65, 1.0);

    const auto blend = fv::blend_quote_center(kBookXchByc, sigma_book,
                                              s.price, s.sigma_bps);
    ASSERT_TRUE(blend.ok);

    // The wide book cedes to the external estimate, but does not vanish.
    EXPECT_NEAR(blend.w_external, 0.837, 0.01);
    EXPECT_NEAR(blend.center, 1.3451, 0.011);
    EXPECT_GT(blend.center, kBookXchByc);   // moved toward the truth...
    EXPECT_LT(blend.center, s.price);       // ...but not past the estimate.
    EXPECT_NEAR(blend.sigma_bps, 427.0, 10.0);

    const double floor_bps = min_half_spread_bps(blend.sigma_bps);
    EXPECT_DOUBLE_EQ(floor_bps,
                     kDefaults.quote_width_sigma_mult * blend.sigma_bps);

    // The lowest permissible ask covers the truth to within ~2% (0.84%
    // measured), against 9-11% below it at the sweep.
    const double lowest_ask = blend.center * (1.0 + floor_bps / 10'000.0);
    EXPECT_NEAR(lowest_ask, 1.4025, 0.012);
    EXPECT_GE(lowest_ask, 0.98 * kTrueXchByc);

    // Every one of the six swept prices is now unreachable.
    for (double px : kSweptAsks) {
        EXPECT_LT(px, lowest_ask * 0.93) << "swept ask " << px;
    }

    // And the actual ladder, built through the same spacing shift Step 7
    // applies, posts no ask below the floor and no bid above its mirror.
    const auto ladder = ladder_with_floor(blend.center, floor_bps);
    ASSERT_FALSE(ladder.empty());
    const auto bid_edge = static_cast<std::int64_t>(std::llround(
        blend.center * 1e12 * (1.0 - floor_bps / 10'000.0)));
    const auto ask_edge = static_cast<std::int64_t>(std::llround(
        blend.center * 1e12 * (1.0 + floor_bps / 10'000.0)));
    for (const auto& tq : ladder) {
        if (tq.side == Side::Ask) {
            EXPECT_GE(tq.price, ask_edge - 1)
                << "ask tier " << int(tq.tier_index);
            EXPECT_GT(static_cast<double>(tq.price) / 1e12, kSweptAsks.back());
        } else {
            EXPECT_LE(tq.price, bid_edge + 1)
                << "bid tier " << int(tq.tier_index);
        }
    }
}

// ---------------------------------------------------------------------------
// ACCEPTANCE (B): the healthy pair must quote essentially as today.
//
// XCH/wUSDC.b at the same block: book 1.428457 at 253.96 bps (its measured
// p50 is 237), solve 1.4335 at ~133 bps, CexDirect.  Arithmetic:
//
//   book sigma = sqrt(126.98^2 + 30^2)                     = 130.5 bps
//   w_external = 130.5^2 / (130.5^2 + 133.2^2)             = 0.490
//   shift      = 0.490 * ln(1.4335/1.428457)               ~ +17 bps
//   combined   = 130.5*133.2 / sqrt(130.5^2 + 133.2^2)     ~  93 bps
//
// Net effect: centre moves ~17 bps (a fifth of the pair's own half-spread);
// the width floor lands at ~93 bps, INSIDE the pair's existing 200-300 bps
// tier spacing, so a configured ladder does not move at all.  On the shipped
// 60 bps innermost default the shift is +33 bps -- bounded, as required, by
// k_sigma * 133 bps.
// ---------------------------------------------------------------------------
TEST(UncertaintyQuoting, AcceptanceB_HealthyPairIsEssentiallyUnchanged) {
    const auto s = fv::solve_pair(live_anchors(), live_edges_with_amm(),
                                  "xch", "wusdc", kPairXchUsdc);
    ASSERT_TRUE(s.ok);
    ASSERT_EQ(tier_of(s), FairValueTier::CexDirect);
    ASSERT_NEAR(s.sigma_bps, 133.0, 5.0);

    const double sigma_book = book_sigma(kBookXchUsdcSpr, 0);
    EXPECT_NEAR(sigma_book, 130.5, 1.0);

    const auto blend = fv::blend_quote_center(kBookXchUsdc, sigma_book,
                                              s.price, s.sigma_bps);
    ASSERT_TRUE(blend.ok);

    // Centre shift: near zero.  (+17.3 bps measured; a tight fresh book
    // keeps pricing itself.)
    const double shift_bps =
        std::log(blend.center / kBookXchUsdc) * 10'000.0;
    EXPECT_LT(std::abs(shift_bps), 25.0);

    // Width: combined sigma ~93 bps, so with k_sigma = 1 the floor is
    // bounded by the solve's own 133 bps and sits INSIDE the pair's
    // existing 200-300 bps tier spacing -- a configured ladder is untouched.
    EXPECT_NEAR(blend.sigma_bps, 93.2, 3.0);
    const double floor_bps = min_half_spread_bps(blend.sigma_bps);
    EXPECT_LE(floor_bps, kDefaults.quote_width_sigma_mult * 133.0 + 5.0);
    EXPECT_LT(floor_bps, 200.0);   // no shift on 200-300 bps spacing

    // On the shipped 60 bps innermost default, the shift is +33 bps.
    LiquidityConfig def;
    EXPECT_NEAR(floor_bps - def.tier_spacing_bps.front(), 33.2, 4.0);
}

// ---------------------------------------------------------------------------
// ACCEPTANCE (C): a hypothetical future pair with NO external anchor and a
// 3000 bps book.  No pair name, no peg, no special case: the blend degrades
// to the pair's own midpoint, the combined sigma IS the book's sigma
// (sqrt(1500^2 + 30^2) ~ 1500 bps), and the ladder is very wide, two-sided,
// and present -- NOT silence, NOT tight quotes.
// ---------------------------------------------------------------------------
TEST(UncertaintyQuoting, AcceptanceC_UnanchoredWideMarketQuotesWideNotSilent) {
    const double mid = 0.5;                       // arbitrary units
    const double sigma_book = book_sigma(3000.0, 0);
    EXPECT_NEAR(sigma_book, 1500.3, 1.0);

    // No external estimate of any kind.
    const auto blend = fv::blend_quote_center(mid, sigma_book, 0.0, 0.0);
    ASSERT_TRUE(blend.ok);
    EXPECT_DOUBLE_EQ(blend.center, mid);          // its own midpoint
    EXPECT_DOUBLE_EQ(blend.sigma_bps, sigma_book);

    const double floor_bps = min_half_spread_bps(blend.sigma_bps);
    EXPECT_GE(floor_bps, 1500.0);                 // >= 1500 bps-ish

    const auto ladder = ladder_with_floor(mid, floor_bps);

    // NOT silence: both sides are quoted.
    int bids = 0;
    int asks = 0;
    for (const auto& tq : ladder) {
        (tq.side == Side::Bid ? bids : asks)++;
    }
    EXPECT_GT(bids, 0);
    EXPECT_GT(asks, 0);

    // NOT tight: every ask >= mid * 1.15, every bid <= mid * 0.85.
    for (const auto& tq : ladder) {
        const double px = static_cast<double>(tq.price) / 1e12;
        if (tq.side == Side::Ask) {
            EXPECT_GE(px, mid * (1.0 + floor_bps / 10'000.0) * 0.999);
        } else {
            EXPECT_LE(px, mid * (1.0 - floor_bps / 10'000.0) * 1.001);
        }
    }
}

// The knobs ship with working defaults: an operator who never touches
// config.yaml gets uncertainty-scaled quoting, exactly like every other
// guard in this family.
TEST(UncertaintyQuoting, DefaultsProtectAnUnconfiguredDeployment) {
    const StrategyConfig d{};
    EXPECT_TRUE(d.quote_center_blend_enabled);
    EXPECT_DOUBLE_EQ(d.quote_width_sigma_mult, 1.0);  // one standard deviation
}

// The accessor contract: an estimate whose sigma exceeds the clamp ceiling is
// invisible to get_fair_value (clamping against it would be theatre) but
// visible to get_fair_value_estimate WITH its sigma, so the quoting path can
// weight it honestly instead of discarding it.
TEST_F(FairValueTest, HighSigmaEstimateIsServedForQuotingButNotForClamping) {
    FairValue in;
    in.price     = 1.3608;
    in.tier      = FairValueTier::Unavailable;   // sigma above the ceiling
    in.sigma_bps = 466.7;
    feed_->ingest_fair_value("XCH/BYC", in);

    EXPECT_FALSE(feed_->get_fair_value("XCH/BYC").has_value());

    const auto est = feed_->get_fair_value_estimate("XCH/BYC");
    ASSERT_TRUE(est.has_value());
    EXPECT_DOUBLE_EQ(est->price, 1.3608);
    EXPECT_DOUBLE_EQ(est->sigma_bps, 466.7);
    EXPECT_EQ(est->tier, FairValueTier::Unavailable);
}

// ...but a solve with no anchored answer at all has no estimate to serve, and
// the accessor must say so rather than reheat a stale one.
TEST_F(FairValueTest, NoAnchoredAnswerMeansNoEstimateEither) {
    // A real estimate arrives first...
    FairValue in;
    in.price     = 1.3608;
    in.tier      = FairValueTier::Unavailable;
    in.sigma_bps = 466.7;
    feed_->ingest_fair_value("XCH/BYC", in);
    ASSERT_TRUE(feed_->get_fair_value_estimate("XCH/BYC").has_value());

    // ...then the anchor path is lost entirely: the estimate must clear.
    feed_->ingest_fair_value("XCH/BYC", FairValue{});
    EXPECT_FALSE(feed_->get_fair_value_estimate("XCH/BYC").has_value());
}

}  // namespace
}  // namespace xop
