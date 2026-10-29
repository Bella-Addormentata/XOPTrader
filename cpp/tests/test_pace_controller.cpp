// test_pace_controller.cpp -- the pace controller's pure decisions
// (cpp/include/xop/strategy/pace_controller.hpp).
//
// Every expected value is a LITERAL produced by the spec's Python mirror,
// which executes these functions with identical binary64 expression order
// (specs/pace_v2_work/pace_model.py, pace_decide.py, pace_tests.py; final run
// out_tests_run3.txt).  A test never restates a formula.  Assertion forms:
// doubles EXPECT_NEAR 1e-9 unless stated, prices EXPECT_NEAR +/-2 mojos,
// sizes EXPECT_EQ, uint32 fields against `u` literals.
//
// ISO/IEC 5055 -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/strategy/pace_controller.hpp>
#include <xop/types.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace pace = xop::strategy::pace;
using xop::BlockHeight;
using xop::Mojo;

constexpr BlockHeight kNow  = 9'287'678u;
constexpr double      kNaN  = std::numeric_limits<double>::quiet_NaN();
constexpr double      kInf  = std::numeric_limits<double>::infinity();
constexpr Mojo        kE12  = 1'000'000'000'000;
constexpr Mojo        kBase0 = 1'426'312'378'968;   // offer 19913's price

// ============================================================================
// Shared fixtures (spec 9.2).  Holdings are built XCH, BYC, DBX: P4 sums in
// input order.
// ============================================================================

pace::HoldingInput holding(const char* key, double units, double target, double tol,
                           bool is_xch, double fv_price, double fv_sigma_bps)
{
    pace::HoldingInput h{};
    h.key               = key;
    h.targeted          = true;
    h.target            = target;
    h.tol               = tol;
    h.is_xch            = is_xch;
    h.id_resolved       = true;
    h.cache_present     = true;
    h.fields_validated  = true;
    h.as_of_block       = kNow;
    h.units             = units;
    h.fv_present        = true;
    h.fv_tier_available = true;
    h.fv_feed_fresh     = true;
    h.asset_is_quote    = true;
    h.fv_price          = fv_price;
    h.fv_sigma_bps      = fv_sigma_bps;
    return h;
}

std::vector<pace::HoldingInput> live_holdings()
{
    return {
        holding("XCH", 24.569461, 0.5, 0.4, true, 0.0, 0.0),
        holding("BYC", 111.054, 0.05, 0.02, false, 1.49285, 172.0),
        holding("DBX", 1740.296, 0.45, 0.4, false, 84.634, 141.0),
    };
}

template <typename Change>
std::vector<pace::HoldingInput> edit_holding(std::vector<pace::HoldingInput> hs,
                                             const std::string& key, Change change)
{
    for (pace::HoldingInput& entry : hs) {
        if (entry.key == key) {
            change(entry);
        }
    }
    return hs;
}

pace::PairInput pair_input(const char* name, const char* base_key, const char* quote_key,
                           double fv_price, double fv_sigma_bps)
{
    pace::PairInput p{};
    p.name               = name;
    p.base_key           = base_key;
    p.quote_key          = quote_key;
    p.enabled            = true;
    p.quote_valid        = true;
    p.fills_ok           = true;
    p.base_mpu           = kE12;
    p.quote_mpu          = 1000;
    p.min_offer_override = 1.0;
    p.xch_base           = true;
    p.side_tier_count    = 6u;
    p.fv_ok              = true;
    p.fv_price           = fv_price;
    p.fv_sigma_bps       = fv_sigma_bps;
    return p;
}

std::vector<pace::PairInput> live_pairs()
{
    pace::PairInput byc_wusdc = pair_input("BYC/wUSDC.b", "BYC", "WUSDC.B", 0.0, 0.0);
    byc_wusdc.enabled         = false;
    byc_wusdc.base_mpu        = 1000;
    byc_wusdc.quote_mpu       = 1000;
    byc_wusdc.xch_base        = false;
    byc_wusdc.side_tier_count = 4u;
    byc_wusdc.fv_ok           = false;
    return {
        pair_input("XCH/BYC", "XCH", "BYC", 1.49285, 172.0),
        pair_input("XCH/DBX", "XCH", "DBX", 84.634, 141.0),
        byc_wusdc,
    };
}

template <typename Change>
std::vector<pace::PairInput> edit_pair(std::vector<pace::PairInput> ps, const std::string& name,
                                       Change change)
{
    for (pace::PairInput& entry : ps) {
        if (entry.name == name) {
            change(entry);
        }
    }
    return ps;
}

pace::FillRow maker_row(const char* pair, const char* side, Mojo size, Mojo price, BlockHeight bh)
{
    pace::FillRow r{};
    r.pair_name    = pair;
    r.is_taker     = false;
    r.side_lower   = side;
    r.size_mojos   = size;
    r.price_mojos  = price;
    r.block_height = bh;
    return r;
}

pace::FillRow taker_row(const char* pair, bool we_bought_base, Mojo base_delta, Mojo quote_delta,
                        BlockHeight bh)
{
    pace::FillRow r{};
    r.pair_name         = pair;
    r.is_taker          = true;
    r.we_bought_base    = we_bought_base;
    r.base_delta_mojos  = base_delta;
    r.quote_delta_mojos = quote_delta;
    r.block_height      = bh;
    return r;
}

std::vector<pace::FillRow> live_fills()
{
    return {
        taker_row("XCH/BYC", true, 2'000'000'000'000, -2848, 9'245'455u),
        // The 37 September asks aggregated: 59.811 BYC.
        maker_row("XCH/BYC", "ask", kE12, 59'811'000'000'000, 9'250'172u),
        maker_row("XCH/DBX", "ask", kE12, 83'783'000'000'000, 9'276'808u),
    };
}

pace::PaceParams live_params()
{
    pace::PaceParams p{};
    p.enabled                = true;
    p.assets                 = {"BYC"};
    p.horizon_blocks         = 64'512u;
    p.enter_tol_mult         = 1.5;
    p.exit_tol_mult          = 1.0;
    p.max_resting_frac       = 0.5;
    p.min_tier_units         = 1.0;
    p.max_tier_units         = 5.0;
    p.max_tiers              = 3u;
    p.tighten_step_bps       = 25.0;
    p.tighten_max_bps        = 300.0;
    p.max_fv_sigma_bps       = 200.0;
    p.max_balance_age_blocks = 20u;
    p.global_min_offer_units = 1.0;
    p.global_max_offer_units = 5.0;
    return p;
}

pace::PaceInputs live_inputs()
{
    pace::PaceInputs in{};
    in.now          = kNow;
    in.ramp_running = true;
    in.params       = live_params();
    in.holdings     = live_holdings();
    in.pairs        = live_pairs();
    in.fills        = live_fills();
    return in;
}

pace::AssetMemory memory_of(bool active, std::uint32_t ramp, BlockHeight last)
{
    pace::AssetMemory m{};
    m.active      = active;
    m.ramp_blocks = ramp;
    m.last_block  = last;
    return m;
}

pace::PriceGuards guards_g()
{
    pace::PriceGuards g{};
    g.fair_value_px         = 1.49285e12;
    g.fv_sigma_bps          = 172.0;
    g.min_edge_bps          = 50.0;
    g.edge_sigma_mult       = 1.0;
    g.centre_px             = 1.4935e12;
    g.min_half_spread_bps   = 172.0;
    g.best_bid_px           = 2.0e12;
    g.best_ask_px           = 3.33333e12;
    g.book_guard_margin_bps = 800.0;
    g.published_mid_px      = 2.66667e12;
    g.has_bbo               = true;
    g.bid_tier_ref_px       = 2.0e12;
    g.effective_mid_px      = 2.66667e12;
    g.max_aggressive_dev    = 0.8;
    g.max_passive_dev       = 0.8;
    g.tier_step_bps         = 10.0;
    return g;
}

pace::PriceGuards guards_gnc()
{
    pace::PriceGuards g = guards_g();
    g.centre_px           = 0.0;
    g.min_half_spread_bps = 0.0;
    return g;
}

pace::FlowUnits reduced_units(double x)
{
    pace::FlowUnits f{};
    f.reduced    = x;
    f.recognised = true;
    return f;
}

pace::FlowUnits increased_units(double x)
{
    pace::FlowUnits f{};
    f.increased  = x;
    f.recognised = true;
    return f;
}

// ============================================================================
// P1, P3
// ============================================================================

TEST(PaceFreshness, FreshWithinAge) {
    EXPECT_TRUE(pace::balance_is_fresh(true, 100u, 120u, 20u));
}

TEST(PaceFreshness, StaleBeyondAge) {
    EXPECT_FALSE(pace::balance_is_fresh(true, 100u, 121u, 20u));
}

TEST(PaceFreshness, UnvalidatedIsStale) {
    EXPECT_FALSE(pace::balance_is_fresh(false, 120u, 120u, 20u));
}

TEST(PaceFreshness, HeightRegressionIsFresh) {
    EXPECT_TRUE(pace::balance_is_fresh(true, 130u, 120u, 20u));
}

TEST(PaceSatSub, RegressionIsZeroNormalIsDifference) {
    EXPECT_EQ(pace::sat_sub_blocks(5u, 9u), 0u);
    EXPECT_EQ(pace::sat_sub_blocks(9u, 5u), 4u);
}

// ============================================================================
// P2 (asset_is_quote, tier_available, feed_fresh, price, sigma, max_sigma)
// ============================================================================

TEST(PaceFairValue, QuoteAssetRateIsReciprocal) {
    const auto r = pace::xch_per_unit_from_fair_value(true, true, true, 1.49285, 172.0, 200.0);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 0.6698596644003081, 1e-9);
}

TEST(PaceFairValue, BaseAssetRateIsPrice) {
    const auto r = pace::xch_per_unit_from_fair_value(false, true, true, 0.012, 50.0, 200.0);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 0.012, 1e-9);
}

TEST(PaceFairValue, UnavailableTierFailsClosed) {
    EXPECT_FALSE(pace::xch_per_unit_from_fair_value(true, false, true, 1.49285, 172.0, 200.0).has_value());
}

TEST(PaceFairValue, StaleFeedFailsClosed) {
    EXPECT_FALSE(pace::xch_per_unit_from_fair_value(true, true, false, 1.49285, 172.0, 200.0).has_value());
}

TEST(PaceFairValue, BadPriceFailsClosed) {
    for (const double price : {0.0, -1.0, kNaN, kInf}) {
        EXPECT_FALSE(pace::xch_per_unit_from_fair_value(true, true, true, price, 172.0, 200.0).has_value())
            << "price " << price;
    }
}

TEST(PaceFairValue, SigmaAboveCeilingFailsClosed) {
    EXPECT_FALSE(pace::xch_per_unit_from_fair_value(true, true, true, 1.49285, 200.0001, 200.0).has_value());
    const auto at_ceiling = pace::xch_per_unit_from_fair_value(true, true, true, 1.49285, 200.0, 200.0);
    ASSERT_TRUE(at_ceiling.has_value());
    EXPECT_NEAR(*at_ceiling, 0.6698596644003081, 1e-9);
}

TEST(PaceFairValue, NanSigmaOrCeilingFailsClosed) {
    EXPECT_FALSE(pace::xch_per_unit_from_fair_value(true, true, true, 1.49285, kNaN, 200.0).has_value());
    EXPECT_FALSE(pace::xch_per_unit_from_fair_value(true, true, true, 1.49285, 172.0, kNaN).has_value());
    EXPECT_FALSE(pace::xch_per_unit_from_fair_value(true, true, true, 1.49285, 172.0, 0.0).has_value());
}

// ============================================================================
// P4 (managed "BYC", kNow, max_age 20u, max_sigma 200)
// ============================================================================

TEST(PaceValuation, LiveCase_BycAtFairValue) {
    const pace::Valuation v = pace::value_portfolio(live_holdings(), "BYC", kNow, 20u, 200.0);
    ASSERT_TRUE(v.ok);
    EXPECT_NEAR(v.share, 0.6223973834752728, 1e-9);
    EXPECT_NEAR(v.total_xch, 119.52266694139672, 1e-9);
    EXPECT_NEAR(v.managed_xch, 74.39059517031181, 1e-9);
    EXPECT_NEAR(v.managed_xch_per_unit, 0.6698596644003081, 1e-9);
}

TEST(PaceValuation, UntargetedAssetExcluded) {
    std::vector<pace::HoldingInput> hs = live_holdings();
    pace::HoldingInput wusdc = holding("WUSDC.B", 78.6, 0.0, 0.0, false, 1.0, 10.0);
    wusdc.targeted = false;
    hs.push_back(wusdc);
    const pace::Valuation v = pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0);
    ASSERT_TRUE(v.ok);
    EXPECT_NEAR(v.total_xch, 119.52266694139672, 1e-9);
}

TEST(PaceValuation, TargetedHeldAssetWithoutFairValueFailsClosed) {
    const auto hs = edit_holding(live_holdings(), "DBX", [](pace::HoldingInput& x) { x.fv_present = false; });
    EXPECT_FALSE(pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0).ok);
}

TEST(PaceValuation, TargetedZeroBalanceWithoutFairValueIgnored) {
    const auto hs = edit_holding(live_holdings(), "DBX", [](pace::HoldingInput& x) {
        x.units = 0.0;
        x.fv_present = false;
    });
    const pace::Valuation v = pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0);
    ASSERT_TRUE(v.ok);
    EXPECT_NEAR(v.total_xch, 98.96005617031182, 1e-9);
}

TEST(PaceValuation, UnvalidatedZeroBalanceFailsClosed) {
    const auto hs = edit_holding(live_holdings(), "DBX", [](pace::HoldingInput& x) {
        x.units = 0.0;
        x.fv_present = false;
        x.fields_validated = false;
    });
    EXPECT_FALSE(pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0).ok);
}

TEST(PaceValuation, StaleZeroBalanceFailsClosed) {
    const auto hs = edit_holding(live_holdings(), "DBX", [](pace::HoldingInput& x) {
        x.units = 0.0;
        x.fv_present = false;
        x.as_of_block = kNow - 21u;
    });
    EXPECT_FALSE(pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0).ok);
}

TEST(PaceValuation, MissingCacheEntryFailsClosed) {
    const auto hs = edit_holding(live_holdings(), "XCH", [](pace::HoldingInput& x) { x.cache_present = false; });
    EXPECT_FALSE(pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0).ok);
}

TEST(PaceValuation, UnresolvedIdFailsClosed) {
    const auto hs = edit_holding(live_holdings(), "DBX", [](pace::HoldingInput& x) { x.id_resolved = false; });
    EXPECT_FALSE(pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0).ok);
}

TEST(PaceValuation, StaleTargetedBalanceFailsClosed) {
    const auto hs = edit_holding(live_holdings(), "BYC", [](pace::HoldingInput& x) { x.as_of_block = kNow - 21u; });
    EXPECT_FALSE(pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0).ok);
}

TEST(PaceValuation, ManagedZeroUnitsStillNeedsRate) {
    const auto hs = edit_holding(live_holdings(), "BYC", [](pace::HoldingInput& x) {
        x.units = 0.0;
        x.fv_present = false;
    });
    EXPECT_FALSE(pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0).ok);
}

TEST(PaceValuation, NonFiniteUnitsFailClosed) {
    const auto hs = edit_holding(live_holdings(), "BYC", [](pace::HoldingInput& x) { x.units = kNaN; });
    EXPECT_FALSE(pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0).ok);
}

TEST(PaceValuation, EmptyPortfolioFailsClosed) {
    std::vector<pace::HoldingInput> hs = live_holdings();
    for (pace::HoldingInput& x : hs) {
        x.units = 0.0;
    }
    EXPECT_FALSE(pace::value_portfolio(hs, "BYC", kNow, 20u, 200.0).ok);
}

// ============================================================================
// P5 (target 0.0625, tol 0.03125, enter 1.5, exit 1.0 unless stated: every
// boundary is exactly representable)
// ============================================================================

constexpr double kTarget = 0.0625;
constexpr double kTol    = 0.03125;

TEST(PaceActivation, EntersAboveEnterLevel) {
    EXPECT_TRUE(pace::decide_activation(false, 0.125, kTarget, kTol, 1.5, 1.0).active);
}

TEST(PaceActivation, DoesNotEnterAtEnterLevel) {
    EXPECT_FALSE(pace::decide_activation(false, 0.109375, kTarget, kTol, 1.5, 1.0).active);
}

TEST(PaceActivation, StaysActiveBetweenExitAndEnter) {
    EXPECT_TRUE(pace::decide_activation(true, 0.1, kTarget, kTol, 1.5, 1.0).active);
}

TEST(PaceActivation, DoesNotEnterBetweenExitAndEnter) {
    EXPECT_FALSE(pace::decide_activation(false, 0.1, kTarget, kTol, 1.5, 1.0).active);
}

TEST(PaceActivation, ExitsAtExitLevelBoundary) {
    EXPECT_FALSE(pace::decide_activation(true, 0.09375, kTarget, kTol, 1.5, 1.0).active);
}

TEST(PaceActivation, InsideBandInactive) {
    EXPECT_FALSE(pace::decide_activation(false, 0.0625, kTarget, kTol, 1.5, 1.0).active);
    EXPECT_FALSE(pace::decide_activation(true, 0.0625, kTarget, kTol, 1.5, 1.0).active);
}

TEST(PaceActivation, LevelsAreExact) {
    const pace::Activation a = pace::decide_activation(false, 0.0, kTarget, kTol, 1.5, 1.0);
    EXPECT_DOUBLE_EQ(a.enter_level, 0.109375);
    EXPECT_DOUBLE_EQ(a.exit_level, 0.09375);
}

TEST(PaceActivation, ZeroToleranceIsConfigConflict) {
    EXPECT_FALSE(pace::decide_activation(false, 0.5, kTarget, 0.0, 1.5, 1.0).config_ok);
}

TEST(PaceActivation, ExitNotBelowEnterIsConfigConflict) {
    EXPECT_FALSE(pace::decide_activation(false, 0.5, kTarget, kTol, 1.5, 1.5).config_ok);
}

TEST(PaceActivation, NegativeExitIsConfigConflict) {
    EXPECT_FALSE(pace::decide_activation(false, 0.5, kTarget, kTol, 1.5, -0.5).config_ok);
}

TEST(PaceActivation, NonFiniteShareInactive) {
    const pace::Activation a = pace::decide_activation(true, kNaN, kTarget, kTol, 1.5, 1.0);
    EXPECT_TRUE(a.config_ok);
    EXPECT_FALSE(a.active);
}

TEST(PaceActivation, LiveShareActivates) {
    const pace::Activation a = pace::decide_activation(false, 0.6223973834752728, 0.05, 0.02, 1.5, 1.0);
    EXPECT_TRUE(a.active);
    EXPECT_NEAR(a.enter_level, 0.08, 1e-12);
    EXPECT_NEAR(a.exit_level, 0.07, 1e-12);
}

// ============================================================================
// P6
// ============================================================================

TEST(PaceExcess, LiveCase) {
    EXPECT_NEAR(pace::excess_native_units(74.39059517031181, 119.52266694139672, 0.05 + 1.0 * 0.02,
                                          0.6698596644003081),
                98.56394106595751, 1e-9);
}

TEST(PaceExcess, BelowExitIsZero) {
    EXPECT_NEAR(pace::excess_native_units(5.0, 100.0, 0.07, 0.5), 0.0, 1e-9);
}

TEST(PaceExcess, BadRateIsZero) {
    EXPECT_NEAR(pace::excess_native_units(50.0, 100.0, 0.07, 0.0), 0.0, 1e-9);
    EXPECT_NEAR(pace::excess_native_units(50.0, 100.0, 0.07, kNaN), 0.0, 1e-9);
}

// ============================================================================
// P7 (recognised / reduced / increased)
// ============================================================================

TEST(PaceFills, MakerBidOnQuoteAssetReduces) {
    const pace::FlowUnits f = pace::classify_maker_fill(false, "bid", 1'500'000'000'000, 1'400'000'000'000, kE12, 1000);
    EXPECT_TRUE(f.recognised);
    EXPECT_NEAR(f.reduced, 2.1000000000000005, 1e-9);
    EXPECT_NEAR(f.increased, 0.0, 1e-9);
}

TEST(PaceFills, MakerAskOnQuoteAssetIncreases_LiveRow1884) {
    const pace::FlowUnits f = pace::classify_maker_fill(false, "ask", kE12, 1'885'000'000'000, kE12, 1000);
    EXPECT_TRUE(f.recognised);
    EXPECT_NEAR(f.reduced, 0.0, 1e-9);
    EXPECT_NEAR(f.increased, 1.885, 1e-9);
}

TEST(PaceFills, MakerAskOnBaseAssetReduces) {
    const pace::FlowUnits f = pace::classify_maker_fill(true, "ask", 1500, 1'002'000'000'000, 1000, 1000);
    EXPECT_TRUE(f.recognised);
    EXPECT_NEAR(f.reduced, 1.5, 1e-9);
    EXPECT_NEAR(f.increased, 0.0, 1e-9);
}

TEST(PaceFills, MakerBidOnBaseAssetIncreases) {
    const pace::FlowUnits f = pace::classify_maker_fill(true, "bid", 2500, 978'000'000'000, 1000, 1000);
    EXPECT_TRUE(f.recognised);
    EXPECT_NEAR(f.reduced, 0.0, 1e-9);
    EXPECT_NEAR(f.increased, 2.5, 1e-9);
}

TEST(PaceFills, UnloweredSideNotRecognised) {
    EXPECT_FALSE(pace::classify_maker_fill(false, "BID", kE12, 1'400'000'000'000, kE12, 1000).recognised);
}

TEST(PaceFills, NonPositiveSizeOrPriceNotRecognised) {
    EXPECT_FALSE(pace::classify_maker_fill(false, "bid", 0, 1'400'000'000'000, kE12, 1000).recognised);
    EXPECT_FALSE(pace::classify_maker_fill(false, "bid", kE12, -1, kE12, 1000).recognised);
}

TEST(PaceFills, TakerBoughtBaseReducesQuote_LiveRow609) {
    const pace::FlowUnits f = pace::classify_taker_fill(false, true, 2'000'000'000'000, -2848, kE12, 1000);
    EXPECT_TRUE(f.recognised);
    EXPECT_NEAR(f.reduced, 2.848, 1e-9);
    EXPECT_NEAR(f.increased, 0.0, 1e-9);
}

TEST(PaceFills, TakerSoldBaseReducesBase) {
    const pace::FlowUnits f = pace::classify_taker_fill(true, false, -1500, 1502, 1000, 1000);
    EXPECT_TRUE(f.recognised);
    EXPECT_NEAR(f.reduced, 1.5, 1e-9);
    EXPECT_NEAR(f.increased, 0.0, 1e-9);
}

TEST(PaceFills, TakerInt64MinDeltaIsFinite) {
    const pace::FlowUnits f = pace::classify_taker_fill(true, false, std::numeric_limits<std::int64_t>::min(), 0,
                                                        kE12, 1000);
    EXPECT_TRUE(f.recognised);
    EXPECT_TRUE(std::isfinite(f.reduced));
}

TEST(PaceFills, LargeProductStaysFinite) {
    const pace::FlowUnits f = pace::classify_maker_fill(false, "bid", 9'000'000'000'000'000'000,
                                                        9'000'000'000'000'000, kE12, 1000);
    EXPECT_TRUE(f.recognised);
    EXPECT_TRUE(std::isfinite(f.reduced));
}

// ============================================================================
// P8 (horizon 64'512u)
// ============================================================================

using Rows = std::vector<std::pair<BlockHeight, pace::FlowUnits>>;

TEST(PaceProgress, WindowBoundary) {
    const Rows rows{{5'392u, reduced_units(1.0)}, {5'393u, reduced_units(2.0)}};
    const pace::Progress p = pace::accumulate_progress(rows, 10'000u, 64'512u);
    EXPECT_NEAR(p.sold_window, 2.0, 1e-9);
    EXPECT_NEAR(p.reduced_horizon, 3.0, 1e-9);
}

TEST(PaceProgress, HorizonBoundary) {
    const Rows rows{{35'488u, reduced_units(1.0)}, {35'489u, reduced_units(2.0)}};
    const pace::Progress p = pace::accumulate_progress(rows, 100'000u, 64'512u);
    EXPECT_NEAR(p.sold_window, 0.0, 1e-9);
    EXPECT_NEAR(p.reduced_horizon, 2.0, 1e-9);
}

TEST(PaceProgress, FutureRowCountsInWindow) {
    const Rows rows{{10'005u, reduced_units(1.0)}};
    EXPECT_NEAR(pace::accumulate_progress(rows, 10'000u, 64'512u).sold_window, 1.0, 1e-9);
}

TEST(PaceProgress, UnrecognisedRowsCounted) {
    const Rows rows{{9'000u, pace::FlowUnits{}}};
    EXPECT_EQ(pace::accumulate_progress(rows, 10'000u, 64'512u).rows_ignored, 1u);
}

TEST(PaceProgress, LiveCaseWindowRows_AggregatedAsks) {
    // Pinned now; the 2026-08-30 take at 9,222,386 is outside the 14-day horizon.
    const Rows rows{{9'245'455u, reduced_units(2.848)},
                    {9'250'172u, increased_units(59.811)},
                    {9'222'386u, reduced_units(8.727)}};
    const pace::Progress p = pace::accumulate_progress(rows, kNow, 64'512u);
    EXPECT_NEAR(p.sold_window, 0.0, 1e-9);
    EXPECT_NEAR(p.reduced_horizon, 2.848, 1e-9);
    EXPECT_NEAR(p.increased_horizon, 59.811, 1e-9);
}

// ============================================================================
// P9
// ============================================================================

TEST(PaceHeadroom, UntargetedIsInfinite) {
    const double headroom = pace::acquired_headroom_units(false, 1.0, 2.0, 0.5, 0.4, 1.0);
    EXPECT_TRUE(std::isinf(headroom));
    EXPECT_GT(headroom, 0.0);
}

TEST(PaceHeadroom, LiveXchHeadroom) {
    EXPECT_NEAR(pace::acquired_headroom_units(true, 24.569461, 119.52266694139672, 0.5, 0.4, 0.6698596644003081),
                123.9079521552677, 1e-9);
}

TEST(PaceHeadroom, AtBandEdgeIsZero) {
    EXPECT_NEAR(pace::acquired_headroom_units(true, 90.0, 100.0, 0.5, 0.4, 1.0), 0.0, 1e-9);
}

// ============================================================================
// P10 (H 64'512u, frac 0.5, headroom +inf unless stated;
// expect budget_window / remaining / resting_cap / exhausted)
// ============================================================================

pace::Progress progress_of(double sold_window, double reduced_horizon, double increased_horizon)
{
    pace::Progress p{};
    p.sold_window       = sold_window;
    p.reduced_horizon   = reduced_horizon;
    p.increased_horizon = increased_horizon;
    return p;
}

void expect_budget(const pace::Budget& b, double window, double remaining, double cap, bool exhausted)
{
    EXPECT_NEAR(b.budget_window, window, 1e-9);
    EXPECT_NEAR(b.remaining, remaining, 1e-9);
    EXPECT_NEAR(b.resting_cap, cap, 1e-9);
    EXPECT_EQ(b.exhausted, exhausted);
}

TEST(PaceBudget, GrossScheduleIgnoresInWindowIncreases) {
    expect_budget(pace::compute_budget(98.0, progress_of(0.0, 14.0, 59.811), 64'512u, 0.5, kInf),
                  8.0, 8.0, 4.0, false);
}

TEST(PaceBudget, ConstantUnderNominalExecution) {
    {
        SCOPED_TRACE("excess 98, reduced 14");
        expect_budget(pace::compute_budget(98.0, progress_of(0.0, 14.0, 0.0), 64'512u, 0.5, kInf),
                      8.0, 8.0, 4.0, false);
    }
    {
        SCOPED_TRACE("excess 90, reduced 22");
        expect_budget(pace::compute_budget(90.0, progress_of(0.0, 22.0, 0.0), 64'512u, 0.5, kInf),
                      8.0, 8.0, 4.0, false);
    }
}

TEST(PaceBudget, RemainingNeverExceedsExcess) {
    expect_budget(pace::compute_budget(1.0, progress_of(0.0, 139.0, 0.0), 64'512u, 0.5, kInf),
                  10.0, 1.0, 1.0, false);
}

TEST(PaceBudget, RestingCapIsFractionOfBudgetCappedByRemaining) {
    {
        SCOPED_TRACE("sold 1");
        expect_budget(pace::compute_budget(98.0, progress_of(1.0, 14.0, 0.0), 64'512u, 0.5, kInf),
                      8.0, 7.0, 4.0, false);
    }
    {
        SCOPED_TRACE("sold 5");
        expect_budget(pace::compute_budget(98.0, progress_of(5.0, 14.0, 0.0), 64'512u, 0.5, kInf),
                      8.0, 3.0, 3.0, false);
    }
}

TEST(PaceBudget, ExhaustedWhenSoldReachesBudget) {
    expect_budget(pace::compute_budget(98.0, progress_of(8.0, 14.0, 0.0), 64'512u, 0.5, kInf),
                  8.0, 0.0, 0.0, true);
}

TEST(PaceBudget, HeadroomBindsRestingCap) {
    expect_budget(pace::compute_budget(98.0, progress_of(0.0, 14.0, 0.0), 64'512u, 0.5, 1.5),
                  8.0, 8.0, 1.5, false);
}

TEST(PaceBudget, HorizonShorterThanWindowFailsClosed) {
    expect_budget(pace::compute_budget(98.0, progress_of(0.0, 14.0, 0.0), 4'607u, 0.5, kInf),
                  0.0, 0.0, 0.0, true);
}

TEST(PaceBudget, HorizonEqualToWindowAllowed) {
    expect_budget(pace::compute_budget(98.0, progress_of(0.0, 14.0, 0.0), 4'608u, 0.5, kInf),
                  112.0, 98.0, 56.0, false);
}

TEST(PaceBudget, NonFiniteInputFailsClosed) {
    {
        SCOPED_TRACE("excess NaN");
        expect_budget(pace::compute_budget(kNaN, progress_of(0.0, 14.0, 0.0), 64'512u, 0.5, kInf),
                      0.0, 0.0, 0.0, true);
    }
    {
        SCOPED_TRACE("reduced +inf");
        expect_budget(pace::compute_budget(98.0, progress_of(0.0, kInf, 0.0), 64'512u, 0.5, kInf),
                      0.0, 0.0, 0.0, true);
    }
}

TEST(PaceBudget, LiveCase14Day) {
    expect_budget(pace::compute_budget(98.56394106595751, progress_of(0.0, 2.848, 59.811), 64'512u, 0.5, kInf),
                  7.2437100761398225, 7.2437100761398225, 3.6218550380699113, false);
}

TEST(PaceBudget, LiveCase7Day) {
    // The 7-day window's actual rows: none.
    expect_budget(pace::compute_budget(98.56394106595751, progress_of(0.0, 0.0, 0.0), 32'256u, 0.5, kInf),
                  14.0805630094225, 14.0805630094225, 7.04028150471125, false);
}

// ============================================================================
// P11 (budget, excess, sold, ramp; step 25, max 300 unless stated)
// ============================================================================

TEST(PaceTighten, BehindFullDayIsMax) {
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 0.0, 4'608u, 25.0, 300.0), 300.0, 1e-9);
}

TEST(PaceTighten, AheadOfScheduleIsZero) {
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 30.0, 4'608u, 25.0, 300.0), 0.0, 1e-9);
}

TEST(PaceTighten, RampsInOverFirstDay) {
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 0.0, 2'304u, 25.0, 300.0), 150.0, 1e-9);
}

TEST(PaceTighten, QuantizesDownToStep) {
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 13.0, 4'608u, 25.0, 300.0), 125.0, 1e-9);
}

TEST(PaceTighten, ReferenceCappedByExcessPlusSold) {
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 2.0, 1.0, 4'608u, 25.0, 300.0), 200.0, 1e-9);
}

TEST(PaceTighten, RampSaturatesAtOneWindow) {
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 13.0, 9'216u, 25.0, 300.0), 125.0, 1e-9);
}

TEST(PaceTighten, ZeroRampIsZero) {
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 0.0, 0u, 25.0, 300.0), 0.0, 1e-9);
}

TEST(PaceTighten, StepOutOfRangeIsZero) {
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 0.0, 4'608u, 0.5, 300.0), 0.0, 1e-9);
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 0.0, 4'608u, 1001.0, 300.0), 0.0, 1e-9);
}

TEST(PaceTighten, MaxOutOfRangeIsZero) {
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 0.0, 4'608u, 25.0, 0.0), 0.0, 1e-9);
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, 0.0, 4'608u, 25.0, 5001.0), 0.0, 1e-9);
}

TEST(PaceTighten, NonFiniteInputsAreZero) {
    EXPECT_NEAR(pace::compute_tighten_bps(kNaN, 100.0, 0.0, 4'608u, 25.0, 300.0), 0.0, 1e-9);
    EXPECT_NEAR(pace::compute_tighten_bps(24.0, 100.0, kInf, 4'608u, 25.0, 300.0), 0.0, 1e-9);
}

// ============================================================================
// P12, P13
// ============================================================================

TEST(PaceBand, XchBaseDefaults) {
    const pace::OfferSizeBand b = pace::effective_offer_size_band(true, std::nullopt, 0.1, 5.0);
    EXPECT_NEAR(b.min_units, 1.0, 1e-9);
    EXPECT_NEAR(b.max_units, 5.0, 1e-9);
}

TEST(PaceBand, CatBaseUsesGlobalMinAndNoMax) {
    const pace::OfferSizeBand b = pace::effective_offer_size_band(false, std::nullopt, 0.1, 5.0);
    EXPECT_NEAR(b.min_units, 0.1, 1e-9);
    EXPECT_NEAR(b.max_units, 0.0, 1e-9);
}

TEST(PaceBand, OverrideWins) {
    const pace::OfferSizeBand b = pace::effective_offer_size_band(true, 2.0, 0.1, 5.0);
    EXPECT_NEAR(b.min_units, 2.0, 1e-9);
    EXPECT_NEAR(b.max_units, 5.0, 1e-9);
}

pace::OfferSizeBand band_of(double min_units, double max_units)
{
    pace::OfferSizeBand b{};
    b.min_units = min_units;
    b.max_units = max_units;
    return b;
}

// upb 1.49285, base_mpu 1e12, pace 1.0 / 5.0, band {1.0, 5.0}, max_tiers 3u,
// side 6u unless stated.
pace::TierPlan plan_live(double cap, double upb = 1.49285, pace::OfferSizeBand band = band_of(1.0, 5.0),
                         std::uint32_t max_tiers = 3u, std::uint32_t side = 6u)
{
    return pace::plan_tiers(cap, upb, kE12, 1.0, 5.0, band, max_tiers, side);
}

void expect_tiers(const pace::TierPlan& t, std::uint32_t tiers, Mojo size, Mojo min_tier, bool conflict)
{
    EXPECT_EQ(t.tiers, tiers);
    EXPECT_EQ(t.tier_size_base_mojos, size);
    EXPECT_EQ(t.min_tier_base_mojos, min_tier);
    EXPECT_EQ(t.conflict, conflict);
}

TEST(PacePlanTiers, LiveCase14Day) {
    expect_tiers(plan_live(3.6218550380699113), 2u, 1'213'067'300'154, kE12, false);
}

TEST(PacePlanTiers, LiveCase7Day) {
    expect_tiers(plan_live(7.04028150471125), 3u, 1'572'000'202'009, kE12, false);
}

TEST(PacePlanTiers, CapBelowMinSizeRestsNothing) {
    expect_tiers(plan_live(1.0), 0u, 0, 0, false);
}

TEST(PacePlanTiers, PairMinAboveHiIsConflict) {
    expect_tiers(plan_live(30.0, 1.49285, band_of(6.0, 5.0)), 0u, 0, 0, true);
}

TEST(PacePlanTiers, SizeClampedToHi) {
    expect_tiers(plan_live(30.0), 3u, 5'000'000'000'000, kE12, false);
}

TEST(PacePlanTiers, SideTierCountBinds) {
    expect_tiers(plan_live(3.6218550380699113, 1.49285, band_of(1.0, 5.0), 3u, 1u),
                 1u, 2'426'134'600'308, kE12, false);
}

TEST(PacePlanTiers, HugeCapFailsClosed) {
    expect_tiers(plan_live(1e300), 0u, 0, 0, false);
}

TEST(PacePlanTiers, MaxTiersClampedToSixteen) {
    expect_tiers(plan_live(149.285, 1.49285, band_of(1.0, 5.0), 20u, 20u), 16u, 5'000'000'000'000, kE12, false);
}

TEST(PacePlanTiers, BadDivisorRestsNothing) {
    {
        SCOPED_TRACE("upb 0");
        expect_tiers(plan_live(3.0, 0.0), 0u, 0, 0, false);
    }
    {
        SCOPED_TRACE("upb NaN");
        expect_tiers(plan_live(3.0, kNaN), 0u, 0, 0, false);
    }
}

// ============================================================================
// P15 -- plan {managed, tiers 2u, tier_size 1.2e12, pool 2.4e12, min_tier 1e12};
// inputs bid_after_floor 1e12, ask_after_floor 5e12, risk_bid 2.4e12, wallet
// cap known at 74e12, keep 1.0, drift 1.0 unless stated.
// ============================================================================

pace::PairPlan pools_plan()
{
    pace::PairPlan p{};
    p.managed              = true;
    p.tiers                = 2u;
    p.tier_size_base_mojos = 1'200'000'000'000;
    p.pool_base_mojos      = 2'400'000'000'000;
    p.min_tier_base_mojos  = kE12;
    return p;
}

pace::PoolInputs pools_in()
{
    pace::PoolInputs in{};
    in.bid_after_floor           = kE12;
    in.ask_after_floor           = 5'000'000'000'000;
    in.risk_bid                  = 2'400'000'000'000;
    in.wallet_cap_known          = true;
    in.wallet_cap_bid            = 74'000'000'000'000;
    in.wallet_concentration_keep = 1.0;
    in.drift_bid_scale           = 1.0;
    return in;
}

void expect_pools(const pace::ComposedPools& c, Mojo bid, Mojo ask, std::uint32_t tiers, Mojo size,
                  pace::PoolBinding binding)
{
    EXPECT_EQ(c.bid, bid);
    EXPECT_EQ(c.ask, ask);
    EXPECT_EQ(c.tiers, tiers);
    EXPECT_EQ(c.tier_size_base_mojos, size);
    EXPECT_EQ(c.binding, binding) << pace::to_string(c.binding);
}

TEST(PacePools, UnmanagedIsIdentity) {
    expect_pools(pace::compose_pace_pools(pace::PairPlan{}, pools_in()),
                 kE12, 5'000'000'000'000, 0u, 0, pace::PoolBinding::None);
}

TEST(PacePools, HoldZeroesBoth) {
    pace::PairPlan plan = pools_plan();
    plan.hold = true;
    expect_pools(pace::compose_pace_pools(plan, pools_in()), 0, 0, 0u, 0, pace::PoolBinding::Hold);
}

TEST(PacePools, PacePoolWhenNothingBinds) {
    expect_pools(pace::compose_pace_pools(pools_plan(), pools_in()),
                 2'400'000'000'000, 0, 2u, 1'200'000'000'000, pace::PoolBinding::Pace);
}

TEST(PacePools, RiskTaperBinds) {
    pace::PoolInputs in = pools_in();
    in.risk_bid = 1'260'000'000'000;
    expect_pools(pace::compose_pace_pools(pools_plan(), in),
                 1'200'000'000'000, 0, 1u, 1'200'000'000'000, pace::PoolBinding::Risk);
}

TEST(PacePools, RiskTaperBelowOneMinTierRestsNothing) {
    pace::PoolInputs in = pools_in();
    in.risk_bid = 120'000'000'000;
    expect_pools(pace::compose_pace_pools(pools_plan(), in), 0, 0, 0u, 0, pace::PoolBinding::MinTier);
}

TEST(PacePools, NeverExceedsRiskTaperedSide) {
    pace::PoolInputs in = pools_in();
    in.risk_bid = 2'100'000'000'000;
    expect_pools(pace::compose_pace_pools(pools_plan(), in),
                 2'100'000'000'000, 0, 2u, 1'050'000'000'000, pace::PoolBinding::Risk);
}

TEST(PacePools, WalletCapBinds) {
    pace::PoolInputs in = pools_in();
    in.wallet_cap_bid = 1'500'000'000'000;
    expect_pools(pace::compose_pace_pools(pools_plan(), in),
                 1'200'000'000'000, 0, 1u, 1'200'000'000'000, pace::PoolBinding::Wallet);
}

TEST(PacePools, UnknownWalletCapSkipped) {
    pace::PoolInputs in = pools_in();
    in.wallet_cap_known = false;
    in.wallet_cap_bid   = 0;
    expect_pools(pace::compose_pace_pools(pools_plan(), in),
                 2'400'000'000'000, 0, 2u, 1'200'000'000'000, pace::PoolBinding::Pace);
}

TEST(PacePools, WalletConcentrationKeepBinds) {
    pace::PoolInputs in = pools_in();
    in.wallet_concentration_keep = 0.5;
    expect_pools(pace::compose_pace_pools(pools_plan(), in),
                 1'200'000'000'000, 0, 1u, 1'200'000'000'000, pace::PoolBinding::WalletConcentration);
}

TEST(PacePools, WalletConcentrationKeepNaNFailsClosed) {
    pace::PoolInputs in = pools_in();
    in.wallet_concentration_keep = kNaN;
    expect_pools(pace::compose_pace_pools(pools_plan(), in), 0, 0, 0u, 0, pace::PoolBinding::WalletConcentration);
}

TEST(PacePools, DriftTaperBinds) {
    pace::PoolInputs in = pools_in();
    in.drift_bid_scale = 0.5;
    expect_pools(pace::compose_pace_pools(pools_plan(), in),
                 1'200'000'000'000, 0, 1u, 1'200'000'000'000, pace::PoolBinding::Drift);
}

TEST(PacePools, DriftScaleAtThresholdIgnored) {
    pace::PoolInputs in = pools_in();
    in.drift_bid_scale = 0.9995;
    expect_pools(pace::compose_pace_pools(pools_plan(), in),
                 2'400'000'000'000, 0, 2u, 1'200'000'000'000, pace::PoolBinding::Pace);
}

TEST(PacePools, DriftHardZeroStops) {
    pace::PoolInputs in = pools_in();
    in.drift_bid_scale = 0.0;
    expect_pools(pace::compose_pace_pools(pools_plan(), in), 0, 0, 0u, 0, pace::PoolBinding::Drift);
}

TEST(PacePools, DriftNaNFailsClosed) {
    pace::PoolInputs in = pools_in();
    in.drift_bid_scale = kNaN;
    expect_pools(pace::compose_pace_pools(pools_plan(), in), 0, 0, 0u, 0, pace::PoolBinding::Drift);
}

TEST(PacePools, DeployIdleFloorCannotRaisePool) {
    pace::PoolInputs in = pools_in();
    in.bid_after_floor = 5'000'000'000'000;
    in.ask_after_floor = 5'000'000'000'000;
    expect_pools(pace::compose_pace_pools(pools_plan(), in),
                 2'400'000'000'000, 0, 2u, 1'200'000'000'000, pace::PoolBinding::Pace);
}

TEST(PacePools, IncreasingSideAlwaysZero) {
    pace::PoolInputs in = pools_in();
    in.ask_after_floor = 5'000'000'000'000;
    EXPECT_EQ(pace::compose_pace_pools(pools_plan(), in).ask, 0);
}

// ============================================================================
// P16 (base kBase0, step 25 unless stated; prices +/-2 mojos)
// ============================================================================

TEST(PacePrice, BidTightensByQuantizedStep) {
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 150.0, 25.0, guards_g())),
                1'447'707'064'652.0, 2.0);
}

TEST(PacePrice, BidCappedBySigmaScaledEdge) {
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 500.0, 25.0, guards_gnc())),
                1'467'172'980'000.0, 2.0);
}

TEST(PacePrice, EdgeFloorUsedWhenSigmaSmall) {
    pace::PriceGuards g = guards_gnc();
    g.fv_sigma_bps = 20.0;
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 500.0, 25.0, g)),
                1'485'385'750'000.0, 2.0);
}

TEST(PacePrice, BidCappedByWidthFloor) {
    pace::PriceGuards g = guards_g();
    g.centre_px           = 1.47e12;
    g.min_half_spread_bps = 250.0;
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 500.0, 25.0, g)),
                1'433'250'000'000.0, 2.0);
}

TEST(PacePrice, BidNeverCrossesBestAsk) {
    // k = 1; every k >= 2 clamps to 1.43e12, which crosses the best ask.
    pace::PriceGuards g = guards_g();
    g.fair_value_px         = 2.0e12;
    g.fv_sigma_bps          = 0.0;
    g.centre_px             = 0.0;
    g.min_half_spread_bps   = 0.0;
    g.best_bid_px           = 0.0;
    g.best_ask_px           = 1.43e12;
    g.book_guard_margin_bps = 0.0;
    g.has_bbo               = false;
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 300.0, 25.0, g)),
                1'429'878'159'915.0, 2.0);
}

TEST(PacePrice, BidBacksOffUntilBboSanityPasses) {
    // k = 17: the higher candidates fail Step 8 Check 2 (aggressive deviation).
    pace::PriceGuards g = guards_g();
    g.fair_value_px         = 2.0e12;
    g.fv_sigma_bps          = 0.0;
    g.centre_px             = 0.0;
    g.min_half_spread_bps   = 0.0;
    g.best_bid_px           = 1.30e12;
    g.best_ask_px           = 3.3e12;
    g.book_guard_margin_bps = 0.0;
    g.published_mid_px      = 2.3e12;
    g.has_bbo               = true;
    g.bid_tier_ref_px       = 1.30e12;
    g.effective_mid_px      = 1.41e12;
    g.max_aggressive_dev    = 0.01;
    g.max_passive_dev       = 0.8;
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(1'350'000'000'000, 500.0, 25.0, g)),
                1'407'375'000'000.0, 2.0);
}

TEST(PacePrice, NeverMovesAPriceAlreadyPastTheBound) {
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(1'470'000'000'000, 300.0, 25.0, guards_gnc())),
                1'470'000'000'000.0, 2.0);
}

TEST(PacePrice, NaNFairValueReturnsBase) {
    pace::PriceGuards g = guards_g();
    g.fair_value_px = kNaN;
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 300.0, 25.0, g)),
                static_cast<double>(kBase0), 2.0);
}

TEST(PacePrice, NaNBookMarginFailsClosed) {
    pace::PriceGuards g = guards_g();
    g.book_guard_margin_bps = kNaN;
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 300.0, 25.0, g)),
                static_cast<double>(kBase0), 2.0);
}

TEST(PacePrice, ZeroTightenReturnsBase) {
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 0.0, 25.0, guards_g())),
                static_cast<double>(kBase0), 2.0);
}

TEST(PacePrice, StepOutOfRangeReturnsBase) {
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 300.0, 0.5, guards_g())),
                static_cast<double>(kBase0), 2.0);
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 300.0, 1001.0, guards_g())),
                static_cast<double>(kBase0), 2.0);
}

TEST(PacePrice, HugeTightenIsBounded) {
    EXPECT_NEAR(static_cast<double>(pace::apply_pace_bid_price(kBase0, 1e9, 1.0, guards_gnc())),
                1'467'172'980'000.0, 2.0);
}

// ============================================================================
// P17 (tighten 300, step 25, GNC unless stated; ladder sizes 1e12)
// ============================================================================

xop::TierQuote tier_quote(std::uint8_t tier, xop::Side side, Mojo price)
{
    xop::TierQuote t{};
    t.tier_index = tier;
    t.side       = side;
    t.price      = price;
    t.size       = kE12;
    t.spread_bps = 0.0;
    return t;
}

TEST(PaceTightenSide, CollidingTiersKeepStrictOrderWithoutLoosening) {
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'460'000'000'000),
                                       tier_quote(1, xop::Side::Bid, 1'455'000'000'000),
                                       tier_quote(0, xop::Side::Ask, 1'600'000'000'000)};
    const std::vector<Mojo> untight = pace::tighten_bid_side(ladder, 300.0, 25.0, guards_gnc());
    static_cast<void>(untight);
    ASSERT_EQ(ladder.size(), 3u);
    EXPECT_NEAR(static_cast<double>(ladder[0].price), 1'467'172'980'000.0, 2.0);
    EXPECT_NEAR(static_cast<double>(ladder[1].price), 1'465'705'807'020.0, 2.0);
    EXPECT_NEAR(static_cast<double>(ladder[2].price), 1'600'000'000'000.0, 2.0);
}

TEST(PaceTightenSide, TierBelowOwnBaseNeverLoosened) {
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'466'000'000'000),
                                       tier_quote(1, xop::Side::Bid, 1'466'000'000'000)};
    const std::vector<Mojo> untight = pace::tighten_bid_side(ladder, 300.0, 25.0, guards_gnc());
    static_cast<void>(untight);
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_NEAR(static_cast<double>(ladder[0].price), 1'467'172'980'000.0, 2.0);
    EXPECT_NEAR(static_cast<double>(ladder[1].price), 1'466'000'000'000.0, 2.0);
}

TEST(PaceTightenSide, SpreadBpsResynced) {
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, kBase0)};
    const std::vector<Mojo> untight = pace::tighten_bid_side(ladder, 150.0, 25.0, guards_g());
    static_cast<void>(untight);
    ASSERT_EQ(ladder.size(), 1u);
    EXPECT_NEAR(ladder[0].spread_bps, -306.6149, 1e-3);
}

TEST(PaceTightenSide, ReturnsUntightenedPrices) {
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'460'000'000'000),
                                       tier_quote(1, xop::Side::Bid, 1'455'000'000'000)};
    const std::vector<Mojo> untight = pace::tighten_bid_side(ladder, 300.0, 25.0, guards_gnc());
    EXPECT_EQ(untight, (std::vector<Mojo>{1'460'000'000'000, 1'455'000'000'000}));
}

TEST(PaceTightenSide, MovedTierKeepsTheOrderBookGuardSpacing) {
    // The engine passes the order-book guard's per-tier step (50 bps at the
    // default fair_value_clamp_tier_step_bps).  Tier 1 moves to 1.466295 but
    // does not collide with tier 0; it still has to stay 50 bps below it.
    pace::PriceGuards g = guards_gnc();
    g.tier_step_bps = 50.0;
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'460'000'000'000),
                                       tier_quote(1, xop::Side::Bid, 1'459'000'000'000)};
    const std::vector<Mojo> untight = pace::tighten_bid_side(ladder, 50.0, 25.0, g);
    static_cast<void>(untight);
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_NEAR(static_cast<double>(ladder[0].price), 1'467'172'980'000.0, 2.0);
    EXPECT_NEAR(static_cast<double>(ladder[1].price), 1'459'837'115'100.0, 2.0);
}

TEST(PaceTightenSide, SpreadBpsResyncedForEveryBid) {
    // Tier 1 cannot move (every candidate fails Check 2's passive bound), and
    // its spread_bps is still resynced against the centre.
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, kBase0),
                                       tier_quote(1, xop::Side::Bid, 300'000'000'000)};
    const std::vector<Mojo> untight = pace::tighten_bid_side(ladder, 150.0, 25.0, guards_g());
    static_cast<void>(untight);
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_NEAR(ladder[0].spread_bps, -306.615, 1e-3);
    EXPECT_NEAR(ladder[1].spread_bps, -7991.296, 1e-3);
    EXPECT_EQ(ladder[1].price, 300'000'000'000);
}

// ============================================================================
// P18 -- plan S {managed, tiers 2u, tier_size 1'213'067'300'154, min_tier 1e12,
// pool 2'426'134'600'308}; B6 = bids tier i at 1'426'312'378'968 - i x 5e10.
// ============================================================================

pace::PairPlan shape_plan()
{
    pace::PairPlan p{};
    p.managed              = true;
    p.tiers                = 2u;
    p.tier_size_base_mojos = 1'213'067'300'154;
    p.min_tier_base_mojos  = kE12;
    p.pool_base_mojos      = 2'426'134'600'308;
    return p;
}

std::vector<xop::TierQuote> six_bids(std::uint8_t first_tier)
{
    std::vector<xop::TierQuote> out;
    for (std::uint8_t i = first_tier; i < 6; ++i) {
        out.push_back(tier_quote(i, xop::Side::Bid, 1'426'312'378'968 - static_cast<Mojo>(i) * 50'000'000'000));
    }
    return out;
}

TEST(PaceShape, KeepsTiersBelowPlanCountAndSetsSizes) {
    std::vector<xop::TierQuote> ladder = six_bids(0);
    ladder.push_back(tier_quote(0, xop::Side::Ask, 1'600'000'000'000));
    ladder.push_back(tier_quote(1, xop::Side::Ask, 1'650'000'000'000));
    pace::shape_bid_side(ladder, shape_plan());
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_EQ(ladder[0].side, xop::Side::Bid);
    EXPECT_EQ(ladder[0].tier_index, 0u);
    EXPECT_EQ(ladder[0].price, 1'426'312'378'968);
    EXPECT_EQ(ladder[0].size, 1'213'067'300'154);
    EXPECT_EQ(ladder[1].side, xop::Side::Bid);
    EXPECT_EQ(ladder[1].tier_index, 1u);
    EXPECT_EQ(ladder[1].price, 1'376'312'378'968);
    EXPECT_EQ(ladder[1].size, 1'213'067'300'154);
}

TEST(PaceShape, MissingTierZeroDoesNotPromoteTierTwo) {
    std::vector<xop::TierQuote> ladder = six_bids(1);
    pace::shape_bid_side(ladder, shape_plan());
    ASSERT_EQ(ladder.size(), 1u);
    EXPECT_EQ(ladder[0].side, xop::Side::Bid);
    EXPECT_EQ(ladder[0].tier_index, 1u);
    EXPECT_EQ(ladder[0].price, 1'376'312'378'968);
    EXPECT_EQ(ladder[0].size, 1'213'067'300'154);
}

TEST(PaceShape, HoldErasesAllBidsAndAsks) {
    std::vector<xop::TierQuote> ladder = six_bids(0);
    ladder.push_back(tier_quote(0, xop::Side::Ask, 1'600'000'000'000));
    pace::PairPlan plan = shape_plan();
    plan.hold = true;
    pace::shape_bid_side(ladder, plan);
    EXPECT_TRUE(ladder.empty());
}

TEST(PaceShape, UnmanagedUntouched) {
    std::vector<xop::TierQuote> ladder = six_bids(0);
    pace::shape_bid_side(ladder, pace::PairPlan{});
    ASSERT_EQ(ladder.size(), 6u);
    for (const xop::TierQuote& t : ladder) {
        EXPECT_EQ(t.side, xop::Side::Bid);
        EXPECT_EQ(t.size, kE12);
    }
}

TEST(PaceShape, ThrottleScaleShrinksSize) {
    std::vector<xop::TierQuote> ladder = six_bids(0);
    pace::PairPlan plan = shape_plan();
    plan.throttle_bid_size_scale = 0.5;
    pace::shape_bid_side(ladder, plan);
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_EQ(ladder[0].tier_index, 0u);
    EXPECT_EQ(ladder[0].size, 606'533'650'077);
    EXPECT_EQ(ladder[1].tier_index, 1u);
    EXPECT_EQ(ladder[1].size, 606'533'650'077);
}

// ============================================================================
// P24 -- plan V: plan S with the live fair value 1.49285 and sigma 172;
// min_edge 50, k 1.0, spacing 50 (the order-book guard's step), centre
// 1.4935e12.  Plan X: fair value 1.5, sigma 0, min_edge 2000, so the cap is
// exactly 1'200'000'000'000.
// ============================================================================

pace::PairPlan cap_plan()
{
    pace::PairPlan p = shape_plan();
    p.fv_price     = 1.49285;
    p.fv_sigma_bps = 172.0;
    return p;
}

pace::FairValueCap cap_live(std::vector<xop::TierQuote>& ladder, const pace::PairPlan& plan)
{
    return pace::cap_bids_at_fair_value(ladder, plan, 50.0, 1.0, 50.0, 1.4935e12);
}

pace::FairValueCap cap_exact(std::vector<xop::TierQuote>& ladder)
{
    pace::PairPlan p = shape_plan();
    p.fv_price     = 1.5;
    p.fv_sigma_bps = 0.0;
    return pace::cap_bids_at_fair_value(ladder, p, 2000.0, 1.0, 50.0, 0.0);
}

TEST(PaceFairValueCap, StageBBidsAboveFairValueLoweredAndSpaced) {
    // Stage B (pace_tighten_max_bps 0): the price post-pass never runs, so a
    // centre-blend ladder above fair value would otherwise post as it is.
    pace::PairPlan plan = cap_plan();
    plan.tighten_bps = 0.0;
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'520'000'000'000),
                                       tier_quote(1, xop::Side::Bid, 1'500'000'000'000)};
    const pace::FairValueCap cap = cap_live(ladder, plan);
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_NEAR(static_cast<double>(ladder[0].price), 1'467'172'980'000.0, 2.0);
    EXPECT_NEAR(static_cast<double>(ladder[1].price), 1'459'837'115'100.0, 2.0);
    EXPECT_EQ(ladder[0].size, kE12);
    EXPECT_EQ(ladder[1].size, kE12);
    EXPECT_NEAR(static_cast<double>(cap.cap_px), 1'467'172'980'000.0, 2.0);
    EXPECT_EQ(cap.lowered, 2u);
    EXPECT_EQ(cap.dropped, 0u);
}

TEST(PaceFairValueCap, RampZeroPlanStillCapped) {
    // The first plan after an activation or a restart has ramp 0, so tighten
    // 0 (PaceDecide.LiveCase_Byc14DayPlansTwoTiers).  Tier 0 at the A-S
    // centre sits above fair value; tier 1 is below the cap and spaced.
    pace::PairPlan plan = cap_plan();
    plan.tighten_bps = 0.0;
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'493'500'000'000),
                                       tier_quote(1, xop::Side::Bid, 1'426'312'378'968)};
    const pace::FairValueCap cap = cap_live(ladder, plan);
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_NEAR(static_cast<double>(ladder[0].price), 1'467'172'980'000.0, 2.0);
    EXPECT_NEAR(static_cast<double>(ladder[1].price), 1'426'312'378'968.0, 2.0);
    EXPECT_EQ(cap.lowered, 1u);
    EXPECT_EQ(cap.dropped, 0u);
}

TEST(PaceFairValueCap, ThrottledHeartbeatShapedThenCapped) {
    // A throttled heartbeat skips the price post-pass even when behind
    // schedule, but not the size post-pass (P18); the cap follows it, in
    // Step 7's order.
    pace::PairPlan plan = cap_plan();
    plan.tighten_bps             = 300.0;
    plan.throttle_bid_size_scale = 0.5;
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'510'000'000'000),
                                       tier_quote(1, xop::Side::Bid, 1'505'000'000'000),
                                       tier_quote(2, xop::Side::Bid, 1'500'000'000'000),
                                       tier_quote(0, xop::Side::Ask, 1'600'000'000'000)};
    pace::shape_bid_side(ladder, plan);
    const pace::FairValueCap cap = cap_live(ladder, plan);
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_EQ(ladder[0].tier_index, 0u);
    EXPECT_EQ(ladder[1].tier_index, 1u);
    EXPECT_NEAR(static_cast<double>(ladder[0].price), 1'467'172'980'000.0, 2.0);
    EXPECT_NEAR(static_cast<double>(ladder[1].price), 1'459'837'115'100.0, 2.0);
    EXPECT_EQ(ladder[0].size, 606'533'650'077);
    EXPECT_EQ(ladder[1].size, 606'533'650'077);
    EXPECT_EQ(cap.lowered, 2u);
    EXPECT_EQ(cap.dropped, 0u);
}

TEST(PaceFairValueCap, BidsAtOrBelowTheCapKeepTheirPrices) {
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'200'000'000'000),
                                       tier_quote(1, xop::Side::Bid, 1'100'000'000'000)};
    const pace::FairValueCap cap = cap_exact(ladder);
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_EQ(ladder[0].price, 1'200'000'000'000);
    EXPECT_EQ(ladder[1].price, 1'100'000'000'000);
    EXPECT_EQ(cap.cap_px, 1'200'000'000'000);
    EXPECT_EQ(cap.lowered, 0u);
    EXPECT_EQ(cap.dropped, 0u);
}

TEST(PaceFairValueCap, OneMojoAboveTheCapIsLowered) {
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'200'000'000'001)};
    const pace::FairValueCap cap = cap_exact(ladder);
    ASSERT_EQ(ladder.size(), 1u);
    EXPECT_EQ(ladder[0].price, 1'200'000'000'000);
    EXPECT_EQ(cap.lowered, 1u);
    EXPECT_EQ(cap.dropped, 0u);
}

TEST(PaceFairValueCap, LowerTierKeepsTheGuardSpacing) {
    // Tier 1 sits 8.3 bps under tier 0: below the cap, but inside the 50-bps
    // step the order-book guard keeps between successive tiers.
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'200'000'000'000),
                                       tier_quote(1, xop::Side::Bid, 1'199'000'000'000)};
    const pace::FairValueCap cap = cap_exact(ladder);
    ASSERT_EQ(ladder.size(), 2u);
    EXPECT_EQ(ladder[0].price, 1'200'000'000'000);
    EXPECT_EQ(ladder[1].price, 1'194'000'000'000);
    EXPECT_EQ(cap.lowered, 1u);
    EXPECT_EQ(cap.dropped, 0u);
}

TEST(PaceFairValueCap, EdgeFloorUsedWhenSigmaSmall) {
    // sigma 20 is below min_edge 50: the cap is FV x (1 - 50 bps).
    pace::PairPlan plan = cap_plan();
    plan.fv_sigma_bps = 20.0;
    std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'490'000'000'000)};
    const pace::FairValueCap cap = cap_live(ladder, plan);
    ASSERT_EQ(ladder.size(), 1u);
    EXPECT_NEAR(static_cast<double>(ladder[0].price), 1'485'385'750'000.0, 2.0);
    EXPECT_NEAR(static_cast<double>(cap.cap_px), 1'485'385'750'000.0, 2.0);
    EXPECT_EQ(cap.lowered, 1u);
}

TEST(PaceFairValueCap, UnusableFairValueDropsEveryBid) {
    for (const double fv : {kNaN, 0.0}) {
        SCOPED_TRACE(fv);
        pace::PairPlan plan = cap_plan();
        plan.fv_price = fv;
        std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'400'000'000'000),
                                           tier_quote(1, xop::Side::Bid, 1'390'000'000'000),
                                           tier_quote(0, xop::Side::Ask, 1'600'000'000'000)};
        const pace::FairValueCap cap = cap_live(ladder, plan);
        ASSERT_EQ(ladder.size(), 1u);
        EXPECT_EQ(ladder[0].side, xop::Side::Ask);
        EXPECT_EQ(cap.cap_px, 0);
        EXPECT_EQ(cap.lowered, 0u);
        EXPECT_EQ(cap.dropped, 2u);
    }
}

TEST(PaceFairValueCap, UnmanagedAndHoldAreNoOps) {
    pace::PairPlan hold = cap_plan();
    hold.hold = true;
    for (const pace::PairPlan& plan : {pace::PairPlan{}, hold}) {
        std::vector<xop::TierQuote> ladder{tier_quote(0, xop::Side::Bid, 1'520'000'000'000),
                                           tier_quote(1, xop::Side::Bid, 1'500'000'000'000)};
        const pace::FairValueCap cap = cap_live(ladder, plan);
        ASSERT_EQ(ladder.size(), 2u);
        EXPECT_EQ(ladder[0].price, 1'520'000'000'000);
        EXPECT_EQ(ladder[1].price, 1'500'000'000'000);
        EXPECT_EQ(cap.cap_px, 0);
        EXPECT_EQ(cap.lowered, 0u);
        EXPECT_EQ(cap.dropped, 0u);
    }
}

// ============================================================================
// P19 -- plan C (plan S + resting_cap 3.6218550380699113, remaining
// 7.2437100761398225); base_mpu 1e12, quote_mpu 1000.  R6 is the live resting
// ladder (offer_log, 15:00); R2 is 19913 and 19969 only.
// ============================================================================

pace::PairPlan cancel_plan()
{
    pace::PairPlan p = shape_plan();
    p.resting_cap_units = 3.6218550380699113;
    p.remaining_units   = 7.2437100761398225;
    return p;
}

pace::RestingOffer resting_bid(const char* id, std::uint8_t tier, Mojo price)
{
    pace::RestingOffer r{};
    r.id    = id;
    r.side  = xop::Side::Bid;
    r.tier  = tier;
    r.price = price;
    r.size  = kE12;
    return r;
}

std::vector<pace::RestingOffer> live_resting()
{
    return {
        resting_bid("19913", 0, 1'426'312'378'968), resting_bid("19969", 1, 1'337'575'455'018),
        resting_bid("19915", 2, 1'278'916'653'910), resting_bid("19916", 3, 1'188'211'592'335),
        resting_bid("19917", 4, 1'086'168'398'063), resting_bid("19918", 5, 950'110'805'701),
    };
}

std::vector<pace::RestingOffer> live_resting_two()
{
    std::vector<pace::RestingOffer> r = live_resting();
    r.resize(2);
    return r;
}

pace::RestingOffer resting_ask_a0()
{
    pace::RestingOffer r{};
    r.id    = "a0";
    r.side  = xop::Side::Ask;
    r.tier  = 0;
    r.price = 1'600'000'000'000;
    r.size  = kE12;
    return r;
}

using Ids = std::vector<std::string>;

void expect_pick(const pace::CancelPick& pick, const Ids& budget, const Ids& tier, const Ids& increasing)
{
    EXPECT_EQ(pick.budget_ids, budget);
    EXPECT_EQ(pick.tier_ids, tier);
    EXPECT_EQ(pick.increasing_ids, increasing);
}

TEST(PaceCancel, LiveActivationCancelsAbsentTiersOnly) {
    expect_pick(pace::select_resting_to_cancel(live_resting(), cancel_plan(), kE12, 1000),
                Ids{}, Ids{"19915", "19916", "19917", "19918"}, Ids{});
}

TEST(PaceCancel, CapIsMinOfRestingCapAndRemaining) {
    pace::PairPlan plan = cancel_plan();
    plan.remaining_units = 2.0;
    expect_pick(pace::select_resting_to_cancel(live_resting_two(), plan, kE12, 1000), Ids{"19969"}, Ids{}, Ids{});
}

TEST(PaceCancel, RestingCapBindsWhenRemainingLarger) {
    pace::PairPlan plan = cancel_plan();
    plan.resting_cap_units = 1.5;
    expect_pick(pace::select_resting_to_cancel(live_resting_two(), plan, kE12, 1000), Ids{"19969"}, Ids{}, Ids{});
}

TEST(PaceCancel, BaseCapBindsAfterRiskComposition) {
    pace::PairPlan plan = cancel_plan();
    plan.pool_base_mojos = 1'500'000'000'000;
    expect_pick(pace::select_resting_to_cancel(live_resting_two(), plan, kE12, 1000), Ids{"19969"}, Ids{}, Ids{});
}

TEST(PaceCancel, ExhaustedCancelsAllBids) {
    pace::PairPlan plan = cancel_plan();
    plan.remaining_units = 0.0;
    expect_pick(pace::select_resting_to_cancel(live_resting_two(), plan, kE12, 1000),
                Ids{"19969", "19913"}, Ids{}, Ids{});
}

TEST(PaceCancel, IncreasingSideAlwaysCancelled) {
    std::vector<pace::RestingOffer> resting = live_resting_two();
    resting.push_back(resting_ask_a0());
    expect_pick(pace::select_resting_to_cancel(resting, cancel_plan(), kE12, 1000), Ids{}, Ids{}, Ids{"a0"});
}

TEST(PaceCancel, HoldCancelsNothing) {
    std::vector<pace::RestingOffer> resting = live_resting();
    resting.push_back(resting_ask_a0());
    pace::PairPlan plan = cancel_plan();
    plan.hold = true;
    expect_pick(pace::select_resting_to_cancel(resting, plan, kE12, 1000), Ids{}, Ids{}, Ids{});
}

TEST(PaceCancel, UnmanagedCancelsNothing) {
    expect_pick(pace::select_resting_to_cancel(live_resting(), pace::PairPlan{}, kE12, 1000), Ids{}, Ids{}, Ids{});
}

TEST(PaceCancel, CancelPendingIgnored) {
    std::vector<pace::RestingOffer> resting = live_resting();
    resting[2].cancel_pending = true;   // 19915
    expect_pick(pace::select_resting_to_cancel(resting, cancel_plan(), kE12, 1000),
                Ids{}, Ids{"19916", "19917", "19918"}, Ids{});
}

// ============================================================================
// P26 -- plan C with fair value 1.5, so the bound is exactly 1'500'000'000'000
// ============================================================================

pace::PairPlan above_fv_plan()
{
    pace::PairPlan p = cancel_plan();
    p.fv_price = 1.5;
    return p;
}

TEST(PaceRestingAboveFairValue, BidsAboveFairValueSelected) {
    std::vector<pace::RestingOffer> resting{resting_bid("b0", 0, 1'500'000'000'001),
                                            resting_bid("b1", 1, 1'500'000'000'000),
                                            resting_bid("b2", 2, 1'426'312'378'968), resting_ask_a0()};
    pace::RestingOffer pending = resting_bid("b3", 0, 1'600'000'000'000);
    pending.cancel_pending = true;
    resting.push_back(pending);
    EXPECT_EQ(pace::select_resting_above_fair_value(resting, above_fv_plan()), Ids{"b0"});
}

TEST(PaceRestingAboveFairValue, HoldUnmanagedOrUnusableFairValueSelectsNothing) {
    const std::vector<pace::RestingOffer> resting{resting_bid("b0", 0, 1'600'000'000'000)};
    pace::PairPlan hold = above_fv_plan();
    hold.hold = true;
    pace::PairPlan nan_fv = above_fv_plan();
    nan_fv.fv_price = kNaN;
    pace::PairPlan zero_fv = above_fv_plan();
    zero_fv.fv_price = 0.0;
    EXPECT_TRUE(pace::select_resting_above_fair_value(resting, hold).empty());
    EXPECT_TRUE(pace::select_resting_above_fair_value(resting, pace::PairPlan{}).empty());
    EXPECT_TRUE(pace::select_resting_above_fair_value(resting, nan_fv).empty());
    EXPECT_TRUE(pace::select_resting_above_fair_value(resting, zero_fv).empty());
}

// ============================================================================
// P20 (resting, desired, untightened, age; min_age 96u, min_bps 50), P21
// ============================================================================

TEST(PaceReprice, PaceTightenedPastRestingAndUntightenedReprices) {
    EXPECT_TRUE(pace::should_reprice_bid(1'430'000'000'000, 1'440'000'000'000, 1'426'300'000'000, 96u, 96u, 50.0));
}

TEST(PaceReprice, BaseLadderMoveAloneDoesNotReprice) {
    EXPECT_FALSE(pace::should_reprice_bid(1'401'800'000'000, 1'429'900'000'000, 1'426'300'000'000, 200u, 96u, 50.0));
}

TEST(PaceReprice, UntightenedTierNotRepriced) {
    EXPECT_FALSE(pace::should_reprice_bid(1'401'800'000'000, 1'426'300'000'000, 1'426'300'000'000, 200u, 96u, 50.0));
}

TEST(PaceReprice, YoungOfferNotRepriced) {
    EXPECT_FALSE(pace::should_reprice_bid(1'430'000'000'000, 1'440'000'000'000, 1'426'300'000'000, 95u, 96u, 50.0));
}

TEST(PaceReprice, SmallImprovementNotRepriced) {
    EXPECT_FALSE(pace::should_reprice_bid(1'430'000'000'000, 1'436'000'000'000, 1'426'300'000'000, 96u, 96u, 50.0));
}

TEST(PaceReprice, NonPositiveOrNaNGuards) {
    EXPECT_FALSE(pace::should_reprice_bid(0, 1'440'000'000'000, 1'426'300'000'000, 96u, 96u, 50.0));
    EXPECT_FALSE(pace::should_reprice_bid(1'430'000'000'000, 1'440'000'000'000, 1'426'300'000'000, 96u, 96u, kNaN));
}

TEST(PaceTake, TakeAtOrBelowFairValueWithinBudgetAllowed) {
    EXPECT_TRUE(pace::take_allowed(1.49, 1.49285, 2.98, 7.24));
}

TEST(PaceTake, TakeAboveFairValueRefused_LiveC4) {
    // v1's live example: 2 XCH at 2.68 BYC/XCH against a fair value of 1.49.
    EXPECT_FALSE(pace::take_allowed(2.68, 1.49285, 5.36, 7.24));
}

TEST(PaceTake, TakeAtExactFairValueAllowed) {
    EXPECT_TRUE(pace::take_allowed(1.5, 1.5, 1.0, 2.0));
}

TEST(PaceTake, AboveRemainingRefused) {
    EXPECT_FALSE(pace::take_allowed(1.4, 1.5, 3.0, 2.0));
}

TEST(PaceTake, ZeroRemainingRefused) {
    EXPECT_FALSE(pace::take_allowed(1.4, 1.5, 1.0, 0.0));
}

TEST(PaceTake, NonFiniteRefused) {
    EXPECT_FALSE(pace::take_allowed(kNaN, 1.5, 1.0, 2.0));
    EXPECT_FALSE(pace::take_allowed(1.4, 0.0, 1.0, 2.0));
}

// ============================================================================
// P25 -- the balance refresh's gates, age and per-asset backoff
// ============================================================================

pace::RefreshGates open_gates()
{
    pace::RefreshGates g{};
    g.pace_enabled       = true;
    g.flash_crash_normal = true;
    return g;
}

TEST(PaceRefreshGates, AllOpenAllowsRefresh) {
    EXPECT_TRUE(pace::pace_refresh_gates_open(open_gates()));
    EXPECT_FALSE(pace::pace_refresh_gates_open(pace::RefreshGates{}));
}

TEST(PaceRefreshGates, EachClosedGateBlocksRefresh) {
    pace::RefreshGates g = open_gates();
    g.pace_enabled = false;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "pace_enabled";
    g = open_gates();
    g.dry_run = true;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "dry_run";
    g = open_gates();
    g.wallet_circuit_open = true;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "wallet_circuit_open";
    g = open_gates();
    g.watchdog_fired = true;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "watchdog_fired";
    g = open_gates();
    g.wallet_consecutive_failures = 1u;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "wallet_consecutive_failures";
    g = open_gates();
    g.gui_pause = true;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "gui_pause";
    g = open_gates();
    g.breaker_pause = true;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "breaker_pause";
    g = open_gates();
    g.cancel_all_inflight = true;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "cancel_all_inflight";
    g = open_gates();
    g.cancel_all_draining = true;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "cancel_all_draining";
    g = open_gates();
    g.flash_crash_normal = false;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "flash_crash_normal";
    g = open_gates();
    g.xch_recovery = true;
    EXPECT_FALSE(pace::pace_refresh_gates_open(g)) << "xch_recovery";
}

TEST(PaceRefreshAge, HalfTheFreshnessBoundAtLeastOne) {
    EXPECT_EQ(pace::pace_refresh_age_blocks(20u), 10u);
    EXPECT_EQ(pace::pace_refresh_age_blocks(21u), 10u);
    EXPECT_EQ(pace::pace_refresh_age_blocks(3u), 1u);
    EXPECT_EQ(pace::pace_refresh_age_blocks(1u), 1u);
    EXPECT_EQ(pace::pace_refresh_age_blocks(0u), 1u);
}

TEST(PaceRefreshDue, MissingOrUnvalidatedEntryIsDue) {
    EXPECT_TRUE(pace::pace_refresh_due(false, false, 0u, false, 0u, kNow, 10u));
    EXPECT_TRUE(pace::pace_refresh_due(true, false, kNow, false, 0u, kNow, 10u));
}

TEST(PaceRefreshDue, DueFromHalfTheFreshnessBound) {
    EXPECT_FALSE(pace::pace_refresh_due(true, true, kNow - 9u, false, 0u, kNow, 10u));
    EXPECT_TRUE(pace::pace_refresh_due(true, true, kNow - 10u, false, 0u, kNow, 10u));
}

TEST(PaceRefreshDue, BacksOffAfterAnyAttempt) {
    // A stale entry whose last attempt, good or bad, was 9 blocks ago waits;
    // at 10 blocks it is retried.
    EXPECT_FALSE(pace::pace_refresh_due(true, true, kNow - 30u, true, kNow - 9u, kNow, 10u));
    EXPECT_TRUE(pace::pace_refresh_due(true, true, kNow - 30u, true, kNow - 10u, kNow, 10u));
}

TEST(PaceRefreshDue, HeightRegressionWaits) {
    EXPECT_FALSE(pace::pace_refresh_due(true, true, kNow + 5u, false, 0u, kNow, 10u));
    EXPECT_FALSE(pace::pace_refresh_due(false, false, 0u, true, kNow + 5u, kNow, 10u));
}

// ============================================================================
// P22 (live fixture unless stated; "plan" is pairs.at("XCH/BYC"), "status" is
// assets[0].status)
// ============================================================================

void expect_memory(const pace::AssetMemory& m, bool active, std::uint32_t ramp, BlockHeight last)
{
    EXPECT_EQ(m.active, active);
    EXPECT_EQ(m.ramp_blocks, ramp);
    EXPECT_EQ(m.last_block, last);
}

pace::PaceStatus status_of(const pace::PaceDecision& d)
{
    // One decision per pace asset (the fixture paces BYC only).  A decision
    // that was never produced must fail the test, not read as DataUnavailable.
    EXPECT_EQ(d.assets.size(), 1u);
    return d.assets.empty() ? pace::PaceStatus::DataUnavailable : d.assets[0].status;
}

TEST(PaceDecide, LiveCase_Byc14DayPlansTwoTiers) {
    const pace::PaceDecision d = pace::decide(live_inputs());
    ASSERT_FALSE(d.assets.empty());
    EXPECT_EQ(d.assets[0].status, pace::PaceStatus::Active);
    expect_memory(d.memory.at("BYC"), true, 0u, 9'287'678u);
    EXPECT_NEAR(d.remaining_units.at("BYC"), 7.2437100761398225, 1e-9);
    const pace::PairPlan& plan = d.pairs.at("XCH/BYC");
    EXPECT_TRUE(plan.managed);
    EXPECT_FALSE(plan.hold);
    EXPECT_EQ(plan.tiers, 2u);
    EXPECT_EQ(plan.tier_size_base_mojos, 1'213'067'300'154);
    EXPECT_EQ(plan.pool_base_mojos, 2'426'134'600'308);
    EXPECT_NEAR(plan.resting_cap_units, 3.6218550380699113, 1e-9);
    EXPECT_NEAR(plan.remaining_units, 7.2437100761398225, 1e-9);
    EXPECT_NEAR(plan.tighten_bps, 0.0, 1e-9);
    EXPECT_EQ(d.pairs.count("XCH/DBX"), 0u);
    EXPECT_EQ(d.pairs.count("BYC/wUSDC.b"), 0u);
    EXPECT_NEAR(d.assets[0].budget_window_units, 7.2437100761398225, 1e-9);
    EXPECT_NEAR(d.assets[0].excess_units, 98.56394106595751, 1e-9);
}

pace::PaceInputs hold_step_inputs()
{
    pace::PaceInputs in = live_inputs();
    in.holdings = edit_holding(live_holdings(), "BYC", [](pace::HoldingInput& x) { x.fv_present = false; });
    in.memory["BYC"] = memory_of(true, 2'304u, 9'287'578u);
    return in;
}

TEST(PaceDecide, HoldWhenFairValueVanishesWhileActive) {
    const pace::PaceDecision d = pace::decide(hold_step_inputs());
    EXPECT_EQ(status_of(d), pace::PaceStatus::Hold);
    const pace::PairPlan& plan = d.pairs.at("XCH/BYC");
    EXPECT_TRUE(plan.managed);
    EXPECT_TRUE(plan.hold);
    expect_memory(d.memory.at("BYC"), true, 2'304u, 9'287'678u);
    EXPECT_NEAR(d.remaining_units.at("BYC"), 0.0, 1e-9);
}

TEST(PaceDecide, HoldPausesRampThenResumes) {
    const pace::PaceDecision a = pace::decide(hold_step_inputs());
    pace::PaceInputs in = live_inputs();
    in.now    = 9'287'688u;
    in.memory = a.memory;
    const pace::PaceDecision b = pace::decide(in);
    expect_memory(b.memory.at("BYC"), true, 2'314u, 9'287'688u);
}

TEST(PaceDecide, InertWhenFairValueMissingAndNotActive) {
    pace::PaceInputs in = live_inputs();
    in.holdings = edit_holding(live_holdings(), "BYC", [](pace::HoldingInput& x) { x.fv_present = false; });
    const pace::PaceDecision d = pace::decide(in);
    EXPECT_EQ(status_of(d), pace::PaceStatus::DataUnavailable);
    EXPECT_EQ(d.pairs.size(), 0u);
    expect_memory(d.memory.at("BYC"), false, 0u, 0u);
}

TEST(PaceDecide, UnvalidatedZeroBalanceIsDataUnavailable) {
    pace::PaceInputs in = live_inputs();
    in.holdings = edit_holding(live_holdings(), "DBX", [](pace::HoldingInput& x) {
        x.units = 0.0;
        x.fields_validated = false;
    });
    EXPECT_EQ(status_of(pace::decide(in)), pace::PaceStatus::DataUnavailable);
}

TEST(PaceDecide, MissingCacheEntryIsDataUnavailable) {
    pace::PaceInputs in = live_inputs();
    in.holdings = edit_holding(live_holdings(), "XCH", [](pace::HoldingInput& x) { x.cache_present = false; });
    EXPECT_EQ(status_of(pace::decide(in)), pace::PaceStatus::DataUnavailable);
}

TEST(PaceDecide, InsideBandDeactivatesAndClearsMemory) {
    pace::PaceInputs in = live_inputs();
    in.holdings = edit_holding(live_holdings(), "BYC", [](pace::HoldingInput& x) { x.units = 3.0; });
    in.memory["BYC"] = memory_of(true, 4'608u, 9'287'668u);
    const pace::PaceDecision d = pace::decide(in);
    EXPECT_EQ(status_of(d), pace::PaceStatus::Inactive);
    expect_memory(d.memory.at("BYC"), false, 0u, 0u);
}

TEST(PaceDecide, ExhaustedWhenSoldWindowCoversBudget) {
    pace::PaceInputs in = live_inputs();
    in.fills.push_back(maker_row("XCH/BYC", "bid", 5'000'000'000'000, 1'600'000'000'000, 9'287'668u));
    const pace::PaceDecision d = pace::decide(in);
    EXPECT_EQ(status_of(d), pace::PaceStatus::Exhausted);
    const pace::PairPlan& plan = d.pairs.at("XCH/BYC");
    EXPECT_TRUE(plan.managed);
    EXPECT_FALSE(plan.hold);
    EXPECT_EQ(plan.tiers, 0u);
    EXPECT_EQ(plan.tier_size_base_mojos, 0);
    EXPECT_EQ(plan.pool_base_mojos, 0);
    EXPECT_NEAR(plan.resting_cap_units, 0.0, 1e-9);
    EXPECT_NEAR(plan.remaining_units, 0.0, 1e-9);
    EXPECT_NEAR(plan.tighten_bps, 0.0, 1e-9);
    EXPECT_NEAR(d.remaining_units.at("BYC"), 0.0, 1e-9);
}

TEST(PaceDecide, TightenReflectsSchedule) {
    pace::PaceInputs in = live_inputs();
    in.memory["BYC"] = memory_of(true, 4'608u, 9'287'678u);
    const pace::PaceDecision a = pace::decide(in);
    ASSERT_FALSE(a.assets.empty());
    EXPECT_NEAR(a.assets[0].tighten_bps, 300.0, 1e-9);
    // 5.0 BYC sold today at 1.6 BYC/XCH.
    in.fills.push_back(maker_row("XCH/BYC", "bid", 3'125'000'000'000, 1'600'000'000'000, 9'287'668u));
    const pace::PaceDecision b = pace::decide(in);
    ASSERT_FALSE(b.assets.empty());
    EXPECT_NEAR(b.assets[0].tighten_bps, 100.0, 1e-9);
}

TEST(PaceDecide, RampDoesNotAdvanceWhenPostingStopped) {
    pace::PaceInputs in = live_inputs();
    in.ramp_running  = false;
    in.memory["BYC"] = memory_of(true, 100u, 9'287'628u);
    expect_memory(pace::decide(in).memory.at("BYC"), true, 100u, 9'287'678u);
}

TEST(PaceDecide, HeightRegressionDoesNotWrapRamp) {
    pace::PaceInputs in = live_inputs();
    in.memory["BYC"] = memory_of(true, 100u, 9'287'728u);
    expect_memory(pace::decide(in).memory.at("BYC"), true, 100u, 9'287'678u);
}

TEST(PaceDecide, FillsUnavailableIsDataUnavailable) {
    pace::PaceInputs in = live_inputs();
    in.pairs = edit_pair(live_pairs(), "XCH/BYC", [](pace::PairInput& x) { x.fills_ok = false; });
    EXPECT_EQ(status_of(pace::decide(in)), pace::PaceStatus::DataUnavailable);
}

TEST(PaceDecide, FillsUnavailableOnDisabledPairHoldsWhenActive) {
    pace::PaceInputs in = live_inputs();
    in.pairs = edit_pair(live_pairs(), "BYC/wUSDC.b", [](pace::PairInput& x) { x.fills_ok = false; });
    in.memory["BYC"] = memory_of(true, 10u, 9'287'673u);
    const pace::PaceDecision d = pace::decide(in);
    EXPECT_EQ(status_of(d), pace::PaceStatus::Hold);
    EXPECT_TRUE(d.pairs.at("XCH/BYC").hold);
}

TEST(PaceDecide, PairNotQuoteValidHolds) {
    pace::PaceInputs in = live_inputs();
    in.pairs = edit_pair(live_pairs(), "XCH/BYC", [](pace::PairInput& x) { x.quote_valid = false; });
    const pace::PaceDecision d = pace::decide(in);
    EXPECT_EQ(status_of(d), pace::PaceStatus::Active);
    EXPECT_TRUE(d.pairs.at("XCH/BYC").managed);
    EXPECT_TRUE(d.pairs.at("XCH/BYC").hold);
}

TEST(PaceDecide, PairFairValueNotOkHolds) {
    pace::PaceInputs in = live_inputs();
    in.pairs = edit_pair(live_pairs(), "XCH/BYC", [](pace::PairInput& x) { x.fv_ok = false; });
    const pace::PaceDecision d = pace::decide(in);
    EXPECT_EQ(status_of(d), pace::PaceStatus::Active);
    EXPECT_TRUE(d.pairs.at("XCH/BYC").managed);
    EXPECT_TRUE(d.pairs.at("XCH/BYC").hold);
}

TEST(PaceDecide, AcquiredAssetWithoutHeadroomRestsNothing) {
    pace::PaceInputs in = live_inputs();
    in.holdings = edit_holding(live_holdings(), "XCH", [](pace::HoldingInput& x) {
        x.target = 0.10;
        x.tol = 0.05;
    });
    const pace::PaceDecision d = pace::decide(in);
    EXPECT_EQ(status_of(d), pace::PaceStatus::Active);
    const pace::PairPlan& plan = d.pairs.at("XCH/BYC");
    EXPECT_FALSE(plan.hold);
    EXPECT_EQ(plan.tiers, 0u);
    EXPECT_NEAR(plan.resting_cap_units, 0.0, 1e-9);
    EXPECT_NEAR(d.remaining_units.at("BYC"), 0.0, 1e-9);
}

TEST(PaceDecide, DisabledProducesNothingAndClearsMemory) {
    pace::PaceInputs in = live_inputs();
    in.params.enabled = false;
    in.memory["BYC"] = memory_of(true, 4'608u, 9'287'678u);
    const pace::PaceDecision d = pace::decide(in);
    EXPECT_EQ(d.memory.size(), 0u);
    EXPECT_EQ(d.remaining_units.size(), 0u);
    EXPECT_EQ(d.pairs.size(), 0u);
}

TEST(PaceDecide, FillOnDisabledPairTouchingAssetCounts) {
    // Flows on a pair disabled since still moved the wallet.
    pace::PaceInputs in = live_inputs();
    in.fills.push_back(maker_row("BYC/wUSDC.b", "ask", 5000, 1'002'000'000'000, 9'287'578u));
    const pace::PaceDecision d = pace::decide(in);
    ASSERT_FALSE(d.assets.empty());
    EXPECT_NEAR(d.assets[0].sold_window_units, 5.0, 1e-9);
    EXPECT_NEAR(d.assets[0].reduced_horizon_units, 7.848, 1e-9);
    EXPECT_NEAR(d.assets[0].budget_window_units, 7.600852933282679, 1e-9);
}

// ============================================================================
// P23 -- plan RP (the live 14-day plan), LIVE_PARAMS, base_mpu 1e12
// ============================================================================

pace::PairPlan replan_plan()
{
    pace::PairPlan p{};
    p.managed              = true;
    p.asset                = "BYC";
    p.tiers                = 2u;
    p.tier_size_base_mojos = 1'213'067'300'154;
    p.pool_base_mojos      = 2'426'134'600'308;
    p.min_tier_base_mojos  = kE12;
    p.resting_cap_units    = 3.6218550380699113;
    p.remaining_units      = 7.2437100761398225;
    p.fv_price             = 1.49285;
    p.band                 = band_of(1.0, 5.0);
    p.side_tier_count      = 6u;
    p.eligible_pairs       = 1u;
    return p;
}

TEST(PaceReplan, TakeShrinksTiersNeverGrows) {
    pace::PairPlan shrunk = replan_plan();
    pace::replan_after_take(shrunk, 1.5, live_params(), kE12);
    EXPECT_EQ(shrunk.tiers, 1u);
    EXPECT_EQ(shrunk.tier_size_base_mojos, 1'004'789'496'600);
    EXPECT_EQ(shrunk.pool_base_mojos, 1'004'789'496'600);

    pace::PairPlan capped = replan_plan();
    capped.tiers           = 1u;
    capped.pool_base_mojos = 1'213'067'300'154;
    pace::replan_after_take(capped, 100.0, live_params(), kE12);
    EXPECT_EQ(capped.tiers, 1u);
    EXPECT_EQ(capped.tier_size_base_mojos, 1'213'067'300'154);
    EXPECT_EQ(capped.pool_base_mojos, 1'213'067'300'154);
}

}  // namespace
