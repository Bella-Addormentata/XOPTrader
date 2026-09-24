#ifndef XOP_RISK_STATE_POSITION_TRUTH_HPP
#define XOP_RISK_STATE_POSITION_TRUTH_HPP
// ---------------------------------------------------------------------------
// state_position_truth.hpp -- where State's per-asset positions come from.
//
// [SEED-FAIL-CLOSED 2026-09-22] State's positions are what the risk limits
// read: concentration, the single-CAT cap and pair capital all mark
// State::get_position() / get_all_positions() to XCH.  (The InventoryTracker,
// the strategy's q, is a separate record and is not decided here.)
//
// What happened on 2026-09-22.  All three startup balance reads (XCH, BYC,
// DBX) timed out, four 30 s attempts each, 16:58:39 to 17:04:49.  The seed
// caught each failure at DEBUG, so State began with no positions and the log
// said nothing.  Step 8 read no balance before the first fills (the wallet
// circuit was open 17:07:41-17:08:11, and from 17:08:37 Step 8 stopped at its
// wallet sync gate every heartbeat): at 17:09:29 record_buy gave State 1.103
// XCH and 102.976 DBX, and both fills' record_sell legs failed on an unknown
// asset.  In that same heartbeat the single-CAT cap read DBX as 52.8% of the
// portfolio (full block at 50%) and sized the XCH/DBX ask to zero.  Step 8's
// recovery seed existed for exactly this, but it fired only while a position
// was EXACTLY zero, and from then on none was -- so once the wallet synced,
// nothing could have corrected State.  After the next fill (17:13:39) State
// held 0.103 XCH and 203.188 DBX, and no BYC -- DBX about 96% of the
// portfolio at the logged mid -- while the wallet held about 34.7 XCH,
// 99.0 BYC and 2,167 DBX.
//
// The rules:
//
//   * AN UNREAD BALANCE IS NEVER ZERO.  A read that failed, or a reply
//     without confirmed_wallet_balance, falls back to the quantity last
//     persisted for the asset (inventory_state) and the engine marks the
//     position unverified.  An empty State is not a cautious default: with
//     no positions compute_concentration() reports "balanced" and
//     compute_portfolio_fraction() 0%, so no concentration or single-CAT
//     limit can trip at all.
//   * A WALLET-MAP MISS IS ZERO ONLY WHEN THE MAP WAS BUILT.  A built map with
//     no wallet for the asset means the wallet holds none of it
//     (stuck_prune_scope.hpp's rule); an unbuilt map proves nothing.
//   * EVERY VALIDATED READ OVERWRITES THE POSITION, whatever State held.  A
//     position moved only by fills is never better than the balance it
//     started from, and it cannot see fees, taker fills or deposits.  The
//     overwrite does not depend on what State held; that dependency (the
//     exactly-zero guard) is the defect this header replaces.
//   * A READ MISSING ITS FIELDS OVERWRITES NOTHING.  Writers default a missing
//     field to 0 for the spendable gates; that zero is not a balance
//     (WalletBalanceEntry::fields_validated, S19 review round 28).
//   * THE BRIDGE ASSET KEEPS ITS SINGLE WRITER.  While its scan is
//     operational, step_ingest_bridge_flows reconciles that asset itself.
//
// NOT DECIDED HERE: when a balance is fresh enough to use.  The engine calls
// apply_wallet_truth() at the moment of each read, below Step 8's wallet sync
// gate and after Step 2 has booked this heartbeat's fills, so the read is
// current by construction.
//
// decide_state_seed() is pure.  apply_wallet_truth() changes nothing but the
// State it is handed, and logs only through State.
// ---------------------------------------------------------------------------

#include "xop/state.hpp"
#include "xop/types.hpp"

#include <cstdint>
#include <optional>

namespace xop::risk {

/// Where the startup seed takes an asset's State position from.
enum class SeedSource : std::uint8_t {
    Wallet,     ///< the wallet reported confirmed_wallet_balance: verified
    NotHeld,    ///< the built wallet map has no wallet for it: a verified zero
    LastKnown,  ///< not read: the last persisted quantity, UNVERIFIED
};

/// Outcome of decide_state_seed().
struct SeedDecision {
    SeedSource source{SeedSource::LastKnown};
    Mojo       quantity{0};   ///< the position State should hold, >= 0
};

/// @param wallet_confirmed  confirmed_wallet_balance from a reply that
///                          carried it; std::nullopt when the read failed,
///                          the reply lacked the field, or no read was made.
/// @param not_held          the wallet-ID map was BUILT and resolves no
///                          wallet for the asset.
/// @param last_known        the asset's quantity as restored from
///                          inventory_state; 0 when there is no record.
[[nodiscard]] constexpr SeedDecision decide_state_seed(
    std::optional<Mojo> wallet_confirmed, bool not_held, Mojo last_known) noexcept
{
    if (wallet_confirmed.has_value() && *wallet_confirmed >= 0) {
        return SeedDecision{SeedSource::Wallet, *wallet_confirmed};
    }
    if (not_held) {
        return SeedDecision{SeedSource::NotHeld, 0};
    }
    return SeedDecision{SeedSource::LastKnown, last_known > 0 ? last_known : 0};
}

/// What apply_wallet_truth() did.
enum class TruthOutcome : std::uint8_t {
    Skipped,    ///< the read may not overwrite State; nothing changed
    Unchanged,  ///< State already held the wallet's balance
    Corrected,  ///< State held something else and now holds the wallet's
};

/// Outcome of apply_wallet_truth().
struct TruthResult {
    TruthOutcome outcome{TruthOutcome::Skipped};
    Mojo         previous{0};   ///< State's balance before the call
};

/// Make State's position in `asset` the wallet's confirmed balance.
/// @param confirmed         confirmed_wallet_balance as read.
/// @param fields_validated  the reply carried confirmed_wallet_balance and
///                          pending_change (WalletBalanceEntry's bit).
/// @param bridge_owned      `asset` is the bridge asset and its scan is
///                          operational.
[[nodiscard]] inline TruthResult apply_wallet_truth(
    State& state, const AssetId& asset, Mojo confirmed,
    bool fields_validated, bool bridge_owned)
{
    if (!fields_validated || bridge_owned) {
        return TruthResult{};
    }
    const std::optional<Mojo> previous = state.reconcile_balance(asset, confirmed);
    if (!previous.has_value()) {
        return TruthResult{};   // a negative balance: rejected, nothing changed
    }
    return TruthResult{*previous == confirmed ? TruthOutcome::Unchanged
                                              : TruthOutcome::Corrected,
                       *previous};
}

}  // namespace xop::risk

#endif  // XOP_RISK_STATE_POSITION_TRUTH_HPP
