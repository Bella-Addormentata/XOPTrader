#include <gtest/gtest.h>
#include <sqlite3.h>

#include <xop/database.hpp>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

class TempDbPath {
public:
    explicit TempDbPath(const std::string& stem)
    {
        const auto unique = std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path()
              / (stem + "_" + std::to_string(unique) + ".sqlite");
    }

    ~TempDbPath()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        std::filesystem::remove(path_.string() + "-wal", ec);
        std::filesystem::remove(path_.string() + "-shm", ec);
    }

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct OfferRow {
    std::string status;
    xop::BlockHeight resolved_block{0};
    std::string cancel_reason;
};

struct ClosureEventRow {
    std::string event_type;
    std::string previous_status;
    std::string observed_status;
    xop::BlockHeight resolved_block{0};
    std::string closure_reason;
};

sqlite3* open_db(const std::filesystem::path& path)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        const std::string message = db
            ? sqlite3_errmsg(db)
            : std::string{"sqlite3_open failed"};
        if (db) {
            sqlite3_close(db);
        }
        throw std::runtime_error(message);
    }
    return db;
}

OfferRow query_offer_row(sqlite3* db, const std::string& offer_id)
{
    constexpr const char* kSql = R"SQL(
SELECT status,
       COALESCE(resolved_block, 0),
       COALESCE(cancel_reason, '')
FROM offer_log
WHERE offer_id = ?
LIMIT 1;
)SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }

    if (sqlite3_bind_text(stmt, 1, offer_id.c_str(), -1, SQLITE_TRANSIENT)
        != SQLITE_OK) {
        sqlite3_finalize(stmt);
        throw std::runtime_error(sqlite3_errmsg(db));
    }

    OfferRow row;
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("offer row not found");
    }

    const char* status_text = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, 0));
    const char* reason_text = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, 2));
    row.status = status_text ? status_text : "";
    row.resolved_block = static_cast<xop::BlockHeight>(
        sqlite3_column_int64(stmt, 1));
    row.cancel_reason = reason_text ? reason_text : "";

    sqlite3_finalize(stmt);
    return row;
}

std::vector<ClosureEventRow> query_closure_events(
    sqlite3* db,
    const std::string& offer_id)
{
    constexpr const char* kSql = R"SQL(
SELECT event_type,
       COALESCE(previous_status, ''),
       observed_status,
       COALESCE(resolved_block, 0),
       COALESCE(closure_reason, '')
FROM offer_closure_events
WHERE offer_id = ?
ORDER BY id ASC;
)SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }

    if (sqlite3_bind_text(stmt, 1, offer_id.c_str(), -1, SQLITE_TRANSIENT)
        != SQLITE_OK) {
        sqlite3_finalize(stmt);
        throw std::runtime_error(sqlite3_errmsg(db));
    }

    std::vector<ClosureEventRow> events;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ClosureEventRow row;
        const char* event_type_text = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 0));
        const char* previous_status_text = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 1));
        const char* observed_status_text = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 2));
        const char* reason_text = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 4));
        row.event_type = event_type_text ? event_type_text : "";
        row.previous_status = previous_status_text ? previous_status_text : "";
        row.observed_status = observed_status_text ? observed_status_text : "";
        row.resolved_block = static_cast<xop::BlockHeight>(
            sqlite3_column_int64(stmt, 3));
        row.closure_reason = reason_text ? reason_text : "";
        events.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return events;
}

xop::DbOfferRecord make_offer(const std::string& offer_id)
{
    xop::DbOfferRecord offer;
    offer.offer_id = offer_id;
    offer.pair_name = "XCH/DBX";
    offer.side = "ask";
    offer.price_mojos = 123456789;
    offer.size_mojos = 1000;
    offer.tier = 1;
    offer.status = "pending";
    offer.created_block = 42;
    offer.fee_mojos = 5000;
    return offer;
}

}  // namespace

TEST(DatabaseTest, PreservesFirstCancelCauseAcrossReconcileObservation)
{
    TempDbPath temp_db{"xop_cancel_preserve"};
    const std::string offer_id = "offer-preserve";

    {
        xop::Database db(temp_db.path().string());
        db.insert_offer(make_offer(offer_id));
        db.update_offer_status(offer_id, "cancelled", 100, "utxo_liberation");
        db.update_offer_status(offer_id, "cancelled", 101, "on_chain_reconcile");
    }

    sqlite3* raw_db = open_db(temp_db.path());
    auto close_db = [&raw_db]() {
        if (raw_db) {
            sqlite3_close(raw_db);
            raw_db = nullptr;
        }
    };

    const OfferRow offer = query_offer_row(raw_db, offer_id);
    EXPECT_EQ(offer.status, "cancelled");
    EXPECT_EQ(offer.resolved_block, 100);
    EXPECT_EQ(offer.cancel_reason, "utxo_liberation");

    const auto events = query_closure_events(raw_db, offer_id);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].event_type, "status_update");
    EXPECT_EQ(events[0].previous_status, "pending");
    EXPECT_EQ(events[0].observed_status, "cancelled");
    EXPECT_EQ(events[0].resolved_block, 100);
    EXPECT_EQ(events[0].closure_reason, "utxo_liberation");
    EXPECT_EQ(events[1].event_type, "reconcile_observation");
    EXPECT_EQ(events[1].previous_status, "cancelled");
    EXPECT_EQ(events[1].observed_status, "cancelled");
    EXPECT_EQ(events[1].resolved_block, 101);
    EXPECT_EQ(events[1].closure_reason, "on_chain_reconcile");

    close_db();
}

TEST(DatabaseTest, SpecificCancelCauseReplacesGenericReconcileCause)
{
    TempDbPath temp_db{"xop_cancel_upgrade"};
    const std::string offer_id = "offer-upgrade";

    {
        xop::Database db(temp_db.path().string());
        db.insert_offer(make_offer(offer_id));
        db.update_offer_status(offer_id, "cancelled", 120, "periodic_reconcile");
        db.update_offer_status(offer_id, "cancelled", 121, "price_adverse(3.210%)");
    }

    sqlite3* raw_db = open_db(temp_db.path());
    auto close_db = [&raw_db]() {
        if (raw_db) {
            sqlite3_close(raw_db);
            raw_db = nullptr;
        }
    };

    const OfferRow offer = query_offer_row(raw_db, offer_id);
    EXPECT_EQ(offer.status, "cancelled");
    EXPECT_EQ(offer.resolved_block, 120);
    EXPECT_EQ(offer.cancel_reason, "price_adverse(3.210%)");

    const auto events = query_closure_events(raw_db, offer_id);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].event_type, "status_update");
    EXPECT_EQ(events[0].closure_reason, "periodic_reconcile");
    EXPECT_EQ(events[1].event_type, "status_update");
    EXPECT_EQ(events[1].previous_status, "cancelled");
    EXPECT_EQ(events[1].closure_reason, "price_adverse(3.210%)");

    close_db();
}

TEST(DatabaseTest, FilledStatusRemainsAuthoritativeAfterLaterReconcile)
{
    TempDbPath temp_db{"xop_fill_authoritative"};
    const std::string offer_id = "offer-filled";

    {
        xop::Database db(temp_db.path().string());
        db.insert_offer(make_offer(offer_id));
        db.update_offer_status(offer_id, "cancelled", 200, "periodic_reconcile");
        db.update_offer_status(offer_id, "filled", 201, "");
        db.update_offer_status(offer_id, "cancelled", 202, "on_chain_reconcile");
    }

    sqlite3* raw_db = open_db(temp_db.path());
    auto close_db = [&raw_db]() {
        if (raw_db) {
            sqlite3_close(raw_db);
            raw_db = nullptr;
        }
    };

    const OfferRow offer = query_offer_row(raw_db, offer_id);
    EXPECT_EQ(offer.status, "filled");
    EXPECT_EQ(offer.resolved_block, 201);
    EXPECT_TRUE(offer.cancel_reason.empty());

    const auto events = query_closure_events(raw_db, offer_id);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].event_type, "status_update");
    EXPECT_EQ(events[0].closure_reason, "periodic_reconcile");
    EXPECT_EQ(events[1].event_type, "status_update");
    EXPECT_TRUE(events[1].closure_reason.empty());
    EXPECT_EQ(events[2].event_type, "reconcile_observation");
    EXPECT_EQ(events[2].previous_status, "filled");
    EXPECT_EQ(events[2].observed_status, "cancelled");
    EXPECT_EQ(events[2].closure_reason, "on_chain_reconcile");

    close_db();
}

TEST(DatabaseTest, TradeCountsBySideSinceBlock)
{
    TempDbPath temp_db{"xop_trade_counts_by_side"};
    xop::Database db(temp_db.path().string());

    auto insert_trade = [&](const std::string& id, const std::string& pair, const std::string& side, xop::BlockHeight block) {
        xop::DbTradeRecord tr;
        tr.timestamp = "2026-09-04T12:00:00Z";
        tr.trade_id = id;
        tr.pair_name = pair;
        tr.side = side;
        tr.price_mojos = 1500000000000LL;
        tr.size_mojos = 1000000000000LL;
        tr.fee_mojos = 10000;
        tr.cost_basis_mojos = 1400000000000LL;
        tr.realized_pnl_mojos = 100;
        tr.block_height = block;
        db.insert_trade(tr);
    };

    insert_trade("t1", "XCH/BYC", "bid", 100);
    insert_trade("t2", "XCH/BYC", "bid", 150);
    insert_trade("t3", "XCH/BYC", "ask", 120);
    insert_trade("t4", "XCH/BYC", "ask", 200);
    insert_trade("t5", "XCH/BYC", "ask", 250);
    insert_trade("t6", "XCH/DBX", "bid", 250); // different pair

    // Query since block 100
    auto counts_100 = db.query_trade_counts_by_side("XCH/BYC", 100);
    EXPECT_EQ(counts_100.first, 2);  // 2 bids
    EXPECT_EQ(counts_100.second, 3); // 3 asks

    // Query since block 150
    auto counts_150 = db.query_trade_counts_by_side("XCH/BYC", 150);
    EXPECT_EQ(counts_150.first, 1);  // 1 bid (t2)
    EXPECT_EQ(counts_150.second, 2); // 2 asks (t4, t5)

    // Query since block 300 (future)
    auto counts_300 = db.query_trade_counts_by_side("XCH/BYC", 300);
    EXPECT_EQ(counts_300.first, 0);
    EXPECT_EQ(counts_300.second, 0);
}

// ===========================================================================
// inventory_state -- cost-basis persistence (PNL-BASIS-PERSIST, 2026-07-30)
// ===========================================================================

TEST(DatabaseTest, InventoryStateRoundTripsAcrossReopen) {
    TempDbPath temp("inventory_state_roundtrip");

    // XCH-scale cost: 1e12 mojos at a 1.39e12 USD-pseudo price = 1.39e24,
    // far beyond int64 -- this is exactly the magnitude that used to
    // overflow, so the REAL column must carry it faithfully.
    const double xch_cost = 1.39e24;

    {
        xop::Database db(temp.path().string());

        std::vector<xop::DbInventoryState> rows;
        xop::DbInventoryState xch;
        xch.asset_id               = "xch";
        xch.total_quantity         = 1'000'000'000'000LL;
        xch.total_cost             = xch_cost;
        xch.basis_is_seed_sentinel = false;
        rows.push_back(xch);

        xop::DbInventoryState cat;
        cat.asset_id               = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";
        cat.total_quantity         = 25'000LL;
        cat.total_cost             = 25'000.0;
        cat.basis_is_seed_sentinel = true;
        rows.push_back(cat);

        db.save_inventory_state(rows);
    }  // closes the connection -- simulated restart

    xop::Database db2(temp.path().string());
    const auto loaded = db2.load_inventory_state();
    ASSERT_EQ(loaded.size(), 2u);

    const xop::DbInventoryState* xch = nullptr;
    const xop::DbInventoryState* cat = nullptr;
    for (const auto& r : loaded) {
        if (r.asset_id == "xch") xch = &r;
        else cat = &r;
    }
    ASSERT_NE(xch, nullptr);
    ASSERT_NE(cat, nullptr);

    EXPECT_EQ(xch->total_quantity, 1'000'000'000'000LL);
    EXPECT_DOUBLE_EQ(xch->total_cost, xch_cost);
    EXPECT_FALSE(xch->basis_is_seed_sentinel);

    EXPECT_EQ(cat->total_quantity, 25'000LL);
    EXPECT_DOUBLE_EQ(cat->total_cost, 25'000.0);
    EXPECT_TRUE(cat->basis_is_seed_sentinel);
}

TEST(DatabaseTest, InventoryStateUpsertReplacesPriorSnapshot) {
    TempDbPath temp("inventory_state_upsert");
    xop::Database db(temp.path().string());

    xop::DbInventoryState rec;
    rec.asset_id               = "xch";
    rec.total_quantity         = 5'000'000'000'000LL;
    rec.total_cost             = 6.95e24;
    rec.basis_is_seed_sentinel = true;
    db.save_inventory_state({rec});

    // A later fill shrinks the position and clears the sentinel.
    rec.total_quantity         = 4'000'000'000'000LL;
    rec.total_cost             = 5.56e24;
    rec.basis_is_seed_sentinel = false;
    db.save_inventory_state({rec});

    const auto loaded = db.load_inventory_state();
    ASSERT_EQ(loaded.size(), 1u) << "upsert must not duplicate the asset row";
    EXPECT_EQ(loaded[0].total_quantity, 4'000'000'000'000LL);
    EXPECT_DOUBLE_EQ(loaded[0].total_cost, 5.56e24);
    EXPECT_FALSE(loaded[0].basis_is_seed_sentinel);
}

TEST(DatabaseTest, InventoryStateEmptyOnFirstRun) {
    TempDbPath temp("inventory_state_firstrun");
    xop::Database db(temp.path().string());
    EXPECT_TRUE(db.load_inventory_state().empty());
}

// ===========================================================================
// ledger_entries -- double-entry accounting (LEDGER 2026-07-30)
// ===========================================================================

namespace {

xop::DbLedgerEntry leg(const std::string& event_id, const std::string& leg_name,
                       const std::string& asset, xop::Mojo delta,
                       const char* type = "fill")
{
    xop::DbLedgerEntry e;
    e.entry_time  = "2026-07-30T12:00:00.000Z";
    e.event_type  = type;
    e.event_id    = event_id;
    e.leg         = leg_name;
    e.asset_id    = asset;
    e.delta_mojos = delta;
    return e;
}

}  // namespace

TEST(DatabaseTest, LedgerBalancesSumSignedLegs) {
    TempDbPath temp("ledger_sum");
    xop::Database db(temp.path().string());

    EXPECT_EQ(db.ledger_entry_count(), 0);
    EXPECT_TRUE(db.ledger_balances().empty());

    // Opening: 100 XCH and 500 wUSDC.b units (500,000 CAT mojos).
    ASSERT_EQ(db.append_ledger_entries({
        leg("genesis:xch", "opening", "xch", 100'000'000'000'000LL, "opening"),
        leg("genesis:usdc", "opening", "usdc", 500'000LL, "opening"),
    }).value_or(999), 2u);

    // An ask fill: sold 1 XCH for 1,350 wUSDC.b mojos, paid a 5,000 mojo fee.
    ASSERT_EQ(db.append_ledger_entries({
        leg("trade-1", "base",  "xch",  -1'000'000'000'000LL),
        leg("trade-1", "quote", "usdc",  1'350LL),
        leg("trade-1", "fee",   "xch",  -5'000LL),
    }).value_or(999), 3u);

    const auto bal = db.ledger_balances();
    ASSERT_EQ(bal.size(), 2u);
    EXPECT_EQ(bal.at("xch"), 100'000'000'000'000LL - 1'000'000'000'000LL - 5'000LL);
    EXPECT_EQ(bal.at("usdc"), 500'000LL + 1'350LL);
    EXPECT_EQ(db.ledger_entry_count(), 5);
}

TEST(DatabaseTest, LedgerRePostingSameEventIsIdempotent) {
    TempDbPath temp("ledger_idempotent");
    xop::Database db(temp.path().string());

    const std::vector<xop::DbLedgerEntry> legs = {
        leg("trade-dup", "base",  "xch",  -1'000'000'000'000LL),
        leg("trade-dup", "quote", "usdc",  1'350LL),
    };

    EXPECT_EQ(db.append_ledger_entries(legs).value_or(999), 2u);
    // A fill re-detected after a crash re-posts identical legs.  This is a
    // SUCCESSFUL write that inserted nothing -- distinct from a failure,
    // which returns nullopt.
    const auto replay = db.append_ledger_entries(legs);
    ASSERT_TRUE(replay.has_value()) << "a duplicate replay is not a failure";
    EXPECT_EQ(*replay, 0u) << "re-posting an existing event must insert nothing";

    const auto bal = db.ledger_balances();
    EXPECT_EQ(bal.at("xch"), -1'000'000'000'000LL)
        << "balance must not double-count the replayed event";
    EXPECT_EQ(bal.at("usdc"), 1'350LL);
    EXPECT_EQ(db.ledger_entry_count(), 2);
}

TEST(DatabaseTest, LedgerOpeningRecordedOncePerAsset) {
    TempDbPath temp("ledger_opening");
    xop::Database db(temp.path().string());

    EXPECT_FALSE(db.has_ledger_opening("xch"));
    ASSERT_EQ(db.append_ledger_entries({
        leg("genesis:xch", "opening", "xch", 61'685'000'000'000LL, "opening"),
    }).value_or(999), 1u);
    EXPECT_TRUE(db.has_ledger_opening("xch"));
    EXPECT_FALSE(db.has_ledger_opening("usdc"));

    // A second genesis attempt (e.g. a later restart) must not re-open.
    EXPECT_EQ(db.append_ledger_entries({
        leg("genesis:xch", "opening", "xch", 99'999'000'000'000LL, "opening"),
    }).value_or(999), 0u);
    EXPECT_EQ(db.ledger_balances().at("xch"), 61'685'000'000'000LL);
}

TEST(DatabaseTest, LedgerSurvivesReopen) {
    TempDbPath temp("ledger_reopen");
    {
        xop::Database db(temp.path().string());
        ASSERT_TRUE(db.append_ledger_entries({
            leg("genesis:xch", "opening", "xch", 10'000'000'000'000LL, "opening"),
            leg("trade-a", "base", "xch", -2'000'000'000'000LL),
        }).has_value());
    }
    xop::Database db2(temp.path().string());
    EXPECT_EQ(db2.ledger_balances().at("xch"), 8'000'000'000'000LL);
    EXPECT_TRUE(db2.has_ledger_opening("xch"));
}

// The invariant's whole purpose: a fill the bot recorded that the wallet
// never reflected shows up as a divergence of exactly the phantom amount.
TEST(DatabaseTest, LedgerRevealsPhantomFillAsDivergence) {
    TempDbPath temp("ledger_phantom");
    xop::Database db(temp.path().string());

    const xop::Mojo opening = 61'685'000'000'000LL;   // 61.685 XCH observed
    ASSERT_TRUE(db.append_ledger_entries({
        leg("genesis:xch", "opening", "xch", opening, "opening"),
    }).has_value());

    // The bot believes it sold 5 XCH, but the wallet balance never moved.
    ASSERT_TRUE(db.append_ledger_entries({
        leg("phantom-1", "base", "xch", -5'000'000'000'000LL),
    }).has_value());

    const xop::Mojo ledger_balance = db.ledger_balances().at("xch");
    const xop::Mojo wallet_confirmed = opening;   // unchanged on chain
    const xop::Mojo divergence = ledger_balance - wallet_confirmed;

    EXPECT_EQ(divergence, -5'000'000'000'000LL)
        << "the books should be short by exactly the phantom fill size";
}

// ============================================================================
// [AS-WARM] get_recent_snapshot_mids -- warm-start query (acceptance D)
// ============================================================================
//
// The volatility warm-start replays the newest N snapshot mids in ASCENDING
// time order.  Verify the query returns exactly that: newest N, ascending,
// zero-mid rows excluded, timestamps usable.

TEST(DatabaseTest, RecentSnapshotMidsAscendingFilteredAndLimited) {
    TempDbPath temp("warmstart_mids");
    xop::Database db(temp.path().string());

    // 120 rows for the pair under test, mids 1000..1119 at blocks 1..120,
    // with every 10th row given a zero mid (excluded at the SQL level), plus
    // decoy rows for another pair that must never leak in.
    std::vector<xop::DbSnapshot> batch;
    for (int i = 0; i < 120; ++i) {
        xop::DbSnapshot s;
        s.block_height    = static_cast<xop::BlockHeight>(i + 1);
        s.pair_name       = "XCH/wUSDC.b";
        s.mid_price_mojos = (i % 10 == 9) ? 0 : (1000 + i);
        batch.push_back(s);

        xop::DbSnapshot decoy;
        decoy.block_height    = static_cast<xop::BlockHeight>(i + 1);
        decoy.pair_name       = "XCH/DBX";
        decoy.mid_price_mojos = 777;
        batch.push_back(decoy);
    }
    db.insert_snapshots_batch(batch);

    // Limit 50: the NEWEST 50 positive-mid rows, ascending on return.
    const auto rows = db.get_recent_snapshot_mids("XCH/wUSDC.b", 50);
    ASSERT_EQ(rows.size(), 50u);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_GT(rows[i].mid_price_mojos, 0);          // filter applied
        EXPECT_GT(rows[i].unix_seconds, 0);             // created_at parsed
        if (i > 0) {
            EXPECT_GT(rows[i].block_height, rows[i - 1].block_height)
                << "rows must come back in ascending block order";
        }
        // mids encode their block: mid = 1000 + (block - 1).
        EXPECT_EQ(rows[i].mid_price_mojos,
                  1000 + static_cast<xop::Mojo>(rows[i].block_height) - 1);
    }
    // Newest positive-mid row is block 119 (block 120 has i=119, mid 1119 --
    // i%10==9 zeroes blocks 10,20,...,120, so the newest kept is block 119).
    EXPECT_EQ(rows.back().block_height, 119u);

    // Unknown pair: empty, not an error.
    EXPECT_TRUE(db.get_recent_snapshot_mids("NOPE/PAIR", 50).empty());
}

// ============================================================================
// [REWARD-INCOME 2026-08-01] A rewarded inflow is EXPLAINED flow: once the
// 'reward' entry is posted, the ledger balance equals the wallet balance and
// the invariant control (which compares ledger_balances() against the
// wallet's confirmed balance) sees zero divergence -- no blind adjusting
// entry.  Numbers are the live DBX state: opening 775,285 mojos, fills
// +201,294, the last two reward bursts 1,381 + 618 mojos.
// ============================================================================

TEST(DatabaseTest, RewardedInflowIsExplainedFlowNotDivergence) {
    TempDbPath temp("ledger_reward");
    xop::Database db(temp.path().string());

    const std::string dbx =
        "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20";

    ASSERT_TRUE(db.append_ledger_entries({
        leg("genesis:" + dbx, "opening", dbx, 775'285, "opening"),
        leg("fill-1", "quote", dbx, 100'000),
        leg("fill-2", "quote", dbx, 101'294),
    }).has_value());

    // Wallet then receives two reward bursts.  BEFORE the reward entries
    // are posted, the books are short by exactly the reward amount -- the
    // old failure mode, where the gap could only become an 'adjust' entry.
    const xop::Mojo wallet_confirmed = 775'285 + 201'294 + 1'381 + 618;
    EXPECT_EQ(db.ledger_balances().at(dbx) - wallet_confirmed, -1'999)
        << "unbooked rewards must show as ledger-short divergence";

    // step_ingest_reward_inflows posts one 'reward' entry per coin batch
    // member; idempotency key is the wallet transaction id.
    ASSERT_EQ(db.append_ledger_entries({
        leg("reward:0xaaa1", "reward", dbx, 1'381, "reward"),
        leg("reward:0xbbb2", "reward", dbx, 618, "reward"),
    }).value_or(0), 2u);

    // Explained: books tie to the wallet exactly; the invariant's
    // divergence (ledger - wallet) is zero.
    EXPECT_EQ(db.ledger_balances().at(dbx), wallet_confirmed);

    // Re-posting the same rewards after a crash/restart is a no-op (the
    // (event_id, leg, asset) uniqueness), so nothing double-books.
    EXPECT_EQ(db.append_ledger_entries({
        leg("reward:0xaaa1", "reward", dbx, 1'381, "reward"),
    }).value_or(99), 0u);
    EXPECT_EQ(db.ledger_balances().at(dbx), wallet_confirmed);
}


// ---------------------------------------------------------------------------
// [S25 2026-08-24] A wallet-reported terminal offer must leave 'pending'.
//
// detect_fills discovers externally cancelled/failed offers but previously
// only dropped them from in-memory State, so the offer_log row stayed
// 'pending' forever -- 48 rows had accumulated by 2026-08-24, the oldest 17
// days, and the GUI (which reads offer_log) kept showing an offer that dexie
// had long since cancelled.  These pin the write the engine now performs.
// ---------------------------------------------------------------------------
TEST(DatabaseTest, WalletTerminalStatusResolvesAPendingOffer)
{
    TempDbPath temp_db{"xop_s25_terminal"};
    const std::string offer_id = "offer-terminal";

    {
        xop::Database db(temp_db.path().string());
        db.insert_offer(make_offer(offer_id));
        db.update_offer_status(offer_id, "cancelled", 9195823,
                               "wallet reported terminal");
    }

    sqlite3* raw_db = open_db(temp_db.path());
    auto close_db = [&raw_db]() {
        if (raw_db) { sqlite3_close(raw_db); raw_db = nullptr; }
    };
    const auto row = query_offer_row(raw_db, offer_id);
    EXPECT_EQ(row.status, "cancelled")
        << "an offer the wallet reports terminal must not stay pending";
    EXPECT_EQ(row.resolved_block, 9195823);
    close_db();
}

// Re-observing the same terminal offer on a later heartbeat -- or a later
// process -- must be harmless.  The wallet keeps reporting a dead offer for
// as long as it remembers it, and one such offer was re-detected on all
// twelve process starts since 2026-08-18.
TEST(DatabaseTest, RepeatedTerminalObservationIsIdempotent)
{
    TempDbPath temp_db{"xop_s25_repeat"};
    const std::string offer_id = "offer-repeat";

    {
        xop::Database db(temp_db.path().string());
        db.insert_offer(make_offer(offer_id));
        for (int i = 0; i < 5; ++i) {
            db.update_offer_status(offer_id, "cancelled", 9195823 + i,
                                   "wallet reported terminal");
        }
    }

    sqlite3* raw_db = open_db(temp_db.path());
    auto close_db = [&raw_db]() {
        if (raw_db) { sqlite3_close(raw_db); raw_db = nullptr; }
    };
    EXPECT_EQ(query_offer_row(raw_db, offer_id).status, "cancelled");
    close_db();
}

// An offer this process never logged must not abort the heartbeat.  The
// engine catches the throw; this pins that the throw is what happens, so a
// future change to either side cannot silently drop the guard.
//
// [S25 2026-08-24] It must throw the TYPED OfferNotFound, not a bare
// runtime_error.  The engine discards a buffered terminal observation on
// this exception and RETRIES on any other, so the two cases have to be
// distinguishable by type.  They were not: the throw site raised the same
// "no offer found" text for every sqlite3_step result other than
// SQLITE_ROW, SQLITE_BUSY included, so a caller matching the message threw
// away transient failures as though the offer were unknown.
TEST(DatabaseTest, TerminalStatusForUnknownOfferThrowsOfferNotFound)
{
    TempDbPath temp_db{"xop_s25_unknown"};
    xop::Database db(temp_db.path().string());
    EXPECT_THROW(
        db.update_offer_status("offer-never-logged", "cancelled", 1, "x"),
        xop::OfferNotFound)
        << "the engine relies on this specific type to tell an unknown "
           "offer apart from a database that is merely busy";
}

// OfferNotFound must remain catchable as a std::exception: the heartbeat's
// outer handlers catch by that base, and a type that escaped them would
// abort the cycle instead of skipping one offer.
TEST(DatabaseTest, OfferNotFoundIsAStdException)
{
    TempDbPath temp_db{"xop_s25_unknown_base"};
    xop::Database db(temp_db.path().string());
    EXPECT_THROW(
        db.update_offer_status("offer-never-logged", "cancelled", 1, "x"),
        std::exception);
}

// The OTHER arm of the same contract, and the one that costs real work if it
// regresses.  The engine DISCARDS a buffered terminal observation on
// OfferNotFound and RETRIES on anything else, so a genuine database fault
// reported as OfferNotFound would permanently drop the write -- which is the
// stuck-pending accumulation this whole change set exists to stop.
//
// A fault is injected rather than simulated: a second connection drops
// offer_log out from under the live Database's prepared statement, so the
// lookup step fails with a real SQLite error instead of SQLITE_DONE.  The
// offer IS logged first, so "not found" cannot be the honest answer -- only
// a mapping bug could produce it.
TEST(DatabaseTest, DatabaseFaultDoesNotMasqueradeAsOfferNotFound)
{
    TempDbPath temp_db{"xop_s25_fault"};
    const std::string offer_id = "offer-fault";

    xop::Database db(temp_db.path().string());
    db.insert_offer(make_offer(offer_id));

    // Sanity: the row is there and resolvable by the normal path.
    ASSERT_NO_THROW(
        db.update_offer_status(offer_id, "pending", 1, "still open"));

    sqlite3* saboteur = open_db(temp_db.path());
    ASSERT_NE(saboteur, nullptr);
    char* err = nullptr;
    const int rc = sqlite3_exec(saboteur, "DROP TABLE offer_log;",
                                nullptr, nullptr, &err);
    const std::string drop_err = err ? err : "";
    if (err) { sqlite3_free(err); }
    sqlite3_close(saboteur);
    ASSERT_EQ(rc, SQLITE_OK) << "could not inject the fault: " << drop_err;

    try {
        db.update_offer_status(offer_id, "cancelled", 2, "wallet terminal");
        FAIL() << "a broken database must not report success";
    } catch (const xop::OfferNotFound& e) {
        FAIL() << "a database FAULT was reported as a missing offer: "
               << e.what()
               << " -- the engine discards on this type, so the "
                  "terminal write would be lost for good.";
    } catch (const std::exception& e) {
        SUCCEED() << "reported as a fault, so the engine retries: "
                  << e.what();
    }
}

// [S25 2026-08-24] A cancelled row is NOT reopened by a later non-fill
// update.
//
// This is why the engine buffers terminal observations to confirmation
// depth rather than writing them immediately.  An earlier revision wrote
// straight away, reasoning that a reorg-revived offer would simply be
// re-reported live and corrected -- but update_offer_status reopens a
// terminal row only for a FILL, so the correction has nowhere to land and
// the row stays cancelled for good.  This pins the one-way behaviour the
// buffering exists to respect; if it ever becomes reopenable, the gating
// can be revisited.
TEST(DatabaseTest, CancelledOfferIsNotReopenedByALaterPendingUpdate)
{
    TempDbPath temp_db{"xop_s25_oneway"};
    const std::string offer_id = "offer-oneway";

    {
        xop::Database db(temp_db.path().string());
        db.insert_offer(make_offer(offer_id));
        db.update_offer_status(offer_id, "cancelled", 100,
                               "wallet reported terminal");
        // A reorg revival would try to put it back to pending.
        db.update_offer_status(offer_id, "pending", 101, "revived");
    }

    sqlite3* raw_db = open_db(temp_db.path());
    auto close_db = [&raw_db]() {
        if (raw_db) { sqlite3_close(raw_db); raw_db = nullptr; }
    };
    EXPECT_EQ(query_offer_row(raw_db, offer_id).status, "cancelled")
        << "if this now reads 'pending', terminal writes are reversible and "
           "the confirmation-depth buffer in step_process_fills can be "
           "reconsidered";
    close_db();
}

// A fill DOES still override a cancelled row -- the one documented
// exception, and the reason the buffer protects cost basis rather than
// merely tidiness.
TEST(DatabaseTest, FillStillOverridesACancelledOffer)
{
    TempDbPath temp_db{"xop_s25_fillwins"};
    const std::string offer_id = "offer-fillwins";

    {
        xop::Database db(temp_db.path().string());
        db.insert_offer(make_offer(offer_id));
        db.update_offer_status(offer_id, "cancelled", 100, "wallet terminal");
        db.update_offer_status(offer_id, "filled", 101, "");
    }

    sqlite3* raw_db = open_db(temp_db.path());
    auto close_db = [&raw_db]() {
        if (raw_db) { sqlite3_close(raw_db); raw_db = nullptr; }
    };
    EXPECT_EQ(query_offer_row(raw_db, offer_id).status, "filled");
    close_db();
}
