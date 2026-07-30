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

}  // namespace
