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
