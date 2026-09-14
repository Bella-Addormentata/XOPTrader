#ifndef XOP_UTIL_SHUTDOWN_FLAG_HPP
#define XOP_UTIL_SHUTDOWN_FLAG_HPP
// ---------------------------------------------------------------------------
// shutdown_flag.hpp -- is a data/shutdown.flag stop request addressed to THIS
//                      engine process?
//
// [shutdown-flag-race 2026-09-12] WHAT WAS OBSERVED
// -------------------------------------------------
//   22:41:07.954  the closing GUI wrote shutdown.flag for engine PID 15916,
//                 which was stuck in wallet RPC retries        (gui.log:3759)
//   22:41:14.581  a newly launched GUI's singleton kill terminated PID 15916,
//                 6.6 s later and before it had read the flag  (gui.log:3760)
//   22:41:14.587  the closing GUI logged "Engine exited gracefully." 6 ms
//                 after that kill                              (gui.log:3761)
//   22:41:19.662  the new GUI started engine PID 11616          (gui.log:3773)
//   22:41:19.729  11616: "shutdown.flag is FRESH -- honouring it as a live
//                 close request" -- for a flag written 11.7 s before 11616
//                 existed
//   22:47:38.619  11616 consumed it at its first analysis poll, cancelled 1
//                 of 12 offers and exited at 22:49:11. No engine ran again
//                 until 23:24.
//
// The constructor's comment said a flag "YOUNGER than this process" was a
// live request; the code compared last_write_time against now() - 60 s, so
// ANY flag under a minute old stopped whichever engine read it. The flag had
// no address (the GUI wrote the literal "shutdown"), so nothing else could be
// checked.
//
// THE RULE
// --------
// A request is honoured only if it names this PID and was written at or
// after this process started. Content with no pid line -- the pre-fix GUI's
// "shutdown", an operator's hand-written flag -- is honoured only if it was
// written at or after this process started. There is no grace window.
//
//   * PID AND start time, because Windows recycles PIDs: the old GUI's Popen
//     handle pins its engine's PID only while that GUI is alive.
//   * No per-launch token. An engine started outside the GUI (a console, a
//     service manager, the successor left by the engine's own
//     kill_old_instances()) has no token, and an operator's editor cannot
//     know one; every writer can name a PID. PID plus start time covers all
//     of them with one rule and no CLI or version-skew handshake.
//   * The start instant is the kernel's process creation time on Windows
//     (process_identity.hpp), so a request written between the GUI's Popen
//     returning and main() running is not mistaken for a leftover.
//
// FORMAT v1 (written by gui/shutdown_flag.py -- ASCII, LF, atomic replace)
// ------------------------------------------------------------------------
//   xop-shutdown-request v1
//   pid=<target engine PID, decimal 1..4294967295>
//   requested_by_pid=<GUI PID>
//   written_at=<local ISO-8601, seconds>
//
// Only a line that STARTS at column 0 with "pid=" addresses the request.
// Every other line is ignored, so "requested_by_pid=" can never match.
//
// SCOPE, stated honestly: these functions are the DECISION. Engine::
// evaluate_shutdown_flag() reads the file, calls them and acts on the result;
// nothing in cpp/tests constructs an Engine (S36), so that wiring -- the
// constructor's boot sweep, the stop checkpoints, the site each call passes
// and the remove/shutdown() calls -- is not covered by a test.
// ---------------------------------------------------------------------------

#include "xop/util/process_identity.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace xop::util {

/// The engine reads at most this much of a flag. The GUI's request is ~90.
inline constexpr std::size_t kShutdownFlagMaxBytes = 4096;
/// PIDs are DWORDs on Windows; nothing larger can name a process.
inline constexpr std::uint64_t kShutdownFlagMaxPid = 0xFFFF'FFFFull;

// ===========================================================================
// Parsing
// ===========================================================================

enum class ShutdownFlagAddress : int {
    Unaddressed = 0,  ///< no pid line: pre-fix GUI content, a hand-written flag
    Addressed   = 1,  ///< exactly one usable pid line
    Malformed   = 2,  ///< a pid line that cannot name a process, or two of them
};

struct ParsedShutdownFlag {
    ShutdownFlagAddress address{ShutdownFlagAddress::Unaddressed};
    std::uint64_t pid{0};
};

/// Parse flag content. A leading UTF-8 BOM is dropped; lines split on '\n'
/// (a final segment without one is still a line) and lose one trailing '\r'.
[[nodiscard]] constexpr ParsedShutdownFlag parse_shutdown_flag(
    std::string_view content) noexcept
{
    constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF"};
    constexpr std::string_view kPidKey{"pid="};
    constexpr ParsedShutdownFlag kMalformed{ShutdownFlagAddress::Malformed, 0};

    if (content.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        content.remove_prefix(kUtf8Bom.size());
    }

    bool seen_pid = false;
    std::uint64_t pid = 0;
    while (!content.empty()) {
        const std::size_t newline = content.find('\n');
        std::string_view line = content.substr(0, newline);
        if (newline == std::string_view::npos) {
            content = std::string_view{};
        } else {
            content.remove_prefix(newline + 1);
        }
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // Column 0 only: "requested_by_pid=" must never address a request.
        if (line.substr(0, kPidKey.size()) != kPidKey) {
            continue;
        }
        std::string_view value = line.substr(kPidKey.size());

        if (seen_pid) {
            return kMalformed;  // two pid lines: which one would be meant?
        }
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }
        if (value.empty()) {
            return kMalformed;
        }
        std::uint64_t acc = 0;
        for (const char c : value) {
            if (c < '0' || c > '9') {
                return kMalformed;
            }
            acc = acc * 10U + static_cast<std::uint64_t>(c - '0');
            // Checked every digit, so acc never exceeds 10 * 2^32 + 9.
            if (acc > kShutdownFlagMaxPid) {
                return kMalformed;
            }
        }
        if (acc == 0) {
            return kMalformed;
        }
        seen_pid = true;
        pid = acc;  // keep scanning: a second pid line is still Malformed
    }

    if (seen_pid) {
        return ParsedShutdownFlag{ShutdownFlagAddress::Addressed, pid};
    }
    return ParsedShutdownFlag{};
}

// ===========================================================================
// The decision
// ===========================================================================

/// `Keep` is the zero value: a default verdict neither stops the engine nor
/// destroys a request someone may still mean.
enum class ShutdownFlagVerdict : int {
    Keep    = 0,  ///< leave the file, stop nothing; re-read at the next check
    Honour  = 1,  ///< a live request for this process
    Discard = 2,  ///< not for this process: remove it, stop nothing
};

enum class ShutdownFlagReason : int {
    Unknown = 0,
    ContentUnreadable,
    AgeUnknown,
    OwnPidUnknown,
    AddressedToThisProcess,
    UnaddressedWrittenAfterStart,
    AddressedToAnotherProcess,
    AddressedPidPredatesThisProcess,
    UnaddressedPredatesThisProcess,
    MalformedAddress,
};

[[nodiscard]] constexpr const char* shutdown_flag_verdict_name(
    ShutdownFlagVerdict verdict) noexcept
{
    switch (verdict) {
        case ShutdownFlagVerdict::Honour:  return "honour";
        case ShutdownFlagVerdict::Discard: return "discard";
        case ShutdownFlagVerdict::Keep:    break;
    }
    return "keep";
}

[[nodiscard]] constexpr const char* shutdown_flag_reason_name(
    ShutdownFlagReason reason) noexcept
{
    switch (reason) {
        case ShutdownFlagReason::ContentUnreadable:
            return "content unreadable";
        case ShutdownFlagReason::AgeUnknown:
            return "age unknown";
        case ShutdownFlagReason::OwnPidUnknown:
            return "own PID unknown";
        case ShutdownFlagReason::AddressedToThisProcess:
            return "addressed to this process";
        case ShutdownFlagReason::UnaddressedWrittenAfterStart:
            return "unaddressed, written after this process started";
        case ShutdownFlagReason::AddressedToAnotherProcess:
            return "addressed to another process";
        case ShutdownFlagReason::AddressedPidPredatesThisProcess:
            return "addressed to this PID but written before this process started";
        case ShutdownFlagReason::UnaddressedPredatesThisProcess:
            return "unaddressed, written before this process started";
        case ShutdownFlagReason::MalformedAddress:
            return "malformed pid line";
        case ShutdownFlagReason::Unknown:
            break;
    }
    return "unknown";
}

/// Everything the decision may use. The engine fills it from the file; a
/// test sets every field.
struct ShutdownFlagFacts {
    bool content_known{false};
    ParsedShutdownFlag parsed{};
    bool mtime_known{false};
    std::filesystem::file_time_type mtime{};
    ProcessIdentity identity{};
};

struct ShutdownFlagDecision {
    ShutdownFlagVerdict verdict{ShutdownFlagVerdict::Keep};
    ShutdownFlagReason reason{ShutdownFlagReason::Unknown};
};

/// THE RULE. The order is load-bearing: see each step.
[[nodiscard]] inline ShutdownFlagDecision decide_shutdown_flag(
    const ShutdownFlagFacts& f) noexcept
{
    using V = ShutdownFlagVerdict;
    using R = ShutdownFlagReason;

    // 1. Content that could not be read decides nothing -- it is not
    //    "unaddressed", it is unknown.
    if (!f.content_known) {
        return {V::Keep, R::ContentUnreadable};
    }
    // 2. A pid line that cannot name a process is nobody's request.
    if (f.parsed.address == ShutdownFlagAddress::Malformed) {
        return {V::Discard, R::MalformedAddress};
    }
    if (f.parsed.address == ShutdownFlagAddress::Addressed) {
        // 3a. Without our own PID, ours cannot be told from anyone else's.
        if (f.identity.pid == 0) {
            return {V::Keep, R::OwnPidUnknown};
        }
        // 3b. Another process's request, whenever it was written -- no clock
        //     is needed to know it is not ours.
        if (f.parsed.pid != f.identity.pid) {
            return {V::Discard, R::AddressedToAnotherProcess};
        }
        // 3c. Our PID -- but a recycled PID needs the clock to tell.
        if (!f.mtime_known) {
            return {V::Keep, R::AgeUnknown};
        }
        // 3d. Written before this process existed: a previous holder of the
        //     PID. The boundary is inclusive -- at the start instant it is ours.
        if (f.mtime < f.identity.start) {
            return {V::Discard, R::AddressedPidPredatesThisProcess};
        }
        // 3e.
        return {V::Honour, R::AddressedToThisProcess};
    }
    // 4a. Unaddressed: only the clock can say whose it is.
    if (!f.mtime_known) {
        return {V::Keep, R::AgeUnknown};
    }
    // 4b. Written before this process started: a leftover, never inherited.
    if (f.mtime < f.identity.start) {
        return {V::Discard, R::UnaddressedPredatesThisProcess};
    }
    // 4c.
    return {V::Honour, R::UnaddressedWrittenAfterStart};
}

// ===========================================================================
// What the caller does about it
// ===========================================================================

enum class ShutdownFlagSite : int {
    BootSweep  = 0,  ///< the Engine constructor: ioc_ is not running yet
    Checkpoint = 1,  ///< a stop checkpoint: fast poll, analysis poll, boot
};

struct ShutdownFlagAction {
    bool remove_file{false};
    bool request_shutdown{false};
};

[[nodiscard]] constexpr ShutdownFlagAction plan_shutdown_flag_action(
    ShutdownFlagVerdict verdict, ShutdownFlagSite site) noexcept
{
    switch (verdict) {
        case ShutdownFlagVerdict::Discard:
            return ShutdownFlagAction{true, false};
        case ShutdownFlagVerdict::Honour:
            if (site == ShutdownFlagSite::Checkpoint) {
                return ShutdownFlagAction{true, true};
            }
            // The boot sweep leaves a live request for the first checkpoint:
            // the constructor has never called shutdown(), and ioc_ is not
            // running to carry its continuation.
            return ShutdownFlagAction{};
        case ShutdownFlagVerdict::Keep:
            break;
    }
    return ShutdownFlagAction{};
}

/// Milliseconds from this process's start to the flag's mtime (negative when
/// the flag is older). 0 when the mtime is unknown. For log lines only.
[[nodiscard]] inline std::int64_t flag_age_vs_start_ms(
    const ShutdownFlagFacts& f) noexcept
{
    if (!f.mtime_known) {
        return 0;
    }
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            f.mtime - f.identity.start).count());
}

}  // namespace xop::util

#endif  // XOP_UTIL_SHUTDOWN_FLAG_HPP
