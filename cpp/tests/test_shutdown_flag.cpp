// ---------------------------------------------------------------------------
// [shutdown-flag-race 2026-09-12] Who a data/shutdown.flag stop request is for.
//
// A closing GUI wrote shutdown.flag for engine PID 15916. A newly launched GUI
// terminated that engine 6.6 s later, before it read the flag. The successor,
// PID 11616, honoured the leftover -- written 11.7 s before it existed --
// because the rule was "any flag younger than 60 s". It cancelled 1 of 12
// offers and exited, and no engine ran for 35 minutes.
//
// These tests drive the production decision in xop/util/shutdown_flag.hpp
// and the identity capture in xop/util/process_identity.hpp. They do NOT
// cover Engine::evaluate_shutdown_flag() -- the file read, the site each call
// passes, the remove and the shutdown() call -- because nothing in cpp/tests
// constructs an Engine (S36).
//
// kGolden is the v1 request the GUI writes. tests/test_shutdown_flag.py reads
// this file and fails unless the same bytes appear here, so the engine's and
// the GUI's copies of the format cannot drift apart silently.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/util/process_identity.hpp"
#include "xop/util/shutdown_flag.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

using xop::util::capture_process_identity;
using xop::util::decide_shutdown_flag;
using xop::util::flag_age_vs_start_ms;
using xop::util::parse_shutdown_flag;
using xop::util::plan_shutdown_flag_action;
using xop::util::process_start_source_name;
using xop::util::shutdown_flag_reason_name;
using xop::util::shutdown_flag_verdict_name;
using xop::util::ParsedShutdownFlag;
using xop::util::ProcessIdentity;
using xop::util::ProcessStartSource;
using xop::util::ShutdownFlagAction;
using xop::util::ShutdownFlagAddress;
using xop::util::ShutdownFlagDecision;
using xop::util::ShutdownFlagFacts;
using xop::util::ShutdownFlagReason;
using xop::util::ShutdownFlagSite;
using xop::util::ShutdownFlagVerdict;

namespace {

// GOLDEN: byte-identical to GOLDEN in tests/test_shutdown_flag.py.
constexpr std::string_view kGolden =
    "xop-shutdown-request v1\npid=15916\nrequested_by_pid=19084\nwritten_at=2026-09-12T22:41:07\n";

constexpr std::uint64_t kKilledEnginePid = 15916;  // the request was written for it
constexpr std::uint64_t kSuccessorPid    = 11616;  // the engine that inherited it

// 22:41:07.954 (flag written, gui.log:3759) -> 22:41:19.662 (successor
// started, gui.log:3773).
constexpr std::chrono::milliseconds kIncidentFlagAge{11'708};

// Built from a chrono duration, never a raw count: file_time_type ticks are
// 100 ns on MSVC and 1 ns on libstdc++.
const std::filesystem::file_time_type kStart{std::chrono::seconds{1'000'000}};

ProcessIdentity identity(std::uint64_t pid)
{
    ProcessIdentity id{};
    id.pid = pid;
    id.start = kStart;
    return id;
}

ParsedShutdownFlag unaddressed()
{
    return ParsedShutdownFlag{ShutdownFlagAddress::Unaddressed, 0};
}

ParsedShutdownFlag addressed(std::uint64_t pid)
{
    return ParsedShutdownFlag{ShutdownFlagAddress::Addressed, pid};
}

ParsedShutdownFlag malformed()
{
    return ParsedShutdownFlag{ShutdownFlagAddress::Malformed, 0};
}

// Every field set explicitly, every time.
ShutdownFlagFacts facts(bool content_known, ParsedShutdownFlag parsed,
                        bool mtime_known, std::filesystem::file_time_type mtime,
                        ProcessIdentity who)
{
    ShutdownFlagFacts f{};
    f.content_known = content_known;
    f.parsed        = parsed;
    f.mtime_known   = mtime_known;
    f.mtime         = mtime;
    f.identity      = who;
    return f;
}

std::uint64_t own_pid()
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::string request_for(std::uint64_t pid)
{
    return "xop-shutdown-request v1\npid=" + std::to_string(pid)
           + "\nrequested_by_pid=19084\nwritten_at=2026-09-13T00:00:00\n";
}

// A real file's mtime -- the clock the engine reads a real flag's age from.
struct WrittenFlag {
    bool ok{false};
    std::filesystem::file_time_type mtime{};
};

WrittenFlag write_temp_flag(const std::string& tag, const std::string& content)
{
    namespace fs = std::filesystem;
    WrittenFlag written{};
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec);
    if (ec) {
        return written;
    }
    const fs::path path =
        dir / ("xop_test_shutdown_flag_" + tag + "_" + std::to_string(own_pid()) + ".flag");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return written;
        }
        out << content;
        if (!out) {
            return written;
        }
    }
    const fs::file_time_type mtime = fs::last_write_time(path, ec);
    const bool mtime_ok = !ec;
    fs::remove(path, ec);
    if (mtime_ok) {
        written.ok = true;
        written.mtime = mtime;
    }
    return written;
}

// ===========================================================================
// ShutdownFlagParse
// ===========================================================================

TEST(ShutdownFlagParse, TheGuiRequestFormatParsesToItsTargetPid)
{
    const ParsedShutdownFlag parsed = parse_shutdown_flag(kGolden);
    EXPECT_EQ(parsed.address, ShutdownFlagAddress::Addressed);
    EXPECT_EQ(parsed.pid, kKilledEnginePid);
}

// "requested_by_pid=" CONTAINS "pid=". Only a line starting with it counts.
TEST(ShutdownFlagParse, ARequesterPidLineIsNeverMistakenForTheTarget)
{
    const ParsedShutdownFlag both =
        parse_shutdown_flag("requested_by_pid=19084\npid=15916\n");
    EXPECT_EQ(both.address, ShutdownFlagAddress::Addressed);
    EXPECT_EQ(both.pid, kKilledEnginePid);

    const ParsedShutdownFlag requester_only =
        parse_shutdown_flag("requested_by_pid=19084\n");
    EXPECT_EQ(requester_only.address, ShutdownFlagAddress::Unaddressed);
    EXPECT_EQ(requester_only.pid, std::uint64_t{0});
}

// "shutdown" is exactly what engine_bridge.py wrote before this fix.
TEST(ShutdownFlagParse, ThePreFixGuiContentIsUnaddressed)
{
    const std::array<std::string_view, 3> contents{{"shutdown", "", "stop\r\n"}};
    for (const std::string_view content : contents) {
        SCOPED_TRACE(std::string(content));
        const ParsedShutdownFlag parsed = parse_shutdown_flag(content);
        EXPECT_EQ(parsed.address, ShutdownFlagAddress::Unaddressed);
        EXPECT_EQ(parsed.pid, std::uint64_t{0});
    }
}

TEST(ShutdownFlagParse, WindowsLineEndingsBomAndPaddingAreTolerated)
{
    const std::array<std::string_view, 3> contents{{
        "\xEF\xBB\xBFpid=42\r\n", "pid= 42 \r\n", "pid=42"}};
    for (const std::string_view content : contents) {
        SCOPED_TRACE(std::string(content));
        const ParsedShutdownFlag parsed = parse_shutdown_flag(content);
        EXPECT_EQ(parsed.address, ShutdownFlagAddress::Addressed);
        EXPECT_EQ(parsed.pid, std::uint64_t{42});
    }
}

// Malformed, NOT unaddressed: an unaddressed flag written after start would
// be honoured, and a pid line that cannot name a process is nobody's request.
TEST(ShutdownFlagParse, AnUnusablePidLineIsMalformedNotUnaddressed)
{
    const std::array<std::string_view, 6> unusable{{
        "pid=\n", "pid=abc\n", "pid=0\n", "pid=12x\n", "pid=4294967296\n",
        "pid=184467440737095516160\n"}};
    for (const std::string_view content : unusable) {
        SCOPED_TRACE(std::string(content));
        const ParsedShutdownFlag parsed = parse_shutdown_flag(content);
        EXPECT_EQ(parsed.address, ShutdownFlagAddress::Malformed);
        EXPECT_EQ(parsed.pid, std::uint64_t{0});
    }

    const ParsedShutdownFlag largest = parse_shutdown_flag("pid=4294967295\n");
    EXPECT_EQ(largest.address, ShutdownFlagAddress::Addressed);
    EXPECT_EQ(largest.pid, std::uint64_t{4294967295U});
}

TEST(ShutdownFlagParse, TwoPidLinesAreMalformed)
{
    const std::array<std::string_view, 2> contents{{"pid=1\npid=2\n", "pid=7\npid=7\n"}};
    for (const std::string_view content : contents) {
        SCOPED_TRACE(std::string(content));
        const ParsedShutdownFlag parsed = parse_shutdown_flag(content);
        EXPECT_EQ(parsed.address, ShutdownFlagAddress::Malformed);
        EXPECT_EQ(parsed.pid, std::uint64_t{0});
    }
}

// ===========================================================================
// ShutdownFlagDecision
// ===========================================================================

// THE INCIDENT. The pre-fix GUI's "shutdown" was written 11.7 s before the
// successor started; the 60 s window honoured it and stopped the successor.
TEST(ShutdownFlagDecision, IncidentReplayPreFixFlagWrittenBeforeTheSuccessorStartedIsDiscarded)
{
    const ShutdownFlagDecision d = decide_shutdown_flag(facts(
        true, unaddressed(), true, kStart - kIncidentFlagAge, identity(kSuccessorPid)));
    EXPECT_EQ(d.verdict, ShutdownFlagVerdict::Discard) << shutdown_flag_verdict_name(d.verdict);
    EXPECT_EQ(d.reason, ShutdownFlagReason::UnaddressedPredatesThisProcess)
        << shutdown_flag_reason_name(d.reason);
}

// THE INCIDENT in the new format: the request names the engine it was written
// for, so the successor discards it whenever it was written.
TEST(ShutdownFlagDecision, IncidentReplayRequestAddressedToTheKilledEngineIsDiscardedBySuccessor)
{
    const std::array<std::filesystem::file_time_type, 2> mtimes{{
        kStart - kIncidentFlagAge, kStart + std::chrono::seconds{1}}};
    for (const std::filesystem::file_time_type& mtime : mtimes) {
        SCOPED_TRACE(flag_age_vs_start_ms(facts(
            true, addressed(kKilledEnginePid), true, mtime, identity(kSuccessorPid))));
        const ShutdownFlagDecision d = decide_shutdown_flag(facts(
            true, addressed(kKilledEnginePid), true, mtime, identity(kSuccessorPid)));
        EXPECT_EQ(d.verdict, ShutdownFlagVerdict::Discard) << shutdown_flag_verdict_name(d.verdict);
        EXPECT_EQ(d.reason, ShutdownFlagReason::AddressedToAnotherProcess)
            << shutdown_flag_reason_name(d.reason);
    }
}

TEST(ShutdownFlagDecision, ARequestAddressedToThisProcessIsHonoured)
{
    const ShutdownFlagDecision d = decide_shutdown_flag(facts(
        true, addressed(kSuccessorPid), true, kStart + std::chrono::seconds{5},
        identity(kSuccessorPid)));
    EXPECT_EQ(d.verdict, ShutdownFlagVerdict::Honour) << shutdown_flag_verdict_name(d.verdict);
    EXPECT_EQ(d.reason, ShutdownFlagReason::AddressedToThisProcess)
        << shutdown_flag_reason_name(d.reason);
}

// Windows recycles PIDs: a request for a previous holder of our PID is not ours.
TEST(ShutdownFlagDecision, AReusedPidDoesNotInheritItsPredecessorsRequest)
{
    const ShutdownFlagDecision d = decide_shutdown_flag(facts(
        true, addressed(kSuccessorPid), true, kStart - std::chrono::milliseconds{1},
        identity(kSuccessorPid)));
    EXPECT_EQ(d.verdict, ShutdownFlagVerdict::Discard) << shutdown_flag_verdict_name(d.verdict);
    EXPECT_EQ(d.reason, ShutdownFlagReason::AddressedPidPredatesThisProcess)
        << shutdown_flag_reason_name(d.reason);
}

// The operator's hand-written route (cancel_retry.hpp's stopper table) still works.
TEST(ShutdownFlagDecision, AHandWrittenFlagCreatedAfterStartIsHonoured)
{
    const ShutdownFlagDecision d = decide_shutdown_flag(facts(
        true, unaddressed(), true, kStart + std::chrono::seconds{60}, identity(kSuccessorPid)));
    EXPECT_EQ(d.verdict, ShutdownFlagVerdict::Honour) << shutdown_flag_verdict_name(d.verdict);
    EXPECT_EQ(d.reason, ShutdownFlagReason::UnaddressedWrittenAfterStart)
        << shutdown_flag_reason_name(d.reason);
}

TEST(ShutdownFlagDecision, WrittenAtTheExactStartInstantCountsAsThisProcess)
{
    const ShutdownFlagDecision loose = decide_shutdown_flag(facts(
        true, unaddressed(), true, kStart, identity(kSuccessorPid)));
    EXPECT_EQ(loose.verdict, ShutdownFlagVerdict::Honour) << shutdown_flag_verdict_name(loose.verdict);
    EXPECT_EQ(loose.reason, ShutdownFlagReason::UnaddressedWrittenAfterStart)
        << shutdown_flag_reason_name(loose.reason);

    const ShutdownFlagDecision ours = decide_shutdown_flag(facts(
        true, addressed(kSuccessorPid), true, kStart, identity(kSuccessorPid)));
    EXPECT_EQ(ours.verdict, ShutdownFlagVerdict::Honour) << shutdown_flag_verdict_name(ours.verdict);
    EXPECT_EQ(ours.reason, ShutdownFlagReason::AddressedToThisProcess)
        << shutdown_flag_reason_name(ours.reason);
}

// Unreadable is not "unaddressed": reading it that way would honour a fresh
// flag nobody could inspect, whoever it was written for.
TEST(ShutdownFlagDecision, UnreadableContentDecidesNothing)
{
    const ShutdownFlagDecision blank = decide_shutdown_flag(facts(
        false, ParsedShutdownFlag{}, true, kStart + std::chrono::seconds{5},
        identity(kSuccessorPid)));
    EXPECT_EQ(blank.verdict, ShutdownFlagVerdict::Keep) << shutdown_flag_verdict_name(blank.verdict);
    EXPECT_EQ(blank.reason, ShutdownFlagReason::ContentUnreadable)
        << shutdown_flag_reason_name(blank.reason);

    const ShutdownFlagDecision stale_parse = decide_shutdown_flag(facts(
        false, addressed(kKilledEnginePid), true, kStart + std::chrono::seconds{5},
        identity(kSuccessorPid)));
    EXPECT_EQ(stale_parse.verdict, ShutdownFlagVerdict::Keep)
        << shutdown_flag_verdict_name(stale_parse.verdict);
    EXPECT_EQ(stale_parse.reason, ShutdownFlagReason::ContentUnreadable)
        << shutdown_flag_reason_name(stale_parse.reason);
}

// The mtime passed here would be honoured if it were trusted.
TEST(ShutdownFlagDecision, WithoutAClockOnlyAPidMismatchCanDecide)
{
    const std::filesystem::file_time_type untrusted = kStart + std::chrono::seconds{5};

    const ShutdownFlagDecision loose = decide_shutdown_flag(facts(
        true, unaddressed(), false, untrusted, identity(kSuccessorPid)));
    EXPECT_EQ(loose.verdict, ShutdownFlagVerdict::Keep) << shutdown_flag_verdict_name(loose.verdict);
    EXPECT_EQ(loose.reason, ShutdownFlagReason::AgeUnknown) << shutdown_flag_reason_name(loose.reason);

    const ShutdownFlagDecision ours = decide_shutdown_flag(facts(
        true, addressed(kSuccessorPid), false, untrusted, identity(kSuccessorPid)));
    EXPECT_EQ(ours.verdict, ShutdownFlagVerdict::Keep) << shutdown_flag_verdict_name(ours.verdict);
    EXPECT_EQ(ours.reason, ShutdownFlagReason::AgeUnknown) << shutdown_flag_reason_name(ours.reason);

    const ShutdownFlagDecision theirs = decide_shutdown_flag(facts(
        true, addressed(kKilledEnginePid), false, untrusted, identity(kSuccessorPid)));
    EXPECT_EQ(theirs.verdict, ShutdownFlagVerdict::Discard) << shutdown_flag_verdict_name(theirs.verdict);
    EXPECT_EQ(theirs.reason, ShutdownFlagReason::AddressedToAnotherProcess)
        << shutdown_flag_reason_name(theirs.reason);
}

TEST(ShutdownFlagDecision, AMalformedAddressIsDiscardedEvenWhenFresh)
{
    const ShutdownFlagDecision d = decide_shutdown_flag(facts(
        true, malformed(), true, kStart + std::chrono::seconds{1}, identity(kSuccessorPid)));
    EXPECT_EQ(d.verdict, ShutdownFlagVerdict::Discard) << shutdown_flag_verdict_name(d.verdict);
    EXPECT_EQ(d.reason, ShutdownFlagReason::MalformedAddress) << shutdown_flag_reason_name(d.reason);
}

TEST(ShutdownFlagDecision, AnUnknownOwnPidNeverActsOnAnAddressedFlag)
{
    const ShutdownFlagDecision d = decide_shutdown_flag(facts(
        true, addressed(kKilledEnginePid), true, kStart + std::chrono::seconds{1}, identity(0)));
    EXPECT_EQ(d.verdict, ShutdownFlagVerdict::Keep) << shutdown_flag_verdict_name(d.verdict);
    EXPECT_EQ(d.reason, ShutdownFlagReason::OwnPidUnknown) << shutdown_flag_reason_name(d.reason);
}

TEST(ShutdownFlagDecision, DefaultsAuthoriseNothing)
{
    EXPECT_EQ(decide_shutdown_flag(ShutdownFlagFacts{}).verdict, ShutdownFlagVerdict::Keep);
    EXPECT_EQ(ShutdownFlagDecision{}.verdict, ShutdownFlagVerdict::Keep);

    const ShutdownFlagAction action =
        plan_shutdown_flag_action(ShutdownFlagVerdict{}, ShutdownFlagSite{});
    EXPECT_FALSE(action.remove_file);
    EXPECT_FALSE(action.request_shutdown);
}

TEST(ShutdownFlagDecision, AgeIsReportedRelativeToProcessStart)
{
    EXPECT_EQ(flag_age_vs_start_ms(facts(true, unaddressed(), true, kStart - kIncidentFlagAge,
                                         identity(kSuccessorPid))),
              std::int64_t{-11'708});
    EXPECT_EQ(flag_age_vs_start_ms(facts(true, unaddressed(), true, kStart + std::chrono::seconds{5},
                                         identity(kSuccessorPid))),
              std::int64_t{5'000});
    EXPECT_EQ(flag_age_vs_start_ms(facts(true, unaddressed(), false, kStart - kIncidentFlagAge,
                                         identity(kSuccessorPid))),
              std::int64_t{0});
}

// Operators grep the engine log for these; two reasons sharing a name would
// make the log lie about why a request was dropped.
TEST(ShutdownFlagDecision, EveryVerdictAndReasonHasItsOwnLogName)
{
    const std::set<std::string_view> verdicts{
        shutdown_flag_verdict_name(ShutdownFlagVerdict::Keep),
        shutdown_flag_verdict_name(ShutdownFlagVerdict::Honour),
        shutdown_flag_verdict_name(ShutdownFlagVerdict::Discard)};
    EXPECT_EQ(verdicts.size(), std::size_t{3});

    std::set<std::string_view> reasons;
    for (int r = static_cast<int>(ShutdownFlagReason::ContentUnreadable);
         r <= static_cast<int>(ShutdownFlagReason::MalformedAddress); ++r) {
        const std::string_view name =
            shutdown_flag_reason_name(static_cast<ShutdownFlagReason>(r));
        EXPECT_NE(name, std::string_view{"unknown"}) << "reason " << r;
        reasons.insert(name);
    }
    EXPECT_EQ(reasons.size(), std::size_t{9});
    EXPECT_EQ(std::string_view{shutdown_flag_reason_name(ShutdownFlagReason::Unknown)},
              std::string_view{"unknown"});
}

// ===========================================================================
// ShutdownFlagAction
// ===========================================================================

TEST(ShutdownFlagAction, OnlyACheckpointActsOnHonour)
{
    const ShutdownFlagAction at_checkpoint =
        plan_shutdown_flag_action(ShutdownFlagVerdict::Honour, ShutdownFlagSite::Checkpoint);
    EXPECT_TRUE(at_checkpoint.remove_file);
    EXPECT_TRUE(at_checkpoint.request_shutdown);

    const ShutdownFlagAction at_boot =
        plan_shutdown_flag_action(ShutdownFlagVerdict::Honour, ShutdownFlagSite::BootSweep);
    EXPECT_FALSE(at_boot.remove_file) << "a live request is left for the first checkpoint";
    EXPECT_FALSE(at_boot.request_shutdown)
        << "the constructor never calls shutdown(): ioc_ is not running yet";
}

TEST(ShutdownFlagAction, DiscardRemovesButNeverStopsAndKeepDoesNothing)
{
    const std::array<ShutdownFlagSite, 2> sites{{ShutdownFlagSite::BootSweep,
                                                 ShutdownFlagSite::Checkpoint}};
    for (const ShutdownFlagSite site : sites) {
        SCOPED_TRACE(static_cast<int>(site));
        const ShutdownFlagAction discard =
            plan_shutdown_flag_action(ShutdownFlagVerdict::Discard, site);
        EXPECT_TRUE(discard.remove_file);
        EXPECT_FALSE(discard.request_shutdown);

        const ShutdownFlagAction keep = plan_shutdown_flag_action(ShutdownFlagVerdict::Keep, site);
        EXPECT_FALSE(keep.remove_file);
        EXPECT_FALSE(keep.request_shutdown);
    }
}

// ===========================================================================
// ProcessIdentityCapture -- the real OS calls, against the real clocks
// ===========================================================================

TEST(ProcessIdentityCapture, IdentifiesThisProcess)
{
    const auto capture = capture_process_identity();
    EXPECT_EQ(capture.identity.pid, own_pid());

    const auto now = std::filesystem::file_time_type::clock::now();
    EXPECT_TRUE(capture.identity.start <= now) << "a process cannot start after it is observed";
    EXPECT_TRUE(now - capture.identity.start < std::chrono::hours{24})
        << "the start instant is not within a day of this test: wrong epoch or unit";
}

TEST(ProcessIdentityCapture, TheStartIsTheKernelCreationTimeOnWindows)
{
    const auto capture = capture_process_identity();
#ifdef _WIN32
    EXPECT_EQ(capture.source, ProcessStartSource::KernelCreationTime)
        << process_start_source_name(capture.source);
#else
    EXPECT_EQ(capture.source, ProcessStartSource::MainEntryClock)
        << process_start_source_name(capture.source);
#endif
}

// End to end through the real clocks: a request this process could only have
// been sent after it started is honoured.
TEST(ProcessIdentityCapture, ARequestWrittenAfterTheCaptureIsHonoured)
{
    const auto capture = capture_process_identity();
    // Longer than any filesystem timestamp tick, so a coarse mtime cannot
    // land before the capture.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const std::string content = request_for(capture.identity.pid);
    const WrittenFlag written = write_temp_flag("after_capture", content);
    ASSERT_TRUE(written.ok) << "could not write a temporary flag";

    const ShutdownFlagDecision d = decide_shutdown_flag(facts(
        true, parse_shutdown_flag(content), true, written.mtime, capture.identity));
    EXPECT_EQ(d.verdict, ShutdownFlagVerdict::Honour) << shutdown_flag_reason_name(d.reason);
    EXPECT_EQ(d.reason, ShutdownFlagReason::AddressedToThisProcess)
        << shutdown_flag_reason_name(d.reason);
}

// THE REVIEW'S WINDOW. The GUI can address a request to an engine's PID as
// soon as Popen returns -- before that engine's main() captures anything. A
// request written after this process was created but BEFORE the capture must
// still be this process's. Windows only: POSIX starts the clock at main()
// entry and documents this window as a limitation (process_identity.hpp).
TEST(ProcessIdentityCapture, ARequestWrittenBeforeTheCaptureButAfterCreationIsHonouredOnWindows)
{
#ifndef _WIN32
    GTEST_SKIP() << "POSIX starts the clock at main() entry -- see process_identity.hpp";
#else
    const std::string content = request_for(own_pid());
    const WrittenFlag written = write_temp_flag("before_capture", content);
    ASSERT_TRUE(written.ok) << "could not write a temporary flag";
    // Past any timestamp tick, so a clock read here is unambiguously later.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto capture = capture_process_identity();

    const ShutdownFlagDecision d = decide_shutdown_flag(facts(
        true, parse_shutdown_flag(content), true, written.mtime, capture.identity));
    EXPECT_EQ(d.verdict, ShutdownFlagVerdict::Honour) << shutdown_flag_reason_name(d.reason);
    EXPECT_EQ(d.reason, ShutdownFlagReason::AddressedToThisProcess)
        << shutdown_flag_reason_name(d.reason);
#endif
}

}  // namespace
