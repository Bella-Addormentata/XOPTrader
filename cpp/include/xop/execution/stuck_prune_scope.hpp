#ifndef XOP_EXECUTION_STUCK_PRUNE_SCOPE_HPP
#define XOP_EXECUTION_STUCK_PRUNE_SCOPE_HPP
// ---------------------------------------------------------------------------
// stuck_prune_scope.hpp -- which wallets may the STARTUP stuck-transaction
// scan visit?
//
// [PRUNE-SCOPE 2026-09-13] The startup prune built its wallet list before the
// wallet-ID map existed. Without the map, OfferManager::resolve_wallet_id()
// answers 1 for "xch" and -1 for every CAT, and the old loop kept only
// positive ids -- so at 23:24:25.792 on 2026-09-12 the scan visited wallet 1
// alone, and "Wallet ID map initialised: 7 CAT wallets found" was not logged
// until 23:24:27.548. The CAT wallets of both enabled pairs were silently
// never scanned at boot.
//
// The engine now builds the map before it builds the list. This header is
// the rule for what it may do with the ids it gets back:
//
//   * AN UNBUILT MAP DEFERS; IT NEVER SHRINKS THE SCAN. If the map could not
//     be built (get_wallets failed) and an asset did not resolve, the list is
//     not "the wallets we trade" -- it is XCH plus holes, and scanning what did
//     resolve reproduces the defect exactly. The whole scan is skipped and the
//     caller says so (complete == false).
//   * A BUILT MAP THAT HAS NO WALLET FOR AN ASSET SKIPS THAT ASSET. The wallet
//     holds nothing of that asset, so there is nothing of ours to prune there.
//   * Every wallet is visited ONCE, however many enabled pairs share it -- the
//     XCH wallet sits under every XCH pair.
//
// NOT DECIDED HERE, deliberately: whether a wallet's unconfirmed rows may be
// deleted. That is stuck_tx_verdict.hpp's rule, and this header does not
// change it.
//
// Pure header: no I/O, no engine types, no RPC, no logging.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace xop::execution {

/// Outcome of stuck_prune_scope().
struct PruneScope {
    /// false: the wallet-ID map was not built and some asset did not resolve,
    /// so the caller must skip the scan rather than scan a subset of it.
    bool complete{false};
    /// Wallet ids to scan, ascending and unique.  Empty whenever !complete.
    std::vector<std::int64_t> scan{};
};

/// @param wallet_map_ready  OfferManager::wallet_ids_resolved(): the
///                          asset-to-wallet map was built.
/// @param candidates        resolve_wallet_id() of the base AND quote asset of
///                          every enabled pair, in any order, duplicates and
///                          non-positive ids included.
[[nodiscard]] inline PruneScope stuck_prune_scope(
    bool wallet_map_ready, const std::vector<std::int64_t>& candidates)
{
    std::vector<std::int64_t> wallets;
    wallets.reserve(candidates.size());
    for (const std::int64_t wid : candidates) {
        if (wid <= 0) {
            if (!wallet_map_ready) {
                return PruneScope{};
            }
            continue;
        }
        wallets.push_back(wid);
    }
    std::sort(wallets.begin(), wallets.end());
    wallets.erase(std::unique(wallets.begin(), wallets.end()), wallets.end());

    PruneScope out;
    out.complete = true;
    out.scan = std::move(wallets);
    return out;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_STUCK_PRUNE_SCOPE_HPP
