// ---------------------------------------------------------------------------
// test_client_logger.cpp -- where an RPC client's diagnosis actually goes.
//
// [CLIENTLOG 2026-09-01] The four RPC clients built their loggers with
//
//     log_ = spdlog::get("<name>");
//     if (!log_) log_ = spdlog::stdout_color_mt("<name>");
//
// and stdout_color_mt() produces a ONE-sink logger with no connection to the
// rotating file sink main.cpp opens.  Measured in the live corpus on
// 2026-09-01: the engine's SYMPTOM line appears 1,743 times in
// logs/xop_trader.log while the four clients' DIAGNOSIS lines appear ZERO
// times.  Any RPC-layer fault was invisible in the only log that survives a
// restart.
//
// Worse than "wrong sink": registry::initialize_logger also stamps the
// registry's global level onto the new logger, which is level::info
// (registry.h:116).  The 14 ->debug() call sites across cpp/src/rpc/ were
// therefore rejected in logger::log_ BEFORE FORMATTING (logger.h:317-322) and
// emitted nowhere at all, not even to stdout.  So a fix that attached the file
// sink and left the level alone would have looked right and changed nothing.
// Both halves are pinned below, separately.
//
// These tests drive xop::util::make_client_logger directly -- it takes its
// sink list as a parameter precisely so they can, per the repo's pure-header
// convention (tier_gain.hpp, cross_guard.hpp, coingecko_parse.hpp).  The last
// group drives a REAL production constructor, because nothing else in
// cpp/tests constructs a client and that is how regressions here have survived
// review rounds before.
//
// [review round 2] Three classes of hole were closed in this file:
//   * the fixture scrubbed the global spdlog registry on the way OUT but not
//     on the way IN, so tests that must observe an EMPTY registry were passing
//     only because of suite ordering (test_tibetswap.cpp registers
//     "tibetswap" five times and runs first);
//   * two tests asserted nothing about the object they had just obtained, so
//     they passed whether or not the adapter had done its job;
//   * the concurrency test aborted the whole binary on the failure it exists
//     to detect, instead of reporting it.
//
// ISO/IEC 27001:2022 -- no secrets; log destinations only, no payloads.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <atomic>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <boost/asio/io_context.hpp>

#include "xop/config.hpp"
#include "xop/rpc/coingecko_client.hpp"
#include "xop/util/client_logger.hpp"

using xop::util::get_or_create_client_logger;
using xop::util::make_client_logger;

namespace {

using Probe = spdlog::sinks::ringbuffer_sink_mt;

/// A capturing sink stands in for main.cpp's rotating_file_sink_mt.  The real
/// one cannot be used here: logs/xop_trader.log is held by the LIVE
/// xop_trader.exe, spdlog opens with _SH_DENYNO so the open would succeed, and
/// a rotating sink would then rename the running process's log out from under
/// it.
std::shared_ptr<Probe> probe(std::size_t capacity = 8) {
    return std::make_shared<Probe>(capacity);
}

/// Payload of the n-th record a probe captured, as a std::string.
std::string payload_at(const std::shared_ptr<Probe>& p, std::size_t idx) {
    const auto recs = p->last_raw();
    if (idx >= recs.size()) {
        return {};
    }
    return std::string(recs[idx].payload.data(), recs[idx].payload.size());
}

/// Every logger name this file or the production code under it can put into
/// the PROCESS-WIDE spdlog registry.  "xop" is on the list because
/// spdlog::set_default_logger() also inserts into loggers_ under the logger's
/// own name (registry-inl.h:105-111), so each test that installs a default
/// logger named "xop" leaves a registry entry holding that test's ringbuffer
/// alive.
constexpr const char* kOwnedNames[] = {
    "dexie", "coingecko", "tibetswap", "chia.wallet", "chia.fullnode",
    "race",  "xop",
};

/// spdlog's registry and default logger are PROCESS state, and every test in
/// this file runs in one process under --gtest_filter.
///
/// The scrub runs in SetUp as well as TearDown.  TearDown alone is not enough:
/// test_tibetswap.cpp constructs five TibetSwapClients, each of which now
/// registers "tibetswap", and TibetSwapClientLifecycle runs BEFORE this
/// fixture in the full binary.  Without the SetUp scrub, whether a test here
/// exercises a fresh registration or a leftover one depends on suite ordering
/// -- which is the vacuous-test shape this repo's mutation-check memory warns
/// about.
class ClientLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        saved_ = spdlog::default_logger();
        scrub();
    }

    void TearDown() override {
        scrub();
        spdlog::set_default_logger(saved_);
    }

    static void scrub() {
        for (const char* n : kOwnedNames) {
            spdlog::drop(n);
        }
    }

    std::shared_ptr<spdlog::logger> saved_;
};

}  // namespace

// -- The defect: the file sink was dropped ----------------------------------

TEST(ClientLogger, TheClientLoggerCarriesEveryProcessSinkNotJustTheConsole)
{
    // THE DEFECT, in one assertion.  stdout_color_mt() returns a ONE-sink
    // logger, so every DIAGNOSIS line went to stdout and nowhere else.
    // Compared by POINTER, not by count: a helper that rebuilt an equivalent
    // rotating sink would still not be writing to the file main.cpp opened,
    // and two sinks on one path fight over rotation.
    auto console = probe();
    auto file    = probe();

    auto log = make_client_logger("dexie", {console, file},
                                  spdlog::level::debug, {},
                                  spdlog::level::warn);
    ASSERT_NE(log, nullptr);

    const auto& s = log->sinks();
    ASSERT_EQ(s.size(), 2u) << "one sink means the file sink was dropped";
    EXPECT_EQ(s[0].get(), console.get());
    EXPECT_EQ(s[1].get(), file.get())
        << "the rotating file sink is the only log that survives a restart";
}

TEST(ClientLogger, ADebugRecordReachesTheFileSideSinkAndNotOnlyTheFirst)
{
    // Holding the right sinks is necessary but NOT sufficient.  A logger can
    // own a sink it never dispatches to, because its own level gates the
    // record out first -- and that is exactly what the old code did: the
    // registry stamps level::info on a factory-built logger, so the clients'
    // 14 ->debug() sites were dropped before formatting.  This is the
    // assertion that fails if someone reinstates stdout_color_mt.
    auto console = probe();
    auto file    = probe();

    auto log = make_client_logger("tibetswap", {console, file},
                                  spdlog::level::debug, {},
                                  spdlog::level::warn);
    ASSERT_NE(log, nullptr);
    log->debug("DIAGNOSIS pool reserves stale age={}", 42);

    for (const auto& p : {console, file}) {
        const auto recs = p->last_raw();
        ASSERT_EQ(recs.size(), 1u)
            << "a sink the record never reached -- level gate or wrong sink";
        EXPECT_EQ(recs[0].level, spdlog::level::debug);
    }
    EXPECT_NE(payload_at(file, 0).find("DIAGNOSIS"), std::string::npos)
        << "the diagnosis must land on the FILE side, not just the console";
}

TEST(ClientLogger, AnInfoLevelLoggerStillDropsTheDebugTier)
{
    // The negative control for the test above, and the reason the wrapper
    // inherits the level instead of leaving it at spdlog's default.  info is
    // 2, debug is 1, and logger::log_ returns on 1 >= 2 == false without
    // consulting any sink.  Without this, "we attached the file sink" reads
    // as a fix when it is half of one.
    auto file = probe();
    auto log  = make_client_logger("dexie", {file}, spdlog::level::info, {},
                                   spdlog::level::warn);
    ASSERT_NE(log, nullptr);

    log->debug("DIAGNOSIS this must not appear");
    EXPECT_TRUE(file->last_raw().empty())
        << "an info-level client logger cannot report a debug diagnosis";

    log->warn("DIAGNOSIS this must appear");
    EXPECT_EQ(file->last_raw().size(), 1u);
}

TEST(ClientLogger, TheNameSurvivesSoTheFileLogSaysWhichClientSpoke)
{
    // The four clients are only distinguishable in a shared file log by their
    // logger name.  Pinned separately because a helper could adopt the right
    // sinks and still hand back the default logger, merging all four into one
    // anonymous stream.
    auto p   = probe(4);
    auto log = make_client_logger("coingecko", {p}, spdlog::level::debug, {},
                                  spdlog::level::warn);
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->name(), "coingecko");

    log->debug("x");
    const auto recs = p->last_raw();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(std::string(recs[0].logger_name.data(),
                          recs[0].logger_name.size()),
              "coingecko");
}

TEST(ClientLogger, TheNamedPatternIsWhatMakesTheNameVisibleInTheFile)
{
    // Carrying the name on the record is only half of "the file log says which
    // client spoke" -- the SINK's formatter decides whether it is printed, and
    // the pre-change pattern in init_logging() had no %n.  That is why
    // CoinMgr/OfferMgr/OnChainReconciler are invisible in logs/xop_trader.log
    // today despite cloning the default logger under those names.
    //
    // main.cpp is not compiled by either permitted build target (xop_core,
    // xop_tests), so its literal cannot be checked here.  What IS pinned is
    // the mechanism the literal relies on: [%n] renders log_msg::logger_name,
    // and a client logger sharing the sink gets it without setting a pattern.
    auto p = probe();
    p->set_pattern("[%n] %v");

    auto log = make_client_logger("dexie", {p}, spdlog::level::debug, {},
                                  spdlog::level::warn);
    ASSERT_NE(log, nullptr);
    log->debug("DIAGNOSIS");

    const auto formatted = p->last_formatted();
    ASSERT_EQ(formatted.size(), 1u);
    EXPECT_NE(formatted[0].find("[dexie]"), std::string::npos)
        << "without %n on the shared sink the file cannot say who spoke";
}

TEST(ClientLogger, TheFlushPolicyIsSetBecauseSharingSinksDoesNotCarryIt)
{
    // flush_level_ is a LOGGER member (logger.h:311), not a sink property, so
    // a logger built over main.cpp's sinks starts at level::off and never
    // auto-flushes -- its records sit in the file buffer and a hard crash
    // loses exactly the diagnosis the post-mortem needs.
    auto p   = probe();
    auto log = make_client_logger("dexie", {p}, spdlog::level::debug, {},
                                  spdlog::level::warn);
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->flush_level(), spdlog::level::warn)
        << "off means the crash that produced the warn also loses it";
    EXPECT_EQ(log->level(), spdlog::level::debug);
}

// -- THE SHARED-SINK TRAP ---------------------------------------------------

TEST(ClientLogger, AnEmptyPatternLeavesTheSharedSinksFormatterAlone)
{
    // logger::set_pattern does NOT configure the logger -- it walks sinks_ and
    // reformats every sink (logger-inl.h:72-82).  Because a client logger
    // SHARES sink objects with the engine's "xop" logger, a helper that set a
    // pattern would silently rewrite the format of logs/xop_trader.log for the
    // whole process.  "" must mean inherit, and the wrapper always passes "".
    auto p = probe();
    p->set_pattern("ENGINEPATTERN %v");

    auto log = make_client_logger("dexie", {p}, spdlog::level::debug, {},
                                  spdlog::level::warn);
    ASSERT_NE(log, nullptr);
    log->info("payload");

    const auto formatted = p->last_formatted();
    ASSERT_EQ(formatted.size(), 1u);
    EXPECT_NE(formatted[0].find("ENGINEPATTERN"), std::string::npos)
        << "the client logger reformatted a sink it does not own";
}

// -- Refusals: an empty sink list is the fail-open case ---------------------

TEST(ClientLogger, AnEmptySinkListIsRefusedRatherThanReturnedSilent)
{
    // spdlog will build a logger over zero sinks (logger.h:53-55).  It never
    // throws, never warns, and discards every record -- strictly WORSE than
    // the stdout-only status quo this change replaces, because stdout at least
    // reaches an operator watching the console.  This repo has a documented
    // family of ten fail-open bugs and a silent logger is that shape exactly.
    EXPECT_EQ(make_client_logger("dexie", {}, spdlog::level::debug, {},
                                 spdlog::level::warn),
              nullptr)
        << "an unusable sink list must make the caller FALL BACK, not log "
           "into nothing";
}

TEST(ClientLogger, ANullSinkInTheListIsRefusedBeforeItCanCrash)
{
    // logger::sink_it_ dereferences every element with no null check, so a
    // null entry is a crash on the first record, on whatever thread got there
    // first.
    EXPECT_EQ(make_client_logger("dexie", {nullptr}, spdlog::level::debug, {},
                                 spdlog::level::warn),
              nullptr);

    auto ok = probe();
    EXPECT_EQ(make_client_logger("dexie", {ok, nullptr},
                                 spdlog::level::debug, {},
                                 spdlog::level::warn),
              nullptr)
        << "one bad entry poisons the list; refuse the whole thing";
}

// -- The other half of the rule: the caller must actually fall back ---------

TEST_F(ClientLoggerTest, TheCallerFallsBackToSomethingThatActuallyWrites)
{
    // Refusing is only safe if the caller then does something.  With a
    // sinkless default logger installed, the adapter must still hand back a
    // logger that WRITES -- never nullptr, and never the silent logger it just
    // refused.
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("xop", spdlog::sinks_init_list{}));

    auto log = get_or_create_client_logger("dexie");
    ASSERT_NE(log, nullptr) << "the 81 log_-> sites have no null guard";
    EXPECT_FALSE(log->sinks().empty())
        << "falling back to a second silent logger is not a fallback";
}

TEST_F(ClientLoggerTest, TheAdapterAdoptsTheProcessSinksAndTheProcessLevel)
{
    // The wrapper's whole job, asserted without going through a client.  The
    // old get()-then-stdout_color_mt() built a FRESH one-sink console logger
    // and took the registry's global level (info); this must instead adopt the
    // process sinks BY IDENTITY and the process level, or the debug diagnoses
    // still never reach the file.
    auto console = probe();
    auto file    = probe();
    auto app     = std::make_shared<spdlog::logger>(
        "xop", spdlog::sinks_init_list{console, file});
    app->set_level(spdlog::level::debug);
    app->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(app);

    auto log = get_or_create_client_logger("dexie");
    ASSERT_NE(log, nullptr);

    const auto& s = log->sinks();
    ASSERT_EQ(s.size(), 2u)
        << "one sink means a fresh console logger, not the process sinks";
    EXPECT_EQ(s[0].get(), console.get());
    EXPECT_EQ(s[1].get(), file.get())
        << "the client is not writing to the log that survives a restart";
    EXPECT_EQ(log->level(), spdlog::level::debug)
        << "an inherited info level drops every ->debug() diagnosis";
    EXPECT_EQ(log->flush_level(), spdlog::level::warn);

    log->debug("DIAGNOSIS reached the file side");
    EXPECT_EQ(payload_at(file, 0), "DIAGNOSIS reached the file side");
}

TEST_F(ClientLoggerTest, TheAdapterDoesNotReformatTheEnginesSharedSinks)
{
    // The adapter shares the engine's sink objects, and logger::set_pattern
    // writes THROUGH to them.  If it ever set a pattern, the format of
    // logs/xop_trader.log would change for the whole process as a side effect
    // of an RPC client being constructed.
    auto p = probe();
    p->set_pattern("ENGINEPATTERN %v");
    auto app = std::make_shared<spdlog::logger>("xop",
                                                spdlog::sinks_init_list{p});
    app->set_level(spdlog::level::debug);
    spdlog::set_default_logger(app);

    auto log = get_or_create_client_logger("tibetswap");
    ASSERT_NE(log, nullptr);

    // [review round 2] Positive assertions FIRST.  Without them this test
    // asserted only that the probe's formatter was unchanged, which is
    // trivially true when the adapter never attached to the probe at all --
    // e.g. because a stale "tibetswap" registration from test_tibetswap.cpp
    // was handed back instead.  The pattern check below is only meaningful
    // once we know `log` is actually writing to `p`.
    ASSERT_EQ(log->sinks().size(), 1u)
        << "the adapter returned a logger that is not on the process sinks";
    EXPECT_EQ(log->sinks()[0].get(), p.get());

    app->info("engine line");
    const auto formatted = p->last_formatted();
    ASSERT_EQ(formatted.size(), 1u);
    EXPECT_NE(formatted[0].find("ENGINEPATTERN"), std::string::npos)
        << "constructing an RPC client rewrote the engine's log format";
}

TEST_F(ClientLoggerTest, ANullDefaultLoggerIsSurvivedRatherThanDereferenced)
{
    // spdlog::shutdown() / drop_all() reset the default logger
    // (registry-inl.h:200), and main.cpp calls shutdown() on four paths.
    // default_logger() has no null substitution, so a helper that reached
    // straight for ->sinks() would be a null deref, not an empty list.
    //
    // [review round 2] The null window is kept to the single call.  While
    // default_logger_ is null, any free spdlog::info()/warn() from ANY thread
    // -- e.g. one left running by an earlier asio-based test -- goes through
    // default_logger_raw() with no null substitution and access-violates.
    // Nothing does that today; there is no reason to leave the window open for
    // the length of a test body anyway.
    spdlog::set_default_logger(nullptr);
    auto log = get_or_create_client_logger("coingecko");
    spdlog::set_default_logger(saved_);

    ASSERT_NE(log, nullptr);
    EXPECT_FALSE(log->sinks().empty());
}

// -- The dead man's switch: a duplicate name must not throw -----------------

TEST_F(ClientLoggerTest, TheSameNameTwiceReturnsOneSharedLoggerAndNeverThrows)
{
    // Engine::watchdog_cancel_book() constructs a SECOND ChiaWalletRPC -- same
    // logger name -- inside the try whose catch is "DEAD MAN'S SWITCH FAILED
    // BEFORE IT COULD CANCEL".  spdlog::register_logger throws spdlog_ex on a
    // duplicate name (registry-inl.h:253-263), and that throw would land
    // before the offers are cancelled, wedging a live book because of a
    // logging change.
    //
    // Sharing one logger is also what makes tibetswap's free parse_log() and
    // its client agree on a destination.
    auto p = probe();
    auto app = std::make_shared<spdlog::logger>("xop",
                                                spdlog::sinks_init_list{p});
    app->set_level(spdlog::level::debug);
    spdlog::set_default_logger(app);

    std::shared_ptr<spdlog::logger> first;
    std::shared_ptr<spdlog::logger> second;
    ASSERT_NO_THROW(first  = get_or_create_client_logger("chia.wallet"));
    ASSERT_NO_THROW(second = get_or_create_client_logger("chia.wallet"));

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first.get(), second.get())
        << "two loggers for one name split the client's output in two";
}

// -- A registered logger is not automatically a CURRENT one -----------------

TEST_F(ClientLoggerTest, AStaleRegistrationIsRebuiltAgainstTheProcessSinks)
{
    // [review round 2] THE REGRESSION THAT REINSTATES THE ORIGINAL DEFECT.
    //
    // A registered name used to be returned unconditionally.  So any client
    // constructed before init_logging() installs the real logger -- which is
    // exactly what test_tibetswap.cpp does five times, and what any future
    // namespace-scope or pre-init client would do in production -- registered
    // a stdout-only, level::info logger and PINNED that name to it for the
    // rest of the process.  init_logging() would then attach the rotating file
    // sink to "xop" and the client would keep writing to stdout at info: the
    // original bug, restored, with the fix apparently in place.
    auto stale = std::make_shared<spdlog::logger>(
        "dexie", spdlog::sinks_init_list{probe()});
    stale->set_level(spdlog::level::info);
    spdlog::register_logger(stale);

    // Now the real process logger arrives, as init_logging() does.
    auto file = probe();
    auto app  = std::make_shared<spdlog::logger>("xop",
                                                 spdlog::sinks_init_list{file});
    app->set_level(spdlog::level::debug);
    app->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(app);

    auto log = get_or_create_client_logger("dexie");
    ASSERT_NE(log, nullptr);
    EXPECT_NE(log.get(), stale.get())
        << "the client is pinned to a logger that predates the file sink";
    ASSERT_EQ(log->sinks().size(), 1u);
    EXPECT_EQ(log->sinks()[0].get(), file.get())
        << "the rebuilt logger is not on the process sinks";
    EXPECT_EQ(log->level(), spdlog::level::debug)
        << "the stale level::info would drop every ->debug() diagnosis";

    // And the replacement must be what the NEXT lookup finds, or the free
    // helpers (tibetswap's parse_log) still reach the stale one.
    auto again = spdlog::get("dexie");
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again.get(), log.get());

    log->debug("DIAGNOSIS after the process logger was installed");
    EXPECT_EQ(payload_at(file, 0),
              "DIAGNOSIS after the process logger was installed");
}

TEST_F(ClientLoggerTest, ADegradedStdoutFallbackDoesNotPinTheClientForever)
{
    // The same hole, reached from the other direction: rung 3 registers a
    // stdout-only logger when there is nothing usable to inherit.  If rung 1
    // then returned it unconditionally, one degraded construction would revert
    // that client to the pre-fix behaviour permanently and silently.
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("xop", spdlog::sinks_init_list{}));

    auto degraded = get_or_create_client_logger("coingecko");
    ASSERT_NE(degraded, nullptr);
    EXPECT_FALSE(degraded->sinks().empty());

    // The process recovers: a real logger with a file sink is installed.
    auto file = probe();
    auto app  = std::make_shared<spdlog::logger>("xop",
                                                 spdlog::sinks_init_list{file});
    app->set_level(spdlog::level::debug);
    spdlog::set_default_logger(app);

    auto healthy = get_or_create_client_logger("coingecko");
    ASSERT_NE(healthy, nullptr);
    EXPECT_NE(healthy.get(), degraded.get())
        << "one degraded construction pinned the client to stdout for good";
    ASSERT_EQ(healthy->sinks().size(), 1u);
    EXPECT_EQ(healthy->sinks()[0].get(), file.get());
}

TEST_F(ClientLoggerTest, ACurrentRegistrationIsSharedRatherThanRebuilt)
{
    // The negative control for the two tests above, and the property the dead
    // man's switch and tibetswap's parse_log() both depend on: when the
    // registered logger IS on the current process sinks, it must be handed
    // back by identity.  A rebuild-always adapter would satisfy "reaches the
    // file" while splitting one client's output across two logger objects with
    // two independent flush policies.
    auto p   = probe();
    auto app = std::make_shared<spdlog::logger>("xop",
                                                spdlog::sinks_init_list{p});
    app->set_level(spdlog::level::debug);
    spdlog::set_default_logger(app);

    auto current = std::make_shared<spdlog::logger>(
        "chia.wallet", spdlog::sinks_init_list{p});
    spdlog::register_logger(current);

    auto log = get_or_create_client_logger("chia.wallet");
    EXPECT_EQ(log.get(), current.get())
        << "a logger already on the process sinks was needlessly replaced";
}

// [review round 2] THE guard against a throw out of a client constructor, and
// the reason the test below can only be a second line of defence.
//
// Measured this round: with the adapter reverted to the pre-fix
// spdlog::get()/stdout_color_mt() body and `noexcept` LEFT IN PLACE, the
// concurrency test below exits the whole binary with code 3 and prints no
// [  FAILED  ] line -- the loser of the race throws spdlog_ex out of a
// noexcept function, which is std::terminate at the function's own boundary.
// No try/catch in the calling thread can intercept that, so the test-side
// catch added below does NOT rescue this case; only keeping `noexcept` plus
// the adapter's internal catch-all does.  A compile-time assertion is
// therefore the right place to pin it.
static_assert(noexcept(get_or_create_client_logger(std::string_view{})),
              "get_or_create_client_logger must stay noexcept: it is called "
              "from five RPC client constructors, one of them inside the S31 "
              "dead man's switch, where a throw lands before the offers are "
              "cancelled and wedges a live book.");

TEST_F(ClientLoggerTest, ConcurrentFirstUseOfOneNameNeitherThrowsNorSplits)
{
    // get-then-register is a TOCTOU: spdlog::get and register_logger take the
    // registry mutex separately, so two threads can both observe "absent" and
    // both construct.  Construction is single-threaded today, but the free
    // parse_log() has no such guarantee written down anywhere, and the loser
    // of the race would throw out of a constructor.
    auto p = probe(64);
    auto app = std::make_shared<spdlog::logger>("xop",
                                                spdlog::sinks_init_list{p});
    app->set_level(spdlog::level::debug);
    spdlog::set_default_logger(app);

    // A start gate, so the threads actually collide on the get-then-register
    // window instead of trickling through it one at a time.  This still cannot
    // GUARANTEE the interleaving -- a race test that passes proves the absence
    // of nothing.  It is here to catch the throw, not to certify it away.
    constexpr int    kThreads = 8;
    std::atomic<bool> go{false};

    std::vector<std::shared_ptr<spdlog::logger>> got(kThreads);
    std::vector<std::exception_ptr>              threw(kThreads);
    std::vector<std::thread>                     workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([i, &got, &threw, &go] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // [review round 2] An exception escaping a std::thread body calls
            // std::terminate: the binary dies mid-test with no [ FAILED ]
            // line, taking every test registered after it with it, and the
            // diagnosis is lost.  Capture and report instead.
            //
            // Honest limit: get_or_create_client_logger is declared noexcept,
            // so a throw from INSIDE it terminates at its own noexcept
            // boundary before this catch can run.  This guard covers the rest
            // of the lambda and survives a future refactor that drops the
            // noexcept -- it is not a substitute for the noexcept.
            try {
                got[static_cast<std::size_t>(i)] =
                    get_or_create_client_logger("race");
            } catch (...) {
                threw[static_cast<std::size_t>(i)] = std::current_exception();
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }

    for (int i = 0; i < kThreads; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        EXPECT_EQ(threw[idx], nullptr)
            << "thread " << i << " threw out of a client constructor";
        ASSERT_NE(got[idx], nullptr) << "a racing thread got nothing back";
        EXPECT_FALSE(got[idx]->sinks().empty());
        // [review round 2] The "NorSplits" half of this test's own name.
        // Without it, a build in which every thread registers its own private
        // logger -- the exact failure the name describes -- passes.
        EXPECT_EQ(got[idx].get(), got[0].get())
            << "the race split one client's output across two loggers";
    }
}

// -- The call site, not just the helper -------------------------------------

TEST_F(ClientLoggerTest, TheRealClientConstructorInheritsTheProcessSinks)
{
    // The helper being correct does not make the CALL SITES correct, and
    // nothing else in cpp/tests drives a production client constructor.
    // CoinGeckoClient is safe here: the ctor opens no sockets, CoinGeckoConfig
    // is fully defaulted with enabled{false}, and ~CoinGeckoClient -> close()
    // returns early on a never-opened client.
    auto p = probe(32);
    auto app = std::make_shared<spdlog::logger>("xop",
                                                spdlog::sinks_init_list{p});
    app->set_level(spdlog::level::debug);
    app->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(app);

    boost::asio::io_context ioc;
    xop::rpc::CoinGeckoClient client{ioc, xop::CoinGeckoConfig{}};

    // The constructor registers under its own name; that logger must be the
    // one writing to the process sinks, at the process level.
    auto built = spdlog::get("coingecko");
    ASSERT_NE(built, nullptr)
        << "the production ctor did not put its logger where the free "
           "parsing helpers and the second client can find it";
    ASSERT_EQ(built->sinks().size(), 1u);
    EXPECT_EQ(built->sinks()[0].get(), p.get())
        << "the client is writing somewhere other than the process sinks";
    EXPECT_EQ(built->level(), spdlog::level::debug)
        << "inherited level::info would drop the client's debug diagnoses";

    built->debug("DIAGNOSIS from the real client's logger");
    EXPECT_FALSE(p->last_raw().empty())
        << "the client's records never reached the process sinks";
}
