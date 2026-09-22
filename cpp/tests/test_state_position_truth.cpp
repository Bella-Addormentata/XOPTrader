// test_state_position_truth.cpp -- [SEED-FAIL-CLOSED 2026-09-22]
//
// State's positions are what the risk limits read.  On 2026-09-22 every
// startup balance read timed out, State began empty, fills made its positions
// small but non-zero, and Step 8's recovery -- which fired only on an
// exactly-zero position -- never fired.  These tests pin the three pieces of
// the fix (risk/state_position_truth.hpp):
//
//   * State::reconcile_balance() overwrites a balance and keeps the basis;
//   * decide_state_seed() never turns an unread balance into zero;
//   * apply_wallet_truth() corrects State whatever it held.
//
// The Incident tests replay the day's fills with their logged amounts and the
// logged XCH/DBX mid, and reproduce the two figures the engine acted on.

#include <gtest/gtest.h>

#include <xop/config.hpp>
#include <xop/risk/limits.hpp>
#include <xop/risk/state_position_truth.hpp>
#include <xop/state.hpp>
#include <xop/types.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace {

using xop::Mojo;
using xop::Position;
using xop::State;
using xop::risk::apply_wallet_truth;
using xop::risk::decide_state_seed;
using xop::risk::SeedSource;
using xop::risk::TruthOutcome;
using xop::risk::TruthResult;

constexpr Mojo kMojosPerXch = 1'000'000'000'000;
constexpr Mojo kMojosPerCat = 1'000;

// The two CATs of 2026-09-22, as the log names them.
const xop::AssetId kByc{
    "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac"};
const xop::AssetId kDbx{
    "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20"};
const xop::AssetId kXch{"xch"};

// ---------------------------------------------------------------------------
// State::reconcile_balance
// ---------------------------------------------------------------------------

TEST(StateReconcileBalance, AbsentAssetIsCreatedAtTheSeedUnitBasis)
{
    State state;
    const std::optional<Mojo> previous = state.reconcile_balance(kDbx, 500);
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(*previous, Mojo{0});

    const Position pos = state.get_position(kDbx);
    EXPECT_EQ(pos.balance, Mojo{500});
    EXPECT_EQ(pos.cost_basis, Mojo{1});
    EXPECT_DOUBLE_EQ(pos.total_cost, 500.0);
}

TEST(StateReconcileBalance, AbsentAssetObservedAtZeroGetsNoEntry)
{
    State state;
    const std::optional<Mojo> previous = state.reconcile_balance(kDbx, 0);
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(*previous, Mojo{0});
    EXPECT_TRUE(state.get_all_positions().empty());
}

TEST(StateReconcileBalance, OverwritesANonZeroPosition)
{
    // The case the old recovery could not reach: a position fills made
    // small but not zero.
    State state;
    state.record_buy(kXch, 1'103'203'898'833, 1'689'624'195'465);

    const std::optional<Mojo> previous =
        state.reconcile_balance(kXch, 34'696 * (kMojosPerXch / 1'000));
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(*previous, Mojo{1'103'203'898'833});
    EXPECT_EQ(state.get_position(kXch).balance, 34'696 * (kMojosPerXch / 1'000));
}

TEST(StateReconcileBalance, KeepsTheWeightedAverageBasisBothWays)
{
    State state;
    state.record_buy(kDbx, 100, 5);
    ASSERT_EQ(state.get_position(kDbx).cost_basis, Mojo{5});

    ASSERT_TRUE(state.reconcile_balance(kDbx, 300).has_value());
    Position pos = state.get_position(kDbx);
    EXPECT_EQ(pos.balance, Mojo{300});
    EXPECT_EQ(pos.cost_basis, Mojo{5});
    EXPECT_DOUBLE_EQ(pos.total_cost, 1500.0);

    ASSERT_TRUE(state.reconcile_balance(kDbx, 30).has_value());
    pos = state.get_position(kDbx);
    EXPECT_EQ(pos.balance, Mojo{30});
    EXPECT_EQ(pos.cost_basis, Mojo{5});
    EXPECT_DOUBLE_EQ(pos.total_cost, 150.0);
}

TEST(StateReconcileBalance, ZeroEmptiesThePositionAndAReturnTakesTheUnitBasis)
{
    State state;
    state.record_buy(kDbx, 100, 5);

    const std::optional<Mojo> previous = state.reconcile_balance(kDbx, 0);
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(*previous, Mojo{100});
    Position pos = state.get_position(kDbx);
    EXPECT_EQ(pos.balance, Mojo{0});
    EXPECT_EQ(pos.cost_basis, Mojo{0});
    EXPECT_DOUBLE_EQ(pos.total_cost, 0.0);

    // Nothing left to take a basis from.
    ASSERT_TRUE(state.reconcile_balance(kDbx, 200).has_value());
    pos = state.get_position(kDbx);
    EXPECT_EQ(pos.balance, Mojo{200});
    EXPECT_EQ(pos.cost_basis, Mojo{1});
}

TEST(StateReconcileBalance, NegativeIsRejectedAndChangesNothing)
{
    State state;
    state.record_buy(kDbx, 100, 5);

    EXPECT_FALSE(state.reconcile_balance(kDbx, -1).has_value());
    const Position pos = state.get_position(kDbx);
    EXPECT_EQ(pos.balance, Mojo{100});
    EXPECT_EQ(pos.cost_basis, Mojo{5});
    EXPECT_DOUBLE_EQ(pos.total_cost, 500.0);

    // And an absent asset stays absent.
    EXPECT_FALSE(state.reconcile_balance(kByc, -1).has_value());
    EXPECT_EQ(state.get_all_positions().size(), std::size_t{1});
}

TEST(StateReconcileBalance, AnEqualBalanceReturnsItAndChangesNothing)
{
    State state;
    state.record_buy(kDbx, 100, 5);

    const std::optional<Mojo> previous = state.reconcile_balance(kDbx, 100);
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(*previous, Mojo{100});
    EXPECT_EQ(state.get_position(kDbx).cost_basis, Mojo{5});
    EXPECT_DOUBLE_EQ(state.get_position(kDbx).total_cost, 500.0);
}

TEST(StateReconcileBalance, AReconciledPositionTakesFillsNormally)
{
    // On 2026-09-22 both fills' record_sell legs failed with "unknown
    // asset".  A seeded position -- from the wallet or the fallback -- is an
    // ordinary position.
    State state;
    ASSERT_TRUE(state.reconcile_balance(kXch, 10 * kMojosPerXch).has_value());
    EXPECT_TRUE(state.record_sell(kXch, 4 * kMojosPerXch));
    EXPECT_EQ(state.get_position(kXch).balance, 6 * kMojosPerXch);
}

// ---------------------------------------------------------------------------
// decide_state_seed
// ---------------------------------------------------------------------------

TEST(DecideStateSeed, AWalletReadIsVerified)
{
    const auto d = decide_state_seed(Mojo{500}, false, 900);
    EXPECT_EQ(d.source, SeedSource::Wallet);
    EXPECT_EQ(d.quantity, Mojo{500});
}

TEST(DecideStateSeed, AZeroTheWalletReportedIsAVerifiedZero)
{
    const auto d = decide_state_seed(Mojo{0}, false, 900);
    EXPECT_EQ(d.source, SeedSource::Wallet);
    EXPECT_EQ(d.quantity, Mojo{0});
}

TEST(DecideStateSeed, AnUnreadBalanceIsNeverZero)
{
    const auto d = decide_state_seed(std::nullopt, false, 900);
    EXPECT_EQ(d.source, SeedSource::LastKnown);
    EXPECT_EQ(d.quantity, Mojo{900});
}

TEST(DecideStateSeed, AnUnreadBalanceWithNoRecordIsStillUnverified)
{
    const auto d = decide_state_seed(std::nullopt, false, 0);
    EXPECT_EQ(d.source, SeedSource::LastKnown);
    EXPECT_EQ(d.quantity, Mojo{0});
}

TEST(DecideStateSeed, ABuiltMapWithNoWalletIsAVerifiedZero)
{
    const auto d = decide_state_seed(std::nullopt, true, 900);
    EXPECT_EQ(d.source, SeedSource::NotHeld);
    EXPECT_EQ(d.quantity, Mojo{0});
}

TEST(DecideStateSeed, AReadOutranksTheWalletMap)
{
    const auto d = decide_state_seed(Mojo{5}, true, 0);
    EXPECT_EQ(d.source, SeedSource::Wallet);
    EXPECT_EQ(d.quantity, Mojo{5});
}

TEST(DecideStateSeed, ANegativeWalletValueIsNotARead)
{
    const auto d = decide_state_seed(Mojo{-1}, false, 900);
    EXPECT_EQ(d.source, SeedSource::LastKnown);
    EXPECT_EQ(d.quantity, Mojo{900});
}

TEST(DecideStateSeed, ANegativeLastKnownClampsToZero)
{
    const auto d = decide_state_seed(std::nullopt, false, -3);
    EXPECT_EQ(d.source, SeedSource::LastKnown);
    EXPECT_EQ(d.quantity, Mojo{0});
}

// ---------------------------------------------------------------------------
// apply_wallet_truth
// ---------------------------------------------------------------------------

TEST(ApplyWalletTruth, CorrectsWhateverStateHeld)
{
    // Absent, empty, below, equal to and above the wallet: every one ends at
    // the wallet's balance.  The old recovery reached only the first two.
    constexpr Mojo kWallet = 2'166'963;
    struct Case {
        const char*         name;
        std::optional<Mojo> held;
    };
    const std::vector<Case> cases = {
        {"absent", std::nullopt},
        {"zero", Mojo{0}},
        {"below", Mojo{203'188}},
        {"equal", kWallet},
        {"above", Mojo{5'000'000}},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        State state;
        if (c.held.has_value()) {
            ASSERT_TRUE(state.reconcile_balance(kDbx, *c.held).has_value());
        }
        const TruthResult r = apply_wallet_truth(state, kDbx, kWallet, true, false);
        EXPECT_EQ(state.get_position(kDbx).balance, kWallet);
        EXPECT_EQ(r.previous, c.held.value_or(0));
        EXPECT_EQ(r.outcome, c.held == kWallet ? TruthOutcome::Unchanged
                                               : TruthOutcome::Corrected);
    }
}

TEST(ApplyWalletTruth, AValidatedZeroEmptiesThePosition)
{
    State state;
    state.record_buy(kDbx, 100, 5);
    const TruthResult r = apply_wallet_truth(state, kDbx, 0, true, false);
    EXPECT_EQ(r.outcome, TruthOutcome::Corrected);
    EXPECT_EQ(r.previous, Mojo{100});
    EXPECT_EQ(state.get_position(kDbx).balance, Mojo{0});
}

TEST(ApplyWalletTruth, AReadMissingItsFieldsChangesNothing)
{
    // A writer defaults a missing field to 0; that zero is not a balance.
    State state;
    state.record_buy(kXch, 5 * kMojosPerXch, 1);
    const TruthResult r = apply_wallet_truth(state, kXch, 0, false, false);
    EXPECT_EQ(r.outcome, TruthOutcome::Skipped);
    EXPECT_EQ(state.get_position(kXch).balance, 5 * kMojosPerXch);
}

TEST(ApplyWalletTruth, TheBridgeAssetKeepsItsSingleWriter)
{
    State state;
    state.record_buy(kDbx, 100, 5);
    const TruthResult r = apply_wallet_truth(state, kDbx, 900, true, true);
    EXPECT_EQ(r.outcome, TruthOutcome::Skipped);
    EXPECT_EQ(state.get_position(kDbx).balance, Mojo{100});
}

TEST(ApplyWalletTruth, ANegativeBalanceChangesNothing)
{
    State state;
    state.record_buy(kDbx, 100, 5);
    const TruthResult r = apply_wallet_truth(state, kDbx, -5, true, false);
    EXPECT_EQ(r.outcome, TruthOutcome::Skipped);
    EXPECT_EQ(state.get_position(kDbx).balance, Mojo{100});
}

// ---------------------------------------------------------------------------
// 2026-09-22, replayed
// ---------------------------------------------------------------------------

// The XCH/DBX mid the engine logged from 17:14 (Step 6, "market
// mid=83.510771"), converted as the Step 1 rate loop converts it.  The
// 17:09:29 figure below reproduces with it as well.
double dbx_rate()
{
    return static_cast<double>(kMojosPerXch)
         / (83.510771 * static_cast<double>(kMojosPerCat));
}

// BYC has no logged mid: this one is the 17:09:29 fill's price, 1.689624
// BYC per XCH.  Illustrative, and used only where the text says so.
double byc_rate()
{
    return static_cast<double>(kMojosPerXch)
         / (1.689624195465 * static_cast<double>(kMojosPerCat));
}

// The two fills of 17:09:29 as detect_fills() booked them into State.
void book_first_fills(State& state)
{
    // FILL ASK XCH/DBX 1 XCH @ 102.976 DBX: sell the XCH, buy the DBX.
    EXPECT_FALSE(state.record_sell(kXch, kMojosPerXch));   // "unknown asset=xch"
    state.record_buy(kDbx, 102'976, 1);
    // FILL BID XCH/BYC 1.103 XCH @ 1.689624 BYC: buy the XCH, sell the BYC.
    state.record_buy(kXch, 1'103'203'898'833, 1'689'624'195'465);
    EXPECT_FALSE(state.record_sell(kByc, 1'864));          // "unknown asset=ae15..."
}

// The fill of 17:13:39: FILL ASK XCH/DBX 1 XCH @ 100.212 DBX.
void book_third_fill(State& state)
{
    EXPECT_TRUE(state.record_sell(kXch, kMojosPerXch));
    state.record_buy(kDbx, 100'212, 1);
}

// The risk limits as config.yaml ran them that evening (the Step 6 line logs
// "cfg_soft=0.600 cfg_hard=0.800 cfg_cat=0.250 cfg_pair=0.850").
struct Incident20260922 : public ::testing::Test {
    xop::RiskConfig     risk_cfg;
    xop::StrategyConfig strat_cfg;
    State               state;

    void SetUp() override {
        risk_cfg.soft_limit_pct           = 0.60;
        risk_cfg.hard_limit_pct           = 0.80;
        risk_cfg.single_cat_cap_pct       = 0.25;
        risk_cfg.max_capital_per_pair_pct = 0.85;
        strat_cfg.min_profit_margin_bps   = 35.0;
        state.set_asset_xch_rate(kDbx, dbx_rate());
        state.set_asset_xch_rate(kByc, byc_rate());
    }

    // What Step 6 reads for XCH/DBX: quote_cat_pct is cat_portfolio_pct.
    xop::LimitStatus xch_dbx() const {
        const xop::PreTradeCheck ptc(risk_cfg, strat_cfg);
        return ptc.get_limit_status(kXch, kDbx, state);
    }
};

TEST_F(Incident20260922, TheFillsAloneReproduceTheFiguresTheEngineActedOn)
{
    // The defect, as arithmetic: a State built only from fills.
    book_first_fills(state);
    // Logged at 17:09:29: "quote_cat_pct=0.528 (full block at 0.500)".
    EXPECT_NEAR(xch_dbx().cat_portfolio_pct, 0.528, 0.0005);
    EXPECT_TRUE(xch_dbx().cat_cap_breached);

    book_third_fill(state);
    EXPECT_EQ(state.get_position(kXch).balance, Mojo{103'203'898'833});
    EXPECT_EQ(state.get_position(kDbx).balance, Mojo{203'188});
    EXPECT_EQ(state.get_position(kByc).balance, Mojo{0});
    EXPECT_NEAR(xch_dbx().cat_portfolio_pct, 0.959, 0.0005);
}

TEST_F(Incident20260922, Step8CorrectsStateAfterTheFillsLandFirst)
{
    // The sequence that disarmed the old recovery: the seed fails, fills land
    // before Step 8's first read, and only then does Step 8 read the wallet.
    book_first_fills(state);
    book_third_fill(state);
    ASSERT_GT(state.get_position(kXch).balance, Mojo{0});   // non-zero: the trap
    ASSERT_GT(state.get_position(kDbx).balance, Mojo{0});

    // The wallet as read during the investigation (confirmed_wallet_balance;
    // XCH to three decimals, the CATs to the mojo).
    const Mojo xch = 34'696 * (kMojosPerXch / 1'000);
    const Mojo byc = 98'989;
    const Mojo dbx = 2'166'963;
    EXPECT_EQ(apply_wallet_truth(state, kXch, xch, true, false).outcome,
              TruthOutcome::Corrected);
    EXPECT_EQ(apply_wallet_truth(state, kByc, byc, true, false).outcome,
              TruthOutcome::Corrected);
    EXPECT_EQ(apply_wallet_truth(state, kDbx, dbx, true, false).outcome,
              TruthOutcome::Corrected);

    EXPECT_EQ(state.get_position(kXch).balance, xch);
    EXPECT_EQ(state.get_position(kByc).balance, byc);
    EXPECT_EQ(state.get_position(kDbx).balance, dbx);

    // mark_to_xch rounds each position to a whole mojo, hence the tolerance.
    const double dbx_value = static_cast<double>(dbx) * dbx_rate();
    const double total = static_cast<double>(xch)
                       + static_cast<double>(byc) * byc_rate() + dbx_value;
    EXPECT_NEAR(xch_dbx().cat_portfolio_pct, dbx_value / total, 1e-9);
    // With BYC at the illustrative rate DBX is about 22% of the book, under
    // the live 25% single-CAT cap; before the correction it read as 96%.
    EXPECT_LT(xch_dbx().cat_portfolio_pct, 0.25);
    EXPECT_FALSE(xch_dbx().cat_cap_breached);
}

TEST_F(Incident20260922, TheFallbackSeedKeepsFillsBookable)
{
    // The fix's startup half: the reads fail, State takes the last persisted
    // quantities (illustrative here), and the same fills book without an
    // "unknown asset" -- then Step 8 moves State onto the wallet.
    const Mojo last_xch = 38 * kMojosPerXch;
    const Mojo last_byc = 83'700;
    const Mojo last_dbx = 2'049'000;
    for (const auto& [asset, last] : {std::pair{kXch, last_xch},
                                      std::pair{kByc, last_byc},
                                      std::pair{kDbx, last_dbx}}) {
        const auto seed = decide_state_seed(std::nullopt, false, last);
        ASSERT_EQ(seed.source, SeedSource::LastKnown);
        ASSERT_TRUE(state.reconcile_balance(asset, seed.quantity).has_value());
    }

    EXPECT_TRUE(state.record_sell(kXch, kMojosPerXch));
    state.record_buy(kDbx, 102'976, 1);
    state.record_buy(kXch, 1'103'203'898'833, 1'689'624'195'465);
    EXPECT_TRUE(state.record_sell(kByc, 1'864));
    EXPECT_EQ(state.get_position(kByc).balance, last_byc - 1'864);

    const Mojo xch = 34'696 * (kMojosPerXch / 1'000);
    EXPECT_EQ(apply_wallet_truth(state, kXch, xch, true, false).outcome,
              TruthOutcome::Corrected);
    EXPECT_EQ(state.get_position(kXch).balance, xch);
}

}  // namespace
