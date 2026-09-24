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
//   * TIME IS MEASURED, NOT COUNTED, on a monotonic clock.  A silence longer
//     than max_observation_gap_s (Step 8 not reached: paused, breaker open)
//     says nothing about the wallet, so it starts a new streak instead of
//     arriving as one enormous unsynced interval.
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
};

/// State carried between Step 8 heartbeats.
struct WalletSyncWatch {
    std::optional<std::int64_t> unsynced_since{};  ///< start of the current unsynced streak
    std::optional<std::int64_t> idle_since{};      ///< start of the current not-syncing run in it
    std::optional<std::int64_t> last_observed{};
    std::uint32_t               restarts{0};       ///< restarts since the wallet last reported synced
    std::uint32_t               failed_restarts{0};///< restart COMMANDS that failed, same period
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
    }
    watch.last_observed = now_s;

    WalletSyncVerdict verdict{};
    verdict.restarts = watch.restarts;
    if (synced) {
        // The streak that just ended, for the "re-synced after" log.
        verdict.unsynced_for_s = watch.unsynced_since.has_value()
                                     ? now_s - *watch.unsynced_since : 0;
        watch = WalletSyncWatch{};
        watch.last_observed = now_s;
        verdict.action = WalletSyncAction::Synced;
        return verdict;
    }

    if (!watch.unsynced_since.has_value()) {
        watch.unsynced_since = now_s;
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

}  // namespace xop::execution

#endif  // XOP_EXECUTION_WALLET_SYNC_WATCH_HPP
