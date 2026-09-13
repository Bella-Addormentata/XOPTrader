#ifndef XOP_UTIL_PROCESS_IDENTITY_HPP
#define XOP_UTIL_PROCESS_IDENTITY_HPP
// ---------------------------------------------------------------------------
// process_identity.hpp -- which process is this, and since when?
//
// [shutdown-flag-race 2026-09-12] A stop request in data/shutdown.flag is
// honoured only when it names THIS process's PID (or no PID at all) and was
// written at or after THIS process started (see shutdown_flag.hpp). Both are
// captured here, once, as the first statement of main().
//
// WHY THE START IS THE KERNEL'S CREATION TIME ON WINDOWS
// -----------------------------------------------------
// The GUI knows an engine's PID as soon as Popen returns (gui.log 22:41:19.662
// on 2026-09-12); that engine's main() ran later (its first log line is
// stamped 22:41:19.702). A clock read inside main() therefore postdates a
// request the GUI could already have written. It is also the precise clock,
// while NTFS stamps a write from the coarser system time, so even a request
// written just AFTER main() began can carry an earlier mtime. Either way the
// request would be discarded as "written before this process existed", the
// GUI would wait out its 30 s and TerminateProcess the engine with no cancel.
//
// GetProcessTimes() reports the creation time the kernel recorded before
// CreateProcess returned to the GUI, so nothing the GUI can address to this
// PID predates it. MSVC's file_clock counts the same 100 ns FILETIME ticks
// from 1601 that last_write_time() reports, and capture_process_identity()
// verifies that assumption at run time instead of trusting it: a creation
// time in the future, or more than a day before main() began, falls back to
// the clock (creation_time_is_plausible, below).
//
// POSIX: the start is file_clock::now() at the first statement of main(). A
// request written between fork() and that statement -- or within one coarse
// filesystem-clock tick after it -- is discarded as predating the process.
// This is a known limitation, not a guarantee; the GUI's own stop is
// unaffected in practice because it cannot close within that window.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace xop::util {

/// Who this process is. `pid` 0 means UNKNOWN: decide_shutdown_flag() then
/// refuses to act on any addressed request (OwnPidUnknown).
struct ProcessIdentity {
    std::uint64_t pid{0};
    std::filesystem::file_time_type start{};
};

enum class ProcessStartSource : int {
    MainEntryClock     = 0,  ///< file_clock::now() when the capture ran.
    KernelCreationTime = 1,  ///< GetProcessTimes() creation time (Windows).
};

[[nodiscard]] constexpr const char* process_start_source_name(
    ProcessStartSource source) noexcept
{
    switch (source) {
        case ProcessStartSource::KernelCreationTime:
            return "kernel process creation time";
        case ProcessStartSource::MainEntryClock:
            break;
    }
    return "clock read at main() entry";
}

struct ProcessIdentityCapture {
    ProcessIdentity identity{};
    ProcessStartSource source{ProcessStartSource::MainEntryClock};
};

/// [review] The run-time representation check behind the Windows start
/// instant. A creation time is used only if it is not after `now` and no more
/// than kMaxPlausibleProcessAge before it; both bounds are inclusive. On a
/// toolchain whose file_clock does not share FILETIME's epoch and unit the
/// value lands centuries away and fails this, so the capture keeps the clock.
///
/// Written as `created >= now - kMaxPlausibleProcessAge`, not as
/// `now - created <= kMaxPlausibleProcessAge`: subtracting a value centuries
/// away could overflow a 1 ns file_clock, which spans about +-292 years.
inline constexpr std::chrono::hours kMaxPlausibleProcessAge{24};

[[nodiscard]] constexpr bool creation_time_is_plausible(
    std::filesystem::file_time_type created,
    std::filesystem::file_time_type now) noexcept
{
    return created <= now && created >= now - kMaxPlausibleProcessAge;
}

/// Capture this process's PID and start instant. Never throws.
///
/// Call it as the FIRST statement of main(): on POSIX, and on Windows when
/// the creation time cannot be used, the start instant is the clock at the
/// moment of this call. kill_old_instances() can block for more than 12 s
/// before the Engine exists, so capturing any later would discard stop
/// requests legitimately written during that wait.
[[nodiscard]] ProcessIdentityCapture capture_process_identity() noexcept;

}  // namespace xop::util

#endif  // XOP_UTIL_PROCESS_IDENTITY_HPP
