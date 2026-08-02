// test_reward_ingest.cpp -- Unit tests for dexie reward-income ingestion
// ([REWARD-INCOME 2026-08-01]).
//
// The constants below are the MEASURED production numbers (live DBX wallet,
// get_transactions, 2026-08-01):
//   - reward coins: 1-219 mojos each, daily bursts at one block
//     (e.g. height 9085813, 28 coins summing 618 mojos on 2026-07-31)
//   - smallest observed trading flow: 100,589 mojos
//   - live DBX basis in inventory_state: 1.3752e10 = $0.013752 * 1e12
//     (the USD-pseudo convention: USD per display unit in 1e12 fixed point)
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/accounting/reward_ingest.hpp>
#include <xop/config.hpp>
#include <xop/risk/inventory.hpp>

#include <chrono>
#include <cmath>
#include <string>

namespace {

using namespace xop::accounting;

// Production-shaped constants.
constexpr xop::Mojo  kMaxRewardMojos = 2'000;      // config default
constexpr xop::BlockHeight kGenesis  = 9'080'000;  // ledger opening block

// ============================================================================
// Filter: what is (and is not) a reward inflow
// ============================================================================

TEST(RewardIngestFilterTest, MeasuredRewardCoinIsAccepted) {
    // A coin from the observed 2026-07-31 burst: 22 mojos, plain incoming,
    // confirmed at 9085813 (after genesis), no matching outgoing.
    EXPECT_TRUE(is_reward_inflow(kIncomingTx, 22, 9'085'813, kGenesis,
                                 kMaxRewardMojos, false));
    // Largest observed reward coin (219 mojos) also passes.
    EXPECT_TRUE(is_reward_inflow(kIncomingTx, 219, 9'085'813, kGenesis,
                                 kMaxRewardMojos, false));
}

TEST(RewardIngestFilterTest, TradingFlowsAreRejectedByCeiling) {
    // The smallest observed trading flow (100,589 mojos) is 50x the
    // 2,000-mojo ceiling: never bookable as a reward.
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, 100'589, 9'085'813, kGenesis,
                                  kMaxRewardMojos, false));
    // A typical ladder-size flow.
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, 100'646, 9'088'216, kGenesis,
                                  kMaxRewardMojos, false));
}

TEST(RewardIngestFilterTest, SelfSpendPairsAreRejected) {
    // The bot's own coin management appears as PAIRED outgoing+incoming of
    // equal amount in one block; the incoming half must not book as income
    // even when it is small.
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, 150, 9'085'813, kGenesis,
                                  kMaxRewardMojos,
                                  /*has_matching_outgoing=*/true));
}

TEST(RewardIngestFilterTest, NonIncomingTypesAreRejected) {
    for (int type : {kOutgoingTx, kCoinbase, kFeeReward,
                     kIncomingTrade, kOutgoingTrade, -1}) {
        EXPECT_FALSE(is_reward_inflow(type, 22, 9'085'813, kGenesis,
                                      kMaxRewardMojos, false))
            << "type=" << type;
    }
}

TEST(RewardIngestFilterTest, UnconfirmedAndPreGenesisAreRejected) {
    // Unconfirmed (height 0): not yet in the wallet's confirmed balance.
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, 22, 0, kGenesis,
                                  kMaxRewardMojos, false));
    // At or below the ledger opening block: already inside the opening
    // balance (the 64.682 DBX of pre-genesis rewards live there) -- booking
    // it again would double-count.
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, 22, kGenesis, kGenesis,
                                  kMaxRewardMojos, false));
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, 22, kGenesis - 5, kGenesis,
                                  kMaxRewardMojos, false));
}

TEST(RewardIngestFilterTest, DegenerateAmountsAreRejected) {
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, 0, 9'085'813, kGenesis,
                                  kMaxRewardMojos, false));
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, -7, 9'085'813, kGenesis,
                                  kMaxRewardMojos, false));
    // A zero ceiling disables ingestion entirely.
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, 1, 9'085'813, kGenesis,
                                  /*max=*/0, false));
}

// ============================================================================
// Valuation: booking at fair value (acceptance: "booking at FMV")
// ============================================================================

TEST(RewardValuationTest, MeasuredBurstAtLiveFmv) {
    // The 2026-07-31 burst summed 618 mojos = 0.618 DBX.  At the live
    // DBX/USD price of ~$0.0137 (CoinGecko dexie-bucks; cross-check:
    // XCH $1.39 / 100.7 DBX-per-XCH = $0.0138):
    //   income   = 618 / 1000 * 0.0137      = $0.0084666
    //   fmv_px   = 0.0137 * 1e12            = 1.37e10 pseudo
    // matching the USD-pseudo basis convention verified against the live
    // inventory_state row (basis 1.3752e10 == $0.013752 * 1e12).
    const auto v = value_reward(618, /*mojos_per_unit=*/1e3,
                                /*usd_per_unit=*/0.0137);
    EXPECT_NEAR(v.income_usd, 0.0084666, 1e-9);
    EXPECT_EQ(v.fmv_pseudo_price, static_cast<xop::Mojo>(13'700'000'000LL));
}

TEST(RewardValuationTest, XchConventionAndGuards) {
    // XCH uses 1e12 mojos per unit: 0.5 XCH at $1.39 = $0.695.
    const auto v = value_reward(500'000'000'000LL, 1e12, 1.39);
    EXPECT_NEAR(v.income_usd, 0.695, 1e-12);
    EXPECT_EQ(v.fmv_pseudo_price, static_cast<xop::Mojo>(1'390'000'000'000LL));

    // Degenerate inputs yield a zeroed valuation -- the caller defers the
    // booking rather than fabricating a price.
    EXPECT_EQ(value_reward(0, 1e3, 0.0137).fmv_pseudo_price, 0);
    EXPECT_EQ(value_reward(618, 0.0, 0.0137).fmv_pseudo_price, 0);
    EXPECT_EQ(value_reward(618, 1e3, 0.0).fmv_pseudo_price, 0);
    EXPECT_EQ(value_reward(618, 1e3, -1.0).fmv_pseudo_price, 0);
    const double nan = std::nan("");
    EXPECT_EQ(value_reward(618, 1e3, nan).fmv_pseudo_price, 0);
    EXPECT_DOUBLE_EQ(value_reward(618, 1e3, nan).income_usd, 0.0);
}

// ============================================================================
// Ledger note: the restart-invariance carrier round-trips
// ============================================================================

TEST(RewardNoteTest, FmvRoundTripsThroughTheNote) {
    const auto v = value_reward(618, 1e3, 0.0137);
    const std::string note = reward_note(v.income_usd, 0.0137, "0x298fc2d3");
    // Human-readable and self-describing.
    EXPECT_NE(note.find("dexie liquidity reward"), std::string::npos);
    EXPECT_NE(note.find("wallet_tx=0x298fc2d3"), std::string::npos);
    // The parsed FMV equals the written income to the note's 1e-10 quantum.
    EXPECT_NEAR(parse_reward_fmv_usd(note), v.income_usd, 1e-10);
}

TEST(RewardNoteTest, ForeignNotesParseToZero) {
    EXPECT_DOUBLE_EQ(parse_reward_fmv_usd(""), 0.0);
    EXPECT_DOUBLE_EQ(
        parse_reward_fmv_usd("unexplained divergence reconciled to wallet"),
        0.0);
    EXPECT_DOUBLE_EQ(parse_reward_fmv_usd("fmv_usd=garbage"), 0.0);
    EXPECT_DOUBLE_EQ(parse_reward_fmv_usd("fmv_usd=-3.5"), 0.0);
}

// ============================================================================
// Cost basis: the reward folds into the weighted average at receipt FMV
// (acceptance: "basis fold-in")
// ============================================================================

TEST(RewardBasisTest, RewardFoldsIntoWeightedAverageBasis) {
    xop::RiskConfig risk_cfg;
    xop::InventoryTracker inv(risk_cfg, 1'000'000'000LL);
    const auto now = std::chrono::system_clock::now();
    const xop::AssetId dbx{
        "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20"};

    // Live position: 978,576 mojos at the persisted basis $0.013752
    // (pseudo 13,752,000,000).
    inv.record_buy(dbx, 978'576, 13'752'000'000LL, 9'080'000, now);

    // Reward receipt: 618 mojos at FMV $0.0137 (pseudo 13,700,000,000).
    const auto v = value_reward(618, 1e3, 0.0137);
    inv.record_buy(dbx, 618, v.fmv_pseudo_price, 9'085'813, now);

    const auto rec = inv.get_record(dbx);
    EXPECT_EQ(rec.total_quantity, 979'194);

    // Weighted average, computed independently:
    //   (978,576 * 1.3752e10 + 618 * 1.37e10) / 979,194 = 1.375197e10
    const double expected_basis =
        (978'576.0 * 13'752'000'000.0 + 618.0 * 13'700'000'000.0)
        / 979'194.0;
    EXPECT_NEAR(static_cast<double>(rec.weighted_avg_cost_basis),
                expected_basis, 1.0);
    // The tiny receipt at a slightly lower FMV nudges the basis DOWN, and
    // by less than 0.01%: recognition at fair value, no basis destruction.
    EXPECT_LT(rec.weighted_avg_cost_basis, 13'752'000'000LL);
    EXPECT_GT(rec.weighted_avg_cost_basis, 13'751'000'000LL);
}

// ============================================================================
// Config defaults: ingestion ships on, targeting DBX, with the measured
// separation ceiling
// ============================================================================

TEST(RewardIngestConfigTest, Defaults) {
    const xop::AccountingConfig d{};
    EXPECT_TRUE(d.reward_ingest_enabled);
    EXPECT_EQ(d.reward_asset_id,
              "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20");
    EXPECT_EQ(d.reward_max_mojos_per_coin, 2'000LL);
}

}  // namespace
