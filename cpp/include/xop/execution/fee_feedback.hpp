// ---------------------------------------------------------------------------
// fee_feedback.hpp -- reading "your fee is too low" out of wallet JSON.
//
// [S67 2026-09-20] The fee controller's most direct signal is the wallet's
// own record of what the node said.  chia 2.7.4:
//
//   * wallet_transaction_store.py increment_sent APPENDS one
//     (peer_id, MempoolInclusionStatus, Err name or null) tuple to
//     TransactionRecord.sent_to for every answer a peer gives;
//   * mempool_inclusion_status.py: SUCCESS = 1, PENDING = 2, FAILED = 3;
//   * transaction_record.py is_valid keeps a record alive -- and the wallet
//     keeps re-sending it -- when the error is INVALID_FEE_LOW_FEE or
//     INVALID_FEE_TOO_CLOSE_TO_ZERO: "a temporary error".
//
// Seen live, verbatim from ~/.chia/mainnet/log/debug.log on 2026-09-20:
//   (peer, included, error) list: [('ec9e...ac0a', 3, 'INVALID_FEE_TOO_CLOSE_TO_ZERO')]
//
// [review #163 r9] THE LIST IS PER PEER, NOT A TIMELINE, AND "THE LAST ENTRY
// WINS" WAS WRONG.  An earlier revision of this comment said the array is
// append-only in time order so the last entry is "the node's current word".
// There is no single node.  increment_sent appends one tuple per PEER ACK
// (wallet_transaction_store.py increment_sent; wallet_node_api.py calls it for
// SUCCESS, PENDING and FAILED alike, keyed on the acking peer), and the wallet
// sends the bundle to every peer BEFORE any of them answers (wallet_node.py),
// so the order is arrival order among peers and carries no transaction-level
// verdict.  chia agrees: transaction_record.py is_in_mempool() is ANY-wins --
// true if ANY tuple is SUCCESS or PENDING.
//
// Reading only back() therefore did two things wrong, both measured against
// this wallet's own debug.log (8 files, 38,250 non-empty lists):
//
//   * 8,289 of those lists (21.7%) carry more than one peer, so one peer
//     accepting and a later peer refusing on fee would have been read as a
//     refusal -- raising the fee on no evidence, the exact opposite of the
//     probe-down this feature exists to provide;
//   * 341 lists DO carry a fee refusal that is simply not last (typically
//     followed by another peer's DOUBLE_SPEND) and were silently dropped.
//
// So: scan the WHOLE array.  Any peer that accepted (SUCCESS or PENDING)
// suppresses the row -- the spend is in a mempool and our fee is not what is
// stopping it.  Nothing else suppresses it: a DOUBLE_SPEND from another peer
// says the coin is gone, which is true regardless of the fee and is not a
// contradiction of a peer that refused the fee.
//
// AND THE PART AN ANY-SUCCESS GUARD CANNOT FIX, said plainly rather than
// glossed.  filter_ok_mempool_status (wallet_transaction_store.py) STRIPS the
// SUCCESS/PENDING tuples on the ~1800 s resend tick, so a transaction sitting
// happily in an accepting peer's mempool later presents as a failures-only
// list ending in a stale fee refusal -- and chia's own is_in_mempool() returns
// false for it too, leaving nothing to suppress.  That is consistent with what
// the live logs show: across all 38,250 lists, ZERO ever carried a SUCCESS or
// PENDING tuple.  This parser cannot distinguish that case, and no reading of
// `sent_to` alone can.  What bounds it is downstream, not here: the signal is
// UNATTRIBUTED, so Controller::observe applies the dead time and the
// kMaxUncorroboratedRaises streak limit to it.  That is a bound on the damage,
// not a cure for the ambiguity, and it is weaker while a probe is in flight
// (the dead-time branch is guarded by !probing_).
//
// prune_stuck_transactions() already holds these rows (get_transactions), so
// reading the field adds no RPC.
//
// Source, tag 2.7.4:
//   https://github.com/Chia-Network/chia-blockchain/blob/2.7.4/chia/wallet/transaction_record.py
//   https://github.com/Chia-Network/chia-blockchain/blob/2.7.4/chia/wallet/wallet_transaction_store.py
//   https://github.com/Chia-Network/chia-blockchain/blob/2.7.4/chia/types/mempool_inclusion_status.py
//
// Pure: JSON in, bool out.  No I/O, no logging.
// ---------------------------------------------------------------------------

#ifndef XOP_EXECUTION_FEE_FEEDBACK_HPP
#define XOP_EXECUTION_FEE_FEEDBACK_HPP

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "xop/strategy/fee_controller.hpp"

namespace xop::execution {

/// Transaction names remembered so a refused row is normally reported once
/// rather than once per prune.
///
/// [review #163 r9] "ONCE" IS NOT AN INVARIANT, AND THIS COMMENT USED TO SAY
/// IT WAS.  OfferManager::prune_stuck_transactions tests the capacity BEFORE
/// it inserts and then clears the WHOLE set, so the prune that trips this
/// bound re-inserts and re-counts every refused row still visible in that
/// sweep -- not one repeat, up to a sweep's worth.  There is also no per-name
/// eviction, so names of transactions the wallet has since deleted are never
/// shed and only the clear ever removes them.  The old text also said the
/// repeat is "absorbed by the controller's dead time"; that branch is guarded
/// by `!probing_` (fee_controller.hpp), so it does not apply while a probe is
/// in flight.  What does apply in every case is the kMaxUncorroboratedRaises
/// streak limit, which caps consecutive unattributed raises at three.
inline constexpr std::size_t kMaxReportedFeeRejections = 256;

/// MempoolInclusionStatus, mempool_inclusion_status.py.
inline constexpr int kMempoolInclusionSuccess = 1;
inline constexpr int kMempoolInclusionPending = 2;
inline constexpr int kMempoolInclusionFailed  = 3;

/// True when one get_transactions row's `sent_to` array reports our FEE as the
/// reason a peer refused it, and NO peer accepted it.
///
/// [review #163 r9] Scans every tuple, because the array is per-peer rather
/// than a timeline -- see the header.  The rules, in order:
///
///   * anything unreadable makes the WHOLE ROW not evidence, and returns
///     immediately rather than skipping the entry.  An entry we cannot parse
///     might be the SUCCESS that would have suppressed the row, so stepping
///     over it would let a malformed tuple hide an acceptance;
///   * any peer reporting SUCCESS or PENDING suppresses the row: the spend is
///     in a mempool, so our fee is not what is stopping it;
///   * a status we do not model suppresses it too, for the same reason;
///   * otherwise the row is evidence iff at least one peer refused it with
///     INVALID_FEE_TOO_CLOSE_TO_ZERO or INVALID_FEE_LOW_FEE.  A FAILED tuple
///     with a null or non-string error is readable and simply is not a fee
///     refusal -- it neither counts nor suppresses.
[[nodiscard]] inline bool sent_to_reports_fee_rejection(const nlohmann::json& tx)
{
    if (!tx.is_object()) {
        return false;
    }
    const auto it = tx.find("sent_to");
    if (it == tx.end() || !it->is_array() || it->empty()) {
        return false;
    }
    bool saw_fee_rejection = false;
    for (const auto& entry : *it) {
        if (!entry.is_array() || entry.size() < 3U || !entry[1].is_number_integer()) {
            return false;   // unreadable: it could have been the SUCCESS
        }
        const int status = entry[1].get<int>();
        if (status != kMempoolInclusionFailed) {
            return false;   // SUCCESS, PENDING, or a status we do not model
        }
        if (entry[2].is_string()
            && strategy::fee::is_fee_rejection(entry[2].get_ref<const std::string&>())) {
            saw_fee_rejection = true;
        }
    }
    return saw_fee_rejection;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_FEE_FEEDBACK_HPP
