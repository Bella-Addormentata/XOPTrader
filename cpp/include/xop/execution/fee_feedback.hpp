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
// The list is append-only and in time order, so the LAST entry is the node's
// current word: a refusal followed by a SUCCESS means the spend is in the
// mempool now and is no longer evidence of anything.
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

/// Transaction names remembered so a refused row is reported once, not once
/// per prune.  When the set fills it is cleared: the worst case is one repeat
/// report, which the controller's dead time absorbs.
inline constexpr std::size_t kMaxReportedFeeRejections = 256;

/// MempoolInclusionStatus.FAILED.
inline constexpr int kMempoolInclusionFailed = 3;

/// True when the LATEST sent_to entry of one get_transactions row is a FAILED
/// inclusion whose error is a fee refusal.  Anything unreadable -- no sent_to,
/// an empty list, an entry that is not a 3-element array, a status that is
/// not a number, a null error -- is false: an unreadable row is not evidence.
[[nodiscard]] inline bool latest_sent_to_is_fee_rejection(const nlohmann::json& tx)
{
    if (!tx.is_object()) {
        return false;
    }
    const auto it = tx.find("sent_to");
    if (it == tx.end() || !it->is_array() || it->empty()) {
        return false;
    }
    const nlohmann::json& last = it->back();
    if (!last.is_array() || last.size() < 3U) {
        return false;
    }
    if (!last[1].is_number_integer() || last[1].get<int>() != kMempoolInclusionFailed) {
        return false;
    }
    if (!last[2].is_string()) {
        return false;
    }
    return strategy::fee::is_fee_rejection(last[2].get_ref<const std::string&>());
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_FEE_FEEDBACK_HPP
