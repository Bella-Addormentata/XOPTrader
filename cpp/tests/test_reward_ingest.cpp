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
constexpr xop::Mojo  kMaxRewardMojos = 20'000;     // config default
constexpr xop::BlockHeight kGenesis  = 9'080'000;  // ledger opening block

// ============================================================================
// Filter: what is (and is not) a reward inflow
// ============================================================================

// [review 2026-09-12] The BACKLOG bound.  value_reward() takes ONE live
// usd_per_unit and the ledger row it feeds is idempotent on event_id, so
// whatever price the FIRST scan sees is the price that receipt keeps forever.
// The genesis gate below does NOT bound that: it is the asset's OPENING
// block, written once ever and ~197k blocks under the head, so the whole
// never-booked backlog passes it and would book at today's price on the
// first restart after the reverse=false window fix.
TEST(RewardIngestFilterTest, StaleBacklogReceiptsAreNotValuedAtTodaysPrice) {
    constexpr xop::BlockHeight kNow = 9'277'273;   // measured head, 2026-09-11

    // Recent receipts price fairly at the live rate.
    EXPECT_TRUE(reward_receipt_is_recent(kNow, kNow));
    EXPECT_TRUE(reward_receipt_is_recent(kNow - 4'608, kNow));

    // The measured 2026-07-31 burst PASSES the genesis gate -- which is
    // exactly why this second bound has to exist.
    EXPECT_TRUE(is_reward_inflow(kIncomingTx, 22, 9'085'813, kGenesis,
                                 kMaxRewardMojos, false));
    EXPECT_FALSE(reward_receipt_is_recent(9'085'813, kNow));

    // Boundary, both sides of it.
    EXPECT_TRUE(reward_receipt_is_recent(kNow - kMaxRewardBacklogBlocks, kNow));
    EXPECT_FALSE(reward_receipt_is_recent(kNow - kMaxRewardBacklogBlocks - 1,
                                          kNow));

    // Degenerate: a height at or ahead of the head (lagging head, reorg) is
    // not stale, and a zero bound disables the check entirely.
    EXPECT_TRUE(reward_receipt_is_recent(kNow + 10, kNow));
    EXPECT_TRUE(reward_receipt_is_recent(9'085'813, kNow, 0));
}

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
    // The smallest observed trading flow of 2026-08 (100,589 mojos) is 5x the
    // 20,000-mojo ceiling: never bookable as a reward.
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

// ---------------------------------------------------------------------------
// classify_reward_row -- the per-row ordering the reward scan delegates to.
//
// [review 2026-09-13] This ordering used to live inline in engine.cpp, where
// no test could reach it. The loop now CALLS this function, so these drive
// production code.
//
// MUTATION CHECK: move the already_booked clause BELOW the is_recent clause
// (the original defect) -> ABookedReceiptIsFinishedBusinessAtAnyAge goes RED
// and nothing else does.
// ---------------------------------------------------------------------------

TEST(RewardRowClassification, ARecentUnbookedInflowIsBooked) {
    EXPECT_EQ(classify_reward_row(/*is_reward_inflow=*/true,
                                  /*has_idempotency_key=*/true,
                                  /*already_booked=*/false,
                                  /*is_recent=*/true),
              RewardRowAction::Book);
}

// THE DEFECT round 11 fixed: a receipt booked while it was fresh must not be
// re-classified as stale once it ages past the cutoff. It was, and the warning
// then called those rows unresolved divergence on every heartbeat.
TEST(RewardRowClassification, ABookedReceiptIsFinishedBusinessAtAnyAge) {
    EXPECT_EQ(classify_reward_row(true, true,
                                  /*already_booked=*/true,
                                  /*is_recent=*/false),
              RewardRowAction::AlreadyBooked)
        << "idempotency outranks freshness; a booked row is not stale work";
    EXPECT_EQ(classify_reward_row(true, true, true, true),
              RewardRowAction::AlreadyBooked);
}

TEST(RewardRowClassification, AnUnbookedStaleReceiptIsReportedNotBooked) {
    EXPECT_EQ(classify_reward_row(true, true, false, /*is_recent=*/false),
              RewardRowAction::TooStale);
}

TEST(RewardRowClassification, NoTxNameMeansNoStableLedgerKey) {
    EXPECT_EQ(classify_reward_row(true, /*has_idempotency_key=*/false, false, true),
              RewardRowAction::NoIdempotencyKey);
}

TEST(RewardRowClassification, NonRewardsAreRejectedBeforeAnythingElse) {
    EXPECT_EQ(classify_reward_row(/*is_reward_inflow=*/false, true, true, true),
              RewardRowAction::NotAReward)
        << "the inflow test comes first so a non-reward never costs a DB read";
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
    EXPECT_EQ(d.reward_max_mojos_per_coin, 20'000LL);
}

// ============================================================================
// [REWARD-CATCHUP 2026-11-24] The 2026-11 bursts
// ============================================================================

// Coins measured on the live DBX wallet that the 2,000 ceiling dropped, and
// the smallest trading flow of the same month: 1 XCH sold for 76.958 DBX.
TEST(RewardIngestFilterTest, NovemberRewardCoinsPassAndTradesStillDoNot) {
    constexpr xop::BlockHeight kBurst = 9'584'747;
    for (const xop::Mojo coin : {2'208, 2'426, 3'086, 3'212, 4'395}) {
        EXPECT_TRUE(is_reward_inflow(kIncomingTx, coin, kBurst, kGenesis,
                                     kMaxRewardMojos, false)) << coin;
    }
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, 76'958, kBurst, kGenesis,
                                  kMaxRewardMojos, false));
    EXPECT_TRUE(is_reward_inflow(kIncomingTx, kMaxRewardMojos, kBurst, kGenesis,
                                 kMaxRewardMojos, false));
    EXPECT_FALSE(is_reward_inflow(kIncomingTx, kMaxRewardMojos + 1, kBurst,
                                  kGenesis, kMaxRewardMojos, false));
}

TEST(RewardIngestScanTest, DeepScanIsDueEveryThirtyBlocks) {
    EXPECT_TRUE(reward_deep_scan_due(0, 9'600'000));           // never ran
    EXPECT_FALSE(reward_deep_scan_due(9'600'000, 9'600'000));
    EXPECT_FALSE(reward_deep_scan_due(9'600'000, 9'600'029));
    EXPECT_TRUE(reward_deep_scan_due(9'600'000, 9'600'030));
    EXPECT_TRUE(reward_deep_scan_due(9'600'000, 9'599'000));   // height went back
    EXPECT_FALSE(reward_deep_scan_due(0, 0));
}

// A 220-coin burst does not fit one page of 200: the deep scan reads on.
TEST(RewardIngestScanTest, AFullPageOfRecentRowsIsFollowedOnADeepScan) {
    constexpr xop::BlockHeight kNow = 9'575'500;
    EXPECT_TRUE(reward_scan_reads_on(true, 1, kRewardScanPageRows, 9'575'482, kNow));
    // Not on an ordinary heartbeat: one page each.
    EXPECT_FALSE(reward_scan_reads_on(false, 1, kRewardScanPageRows, 9'575'482, kNow));
    // A short page is the wallet's last.
    EXPECT_FALSE(reward_scan_reads_on(true, 1, kRewardScanPageRows - 1, 9'575'482, kNow));
    // A page whose oldest row is unconfirmed says nothing about age: read on.
    EXPECT_TRUE(reward_scan_reads_on(true, 1, kRewardScanPageRows, 0, kNow));
}

TEST(RewardIngestScanTest, TheScanStopsWhereNothingFurtherBackCouldBook) {
    constexpr xop::BlockHeight kNow = 9'616'967;
    // Past the backlog bound every older row is too stale to book.
    EXPECT_FALSE(reward_scan_reads_on(true, 1, kRewardScanPageRows,
                                      kNow - kMaxRewardBacklogBlocks - 1, kNow));
    EXPECT_TRUE(reward_scan_reads_on(true, 1, kRewardScanPageRows,
                                     kNow - kMaxRewardBacklogBlocks, kNow));
    // And never more than kRewardScanMaxPages pages.
    EXPECT_TRUE(reward_scan_reads_on(true, kRewardScanMaxPages - 1,
                                     kRewardScanPageRows, kNow, kNow));
    EXPECT_FALSE(reward_scan_reads_on(true, kRewardScanMaxPages,
                                      kRewardScanPageRows, kNow, kNow));
}

}  // namespace
