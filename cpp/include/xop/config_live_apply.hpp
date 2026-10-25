// ---------------------------------------------------------------------------
// config_live_apply.hpp -- which saved settings a RUNNING engine adopts.
//
// [LIVECFG 2026-10-25] A GUI Save used to reach the running engine as two
// changes only (a pair going OFF, the pace controller going OFF); every other
// edit waited for the next engine start, and the operator had no way to tell
// which. The engine now adopts the whole saved file at the next heartbeat,
// except for the short list below, which genuinely cannot change under a
// running process:
//
//   * chia.*                  -- the node and wallet RPC clients, their TLS
//                                identities and the operating mode are opened
//                                once, and the dead man's switch thread
//                                builds its own wallet client from them.
//   * dexie.api_base / max_requests_per_10s
//                             -- baked into the shared Dexie client.
//   * database.path           -- the open SQLite handle, P&L tracker and
//                                every flag file live beside it.
//   * monitoring.prometheus_port
//                             -- bound by the metrics exporter at startup.
//   * risk.watchdog_stall_seconds
//                             -- read by the dead man's switch thread, which
//                                is armed (or not) once at startup.
//   * strategy.startup_analysis_blocks
//                             -- the observe-before-quoting phase runs once,
//                                before the first heartbeat.
//   * accounting.ledger_enabled / bridge_ingest_enabled turned ON, or the
//     bridge asset ids changing
//                             -- the ledger openings they rely on are posted
//                                once, at startup (turning either OFF is live).
//   * a pair being ENABLED, ADDED or REMOVED, or a pair's asset ids or
//                                units changing -- per-pair strategy objects,
//                                estimators and seats exist only for pairs
//                                enabled at startup (config_reload.hpp).
//
// Each of those keeps its running value in the merged config, and is named
// in `restart_required` so the GUI can say exactly what still waits for a
// restart instead of a blanket "other changes need an engine restart".
//
// Pure and total: the plan is computed here, testable without an engine.
// The Engine owns the wiring (validation, the in-place assignment, pushing
// the new values into subsystems that cached them).
// ---------------------------------------------------------------------------

#ifndef XOP_CONFIG_LIVE_APPLY_HPP
#define XOP_CONFIG_LIVE_APPLY_HPP

#include "xop/config.hpp"
#include "xop/config_reload.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace xop {

/// What a saved config does to a running engine.
struct LiveConfigPlan {
    /// The config the running engine adopts: the saved file, with every
    /// restart-only setting held at its running value. `merged.pairs` has
    /// EXACTLY the running pairs, in the running order, so the engine can
    /// assign it element by element without moving a PairConfig (other
    /// structures hold pointers into config_.pairs).
    AppConfig merged;

    /// Pair enable flags by name (config_reload.hpp). `to_disable` is
    /// applied live; `to_enable` and `structural` are restart-required and
    /// are also listed in `restart_required`.
    ReloadDiff pairs;

    /// Human-readable names of settings the save changed that the running
    /// engine cannot adopt. Empty means the whole save applied live.
    std::vector<std::string> restart_required;
};

namespace detail {

inline void note_restart(std::vector<std::string>& out, bool changed,
                         std::string what)
{
    if (changed) out.push_back(std::move(what));
}

/// The per-pair fields that identify a pair's market: which subsystems were
/// built for it, and how its prices and sizes convert to mojos.
inline bool pair_identity_differs(const PairConfig& a, const PairConfig& b)
{
    return a.base_asset_id != b.base_asset_id
        || a.quote_asset_id != b.quote_asset_id
        || a.base_mojos_per_unit != b.base_mojos_per_unit
        || a.quote_mojos_per_unit != b.quote_mojos_per_unit;
}

}  // namespace detail

/// Plan the live adoption of `saved` by an engine running `running`.
[[nodiscard]] inline LiveConfigPlan plan_live_config(const AppConfig& running,
                                                     const AppConfig& saved)
{
    LiveConfigPlan plan;
    plan.merged = saved;
    AppConfig& m = plan.merged;
    auto& rr = plan.restart_required;

    // -- Connections opened once --------------------------------------------
    const ChiaConfig& rc = running.chia;
    const ChiaConfig& sc = saved.chia;
    detail::note_restart(rr, rc.mode != sc.mode, "chia.mode");
    detail::note_restart(rr,
        rc.full_node_host != sc.full_node_host
            || rc.full_node_port != sc.full_node_port,
        "chia full node host/port");
    detail::note_restart(rr,
        rc.wallet_host != sc.wallet_host || rc.wallet_port != sc.wallet_port,
        "chia wallet host/port");
    detail::note_restart(rr,
        rc.ssl_cert_path != sc.ssl_cert_path
            || rc.ssl_key_path != sc.ssl_key_path
            || rc.wallet_cert_path != sc.wallet_cert_path
            || rc.wallet_key_path != sc.wallet_key_path
            || rc.ca_cert_path != sc.ca_cert_path,
        "chia TLS certificate paths");
    detail::note_restart(rr, rc.verify_ssl != sc.verify_ssl, "chia.verify_ssl");
    detail::note_restart(rr, rc.wallet_fingerprint != sc.wallet_fingerprint,
                         "chia wallet fingerprint");
    m.chia = running.chia;

    detail::note_restart(rr, running.dexie.api_base != saved.dexie.api_base,
                         "dexie.api_base");
    detail::note_restart(rr,
        running.dexie.max_requests_per_10s != saved.dexie.max_requests_per_10s,
        "dexie.max_requests_per_10s");
    m.dexie.api_base             = running.dexie.api_base;
    m.dexie.max_requests_per_10s = running.dexie.max_requests_per_10s;

    detail::note_restart(rr, running.database.path != saved.database.path,
                         "database.path");
    m.database = running.database;

    detail::note_restart(rr,
        running.monitoring.prometheus_port != saved.monitoring.prometheus_port,
        "monitoring.prometheus_port");
    m.monitoring.prometheus_port = running.monitoring.prometheus_port;

    detail::note_restart(rr,
        running.risk.watchdog_stall_seconds != saved.risk.watchdog_stall_seconds,
        "risk.watchdog_stall_seconds");
    m.risk.watchdog_stall_seconds = running.risk.watchdog_stall_seconds;

    detail::note_restart(rr,
        running.strategy.startup_analysis_blocks
            != saved.strategy.startup_analysis_blocks,
        "strategy.startup_analysis_blocks");
    m.strategy.startup_analysis_blocks = running.strategy.startup_analysis_blocks;

    // Ledger openings (post_ledger_genesis) are written once, at startup,
    // for the accounting set-up of that moment: turning the ledger or bridge
    // booking ON, or moving the bridge assets, needs that genesis to run
    // again. Turning either OFF applies live.
    const AccountingConfig& ra = running.accounting;
    const AccountingConfig& sa = saved.accounting;
    if (!ra.ledger_enabled && sa.ledger_enabled) {
        rr.push_back("accounting.ledger_enabled");
        m.accounting.ledger_enabled = false;
    }
    if (!ra.bridge_ingest_enabled && sa.bridge_ingest_enabled) {
        rr.push_back("accounting.bridge_ingest_enabled");
        m.accounting.bridge_ingest_enabled = false;
    }
    if (ra.bridge_asset_id != sa.bridge_asset_id
        || ra.bridge_secondary_asset_ids != sa.bridge_secondary_asset_ids) {
        rr.push_back("accounting bridge asset ids");
        m.accounting.bridge_asset_id            = ra.bridge_asset_id;
        m.accounting.bridge_secondary_asset_ids = ra.bridge_secondary_asset_ids;
        m.accounting.bridge_secondary_asset_ids_explicit =
            ra.bridge_secondary_asset_ids_explicit;
    }
    // [review] The stuck-transaction guard reads the warp jobs database
    // (warp.jobs_db) before every cleanup, and the GUI's warp worker records
    // its sends in the database THIS engine was launched with (it follows
    // the launched config: EngineBridge._sync_warp_engine_store).  A guard
    // moved by a live save would read the new file -- empty, "nothing to
    // protect" -- while the reservations sit in the old one.  Both database
    // paths wait for a restart, like database.path; the warp switch itself
    // applies live (an existing database is always read).
    if (ra.warp_jobs_db_path != sa.warp_jobs_db_path) {
        rr.push_back("warp.jobs_db");
        m.accounting.warp_jobs_db_path = ra.warp_jobs_db_path;
    }
    if (ra.bridge_jobs_db_path != sa.bridge_jobs_db_path) {
        rr.push_back("accounting.bridge_jobs_db_path");
        m.accounting.bridge_jobs_db_path = ra.bridge_jobs_db_path;
    }

    // -- Pairs: the running set, in the running order -----------------------
    plan.pairs = diff_pair_enables(running.pairs, saved.pairs);
    m.pairs.clear();
    m.pairs.reserve(running.pairs.size());
    for (const PairConfig& r : running.pairs) {
        const PairConfig* s = nullptr;
        for (const PairConfig& cand : saved.pairs) {
            if (cand.name == r.name) { s = &cand; break; }
        }
        if (s == nullptr) {
            // Removed from the file: it keeps every seat it holds until the
            // restart, so it keeps its running settings too.
            m.pairs.push_back(r);
            continue;
        }
        PairConfig next = *s;
        if (detail::pair_identity_differs(r, *s)) {
            rr.push_back("pair " + r.name + " asset ids/units");
            next.base_asset_id        = r.base_asset_id;
            next.quote_asset_id       = r.quote_asset_id;
            next.base_mojos_per_unit  = r.base_mojos_per_unit;
            next.quote_mojos_per_unit = r.quote_mojos_per_unit;
        }
        // Off may go live; on may not (config_reload.hpp).
        next.enabled = r.enabled && s->enabled;
        m.pairs.push_back(std::move(next));
    }
    for (const auto& name : plan.pairs.to_enable) {
        rr.push_back("pair " + name + " enabled");
    }
    for (const auto& name : plan.pairs.structural) {
        bool in_running = false;
        for (const PairConfig& r : running.pairs) {
            if (r.name == name) { in_running = true; break; }
        }
        rr.push_back("pair " + name + (in_running ? " removed" : " added"));
    }
    return plan;
}

}  // namespace xop

#endif  // XOP_CONFIG_LIVE_APPLY_HPP
