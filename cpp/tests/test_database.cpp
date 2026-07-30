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

