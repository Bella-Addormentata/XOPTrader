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

#include <xop/accounting/bridge_ingest.hpp>
#include <xop/accounting/reward_ingest.hpp>
#include <xop/database.hpp>
#include <xop/monitoring/pnl.hpp>
#include <xop/types.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
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
// [REWARD-INCOME 2026-08-01] Reward income: separate accumulator, restart-
// invariant via the ledger (acceptance: "restart-invariance").
//
// Production layout is reproduced exactly: Database owns ledger_entries and
// PnLTracker opens the SAME SQLite file (both are constructed on
// config.database.path); reward receipts are journaled with their receipt
// FMV in the note, and a fresh tracker rebuilds the income total from those
// rows alone.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, RewardIncomeRebuildsFromLedgerAcrossRestart) {
    const std::string dbx =
        "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20";

    // The two most recent live bursts, valued at the live ~$0.0137/DBX.
    const auto v1 = xop::accounting::value_reward(1'381, 1e3, 0.0137);
    const auto v2 = xop::accounting::value_reward(618, 1e3, 0.0137);
    const double expected = v1.income_usd + v2.income_usd;  // $0.0273863

    {
        xop::Database db(db_path_);
        auto mk = [&](const char* txid, xop::Mojo amt, double usd) {
            xop::DbLedgerEntry e;
            e.entry_time   = "2026-07-31T19:13:14.000Z";
            e.event_type   = "reward";
            e.event_id     = std::string("reward:") + txid;
            e.leg          = "reward";
            e.asset_id     = dbx;
            e.delta_mojos  = amt;
            e.block_height = 9'085'813;
            e.note = xop::accounting::reward_note(usd, 0.0137, txid);
            return e;
        };
        ASSERT_EQ(db.append_ledger_entries({
            mk("0xaaa1", 1'381, v1.income_usd),
            mk("0xbbb2", 618, v2.income_usd),
            // An adjusting entry must NOT count as reward income.
            [&] {
                xop::DbLedgerEntry e;
                e.entry_time  = "2026-07-31T19:20:00.000Z";
                e.event_type  = "adjust";
                e.event_id    = "adjust:" + dbx + ":9085900";
                e.leg         = "adjust";
                e.asset_id    = dbx;
                e.delta_mojos = 500;
                e.note = "unexplained divergence reconciled to wallet";
                return e;
            }(),
        }).value_or(0), 3u);
    }

    // "Restart": a fresh tracker over the same file rebuilds the total
    // from the ledger.  Must FAIL if rehydration silently loads nothing.
    xop::PnLTracker t(db_path_);
    t.init_database();
    ASSERT_NEAR(t.get_reward_income_usd(), expected, 1e-9);
    EXPECT_NEAR(t.get_reward_income_usd(), 0.0273863, 1e-6);

    // Live accumulation stacks on top of the rehydrated total.
    t.add_reward_income_usd(0.0100);
    EXPECT_NEAR(t.get_reward_income_usd(), expected + 0.0100, 1e-9);

    // Surfaced beside the trading figures but NOT summed into them: with
    // zero trades the trading total stays exactly 0 despite the income.
    const auto s = t.get_total_pnl();
    EXPECT_NEAR(s.reward_income_usd, expected + 0.0100, 1e-9);
    EXPECT_DOUBLE_EQ(s.total_pnl_usd, 0.0);
    EXPECT_DOUBLE_EQ(s.realized_pnl_usd, 0.0);
    EXPECT_DOUBLE_EQ(s.fee_pnl_usd, 0.0);

    // Garbage guards: negative / NaN contributions are ignored.
    t.add_reward_income_usd(-5.0);
    t.add_reward_income_usd(std::numeric_limits<double>::quiet_NaN());
    EXPECT_NEAR(t.get_reward_income_usd(), expected + 0.0100, 1e-9);
}

TEST_F(PnLTrackerTest, NetDepositsRebuildFromLedgerAcrossRestart) {
    // [S19 2026-08-23] Bridge flows rebuild the SIGNED net-deposits total
    // from the ledger notes on restart, without touching reward income or
    // any trading figure.
    const std::string wusdc =
        "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";

    {
        xop::Database db(db_path_);
        auto mk = [&](const char* type, xop::Mojo delta, double usd,
                      std::int64_t job) {
            xop::DbLedgerEntry e;
            e.entry_time   = "2026-08-23T19:28:03.000Z";
            e.event_type   = type;
            e.event_id     = std::string("bridge:job:")
                           + std::to_string(job) + ":2026-08-23T11:59:38Z";
            e.leg          = "bridge";
            e.asset_id     = wusdc;
            e.delta_mojos  = delta;
            e.block_height = 9'189'949;
            e.note = xop::accounting::bridge_note(usd, 1.0, job);
            return e;
        };
        ASSERT_EQ(db.append_ledger_entries({
            // The live job 2 mint and a hypothetical later unwrap.
            mk("bridge_deposit", 4'985, 4.985, 2),
            mk("bridge_withdrawal", -20'000, -20.0, 3),
            // Neither an adjust nor a reward row may leak into the total.
            [&] {
                xop::DbLedgerEntry e;
                e.entry_time  = "2026-08-23T19:30:00.000Z";
                e.event_type  = "adjust";
                e.event_id    = "adjust:" + wusdc + ":9189950";
                e.leg         = "adjust";
                e.asset_id    = wusdc;
                e.delta_mojos = 500;
                e.note = "unexplained divergence reconciled to wallet";
                return e;
            }(),
            [&] {
                xop::DbLedgerEntry e;
                e.entry_time  = "2026-08-23T19:31:00.000Z";
                e.event_type  = "reward";
                e.event_id    = "reward:0xccc3";
                e.leg         = "reward";
                e.asset_id    = wusdc;
                e.delta_mojos = 10;
                e.note = xop::accounting::reward_note(0.01, 1.0, "0xccc3");
                return e;
            }(),
        }).value_or(0), 4u);
    }

    // "Restart": a fresh tracker rebuilds the signed total from the
    // ledger.  Must FAIL if the event filter or note parse regresses.
    xop::PnLTracker t(db_path_);
    t.init_database();
    EXPECT_NEAR(t.get_net_deposits_usd(), 4.985 - 20.0, 1e-9);
    EXPECT_NEAR(t.get_reward_income_usd(), 0.01, 1e-9);  // untouched

    // Live accumulation stacks, and negatives are VALID here (unlike
    // reward income): a withdrawal is a negative external flow.
    t.add_net_deposit_usd(400.0);
    t.add_net_deposit_usd(-5.0);
    EXPECT_NEAR(t.get_net_deposits_usd(), 4.985 - 20.0 + 395.0, 1e-9);

    // Surfaced beside the trading figures but part of NONE of them.
    const auto s = t.get_total_pnl();
    EXPECT_NEAR(s.net_deposits_usd, 4.985 - 20.0 + 395.0, 1e-9);
    EXPECT_DOUBLE_EQ(s.total_pnl_usd, 0.0);
    EXPECT_NEAR(s.reward_income_usd, 0.01, 1e-9);

    // Garbage guards: NaN and absurd magnitudes are ignored.
    t.add_net_deposit_usd(std::numeric_limits<double>::quiet_NaN());
    t.add_net_deposit_usd(2e12);
    EXPECT_NEAR(t.get_net_deposits_usd(), 4.985 - 20.0 + 395.0, 1e-9);
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

// ---------------------------------------------------------------------------
// 4c. [S20 2026-08-24] Price-only failures CARRY, and a carry must never
//     pre-empt a pair that can actually price the asset.
//
//     pair_pnl_ is an unordered_map and XCH is the base of three pairs, so
//     an earlier cut that claimed the asset on the first unpriced pair
//     visited made the mark depend on iteration order: an ungraded pair
//     could lock out a graded one and freeze unrealized P&L despite usable
//     data.  Here exactly one of the three XCH pairs has a price; the mark
//     must be the priced one's, whatever order the map yields.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, CarryNeverPreemptsAPairThatCanPrice) {
    xop::PnLTracker t(db_path_);
    t.init_database();

    for (const char* pair : {"XCH/wUSDC.b", "XCH/BYC", "XCH/DBX"}) {
        t.set_pair_conversion(pair, "xch", kBaseXchD, kCatDenomD, 1.0);
        xop::Fill f = make_fill(std::string("trade-") + pair, xop::Side::Ask,
                                static_cast<xop::Mojo>(2.5e12),
                                static_cast<xop::Mojo>(1e12));
        f.pair_name = pair;
        ASSERT_TRUE(t.record_fill(f, 0, static_cast<xop::Mojo>(2.0e12), 0));
    }

    // Only XCH/BYC has a usable price; the other two are withheld exactly
    // as the engine withholds an ungraded mid.
    t.mark_to_market(
        [](const std::string& pair, const std::string&) -> xop::Mojo {
            return pair == "XCH/BYC" ? static_cast<xop::Mojo>(2.5e12) : 0;
        },
        [](const std::string&) -> xop::Mojo { return static_cast<xop::Mojo>(1e12); },
        [](const std::string&) -> xop::Mojo { return static_cast<xop::Mojo>(2.0e12); },
        2.5,
        [](const std::string&) -> double { return kCatDenomD / kBaseXchD; });

    // ($2.50 - $2.00) * 1 XCH = 500 quote mojos, booked to the priced pair.
    EXPECT_EQ(t.get_pair_pnl("XCH/BYC").inventory_pnl, 500)
        << "the pair that CAN price the asset must own the mark";
    EXPECT_EQ(t.get_total_pnl().inventory_pnl, 500)
        << "asset marked exactly once, by the priced pair";
}

// A price-only failure across EVERY pair for the asset carries the previous
// mark rather than zeroing it -- zeroing injected a spurious step into the
// rolling-window loss series the engine's breaker watches.
TEST_F(PnLTrackerTest, PriceOnlyFailureCarriesPreviousMark) {
    xop::PnLTracker t(db_path_);
    t.init_database();
    register_pair(t);

    ASSERT_TRUE(t.record_fill(
        make_fill("trade-carry", xop::Side::Ask,
                  static_cast<xop::Mojo>(2.5e12),
                  static_cast<xop::Mojo>(1e12)),
        0, static_cast<xop::Mojo>(2.0e12), 0));

    auto balance = [](const std::string&) -> xop::Mojo {
        return static_cast<xop::Mojo>(1e12);
    };
    auto basis = [](const std::string&) -> xop::Mojo {
        return static_cast<xop::Mojo>(2.0e12);
    };
    auto unit = [](const std::string&) -> double {
        return kCatDenomD / kBaseXchD;
    };

    // Cycle 1: priced.
    t.mark_to_market(
        [](const std::string&, const std::string&) -> xop::Mojo {
            return static_cast<xop::Mojo>(2.5e12);
        }, balance, basis, 2.5, unit);
    const auto marked = t.get_pair_pnl("XCH/wUSDC.b").inventory_pnl;
    ASSERT_EQ(marked, 500);

    // Cycle 2: no usable price anywhere -- carry, do not zero.
    t.mark_to_market(
        [](const std::string&, const std::string&) -> xop::Mojo { return 0; },
        balance, basis, 2.5, unit);

    EXPECT_EQ(t.get_pair_pnl("XCH/wUSDC.b").inventory_pnl, marked)
        << "an unpriced cycle must not erase the position's mark";
    EXPECT_EQ(t.get_total_pnl().inventory_pnl, marked);
}

// ---------------------------------------------------------------------------
// 4d. [S20 2026-08-24] The carry must come from the pair that actually OWNS
//     the previous cycle's mark, not merely the first unpriced pair the
//     unordered_map happens to yield.
//
//     Dedup gives exactly one pair the asset's mark and zeroes the others,
//     so carrying a dedup LOSER carries its 0 -- dropping the asset's whole
//     unrealized P&L to zero and injecting the very discontinuity the carry
//     exists to prevent.  Here cycle 1 gives the mark to one XCH pair, then
//     cycle 2 withholds every price; the total must survive intact whatever
//     order the map iterates.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, CarryComesFromThePairHoldingThePriorMark) {
    xop::PnLTracker t(db_path_);
    t.init_database();

    for (const char* pair : {"XCH/wUSDC.b", "XCH/BYC", "XCH/DBX"}) {
        t.set_pair_conversion(pair, "xch", kBaseXchD, kCatDenomD, 1.0);
        xop::Fill f = make_fill(std::string("trade-") + pair, xop::Side::Ask,
                                static_cast<xop::Mojo>(2.5e12),
                                static_cast<xop::Mojo>(1e12));
        f.pair_name = pair;
        ASSERT_TRUE(t.record_fill(f, 0, static_cast<xop::Mojo>(2.0e12), 0));
    }

    auto balance = [](const std::string&) -> xop::Mojo {
        return static_cast<xop::Mojo>(1e12);
    };
    auto basis = [](const std::string&) -> xop::Mojo {
        return static_cast<xop::Mojo>(2.0e12);
    };
    auto unit = [](const std::string&) -> double {
        return kCatDenomD / kBaseXchD;
    };

    // Cycle 1: every pair can price, so dedup awards the mark to exactly
    // one of them and zeroes the other two.
    t.mark_to_market(
        [](const std::string&, const std::string&) -> xop::Mojo {
            return static_cast<xop::Mojo>(2.5e12);
        }, balance, basis, 2.5, unit);
    const auto total_after_cycle1 = t.get_total_pnl().inventory_pnl;
    ASSERT_EQ(total_after_cycle1, 500);

    // Cycle 2: no pair can price the asset -- carry the OWNER's mark.
    t.mark_to_market(
        [](const std::string&, const std::string&) -> xop::Mojo { return 0; },
        balance, basis, 2.5, unit);

    EXPECT_EQ(t.get_total_pnl().inventory_pnl, total_after_cycle1)
        << "carried a dedup loser's zero instead of the owner's mark";

    // Per-pair figures must still sum to the total: exactly one pair holds
    // the carried mark and the rest are zero.
    int nonzero = 0;
    xop::Mojo sum = 0;
    for (const char* pair : {"XCH/wUSDC.b", "XCH/BYC", "XCH/DBX"}) {
        const auto v = t.get_pair_pnl(pair).inventory_pnl;
        if (v != 0) ++nonzero;
        sum += v;
    }
    EXPECT_EQ(nonzero, 1) << "stale per-pair marks left on dedup losers";
    EXPECT_EQ(sum, total_after_cycle1);
}

// ---------------------------------------------------------------------------
// 4e. [S20 2026-08-24] A non-positive conversion factor zeroes the mark.
//
//     PnLTracker reads usd_per_quote_unit <= 0 as "basis unknown", which
//     drives basis to 0 and drops the pair out of the marking branch --
//     so pushing 0 for an untrusted factor would erase the mark rather
//     than preserve it.  This pins the semantics the engine relies on when
//     it CARRIES the last trusted factor instead of zeroing it.
// ---------------------------------------------------------------------------
TEST_F(PnLTrackerTest, NonPositiveConversionFactorErasesTheMark) {
    xop::PnLTracker t(db_path_);
    t.init_database();
    t.set_pair_conversion("XCH/wUSDC.b", "xch", kBaseXchD, kCatDenomD, 1.0);

    ASSERT_TRUE(t.record_fill(
        make_fill("trade-conv", xop::Side::Ask,
                  static_cast<xop::Mojo>(2.5e12),
                  static_cast<xop::Mojo>(1e12)),
        0, static_cast<xop::Mojo>(2.0e12), 0));

    auto price = [](const std::string&, const std::string&) -> xop::Mojo {
        return static_cast<xop::Mojo>(2.5e12);
    };
    auto balance = [](const std::string&) -> xop::Mojo {
        return static_cast<xop::Mojo>(1e12);
    };
    auto basis = [](const std::string&) -> xop::Mojo {
        return static_cast<xop::Mojo>(2.0e12);
    };
    auto unit = [](const std::string&) -> double {
        return kCatDenomD / kBaseXchD;
    };

    t.mark_to_market(price, balance, basis, 2.5, unit);
    ASSERT_EQ(t.get_pair_pnl("XCH/wUSDC.b").inventory_pnl, 500);

    // Re-register with a non-positive factor, as an untrusted cross would
    // if the engine pushed 0 instead of carrying the last trusted value.
    t.set_pair_conversion("XCH/wUSDC.b", "xch", kBaseXchD, kCatDenomD, 0.0);
    t.mark_to_market(price, balance, basis, 2.5, unit);

    EXPECT_EQ(t.get_pair_pnl("XCH/wUSDC.b").inventory_pnl, 0)
        << "a non-positive factor must be recognised as mark-erasing -- "
           "this is why the engine carries the last TRUSTED factor";
}
