// warp_send_guard.hpp -- Which wallets the GUI's warp jobs have a Chia send
// in flight on, so the engine's WALLET-WIDE unconfirmed-transaction deletes
// leave those wallets alone ([WARP-LOST-SEND 2026-10-25]).
//
// WHY THIS EXISTS.  Step 8 treats pending_change on a wallet as "our own
// spends are not confirming": after kForceDeletePendingBlocks consecutive
// heartbeats it calls delete_unconfirmed_transactions for the wallet, and
// the periodic stuck-transaction prune does the same for rows past their
// age threshold.  Both are wallet-wide, and the wallet is shared with the
// GUI's warp jobs.  On 2026-10-24 (job 4) a claim's 0.0001 XCH funding send
// put pending_change on the XCH wallet; it had spent a coin an engine offer
// also used, so it could never confirm, and 53 seconds after it was sent
// the force-delete removed it -- the job then waited two hours for a coin
// that could never appear.  Even a healthy fee-less send takes longer than
// the ~10-minute escalation to confirm in a full mempool, so the engine was
// deleting warp's sends by design.
//
// THE RULE.  A wallet an ACTIVE warp job has a Chia send in flight on is
// off limits: its pending_change does not count toward the escalation, and
// neither delete runs against it.  The GUI records every such send in
// warp_jobs.db (funding_tx_id for the claim's security coin and the
// unwrap's toll coin, burn_tx_id for the unwrap's cat_spend) and every
// Dexie take it submits (dexie_takes); the engine already reads that
// database read-only for bridge accounting.  Classification is pure, here,
// so ctest drives it; the SQLite read and the wallet-id resolution are the
// engine's (engine.cpp read_warp_send_guard / wallet_holds_warp_send).
//
// FAIL CLOSED.  A database that exists but cannot be read protects EVERY
// wallet for that heartbeat: a delayed cleanup costs nothing, a deleted warp
// send costs the operator a two-hour stall and a manual retry.
//
// THE RACE, AND WHAT IS LEFT OF IT.  The GUI writes its marker BEFORE the
// wallet RPC that sends (a durable reservation; service.py _send_reserved),
// and the engine re-reads this database immediately before each delete
// instead of trusting the heartbeat's cached read.  What remains is the
// GUI reserving between the engine's final read and the delete RPC
// completing -- a window of one SQLite read plus one wallet RPC, and the
// periodic prune HOLDS when any fresh unconfirmed row exists anyway.  A
// shared lock across the two processes would close it entirely; the two
// databases are separate and the wallet RPC is not transactional, so that
// is left as a follow-up.

#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <set>
#include <string>

namespace xop::execution {

/// The wallets one in-flight warp Chia send touches.
struct WarpChiaSend {
    bool        xch{false};        ///< the XCH wallet (amount and/or fee)
    std::string cat_asset_id;      ///< a CAT wallet, by asset id ("" = none)
};

[[nodiscard]] inline bool is_lower_hex64_asset(const std::string& s) noexcept
{
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

/// The asset id a job's frozen asset fingerprint names
/// ("v1:<erc20>:<decimals>:<cat decimals>:<asset id>"), or "" when absent
/// or malformed.
[[nodiscard]] inline std::string fingerprint_asset_id(const std::string& fp)
{
    const auto pos = fp.rfind(':');
    if (pos == std::string::npos) return {};
    std::string id = fp.substr(pos + 1);
    for (auto& c : id) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return is_lower_hex64_asset(id) ? id : std::string{};
}

/// From one ACTIVE warp job: the Chia send it has in flight, if any.
///
/// FUNDING_CLAIM funds the claim's security coin and BURNING the unwrap's
/// toll coin, both XCH sends recorded as state.funding_tx_id.  BURN_SENT's
/// cat_spend (state.burn_tx_id) moves the job's CAT and pays any fee from
/// XCH, so it touches both wallets; the CAT is the fingerprint's asset,
/// else @p default_cat_asset_id (the wUSDC.b bridge asset, the only one
/// jobs stamped before fingerprints could unwrap).  A FAILED job keeps the
/// active slot AND its markers -- a confirmation timeout does not stop the
/// send it recorded -- so it is read by the status it failed from
/// (state.failed_from) until the operator retries or closes it.  Every
/// other status either has no Chia send outstanding or pushes bundles the
/// wallet never holds (the claim and burn bundles go straight to the node).
[[nodiscard]] inline std::optional<WarpChiaSend> warp_job_chia_send(
    const std::string& status_in, const nlohmann::json& state,
    const std::string& default_cat_asset_id)
{
    const auto text = [&](const char* key) -> std::string {
        if (!state.is_object() || !state.contains(key)
            || !state[key].is_string()) {
            return {};
        }
        return state[key].get<std::string>();
    };
    const auto has_priors = [&](const char* key) -> bool {
        return state.is_object() && state.contains(key)
            && state[key].is_array() && !state[key].empty();
    };
    const std::string status =
        status_in == "FAILED" ? text("failed_from") : status_in;
    WarpChiaSend s;
    bool any = false;
    if (status == "FUNDING_CLAIM" || status == "BURNING") {
        if (!text("funding_tx_id").empty()) {
            s.xch = true;
            any   = true;
        }
    }
    if (status == "BURN_SENT") {
        if (!text("burn_tx_id").empty()) {
            s.xch          = true;
            s.cat_asset_id = fingerprint_asset_id(text("asset_fingerprint"));
            if (s.cat_asset_id.empty()) s.cat_asset_id = default_cat_asset_id;
            any = true;
        }
    }
    // A job that REPLACED a send keeps its wallets protected in every
    // active status until it closes: an earlier attempt's coin may have
    // carried the job forward while the replacement is still confirming,
    // and the GUI settles the attempts only when it closes the job.
    if (has_priors("funding_tx_id_prior") && !text("funding_tx_id").empty()) {
        s.xch = true;
        any   = true;
    }
    if (has_priors("burn_tx_id_prior") && !text("burn_tx_id").empty()) {
        s.xch = true;
        if (s.cat_asset_id.empty()) {
            s.cat_asset_id = fingerprint_asset_id(text("asset_fingerprint"));
            if (s.cat_asset_id.empty()) s.cat_asset_id = default_cat_asset_id;
        }
        any = true;
    }
    if (!any) return std::nullopt;
    return s;
}

/// From one dexie_takes row: a take the wallet is still confirming spends
/// XCH (the price and our fee) and receives the CAT, and the wallet's
/// trade record rides on its unconfirmed transactions.
[[nodiscard]] inline std::optional<WarpChiaSend> warp_take_chia_send(
    const std::string& status, const std::string& asset_id)
{
    if (status != "submitting" && status != "submitted") return std::nullopt;
    WarpChiaSend s;
    s.xch          = true;
    s.cat_asset_id = asset_id;
    return s;
}

/// Every wallet warp has a Chia send in flight on, this heartbeat.
struct WarpSendGuard {
    /// False when warp_jobs.db exists but could not be read completely:
    /// then EVERY wallet is protected (fail closed).
    bool                  readable{true};
    bool                  xch{false};
    std::set<std::string> cat_asset_ids;

    void add(const WarpChiaSend& s)
    {
        if (s.xch) xch = true;
        if (!s.cat_asset_id.empty()) cat_asset_ids.insert(s.cat_asset_id);
    }

    /// Whether anything is protected at all (cheap skip for the common case).
    [[nodiscard]] bool any() const noexcept
    {
        return !readable || xch || !cat_asset_ids.empty();
    }
};

}  // namespace xop::execution
