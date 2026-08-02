// test_pnl_tracker.cpp -- Integration tests for xop::PnLTracker.
//
// Added 2026-07-30 alongside the P&L overhaul.  The audit that preceded it
// found PnLTracker had zero direct test coverage while being broken at every
// layer (no restart rehydration, display-symbol keying in mark_to_market,
// mixed-currency USD conversion).  These tests lock in:
//
//   1. PNL-REHYDRATE  -- accumulators rebuilt from trade_log at init.
//   2. PNL-DURABILITY -- duplicate trade_ids are idempotent (no double count).
//   3. PNL-UNITS      -- USD totals use per-pair quote conversions.
//   4. PNL-KEYING     -- mark_to_market resolves the canonical base asset id.
//   5. PNL-HISTORY    -- fills are mirrored to trade_history/trades_live.csv.
//
// Each test uses its own SQLite file under the system temp directory.

#include <gtest/gtest.h>

#include <xop/monitoring/pnl.hpp>
#include <xop/types.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

constexpr double kBaseXchD  = 1e12;   // XCH mojos per unit
constexpr double kCatDenomD = 1e3;    // CAT mojos per unit

class PnLTrackerTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path()
             / ("xop_pnl_test_" + std::to_string(::testing::UnitTest::
                    GetInstance()->random_seed())
                + "_" + ::testing::UnitTest::GetInstance()
                          ->current_test_info()->name());
        fs::create_directories(dir_);
        db_path_ = (dir_ / "pnl_test.db").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);  // best-effort cleanup
    }

    static xop::Fill make_fill(const std::string& id, xop::Side side,
                               xop::Mojo price, xop::Mojo size,
                               xop::Mojo fee = 0) {
        xop::Fill f;
        f.offer_id     = id;
        f.pair_name    = "XCH/wUSDC.b";
        f.side         = side;
        f.price        = price;
        f.size         = size;
        f.block_height = 100;
        f.timestamp    = std::chrono::system_clock::now();
        f.fee_mojos    = fee;
        return f;
    }

    static void register_pair(xop::PnLTracker& t) {
        t.set_pair_conversion("XCH/wUSDC.b", "xch",
                              kBaseXchD, kCatDenomD,
                              /*usd_per_quote_unit=*/1.0);
    }

    fs::path    dir_;
    std::string db_path_;
};

// ---------------------------------------------------------------------------
// 1. Rehydration: a new tracker over an existing DB restores the realized
//    accumulators that used to silently reset to zero on every restart.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, RehydrationRestoresAccumulatorsAcrossRestart) {
    const xop::Mojo realized = 500;   // quote mojos ($0.50)
    const xop::Mojo fee      = 5'000; // XCH mojos

    {
        xop::PnLTracker a(db_path_);
        a.init_database();
        register_pair(a);

        ASSERT_TRUE(a.record_fill(
            make_fill("trade-ask-1", xop::Side::Ask,
                      static_cast<xop::Mojo>(2.5e12),
                      static_cast<xop::Mojo>(1e12), fee),
            fee, static_cast<xop::Mojo>(2.0e12), realized));
        ASSERT_TRUE(a.record_fill(
            make_fill("trade-bid-1", xop::Side::Bid,
                      static_cast<xop::Mojo>(2.4e12),
                      static_cast<xop::Mojo>(1e12), 0),
            0, 0, 0));

        auto s = a.get_total_pnl();
        EXPECT_EQ(s.spread_pnl, realized);
        EXPECT_EQ(s.fill_count, 2u);
    }  // destructor closes the DB -- simulated shutdown

    xop::PnLTracker b(db_path_);
    b.init_database();  // rehydrates

    auto s = b.get_total_pnl();
    EXPECT_EQ(s.spread_pnl, realized)
        << "realized P&L must survive a restart";
    EXPECT_EQ(s.fill_count, 2u);
    EXPECT_EQ(s.fee_pnl, -fee) << "fees accumulate as costs (negative)";

    auto p = b.get_pair_pnl("XCH/wUSDC.b");
    EXPECT_EQ(p.spread_pnl, realized);
}

// ---------------------------------------------------------------------------
// 2. Idempotent duplicates: replaying an already-journalled trade_id must
//    return false and leave the accumulators untouched.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, DuplicateTradeIdDoesNotDoubleCount) {
    xop::PnLTracker t(db_path_);
    t.init_database();
    register_pair(t);

    const auto fill = make_fill("trade-dup", xop::Side::Ask,
                                static_cast<xop::Mojo>(2.5e12),
                                static_cast<xop::Mojo>(1e12));

    EXPECT_TRUE(t.record_fill(fill, 0, static_cast<xop::Mojo>(2.0e12), 500));
    EXPECT_FALSE(t.record_fill(fill, 0, static_cast<xop::Mojo>(2.0e12), 500))
        << "second insert of the same trade_id must report duplicate";

    auto s = t.get_total_pnl();
    EXPECT_EQ(s.spread_pnl, 500);
    EXPECT_EQ(s.fill_count, 1u);
}

// ---------------------------------------------------------------------------
// 3. USD totals: quote mojos convert per-pair (1000 quote mojos = $1 for a
//    USD-pegged CAT quote), not via the old /1e12 * xch_usd path that
//    understated by ~1e9.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, UsdTotalUsesQuoteConversion) {
    xop::PnLTracker t(db_path_);
    t.init_database();
    register_pair(t);

    ASSERT_TRUE(t.record_fill(
        make_fill("trade-usd", xop::Side::Ask,
                  static_cast<xop::Mojo>(2.5e12),
                  static_cast<xop::Mojo>(1e12)),
        0, static_cast<xop::Mojo>(2.0e12), 500));

    auto s = t.get_total_pnl();
    // 500 wUSDC.b mojos = 0.5 units = $0.50.  (No fees, so no XCH leg.)
    EXPECT_NEAR(s.total_pnl_usd, 0.50, 1e-9);
}

// ---------------------------------------------------------------------------
// 3b. Retired pairs: rehydration resurrects pairs no longer in config (the
//     pre-migration "XCH/wUSDC"), which therefore never get a registered
//     conversion.  Their USD-pegged P&L must still reach the USD total, or
//     the engine's live figure silently disagrees with the GUI's SQL
//     reports (which match LIKE '%/wUSDC%').
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, RetiredPairStillCountsTowardUsdTotal) {
    xop::PnLTracker t(db_path_);
    t.init_database();
    // Deliberately register ONLY the current pair, as the engine does.
    register_pair(t);

    xop::Fill legacy = make_fill("trade-legacy", xop::Side::Ask,
                                 static_cast<xop::Mojo>(2.5e12),
                                 static_cast<xop::Mojo>(1e12));
    legacy.pair_name = "XCH/wUSDC";   // retired pair, never registered

    ASSERT_TRUE(t.record_fill(legacy, 0, static_cast<xop::Mojo>(2.0e12), 500));

    auto s = t.get_total_pnl();
    EXPECT_NEAR(s.total_pnl_usd, 0.50, 1e-9)
        << "retired USD-pegged pair must not drop out of the USD total";

    // An unpegged quote has no defensible USD value and stays excluded.
    xop::Fill dbx = make_fill("trade-dbx", xop::Side::Ask,
                              static_cast<xop::Mojo>(1.4e14),
                              static_cast<xop::Mojo>(1e12));
    dbx.pair_name = "XCH/DBX";
    ASSERT_TRUE(t.record_fill(dbx, 0, static_cast<xop::Mojo>(1.3e14), 900));

    s = t.get_total_pnl();
    EXPECT_NEAR(s.total_pnl_usd, 0.50, 1e-9)
        << "unpegged quote must not be guessed at";
}

// ---------------------------------------------------------------------------
// 4. mark_to_market keying: balance / cost-basis callbacks must receive the
//    canonical base asset id ("xch"), not the display symbol ("XCH") that
//    previously missed every lookup and pinned unrealized P&L at zero.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, MarkToMarketUsesCanonicalAssetId) {
    xop::PnLTracker t(db_path_);
    t.init_database();
    register_pair(t);

    // Seed the pair accumulator so mark_to_market iterates it.
    ASSERT_TRUE(t.record_fill(
        make_fill("trade-mtm", xop::Side::Ask,
                  static_cast<xop::Mojo>(2.5e12),
                  static_cast<xop::Mojo>(1e12)),
        0, static_cast<xop::Mojo>(2.0e12), 500));

    std::string seen_balance_key, seen_basis_key;

    t.mark_to_market(
        [](const std::string&, const std::string&) -> xop::Mojo {
            return static_cast<xop::Mojo>(2.5e12);   // mid (pair quote pseudo)
        },
        [&](const std::string& asset) -> xop::Mojo {
            seen_balance_key = asset;
            return static_cast<xop::Mojo>(1e12);     // 1 XCH held
        },
        [&](const std::string& asset) -> xop::Mojo {
            seen_basis_key = asset;
            return static_cast<xop::Mojo>(2.0e12);   // USD-pseudo basis $2.00
        },
        /*xch_usd_price=*/2.5,
        [](const std::string&) -> double {
            return kCatDenomD / kBaseXchD;           // quote_denom / base_denom
        });

    EXPECT_EQ(seen_balance_key, "xch")
        << "must use the registered canonical id, not the display symbol";
    EXPECT_EQ(seen_basis_key, "xch");

    // ($2.50 - $2.00) * 1 XCH = $0.50 = 500 quote mojos of unrealized P&L.
    auto p = t.get_pair_pnl("XCH/wUSDC.b");
    EXPECT_EQ(p.inventory_pnl, 500);
}

// ---------------------------------------------------------------------------
// 4b. Shared base asset must be marked ONCE.  XCH is the base of three
//     enabled pairs; balances are per ASSET while mark_to_market loops per
//     PAIR, so without dedup the whole XCH holding is valued three times and
//     summed.  This was invisible while the display-symbol keying made every
//     balance lookup miss -- fixing the keying made the triple-count live.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, SharedBaseAssetIsMarkedOnlyOnce) {
    xop::PnLTracker t(db_path_);
    t.init_database();

    // Three pairs, all based on "xch", exactly like the live config.
    for (const char* pair : {"XCH/wUSDC.b", "XCH/BYC", "XCH/DBX"}) {
        t.set_pair_conversion(pair, "xch", kBaseXchD, kCatDenomD, 1.0);
        xop::Fill f = make_fill(std::string("trade-") + pair, xop::Side::Ask,
                                static_cast<xop::Mojo>(2.5e12),
                                static_cast<xop::Mojo>(1e12));
        f.pair_name = pair;
        ASSERT_TRUE(t.record_fill(f, 0, static_cast<xop::Mojo>(2.0e12), 0));
    }

    int balance_calls = 0;
    t.mark_to_market(
        [](const std::string&, const std::string&) -> xop::Mojo {
            return static_cast<xop::Mojo>(2.5e12);
        },
        [&](const std::string&) -> xop::Mojo {
            ++balance_calls;
            return static_cast<xop::Mojo>(1e12);   // ONE XCH held in total
        },
        [](const std::string&) -> xop::Mojo {
            return static_cast<xop::Mojo>(2.0e12);
        },
        /*xch_usd_price=*/2.5,
        [](const std::string&) -> double { return kCatDenomD / kBaseXchD; });

    EXPECT_EQ(balance_calls, 3) << "callback still runs per pair";

    // ($2.50 - $2.00) x 1 XCH = $0.50 = 500 quote mojos, counted ONCE --
    // not 1500 across the three XCH-based pairs.
    auto s = t.get_total_pnl();
    EXPECT_EQ(s.inventory_pnl, 500)
        << "one XCH position must not be marked once per pair";
    EXPECT_NEAR(s.unrealized_pnl_usd, 0.50, 1e-9);
}

// ---------------------------------------------------------------------------
// 4c. [PNL-USD-TOTALS 2026-08-01] Cross-pair realized totals normalize each
//     pair's quote mojos through its own registered USD conversion.  Raw
//     summing was ~70x wrong for DBX: 3045 DBX mojos (~$0.04) added to the
//     same accumulator where 3045 wUSDC.b mojos mean ~$3.05.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, CrossPairUsdTotalsNormalizePerQuote) {
    constexpr double kUsdPerDbx = 0.0145;   // ~live cross-derived rate

    xop::PnLTracker t(db_path_);
    t.init_database();
    register_pair(t);
    t.set_pair_conversion("XCH/DBX", "xch", kBaseXchD, kCatDenomD, kUsdPerDbx);

    // $0.50 realized on the USD-pegged pair (500 quote mojos).
    ASSERT_TRUE(t.record_fill(
        make_fill("trade-usdc-x", xop::Side::Ask,
                  static_cast<xop::Mojo>(2.5e12),
                  static_cast<xop::Mojo>(1e12)),
        0, static_cast<xop::Mojo>(2.0e12), 500));

    // 3045 DBX quote mojos = 3.045 DBX ~= $0.0442, NOT $3.05.
    xop::Fill dbx = make_fill("trade-dbx-x", xop::Side::Ask,
                              static_cast<xop::Mojo>(1.4e14),
                              static_cast<xop::Mojo>(1e12));
    dbx.pair_name = "XCH/DBX";
    ASSERT_TRUE(t.record_fill(dbx, 0, static_cast<xop::Mojo>(1.3e14), 3045));

    auto s = t.get_total_pnl();
    const double expected = 0.50 + 3.045 * kUsdPerDbx;
    EXPECT_NEAR(s.realized_pnl_usd, expected, 1e-9)
        << "each pair must convert through its own quote-USD factor";
    EXPECT_NEAR(s.profit_factor, 1e9, 1.0)
        << "all-profit history reports the no-loss sentinel";
}

// ---------------------------------------------------------------------------
// 4d. [PNL-USD-TOTALS 2026-08-01] The cross-pair profit factor uses
//     USD-normalized grosses, and a restart (rehydrate_from_db) reproduces
//     the same USD figures because they are derived at query time from the
//     per-pair accumulators through the same conversion registry.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, ProfitFactorIsUsdNormalizedAndRestartStable) {
    constexpr double kUsdPerDbx = 0.0145;
    const double dbx_loss_usd   = 3.045 * kUsdPerDbx;      // ~$0.0442
    const double expected_pf    = 0.50 / dbx_loss_usd;     // ~11.3

    auto register_all = [&](xop::PnLTracker& t) {
        register_pair(t);
        t.set_pair_conversion("XCH/DBX", "xch",
                              kBaseXchD, kCatDenomD, kUsdPerDbx);
    };

    {
        xop::PnLTracker t(db_path_);
        t.init_database();
        register_all(t);

        // +$0.50 on wUSDC.b, -$0.0442 on DBX.  The raw quote-mojo ratio
        // would be 500/3045 = 0.16 (an apparent net loser); in USD the
        // strategy is clearly profitable.
        ASSERT_TRUE(t.record_fill(
            make_fill("trade-pf-win", xop::Side::Ask,
                      static_cast<xop::Mojo>(2.5e12),
                      static_cast<xop::Mojo>(1e12)),
            0, static_cast<xop::Mojo>(2.0e12), 500));
        xop::Fill dbx = make_fill("trade-pf-loss", xop::Side::Ask,
                                  static_cast<xop::Mojo>(1.4e14),
                                  static_cast<xop::Mojo>(1e12));
        dbx.pair_name = "XCH/DBX";
        ASSERT_TRUE(t.record_fill(dbx, 0, static_cast<xop::Mojo>(1.5e14),
                                  -3045));

        auto s = t.get_total_pnl();
        EXPECT_NEAR(s.profit_factor, expected_pf, 1e-6)
            << "cross-pair profit factor must compare USD, not raw mojos";
        EXPECT_NEAR(s.realized_pnl_usd, 0.50 - dbx_loss_usd, 1e-9);
    }  // shutdown

    // Restart: rehydration rebuilds the per-pair accumulators; once the
    // engine re-registers the conversions (as it does every heartbeat) the
    // USD totals must be identical to the pre-restart values.
    xop::PnLTracker t2(db_path_);
    t2.init_database();
    register_all(t2);

    auto s2 = t2.get_total_pnl();
    EXPECT_NEAR(s2.profit_factor, expected_pf, 1e-6)
        << "restart must not change the USD-normalized totals";
    EXPECT_NEAR(s2.realized_pnl_usd, 0.50 - dbx_loss_usd, 1e-9);
}

// ---------------------------------------------------------------------------
// 5. Durable history: every recorded fill lands in
//    <db_dir>/trade_history/trades_live.csv (header + one row per fill).
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, TradeHistoryCsvMirrorsFills) {
    xop::PnLTracker t(db_path_);
    t.init_database();
    register_pair(t);

    ASSERT_TRUE(t.record_fill(
        make_fill("trade-csv", xop::Side::Ask,
                  static_cast<xop::Mojo>(2.5e12),
                  static_cast<xop::Mojo>(1e12), 5'000),
        5'000, static_cast<xop::Mojo>(2.0e12), 500));

    const fs::path csv = dir_ / "trade_history" / "trades_live.csv";
    ASSERT_TRUE(fs::exists(csv)) << "CSV mirror must be created";

    std::ifstream in(csv);
    std::string header, row, extra;
    ASSERT_TRUE(static_cast<bool>(std::getline(in, header)));
    ASSERT_TRUE(static_cast<bool>(std::getline(in, row)));
    EXPECT_FALSE(static_cast<bool>(std::getline(in, extra)))
        << "exactly one data row expected";

    EXPECT_NE(header.find("realized_pnl_quote_mojos"), std::string::npos);
    EXPECT_NE(row.find("trade-csv"), std::string::npos);
    EXPECT_NE(row.find("XCH/wUSDC.b"), std::string::npos);
    EXPECT_NE(row.find(",500,"), std::string::npos)
        << "realized quote mojos must appear in the row";
}

// ---------------------------------------------------------------------------
// [SHARPE-CADENCE 2026-08-01] Sharpe annualization uses the MEASURED
// snapshot cadence, never the 52-second chain-block constant.
//
// Snapshots are taken once per engine heartbeat (~19 min; median measured
// inter-snapshot spacing 1,165 s).  Annualizing 19-minute returns at the
// 52-second constant would multiply Sharpe by sqrt(1165/52) ~ 4.73.
// ---------------------------------------------------------------------------
TEST(SharpeAnnualizationTest, UsesMeasuredCadenceNotBlockConstant) {
    const double mean  = 0.02;   // USD per snapshot interval
    const double stdev = 0.10;

    // At the measured 1,165 s heartbeat cadence:
    //   periods/year = 31,557,600 / 1,165 = 27,088.07
    //   sqrt         = 164.58
    //   sharpe       = 0.2 * 164.58 = 32.916
    const double at_heartbeat =
        xop::PnLTracker::annualized_sharpe(mean, stdev, 1165.0);
    EXPECT_NEAR(at_heartbeat, 0.2 * std::sqrt(31'557'600.0 / 1165.0), 1e-9);
    EXPECT_NEAR(at_heartbeat, 32.916, 0.01);

    // The 52-second constant would give 0.2 * sqrt(31,557,600/52) = 155.8
    // -- ~4.73x larger.  The measured-cadence result must NOT be that.
    const double at_block_const =
        xop::PnLTracker::annualized_sharpe(mean, stdev, 52.0);
    EXPECT_NEAR(at_block_const / at_heartbeat,
                std::sqrt(1165.0 / 52.0), 1e-9);
    EXPECT_LT(at_heartbeat, 0.5 * at_block_const);

    // Degenerate inputs are safe.
    EXPECT_DOUBLE_EQ(xop::PnLTracker::annualized_sharpe(mean, 0.0, 1165.0), 0.0);
    EXPECT_DOUBLE_EQ(xop::PnLTracker::annualized_sharpe(mean, stdev, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(xop::PnLTracker::annualized_sharpe(mean, stdev, -5.0), 0.0);
}

}  // namespace
