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

using xop::execution::describe_cut_cycle;
using xop::execution::describe_kept_book;
using xop::execution::describe_untracked_create;
using xop::execution::describe_watchdog_disarm;
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
// 2b. [review #165] A keep stop waits for an in-flight post, and only so long
// ===========================================================================
//
// A signal can deliver a keep stop while Step 8 is awaiting create_offer.
// Stopping at once would let the wallet finish a create nobody records, and the
// next boot may CANCEL that orphan. So the stop waits while a post is in
// flight -- bounded, because a wallet that never answers must not turn a stop
// into a hang.

TEST(KeepStopDrain, NothingInFlightProceedsAtOnceHoweverLongItHasWaited)
{
    using xop::util::keep_stop_drain_step;
    using xop::util::KeepStopDrainStep;
    // The GUI's case: shutdown.flag is read between cycles, nothing is posting.
    EXPECT_EQ(keep_stop_drain_step(false, 0, 60'000), KeepStopDrainStep::Proceed);
    EXPECT_EQ(keep_stop_drain_step(false, 59'999, 60'000), KeepStopDrainStep::Proceed);
    EXPECT_EQ(keep_stop_drain_step(false, 600'000, 60'000), KeepStopDrainStep::Proceed);
}

TEST(KeepStopDrain, APostInFlightIsWaitedForUntilTheBudgetAndNoLonger)
{
    using xop::util::keep_stop_drain_step;
    using xop::util::KeepStopDrainStep;
    EXPECT_EQ(keep_stop_drain_step(true, 0, 60'000), KeepStopDrainStep::Wait);
    EXPECT_EQ(keep_stop_drain_step(true, 59'999, 60'000), KeepStopDrainStep::Wait);
    // AT the budget it gives up: a stop must end even if the wallet never answers.
    EXPECT_EQ(keep_stop_drain_step(true, 60'000, 60'000), KeepStopDrainStep::GiveUp);
    EXPECT_EQ(keep_stop_drain_step(true, 600'000, 60'000), KeepStopDrainStep::GiveUp);
    // A zero budget never waits -- and never pretends nothing was in flight.
    EXPECT_EQ(keep_stop_drain_step(true, 0, 0), KeepStopDrainStep::GiveUp);
}

TEST(KeepStopDrain, OneRpcCallsWorstCaseIsEveryAttemptPlusTheBackoffBetweenThem)
{
    using xop::util::rpc_call_worst_case_ms;
    // rpc_post: max_retries + 1 attempts, each able to spend the whole request
    // timeout, with the backoff doubling from retry_base_delay between them.
    EXPECT_EQ(rpc_call_worst_case_ms(30'000, 1, 500), 30'000ull);
    EXPECT_EQ(rpc_call_worst_case_ms(30'000, 2, 500), 60'500ull);
    EXPECT_EQ(rpc_call_worst_case_ms(30'000, 4, 500), 123'500ull);  // as shipped
    EXPECT_EQ(xop::util::kShippedRpcWorstCaseMs, 123'500ull);
    // Degenerate inputs answer, and never wrap.
    EXPECT_EQ(rpc_call_worst_case_ms(30'000, 0, 500), 0ull);
    EXPECT_EQ(rpc_call_worst_case_ms(~0ull, 4, ~0ull), ~0ull);
}

TEST(KeepStopDrain, TheBudgetIsTheMarkedWindowsWorstCaseNotARoundNumber)
{
    using xop::util::keep_stop_drain_budget_ms;
    const unsigned long long one = xop::util::kShippedRpcWorstCaseMs;
    // [review round 3] THE POINT OF THIS TEST. The drain waits for ONE create
    // and the publish that stands between the wallet's answer and the offer
    // entering State (offer_manager.cpp PostingMark). Two rpc_post calls, so
    // the budget must outlast two of them -- the 60 s constant this replaced
    // could not even outlast one, and gave up with the create still in flight:
    // exactly the orphan the drain exists to prevent.
    EXPECT_GE(keep_stop_drain_budget_ms(one, one), 2 * one);
    EXPECT_EQ(keep_stop_drain_budget_ms(one, one), 247'000ull);
    // A stop still ends: the cap binds only on a configuration far outside the
    // shipped one, and the floor keeps a 0 ms timeout from making the wait a
    // no-op that is never seen to fail.
    EXPECT_EQ(keep_stop_drain_budget_ms(~0ull, ~0ull),
              xop::util::kKeepStopDrainBudgetCapMs);
    EXPECT_EQ(keep_stop_drain_budget_ms(0, 0), xop::util::kKeepStopDrainBudgetFloorMs);
    EXPECT_LT(keep_stop_drain_budget_ms(one, one), xop::util::kKeepStopDrainBudgetCapMs);
    // The cycle being waited on runs on the same thread and carries on for up
    // to one poll after its create returns.
    EXPECT_GE(xop::util::kKeepStopDrainPollMs, 10ull);
    EXPECT_LE(xop::util::kKeepStopDrainPollMs, 250ull);
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

    const std::string text = describe_kept_book(s, false, false);
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
    const std::string text =
        describe_kept_book(summarise_kept_book(offers, kNow), false, false);
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

TEST(KeptBook, AnEmptyBookSaysSoOnlyWithNoCancelAndNoUnbookedCreate)
{
    // [review -- round 6] THE OLD NAME WAS THE DEFECT AGAIN. This test was
    // `AnEmptyBookSaysSoOnlyWhenNoCancelIsInFlight`, and it passed an empty
    // summary with NO untracked-create fact anywhere in it -- so the gtest
    // added to "read what the operator reads" blessed the all-clear for a stop
    // that had just told the operator an offer may be unrecorded. The only
    // state in which "nothing was left on the book" is true is this one.
    const std::string empty = describe_kept_book(summarise_kept_book({}, kNow),
                                                 /*post_abandoned=*/false,
                                                 /*create_outcome_unknown=*/false);
    EXPECT_NE(empty.find("no offers were resting and none had a cancel in flight"),
              std::string::npos) << empty;
    EXPECT_NE(empty.find("nothing was left on the book"), std::string::npos) << empty;
}

// The three states in which it is NOT true. Each is a stop that ALSO printed an
// error saying the wallet may hold an offer this engine never recorded -- and
// those errors qualify the COUNT ("NOT in the count above"), so not one of them
// retracts the word "nothing". The all-clear is printed FIRST, so nothing
// downstream can be relied on to take it back.
TEST(KeptBook, AnUnbookedCreateIsNeverDescribedAsAnEmptyBook)
{
    const KeptBookSummary none = summarise_kept_book({}, kNow);
    const std::array<std::pair<bool, bool>, 3> unbooked{{
        {true, false},   // the drain budget ran out with a create in flight
        {false, true},   // a create ended with no answer from the wallet
        {true, true},    // both
    }};
    for (const auto& [abandoned, unknown] : unbooked) {
        const std::string text = describe_kept_book(none, abandoned, unknown);
        EXPECT_EQ(text.find("nothing was left on the book"), std::string::npos)
            << text;
        EXPECT_EQ(text.find("no offers were resting and none"), std::string::npos)
            << text;
        EXPECT_NE(text.find("UNACCOUNTED FOR"), std::string::npos) << text;
        EXPECT_NE(text.find("DO NOT read this as an empty book"), std::string::npos)
            << text;
        EXPECT_NE(text.find("NO count here includes it"), std::string::npos) << text;
    }
}

// The same fact on the other two shapes: every branch states a count taken from
// State, and an offer built from a create this process never booked is in none
// of them.
TEST(KeptBook, AnUnbookedCreateQualifiesTheCountOnEveryShapeOfBook)
{
    const std::vector<KeptOfferFacts> resting{offer("XCH/DBX", kNow - 3600, kDay)};
    const std::string with_resting =
        describe_kept_book(summarise_kept_book(resting, kNow), true, false);
    EXPECT_NE(with_resting.find("1 offer(s) left RESTING"), std::string::npos)
        << with_resting;
    EXPECT_NE(with_resting.find("Beyond that count,"), std::string::npos)
        << with_resting;
    EXPECT_NE(with_resting.find("UNACCOUNTED FOR"), std::string::npos) << with_resting;

    const std::vector<KeptOfferFacts> only_pending{
        offer("XCH/BYC", kNow - 10, kDay, /*cancel_pending=*/true)};
    const std::string with_pending =
        describe_kept_book(summarise_kept_book(only_pending, kNow), false, true);
    EXPECT_NE(with_pending.find("this stop left no offer RESTING"), std::string::npos)
        << with_pending;
    EXPECT_NE(with_pending.find("UNACCOUNTED FOR"), std::string::npos) << with_pending;

    // ...and none of the three says it when there is nothing to say.
    for (const KeptBookSummary& s : {summarise_kept_book({}, kNow),
                                     summarise_kept_book(resting, kNow),
                                     summarise_kept_book(only_pending, kNow)}) {
        const std::string clean = describe_kept_book(s, false, false);
        EXPECT_EQ(clean.find("UNACCOUNTED FOR"), std::string::npos) << clean;
    }
}

// The clause names WHICH fact, because the two are not the same failure and the
// operator's next move differs: a drain that ran out has an error line with a
// wait in ms beside it, a create with no answer never had one to wait for.
TEST(KeptBook, TheUnbookedCreateClauseNamesWhichFactHolds)
{
    EXPECT_TRUE(describe_untracked_create(false, false).empty());
    EXPECT_NE(describe_untracked_create(true, false).find(
                  "still in flight when this stop's drain budget ran out"),
              std::string::npos);
    EXPECT_EQ(describe_untracked_create(true, false).find("no answer from the wallet"),
              std::string::npos);
    EXPECT_NE(describe_untracked_create(false, true).find(
                  "ended with no answer from the wallet, which is not a refusal"),
              std::string::npos);
    const std::string both = describe_untracked_create(true, true);
    EXPECT_NE(both.find("drain budget ran out, AND one ended with no answer"),
              std::string::npos) << both;
}

// [review] THE OLD NAME WAS THE DEFECT. `AnEmptyBookSaysSoAndClaimsNothing`
// blessed a line that said "nothing was left on the book" for a book whose
// every offer was cancel_pending -- which summarise_kept_book deliberately
// diverts out of `resting`, out of `per_pair` and out of the expiry
// accounting.
//
// This repo's rule is the opposite of that reassurance: PENDING_CANCEL means
// the wallet INTENDS to cancel, a bulk cancel can fail to broadcast most of
// its spends with no error anywhere, and the only ground truth is a spent
// maker coin. 24 such offers stayed takeable on dexie for 2.5 h after a
// cancel-all on 2026-08-29; three XCH/BYC bids stayed takeable for 13 days.
// The keep stop then disarms the dead man's switch and exits, so nothing
// chases them until the next start adopts them.
TEST(KeptBook, ACancelInFlightIsNeverDescribedAsAnEmptyBook)
{
    const std::vector<KeptOfferFacts> only_pending{
        offer("XCH/DBX", kNow - 10, kDay, /*cancel_pending=*/true)};
    const std::string text =
        describe_kept_book(summarise_kept_book(only_pending, kNow), false, false);

    // Nothing was KEPT -- that part was right and stays.
    EXPECT_EQ(text.find("left RESTING"), std::string::npos) << text;
    EXPECT_NE(text.find("this stop left no offer RESTING"), std::string::npos) << text;
    EXPECT_NE(text.find("1 already had a cancel in flight"), std::string::npos) << text;

    // ...and the book is NOT claimed to be empty, in any of its spellings.
    EXPECT_EQ(text.find("nothing was left on the book"), std::string::npos) << text;
    EXPECT_EQ(text.find("no offers were resting and none"), std::string::npos) << text;
    // The operator is told the thing that decides what to do next.
    EXPECT_NE(text.find("STILL TAKEABLE"), std::string::npos) << text;
    EXPECT_NE(text.find("is NOT proof"), std::string::npos) << text;
}

// The same rule on the non-empty line: "not counted" must not read as "gone".
TEST(KeptBook, ACancelInFlightBesideARestingBookIsAlsoSaidToBeTakeable)
{
    const std::vector<KeptOfferFacts> mixed{
        offer("XCH/DBX", kNow - 3600, kDay),
        offer("XCH/BYC", kNow - 10, kDay, /*cancel_pending=*/true),
        offer("XCH/BYC", kNow - 20, kDay, /*cancel_pending=*/true),
    };
    const std::string text =
        describe_kept_book(summarise_kept_book(mixed, kNow), false, false);
    EXPECT_NE(text.find("1 offer(s) left RESTING"), std::string::npos) << text;
    EXPECT_NE(text.find("2 more already had a cancel in flight"), std::string::npos) << text;
    EXPECT_NE(text.find("STILL TAKEABLE"), std::string::npos) << text;
    EXPECT_NE(text.find("is NOT proof"), std::string::npos) << text;
}

// ===========================================================================
// 3b. The two sentences the wiring scan structurally cannot read
// ===========================================================================
//
// tests/test_stop_keep_wiring.py pins Engine::report_offers_kept_on_stop over
// the SOURCE TEXT, and _code() empties every string literal before it matches.
// So a log line can assert anything at all and the scan still passes -- which
// is exactly how an unconditional "No offer post was left unrecorded." shipped
// beside the two facts that contradict it. These sentences therefore live in
// kept_book.hpp, and these tests read what the operator reads.

TEST(CutCycleLine, WithNothingOutstandingItSaysThePostsAreRecorded)
{
    const std::string text = describe_cut_cycle(/*post_abandoned=*/false,
                                                /*create_outcome_unknown=*/false);
    EXPECT_NE(text.find("arrived by SIGNAL while a heartbeat cycle was in flight"),
              std::string::npos) << text;
    EXPECT_NE(text.find("every offer post this process began is recorded above"),
              std::string::npos) << text;
    EXPECT_EQ(text.find("MAY HAVE BEEN LEFT UNRECORDED"), std::string::npos) << text;
}

// THE MERGE BLOCKER, stated as a test. post_abandoned IMPLIES this branch --
// the only path that sets posting_in_flight_ runs inside the marked cycle --
// so an unconditional reassurance here contradicts the error line above it on
// EVERY stop that abandons a post, not on some of them.
TEST(CutCycleLine, AnAbandonedPostIsNeverFollowedByAnAllClear)
{
    const std::string text = describe_cut_cycle(/*post_abandoned=*/true,
                                                /*create_outcome_unknown=*/false);
    EXPECT_NE(text.find("AN OFFER POST MAY HAVE BEEN LEFT UNRECORDED"),
              std::string::npos) << text;
    EXPECT_NE(text.find("still in flight when the drain budget ran out"),
              std::string::npos) << text;
    EXPECT_NE(text.find("NOT an all-clear"), std::string::npos) << text;
    // The sentence that used to be printed here regardless.
    EXPECT_EQ(text.find("No offer post was left unrecorded"), std::string::npos) << text;
    EXPECT_EQ(text.find("every offer post this process began is recorded above"),
              std::string::npos) << text;
}

TEST(CutCycleLine, ACreateWithNoAnswerIsNeverFollowedByAnAllClear)
{
    const std::string text = describe_cut_cycle(/*post_abandoned=*/false,
                                                /*create_outcome_unknown=*/true);
    EXPECT_NE(text.find("AN OFFER POST MAY HAVE BEEN LEFT UNRECORDED"),
              std::string::npos) << text;
    EXPECT_NE(text.find("no answer from the wallet, which is not a refusal"),
              std::string::npos) << text;
    EXPECT_EQ(text.find("every offer post this process began is recorded above"),
              std::string::npos) << text;
}

TEST(CutCycleLine, BothFactsAreNamedWhenBothHold)
{
    const std::string text = describe_cut_cycle(/*post_abandoned=*/true,
                                                /*create_outcome_unknown=*/true);
    EXPECT_NE(text.find("still in flight when the drain budget ran out, AND a "
                        "create ended with no answer"), std::string::npos) << text;
    EXPECT_EQ(text.find("every offer post this process began is recorded above"),
              std::string::npos) << text;
}

// [review] TODO.md S76 (c) contradicts the clause this line used to end with.
// A cancel really is recovered from the wallet's record; a TAKE completed
// after the cut is booked nowhere -- no trade row, no P&L -- exactly as after
// a hard kill. Said on every combination, because it is true on every one.
TEST(CutCycleLine, ACutTakeIsNeverClaimedToBeRecovered)
{
    for (const bool abandoned : {false, true}) {
        for (const bool unknown : {false, true}) {
            const std::string text = describe_cut_cycle(abandoned, unknown);
            EXPECT_NE(text.find("adopts a cancel from the wallet's PENDING_CANCEL record"),
                      std::string::npos) << text;
            EXPECT_NE(text.find("booked NOWHERE in this engine"), std::string::npos)
                << text;
            EXPECT_EQ(text.find("the next start reads the result from the wallet"),
                      std::string::npos) << text;
        }
    }
}

// [review] engine.hpp: "A cancel the switch had ALREADY started before the
// operator asked is not recalled: it holds the mutex, and it was a real
// firing." The log line said "the dead man's switch is disarmed for this stop"
// flat. watchdog_fired_ is latched BEFORE that cancel and never cleared, so it
// is exactly the fact the sentence needs.
TEST(WatchdogDisarmLine, ItIsFlatOnlyWhenTheSwitchNeverFired)
{
    const std::string quiet = describe_watchdog_disarm(/*already_fired=*/false);
    EXPECT_NE(quiet.find("sent nothing for this stop"), std::string::npos) << quiet;
    EXPECT_EQ(quiet.find("ALREADY FIRED"), std::string::npos) << quiet;

    const std::string fired = describe_watchdog_disarm(/*already_fired=*/true);
    EXPECT_NE(fired.find("had ALREADY FIRED before this stop"), std::string::npos) << fired;
    EXPECT_NE(fired.find("NOT recalled"), std::string::npos) << fired;
    EXPECT_EQ(fired.find("sent nothing for this stop"), std::string::npos) << fired;
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

// [review round 3] ...AND THE HARD TTL IS NOT THE BOUND THE OPERATOR GETS.
//
// The test above says what happens to an offer that is ALREADY expired. The
// operator note's claim was different and stronger -- "an offer older than the
// hard TTL when the engine comes back is cancelled on its first cycle anyway"
// -- and open PR #164 falsifies it: under strategy.ttl_cancel_mode: expire,
// execution::age_limit_cancel_applies(expire_mode, verified_max_time) returns
// false for any offer carrying a verified expiry, so the hard-TTL cancel is
// skipped and the offer rests until retire_expired_offers frees it, after its
// own on-chain expiry.
//
// That expiry is the LONGER of the two at every configuration anyone runs: the
// startup floor this repo already enforces (#150, expiry_outlasts_hard_ttl)
// demands it outlast kExpiryFloorMargin x the hard TTL measured at the
// CONFIGURED block spacing, which at the shipped 52 s is already 5.5x the hard
// TTL's real (peak-height) wall-clock length. So the operator note now states
// both bounds rather than promising the shorter one.
TEST(KeptBookAfterRestart, UnderExpireModeTheBoundIsTheExpiryAndItOutlastsTheHardTtl)
{
    constexpr std::uint32_t kHardMult = 2;  // OfferManager::kHardTtlMultiplier
    // The hard TTL is a COUNT OF BLOCK HEIGHTS, so its wall-clock length uses
    // the PEAK-HEIGHT cadence -- 4,608 blocks a day, 18.75 s -- and never
    // strategy.block_time_seconds, which is TRANSACTION-block spacing (52 s)
    // and is what the floor formula below uses for its own, different purpose.
    // Reading 800 blocks at 52 s gives 11.6 h instead of 4 h 10 min; reading
    // orphan_max_adopt_age_blocks: 120 that way gives 1.7 h instead of 37 min.
    constexpr double kPeakHeightSecsPerBlock = 86'400.0 / 4'608.0;  // 18.75
    // The live configuration, and three others the floor accepts.
    struct Case {
        std::uint32_t ttl_blocks;
        double        secs_per_block;   // strategy.block_time_seconds
        std::uint32_t expiry_secs;      // strategy.offer_expiry_secs
    };
    for (const Case c : {Case{400, 52.0, 86'400},   // LIVE: 24 h expiry
                         Case{400, 52.0, 172'800},
                         Case{200, 52.0, 86'400},
                         Case{600, 18.75, 86'400}}) {
        ASSERT_TRUE(xop::execution::expiry_outlasts_hard_ttl(
            c.expiry_secs, c.ttl_blocks, kHardMult, c.secs_per_block))
            << "the engine would refuse this configuration at startup";

        // What the operator is told: how long a kept offer can stay takeable
        // with no engine behind it. In `age` mode (today's default and every
        // build before #164) the next start cancels it once it is past the hard
        // TTL; in `expire` mode nothing does until its own max_time passes.
        //
        const double hard_ttl_wall_s =
            static_cast<double>(c.ttl_blocks) * kHardMult * kPeakHeightSecsPerBlock;
        EXPECT_GT(static_cast<double>(c.expiry_secs), hard_ttl_wall_s)
            << "ttl=" << c.ttl_blocks << ": in expire mode a kept offer would be "
            << "bounded by the hard TTL after all, and the operator note would hold";
    }

    // The live numbers, spelled out, because they are what the operator note
    // says: 400 blocks x 2 = 800 peak-height blocks = 4 h 10 min, against a
    // 24 h expiry. Nearly SIX times longer, not "cancelled on the first cycle".
    const double live_hard_ttl_wall_s = 800.0 * kPeakHeightSecsPerBlock;
    EXPECT_NEAR(live_hard_ttl_wall_s, 15'000.0, 1.0);   // 4 h 10 min
    EXPECT_GT(86'400.0 / live_hard_ttl_wall_s, 5.0);
}
