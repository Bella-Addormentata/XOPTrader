#ifndef XOP_EXECUTION_COIN_POOL_VERDICT_HPP
#define XOP_EXECUTION_COIN_POOL_VERDICT_HPP
// ---------------------------------------------------------------------------
// coin_pool_verdict.hpp -- may the coin pool spend, given what we actually know?
//
// [S41 2026-09-01] THE ELEVENTH FAIL-OPEN, AND WHY THE RETURN TYPE ALONE
// DOES NOT FIX IT
// ----------------------------------------------------------------------
// CoinManager::get_spendable_coins() caught ChiaRPCError, logged it, and then
// returned the SAME EMPTY VECTOR a genuinely empty wallet returns. The error
// path and a valid negative answer were indistinguishable -- the exact shape
// of this repo's documented close_out fail-open family.
//
// Measured across logs/xop_trader.log and .1-.5:
//
//     get_spendable_coins failed .......... 68
//     "Have 0 mojos total" ................ 34
//     XCH split failed .................... 34
//     count_free_coins failed (handler) ....  0
//
// 68 is twice 34 because both reads in a cycle fail: count_free_coins and
// then ensure_split's own re-read. The cause was always one application
// error -- "Wallet needs to be fully synced before getting all coins" --
// and three minutes later the same process logged "33 free coins,
// 58996979793936 mojos free". THE WALLET HELD ~59 XCH WHILE THE LOG SAID
// "Have 0 mojos total". A trading bot that reports insolvency because an RPC
// failed is an operational hazard on its own, before any spend.
//
// Making get_spendable_coins return std::optional makes the failure
// REPRESENTABLE. It does not make the fail-open UNREPRESENTABLE, because the
// engine's own handler was fail-open too:
//
//     int free_count = 0;                                   // engine.cpp
//     try   { free_count = co_await count_free_coins(1); }
//     catch (...) { warn(...); }                            // FALLS THROUGH
//     if (free_count >= target) { ok } else { ensure_split(); }  // SPLITS
//
// Un-swallowing the exception upstream would have made that handler reachable
// and it would still have split on a failed read, because free_count keeps
// its 0 initialiser. A mechanical port to maybe.value_or(0) preserves the bug
// verbatim. That is precisely how this family "spawns the next one", so the
// policy is moved out of engine.cpp and into this header, where a test can
// hold it -- nothing in cpp/tests constructs an Engine (TODO S36), and
// ChiaWalletRPC is final with non-virtual methods and no gmock in the repo,
// so neither end of that branch is otherwise testable.
//
// THE RULE: a number may never be substituted for a failed read. Unknown is
// its own state, it is the DEFAULT state, and it authorises nothing.
//
// Modelled on cross_guard.hpp's CrossVerdict::Indeterminate -- "no usable
// reference, decide nothing". Pure header: plain int comparisons, no
// arithmetic, no narrowing, no engine types, no asio, no RPC, no spdlog.
// ---------------------------------------------------------------------------

namespace xop::execution {

enum class CoinPoolAction {
    Satisfied,  ///< read succeeded, pool at or above target -- nothing to do
    Split,      ///< read succeeded, pool below target -- a split is authorised
    Skip,       ///< the pool state is UNKNOWN -- decide nothing, spend nothing
};

// ---------------------------------------------------------------------------
// What we know about the pool. read_ok defaults to FALSE, which is the
// inverse of engine.cpp's `int free_count = 0`: a default-constructed or
// half-initialised reading fails CLOSED.
// ---------------------------------------------------------------------------
struct CoinPoolReading {
    bool read_ok{false};       ///< did the coin enumeration actually succeed?
    int  pool_ready_count{0};  ///< meaningless unless read_ok
};

// ---------------------------------------------------------------------------
// decide_coin_pool_action
//
// The !read_ok clause is FIRST and unconditional, before target_count is even
// examined. Ordering is the whole point: any later clause that can reach
// Satisfied or Split from an unknown reading resurrects S41. Reachability
// argument, for the call site that no test can reach: Split is returned from
// exactly one place, and every path to it passes the read_ok gate.
//
// @param split_pending  a prior split is still confirming -- the pool count is
//                       stale by construction, so authorise nothing.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr CoinPoolAction decide_coin_pool_action(
    CoinPoolReading reading, int target_count, bool split_pending) noexcept
{
    if (!reading.read_ok)                              return CoinPoolAction::Skip;
    if (target_count <= 0)                             return CoinPoolAction::Satisfied;
    if (split_pending)                                 return CoinPoolAction::Skip;
    if (reading.pool_ready_count >= target_count)      return CoinPoolAction::Satisfied;
    return CoinPoolAction::Split;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_COIN_POOL_VERDICT_HPP
