// =============================================================================
// main.cpp -- Entry point for XOPTrader CHIA DEX market-making engine.
// =============================================================================
//
// Initialization order (strict — later steps depend on earlier ones):
//   1. Parse CLI arguments    (--config, --dry-run, --verbose)
//   2. Initialize structured logging (spdlog — must precede any log calls)
//   3. Load and validate YAML configuration via xop::load_config()
//   4. Create boost::asio::io_context (drives all async I/O)
//   5. Construct xop::State (thread-safe global runtime state)
//   6. Construct Engine      (owns all trading subsystems)
//   7. Install signal handlers (SIGINT, SIGTERM) via asio::signal_set
//   8. Spawn Engine::run() coroutine and enter the event loop
//
// Shutdown sequence (on signal or unrecoverable error):
//   1. Signal handler fires -> Engine::request_shutdown()
//   2. Engine breaks out of the heartbeat loop
//   3. Engine cancels all outstanding CHIA offers via Wallet RPC
//   4. Engine coroutine completes -> completion handler calls io_context.stop()
//   5. Worker threads join (jthread RAII), logging flushes, objects destruct
//
// A second signal (double Ctrl-C) force-stops the io_context immediately,
// bypassing the graceful offer-cancellation path.  This is the escape hatch
// if the wallet RPC is unreachable during shutdown.
//
// Security (ISO/IEC 27001:2022):
//   - No hardcoded secrets.  SSL cert paths and tokens are loaded from YAML.
//   - Secret fields (certs, fingerprint, Telegram token) are never logged.
//
// Secure coding (ISO/IEC 5055):
//   - Stack protector and control-flow guard enabled via CMake.
//   - No raw owning pointers; RAII throughout (unique_ptr, jthread).
//   - All external input (CLI, YAML) validated before use.
// =============================================================================

#include <xop/config.hpp>
#include <xop/state.hpp>
#include <xop/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Boost
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>

// Third-party
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace asio = boost::asio;
namespace po   = boost::program_options;
namespace fs   = std::filesystem;

// =============================================================================
// Engine — Central orchestrator (stub).
//
// The full implementation lives in engine.cpp (future).  This inline stub
// provides enough structure for main.cpp to compile, link, and run the
// heartbeat loop while subsystems are developed incrementally.
//
// Subsystems (constructed in dependency order, torn down in reverse):
//   1. MarketDataFeed   — CEX + DEX price aggregation
//   2. VolatilityModel  — Yang-Zhang hybrid estimator
//   3. OfferManager     — CHIA Wallet RPC: create / cancel / monitor offers
//   4. StrategyEngine   — Avellaneda-Stoikov / GLFT quote computation
//   5. RiskManager      — inventory limits, never-sell-at-loss enforcement
//   6. CostBasisTracker — FIFO per-asset cost accounting (via xop::State)
//   7. MetricsExporter  — Prometheus pull endpoint
//
// The Engine exposes a single coroutine entry point run() that implements
// the per-block heartbeat loop described in Section 13 of the strategy doc.
// =============================================================================

class Engine {
public:
    /// Construct the Engine.  All subsystem constructors run here (fail-fast).
    ///
    /// @param io       io_context that drives all async operations.
    /// @param config   Validated application configuration.
    /// @param state    Shared mutable runtime state (positions, offers, markets).
    /// @param dry_run  If true, compute quotes but never submit on-chain offers.
    explicit Engine(asio::io_context&  io,
                    xop::AppConfig     config,
                    xop::State&        state,
                    bool               dry_run)
        : io_context_{io}
        , config_{std::move(config)}
        , state_{state}
        , dry_run_{dry_run}
        , shutdown_requested_{false}
    {
        state_.set_status(xop::BotStatus::Initializing);

        // TODO: construct subsystems in dependency order:
        //   1. MarketDataFeed  (needs config_.chia, config_.dexie)
        //   2. VolatilityModel (needs config_.volatility)
        //   3. OfferManager    (needs config_.chia for wallet RPC + io_context)
        //   4. StrategyEngine  (needs config_.strategy)
        //   5. RiskManager     (needs config_.risk, state_)
        //   6. MetricsExporter (needs config_.monitoring.prometheus_port)

        spdlog::info("Engine constructed (dry_run={}, pairs={})",
                     dry_run_, config_.pairs.size());
    }

    /// Request a graceful shutdown.  Thread-safe (called from signal handler
    /// context via asio::post, but the atomic itself is safe from any thread).
    void request_shutdown() noexcept {
        bool expected = false;
        if (shutdown_requested_.compare_exchange_strong(expected, true)) {
            spdlog::info("Shutdown requested — will cancel all outstanding offers");
            state_.set_status(xop::BotStatus::ShuttingDown);

            // Post teardown work onto the io_context so it runs on the event-
            // loop thread, avoiding data races with running coroutines.
            asio::post(io_context_, [this]() { initiate_teardown(); });
        }
    }

    /// True once request_shutdown() has been called at least once.
    [[nodiscard]] bool is_shutdown_requested() const noexcept {
        return shutdown_requested_.load(std::memory_order_acquire);
    }

    /// Main coroutine — the per-block heartbeat loop (Section 13).
    /// Spawned onto io_context via co_spawn() from main().
    asio::awaitable<void> run() {
        state_.set_status(xop::BotStatus::Running);
        spdlog::info("Engine::run() — entering per-block heartbeat loop");

        // Steady timer paces the heartbeat at one CHIA block interval (~52 s).
        // The timer is cancellable: initiate_teardown() cancels it to break
        // the loop promptly rather than waiting up to 52 seconds.
        asio::steady_timer heartbeat{io_context_};

        // CHIA block time is approximately 52 seconds (Section 5 of strategy).
        constexpr auto kBlockInterval = std::chrono::seconds{52};

        while (!is_shutdown_requested()) {
            try {
                // ----- Per-block heartbeat (Section 13 of strategy doc) ------
                //
                // The nine steps executed every block:
                //
                //  1. Update market state (prices from CEX + DEX feeds)
                //  2. Process any fills detected since last heartbeat
                //  3. Recompute volatility, PIN, regime estimates
                //  4. Compute optimal quotes (Avellaneda-Stoikov / GLFT)
                //  5. Apply never-sell-at-loss constraint:
                //       ask = max(optimal_ask, cost_basis + min_profit_margin)
                //  6. Apply Half-Kelly position size limits
                //  7. Cancel stale offers (TTL expired), post new ones
                //  8. Update PnL attribution (spread / inventory / fees)
                //  9. Export metrics to Prometheus
                //
                // Each step will be implemented as a subsystem coroutine call
                // once the respective module is built.

                spdlog::debug("Heartbeat tick — awaiting next block interval");

                heartbeat.expires_after(kBlockInterval);
                co_await heartbeat.async_wait(asio::use_awaitable);
            } catch (const boost::system::system_error& e) {
                // timer.cancel() during shutdown raises operation_aborted.
                if (e.code() == asio::error::operation_aborted) {
                    spdlog::info("Heartbeat timer cancelled — exiting run loop");
                    break;
                }
                spdlog::error("Heartbeat error: {}", e.what());
            }
        }

        // Graceful teardown: cancel all CHIA offers before returning.
        // This is the critical safety step — we must not leave orphaned offers
        // on the network after the bot shuts down, or a counterparty could take
        // them at stale prices.
        co_await cancel_all_offers();

        state_.set_status(xop::BotStatus::Stopped);
        spdlog::info("Engine::run() — exited cleanly");
    }

private:
    /// Cancel every outstanding offer via Chia Wallet RPC (cancel_offers).
    ///
    /// In dry-run mode this is a no-op because no offers were ever submitted.
    /// In live mode, each cancellation spends a locked coin (secure cancel)
    /// and requires ~52 seconds to confirm on-chain.  We fire all cancellations
    /// in parallel and await their collective completion.
    asio::awaitable<void> cancel_all_offers() {
        const auto offers = state_.get_all_offers();

        if (offers.empty()) {
            spdlog::info("No outstanding offers to cancel");
            co_return;
        }

        if (dry_run_) {
            spdlog::info("Dry-run mode — skipping cancellation of {} offer(s)",
                         offers.size());
            co_return;
        }

        spdlog::info("Cancelling {} outstanding offer(s)...", offers.size());

        // TODO: for each offer in `offers`:
        //   co_await wallet_rpc_->cancel_offer(offer.offer_id, fee, secure=true);
        //   state_.remove_offer(offer.offer_id);
        //
        // Cancellations should be dispatched concurrently (co_spawn per offer)
        // with a timeout guard — if the wallet is unreachable we must not hang
        // forever.  The second-signal force-kill path in main() is the backstop.

        for (const auto& offer : offers) {
            spdlog::info("  Would cancel offer id={} pair={} side={} price={}",
                         offer.offer_id, offer.pair_name,
                         xop::to_string(offer.side), offer.price);
            state_.remove_offer(offer.offer_id);
        }

        spdlog::info("All offers cancelled (or removal recorded)");
        co_return;
    }

    /// Post-shutdown subsystem cleanup dispatched onto the event loop.
    /// Called via asio::post() from request_shutdown().
    void initiate_teardown() {
        spdlog::info("Tearing down subsystems in reverse construction order...");
        // TODO: stop subsystems in reverse order:
        //   7. MetricsExporter.stop()
        //   6. (CostBasisTracker — no explicit stop; state persists in DB)
        //   5. RiskManager.stop()
        //   4. StrategyEngine.stop()
        //   3. OfferManager.stop()   — cancel_all_offers() handles the heavy lifting
        //   2. VolatilityModel.stop()
        //   1. MarketDataFeed.stop()
        //
        // The io_context::stop() call happens in the co_spawn completion handler
        // in main(), not here, to ensure cancel_all_offers() runs to completion.
    }

    asio::io_context&  io_context_;         ///< Borrowed reference; owned by main().
    xop::AppConfig     config_;             ///< Immutable after construction.
    xop::State&        state_;              ///< Shared mutable runtime state.
    bool               dry_run_;            ///< If true, never submit real offers.
    std::atomic<bool>  shutdown_requested_; ///< Signal-safe shutdown flag.
};

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
    // 1. Parse command-line arguments
    // ------------------------------------------------------------------
    const auto cli_opt = parse_cli(argc, argv);
    if (!cli_opt.has_value()) {
        return EXIT_SUCCESS;  // --help was requested.
    }
    const auto& cli = cli_opt.value();

    // ------------------------------------------------------------------
    // 2. Initialize structured logging (must precede any spdlog calls)
    // ------------------------------------------------------------------
    try {
        init_logging(cli.verbose);
    } catch (const spdlog::spdlog_ex& e) {
        std::cerr << "Fatal: logging init failed: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    spdlog::info("XOPTrader v{} starting", "0.1.0");
    if (cli.dry_run) {
        spdlog::warn("*** DRY-RUN MODE -- no offers will be submitted ***");
    }

    // ------------------------------------------------------------------
    // 3. Load and validate YAML configuration (xop::load_config)
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
        return EXIT_FAILURE;
    }

    // Count enabled trading pairs for the startup banner.
    std::size_t enabled_pairs = 0;
    for (const auto& pair : app_config.pairs) {
        if (pair.enabled) ++enabled_pairs;
    }
    spdlog::info("Configuration loaded: {} pair(s) enabled, target {}:{}",
                 enabled_pairs,
                 app_config.chia.full_node_host,
                 app_config.chia.full_node_port);

    // ------------------------------------------------------------------
    // 4. Create the boost::asio io_context
    //
    //    Concurrency hint: one thread per core up to 4.  More threads add
    //    overhead without benefit — the bot is I/O-bound (52-second block
    //    intervals, sparse HTTP RPCs) not CPU-bound.
    // ------------------------------------------------------------------
    const auto hw_threads = std::max(1u, std::thread::hardware_concurrency());
    const auto io_threads = std::min(hw_threads, 4u);
    asio::io_context io_context{static_cast<int>(io_threads)};

    spdlog::info("io_context created with concurrency_hint={}", io_threads);

    // ------------------------------------------------------------------
    // 5. Construct global State (thread-safe runtime data store)
    // ------------------------------------------------------------------
    xop::State state;

    // ------------------------------------------------------------------
    // 6. Construct the Engine
    // ------------------------------------------------------------------
    auto engine = std::make_unique<Engine>(
        io_context, std::move(app_config), state, cli.dry_run);

    // ------------------------------------------------------------------
    // 7. Install signal handlers (SIGINT, SIGTERM) via asio::signal_set
    //
    //    This is cross-platform: on Windows, asio::signal_set correctly
    //    handles console Ctrl+C.  SIGTERM is best-effort on Windows
    //    (only delivered by explicit TerminateProcess or taskkill).
    //
    //    Two-phase protocol:
    //      First signal  -> graceful shutdown (cancel offers, drain work)
    //      Second signal -> force io_context.stop() (escape hatch)
    // ------------------------------------------------------------------
    asio::signal_set signals(io_context, SIGINT, SIGTERM);

    // Raw pointer is safe: engine outlives the signal handler because
    // engine is destroyed only after io_context.run() returns below.
    Engine* engine_ptr = engine.get();

    signals.async_wait([engine_ptr, &io_context, &signals](
            const boost::system::error_code& ec, int signal_number) {
        if (ec) {
            return;  // Handler cancelled during normal shutdown.
        }
        spdlog::warn("Caught signal {} -- initiating graceful shutdown",
                     signal_number);

        // Phase 1: request graceful shutdown (cancel offers, drain work).
        engine_ptr->request_shutdown();

        // Re-arm for a second signal: force-kill escape hatch.
        signals.async_wait([&io_context](
                const boost::system::error_code& ec2, int sig2) {
            if (ec2) return;
            spdlog::critical(
                "Second signal ({}) received -- forcing immediate exit", sig2);
            io_context.stop();
        });
    });

    // ------------------------------------------------------------------
    // 8. Spawn the Engine coroutine and run the event loop
    //
    //    co_spawn launches Engine::run() as an asio coroutine.  When it
    //    completes (after cancel_all_offers finishes), the completion
    //    handler stops the io_context so all threads' run() calls return.
    // ------------------------------------------------------------------
    asio::co_spawn(io_context, engine->run(),
        [&io_context](std::exception_ptr eptr) {
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception& e) {
                    spdlog::critical(
                        "Engine coroutine terminated with exception: {}",
                        e.what());
                }
            }
            // Engine exited — all offers cancelled.  Stop the event loop.
            io_context.stop();
        });

    // Launch worker threads.  The main thread also participates as a
    // worker, blocking on io_context.run() until stop() is called.
    std::vector<std::jthread> workers;
    workers.reserve(io_threads - 1);
    for (unsigned i = 1; i < io_threads; ++i) {
        workers.emplace_back([&io_context, i]() {
            spdlog::debug("Worker thread {} started", i);
            io_context.run();
            spdlog::debug("Worker thread {} exited", i);
        });
    }

    spdlog::info("Event loop running on {} thread(s) -- ready to trade",
                 io_threads);
    io_context.run();  // Main thread blocks here.

    // ------------------------------------------------------------------
    // 9. Cleanup — deterministic, RAII-driven.
    //
    //    Destruction order (reverse of construction):
    //      - jthread workers join automatically (RAII destructor)
    //      - unique_ptr<Engine> destroys the engine and all subsystems
    //      - State destructor releases mutex-guarded maps
    //      - io_context destructor cleans up residual handlers
    //      - spdlog::shutdown() flushes all sinks
    //
    //    This mirrors the construction order and satisfies the invariant
    //    that no destroyed object is referenced by a still-alive one.
    // ------------------------------------------------------------------
    spdlog::info("XOPTrader shutdown complete");
    spdlog::shutdown();

    return EXIT_SUCCESS;
}
