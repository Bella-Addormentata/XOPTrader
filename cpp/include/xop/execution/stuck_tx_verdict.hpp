#ifndef XOP_EXECUTION_STUCK_TX_VERDICT_HPP
#define XOP_EXECUTION_STUCK_TX_VERDICT_HPP
// ---------------------------------------------------------------------------
// stuck_tx_verdict.hpp -- may the pruner issue the WALLET-WIDE delete?
//
// [S33 review] delete_unconfirmed_transactions takes a wallet_id and nothing
// else: it removes EVERY unconfirmed row on that wallet.
// prune_stuck_transactions counted only rows past their age threshold, so one
// stuck row from an hour ago authorised deleting this heartbeat's offer
// creations and any unconfirmed secure cancel beside it -- the destruction
// engine.cpp's dry-run guard already names ("pending offer creations,
// unconfirmed secure cancels").
//
// WHY THIS LANDS NOW. The defect is older than this PR, but it was DEAD CODE:
// get_transactions used to send reverse=true, which sorts unconfirmed rows
// last, so on a wallet with 200+ confirmed rows the scan saw none of them and
// stuck_count was structurally zero (test_wallet_requests.cpp pins exactly
// that, and the live wallets hold 32,966 and 9,640 rows). This PR's
// reverse=false is what makes the delete reachable, so the gate ships with it.
//
// The gate is the whole decision: an old row is necessary, and NO fresh row is
// equally necessary, because the delete cannot be aimed.
//
// An unconfirmed row whose age cannot be read is UNKNOWN, not old, and the
// caller must count it as fresh -- coin_pool_verdict.hpp's rule: "Unknown is
// its own state, it is the DEFAULT state, and it authorises nothing."
//
// KNOWN AND DELIBERATE: on a wallet that churns fresh spends faster than the
// threshold this returns false and the prune does not fire. That is the safe
// direction and it is BOUNDED -- engine.hpp's kForceDeletePendingBlocks
// escalation still clears the wallet unconditionally. Do not "fix" the
// starvation by weakening this gate; the backstop already exists.
//
// Pure header: ints and an enum, no I/O, no engine types, no asio, no RPC,
// no spdlog, no JSON.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace xop::execution {

/// @param past_threshold    unconfirmed rows older than their own threshold.
/// @param fresh_or_unknown  unconfirmed rows younger than it, OR whose age
///                          could not be read at all.
[[nodiscard]] inline bool authorises_wallet_wide_delete(
    int past_threshold, int fresh_or_unknown) noexcept
{
    return past_threshold > 0 && fresh_or_unknown == 0;
}

/// How one unconfirmed wallet row lands in the two counts above.
enum class StuckRowClass {
    Confirmed,        ///< Not unconfirmed at all; the pruner ignores it.
    FreshOrUnknown,   ///< Too young to be stuck, OR its age cannot be read.
    PastThreshold,    ///< Older than its own threshold: genuinely stuck.
};

/// Classify ONE unconfirmed row for prune_stuck_transactions.
///
/// [review 2026-09-13] Lifted out of the loop in offer_manager.cpp so ctest
/// drives this exact rule instead of a parallel copy of it. The loop that
/// reads the JSON fields is still not reachable from ctest -- nothing
/// constructs an OfferManager -- but the DECISION it makes now is.
///
/// Two rules here are easy to get wrong and are the reason this is a function:
///
///   * A row whose age cannot be read is FRESH, never old. The delete is
///     wallet-wide and would take that row too, so an unreadable age must not
///     help authorise it -- coin_pool_verdict.hpp's rule that "Unknown is its
///     own state, and it authorises nothing".
///   * A row that CARRIES a spend bundle gets 3x the threshold. It was
///     broadcast, and the mempool drops entries after ~5 minutes, so it is
///     only hopeless once it has outlived a much longer window. This branch
///     was unreachable before this PR: reverse=true sorted unconfirmed rows
///     out of the 200-row window entirely.
///
/// @param confirmed         The row reports confirmed == true.
/// @param age_known         created_at_time was present and readable.
/// @param age_seconds       now - created_at_time; ignored when age_known is
///                          false.
/// @param has_spend_bundle  A non-null spend_bundle is attached.
/// @param max_age_seconds   Base stuck threshold; tripled when a bundle exists.
[[nodiscard]] constexpr StuckRowClass classify_stuck_row(
    bool         confirmed,
    bool         age_known,
    std::int64_t age_seconds,
    bool         has_spend_bundle,
    std::int64_t max_age_seconds) noexcept
{
    if (confirmed) return StuckRowClass::Confirmed;
    if (!age_known) return StuckRowClass::FreshOrUnknown;
    const std::int64_t threshold =
        has_spend_bundle ? max_age_seconds * 3 : max_age_seconds;
    return (age_seconds < threshold) ? StuckRowClass::FreshOrUnknown
                                     : StuckRowClass::PastThreshold;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_STUCK_TX_VERDICT_HPP
