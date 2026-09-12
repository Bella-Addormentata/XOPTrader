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
// Pure header: two ints, no I/O, no engine types, no asio, no RPC, no spdlog.
// ---------------------------------------------------------------------------

namespace xop::execution {

/// @param past_threshold    unconfirmed rows older than their own threshold.
/// @param fresh_or_unknown  unconfirmed rows younger than it, OR whose age
///                          could not be read at all.
[[nodiscard]] inline bool authorises_wallet_wide_delete(
    int past_threshold, int fresh_or_unknown) noexcept
{
    return past_threshold > 0 && fresh_or_unknown == 0;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_STUCK_TX_VERDICT_HPP
