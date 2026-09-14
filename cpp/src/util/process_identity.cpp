// process_identity.cpp -- see xop/util/process_identity.hpp for why the start
// instant is the kernel's creation time on Windows and the main()-entry clock
// on POSIX.

#include "xop/util/process_identity.hpp"

#include <chrono>

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

namespace xop::util {

ProcessIdentityCapture capture_process_identity() noexcept
{
    ProcessIdentityCapture capture{};
    const std::filesystem::file_time_type now =
        std::filesystem::file_time_type::clock::now();
    capture.identity.start = now;
    capture.source = ProcessStartSource::MainEntryClock;

#ifdef _WIN32
    capture.identity.pid = static_cast<std::uint64_t>(GetCurrentProcessId());

    FILETIME creation{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time,
                        &kernel_time, &user_time) != 0) {
        const std::uint64_t ticks =
            (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U)
            | static_cast<std::uint64_t>(creation.dwLowDateTime);
        const std::filesystem::file_time_type created{
            std::filesystem::file_time_type::duration{
                static_cast<std::filesystem::file_time_type::rep>(ticks)}};
        // The representation check. MSVC's file_clock and a FILETIME share
        // epoch (1601-01-01) and unit (100 ns); a toolchain where they do not
        // lands centuries away and fails this test, keeping the clock above.
        if (creation_time_is_plausible(created, now)) {
            capture.identity.start = created;
            capture.source = ProcessStartSource::KernelCreationTime;
        }
    }
#else
    capture.identity.pid = static_cast<std::uint64_t>(getpid());
#endif

    return capture;
}

}  // namespace xop::util
