// reward_ingest.hpp -- Detection and valuation of dexie DBX liquidity-reward
// inflows, booked as OTHER INCOME ([REWARD-INCOME 2026-08-01]).
//
// WHY THIS EXISTS.  Every offer submission passes claim_rewards=true
// (offer_manager.cpp -> dexie_client.cpp POST /v1/offers), so dexie pays DBX
// liquidity incentives for qualifying offers -- but the inflows were booked
// NOWHERE.  The ledger had only opening/fill/adjust event types, so reward
// DBX surfaced as wallet-vs-books divergence and was eventually absorbed by
// the invariant control's blind adjusting entries: income silently
// reclassified as "unexplained discrepancy".
//
// DETECTION POINT (investigated 2026-08-01, live wallet + dexie API):
//
//   * The dexie API has NO per-claim reward records.  GET /v1/offers/{id}
//     for a completed incentivized offer carries no reward field at all
//     (verified against a settled XCH/wUSDC.b offer), and the only
//     rewards-related endpoint is /v1/incentives -- program metadata
//     (per-market DBX rewardRate / APR), not claims.
//
//   * The WALLET is where rewards are observable.  Dexie pays accrued
//     rewards in a DAILY BATCH: a burst of many small plain incoming DBX
//     transactions, one per rewarded offer, all confirmed in one block.
//     Measured from the live DBX wallet (wallet_id 8, get_transactions):
//     41 bursts between 2026-06-11 and 2026-07-31, arriving ~19:05-19:15
//     UTC daily, 1-123 coins per burst, EACH COIN 1-219 mojos
//     (0.001-0.219 DBX), totaling 64.682 DBX.  The two most recent bursts
//     (1,381 + 618 mojos) match the live ledger-vs-wallet DBX gap of 1,997
//     mojos to within 2 mojos.
//
//   * Reward coins are cleanly separable from trading flows: they are
//     transaction type 0 (INCOMING_TX -- a plain send), while the bot's own
//     coin management appears as PAIRED outgoing+incoming transactions of
//     equal amount in the same block, and trade settlements move >= ~100,000
//     mojos (~1 XCH worth of DBX; smallest observed 100,589).  Rewards are
//     three orders of magnitude smaller than the smallest trading flow.
//
// So a reward inflow is: a CONFIRMED plain incoming transaction on the
// reward asset's wallet, at most reward_max_mojos_per_coin mojos, with no
// equal-amount outgoing transaction in the same block, confirmed after the
// ledger's opening block for the asset (older inflows are already inside the
// opening balance).
//
// ACCOUNTING TREATMENT (owner-approved): recognize on receipt at fair
// value; FMV becomes cost basis; kept OUT of trading P&L as a separate
// reward-income figure.  FMV comes from the live CoinGecko dexie-bucks
// feed at the moment of booking; the USD amount is embedded in the ledger
// note so restarts rebuild the income total from the ledger alone.
//
// Compliant with:
//   ISO/IEC 5055  -- pure functions, NaN-guarded, no UB
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
//                    (tests/test_reward_ingest.cpp)

#ifndef XOP_ACCOUNTING_REWARD_INGEST_HPP
#define XOP_ACCOUNTING_REWARD_INGEST_HPP

#include "xop/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace xop::accounting {

// Chia wallet RPC transaction types (chia.wallet.util.transaction_type).
// Only kIncomingTx qualifies as a reward: trades settle as trade types, the
// bot's own sends are outgoing, and coinbase/fee rewards are farming.
inline constexpr int kIncomingTx    = 0;
inline constexpr int kOutgoingTx    = 1;
inline constexpr int kCoinbase      = 2;
inline constexpr int kFeeReward     = 3;
inline constexpr int kIncomingTrade = 4;
inline constexpr int kOutgoingTrade = 5;

/// The reward-candidate filter (see file header for the measured evidence
/// behind each condition).
///
/// @param tx_type                Chia wallet transaction type.
/// @param amount_mojos           Transaction amount in CAT mojos.
/// @param confirmed_height       0 while unconfirmed.
/// @param ledger_genesis_height  The asset's ledger opening block; inflows
///                               at or below it are already inside the
///                               opening balance and must not be re-booked.
/// @param max_reward_mojos       Per-coin ceiling separating rewards
///                               (observed 1-219 mojos) from trading flows
///                               (observed >= 100,589 mojos).
/// @param has_matching_outgoing  True when an outgoing transaction of the
///                               same amount confirmed in the same block
///                               (the signature of the bot's own coin
///                               management, not an external inflow).
[[nodiscard]] constexpr bool is_reward_inflow(
    int         tx_type,
    Mojo        amount_mojos,
    BlockHeight confirmed_height,
    BlockHeight ledger_genesis_height,
    Mojo        max_reward_mojos,
    bool        has_matching_outgoing) noexcept
{
    if (tx_type != kIncomingTx) return false;
    if (confirmed_height == 0) return false;            // unconfirmed
    if (confirmed_height <= ledger_genesis_height) return false;
    if (amount_mojos <= 0 || amount_mojos > max_reward_mojos) return false;
    if (has_matching_outgoing) return false;            // self-spend pair
    return true;
}

/// Fair-value numbers for one reward receipt.
struct RewardValuation {
    /// Cost-basis price in the InventoryTracker's USD-pseudo convention:
    /// USD per display unit in kMojosPerXch (1e12) fixed point -- the same
    /// convention as Engine::asset_usd_pseudo_price's quote branch and the
    /// persisted inventory_state basis (verified: live DBX basis
    /// 1.3752e10 == $0.013752 * 1e12).
    Mojo   fmv_pseudo_price{0};

    /// The income recognized, in USD:
    ///   (amount_mojos / mojos_per_unit) * usd_per_unit.
    double income_usd{0.0};
};

/// Value a reward receipt at fair market value.
///
/// @param amount_mojos    Received quantity in asset mojos.
/// @param mojos_per_unit  1e3 for CATs (DBX), 1e12 for XCH.
/// @param usd_per_unit    Live USD price of one display unit (CoinGecko
///                        dexie-bucks feed).
/// @return Zeroed valuation when any input is non-positive or non-finite.
[[nodiscard]] inline RewardValuation value_reward(
    Mojo amount_mojos, double mojos_per_unit, double usd_per_unit) noexcept
{
    RewardValuation v{};
    if (amount_mojos <= 0) return v;
    if (!(mojos_per_unit > 0.0) || !(usd_per_unit > 0.0)) return v;   // NaN-safe
    if (!(usd_per_unit < 1e12)) return v;                             // Inf-guard

    v.fmv_pseudo_price = static_cast<Mojo>(usd_per_unit * 1e12 + 0.5);
    v.income_usd = (static_cast<double>(amount_mojos) / mojos_per_unit)
                 * usd_per_unit;
    return v;
}

/// Ledger-note format for a reward entry.  The FMV is embedded here so the
/// income total is rebuilt from the ledger alone on restart
/// (PnLTracker::rehydrate_from_db) -- writer and parser live side by side
/// so the format cannot drift.
[[nodiscard]] inline std::string reward_note(double income_usd,
                                             double usd_per_unit,
                                             const std::string& wallet_tx)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "dexie liquidity reward; fmv_usd=%.10f; "
                  "px_usd_per_unit=%.10f; wallet_tx=",
                  income_usd, usd_per_unit);
    return std::string(buf) + wallet_tx;
}

/// Parse the fmv_usd field back out of a reward note.  Returns 0.0 for
/// anything that does not carry the field (foreign notes, hand edits).
[[nodiscard]] inline double parse_reward_fmv_usd(
    const std::string& note) noexcept
{
    static constexpr char kKey[] = "fmv_usd=";
    const auto pos = note.find(kKey);
    if (pos == std::string::npos) return 0.0;
    const double v = std::strtod(note.c_str() + pos + sizeof(kKey) - 1,
                                 nullptr);
    // strtod yields 0.0 on garbage; reject negatives and non-finite.
    if (!(v > 0.0) || !(v < 1e15)) return 0.0;
    return v;
}

}  // namespace xop::accounting

#endif  // XOP_ACCOUNTING_REWARD_INGEST_HPP
