// ---------------------------------------------------------------------------
// [S74 2026-09-20] A stop that KEEPS the resting offers.
//
// Engine::shutdown() had one behaviour -- cancel the whole book -- so whenever
// offers had to survive a restart the engine was hard-killed instead. A stop
// now carries a policy: the stop request's "offers=" line, or else the config
// default engine.shutdown_offers.
//
// These tests drive the production DECISIONS:
//   * xop/util/stop_offers_policy.hpp -- the spelling of a policy and the table
//     (request x config default x dry run);
//   * xop/execution/kept_book.hpp     -- what a keep stop tells the operator it
//     left behind, and until when it can live;
//   * xop/database.hpp offer_log_row_for -- the row a kept offer with no
//     offer_log row is given.
// The "offers=" line itself is parsed in shutdown_flag.hpp and tested in
// test_shutdown_flag.cpp, beside the rest of the request format.
//
// They do NOT cover the wiring in engine.cpp -- nothing in cpp/tests constructs
// an Engine (S36). tests/test_stop_keep_wiring.py pins that over the source
// text: the keep path reaches no cancel submit site, writes no intent, and
// disarms the dead man's switch before anything else.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/database.hpp"
#include "xop/execution/kept_book.hpp"
#include "xop/execution/offer_expiry.hpp"
#include "xop/types.hpp"
#include "xop/util/stop_offers_policy.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using xop::execution::describe_kept_book;
using xop::execution::format_unix_utc;
using xop::execution::KeptBookSummary;
using xop::execution::KeptOfferFacts;
using xop::execution::summarise_kept_book;
using xop::util::parse_stop_offers_policy;
using xop::util::plan_stop_offers;
using xop::util::stop_offers_request_from_value;
using xop::util::StopBookAction;
using xop::util::StopOffersPlan;
using xop::util::StopOffersPolicy;
using xop::util::StopOffersRequest;
using xop::util::StopPolicySource;

namespace {

// 2026-09-20 15:04:05 UTC.
constexpr std::int64_t kNow = 1'789'916'645;
constexpr std::uint32_t kDay = 86'400;

KeptOfferFacts offer(std::string pair, std::int64_t posted_unix_s,
                     std::uint32_t expiry_secs, bool cancel_pending = false)
{
    KeptOfferFacts f;
    f.pair_name      = std::move(pair);
    f.cancel_pending = cancel_pending;
    f.posted_unix_s  = posted_unix_s;
    f.expiry_secs    = expiry_secs;
    return f;
}

}  // namespace

// ===========================================================================
// 1. How a policy is spelled
// ===========================================================================

TEST(StopOffersPolicyParse, CancelAndKeepAreTheOnlyTwoWords)
{
    ASSERT_TRUE(parse_stop_offers_policy("cancel").has_value());
    EXPECT_EQ(*parse_stop_offers_policy("cancel"), StopOffersPolicy::Cancel);
    ASSERT_TRUE(parse_stop_offers_policy("keep").has_value());
    EXPECT_EQ(*parse_stop_offers_policy("keep"), StopOffersPolicy::Keep);
}

TEST(StopOffersPolicyParse, PaddingAndAsciiCaseAreTolerated)
{
    // A hand-written flag, or a config edited by hand, must not turn into the
    // OTHER policy over a capital letter or a trailing space.
    for (const std::string_view text : {"Keep", "KEEP", " keep", "keep ", "\tkeep\t", " kEeP \t"}) {
        const auto parsed = parse_stop_offers_policy(text);
        ASSERT_TRUE(parsed.has_value()) << "[" << text << "]";
        EXPECT_EQ(*parsed, StopOffersPolicy::Keep) << "[" << text << "]";
    }
    for (const std::string_view text : {"Cancel", "CANCEL", "  cancel  "}) {
        const auto parsed = parse_stop_offers_policy(text);
        ASSERT_TRUE(parsed.has_value()) << "[" << text << "]";
        EXPECT_EQ(*parsed, StopOffersPolicy::Cancel) << "[" << text << "]";
    }
}

TEST(StopOffersPolicyParse, AnythingElseIsNotAPolicyAndIsNeverGuessed)
{
    // Each of these is one keystroke from a real policy. None may read as one:
    // "keep-bids" read as keep would leave asks resting that the writer meant
    // to cancel, and a prefix match would make "kee" a policy.
    for (const std::string_view text :
         {"", " ", "kee", "keeps", "keep-bids", "keep;cancel", "cancel,keep", "cancelled",
          "cancel all", "yes", "no", "true", "false", "1", "0", "k e e p", "keep\n",
          "\"keep\"", "offers=keep"}) {
        EXPECT_FALSE(parse_stop_offers_policy(text).has_value()) << "[" << text << "]";
    }
}

TEST(StopOffersPolicyParse, ARequestValueIsCancelKeepOrUnrecognised)
{
    EXPECT_EQ(stop_offers_request_from_value("cancel"), StopOffersRequest::Cancel);
    EXPECT_EQ(stop_offers_request_from_value(" Keep "), StopOffersRequest::Keep);
    // A line that is THERE but unreadable is Unrecognised, never Unspecified:
    // the engine says which of the two it met, and the GUI's writer can only
    // produce the first two.
    EXPECT_EQ(stop_offers_request_from_value(""), StopOffersRequest::Unrecognised);
    EXPECT_EQ(stop_offers_request_from_value("keep-bids"), StopOffersRequest::Unrecognised);
}

// ===========================================================================
// 2. The decision table: request x config default x dry run
// ===========================================================================

TEST(StopOffersPlanTable, EveryRowOfTheTable)
{
    struct Row {
        StopOffersRequest requested;
        StopOffersPolicy  config_default;
        bool              dry_run;
        StopBookAction    action;
        StopPolicySource  source;
    };
    using Req = StopOffersRequest;
    using Pol = StopOffersPolicy;
    using Act = StopBookAction;
    using Src = StopPolicySource;
    const std::array<Row, 16> rows{{
        // live engine: an explicit request beats the default, both ways round
        {Req::Cancel,       Pol::Cancel, false, Act::CancelBook, Src::Request},
        {Req::Cancel,       Pol::Keep,   false, Act::CancelBook, Src::Request},
        {Req::Keep,         Pol::Cancel, false, Act::KeepBook,   Src::Request},
        {Req::Keep,         Pol::Keep,   false, Act::KeepBook,   Src::Request},
        // no policy in the request: the config default decides
        {Req::Unspecified,  Pol::Cancel, false, Act::CancelBook, Src::ConfigDefault},
        {Req::Unspecified,  Pol::Keep,   false, Act::KeepBook,   Src::ConfigDefault},
        // a policy this build cannot read: the config default, and SAID so
        {Req::Unrecognised, Pol::Cancel, false, Act::CancelBook, Src::ConfigDefaultUnreadable},
        {Req::Unrecognised, Pol::Keep,   false, Act::KeepBook,   Src::ConfigDefaultUnreadable},
        // dry run: nothing, whatever anyone asked for
        {Req::Cancel,       Pol::Cancel, true,  Act::DryRunNothing, Src::DryRun},
        {Req::Cancel,       Pol::Keep,   true,  Act::DryRunNothing, Src::DryRun},
        {Req::Keep,         Pol::Cancel, true,  Act::DryRunNothing, Src::DryRun},
        {Req::Keep,         Pol::Keep,   true,  Act::DryRunNothing, Src::DryRun},
        {Req::Unspecified,  Pol::Cancel, true,  Act::DryRunNothing, Src::DryRun},
        {Req::Unspecified,  Pol::Keep,   true,  Act::DryRunNothing, Src::DryRun},
        {Req::Unrecognised, Pol::Cancel, true,  Act::DryRunNothing, Src::DryRun},
        {Req::Unrecognised, Pol::Keep,   true,  Act::DryRunNothing, Src::DryRun},
    }};
    for (const Row& row : rows) {
        const StopOffersPlan plan =
            plan_stop_offers(row.requested, row.config_default, row.dry_run);
        EXPECT_EQ(plan.action, row.action)
            << "request=" << xop::util::stop_offers_request_name(row.requested)
            << " default=" << xop::util::stop_offers_policy_name(row.config_default)
            << " dry_run=" << row.dry_run;
        EXPECT_EQ(plan.source, row.source)
            << "request=" << xop::util::stop_offers_request_name(row.requested)
            << " default=" << xop::util::stop_offers_policy_name(row.config_default)
            << " dry_run=" << row.dry_run;
    }
}

TEST(StopOffersPlanTable, AnUpgradeChangesNothing)
{
    // A config written before the key existed parses to the zero value, and a
    // GUI or a console that predates the policy sends none. That stop must be
    // today's stop: cancel.
    const StopOffersPlan plan =
        plan_stop_offers(StopOffersRequest{}, StopOffersPolicy{}, /*dry_run=*/false);
    EXPECT_EQ(plan.action, StopBookAction::CancelBook);
    EXPECT_EQ(StopOffersPlan{}.action, StopBookAction::CancelBook);
}

TEST(StopOffersPlanTable, AnUnreadablePolicyNeverBecomesKeepOnItsOwn)
{
    // The fail-open reading of garbage would be "it said SOMETHING about
    // keeping, keep". With the default at cancel, garbage cancels.
    EXPECT_EQ(plan_stop_offers(StopOffersRequest::Unrecognised, StopOffersPolicy::Cancel, false)
                  .action,
              StopBookAction::CancelBook);
}

TEST(StopOffersPlanTable, EveryActionSourceAndRequestHasItsOwnLogName)
{
    std::set<std::string> actions;
    for (const auto a : {StopBookAction::CancelBook, StopBookAction::KeepBook,
                         StopBookAction::DryRunNothing}) {
        actions.insert(xop::util::stop_book_action_name(a));
    }
    EXPECT_EQ(actions.size(), 3u);

    std::set<std::string> sources;
    for (const auto s : {StopPolicySource::Request, StopPolicySource::ConfigDefault,
                         StopPolicySource::ConfigDefaultUnreadable, StopPolicySource::DryRun}) {
        sources.insert(xop::util::stop_policy_source_name(s));
    }
    EXPECT_EQ(sources.size(), 4u);

    std::set<std::string> requests;
    for (const auto r : {StopOffersRequest::Unspecified, StopOffersRequest::Cancel,
                         StopOffersRequest::Keep, StopOffersRequest::Unrecognised}) {
        requests.insert(xop::util::stop_offers_request_name(r));
    }
    EXPECT_EQ(requests.size(), 4u);

    // The config spelling round-trips through its own parser.
    for (const auto p : {StopOffersPolicy::Cancel, StopOffersPolicy::Keep}) {
        const auto back = parse_stop_offers_policy(xop::util::stop_offers_policy_name(p));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, p);
    }
}

TEST(StopOffersPlanTable, TheHelpTokenNamesBothPolicies)
{
    // gui/stop_offers.py carries the same bytes and tests/test_stop_offers.py
    // reads this header to hold them together; this only pins that the token
    // says what the engine actually accepts.
    const std::string_view token = xop::util::kStopPolicyHelpToken;
    EXPECT_NE(token.find("offers="), std::string_view::npos);
    EXPECT_NE(token.find("cancel"), std::string_view::npos);
    EXPECT_NE(token.find("keep"), std::string_view::npos);
}

// ===========================================================================
// 3. What a keep stop says it left behind
// ===========================================================================

TEST(KeptBook, CountsRestingOffersPerPairAndLeavesCancelPendingOut)
{
    const std::vector<KeptOfferFacts> offers{
        offer("XCH/DBX", kNow - 3600, kDay),
        offer("XCH/BYC", kNow - 7200, kDay),
        offer("XCH/DBX", kNow - 60, kDay),
        offer("XCH/DBX", kNow - 90, kDay, /*cancel_pending=*/true),
    };
    const KeptBookSummary s = summarise_kept_book(offers, kNow);
    EXPECT_EQ(s.resting, 3u);
    EXPECT_EQ(s.cancel_in_flight, 1u);
    ASSERT_EQ(s.per_pair.size(), 2u);
    EXPECT_EQ(s.per_pair[0].first, "XCH/BYC");   // sorted by name
    EXPECT_EQ(s.per_pair[0].second, 1u);
    EXPECT_EQ(s.per_pair[1].first, "XCH/DBX");
    EXPECT_EQ(s.per_pair[1].second, 2u);         // the cancel_pending one is NOT here
}

TEST(KeptBook, SoonestAndLatestExpiryComeFromTheOffersThisProcessPosted)
{
    const std::vector<KeptOfferFacts> offers{
        offer("XCH/DBX", kNow - 3600, kDay),      // expires kNow + 82800
        offer("XCH/BYC", kNow - 7200, 2 * kDay),  // expires kNow + 165600 (pair override)
        offer("XCH/DBX", kNow - 80000, kDay),     // expires kNow + 6400   <- soonest
    };
    const KeptBookSummary s = summarise_kept_book(offers, kNow);
    EXPECT_EQ(s.expiry_known, 3u);
    EXPECT_EQ(s.soonest_expiry_unix_s, kNow + 6400);
    EXPECT_EQ(s.latest_expiry_unix_s, kNow + 165600);
    EXPECT_EQ(s.expiry_unseen, 0u);
    EXPECT_EQ(s.no_expiry, 0u);
}

TEST(KeptBook, ACancelPendingOfferNeverMovesTheExpiryBounds)
{
    // Its cancel is already out; it is not something this stop "kept", and an
    // old one would otherwise drag the soonest expiry into the past.
    const std::vector<KeptOfferFacts> offers{
        offer("XCH/DBX", kNow - 3600, kDay),
        offer("XCH/DBX", kNow - 5 * 86400, kDay, /*cancel_pending=*/true),
    };
    const KeptBookSummary s = summarise_kept_book(offers, kNow);
    EXPECT_EQ(s.expiry_known, 1u);
    EXPECT_EQ(s.soonest_expiry_unix_s, kNow + 82800);
    EXPECT_EQ(s.latest_expiry_unix_s, kNow + 82800);
}

TEST(KeptBook, ARestoredOfferHasNoKnownExpiryOnlyABound)
{
    // posted_unix_s == 0: restored from offer_log at boot, or adopted. This
    // process never saw what it was posted with, so the summary must not
    // invent a date -- only the bound that holds IF it carries the expiry.
    const std::vector<KeptOfferFacts> offers{
        offer("XCH/DBX", 0, kDay),
        offer("XCH/BYC", 0, 2 * kDay),
    };
    const KeptBookSummary s = summarise_kept_book(offers, kNow);
    EXPECT_EQ(s.resting, 2u);
    EXPECT_EQ(s.expiry_known, 0u);
    EXPECT_EQ(s.soonest_expiry_unix_s, 0);
    EXPECT_EQ(s.latest_expiry_unix_s, 0);
    EXPECT_EQ(s.expiry_unseen, 2u);
    EXPECT_EQ(s.unseen_expiry_bound_unix_s, kNow + 2 * kDay);  // the LONGEST of them
}

TEST(KeptBook, APairWithNoExpiryIsSaidToHaveNone)
{
    const std::vector<KeptOfferFacts> offers{
        offer("XCH/DBX", kNow - 3600, 0),
        offer("XCH/DBX", 0, 0),
    };
    const KeptBookSummary s = summarise_kept_book(offers, kNow);
    EXPECT_EQ(s.no_expiry, 2u);
    EXPECT_EQ(s.expiry_known, 0u);
    EXPECT_EQ(s.expiry_unseen, 0u);

    const std::string text = describe_kept_book(s);
    EXPECT_NE(text.find("2 carry NO on-chain expiry"), std::string::npos) << text;
    EXPECT_EQ(text.find("stop being takeable between"), std::string::npos) << text;
}

TEST(KeptBook, TheLineNamesCountPairsAndBothExpiryBounds)
{
    const std::vector<KeptOfferFacts> offers{
        offer("XCH/DBX", kNow - 3600, kDay),
        offer("XCH/DBX", kNow - 80000, kDay),
        offer("XCH/BYC", 0, kDay),
        offer("XCH/BYC", kNow - 10, kDay, /*cancel_pending=*/true),
    };
    const std::string text = describe_kept_book(summarise_kept_book(offers, kNow));
    EXPECT_NE(text.find("3 offer(s) left RESTING on the book and NOT cancelled"),
              std::string::npos) << text;
    EXPECT_NE(text.find("XCH/BYC 1, XCH/DBX 2"), std::string::npos) << text;
    EXPECT_NE(text.find("1 more already had a cancel in flight"), std::string::npos) << text;
    // soonest = kNow + 6400, latest = kNow + 82800, unseen bound = kNow + 86400
    EXPECT_NE(text.find(format_unix_utc(kNow + 6400) + " (soonest)"), std::string::npos) << text;
    EXPECT_NE(text.find(format_unix_utc(kNow + 82800) + " (latest)"), std::string::npos) << text;
    EXPECT_NE(text.find("no later than " + format_unix_utc(kNow + kDay)), std::string::npos)
        << text;
    EXPECT_NE(text.find("TAKEABLE and UNMANAGED"), std::string::npos) << text;
}

TEST(KeptBook, AnEmptyBookSaysSoAndClaimsNothing)
{
    EXPECT_NE(describe_kept_book(summarise_kept_book({}, kNow)).find("no offers were resting"),
              std::string::npos);

    // Only cancel_pending offers: still nothing was KEPT.
    const std::vector<KeptOfferFacts> only_pending{
        offer("XCH/DBX", kNow - 10, kDay, /*cancel_pending=*/true)};
    const std::string text = describe_kept_book(summarise_kept_book(only_pending, kNow));
    EXPECT_NE(text.find("no offers were resting"), std::string::npos) << text;
    EXPECT_NE(text.find("1 already had a cancel in flight"), std::string::npos) << text;
    EXPECT_EQ(text.find("left RESTING"), std::string::npos) << text;
}

TEST(KeptBook, UnixSecondsAreFormattedAsUtcCivilTime)
{
    EXPECT_EQ(format_unix_utc(0), "1970-01-01 00:00:00 UTC");
    EXPECT_EQ(format_unix_utc(kNow), "2026-09-20 15:04:05 UTC");
    EXPECT_EQ(format_unix_utc(1'709'251'199), "2024-02-29 23:59:59 UTC");  // leap day
    EXPECT_EQ(format_unix_utc(1'709'251'200), "2024-03-01 00:00:00 UTC");
    EXPECT_EQ(format_unix_utc(1'616'162'400), "2021-03-19 14:00:00 UTC");  // kMinPlausibleUnixTime
}

// ===========================================================================
// 4. The offer_log row a kept offer with no row is given
// ===========================================================================

TEST(KeptBookFlush, AStateOfferBecomesARowTheNextBootRestoresUnchanged)
{
    xop::PendingOffer po{};
    po.offer_id         = "0xabc";
    po.pair_name        = "XCH/DBX";
    po.side             = xop::Side::Ask;
    po.price            = 123'456;
    po.size             = 7'000'000;
    po.tier             = 2;
    po.created_at_block = 9'300'000;
    po.fee_mojos        = 10'000'000;

    const xop::DbOfferRecord rec = xop::offer_log_row_for(po);
    EXPECT_EQ(rec.status, "pending");
    EXPECT_EQ(rec.side, "ask");

    // The round trip the next boot performs (engine.cpp restore loop).
    const xop::PendingOffer back = xop::pending_offer_from_db(rec);
    EXPECT_EQ(back.offer_id, po.offer_id);
    EXPECT_EQ(back.pair_name, po.pair_name);
    EXPECT_EQ(back.side, po.side);
    EXPECT_EQ(back.price, po.price);
    EXPECT_EQ(back.size, po.size);
    EXPECT_EQ(back.tier, po.tier);
    EXPECT_EQ(back.created_at_block, po.created_at_block);
    EXPECT_EQ(back.fee_mojos, po.fee_mojos);
    EXPECT_FALSE(back.cancel_pending);
}

TEST(KeptBookFlush, AnOfferWhoseCancelIsAlreadyOutIsNeverWrittenLive)
{
    xop::PendingOffer po{};
    po.offer_id       = "0xdef";
    po.pair_name      = "XCH/BYC";
    po.side           = xop::Side::Bid;
    po.cancel_pending = true;

    const xop::DbOfferRecord rec = xop::offer_log_row_for(po);
    EXPECT_EQ(rec.status, "cancel_pending");
    EXPECT_EQ(rec.side, "bid");
    EXPECT_TRUE(xop::pending_offer_from_db(rec).cancel_pending);
}

// ===========================================================================
// 5. After a keep stop: an offer past its on-chain expiry is past the hard TTL
// ===========================================================================
//
// The reference wallet never reacts to max_time passing: an untaken maker
// offer stays PENDING_ACCEPT and its coins stay locked until it is cancelled
// (chia-blockchain 2.7.4, chia/wallet/trade_manager.py -- no set_status call
// depends on time, and TradeStatus.EXPIRED is referenced nowhere). So after a
// keep stop longer than the expiry, startup_reconcile restores such an offer
// as known and live. What stops it being QUOTED as live is the engine's own
// hard TTL, which classify_tier_staleness applies unconditionally -- and the
// config floor (expiry_outlasts_hard_ttl) is what guarantees an expired offer
// is always past it. This pins that argument, including how much slower than
// configured the chain would have to run to break it.

TEST(KeptBookAfterRestart, AnExpiredOfferIsAlwaysPastTheHardTtl)
{
    constexpr std::uint32_t kHardMult = 2;  // OfferManager::kHardTtlMultiplier
    struct Case {
        std::uint32_t ttl_blocks;
        double        secs_per_block;   // strategy.block_time_seconds
    };
    for (const Case c : {Case{600, 52.0}, Case{200, 52.0}, Case{60, 18.75}, Case{1000, 30.0}}) {
        // The SMALLEST expiry the engine accepts for this config.
        std::uint32_t expiry = 1;
        while (!xop::execution::expiry_outlasts_hard_ttl(expiry, c.ttl_blocks, kHardMult,
                                                         c.secs_per_block)) {
            ++expiry;
        }
        const double hard_ttl_blocks = static_cast<double>(c.ttl_blocks) * kHardMult;
        // Blocks that pass while such an offer lives out its expiry, with the
        // chain running at the configured rate and at HALF of it.
        const double at_rate      = static_cast<double>(expiry) / c.secs_per_block;
        const double at_half_rate = static_cast<double>(expiry) / (2.0 * c.secs_per_block);
        EXPECT_GT(at_rate, hard_ttl_blocks * xop::execution::kExpiryFloorMargin - 1.0)
            << "ttl=" << c.ttl_blocks;
        EXPECT_GE(at_half_rate, hard_ttl_blocks)
            << "ttl=" << c.ttl_blocks << ": an expired offer could still be inside the "
            << "hard TTL and be kept as a live quote after a restart";
    }
}
