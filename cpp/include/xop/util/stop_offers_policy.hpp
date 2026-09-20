#ifndef XOP_UTIL_STOP_OFFERS_POLICY_HPP
#define XOP_UTIL_STOP_OFFERS_POLICY_HPP
// ---------------------------------------------------------------------------
// stop_offers_policy.hpp -- [S74 2026-09-20] what a STOP does with the book.
//
// WHY THIS EXISTS
// ---------------
// Engine::shutdown() had one behaviour: cancel the whole book (a wallet-wide
// sweep, then the S46 retry ladder, then the S31 fallback), write the cancel
// intent to uncancelled.txt and record the rows cancel_pending. There was no
// way to stop the engine and leave the offers resting, so whenever offers had
// to survive a restart the engine and the GUI were hard-killed instead
// (TerminateProcess), and the next boot's startup reconcile re-adopted the
// book. With Chia blocks ~97% full, a stop that cancels everything also leaves
// unconfirmed cancel spends and locked coins for the next start to clear.
//
// A stop now carries a POLICY:
//
//   cancel   the behaviour above, unchanged;
//   keep     send NO cancel of any kind, write NO cancel intent, leave every
//            offer_log row as it is, disarm the dead man's switch so it cannot
//            fire during the stop, and say in one line what was left resting.
//
// WHERE THE POLICY COMES FROM, in order:
//
//   1. the stop request itself -- an "offers=" line in data/shutdown.flag
//      (xop/util/shutdown_flag.hpp), written by the GUI after it asked the
//      operator;
//   2. otherwise the config default, engine.shutdown_offers. That covers every
//      stop where nobody was there to answer: a pre-policy GUI, a hand-written
//      flag, a console Ctrl+C, SIGTERM, a service stop, an OS session end.
//
// An "offers=" line this build cannot read (a typo, a value from a later
// build, two such lines) does NOT refuse the stop -- an operator who wants out
// must get out -- and does not guess: it falls back to the config default,
// which is the operator's own standing answer, and the engine says so.
//
// Pure: no I/O, no engine types, no clock. cpp/tests/test_stop_offers_policy
// drives every row of the table below, and the static_asserts at the end make
// GCC and MSVC evaluate it at build time.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <optional>
#include <string_view>

namespace xop::util {

/// Printed by `xop_trader --help` (main.cpp). An engine built before the
/// policy existed ignores an "offers=" line and CANCELS the book, which is the
/// one thing an operator who chose Keep must not get -- so the GUI writes
/// "offers=keep" only to an engine binary whose --help carries this token
/// (gui/stop_offers.py ENGINE_HELP_TOKEN, same bytes; tests/test_stop_offers.py
/// holds the two together). --help returns before main() touches anything.
inline constexpr std::string_view kStopPolicyHelpToken =
    "shutdown.flag offers=cancel|keep";

/// A policy that can be ACTED on: the config default, and what a readable
/// request resolves to. `Cancel` is the zero value -- a default-constructed
/// policy does what the engine did before this header existed.
enum class StopOffersPolicy : int {
    Cancel = 0,
    Keep   = 1,
};

/// What a stop REQUEST said about the book.
enum class StopOffersRequest : int {
    Unspecified  = 0,  ///< no offers line: pre-policy GUI, hand-written flag, a signal
    Cancel       = 1,
    Keep         = 2,
    Unrecognised = 3,  ///< an offers line this build cannot read, or two of them
};

[[nodiscard]] constexpr const char* stop_offers_policy_name(
    StopOffersPolicy policy) noexcept
{
    return policy == StopOffersPolicy::Keep ? "keep" : "cancel";
}

[[nodiscard]] constexpr const char* stop_offers_request_name(
    StopOffersRequest request) noexcept
{
    switch (request) {
        case StopOffersRequest::Cancel:       return "cancel";
        case StopOffersRequest::Keep:         return "keep";
        case StopOffersRequest::Unrecognised: return "unrecognised";
        case StopOffersRequest::Unspecified:  break;
    }
    return "unspecified";
}

namespace detail {

[[nodiscard]] constexpr char ascii_lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool ascii_iequals(std::string_view a,
                                           std::string_view lower) noexcept
{
    if (a.size() != lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != lower[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

/// "cancel" or "keep": surrounding spaces and tabs are dropped and ASCII case
/// is ignored, so a hand-written "offers= Keep" reads. Anything else --
/// empty, "yes", "keep-bids", "keep;cancel" -- is nullopt, never a guess.
///
/// ONE parser for both spellings of the policy: the config key
/// engine.shutdown_offers and the shutdown.flag "offers=" line. The GUI's
/// gui/stop_offers.py parse_policy mirrors it, and tests/test_stop_offers.py
/// holds the two to the same table.
[[nodiscard]] constexpr std::optional<StopOffersPolicy> parse_stop_offers_policy(
    std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (detail::ascii_iequals(text, "cancel")) {
        return StopOffersPolicy::Cancel;
    }
    if (detail::ascii_iequals(text, "keep")) {
        return StopOffersPolicy::Keep;
    }
    return std::nullopt;
}

/// The value of one "offers=" line, as a request.
[[nodiscard]] constexpr StopOffersRequest stop_offers_request_from_value(
    std::string_view value) noexcept
{
    const std::optional<StopOffersPolicy> policy = parse_stop_offers_policy(value);
    if (!policy.has_value()) {
        return StopOffersRequest::Unrecognised;
    }
    return *policy == StopOffersPolicy::Keep ? StopOffersRequest::Keep
                                             : StopOffersRequest::Cancel;
}

// ===========================================================================
// The decision table
// ===========================================================================

/// What shutdown() does with the book. `CancelBook` is the zero value.
enum class StopBookAction : int {
    CancelBook    = 0,  ///< sweep, ladder, intent file, cancel_pending rows
    KeepBook      = 1,  ///< no cancel, no intent, rows untouched, switch disarmed
    DryRunNothing = 2,  ///< a dry run owns no offers: nothing to cancel or keep
};

/// Why that action, for the one log line that announces it.
enum class StopPolicySource : int {
    Request                  = 0,  ///< the stop request named the policy
    ConfigDefault            = 1,  ///< the request named none
    ConfigDefaultUnreadable  = 2,  ///< the request named one this build cannot read
    DryRun                   = 3,  ///< dry run: neither applies
};

struct StopOffersPlan {
    StopBookAction   action{StopBookAction::CancelBook};
    StopPolicySource source{StopPolicySource::ConfigDefault};
};

[[nodiscard]] constexpr const char* stop_book_action_name(
    StopBookAction action) noexcept
{
    switch (action) {
        case StopBookAction::KeepBook:      return "KEEP the resting offers";
        case StopBookAction::DryRunNothing: return "nothing (dry run owns no offers)";
        case StopBookAction::CancelBook:    break;
    }
    return "CANCEL the resting offers";
}

[[nodiscard]] constexpr const char* stop_policy_source_name(
    StopPolicySource source) noexcept
{
    switch (source) {
        case StopPolicySource::Request:
            return "named by the stop request";
        case StopPolicySource::ConfigDefaultUnreadable:
            return "the stop request's offers line could not be read, so the "
                   "config default engine.shutdown_offers applies";
        case StopPolicySource::DryRun:
            return "dry run";
        case StopPolicySource::ConfigDefault:
            break;
    }
    return "the stop request named no policy, so the config default "
           "engine.shutdown_offers applies";
}

/// THE TABLE (request x config default x dry run):
///
///   dry run   request        default   ->  action          source
///   -------   ------------   -------       -------------   -----------------
///   yes       (any)          (any)         DryRunNothing   DryRun
///   no        Cancel         (any)         CancelBook      Request
///   no        Keep           (any)         KeepBook        Request
///   no        Unspecified    cancel        CancelBook      ConfigDefault
///   no        Unspecified    keep          KeepBook        ConfigDefault
///   no        Unrecognised   cancel        CancelBook      ConfigDefaultUnreadable
///   no        Unrecognised   keep          KeepBook        ConfigDefaultUnreadable
///
/// Dry run is decided FIRST: a dry run posts nothing, reconciles nothing and
/// owns no part of the live book (engine.cpp start_watchdog), so it must never
/// reach a cancel whatever the request says.
[[nodiscard]] constexpr StopOffersPlan plan_stop_offers(
    StopOffersRequest requested,
    StopOffersPolicy  config_default,
    bool              dry_run) noexcept
{
    if (dry_run) {
        return StopOffersPlan{StopBookAction::DryRunNothing, StopPolicySource::DryRun};
    }
    const auto action_for = [](StopOffersPolicy policy) constexpr noexcept {
        return policy == StopOffersPolicy::Keep ? StopBookAction::KeepBook
                                                : StopBookAction::CancelBook;
    };
    switch (requested) {
        case StopOffersRequest::Cancel:
            return StopOffersPlan{StopBookAction::CancelBook, StopPolicySource::Request};
        case StopOffersRequest::Keep:
            return StopOffersPlan{StopBookAction::KeepBook, StopPolicySource::Request};
        case StopOffersRequest::Unrecognised:
            return StopOffersPlan{action_for(config_default),
                                  StopPolicySource::ConfigDefaultUnreadable};
        case StopOffersRequest::Unspecified:
            break;
    }
    return StopOffersPlan{action_for(config_default), StopPolicySource::ConfigDefault};
}

// ===========================================================================
// [review #165] A keep stop lets an in-flight post LAND before it stops
// ===========================================================================
//
// shutdown.flag is read between heartbeat cycles, so a GUI stop never lands
// inside one. A SIGNAL (Ctrl+C, SIGTERM) can: it may arrive while Step 8 is
// suspended in post_quotes, awaiting create_offer or the Dexie submission. The
// keep continuation does nothing that takes time, so it used to reach
// ioc_.stop() at once -- the wallet could then complete the create with nobody
// left to hear the answer: a resting offer with no State entry and no offer_log
// row. The next boot files such an offer as an ORPHAN, and evaluate_orphan
// CANCELS an orphan that is older than orphan_max_adopt_age_blocks, adversely
// priced, or on a pair with no mid price (offer_manager.cpp) -- the one outcome
// a keep stop exists to avoid.
//
// So the keep continuation waits while a post is in flight, and only that
// long: post_quotes records every offer it creates in State before it returns,
// and Step 8 manages nothing further once it sees the keep latch. The wait is
// BOUNDED. A wallet that never answers must not turn "stop" into "hang", and
// past the budget the stop proceeds and says an offer may have been left
// unrecorded.

/// How long a keep stop waits for an in-flight post. One create_offer is
/// bounded by the wallet client's request timeout (30 s); a pair's post is a
/// few of them plus the Dexie submissions.
inline constexpr unsigned long long kKeepStopDrainBudgetMs = 60'000;
/// How often the wait looks again. The cycle it is waiting on runs on the
/// same thread, so this is also how long that cycle can run on past its post.
inline constexpr unsigned long long kKeepStopDrainPollMs = 50;

enum class KeepStopDrainStep : int {
    Proceed = 0,  ///< nothing is being posted: report and stop
    Wait    = 1,  ///< a post is in flight and the budget is not spent
    GiveUp  = 2,  ///< a post is STILL in flight at the budget: stop, and say so
};

[[nodiscard]] constexpr KeepStopDrainStep keep_stop_drain_step(
    bool               posting_in_flight,
    unsigned long long waited_ms,
    unsigned long long budget_ms) noexcept
{
    if (!posting_in_flight) {
        return KeepStopDrainStep::Proceed;
    }
    return waited_ms < budget_ms ? KeepStopDrainStep::Wait : KeepStopDrainStep::GiveUp;
}

// ---------------------------------------------------------------------------
// Build-time pins. GCC and MSVC both evaluate these, so a wrong row is a
// compile error on every platform rather than a red runner on one.
// ---------------------------------------------------------------------------
static_assert(keep_stop_drain_step(false, 0, kKeepStopDrainBudgetMs) == KeepStopDrainStep::Proceed);
static_assert(keep_stop_drain_step(true, 0, kKeepStopDrainBudgetMs) == KeepStopDrainStep::Wait);
static_assert(keep_stop_drain_step(true, kKeepStopDrainBudgetMs, kKeepStopDrainBudgetMs)
              == KeepStopDrainStep::GiveUp);
static_assert(plan_stop_offers(StopOffersRequest::Keep, StopOffersPolicy::Cancel, false).action
              == StopBookAction::KeepBook);
static_assert(plan_stop_offers(StopOffersRequest::Cancel, StopOffersPolicy::Keep, false).action
              == StopBookAction::CancelBook);
static_assert(plan_stop_offers(StopOffersRequest::Unspecified, StopOffersPolicy::Cancel, false).action
              == StopBookAction::CancelBook);
static_assert(plan_stop_offers(StopOffersRequest::Unspecified, StopOffersPolicy::Keep, false).action
              == StopBookAction::KeepBook);
static_assert(plan_stop_offers(StopOffersRequest::Unrecognised, StopOffersPolicy::Cancel, false).action
              == StopBookAction::CancelBook);
static_assert(plan_stop_offers(StopOffersRequest::Keep, StopOffersPolicy::Keep, true).action
              == StopBookAction::DryRunNothing);
static_assert(StopOffersPlan{}.action == StopBookAction::CancelBook);
static_assert(parse_stop_offers_policy(" Keep\t").has_value());
static_assert(!parse_stop_offers_policy("keep-bids").has_value());

}  // namespace xop::util

#endif  // XOP_UTIL_STOP_OFFERS_POLICY_HPP
