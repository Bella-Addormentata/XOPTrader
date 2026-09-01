#ifndef XOP_UTIL_CLIENT_LOGGER_HPP
#define XOP_UTIL_CLIENT_LOGGER_HPP
// ---------------------------------------------------------------------------
// client_logger.hpp -- one logger construction for every RPC client, and the
// refusal that stops it from becoming a silent one.
//
// [CLIENTLOG 2026-09-01] The four RPC clients logged to STDOUT ONLY. Measured
// in the live corpus on 2026-09-01: the engine's SYMPTOM line appears 1,743
// times in logs/xop_trader.log while the RPC clients' DIAGNOSIS lines appear
// ZERO times. Every RPC-layer fault was invisible in the only log that
// survives a restart -- the file the operator actually reads during a
// post-mortem.
//
// Every file:line citation below was re-derived against the POST-change tree
// on 2026-09-01 (review round 2). The first round's citations were written
// against HEAD and were invalidated by this commit's own comment insertions;
// if you are editing this header, re-derive them again before you commit.
//
// THE DEFECT
// ----------
// All five construction sites ran
//
//     log_ = spdlog::get("<name>");
//     if (!log_) log_ = spdlog::stdout_color_mt("<name>");
//
// The premise "spdlog::get() always returns nullptr" is only half true, and
// the half that is false is the dangerous half:
//
//   * stdout_color_mt() SELF-REGISTERS (details/synchronous_factory.h:15-20 ->
//     registry-inl.h:81-83, automatic_registration_ defaults true). So the
//     FIRST construction of a name registers a stdout-only logger and every
//     later spdlog::get() of that name finds it. That is why the second
//     ChiaWalletRPC in Engine::watchdog_cancel_book() -- the dead man's switch
//     -- does not throw today, and why tibetswap_client.cpp's free parse_log()
//     shares the client's logger. Both behaviours must be PRESERVED, not just
//     the sinks.
//
//   * main.cpp's init_logging() installs the real logger under the name "xop"
//     with a stdout_color_sink_mt AND a rotating_file_sink_mt. set_default_
//     logger() registers it (registry-inl.h:105-111) -- under "xop", never
//     under "dexie". The client names were therefore never wired to the file.
//
//   * The factory also assigns the registry's global level, which is
//     level::info (registry.h:116), so the 14 ->debug() call sites across
//     cpp/src/rpc/ were rejected in logger::log_ BEFORE FORMATTING
//     (logger.h:317-322). They emitted nowhere at all -- not even to stdout.
//     Attaching the file sink without also fixing the LEVEL would have left
//     them silent. Both halves are required.
//
// WHY TWO FUNCTIONS
// -----------------
// make_client_logger() is PURE: it takes the sink list as a parameter and
// touches no global state, so cpp/tests/test_client_logger.cpp can drive it
// with ringbuffer probes and assert what actually arrives. That is the repo's
// pure-header convention (xop/strategy/tier_gain.hpp,
// xop/execution/cross_guard.hpp, xop/rpc/coingecko_parse.hpp) and it exists
// because NOTHING in cpp/tests constructs an Engine -- which is how several
// regressions here survived multiple review rounds.
//
// get_or_create_client_logger() is the thin registry-aware adapter the six
// call sites use. It is the only part that reads process state.
//
// THE REFUSAL -- DO NOT FAIL OPEN
// -------------------------------
// spdlog will happily build a logger over ZERO sinks (logger.h:53-55). Such a
// logger accepts every record, iterates an empty sinks_ in sink_it_
// (logger-inl.h:136) and discards everything with no error, no warning and no
// error-handler call. That is STRICTLY WORSE than the stdout-only status quo
// this header replaces, because stdout at least reaches an operator watching
// the console.
//
// This repo has a documented family of ten fail-open bugs and the rule that
// came out of it: refuse the bad input, do not paper over it. So
// make_client_logger() returns NULLPTR on an empty sink list -- and on a list
// containing a null element, which is a crash on the first record rather than
// a silence. The refusal is only safe because the CALLER then falls back to
// something that writes: get_or_create_client_logger() walks a ladder whose
// first three rungs all have a real destination, and never hands back the
// silent logger it just refused. Rung 4 is the honest exception; see its own
// note in the ladder documentation.
//
// A REGISTERED LOGGER IS NOT AUTOMATICALLY A CURRENT ONE
// ------------------------------------------------------
// [review round 2] The first version of this adapter returned ANY registered
// logger of that name unconditionally. That reinstates the very defect it
// fixes, permanently and with no diagnostic, in two reachable ways:
//
//   * A client constructed BEFORE init_logging() installs the real logger
//     inherits spdlog's built-in default -- one stdout sink, level::info --
//     and registers it. init_logging() then installs the "xop" logger with the
//     file sink, and every later lookup of that name short-circuits to the
//     stdout-only, info-level logger. All 14 ->debug() sites stay silent.
//     Not reachable from main.cpp today (init_logging() runs before the Engine
//     and therefore before any client), but the ordering was load-bearing and
//     undocumented -- and it IS reachable inside xop_tests.exe, where
//     test_tibetswap.cpp constructs five TibetSwapClients with no real default
//     logger installed.
//
//   * Rung 3 registers a degraded stdout-only logger. One degraded
//     construction would have pinned that client to the pre-fix behaviour for
//     the remaining life of the process.
//
// So rung 1 now accepts a registered logger only when its sink list is
// IDENTICAL BY POINTER to the current process logger's. Otherwise the name is
// rebuilt against the current sinks and replaced. The comparison is
// vector<shared_ptr<sink>> operator==, i.e. element-wise pointer identity, and
// it runs on a path taken once per client construction plus tibetswap's
// malformed-payload branches -- never on a hot path.
//
// Two properties are deliberately preserved through that change: a duplicate
// name never throws (the dead man's switch depends on it) and two callers with
// the same name and the same process logger get the SAME object (tibetswap's
// free parse_log() and its client must agree on a destination).
//
// LEVEL -- INHERIT, DO NOT FORCE
// ------------------------------
// The wrapper takes the level from spdlog::default_logger()->level() rather
// than hardcoding level::debug. Hardcoding would work today (main.cpp:351 sets
// debug) and would silently diverge the moment anyone edits init_logging().
// Inheriting is also what the three subsystems that already got this right do
// via clone(): cpp/src/execution/coin_manager.cpp:64,
// cpp/src/execution/offer_manager.cpp:85,
// cpp/src/monitoring/on_chain_reconciler.cpp:45.
//
// PATTERN -- IT LIVES ON THE SINK, SO DO NOT SET IT
// -------------------------------------------------
// logger::set_pattern() does NOT configure the logger. It walks sinks_ and
// reformats each sink (logger-inl.h:72-82). Because a client logger SHARES the
// sink objects with the engine's "xop" logger, calling set_pattern() here
// would silently rewrite the format of logs/xop_trader.log for the whole
// process. The `pattern` parameter therefore means "" == INHERIT, and the
// wrapper always passes "". Sharing the sink object is how pattern intent is
// inherited: the formatter is already on the sink. Pass a non-empty pattern
// ONLY for sinks you own exclusively (i.e. from a test).
//
// For the same reason this header never routes through spdlog::create<>(),
// stdout_color_mt() or spdlog::initialize_logger(): all three reach
// registry::initialize_logger, which does set_formatter(formatter_->clone())
// at registry-inl.h:64 and would clobber main.cpp:360's pattern on the shared
// sinks. spdlog::register_logger() and register_or_replace() are safe --
// register_logger_ does the exists-check and the map insert
// (registry-inl.h:259-263) and register_or_replace_ does the insert alone
// (registry-inl.h:265-267); neither touches the formatter or the level.
//
// THE S31 DEAD MAN'S SWITCH SHARES THESE SINKS -- BY DESIGN, NOT BY ACCIDENT
// --------------------------------------------------------------------------
// [review round 2] Engine::watchdog_cancel_book() constructs a SECOND
// ChiaWalletRPC on its own thread and its own io_context, and
// ChiaRPCBase::open() emits three records (chia_rpc.cpp:278, :293, :298)
// BEFORE cancel_offers(). Those records now go through the engine's rotating
// file sink, so the watchdog can block on base_sink<std::mutex>::mutex_
// (base_sink-inl.h:26-29) while the engine thread is inside rotate_().
//
// What is NOT true: that this coupling is new. Pre-change, the second
// ChiaWalletRPC's spdlog::get("chia.wallet") found the logger the engine's own
// wallet_ had already registered -- the same logger object, hence the same
// wincolor_stdout_sink_mt and the same mutex. The two clients were never
// isolated. Engine::start_watchdog()'s "shares nothing with the engine except
// two atomics" was already inaccurate about logging; that comment has been
// amended in the same commit rather than left to mislead.
//
// What IS new, and the honest cost: the ROTATING sink can hold its mutex far
// longer than a console write. rotate_() (rotating_file_sink-inl.h:138-166)
// walks max_files_ down to 1 doing a remove+rename each, and a failed rename
// sleeps 100 ms under the lock and retries once. With kMaxFiles raised to 9
// that is a worst case of ~900 ms, up from ~500 ms at 5. Against
// risk.watchdog_stall_seconds, whose default is 600 s (config.hpp:1337), a
// bounded ~1 s is 0.15% of the stall the switch already tolerated, and the
// cancel it delays is a network round trip. Accepted.
//
// The unbounded case -- a stalled volume wedging rotate_() forever -- would
// wedge the watchdog too. That is a real new failure mode and it has no cheap
// fix: rung 2 adopts the process sinks BY IDENTITY for every name, so giving
// the watchdog a distinct logger name would NOT detach it from the file sink;
// only injecting a stdout-only logger would, and that would make the one
// component whose diagnosis matters most during an incident invisible in the
// only log that survives the restart. Sharing was chosen knowingly.
//
// LOG VOLUME -- THE COST, MEASURED, AND WHAT WAS DONE ABOUT IT
// ------------------------------------------------------------
// Routing client records into the rotating file is not free, and the honest
// number is large enough that it had to be paid for in the same commit.
//
// Baseline, re-measured 2026-09-01T16:30 from logs/xop_trader.log: 9,129,988
// bytes over 22,163 s (10:20:45 -> 16:30:08, 818 engine cycles, 62,602 lines,
// 146 B/line) = 411.9 B/s = 35.6 MB/day. Rotation cadence from the mtimes of
// xop_trader.5/.4/.3/.2/.1.log is 6h52m / 7h34m / 7h30m / 7h10m per 10 MB.
// Retention was 6 files x 10 MB = 60 MB = ~40 h.
//
// New traffic at info and above, counted per calendar day from the stdout
// capture cpp/build/Release/engine.log (dexie + tibetswap + coingecko +
// chia.wallet + chia.fullnode): 2026-08-30 3.45 MB, 2026-08-31 1.78 MB,
// 2026-09-01 2.00 MB over 16.5 h = 2.9 MB extrapolated. Call it 1.8-3.5
// MB/day, central ~2.7. Cheap: +8%.
//
// The expensive part is the DEBUG tier, which today emits nowhere and becomes
// visible for the first time. It CANNOT be measured from the corpus, because
// the defect being fixed is exactly that it produces no records anywhere -- so
// what follows is a bottom-up ESTIMATE and must be treated as one. The
// dominant term is chia_rpc.cpp:540 and :629, two debug lines per wallet or
// full-node RPC attempt, a tier with no info-level proxy to count from. At
// 3,192 engine cycles/day (measured: 818 cycles over 6h09m) and 8-20 chia RPCs
// per cycle that is 26k-64k RPCs/day -> 51k-128k lines/day; with dexie's
// per-request pair (dexie_client.cpp:402,466) and the tibetswap/coingecko
// pairs the total lands at +13 to +25 MB/day.
//
// New total ~51-64 MB/day, central ~57. At the old 60 MB retention the central
// case is 25 h -- BELOW the overnight line. The repo's own memory has two
// overnight incidents (the frozen oracle, batch_failed) whose post-mortems
// needed more history than that, so silently accepting it was not an option.
//
// DECISION: inherit the level (full debug fidelity, which is the point of the
// change) and pay for it with retention. main.cpp:343 kMaxFiles 5 -> 9, i.e.
// 10 files x 10 MB = 100 MB, giving ~42 h central and 37-47 h across the range
// -- slightly more history than before the change, for +40 MB of disk on a
// volume with 1.1 TB free. DO NOT QUOTE 42 h AS AN OPERATING FIGURE: the
// debug term is an estimate. Re-measure the rotation cadence one day after the
// next restart and correct this block.
//
// The retention increase is not free on the rotation side either, and that
// cost lands on the trading thread rather than on disk: 9 renames per rotation
// instead of 5, a worst-case sink-mutex hold of ~900 ms instead of ~500 ms
// (see the S31 note above), and -- because the rotation interval also drops
// from ~7h16m to ~4h -- roughly 3x as many rotations per day, hence ~1.8x the
// per-day count of chances to hit spdlog's rename-failure path, which calls
// file_helper_.reopen(true) and TRUNCATES the live log
// (rotating_file_sink-inl.h:156-161). On Windows a rename fails when any other
// handle holds the file, and this repo's memory records exactly that class of
// wedge. The standing rule (grep/sed/head only, never a follow-mode read of
// logs/*.log) is now ~3x more expensive to violate.
//
// A related, cheaper cost was also paid: main.cpp:360's pattern gained [%n].
// Without it the file log cannot say WHICH client spoke -- the four client
// names would be as invisible as CoinMgr/OfferMgr/OnChainReconciler are today
// (grep -c "\[CoinMgr\]" logs/xop_trader.log == 0, because they clone the
// default logger and the pattern has no %n). It also preserves the [dexie]
// tag operators grep in engine.log, which sharing the engine's pattern would
// otherwise have removed. Cost: 6 B x 62,602 lines / 6.15 h = 1.4 MB/day (4%),
// already inside the retention figure above.
//
// WHAT THIS DOES NOT PROMISE
// --------------------------
//   * Not "the diagnosis reaches disk". logger::sink_it_ wraps every
//     sink->log() in SPDLOG_LOGGER_CATCH (logger-inl.h:138-139); if the
//     rotating sink throws -- disk full, a rename failure during rotation --
//     the record is DROPPED and at most one line per second reaches stderr.
//     spdlog does not guarantee delivery and neither does this.
//   * Not immediate durability. flush_on is inherited as warn (main.cpp:361),
//     so a debug record sits in the sink's buffer until some logger emits a
//     warn-or-above or the process exits cleanly. A hard crash loses it.
//   * Not cross-sink ordering. sink_it_ holds no logger-level lock; console
//     and file order can differ under concurrency. Records are individually
//     intact. Do not write a test that asserts cross-sink ordering.
//   * The inherited flush_on(warn) does mean dexie's per-retry warn
//     (dexie_client.cpp:450,501) now costs a file flush during a dexie outage
//     -- synchronous IO precisely when the venue is degraded. Accepted: the
//     flush is an fflush on an already-open handle, retries are bounded by
//     max_retries, and losing the outage's own diagnosis to a buffer is the
//     worse failure. Tunable by passing a higher flush_level.
//
// Pure header, no engine types, spdlog only, NO globals and NO function-local
// statics -- a cached namespace-scope shared_ptr here would outlive
// spdlog::shutdown() (main.cpp:464,619,670,743) and flush a rotating sink
// after curl_global_cleanup(). All state in, logger out.
// ---------------------------------------------------------------------------

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace xop::util {

// ---------------------------------------------------------------------------
// The pure half.
// ---------------------------------------------------------------------------

/// Build a logger named @p name over EXACTLY the sinks in @p sinks.
///
/// The sink shared_ptrs are adopted by identity -- the returned logger writes
/// through the very same sink objects the caller passed, which is what makes
/// a client logger reach the file main.cpp already opened. It does not clone,
/// rebuild or reopen anything, and it never touches the spdlog registry.
///
/// @param name         logger name, reported by %n and by log_msg::logger_name.
/// @param sinks        the destinations. MUST be non-empty and contain no
///                     null element -- see the refusal note below.
/// @param level        the logger's own gate. Records below it are dropped in
///                     logger::log_ before any sink is consulted, so this
///                     must be <= the lowest level you expect to see.
/// @param pattern      "" means INHERIT (leave every sink's formatter alone).
///                     A non-empty value MUTATES EVERY SINK IN @p sinks, which
///                     for shared sinks reformats other loggers' output too.
/// @param flush_level  the logger's auto-flush threshold. Per-logger, NOT
///                     inherited by sharing sinks (logger.h:311).
///
/// @return the logger, or NULLPTR if @p sinks is empty or holds a null entry.
///         Returning nullptr is deliberate: a logger over zero sinks discards
///         every record in silence, which is the fail-open shape this repo has
///         been bitten by ten times. The caller must fall back to something
///         that writes -- never to a second silent logger.
inline std::shared_ptr<spdlog::logger>
make_client_logger(std::string_view                     name,
                   const std::vector<spdlog::sink_ptr>& sinks,
                   spdlog::level::level_enum            level,
                   std::string_view                     pattern,
                   spdlog::level::level_enum            flush_level)
{
    // -- Refusals -----------------------------------------------------------
    // Empty: silent by construction. Null entry: logger::sink_it_ dereferences
    // every element with no null check, so this is a crash on the first record
    // on whatever thread reaches it first.
    if (sinks.empty()) {
        return nullptr;
    }
    for (const auto& s : sinks) {
        if (!s) {
            return nullptr;
        }
    }

    auto built = std::make_shared<spdlog::logger>(
        std::string{name}, sinks.begin(), sinks.end());

    built->set_level(level);
    built->flush_on(flush_level);

    // Only when the caller owns these sinks exclusively. See the header note.
    if (!pattern.empty()) {
        built->set_pattern(std::string{pattern});
    }

    return built;
}

// ---------------------------------------------------------------------------
// The registry-aware half -- what the six call sites actually call.
// ---------------------------------------------------------------------------

/// Return the logger the RPC client @p name should use.
///
/// Ladder, best rung first:
///
///   1. an already-registered logger of that name, PROVIDED its sinks are the
///      current process logger's sinks by pointer identity (or there is no
///      usable process logger to compare against, in which case the
///      registered one is the best available). This is what preserves the two
///      live behaviours that must not change: the second ChiaWalletRPC in
///      Engine::watchdog_cancel_book() -- the dead man's switch -- must not
///      throw, and tibetswap's free parse_log() must share the client's
///      logger. A registered logger whose sinks are STALE is not returned; it
///      is rebuilt at rung 2 and replaced, which is what stops one early or
///      degraded construction from pinning the client to the pre-fix
///      stdout-only behaviour for the life of the process.
///   2. a new logger over spdlog::default_logger()'s sinks, inheriting its
///      level and flush level, then registered so rung 1 finds it next time.
///   3. a fresh stdout colour sink -- exactly the pre-fix behaviour. Reached
///      when the default logger is null (spdlog::shutdown() resets it,
///      registry-inl.h:200) or has no usable sinks. Degraded, never silent.
///   4. the process logger itself, WITHOUT a sink check.
///
/// Rungs 1-3 always have a real destination. Rung 4 is the honest exception
/// and its contract is weaker on purpose: it is reached only from the outer
/// catch, i.e. after an allocation failure inside rungs 1-3, and at that point
/// building one more sink to check against is no more likely to succeed than
/// the allocation that just failed. If the process logger is itself sinkless
/// then rung 4 does return a silent logger. That is worse than rungs 1-3 and
/// better than the alternative: the 81 unguarded `logger_->` / `log_->`
/// dereferences in cpp/src/rpc/ turn a nullptr into an access violation, and a
/// crash inside an RPC client constructor is not an improvement on silence in
/// a process that is already failing to allocate. Do not "fix" rung 4 by
/// returning nullptr without first guarding those 81 sites.
///
/// Never throws -- a client constructor is not a place to raise. In particular
/// the get-then-register sequence is a TOCTOU (spdlog::get and register_logger
/// take the registry mutex separately), and register_logger THROWS spdlog_ex on
/// a duplicate name (registry-inl.h:253-263). Losing that race is handled by
/// taking the winner, not by propagating.
///
/// @return never nullptr in any recoverable state. It can only be null if the
///         registry has been shut down AND sink allocation is also failing, at
///         which point the process is already unrecoverable.
inline std::shared_ptr<spdlog::logger>
get_or_create_client_logger(std::string_view name) noexcept
{
    try {
        // Inside the try: this allocates, and this function is noexcept, so a
        // bad_alloc out here would std::terminate at the noexcept boundary --
        // bypassing rung 4, which exists for exactly that input.
        const std::string key{name};

        // Read the process logger ONCE. Every rung below compares against this
        // same snapshot, so a concurrent set_default_logger() cannot make rung
        // 1 accept sinks that rung 2 would then have rejected.
        auto app = spdlog::default_logger();

        // -- 1. Already registered, and still pointed at the process sinks ---
        if (auto existing = spdlog::get(key)) {
            if (!app || app->sinks().empty() ||
                existing->sinks() == app->sinks()) {
                return existing;
            }
            // Stale: registered against sinks that are no longer the process's
            // (registered before init_logging(), or by rung 3 while the
            // process logger was unusable). Fall through and REPLACE it,
            // rather than pinning this client to the pre-fix behaviour.
        }

        // -- 2. Adopt the process logger's sinks ----------------------------
        // default_logger() is resettable and CAN be null; do not dereference
        // it blind. Its sinks() may also be empty, which make_client_logger
        // refuses -- that refusal is the whole point, and it falls through to
        // rung 3 rather than returning something silent.
        if (app) {
            if (auto built = make_client_logger(key,
                                                app->sinks(),
                                                app->level(),
                                                std::string_view{},
                                                app->flush_level())) {
                try {
                    spdlog::register_logger(built);
                    return built;
                } catch (const spdlog::spdlog_ex&) {
                    // The name is taken. Two different situations reach here
                    // and they want opposite outcomes:
                    //
                    //  - we lost the get-then-register race, and the winner is
                    //    already current -> take the winner, so every user of
                    //    this name shares ONE logger, as they do today;
                    //  - we are here to replace a stale registration -> the
                    //    winner is not current, so replacing is the point.
                    if (auto winner = spdlog::get(key)) {
                        if (winner->sinks() == app->sinks()) {
                            return winner;
                        }
                    }
                    try {
                        spdlog::register_or_replace(built);
                    } catch (...) {
                        // Unregistered is still sound: `built` shares the
                        // process sinks, so it is flushed via "xop".
                    }
                    return built;
                }
            }
        }

        // -- 3. The pre-fix behaviour, as an explicit fallback ---------------
        // Reached only when there is nothing usable to inherit. Degraded (no
        // file sink) but it WRITES, which the refused logger would not.
        // register_or_replace, not register_logger: this rung is the one that
        // can be re-entered after a shutdown/drop, and it must not throw.
        // Registering here is now safe to do -- rung 1's identity check means
        // a later, healthy call rebuilds this name against the real sinks
        // instead of returning this degraded logger forever.
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        if (auto built = make_client_logger(key,
                                            {console},
                                            spdlog::level::debug,
                                            std::string_view{},
                                            spdlog::level::warn)) {
            try {
                spdlog::register_or_replace(built);
            } catch (...) {
                // Unregistered is fine; the caller holds the only reference.
            }
            return built;
        }
    } catch (...) {
        // Fall through to rung 4.
    }

    // -- 4. Last resort -------------------------------------------------------
    // Only allocation failure gets here. Hand back the process logger rather
    // than null -- see the weaker contract documented above the function.
    try {
        return spdlog::default_logger();
    } catch (...) {
        return nullptr;
    }
}

}  // namespace xop::util

#endif  // XOP_UTIL_CLIENT_LOGGER_HPP
