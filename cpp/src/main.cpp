// =============================================================================
// main.cpp -- Entry point for XOPTrader CHIA DEX market-making engine.
// =============================================================================
//
// Initialization order (strict -- later steps depend on earlier ones):
//   1. Initialize libcurl globally  (curl_global_init, once per process)
//   2. Parse CLI arguments           (--config, --dry-run, --verbose)
//   3. Initialize structured logging (spdlog -- must precede any log calls)
//   4. Load and validate YAML config via xop::load_config()
//   5. Construct xop::Engine(config, dry_run) -- owns io_context, all subsystems
//   6. Install signal handlers       (SIGINT, SIGTERM via std::signal)
//   7. Call engine.run()             (blocks until shutdown completes)
//
// Shutdown sequence (on signal or unrecoverable error):
//   1. Signal handler fires -> engine->shutdown()
//   2. Engine cancels all outstanding CHIA offers via Wallet RPC
//   3. Engine stops its internal io_context, run() returns
//   4. curl_global_cleanup(), spdlog::shutdown(), process exits
//
// A second signal (double Ctrl-C) calls std::_Exit() to force-terminate
// immediately, bypassing the graceful offer-cancellation path.  This is the
// escape hatch if the wallet RPC is unreachable during shutdown.
//
// Security (ISO/IEC 27001:2022):
//   - No hardcoded secrets.  SSL cert paths and tokens are loaded from YAML.
//   - Secret fields (certs, fingerprint, Telegram token) are never logged.
//
// Secure coding (ISO/IEC 5055):
//   - Stack protector and control-flow guard enabled via CMake.
//   - No raw owning pointers; RAII throughout.
//   - All external input (CLI, YAML) validated before use.
//   - curl_global_init called exactly once at process start (not per-client).
// =============================================================================

#include "xop/engine.hpp"
#include "xop/config.hpp"
#include "xop/version.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <tlhelp32.h>
#else
#  include <dirent.h>
#  include <signal.h>
#  include <unistd.h>
#endif
// [F4] Both platforms now write data/shutdown.flag in kill_old_instances(),
// so this can no longer live inside the POSIX branch.
#include <fstream>

// Third-party
#include <sqlite3.h>

#include <boost/program_options.hpp>
#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace po = boost::program_options;
namespace fs = std::filesystem;

// =============================================================================
// Singleton enforcement: kill old xop_trader instances on startup.
//
// When deploying a new binary, the old process may still be running and
// holding the Prometheus port, wallet RPC connections, and pending offers.
// This function finds all other processes named "xop_trader" (or
// "xop_trader.exe" on Windows), terminates them, and waits for cleanup
// so the new instance can bind its ports cleanly.
//
// [F4 2026-09-03] GRACEFUL FIRST, ON BOTH PLATFORMS.
//
// The two branches were asymmetric and the asymmetry was expensive. POSIX
// sent SIGTERM, waited up to 10 s, and only then SIGKILLed -- so the old
// engine ran its own shutdown(), cancelled its book, and wrote its cancel
// intent. WINDOWS went straight to TerminateProcess with no graceful rung at
// all. Windows is the production platform, so the NORMAL REDEPLOY was an
// uncovered hard kill: the old process died before the replacement Engine was
// even constructed, no intent marker was ever written for its live book, and
// startup reconciliation classified those rows as still_live and restored
// them as ordinary quotes. The S46 PR described its durable recovery as
// covering this route. It did not, and could not.
//
// The mechanism to fix it already existed and was already wired:
// data/shutdown.flag, written by the GUI's graceful-close button and consumed
// by Engine::check_shutdown_flag(), which invokes the same full shutdown().
// This writes that flag and waits, exactly mirroring the POSIX ladder, before
// falling through to the hard kill that always followed.
//
// Two properties worth stating, because they bound what the wait has to buy:
//
//   * The wait does NOT need to cover a full cancel ladder.
//     Engine::shutdown() writes the intent marker EARLY -- before the retry
//     loop, before the sync probe -- so an old process that is hard-killed at
//     the end of this window has still recorded which offers it ordered dead.
//     The marker is the durable half; the cancels are best-effort.
//   * The flag is removed again before this function returns, unconditionally.
//     A leftover fresh flag is not inert: the Engine constructor honours a
//     flag younger than 60 s as a live close request, so failing to clean up
//     would make the REPLACEMENT process immediately shut itself down.
// =============================================================================

/// How long to let an old instance shut itself down before terminating it.
/// Matches the GUI's stop window and the POSIX branch's existing patience;
/// see above for why this need not cover the whole 90 s cancel budget.
static constexpr int kGracefulExitWaitMs = 30'000;

#ifdef _WIN32

static void kill_old_instances(const std::filesystem::path& shutdown_flag) {
    const DWORD current_pid = GetCurrentProcessId();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        spdlog::warn("[Startup] Failed to create process snapshot (err={})",
                     GetLastError());
        return;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    std::vector<DWORD> old_pids;

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID != current_pid &&
                _wcsicmp(pe.szExeFile, L"xop_trader.exe") == 0) {
                old_pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);

    if (old_pids.empty()) {
        spdlog::info("[Startup] No old xop_trader instances found");
        return;
    }

    // ---- Graceful rung: ask, then wait, then kill -------------------------
    // [F4] Written BEFORE any handle is opened so every old instance sees it
    // at once, and cleaned up in the block below whatever happens afterwards.
    bool flag_written = false;
    if (!shutdown_flag.empty()) {
        std::error_code fec;
        std::filesystem::create_directories(shutdown_flag.parent_path(), fec);
        std::ofstream flag(shutdown_flag, std::ios::trunc);
        if (flag) {
            flag << "kill_old_instances\n";
            flag.close();
            flag_written = flag.good();
        }
        if (flag_written) {
            spdlog::info("[Startup] [F4] wrote {} -- giving {} old instance(s) "
                         "up to {} ms to cancel their book and record their "
                         "cancel intent before terminating",
                         shutdown_flag.string(), old_pids.size(),
                         kGracefulExitWaitMs);

            // Poll rather than one long wait: the common case is a clean exit
            // in a few seconds and there is no reason to make every redeploy
            // pay the full window.
            const auto give_up = std::chrono::steady_clock::now()
                               + std::chrono::milliseconds(kGracefulExitWaitMs);
            while (std::chrono::steady_clock::now() < give_up) {
                bool any_alive = false;
                for (const DWORD pid : old_pids) {
                    HANDLE probe = OpenProcess(SYNCHRONIZE, FALSE, pid);
                    if (probe == nullptr) {
                        // [F6] "OpenProcess failed" is NOT "the process
                        // exited". ERROR_ACCESS_DENIED means the process is
                        // very much alive and we simply may not touch it --
                        // an old xop_trader running elevated or as another
                        // user while the replacement is not. Reading that as
                        // "gone" broke the loop immediately, logged "exited
                        // gracefully", and let two engines quote the same
                        // book, which is the exact outcome this function
                        // exists to prevent. Only ERROR_INVALID_PARAMETER
                        // (no such pid) is evidence of exit.
                        const DWORD probe_err = GetLastError();
                        if (probe_err == ERROR_INVALID_PARAMETER) {
                            continue;  // genuinely gone
                        }
                        any_alive = true;
                        spdlog::warn("[Startup] [F6] cannot open PID {} "
                                     "(err={}) -- treating it as ALIVE, not "
                                     "as exited. If this is ACCESS_DENIED "
                                     "the old instance is running at a "
                                     "privilege this process cannot signal "
                                     "and will NOT be killed below.",
                                     pid, probe_err);
                        break;
                    }
                    if (WaitForSingleObject(probe, 0) == WAIT_TIMEOUT) {
                        any_alive = true;
                    }
                    CloseHandle(probe);
                    if (any_alive) break;
                }
                if (!any_alive) {
                    spdlog::info("[Startup] [F4] old instance(s) exited "
                                 "gracefully -- no hard kill needed");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        } else {
            // Not fatal, but it means this redeploy is back to being a hard
            // kill, and the operator should know which kind of event they are
            // looking at when the next boot reports restored offers.
            spdlog::error("[Startup] [F4] could NOT write {} -- falling back "
                          "to an immediate hard kill. The old instance will "
                          "not run its shutdown path, so it will record no "
                          "cancel intent for whatever book it is holding.",
                          shutdown_flag.string());
        }
    }

    // Remove it unconditionally, INCLUDING on every early path below: the
    // Engine constructor treats a flag younger than 60 s as a live close
    // request, so leaving ours behind would shut down the process we are
    // starting.
    struct FlagCleanup {
        const std::filesystem::path& path;
        bool active;
        ~FlagCleanup() {
            if (!active) return;
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } flag_cleanup{shutdown_flag, flag_written};

    for (const DWORD pid : old_pids) {
        HANDLE proc = OpenProcess(
            PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (proc == nullptr) {
            // Already gone is the EXPECTED outcome now that the graceful rung
            // runs first, so ERROR_INVALID_PARAMETER is an info line.
            //
            // [F6] Anything else is not. This used to downgrade every open
            // failure to info, including ACCESS_DENIED -- a live process we
            // cannot terminate -- so the single-instance guarantee could fail
            // with no warning-level trace anywhere in the log.
            const DWORD open_err = GetLastError();
            if (open_err == ERROR_INVALID_PARAMETER) {
                spdlog::info("[Startup] PID {} is no longer open -- already "
                             "exited", pid);
            } else {
                spdlog::error("[Startup] [F6] cannot open PID {} for "
                              "termination (err={}) -- it may still be "
                              "RUNNING and holding the Prometheus port, "
                              "wallet RPC handles and a live book. TWO "
                              "ENGINES MAY NOW BE QUOTING. Stop it by hand.",
                              pid, open_err);
            }
            continue;
        }
        if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
            spdlog::info("[Startup] PID {} already exited -- no kill needed",
                         pid);
            CloseHandle(proc);
            continue;
        }
        spdlog::warn("[Startup] PID {} did not exit gracefully within {} ms "
                     "-- TERMINATING. Its cancel intent marker was written "
                     "early in shutdown() and should be on disk, but its "
                     "book may not be fully cancelled.",
                     pid, kGracefulExitWaitMs);
        if (TerminateProcess(proc, 1)) {
            // Wait up to 10 s for the process to exit and release its
            // resources (Prometheus port, wallet RPC handles, etc.).
            WaitForSingleObject(proc, 10000);
        } else {
            spdlog::warn("[Startup] TerminateProcess failed for PID {} "
                         "(err={})", pid, GetLastError());
        }
        CloseHandle(proc);
    }

    spdlog::info("[Startup] Cleared {} old instance(s) -- waiting for "
                 "port release", old_pids.size());
    // Brief pause for the OS to fully release bound sockets.
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

#else  // POSIX (Linux / macOS)

static void kill_old_instances(const std::filesystem::path& shutdown_flag) {
    const pid_t current_pid = getpid();
    std::vector<pid_t> old_pids;

    DIR* proc_dir = opendir("/proc");
    if (proc_dir == nullptr) {
        spdlog::warn("[Startup] Cannot open /proc -- skipping old instance "
                     "check");
        return;
    }

    while (struct dirent* entry = readdir(proc_dir)) {
        // Only numeric directory names are PIDs.
        char* endptr = nullptr;
        const long pid_long = std::strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid_long <= 0) continue;
        const pid_t pid = static_cast<pid_t>(pid_long);
        if (pid == current_pid) continue;

        // Read /proc/<pid>/comm to get the process name.
        const std::string comm_path =
            "/proc/" + std::string(entry->d_name) + "/comm";
        std::ifstream comm_file(comm_path);
        if (!comm_file.is_open()) continue;

        std::string name;
        std::getline(comm_file, name);
        if (name == "xop_trader") {
            old_pids.push_back(pid);
        }
    }
    closedir(proc_dir);

    if (old_pids.empty()) {
        spdlog::info("[Startup] No old xop_trader instances found");
        return;
    }

    // [F4] The flag as well as the signal. SIGTERM already reaches
    // signal_handler -> engine->shutdown() here, so this branch was never the
    // broken one -- but writing the same flag on both platforms means there
    // is ONE graceful-stop channel to reason about, and it still works if a
    // supervisor has the signal blocked or the handler has not been installed
    // yet (there is a window during startup where it has not).
    bool flag_written = false;
    if (!shutdown_flag.empty()) {
        std::error_code fec;
        std::filesystem::create_directories(shutdown_flag.parent_path(), fec);
        std::ofstream flag(shutdown_flag, std::ios::trunc);
        if (flag) {
            flag << "kill_old_instances\n";
            flag.close();
            flag_written = flag.good();
        }
        if (!flag_written) {
            spdlog::warn("[Startup] [F4] could not write {} -- relying on "
                         "SIGTERM alone", shutdown_flag.string());
        }
    }
    struct FlagCleanup {
        const std::filesystem::path& path;
        bool active;
        ~FlagCleanup() {
            if (!active) return;
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } flag_cleanup{shutdown_flag, flag_written};

    for (const pid_t pid : old_pids) {
        spdlog::info("[Startup] Sending SIGTERM to old xop_trader "
                     "(PID {})", pid);
        kill(pid, SIGTERM);
    }

    // [F4] Wait the same window as the Windows branch. This was 10 s while
    // Windows waited 0; the two platforms disagreeing about how much patience
    // a graceful stop deserves is what let the Windows redeploy become an
    // uncovered hard kill without anyone noticing.
    const auto give_up = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(kGracefulExitWaitMs);
    while (std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        bool any_alive = false;
        for (const pid_t pid : old_pids) {
            if (kill(pid, 0) == 0) { any_alive = true; break; }
        }
        if (!any_alive) break;
    }

    // Force-kill survivors.
    for (const pid_t pid : old_pids) {
        if (kill(pid, 0) == 0) {
            spdlog::warn("[Startup] PID {} did not exit gracefully within {} "
                         "ms -- sending SIGKILL. Its cancel intent marker was "
                         "written early in shutdown() and should be on disk, "
                         "but its book may not be fully cancelled.",
                         pid, kGracefulExitWaitMs);
            kill(pid, SIGKILL);
        }
    }

    spdlog::info("[Startup] Cleared {} old instance(s) -- waiting for "
                 "port release", old_pids.size());
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

#endif  // _WIN32

// =============================================================================
// Global engine pointer for signal handler access.
//
// std::signal handlers must have C linkage and access only signal-safe state.
// We store a raw pointer to the Engine (which outlives the signal handler
// registration) and an atomic counter to implement two-phase shutdown:
//   First signal  -> engine->shutdown() (graceful)
//   Second signal -> std::_Exit()       (force-kill escape hatch)
// =============================================================================

/// Global engine pointer set before entering the main loop.
/// The Engine's shutdown() method is safe to call from a signal context
/// because it only sets an atomic flag and posts work to the io_context.
static std::atomic<xop::Engine*> g_engine_ptr{nullptr};

/// Count of signals received.  Second signal triggers immediate exit.
static std::atomic<int> g_signal_count{0};

/// Signal handler for SIGINT / SIGTERM.  Async-signal-safe: only touches
/// atomics and calls std::_Exit (which is async-signal-safe per POSIX).
static void signal_handler(int signum) {
    const int count = g_signal_count.fetch_add(1, std::memory_order_relaxed) + 1;

    if (count >= 2) {
        // Second signal: force-kill.  Wallet RPC may be unreachable;
        // this is the escape hatch documented in the header comment.
        std::_Exit(EXIT_FAILURE);
    }

    // First signal: request graceful shutdown via the Engine.
    xop::Engine* engine = g_engine_ptr.load(std::memory_order_acquire);
    if (engine != nullptr) {
        engine->shutdown();
    }

    // Suppress unused-parameter warning for signal number.
    (void)signum;
}

// =============================================================================
// CLI argument parsing
// =============================================================================

/// Parsed command-line arguments.
struct CliArgs {
    std::string config_path;   ///< Path to YAML configuration file.
    std::string secrets_path;  ///< Path to secrets YAML (optional overlay).
    bool        dry_run;       ///< Compute quotes without submitting offers.
    bool        verbose;       ///< Enable DEBUG-level logging.
};

/// Parse argc/argv.  Returns std::nullopt if --help was requested.
[[nodiscard]]
static std::optional<CliArgs> parse_cli(int argc, char* argv[]) {
    po::options_description desc("XOPTrader -- CHIA DEX Market-Making Engine");
    desc.add_options()
        ("help,h",    "Show this help message and exit")
        ("config,c",  po::value<std::string>()->default_value("config.yaml"),
                       "Path to YAML configuration file")
        ("secrets,s", po::value<std::string>()->default_value("secrets.yaml"),
                       "Path to secrets YAML (wallet, API keys). "
                       "Merged on top of config. Ignored if file does not exist.")
        ("dry-run,d", po::bool_switch()->default_value(false),
                       "Compute quotes without submitting offers to the network")
        ("verbose,v", po::bool_switch()->default_value(false),
                       "Enable DEBUG-level logging output");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const po::error& e) {
        std::cerr << "Error: " << e.what() << "\n\n" << desc << "\n";
        std::exit(EXIT_FAILURE);
    }

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return std::nullopt;
    }

    // Resolve secrets path: default "secrets.yaml" is only used if the file
    // actually exists; otherwise pass empty to skip secrets loading.
    std::string secrets = vm["secrets"].as<std::string>();
    if (!std::filesystem::exists(secrets)) {
        secrets.clear();
    }

    return CliArgs{
        .config_path  = vm["config"].as<std::string>(),
        .secrets_path = std::move(secrets),
        .dry_run      = vm["dry-run"].as<bool>(),
        .verbose      = vm["verbose"].as<bool>(),
    };
}

// =============================================================================
// Logging initialization
// =============================================================================

/// Configure spdlog with a coloured console sink and a rotating file sink.
///
/// Log format: ISO-8601 timestamp | level | thread-id | message
/// File sink always captures DEBUG for post-mortem analysis; console sink
/// respects the --verbose flag.
///
/// The log directory (logs/) is created if absent.
static void init_logging(bool verbose) {
    // Ensure the log directory exists before opening the file sink.
    const fs::path log_dir{"logs"};
    if (!fs::exists(log_dir)) {
        fs::create_directories(log_dir);
    }

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(verbose ? spdlog::level::debug : spdlog::level::info);

    // Rotating file: 10 MB per file, 9 rotated files kept (100 MB total).
    //
    // [CLIENTLOG 2026-09-01] Raised 5 -> 9 in the same commit that routed the
    // four RPC clients onto these sinks (xop/util/client_logger.hpp).  Those
    // clients were stdout-only and their DEBUG tier emitted nowhere at all, so
    // making them visible adds a measured +13 to +25 MB/day (central ~19) on
    // top of the measured 35.8 MB/day baseline.  At the old 60 MB the operator's
    // post-mortem window would have fallen from ~40 h to ~26 h -- below the
    // overnight line that the frozen-oracle and batch_failed incidents both
    // needed.  100 MB / ~55 MB/day restores ~44 h.  Cost: +40 MB of disk.
    constexpr std::size_t kMaxFileSize = 10 * 1024 * 1024;
    constexpr std::size_t kMaxFiles    = 9;
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "logs/xop_trader.log", kMaxFileSize, kMaxFiles);
    file_sink->set_level(spdlog::level::debug);

    auto logger = std::make_shared<spdlog::logger>(
        "xop", spdlog::sinks_init_list{console_sink, file_sink});

    logger->set_level(spdlog::level::debug);
    // [CLIENTLOG 2026-09-01] %n (logger name) added.  Every logger that clones
    // or shares these sinks -- CoinMgr, OfferMgr, OnChainReconciler, and now
    // chia.fullnode / chia.wallet / dexie / coingecko / tibetswap -- was
    // indistinguishable from the engine in this file, because the shared sink
    // owns the formatter and the pattern had no %n.  It also keeps the [dexie]
    // tag operators grep in engine.log, which adopting this pattern would
    // otherwise have dropped.  Engine lines now carry [xop]; message text is
    // unchanged, so greps on content still match.  Cost ~1.4 MB/day.
    logger->set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%n] [%^%l%$] [tid:%t] %v");
    logger->flush_on(spdlog::level::warn);  // Auto-flush on warn and above.

    spdlog::set_default_logger(std::move(logger));
    spdlog::info("Logging initialized (console={}, file=logs/xop_trader.log)",
                 verbose ? "DEBUG" : "INFO");
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    // ------------------------------------------------------------------
    // 1. Initialize libcurl globally (ISO/IEC 5055: resource init once).
    //
    //    curl_global_init() is NOT thread-safe.  It must be called exactly
    //    once, before any threads are spawned or any curl handles created.
    //    The Engine's RPC clients (ChiaFullNodeRPC, ChiaWalletRPC,
    //    DexieClient) all use libcurl internally; calling this here
    //    ensures the library is ready before any subsystem construction.
    // ------------------------------------------------------------------
    const CURLcode curl_rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_rc != CURLE_OK) {
        std::cerr << "Fatal: curl_global_init failed: "
                  << curl_easy_strerror(curl_rc) << "\n";
        return EXIT_FAILURE;
    }

    // ------------------------------------------------------------------
    // 2. Parse command-line arguments
    // ------------------------------------------------------------------
    const auto cli_opt = parse_cli(argc, argv);
    if (!cli_opt.has_value()) {
        curl_global_cleanup();
        return EXIT_SUCCESS;  // --help was requested.
    }
    const auto& cli = cli_opt.value();

    // ------------------------------------------------------------------
    // 3. Initialize structured logging (must precede any spdlog calls)
    // ------------------------------------------------------------------
    try {
        init_logging(cli.verbose);
    } catch (const spdlog::spdlog_ex& e) {
        std::cerr << "Fatal: logging init failed: " << e.what() << "\n";
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    spdlog::info("XOPTrader v{} starting (PID {})", XOP_VERSION,
#ifdef _WIN32
                 GetCurrentProcessId()
#else
                 getpid()
#endif
    );

    if (cli.dry_run) {
        spdlog::warn("*** DRY-RUN MODE -- no offers will be submitted ***");
    }

    // ------------------------------------------------------------------
    // 3a. Kill any old xop_trader instances still running.
    //
    //     This ensures only one engine is active at a time -- prevents
    //     double-posting offers, port conflicts on the Prometheus
    //     exporter, and wallet RPC contention.
    //
    //     [S31] NOT in dry run. This used to run before cli.dry_run was
    //     ever inspected, which made a dry run the most dangerous thing in
    //     the repository: it terminated the live engine -- on Windows via
    //     TerminateProcess, with no graceful path and therefore no
    //     cancel_all() -- and the replacement process then neither posts
    //     nor cancels, because both the watchdog and shutdown()'s
    //     cancellation are dry-run gated. Startup reconciliation is NOT
    //     gated, so the dry run would even adopt the orphans it could not
    //     cancel. That is the ten-orphaned-offers incident of 2026-08-26,
    //     reachable by running an inspection command.
    //
    //     A dry run has no reason to want the live engine dead: it submits
    //     nothing, so there is no double-posting to prevent. The residual
    //     conflicts are the Prometheus port and wallet RPC contention,
    //     which are inconveniences rather than money.
    // ------------------------------------------------------------------
    //     [F4 2026-09-03] MOVED BELOW load_config. The graceful rung needs
    //     the data directory to write data/shutdown.flag into, and that comes
    //     from config_.database.path -- the same derivation the Engine uses.
    //     Nothing between here and there depends on the old instance being
    //     dead: load_config only reads YAML. The DB is opened afterwards,
    //     which is the first thing that actually needs the exclusion.
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // 4. Load and validate YAML configuration (xop::load_config)
    //
    //    Uses the fully-validated loader from config.cpp that maps every
    //    YAML field into typed AppConfig structs with domain checks.
    //    Throws xop::ConfigError on any structural or value error.
    // ------------------------------------------------------------------
    xop::AppConfig app_config;
    try {
        app_config = xop::load_config(cli.config_path, cli.secrets_path);
    } catch (const xop::ConfigError& e) {
        spdlog::critical("Configuration error: {}", e.what());
        spdlog::shutdown();
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    // ------------------------------------------------------------------
    // 3a (deferred). Kill any old xop_trader instances still running.
    //
    //     Graceful first on both platforms now -- see the long note on
    //     kill_old_instances(). The flag path is derived exactly as
    //     Engine's constructor derives it, so the old process recognises it.
    // ------------------------------------------------------------------
    if (cli.dry_run) {
        spdlog::warn("[S31] dry run -- NOT killing any running engine. A live "
                     "instance keeps its book and its dead man's switch.");
    } else {
        std::filesystem::path db_dir =
            std::filesystem::path(app_config.database.path).parent_path();
        if (db_dir.empty()) db_dir = std::filesystem::current_path();
        kill_old_instances(db_dir / "shutdown.flag");
    }

    // ------------------------------------------------------------------
    // 4a. [review] A DRY RUN GETS ITS OWN DATABASE.
    //
    //     Skipping kill_old_instances() above is right -- a dry run submits
    //     nothing, so there is no double-posting to prevent -- but it also
    //     removed the mutual exclusion that every DB write in the engine
    //     silently depended on. Until this branch, exactly one process ever
    //     held that file.
    //
    //     Dry run is NOT read-only there. It restores the live engine's
    //     pending offers into State, so step_process_fills() then observes
    //     the live engine's fills and writes them: offer status, trade log,
    //     ledger entries, and persist_inventory_state() -- which rewrites
    //     EVERY inventory row from a snapshot that stopped tracking reality
    //     at startup. inventory_state.total_cost is the table whose loss
    //     produced the P&L failure this project already spent a month on.
    //     step_update_pnl() adds snapshot and strategy_quotes rows every
    //     block, and the reward/bridge ingest branches on the ledger insert
    //     COUNT -- so whichever process books a reward first makes the other
    //     silently drop that income from its own running state.
    //
    //     Fixed at the persistence boundary rather than by guarding each
    //     step. There are a dozen reachable write sites across six
    //     functions, and this is the fourth round on dry-run isolation --
    //     each previous one gated the sites it was shown and missed the
    //     rest. A copy cannot be missed by a new step.
    //
    //     Copied rather than opened read-only, deliberately: writes would
    //     then throw, and several callers convert a write failure into a
    //     LATCHED degradation -- post_ledger_fill and the reward/bridge
    //     ingest set ledger_incomplete_ and raise an ExposureBreach alert.
    //     A read-only dry run would spend its life alarming about a database
    //     that is fine. The copy also keeps every read the run needs
    //     truthful: pending offers, inventory cost basis, and the snapshot
    //     history the warm start replays.
    //
    //     PnLTracker opens its own second handle on this same path and
    //     derives its trade_history CSV directory from it, so redirecting
    //     the path covers all three. accounting.bridge_jobs_db_path needs
    //     no change: it is already opened SQLITE_OPEN_READONLY.
    // ------------------------------------------------------------------
    // Removed at exit by the guard below -- and stale copies from EARLIER
    // dry runs (a crash skips any exit path) are swept here, so repeated
    // inspections cannot accumulate accounting snapshots in the temp
    // directory until it fills.
    std::filesystem::path dryrun_scratch;
    if (cli.dry_run) {
        namespace fs = std::filesystem;
        try {
            // [review round 11] Reap only ORPHANED copies. Multiple dry
            // runs are legal now (kill_old_instances is skipped), and on
            // POSIX unlinking another live run's open database succeeds --
            // its next connection at that path would find a fresh empty
            // file instead of the snapshot it was reading. The pid is in
            // the filename; a copy is swept only when that process is
            // provably gone.
            auto pid_alive = [](long long pid) -> bool {
                if (pid <= 0) return false;
#ifdef _WIN32
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, static_cast<DWORD>(pid));
                if (!h) return false;
                DWORD code = 0;
                const bool alive = GetExitCodeProcess(h, &code)
                                   && code == STILL_ACTIVE;
                CloseHandle(h);
                return alive;
#else
                return ::kill(static_cast<pid_t>(pid), 0) == 0
                       || errno == EPERM;
#endif
            };
            std::error_code sweep_ec;
            for (const auto& entry : fs::directory_iterator(
                     fs::temp_directory_path(), sweep_ec)) {
                const auto name = entry.path().filename().string();
                if (name.rfind("xop_dryrun_", 0) != 0) continue;
                long long owner = 0;
                try {
                    owner = std::stoll(name.substr(11));
                } catch (const std::exception&) {
                    owner = 0;   // unparseable -> treat as orphaned
                }
                if (!pid_alive(owner)) {
                    fs::remove(entry.path(), sweep_ec);
                }
            }
            const fs::path live{app_config.database.path};
            const auto stamp = std::to_string(static_cast<long long>(
#ifdef _WIN32
                GetCurrentProcessId()
#else
                getpid()
#endif
            ));
            const fs::path scratch =
                fs::temp_directory_path()
                / ("xop_dryrun_" + stamp + ".sqlite");

            std::error_code ec;
            fs::remove(scratch, ec);
            fs::remove(fs::path{scratch.string() + "-wal"}, ec);
            fs::remove(fs::path{scratch.string() + "-shm"}, ec);
            // [review round 9] SQLite's backup API, not a file copy.
            //
            // Sequentially copying main + -wal + -shm of a database another
            // process is WRITING is not a snapshot of anything: a checkpoint
            // landing between the two copies moves committed frames into the
            // live main file after we copied it, so the scratch DB silently
            // loses committed rows -- and a half-copied WAL is not stale, it
            // is CORRUPT. sqlite3_backup_step(-1) runs under a single WAL
            // read snapshot, sees every committed frame, and is neither
            // blocked by nor blocks the live writer.
            if (fs::exists(live)) {
                sqlite3* src = nullptr;
                sqlite3* dst = nullptr;
                int rc = sqlite3_open_v2(live.string().c_str(), &src,
                                         SQLITE_OPEN_READONLY, nullptr);
                if (rc == SQLITE_OK) {
                    rc = sqlite3_open(scratch.string().c_str(), &dst);
                }
                if (rc == SQLITE_OK) {
                    sqlite3_busy_timeout(src, 5000);
                    sqlite3_busy_timeout(dst, 5000);
                    sqlite3_backup* bk =
                        sqlite3_backup_init(dst, "main", src, "main");
                    rc = bk ? sqlite3_backup_step(bk, -1) : SQLITE_ERROR;
                    if (bk) sqlite3_backup_finish(bk);
                }
                const std::string err =
                    (rc == SQLITE_DONE || rc == SQLITE_OK)
                        ? std::string{}
                        : (src ? sqlite3_errmsg(src) : "open failed");
                if (src) sqlite3_close(src);
                if (dst) sqlite3_close(dst);
                if (!err.empty()) {
                    throw std::runtime_error(
                        "sqlite backup failed: " + err);
                }
            }
            spdlog::warn("[S31] dry run -- database redirected to {} so this "
                         "inspection cannot write to the live engine's "
                         "accounting state", scratch.string());
            app_config.database.path = scratch.string();
            dryrun_scratch = scratch;
        } catch (const std::exception& e) {
            spdlog::critical("[S31] dry run -- could NOT isolate the database "
                             "({}). Refusing to start: running against the "
                             "live file would corrupt its accounting.",
                             e.what());
            spdlog::shutdown();
            curl_global_cleanup();
            return EXIT_FAILURE;
        }
    }

    // [review round 10] The copy is deleted when this scope unwinds --
    // normal exit, config error, or engine-constructor throw alike. The
    // sweep above catches what a hard crash leaves behind.
    struct ScratchGuard {
        std::filesystem::path path;
        ~ScratchGuard() {
            if (path.empty()) return;
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::filesystem::remove(
                std::filesystem::path{path.string() + "-wal"}, ec);
            std::filesystem::remove(
                std::filesystem::path{path.string() + "-shm"}, ec);
        }
    } scratch_guard{dryrun_scratch};

    // Count enabled trading pairs for the startup banner.
    std::size_t enabled_pairs = 0;
    for (const auto& pair : app_config.pairs) {
        if (pair.enabled) ++enabled_pairs;
    }
    spdlog::info("Configuration loaded: {} pair(s) enabled, mode={}, target {}:{}",
                 enabled_pairs,
                 xop::to_string(app_config.chia.mode),
                 app_config.chia.mode == xop::ChiaMode::WalletOnly
                     ? app_config.chia.wallet_host
                     : app_config.chia.full_node_host,
                 app_config.chia.mode == xop::ChiaMode::WalletOnly
                     ? app_config.chia.wallet_port
                     : app_config.chia.full_node_port);

    // ------------------------------------------------------------------
    // 5. Construct the Engine (owns io_context, State, all subsystems).
    //
    //    xop::Engine takes (const AppConfig&, bool dry_run) and internally
    //    constructs the io_context, State, Database, RPC clients, strategy
    //    layer, risk layer, and monitoring layer in dependency order.
    //    The constructor validates the full configuration and fails fast
    //    if any subsystem cannot initialise.
    // ------------------------------------------------------------------
    std::unique_ptr<xop::Engine> engine;
    try {
        engine = std::make_unique<xop::Engine>(app_config, cli.dry_run);
    } catch (const std::exception& e) {
        spdlog::critical("Engine construction failed: {}", e.what());
        spdlog::shutdown();
        curl_global_cleanup();
        return EXIT_FAILURE;
    }
    // [RELOAD] The engine needs its own config file locations to honour a
    // GUI-initiated hot reload (pair disables applied live).
    engine->set_config_paths(cli.config_path, cli.secrets_path);

    // ------------------------------------------------------------------
    // 6. Install signal handlers (SIGINT, SIGTERM) via std::signal.
    //
    //    The xop::Engine owns its own io_context, so we cannot use
    //    asio::signal_set from outside.  std::signal is portable and
    //    sufficient: the handler calls engine->shutdown() (which posts
    //    work to the internal io_context via an atomic flag).
    //
    //    Two-phase protocol:
    //      First signal  -> engine->shutdown() (graceful, cancels offers)
    //      Second signal -> std::_Exit()       (force-kill escape hatch)
    //
    //    On Windows, SIGINT maps to Ctrl+C and SIGTERM is best-effort
    //    (delivered only by explicit TerminateProcess / taskkill).
    // ------------------------------------------------------------------
    g_engine_ptr.store(engine.get(), std::memory_order_release);

    std::signal(SIGINT,  signal_handler);
#ifdef SIGTERM
    std::signal(SIGTERM, signal_handler);
#endif

    // ------------------------------------------------------------------
    // 7. Enter the main loop.
    //
    //    engine->run() opens all RPC connections, starts the Prometheus
    //    exporter, begins the 5-second block-height polling timer, and
    //    blocks on ioc_.run() until shutdown() is called.  All 13 steps
    //    of the per-block heartbeat cycle execute within this call.
    // ------------------------------------------------------------------
    try {
        spdlog::info("Entering main loop -- ready to trade");
        engine->run();
    } catch (const std::exception& e) {
        spdlog::critical("Engine terminated with unhandled exception: {}",
                         e.what());
    }

    // ------------------------------------------------------------------
    // 8. Cleanup -- deterministic, RAII-driven.
    //
    //    Destruction order (reverse of construction):
    //      - Deregister signal handler (prevent use-after-free)
    //      - Engine destructor: calls shutdown() if still running,
    //        destroys all subsystems in reverse construction order,
    //        stops internal io_context
    //      - curl_global_cleanup(): release libcurl global state
    //      - spdlog::shutdown(): flush all sinks
    //
    //    This mirrors the construction order and satisfies the invariant
    //    that no destroyed object is referenced by a still-alive one.
    // ------------------------------------------------------------------

    // Deregister the signal handler before destroying the engine to
    // prevent a signal from calling shutdown() on a dangling pointer.
    g_engine_ptr.store(nullptr, std::memory_order_release);
    std::signal(SIGINT,  SIG_DFL);
#ifdef SIGTERM
    std::signal(SIGTERM, SIG_DFL);
#endif

    // Engine destructor handles subsystem teardown.
    engine.reset();

    spdlog::info("XOPTrader shutdown complete");
    spdlog::shutdown();
    curl_global_cleanup();

    return EXIT_SUCCESS;
}
