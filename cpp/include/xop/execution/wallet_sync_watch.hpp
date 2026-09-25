#ifndef XOP_EXECUTION_WALLET_SYNC_WATCH_HPP
#define XOP_EXECUTION_WALLET_SYNC_WATCH_HPP
// ---------------------------------------------------------------------------
// wallet_sync_watch.hpp -- when may the engine restart an unsynced wallet?
//
// [WALLET-RESTART-LIVELOCK 2026-09-22] Step 8 used to restart the Chia wallet
// service after 20 consecutive unsynced heartbeats (the comment said "~3 min";
// on 2026-09-22 it fired every 6-10 minutes).  That restart is what kept the
// wallet from ever syncing.  From Chia 2.7.4's wallet_node.py:
//
//   * A long sync records its progress ONLY WHEN IT COMPLETES ("Only update
//     this fully when the entire sync has completed":
//     set_finished_sync_up_to(target_height) is its last step).  Killing it
//     midway throws all of its work away.
//   * With use_delta_sync: false (this installation's setting) a long sync
//     re-subscribes every puzzle hash and coin id from height 0.  For a wallet
//     with this one's history that takes longer than the old escalation
//     allowed.
//   * A freshly started wallet process has no synced peers, so its first peak
//     from the trusted node starts a long sync with rollback=True from
//     finished_sync_up_to - 256.  EVERY RESTART THAT INTERRUPTS A SYNC THEREFORE
//     MOVES THE WALLET 256 BLOCKS BACKWARDS.
//
// So the escalation was a livelock that also ran the wallet in reverse.  On
// 2026-09-22 (boot 16:53) the wallet reported synced=false, syncing=true on
// every Step 8 heartbeat, the engine restarted it 9 times between 17:18 and
// 18:24, and at 18:29 its finished-sync height was 9,297,547 against a node
// peak of 9,329,985.  Step 8 managed no offers the whole time.
//
// The rules:
//
//   * A WALLET THAT SAYS IT IS SYNCING IS NOT RESTARTED until the whole
//     unsynced streak reaches the syncing budget.  Its only visible progress
//     is completion, so the budget is long: a sync that runs past it is
//     presumed hung.  [review round 4] The streak counts from the first
//     unsynced reading, not from the start of syncing, so any idle time
//     before the sync counts too -- at most the idle budget, since an idle
//     run that long is restarted first.  That is deliberate: a wallet that
//     keeps flipping between syncing and idle would otherwise never reach
//     a restart.
//   * A WALLET NEITHER SYNCED NOR SYNCING (not even trying -- e.g. no peer)
//     is restarted after the much shorter idle budget.  A sync attempt resets
//     that idle run.
//   * EACH RESTART DOUBLES BOTH BUDGETS for the next attempt, up to the cap,
//     and starts a fresh streak.  A wallet that needs longer than the budget
//     gets it on a later attempt instead of being killed forever.  A restart
//     command that FAILED takes its doubling back (record_failed_wallet_restart)
//     but keeps the fresh streak, so failures retry once per budget, never once
//     per heartbeat.  Reporting synced clears everything, the backoff included.
//   * A RESTART WHOSE START FAILED OWES A START.  [review round 6] The stop may
//     have worked, leaving no wallet to answer the sync check, so the watch
//     could never decide again.  The engine retries the start from its poll
//     loop, before any wallet or height call [round 7] (WalletStartDebt):
//     after 60 s, then at doubling intervals up to 15 minutes, until a start
//     succeeds or the wallet answers.  [round 7] A restart whose stop worked
//     then counts again: settling the debt gives back the doubling
//     record_failed_wallet_restart() took.
//   * TIME IS MEASURED, NOT COUNTED, on a monotonic clock.  A silence longer
//     than max_observation_gap_s (Step 8 not reached: paused, breaker open)
//     says nothing about the wallet, so it starts a new streak instead of
//     arriving as one enormous unsynced interval.
//   * THE OUTAGE IS NOT THE STREAK.  [review round 5] A restart starts a fresh
//     streak, so the streak cannot say how long the wallet was out: right
//     after a restart it is empty.  The watch also keeps the whole outage,
//     from the first unsynced reading since the wallet last reported synced,
//     through every restart.  A silence inside it makes its length unknown,
//     and the verdict says so instead of reporting a number.
//
// NOT DECIDED HERE: how the engine restarts the wallet, and what an unknown
// sync state means.  The engine treats a reply without `syncing` as syncing:
// an unread state never earns the short budget.
//
// Pure header: no I/O, no clock, no logging.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <optional>

namespace xop::execution {

/// Budgets, in seconds of continuous observation.
struct WalletRestartPolicy {
    std::int64_t idle_budget_s{900};           ///< unsynced and not syncing
    std::int64_t syncing_budget_s{7200};       ///< unsynced at all: a sync presumed hung
    std::int64_t max_budget_s{86400};          ///< cap on either budget after backoff
    std::int64_t max_observation_gap_s{600};   ///< a longer silence starts a new streak
    std::int64_t start_retry_base_s{60};       ///< [round 6] first retry of a failed wallet start
    std::int64_t start_retry_cap_s{900};       ///< [round 6] cap on the doubling retry delay
};

/// State carried between Step 8 heartbeats.
struct WalletSyncWatch {
    std::optional<std::int64_t> unsynced_since{};  ///< start of the current unsynced streak
    std::optional<std::int64_t> idle_since{};      ///< start of the current not-syncing run in it
    std::optional<std::int64_t> last_observed{};
    std::uint32_t               restarts{0};       ///< restarts since the wallet last reported synced
    std::uint32_t               failed_restarts{0};///< restart COMMANDS that failed, same period
    std::optional<std::int64_t> outage_since{};    ///< [round 5] first unsynced reading since then; restarts keep it
    bool                        outage_blurred{false}; ///< [round 5] a silence fell inside that outage
};

enum class WalletSyncAction : std::uint8_t {
    Synced,   ///< the wallet is synced; the watch is cleared
    Wait,     ///< unsynced, within budget
    Restart,  ///< unsynced past a budget: restart the wallet service now
};

/// Outcome of observe_wallet_sync().
struct WalletSyncVerdict {
    WalletSyncAction action{WalletSyncAction::Wait};
    std::int64_t     unsynced_for_s{0};    ///< length of the current unsynced streak
                                           ///< (on Synced: of the streak that ended)
    std::int64_t     idle_for_s{0};        ///< length of the current not-syncing run (0 while syncing)
    std::int64_t     idle_budget_s{0};     ///< budgets in force for this streak, after backoff
    std::int64_t     syncing_budget_s{0};
    std::uint32_t    restarts{0};          ///< restarts before this observation's decision
    /// [review round 5] On Synced: an unsynced outage just ended, and its whole
    /// length, restarts included.  Empty when a silence longer than
    /// max_observation_gap_s fell inside it, since its length is then unknown.
    bool                        outage{false};
    std::optional<std::int64_t> outage_s{};
};

/// `base` doubled once per restart, capped at `cap`; overflow-safe.
[[nodiscard]] constexpr std::int64_t wallet_restart_budget(
    std::int64_t base, std::uint32_t restarts, std::int64_t cap) noexcept
{
    std::int64_t budget = base;
    for (std::uint32_t i = 0; i < restarts && budget < cap; ++i) {
        budget = (budget > cap / 2) ? cap : budget * 2;
    }
    return budget < cap ? budget : cap;
}

/// Record one Step 8 sync-gate reading and decide.
/// @param synced   the wallet is synced (the engine's reading: synced && !syncing).
/// @param syncing  the wallet reports a sync in progress, or its state is unknown.
/// @param now_s    a monotonic clock, in seconds.
[[nodiscard]] inline WalletSyncVerdict observe_wallet_sync(
    WalletSyncWatch& watch, bool synced, bool syncing, std::int64_t now_s,
    const WalletRestartPolicy& policy = WalletRestartPolicy{}) noexcept
{
    if (watch.last_observed.has_value()
        && now_s - *watch.last_observed > policy.max_observation_gap_s) {
        watch.unsynced_since.reset();
        watch.idle_since.reset();
        // [review round 5] The outage goes on; only its length is lost.
        if (watch.outage_since.has_value()) {
            watch.outage_blurred = true;
        }
    }
    watch.last_observed = now_s;

    WalletSyncVerdict verdict{};
    verdict.restarts = watch.restarts;
    if (synced) {
        // The streak that just ended: what the budgets measured.
        verdict.unsynced_for_s = watch.unsynced_since.has_value()
                                     ? now_s - *watch.unsynced_since : 0;
        // [review round 5] And the whole outage, for the "re-synced after" log.
        verdict.outage = watch.outage_since.has_value();
        if (verdict.outage && !watch.outage_blurred) {
            verdict.outage_s = now_s - *watch.outage_since;
        }
        watch = WalletSyncWatch{};
        watch.last_observed = now_s;
        verdict.action = WalletSyncAction::Synced;
        return verdict;
    }

    if (!watch.unsynced_since.has_value()) {
        watch.unsynced_since = now_s;
    }
    if (!watch.outage_since.has_value()) {
        watch.outage_since = now_s;
    }
    if (syncing) {
        watch.idle_since.reset();
    } else if (!watch.idle_since.has_value()) {
        watch.idle_since = now_s;
    }

    verdict.unsynced_for_s   = now_s - *watch.unsynced_since;
    verdict.idle_for_s       = watch.idle_since.has_value() ? now_s - *watch.idle_since : 0;
    verdict.idle_budget_s    = wallet_restart_budget(policy.idle_budget_s, watch.restarts,
                                                     policy.max_budget_s);
    verdict.syncing_budget_s = wallet_restart_budget(policy.syncing_budget_s, watch.restarts,
                                                     policy.max_budget_s);

    const bool idle_spent    = watch.idle_since.has_value()
                            && verdict.idle_for_s >= verdict.idle_budget_s;
    const bool syncing_spent = verdict.unsynced_for_s >= verdict.syncing_budget_s;
    if (idle_spent || syncing_spent) {
        ++watch.restarts;
        watch.unsynced_since.reset();
        watch.idle_since.reset();
        verdict.action = WalletSyncAction::Restart;
    }
    return verdict;
}

/// The restart COMMAND failed (review, PR #170): a restart that did not
/// happen must not double the next budget, so this takes back the backoff
/// step observe_wallet_sync() took for it.  The streak stays reset, so the
/// next attempt still waits a full budget -- the budget this attempt had.
/// Restoring the old streak instead would retry on every heartbeat, a
/// blocking command each time, for as long as the command keeps failing.
inline void record_failed_wallet_restart(WalletSyncWatch& watch) noexcept
{
    if (watch.restarts > 0) {
        --watch.restarts;
    }
    ++watch.failed_restarts;
}

/// [review round 6] A wallet START still owed after a restart whose start
/// command failed.  The stop may well have worked, and then there is no wallet
/// to answer Step 8's sync check: the watch never observes again, and the
/// wallet circuit breaker skips Step 8 altogether.  So the retry cannot wait on
/// the watch.  [review round 7] The engine asks on every poll, before any
/// wallet or height call -- a heartbeat needs a height, which in wallet-only
/// mode comes from the wallet that is down -- and settles the debt when the
/// wallet answers.
struct WalletStartDebt {
    std::optional<std::int64_t> retry_at{};   ///< when the next start is due; empty: none owed
    std::uint32_t               attempts{0};  ///< start commands that failed since it was owed
    /// [review round 7] The restart's stop worked, so a restart did begin.
    /// record_failed_wallet_restart() took its doubling back when the start
    /// failed; settling the debt gives it back, once.
    bool                        restart_credit{false};
};

/// The restart's start command failed: a start is owed, first due after the
/// base delay.  A debt already owed keeps its own schedule.  [review round 7]
/// `stop_worked`: the restart's stop command succeeded, so the restart began.
inline void owe_wallet_start(WalletStartDebt& debt, std::int64_t now_s, bool stop_worked,
                             const WalletRestartPolicy& policy = WalletRestartPolicy{}) noexcept
{
    if (!debt.retry_at.has_value()) {
        debt.retry_at = now_s + policy.start_retry_base_s;
        debt.attempts = 0;
    }
    debt.restart_credit = debt.restart_credit || stop_worked;
}

/// Is a start owed, and due?
[[nodiscard]] constexpr bool wallet_start_due(const WalletStartDebt& debt,
                                              std::int64_t now_s) noexcept
{
    return debt.retry_at.has_value() && now_s >= *debt.retry_at;
}

/// [review round 7] The debt is settled: a start worked, or the wallet
/// answered.  If the restart's stop had worked, that restart has now
/// finished, so it counts: the doubling record_failed_wallet_restart() took
/// back is given back, once, and the next budgets double as a successful
/// restart's do.
inline void settle_wallet_start(WalletStartDebt& debt, WalletSyncWatch& watch) noexcept
{
    if (debt.restart_credit) {
        ++watch.restarts;
    }
    debt = WalletStartDebt{};
}

/// A start command sent because one was due: success settles the debt, and a
/// failure doubles the delay to the next one, up to the cap.
inline void record_wallet_start(WalletStartDebt& debt, WalletSyncWatch& watch, bool started,
                                std::int64_t now_s,
                                const WalletRestartPolicy& policy = WalletRestartPolicy{}) noexcept
{
    if (started) {
        settle_wallet_start(debt, watch);
        return;
    }
    ++debt.attempts;
    debt.retry_at = now_s + wallet_restart_budget(policy.start_retry_base_s, debt.attempts,
                                                  policy.start_retry_cap_s);
}

/// The wallet answered: whatever was owed, it is running.
inline void clear_wallet_start(WalletStartDebt& debt, WalletSyncWatch& watch) noexcept
{
    settle_wallet_start(debt, watch);
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_WALLET_SYNC_WATCH_HPP
