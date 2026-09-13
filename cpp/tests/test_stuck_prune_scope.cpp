// ---------------------------------------------------------------------------
// [PRUNE-SCOPE 2026-09-13] The startup stuck-transaction scan sees every
// traded wallet, or none.
//
// On 2026-09-12 the startup prune ran before the wallet-ID map was built. At
// 23:24:25.792 it scanned wallet 1 alone; "Wallet ID map initialised: 7 CAT
// wallets found" was logged only at 23:24:27.548. Without the map
// resolve_wallet_id() answers 1 for "xch" and -1 for every CAT, and the old
// loop kept only positive ids, so the CAT wallets of both enabled pairs
// (XCH/BYC and XCH/DBX) were skipped without a word.
//
// The engine now builds the map before it builds the list. These tests pin
// what it may do with the list (execution/stuck_prune_scope.hpp).
//
// SCOPE: that engine.cpp builds the map FIRST, and hands these ids over, is
// not covered -- nothing in cpp/tests constructs an Engine (TODO S36).
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "xop/execution/stuck_prune_scope.hpp"

namespace {

using xop::execution::PruneScope;
using xop::execution::stuck_prune_scope;
using Ids = std::vector<std::int64_t>;

// THE BUG. XCH/BYC and XCH/DBX before the map existed: {xch, BYC, xch, DBX}
// resolved to {1, -1, 1, -1}.
TEST(StuckPruneScope, AnUnbuiltWalletMapDefersInsteadOfScanningOnlyXch)
{
    const PruneScope scope = stuck_prune_scope(false, Ids{1, -1, 1, -1});
    EXPECT_FALSE(scope.complete)
        << "scanning what resolved would scan the XCH wallet alone";
    EXPECT_TRUE(scope.scan.empty());
}

// The same two pairs once the map is built (BYC is wallet 4, DBX wallet 8).
// XCH sits under both, and is scanned once.
TEST(StuckPruneScope, EveryTradedWalletIsScannedExactlyOnce)
{
    const PruneScope scope = stuck_prune_scope(true, Ids{1, 8, 1, 4});
    EXPECT_TRUE(scope.complete);
    EXPECT_EQ(scope.scan, (Ids{1, 4, 8}));
}

// A built map with no wallet for an asset (-1), or an id that names no wallet
// at all (0): skip that asset, scan the rest.
TEST(StuckPruneScope, AnAssetWithNoWalletIsSkippedOnceTheMapIsBuilt)
{
    const PruneScope scope = stuck_prune_scope(true, Ids{1, -1, 0, 8});
    EXPECT_TRUE(scope.complete);
    EXPECT_EQ(scope.scan, (Ids{1, 8}));
}

}  // namespace
