// height_source.hpp -- which RPC answers "what block is it?", and when to
// change our mind about that.
//
// [S28 2026-08-28] On 2026-08-25 the full node was unreachable for ~2.5 hours
// while the WALLET RPC stayed healthy the whole time -- 529 consecutive
// get_block_height failures, one after another, with a working alternative
// sitting right there. The engine could not switch because `wallet_only_mode_`
// is assigned exactly once, inside open_connections(), so `mode: auto` is a
// startup decision rather than an ongoing one. It auto-detects, and then never
// detects again.
//
// The heartbeat is driven entirely by observing new blocks, so no height means
// no cycle: no quote refresh, no cancel, no reconciliation, no breaker
// evaluation. Offers rest on dexie the entire time and the engine cannot even
// see that they are there. S31 exists because of what happens next.
//
// Why this is a separate header, and pure: switching the height source
// mid-flight changes which RPC the whole heartbeat depends on, so the decision
// wants to be exhaustively testable without an Engine, a socket, or a running
// node -- the same reason drawdown_breaker.hpp and valuation_authority.hpp are
// shaped this way.
//
// Compliant with:
//   ISO/IEC 5055  -- pure, no I/O, no allocation
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
//                    (tests/test_height_source.cpp)

#ifndef XOP_RISK_HEIGHT_SOURCE_HPP
#define XOP_RISK_HEIGHT_SOURCE_HPP

#include <cstdint>

namespace xop::risk {

/// Which RPC the poll loop should ask for the current block height.
enum class HeightSource {
    FullNode,   ///< the normal source: authoritative and cheapest
    Wallet,     ///< the fallback: `wallet_->get_height_info()`
};

/// Mutable state carried across poll cycles by the caller.
///
/// Deliberately plain data. The engine owns one of these; every transition
/// below is a pure function of it plus the outcome of the last attempt.
struct HeightSourceState {
    HeightSource current{HeightSource::FullNode};
    std::uint32_t consecutive_node_failures{0};
    std::uint32_t consecutive_node_successes{0};
};

/// Failures before abandoning the full node for the wallet.
///
/// Not 1: a single failed poll is a transient, and flapping the height source
/// between two RPCs that disagree about the tip by a block or two would make
/// the engine reprocess or skip heights. Not 529 either, which is what
/// "never" cost on 2026-08-25. At the default ~5s poll cadence this is well
/// under a minute of blindness before the engine helps itself.
inline constexpr std::uint32_t kNodeFailuresBeforeWalletFallback = 6;

/// Consecutive node successes required to go back.
///
/// Higher than the fallback threshold ON PURPOSE. A node that is flapping --
/// answering one poll in three as it validates a backlog -- is worse than one
/// that is cleanly down, because each brief return drags the height source
/// back and forth. Returning is not urgent; the wallet is already answering.
inline constexpr std::uint32_t kNodeSuccessesBeforeReturn = 10;

/// Whether a mid-flight switch is permitted at all.
///
/// Only `auto` may switch. `full_node` means the operator asked for the node
/// specifically and a silent downgrade would hide exactly what they wanted to
/// be told about; `wallet_only` is already there.
[[nodiscard]] constexpr bool height_fallback_allowed(bool mode_is_auto) noexcept
{
    return mode_is_auto;
}

/// Fold one poll outcome into `state` and return the source to use NEXT.
///
/// `node_attempt_ok` is the result of the height call that just ran, whichever
/// source served it; a wallet poll counts as neither a node success nor a node
/// failure, so pass the outcome of an explicit node probe (or leave the
/// counters alone by not calling this).
constexpr HeightSource next_height_source(HeightSourceState& state,
                                          bool node_attempt_ok,
                                          bool mode_is_auto) noexcept
{
    if (node_attempt_ok) {
        state.consecutive_node_failures = 0;
        if (state.consecutive_node_successes
            < kNodeSuccessesBeforeReturn) {
            ++state.consecutive_node_successes;
        }
    } else {
        state.consecutive_node_successes = 0;
        if (state.consecutive_node_failures
            < kNodeFailuresBeforeWalletFallback) {
            ++state.consecutive_node_failures;
        }
    }

    if (!height_fallback_allowed(mode_is_auto)) {
        return state.current;   // pinned by configuration
    }

    if (state.current == HeightSource::FullNode) {
        if (state.consecutive_node_failures
            >= kNodeFailuresBeforeWalletFallback) {
            state.current = HeightSource::Wallet;
            state.consecutive_node_successes = 0;
        }
    } else {
        if (state.consecutive_node_successes >= kNodeSuccessesBeforeReturn) {
            state.current = HeightSource::FullNode;
            state.consecutive_node_failures = 0;
        }
    }
    return state.current;
}

/// A height from the wallet may LAG the node's tip, so it must never be
/// allowed to move the engine backwards.
///
/// The two RPCs are different observers of the same chain and the wallet is
/// often a block or two behind. `last_seen` going down would look like a
/// reorg to every downstream consumer -- and on the way back from a fallback
/// the node's higher tip must be accepted immediately, which it is, because
/// this only rejects the backwards direction.
[[nodiscard]] constexpr bool height_is_usable(std::int64_t height,
                                              std::uint32_t last_seen) noexcept
{
    if (height < 0) return false;                       // malfunctioning RPC
    return static_cast<std::uint64_t>(height) >= last_seen;
}

}  // namespace xop::risk

#endif  // XOP_RISK_HEIGHT_SOURCE_HPP
