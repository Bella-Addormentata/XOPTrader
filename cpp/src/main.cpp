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
#  include <fstream>
#endif

// Third-party
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
// =============================================================================

#ifdef _WIN32

static void kill_old_instances() {
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

    for (const DWORD pid : old_pids) {
        HANDLE proc = OpenProcess(
            PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (proc == nullptr) {
            spdlog::warn("[Startup] Cannot open PID {} (err={}) -- skipping",
                         pid, GetLastError());
            continue;
        }
        spdlog::info("[Startup] Terminating old xop_trader instance (PID {})",
                     pid);
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

    spdlog::info("[Startup] Terminated {} old instance(s) -- waiting for "
                 "port release", old_pids.size());
    // Brief pause for the OS to fully release bound sockets.
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

#else  // POSIX (Linux / macOS)

static void kill_old_instances() {
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

    for (const pid_t pid : old_pids) {
        spdlog::info("[Startup] Sending SIGTERM to old xop_trader "
                     "(PID {})", pid);
        kill(pid, SIGTERM);
    }

    // Wait up to 10 s for graceful shutdown.
    for (int i = 0; i < 20; ++i) {
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
            spdlog::warn("[Startup] PID {} did not exit gracefully -- "
                         "sending SIGKILL", pid);
            kill(pid, SIGKILL);
        }
    }

    spdlog::info("[Startup] Terminated {} old instance(s) -- waiting for "
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

    return CliArgs{
        .config_path = vm["config"].as<std::string>(),
        .dry_run     = vm["dry-run"].as<bool>(),
        .verbose     = vm["verbose"].as<bool>(),
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

    // Rotating file: 10 MB per file, 5 rotated files kept.
    constexpr std::size_t kMaxFileSize = 10 * 1024 * 1024;
    constexpr std::size_t kMaxFiles    = 5;
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "logs/xop_trader.log", kMaxFileSize, kMaxFiles);
    file_sink->set_level(spdlog::level::debug);

    auto logger = std::make_shared<spdlog::logger>(
        "xop", spdlog::sinks_init_list{console_sink, file_sink});

    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%^%l%$] [tid:%t] %v");
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

    // ------------------------------------------------------------------
    // 3a. Kill any old xop_trader instances still running.
    //
    //     This ensures only one engine is active at a time -- prevents
    //     double-posting offers, port conflicts on the Prometheus
    //     exporter, and wallet RPC contention.
    // ------------------------------------------------------------------
    kill_old_instances();

    if (cli.dry_run) {
        spdlog::warn("*** DRY-RUN MODE -- no offers will be submitted ***");
    }

    // ------------------------------------------------------------------
    // 4. Load and validate YAML configuration (xop::load_config)
    //
    //    Uses the fully-validated loader from config.cpp that maps every
    //    YAML field into typed AppConfig structs with domain checks.
    //    Throws xop::ConfigError on any structural or value error.
    // ------------------------------------------------------------------
    xop::AppConfig app_config;
    try {
        app_config = xop::load_config(cli.config_path);
    } catch (const xop::ConfigError& e) {
        spdlog::critical("Configuration error: {}", e.what());
        spdlog::shutdown();
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

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
