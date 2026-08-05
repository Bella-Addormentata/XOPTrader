// wallet_poll_throttle.hpp -- Pure decision logic for cutting the engine's
// wallet-RPC volume WITHOUT weakening the fill-detection and
// phantom-verification guarantees from cae2bfd ([WALLET-LOAD 2026-08-04]).
//
// MOTIVATION (measured): the Chia wallet daemon was being hammered until
// the wallet app itself froze.  Engine load before this change:
//   - detect_fills: one get_offer per tracked offer (~26 live) EVERY
//     heartbeat.
//   - reconcile_offers: get_all_offers paginated over the FULL trade
//     archive (14,100+ records at 50/page = ~282 calls) every 20 blocks
//     (~6 min) = ~2,800 calls/hour for reconcile alone, each page an
//     expensive trade-record scan in the wallet DB.
//
// SAFETY ARGUMENT (why throttling cannot lose a fill):
//   - A fill missed for K-1 heartbeats is DELAYED detection, not lost:
//     cae2bfd keeps CONFIRMED offers tracked in State until detect_fills
//     processes them through the normal path (fee capture + position
//     accounting), and the fill-completeness sweep catches stragglers.
//   - reconcile's phantom removal NEVER acts on absence-from-scan alone:
//     every tracked offer missing from the scanned pages is individually
//     verified with get_offer before any state is destroyed (SETTLE-FIX
//     2026-07-31).  Early-stopping the pagination therefore cannot cause
//     a wrong removal -- at worst it adds a handful of targeted get_offer
//     calls, still orders of magnitude below the full walk.
//
// Compliant with:
//   ISO/IEC 5055  -- pure functions, overflow/NaN-guarded
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
//                    (tests/test_wallet_poll_throttle.cpp)

#ifndef XOP_EXECUTION_WALLET_POLL_THROTTLE_HPP
#define XOP_EXECUTION_WALLET_POLL_THROTTLE_HPP

#include "xop/types.hpp"

#include <cmath>
#include <cstdint>

namespace xop::execution {

// ---------------------------------------------------------------------------
// detect_fills throttling
// ---------------------------------------------------------------------------

/// Striking-distance test for the poll backoff: an offer is "in striking
/// distance" when the book mid has moved to within
/// `proximity_factor` x its distance-from-mid AT POST TIME of the offer's
/// price.  A deep tier nobody is near does not need 3 polls an hour; one
/// the market is approaching goes back to every-heartbeat polling.
///
///     dist_now_bps = |price - mid| / mid * 1e4
///     in range  <=>  dist_now_bps <= proximity_factor * post_spread_bps
///
/// FAIL-SAFE DEFAULTS: unknown post-time distance (post_spread_bps <= 0 --
/// adopted offers, legacy records), missing market data (mid <= 0), or a
/// degenerate price all return TRUE, i.e. "poll every heartbeat".  The
/// throttle only ever relaxes polling for offers whose provenance and
/// market context are both fully known.
[[nodiscard]] inline bool within_striking_distance(
    Mojo   price,
    Mojo   current_mid,
    double post_spread_bps,
    double proximity_factor = 2.0) noexcept
{
    if (!(post_spread_bps > 0.0)) return true;   // unknown -> vigilant
    if (price <= 0 || current_mid <= 0) return true;
    if (!(proximity_factor > 0.0)) return true;

    const double dist_now_bps =
        std::abs(static_cast<double>(price) -
                 static_cast<double>(current_mid))
        / static_cast<double>(current_mid) * 10'000.0;
    return dist_now_bps <= proximity_factor * post_spread_bps;
}

/// Should detect_fills poll this offer's wallet status this heartbeat?
///
/// @param age_blocks           current_block - created_at_block (<0 treated
///                             as unknown -> poll).
/// @param min_age_blocks       Offers younger than this cannot have settled
///                             (a spend needs at least a block to confirm)
///                             -> skipped entirely.  Config
///                             detect_fills_min_age_blocks, default 2.
/// @param consecutive_pending  How many consecutive polls have seen this
///                             offer still PENDING_ACCEPT.
/// @param backoff_after_polls  M: once consecutive_pending reaches this,
///                             back off.  0 disables the backoff.  Config
///                             detect_fills_backoff_polls, default 10.
/// @param backoff_interval     K: while backed off, poll only every Kth
///                             heartbeat.  Config
///                             detect_fills_backoff_interval, default 3.
/// @param heartbeat_index      Monotonic detect_fills invocation counter.
/// @param in_striking_distance Result of within_striking_distance -- true
///                             RESETS the offer to every-heartbeat polling.
[[nodiscard]] constexpr bool fill_poll_due(
    std::int64_t  age_blocks,
    std::uint32_t min_age_blocks,
    std::uint32_t consecutive_pending,
    std::uint32_t backoff_after_polls,
    std::uint32_t backoff_interval,
    std::uint64_t heartbeat_index,
    bool          in_striking_distance) noexcept
{
    // Age gate: a just-posted offer cannot have settled yet.
    if (age_blocks >= 0
        && age_blocks < static_cast<std::int64_t>(min_age_blocks)) {
        return false;
    }
    // Striking distance overrides any backoff.
    if (in_striking_distance) return true;
    // Backoff disabled, or not yet earned.
    if (backoff_after_polls == 0
        || consecutive_pending < backoff_after_polls) {
        return true;
    }
    // Backed off: poll every Kth heartbeat.
    if (backoff_interval <= 1) return true;
    return (heartbeat_index % backoff_interval) == 0;
}

// ---------------------------------------------------------------------------
// reconcile_offers pagination early-stop
// ---------------------------------------------------------------------------
//
// VERIFIED RPC SEMANTICS (docs.chia.net offer-rpc + chia-blockchain
// trade_store.py, checked 2026-08-04):
//   - get_all_offers start/end are array INDICES (start inclusive, end
//     exclusive), NOT block heights -- a since-height filter is impossible.
//   - sort_key defaults to CONFIRMED_AT_HEIGHT: ORDER BY
//     confirmed_at_index DESC -- pending offers (confirmed_at_index = 0)
//     sort LAST, which is why the old scan had to walk the entire archive
//     to reach the very records it cared about.
//   - sort_key = "RELEVANCE": pending statuses order FIRST, then records
//     by created_at_time DESC.  With reverse=false this yields exactly the
//     stream we need: page 1 carries the whole live set (tracked offers +
//     PENDING_ACCEPT adoptees, ~26 records), followed by terminal records
//     newest-created first.
//
// STOP RULE: after kReconcileStopAfterOldPages consecutive pages whose
// NEWEST record is older than the cutoff, no later page can contain a
// relevant record (created_at_time is monotonically non-increasing in the
// terminal region), so pagination stops.  The cutoff is the oldest tracked
// offer's creation time minus kReconcileScanSlackSecs -- the slack covers
// PENDING_ACCEPT adoptees that may predate the tracked set (an untracked
// offer is adopted within one reconcile cadence of losing tracking, so a
// 24 h allowance is generous by orders of magnitude).  Expected pages at
// live numbers: ~2-4 instead of ~282.

/// Pages whose newest record is older than the cutoff by this many
/// consecutive pages end the scan.  > 1 tolerates one out-of-order page
/// (e.g. a record inserted mid-scan shifting indices).
inline constexpr int kReconcileStopAfterOldPages = 2;

/// Adoptee allowance below the oldest tracked offer's creation.
inline constexpr std::int64_t kReconcileScanSlackSecs = 24 * 3600;

/// The creation-time cutoff for the early stop.  When nothing is tracked
/// (fresh start, all offers cancelled) the scan still covers the last
/// slack window for adoptees, anchored at `now`.
[[nodiscard]] constexpr std::int64_t reconcile_scan_cutoff(
    std::int64_t oldest_tracked_created_at_unix,
    std::int64_t now_unix,
    std::int64_t slack_secs = kReconcileScanSlackSecs) noexcept
{
    const std::int64_t base = (oldest_tracked_created_at_unix > 0)
                                  ? oldest_tracked_created_at_unix
                                  : now_unix;
    return (slack_secs > 0 && base > slack_secs) ? base - slack_secs : 0;
}

/// True when a page's NEWEST created_at_time is strictly older than the
/// cutoff.  A page with no parseable created_at_time (newest == 0) is NOT
/// old -- fail open and keep scanning.
[[nodiscard]] constexpr bool page_entirely_older(
    std::int64_t newest_created_at_in_page,
    std::int64_t cutoff_unix) noexcept
{
    return newest_created_at_in_page > 0
        && cutoff_unix > 0
        && newest_created_at_in_page < cutoff_unix;
}

/// Consecutive-old-page counter for the stop rule.  observe_page returns
/// true when pagination should stop AFTER processing the observed page.
class ReconcileEarlyStop {
public:
    explicit constexpr ReconcileEarlyStop(
        int stop_after = kReconcileStopAfterOldPages) noexcept
        : stop_after_(stop_after > 0 ? stop_after : 1) {}

    [[nodiscard]] constexpr bool observe_page(bool entirely_older) noexcept
    {
        consecutive_old_ = entirely_older ? consecutive_old_ + 1 : 0;
        return consecutive_old_ >= stop_after_;
    }

    [[nodiscard]] constexpr int consecutive_old_pages() const noexcept
    {
        return consecutive_old_;
    }

private:
    int stop_after_;
    int consecutive_old_{0};
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_WALLET_POLL_THROTTLE_HPP
