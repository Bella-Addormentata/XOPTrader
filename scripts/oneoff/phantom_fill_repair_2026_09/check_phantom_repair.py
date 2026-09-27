#!/usr/bin/env python3
"""Read-only checks for the phantom-fill repair (trade_log 1900-1902).

Compares the repaired database (TARGET) with the pre-repair BASELINE and
prints one PASS/FAIL line per post-condition.  For the live repair the
baseline is the file apply_phantom_repair.py --backup-to wrote inside its
transaction; for verification, a copy of the pre-repair state.  --baseline
is required (there is no default: the design-time snapshot lived in a
session Temp directory).

TWO MODES
    default      Valid ONLY in the rollback window: from the apply's COMMIT to
                 the first start of the engine or the GUI.  Every table is
                 compared with the baseline.  Refuses the live database while
                 an XOPTrader process runs (fail closed).
                   A  environment: integrity, foreign keys, schema unchanged,
                      the baseline is the exact pre-repair state
                   B  per-table row counts (only trade_log -3, ledger +10,
                      closure events +6)
                   C  sqlite_sequence (only ledger +10, closure events +6)
                   D  checksums of every row outside the touched set
                   E  trade_log: the three rows gone; P&L rehydrate aggregates
                      equal the baseline's without them; trade 1903 untouched
                   F  ledger: originals untouched; each fill leg reversed
                      exactly once; the DBX correction of adjust 2641;
                      per-asset balance changes; per-offer zero sums; the
                      SUM(adjust) measure; structural invariants
                   G  offer_log: the three rows cancelled with their cancel
                      cause and dead spend height, nothing else changed
                   H  closure events: two new events per offer (dead_on_chain
                      verdict, and the correction whose archived JSON equals
                      the deleted trade_log row)
                   I  untouched stores: inventory_state, taker_fills, snapshots
    --after-boot After the engine has run (it writes every table, so B, C, D,
                 E5, F5, F12, G2, G3 and I are skipped and the rest is limited
                 to rows the baseline had plus the repair rows).  Checks that
                 the three offers' records are still repaired, tolerating
                 the engine events v0.10.26 may append after the repair rows:
                 status/reconcile observations, an S14 reopen_observation
                 (cancelled -> cancel_pending, when the wallet reports
                 PENDING_CANCEL) and a re-close of the reopened row
                 (status_update cancel_pending -> cancelled with one of the
                 engine's reasons, apply_phantom_repair.ENGINE_RECLOSE_REASONS:
                 'dead_on_chain' once the fill proof finds it Dead, 'wallet
                 reported terminal' once the wallet flips it to CANCELLED,
                 and the S46 / Step 8 closers), in an order the engine can
                 write them (a reopen only from cancelled, a re-close only
                 from cancel_pending), with the offer_log state the last one
                 implies (a re-close stores its event's resolved_block, or the
                 first cancel-submit height when that is 0).  May run while
                 the engine runs (read-only).

Every read of the target happens inside ONE read transaction (BEGIN ...
ROLLBACK on a mode=ro connection), so an engine write during the check can
never produce a mixed view; in WAL mode the engine keeps writing meanwhile.

Expectations are derived from the BASELINE rows plus a few literals (offer
ids, spend heights, expected net ledger change, the 3,600 residual),
independently of the apply script's tables.  From the apply script (loaded
from this directory, with bytecode writing off so no __pycache__ appears
beside the pinned scripts) it takes only: REVERSAL_EVENT_TYPE and
CORRECTION_EVENT_TYPE, from which the SUM(event_type='adjust') expectation
is derived (one source of truth); the engine-event tolerance and replay
rule; its exact pre-state assertion (A5, on the baseline); and the path and
process guards (\\\\?\\, \\\\.\\, UNC, 8.3 and hard-link spellings are
refused, so the live-path test cannot be bypassed).  It makes no git call.

The target is opened mode=ro, the baseline mode=ro&immutable=1 (refused if
its -wal is not empty); nothing is written.
Exit 0 = every check PASS, 1 = at least one FAIL, 2 = refused.

Usage:
    python check_phantom_repair.py <target_db> --baseline <pre-repair db>
        [--allow-live-readonly] [--after-boot]
"""

# ruff: noqa: S608, N803, N806
# S608: only table/column identifiers and "?" placeholder lists are
#       interpolated; every value is bound.  N803/N806: the report object is
#       `R`, as in the first-pass version.

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import sqlite3
import sys
import time
from pathlib import Path

# Loading apply_phantom_repair.py must not write __pycache__ next to the
# pinned scripts.
sys.dont_write_bytecode = True

XCH = "xch"
DBX = "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20"
BYC = "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac"
NAMES = {XCH: "XCH", DBX: "DBX", BYC: "BYC",
         "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d": "wUSDC.b",
         "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6": "wmilliETH.b"}

# offer id -> (trade_log id, offer_log id, dead spend height per CHANGELOG v0.10.26)
PHANTOMS = {
    "0xd6a8325c15af4fdb2674061afdd613ffb6c240be5fb0ef23aab6ddf31262d0d6": (1900, 21185, 9324680),
    "0xdb63709cb9b2c3794cbf1a57bb15f2d5c1830dca620b16ac36926d0d60c6c556": (1901, 21311, 9325694),
    "0x83eef9df80a4d5557422c4e6b1789c8d7504e7e83759a47a818c103b8099511c": (1902, 21243, 9325004),
}
OFFERS = tuple(PHANTOMS)
TRADE_IDS = tuple(v[0] for v in PHANTOMS.values())
OFFER_LOG_IDS = tuple(v[1] for v in PHANTOMS.values())
DOWNSTREAM_TRADE = 1903          # a REAL take (proven on chain 2026-09-26)
ADJ_2641 = 2641
EXPECTED_NET = {XCH: 896841101167, DBX: 0, BYC: 1864}
EXPECTED_2641_RESIDUAL = 3600

REVERSAL_PREFIX = "reversal:"
CORRECTION_PREFIX = "correction:"
VERDICT_REASON = "dead_on_chain"
CORRECTION_CLOSURE_TYPE = "phantom_fill_correction"

REHYDRATE_SQL = """
    SELECT pair_name, COUNT(*), COALESCE(SUM(realized_pnl_mojos), 0),
           COALESCE(SUM(CASE WHEN realized_pnl_mojos > 0 THEN realized_pnl_mojos ELSE 0 END), 0),
           COALESCE(SUM(CASE WHEN realized_pnl_mojos < 0 THEN -realized_pnl_mojos ELSE 0 END), 0),
           COALESCE(SUM(fee_mojos), 0), MIN(timestamp), MAX(timestamp)
    FROM trade_log {where} GROUP BY pair_name ORDER BY pair_name
"""
# The engine's kQueryCancelSubmitBlock (database.cpp).
CANCEL_SUBMIT_SQL = """
    SELECT resolved_block FROM offer_closure_events
    WHERE offer_id = ?1 AND event_type = 'status_update'
      AND observed_status IN ('cancel_pending', 'cancelled')
      AND COALESCE(resolved_block, 0) > 0
    ORDER BY id ASC LIMIT 1
"""
ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")
CCOLS = ("id", "offer_id", "pair_name", "event_type", "previous_status",
         "observed_status", "closure_reason", "resolved_block", "created_at", "fee_mojos")
LCOLS = ("id", "entry_time", "event_type", "event_id", "leg", "asset_id",
         "delta_mojos", "pair_name", "block_height", "note", "created_at")


class Report:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        if ok:
            self.passed += 1
        else:
            self.failed += 1
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f" -- {detail}" if detail else ""))
        return ok

    @staticmethod
    def info(name: str, detail: str) -> None:
        print(f"INFO  {name} -- {detail}")

    @staticmethod
    def skip(name: str, why: str) -> None:
        print(f"SKIP  {name} -- {why}")

    @staticmethod
    def section(title: str) -> None:
        print(f"\n== {title}")


def qm(n: int) -> str:
    return ",".join("?" * n)


def rows(con, sql, args=()) -> list[tuple]:
    return [tuple(r) for r in con.execute(sql, args).fetchall()]


def one(con, sql, args=()):
    r = con.execute(sql, args).fetchone()
    return None if r is None else r[0]


def dict_rows(con, sql, args=()) -> list[dict]:
    cur = con.execute(sql, args)
    cols = [d[0] for d in cur.description]
    return [dict(zip(cols, r, strict=True)) for r in cur.fetchall()]


def tables(con) -> list[str]:
    return [r[0] for r in rows(con, "SELECT name FROM sqlite_master WHERE type='table' "
                                    "ORDER BY name")]


def digest(con, table: str, where: str = "", args=()) -> tuple[int, str]:
    """(row count, sha256 over every row incl. rowid, in rowid order)."""
    h = hashlib.sha256()
    n = 0
    cur = con.execute(f'SELECT rowid, * FROM "{table}" {where} ORDER BY rowid', args)
    while True:
        batch = cur.fetchmany(20000)
        if not batch:
            break
        for r in batch:
            h.update(repr(tuple(r)).encode())
            h.update(b"\n")
        n += len(batch)
    return n, h.hexdigest()


def asset_name(a: str) -> str:
    return NAMES.get(a, a[:8])


def load_apply_module():
    p = Path(__file__).resolve().with_name("apply_phantom_repair.py")
    spec = importlib.util.spec_from_file_location("apply_phantom_repair", p)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {p}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------

def run(tgt: sqlite3.Connection, base: sqlite3.Connection, R: Report, apr,
        after_boot: bool) -> None:
    strict = not after_boot
    ab_why = "after-boot: the engine writes this"
    b_ledger_max = one(base, "SELECT MAX(id) FROM ledger_entries")
    b_closure_max = one(base, "SELECT MAX(id) FROM offer_closure_events")
    b_trade_max = one(base, "SELECT MAX(id) FROM trade_log")

    # ---------------------------------------------------------------- A
    R.section("A. environment")
    ic = [r[0] for r in rows(tgt, "PRAGMA integrity_check")]
    R.check("A1 target integrity_check", ic == ["ok"], ", ".join(ic[:5]))
    fk = rows(tgt, "PRAGMA foreign_key_check")
    R.check("A2 target foreign_key_check", not fk, f"{len(fk)} violations")
    schema_sql = "SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY type, name"
    same_schema = rows(tgt, schema_sql) == rows(base, schema_sql)
    if strict:
        R.check("A3 schema (sqlite_master) identical to baseline", same_schema)
    else:
        R.info("A3 schema identical to baseline", f"{same_schema} (a migration at boot "
                                                  f"would change it; none is expected)")
    b_trades = rows(base, f"SELECT id, trade_id FROM trade_log WHERE id IN ({qm(3)}) "
                          f"ORDER BY id", TRADE_IDS)
    b_rev = one(base, f"SELECT COUNT(*) FROM ledger_entries WHERE event_id IN ({qm(3)})",
                tuple(REVERSAL_PREFIX + o for o in OFFERS))
    R.check("A4 baseline is the pre-repair state (trade_log 1900-1902 present, no "
            "reversal rows)",
            b_trades == sorted((PHANTOMS[o][0], o) for o in OFFERS) and b_rev == 0,
            f"trades={b_trades} reversal_rows={b_rev}")
    pre_problems, _ = apr.check_pre(base)
    R.check("A5 baseline passes the apply script's exact pre-state assertion",
            not pre_problems, "; ".join(pre_problems)[:400])

    all_tables = sorted(set(tables(tgt)) | set(tables(base)))
    # ---------------------------------------------------------------- B
    R.section("B. per-table row counts (target vs baseline)")
    if strict:
        want_delta = {"trade_log": -3, "ledger_entries": 10, "offer_closure_events": 6}
        for t in all_tables:
            if t not in tables(tgt) or t not in tables(base):
                R.check(f"B  {t}", False, "table missing on one side")
                continue
            bt = one(base, f'SELECT COUNT(*) FROM "{t}"')
            tt = one(tgt, f'SELECT COUNT(*) FROM "{t}"')
            d = want_delta.get(t, 0)
            R.check(f"B  {t} count", tt - bt == d, f"{bt} -> {tt} (delta {tt - bt:+d}, "
                                                      f"expected {d:+d})")
    else:
        R.skip("B", ab_why)

    # ---------------------------------------------------------------- C
    R.section("C. sqlite_sequence")
    if strict:
        bs = dict(rows(base, "SELECT name, seq FROM sqlite_sequence"))
        ts = dict(rows(tgt, "SELECT name, seq FROM sqlite_sequence"))
        want_seq = {"ledger_entries": 10, "offer_closure_events": 6}
        for name in sorted(set(bs) | set(ts)):
            d = (ts.get(name) or 0) - (bs.get(name) or 0)
            R.check(f"C  {name} seq", d == want_seq.get(name, 0),
                    f"{bs.get(name)} -> {ts.get(name)} (expected "
                    f"{want_seq.get(name, 0):+d})")
    else:
        R.skip("C", ab_why)

    # ---------------------------------------------------------------- D
    R.section("D. rows outside the touched set are byte-identical (sha256 per table)")
    if strict:
        special = {
            "trade_log": (f"WHERE rowid NOT IN ({qm(3)})", TRADE_IDS, "", ()),
            "offer_log": (f"WHERE rowid NOT IN ({qm(3)})", OFFER_LOG_IDS,
                          f"WHERE rowid NOT IN ({qm(3)})", OFFER_LOG_IDS),
            "ledger_entries": ("", (), "WHERE rowid <= ?", (b_ledger_max,)),
            "offer_closure_events": ("", (), "WHERE rowid <= ?", (b_closure_max,)),
        }
        for t in all_tables:
            if t == "sqlite_sequence" or t not in tables(tgt) or t not in tables(base):
                continue
            bw, ba, tw, ta = special.get(t, ("", (), "", ()))
            t0 = time.time()
            bd = digest(base, t, bw, ba)
            td = digest(tgt, t, tw, ta)
            scope = {"trade_log": "all but ids 1900-1902",
                     "offer_log": "all but ids 21185/21243/21311",
                     "ledger_entries": f"ids <= {b_ledger_max}",
                     "offer_closure_events": f"ids <= {b_closure_max}"}.get(t, "all rows")
            R.check(f"D  {t} ({scope})", bd == td,
                    f"{bd[0]} rows, {bd[1][:16]} vs {td[0]} rows, {td[1][:16]} "
                    f"[{time.time() - t0:.1f}s]")
    else:
        R.skip("D", ab_why)

    # ---------------------------------------------------------------- E
    R.section("E. trade_log")
    left = rows(tgt, f"SELECT id, trade_id FROM trade_log WHERE id IN ({qm(3)}) OR "
                     f"trade_id IN ({qm(3)}) OR offer_hash IN ({qm(3)})",
                TRADE_IDS + OFFERS + OFFERS)
    R.check("E1 no trade_log row for the three phantom offers", not left, repr(left))
    b1903 = rows(base, "SELECT * FROM trade_log WHERE id = ?", (DOWNSTREAM_TRADE,))
    t1903 = rows(tgt, "SELECT * FROM trade_log WHERE id = ?", (DOWNSTREAM_TRADE,))
    R.check("E2 trade 1903 (a real take, proven on chain) untouched; its inherited "
            "basis contamination is a documented residual",
            b1903 == t1903 and len(b1903) == 1)
    want = rows(base, REHYDRATE_SQL.format(where=f"WHERE id NOT IN ({qm(3)})"), TRADE_IDS)
    if strict:
        got = rows(tgt, REHYDRATE_SQL.format(where=""))
        scope = "every row"
    else:
        got = rows(tgt, REHYDRATE_SQL.format(where="WHERE id <= ?"), (b_trade_max,))
        scope = f"rows with id <= {b_trade_max}"
    R.check(f"E3 PnL rehydrate aggregates ({scope}) == baseline without 1900-1902 "
            f"(count, realized, gross profit/loss, fees, first/last ts per pair)",
            got == want, "" if got == want else f"got {got} want {want}")
    b_all = {r[0]: r for r in rows(base, REHYDRATE_SQL.format(where=""))}
    t_all = {r[0]: r for r in got}
    for pair, (dc, dr, df) in {"XCH/DBX": (-2, -44876, -30000000),
                               "XCH/BYC": (-1, 0, -15000000)}.items():
        b, t = b_all.get(pair), t_all.get(pair)
        ok = b is not None and t is not None and (t[1] - b[1], t[2] - b[2],
                                                  t[5] - b[5]) == (dc, dr, df)
        R.check(f"E4 {pair} fills/realized/fees change = {dc}/{dr}/{df}", ok,
                f"base {b[1:3] + (b[5],) if b else None} target "
                f"{t[1:3] + (t[5],) if t else None}")
    if strict:
        newest = rows(tgt, "SELECT id FROM trade_log WHERE pair_name = 'XCH/BYC' "
                           "ORDER BY id DESC LIMIT 1")
        want_newest = rows(base, f"SELECT id FROM trade_log WHERE pair_name = 'XCH/BYC' "
                                 f"AND id NOT IN ({qm(3)}) ORDER BY id DESC LIMIT 1",
                           TRADE_IDS)
        R.check("E5 newest XCH/BYC trade (GUI last-price fallback) is no longer the "
                "phantom", newest == want_newest, f"{newest} (expected {want_newest})")
    else:
        R.skip("E5", ab_why)

    # ---------------------------------------------------------------- F
    R.section("F. ledger_entries")
    sel = ", ".join(LCOLS)
    b_fill = dict_rows(base, f"SELECT {sel} FROM ledger_entries WHERE event_id IN "
                             f"({qm(3)}) ORDER BY id", OFFERS)
    t_fill = dict_rows(tgt, f"SELECT {sel} FROM ledger_entries WHERE event_id IN "
                            f"({qm(3)}) ORDER BY id", OFFERS)
    R.check("F1 the 9 original fill legs are untouched (append-only)",
            len(b_fill) == 9 and b_fill == t_fill,
            f"ids {[r['id'] for r in t_fill]}")
    b_adj = dict_rows(base, f"SELECT {sel} FROM ledger_entries WHERE id = ?", (ADJ_2641,))
    t_adj = dict_rows(tgt, f"SELECT {sel} FROM ledger_entries WHERE id = ?", (ADJ_2641,))
    R.check("F1 adjust 2641 is untouched (append-only)", len(b_adj) == 1 and b_adj == t_adj)
    corr_event_id = CORRECTION_PREFIX + (b_adj[0]["event_id"] if b_adj else "")

    repair_event_ids = tuple(REVERSAL_PREFIX + o for o in OFFERS) + (corr_event_id,)
    rep = dict_rows(tgt, f"SELECT {sel} FROM ledger_entries WHERE event_id IN "
                         f"({qm(len(repair_event_ids))}) ORDER BY id", repair_event_ids)
    rep_ids = {n["id"] for n in rep}
    new = dict_rows(tgt, f"SELECT {sel} FROM ledger_entries WHERE id > ? ORDER BY id",
                    (b_ledger_max,))
    if strict:
        R.check("F2 exactly 10 ledger rows appended, all of them repair rows",
                len(new) == 10 and {n["id"] for n in new} == rep_ids,
                f"{len(new)} new rows, {len(rep)} repair rows")
    else:
        R.check("F2 exactly 10 repair ledger rows, all appended after the baseline",
                len(rep) == 10 and all(i > b_ledger_max for i in rep_ids),
                f"{len(rep)} repair rows, ids {sorted(rep_ids)}")
    matched: set[int] = set()
    for leg in b_fill:
        cands = [n for n in rep
                 if n["event_type"] == apr.REVERSAL_EVENT_TYPE
                 and n["event_id"] == REVERSAL_PREFIX + leg["event_id"]
                 and n["leg"] == leg["leg"] and n["asset_id"] == leg["asset_id"]]
        ok = (len(cands) == 1
              and cands[0]["delta_mojos"] == -leg["delta_mojos"]
              and cands[0]["pair_name"] == leg["pair_name"]
              and cands[0]["block_height"] == leg["block_height"]
              and f"id={leg['id']} " in (cands[0]["note"] or "")
              and VERDICT_REASON in (cands[0]["note"] or ""))
        if len(cands) == 1:
            matched.add(cands[0]["id"])
        R.check(f"F3 fill leg {leg['id']} ({leg['event_id'][:12]} {leg['leg']} "
                f"{asset_name(leg['asset_id'])} {leg['delta_mojos']:+d}) reversed exactly "
                f"once ('{apr.REVERSAL_EVENT_TYPE}')", ok,
                f"reversal id {cands[0]['id']} delta {cands[0]['delta_mojos']:+d}"
                if len(cands) == 1 else f"{len(cands)} candidates")
    dbx_phantom = sum(r["delta_mojos"] for r in b_fill if r["asset_id"] == DBX)
    corr = [n for n in rep if b_adj and n["event_id"] == corr_event_id]
    ok = (len(corr) == 1 and corr[0]["event_type"] == apr.CORRECTION_EVENT_TYPE
          and corr[0]["leg"] == "adjust"
          and corr[0]["asset_id"] == DBX and corr[0]["delta_mojos"] == dbx_phantom
          and corr[0]["block_height"] == b_adj[0]["block_height"]
          and f"id={ADJ_2641}" in (corr[0]["note"] or ""))
    if len(corr) == 1:
        matched.add(corr[0]["id"])
    R.check(f"F4 one DBX correction of adjust 2641 for +{dbx_phantom} (the phantom DBX "
            f"it absorbed)", ok, repr([(c['id'], c['event_type'], c['delta_mojos'])
                                       for c in corr]))
    if strict:
        R.check("F5 no other ledger row appended", matched == {n["id"] for n in new},
                f"unexplained new ids {sorted({n['id'] for n in new} - matched)}")
    else:
        R.skip("F5", ab_why)
    times = {n["entry_time"] for n in rep}
    b_maxtime = one(base, "SELECT MAX(entry_time) FROM ledger_entries")
    R.check("F6 repair rows share one ISO-8601 UTC entry_time after every baseline row",
            len(times) == 1 and all(ISO_RE.match(t or "") for t in times)
            and min(times) > (b_maxtime or ""), f"{sorted(times)} vs baseline max {b_maxtime}")

    for o in OFFERS:
        per = rows(tgt, "SELECT asset_id, SUM(delta_mojos) FROM ledger_entries WHERE "
                        "event_id IN (?, ?) GROUP BY asset_id", (o, REVERSAL_PREFIX + o))
        R.check(f"F7 {o[:12]}: fill legs + reversal net to zero per asset",
                bool(per) and all(s == 0 for _, s in per),
                ", ".join(f"{asset_name(a)} {s}" for a, s in per))
    pair_sum = one(tgt, "SELECT SUM(delta_mojos) FROM ledger_entries WHERE id = ? OR "
                        "event_id = ?", (ADJ_2641, corr_event_id))
    R.check(f"F8 adjust 2641 + its correction = +{EXPECTED_2641_RESIDUAL} (the part "
            f"still unexplained)", pair_sum == EXPECTED_2641_RESIDUAL, f"{pair_sum}")

    # In after-boot mode, limit the target to the baseline's rows + the repair.
    if strict:
        t_where, t_args = "", ()
    else:
        t_where = f"WHERE id <= ? OR id IN ({qm(len(rep_ids))})"
        t_args = (b_ledger_max,) + tuple(sorted(rep_ids))
    b_bal = dict(rows(base, "SELECT asset_id, SUM(delta_mojos) FROM ledger_entries "
                            "GROUP BY asset_id"))
    t_bal = dict(rows(tgt, f"SELECT asset_id, SUM(delta_mojos) FROM ledger_entries "
                           f"{t_where} GROUP BY asset_id", t_args))
    for a in sorted(set(b_bal) | set(t_bal)):
        d = (t_bal.get(a) or 0) - (b_bal.get(a) or 0)
        R.check(f"F9 ledger balance change {asset_name(a)}", d == EXPECTED_NET.get(a, 0),
                f"{b_bal.get(a)} -> {t_bal.get(a)} ({d:+d}, expected "
                f"{EXPECTED_NET.get(a, 0):+d})")
    # SUM(adjust), the engine's 'unaccounted for' measure.  The expectation is
    # derived from the apply script's event types: the DBX correction counts
    # when CORRECTION_EVENT_TYPE is 'adjust', the reversals when
    # REVERSAL_EVENT_TYPE is.
    want_kpi: dict[str, int] = {}
    if apr.REVERSAL_EVENT_TYPE == "adjust":
        for r in b_fill:
            want_kpi[r["asset_id"]] = want_kpi.get(r["asset_id"], 0) - r["delta_mojos"]
    if apr.CORRECTION_EVENT_TYPE == "adjust":
        want_kpi[DBX] = want_kpi.get(DBX, 0) + dbx_phantom
    kpi_b = "SELECT asset_id, SUM(delta_mojos) FROM ledger_entries WHERE event_type = " \
            "'adjust' GROUP BY asset_id"
    kpi_t = ("SELECT asset_id, SUM(delta_mojos) FROM ledger_entries WHERE event_type = "
             "'adjust'" + (f" AND ({t_where[6:]})" if t_where else "")
             + " GROUP BY asset_id")
    b_kpi, t_kpi = dict(rows(base, kpi_b)), dict(rows(tgt, kpi_t, t_args))
    for a in sorted(set(b_kpi) | set(t_kpi) | set(want_kpi)):
        d = (t_kpi.get(a) or 0) - (b_kpi.get(a) or 0)
        want_d = want_kpi.get(a, 0)
        R.check(f"F10 SUM(adjust) 'unaccounted' measure change {asset_name(a)}",
                d == want_d, f"{d:+d} (expected {want_d:+d}, derived from "
                             f"REVERSAL_EVENT_TYPE='{apr.REVERSAL_EVENT_TYPE}')")
    dup = rows(tgt, "SELECT event_id, leg, asset_id, COUNT(*) FROM ledger_entries "
                    "GROUP BY event_id, leg, asset_id HAVING COUNT(*) > 1")
    R.check("F11 UNIQUE(event_id, leg, asset_id) holds", not dup, repr(dup[:3]))
    multi = rows(tgt, "SELECT event_id FROM ledger_entries GROUP BY event_id "
                      "HAVING COUNT(DISTINCT event_type) > 1")
    R.check("F11 every event_id has a single event_type", not multi, repr(multi[:3]))
    op_sql = "SELECT * FROM ledger_entries WHERE event_type = 'opening' AND id <= ? ORDER BY id"
    R.check("F11 opening (genesis) rows untouched",
            rows(base, op_sql, (b_ledger_max,)) == rows(tgt, op_sql, (b_ledger_max,))
            and len(rows(tgt, op_sql, (b_ledger_max,))) > 0)
    if strict:
        inv = dict(rows(tgt, "SELECT asset_id, total_quantity FROM inventory_state"))
        b_inv = dict(rows(base, "SELECT asset_id, total_quantity FROM inventory_state"))
        b_gap = (b_bal.get(DBX) or 0) - (b_inv.get(DBX) or 0)
        t_gap = (t_bal.get(DBX) or 0) - (inv.get(DBX) or 0)
        R.check("F12 DBX ledger-minus-tracker gap unchanged by the repair (the tracker "
                "and the ledger were both tied to the same wallet read around 09-23 "
                "01:17; the snapshot gap is 0)",
                t_gap == b_gap, f"{b_gap:+d} -> {t_gap:+d} (ledger {t_bal.get(DBX)}, "
                                f"tracker {inv.get(DBX)})")
        for a in (XCH, BYC):
            R.info(f"F12 {asset_name(a)} ledger minus tracker quantity",
                   f"{(b_bal.get(a) or 0) - (b_inv.get(a) or 0):+d} -> "
                   f"{(t_bal.get(a) or 0) - (inv.get(a) or 0):+d} mojos; the engine ties "
                   f"the ledger to the WALLET's confirmed balance, so compare the wallet "
                   f"with the ledger sum ({t_bal.get(a)}) before the first boot")
    else:
        R.skip("F12", "after-boot: Step 11 re-ties the tracker at boot")

    # ---------------------------------------------------------------- H (collect)
    csel = ", ".join(CCOLS)
    newc = rows(tgt, f"SELECT {csel} FROM offer_closure_events WHERE id > ? AND offer_id "
                     f"IN ({qm(3)}) ORDER BY id", (b_closure_max,) + OFFERS)
    per_offer: dict[str, dict] = {}
    for o in OFFERS:
        evs = [e for e in newc if e[1] == o]
        verdict = [e for e in evs if e[3] == "status_update" and e[4] == "filled"
                   and e[5] == "cancelled" and e[6] == VERDICT_REASON]
        corr_c = [e for e in evs if e[3] == CORRECTION_CLOSURE_TYPE]
        rep_c = verdict + corr_c
        hi = max((e[0] for e in rep_c), default=0)
        others = [e for e in evs if e not in rep_c]
        engine = [e for e in others if e[0] > hi and apr.is_tolerated_post_event(e)]
        stray = [e for e in others if e not in engine]
        # Replay the engine's events from the repaired 'cancelled' row: the
        # order must be one the engine can write, and the last change says
        # what offer_log must hold (None: no event changed the row).
        seq_problems, expected = apr.replay_engine_events(
            engine, apr.cancel_submit_block(tgt, o))
        per_offer[o] = dict(verdict=verdict, corr=corr_c, engine=engine, stray=stray,
                            seq_problems=seq_problems, expected=expected)

    # ---------------------------------------------------------------- G
    R.section("G. offer_log")
    ocols = [r[1] for r in rows(base, "PRAGMA table_info(offer_log)")]
    for o, (_tid, oid, h) in PHANTOMS.items():
        b = dict_rows(base, "SELECT * FROM offer_log WHERE offer_id = ?", (o,))
        t = dict_rows(tgt, "SELECT * FROM offer_log WHERE offer_id = ?", (o,))
        cause = one(base, "SELECT closure_reason FROM offer_closure_events WHERE "
                          "offer_id = ? AND observed_status = 'cancel_pending' "
                          "ORDER BY id LIMIT 1", (o,))
        if len(b) != 1 or len(t) != 1:
            R.check(f"G1 offer_log {oid}", False, f"rows base={len(b)} target={len(t)}")
            continue
        b, t = b[0], t[0]
        expected = per_offer[o]["expected"] if after_boot else None
        R.check(f"G1 offer_log {oid} cancel_reason restored to the cancel cause",
                bool(cause) and t["cancel_reason"] == cause,
                f"'{b['cancel_reason']}' -> '{t['cancel_reason']}' (cause '{cause}')")
        free = ("status", "cancel_reason") + (("resolved_block", "resolved_at")
                                              if expected is not None else ())
        others = [c for c in ocols if c not in free and b[c] != t[c]]
        R.check(f"G1 offer_log {oid} every other column unchanged", not others,
                f"changed: {others}")
        if expected is None:
            R.check(f"G1 offer_log {oid} ({o[:12]}) status cancelled",
                    t["status"] == "cancelled", f"{b['status']} -> {t['status']}")
            R.check(f"G1 offer_log {oid} resolved_block = dead spend height {h}",
                    t["resolved_block"] == h == b["resolved_block"], f"{t['resolved_block']}")
            continue
        st, blk, rat_null, desc = expected
        seq = per_offer[o]["seq_problems"]
        ok = (not seq and t["status"] == st and t["resolved_block"] == blk
              and (t["resolved_at"] is None) == rat_null)
        R.check(f"G1 offer_log {oid} consistent with the engine's events after the "
                f"repair: {desc}", ok,
                f"status {t['status']} resolved_block {t['resolved_block']} resolved_at "
                f"{t['resolved_at']} (expected {st}, {blk}, resolved_at "
                f"{'NULL' if rat_null else 'set'})"
                + (f"; order: {'; '.join(seq)}" if seq else ""))
        if st == "cancel_pending":
            R.info(f"G1 offer_log {oid}", "REOPENED by the engine (the wallet reports "
                                          "PENDING_CANCEL); expect a re-close "
                                          "('dead_on_chain' once the fill proof finds it "
                                          "Dead, or 'wallet reported terminal')")
    if strict:
        bst = dict(rows(base, "SELECT status, COUNT(*) FROM offer_log GROUP BY status"))
        tst = dict(rows(tgt, "SELECT status, COUNT(*) FROM offer_log GROUP BY status"))
        want_st = dict(bst)
        want_st["filled"] = want_st.get("filled", 0) - 3
        want_st["cancelled"] = want_st.get("cancelled", 0) + 3
        R.check("G2 status counts: filled -3, cancelled +3, others unchanged",
                tst == want_st, f"{bst} -> {tst}")
        pend = ("SELECT offer_id FROM offer_log WHERE status IN ('pending', "
                "'cancel_pending') ORDER BY created_block, offer_id")
        R.check("G3 the engine's restore-into-State set (pending/cancel_pending) is "
                "unchanged", rows(base, pend) == rows(tgt, pend))
    else:
        R.skip("G2/G3", ab_why)

    # ---------------------------------------------------------------- H
    R.section("H. offer_closure_events")
    b_old = rows(base, f"SELECT {csel} FROM offer_closure_events WHERE offer_id IN "
                       f"({qm(3)}) ORDER BY id", OFFERS)
    t_old = rows(tgt, f"SELECT {csel} FROM offer_closure_events WHERE offer_id IN "
                      f"({qm(3)}) AND id <= ? ORDER BY id", OFFERS + (b_closure_max,))
    R.check("H1 the offers' existing closure events are untouched", b_old == t_old,
            f"{len(b_old)} events")
    if strict:
        all_new = rows(tgt, f"SELECT {csel} FROM offer_closure_events WHERE id > ? "
                            f"ORDER BY id", (b_closure_max,))
        R.check("H2 exactly 6 closure events appended, a verdict and a correction per "
                "offer, none for other offers",
                len(all_new) == 6 and all(len(v["verdict"]) == 1 and len(v["corr"]) == 1
                                          for v in per_offer.values()),
                f"{len(all_new)} new: {[(e[0], e[1][:12], e[3]) for e in all_new]}")
    else:
        ok = all(len(v["verdict"]) == 1 and len(v["corr"]) == 1 and not v["stray"]
                 and not v["seq_problems"] for v in per_offer.values())
        R.check("H2 a verdict and a correction per offer; every later event for the "
                "offers is a tolerated engine event, in an order the engine can write",
                ok,
                "; ".join(f"{o[:12]}: {len(v['verdict'])}+{len(v['corr'])} repair, "
                          f"{len(v['engine'])} engine, stray "
                          f"{[(e[0], e[3], e[4], e[5], e[6]) for e in v['stray']]}"
                          + (f", order {v['seq_problems']}" if v["seq_problems"] else "")
                          for o, v in per_offer.items()))
        for o, v in per_offer.items():
            for e in v["engine"]:
                R.info(f"H2 {o[:12]} engine event {e[0]}",
                       f"{e[3]} {e[4]}->{e[5]} '{e[6]}' at {e[7]}")
    for o, (tid, _oid, h) in PHANTOMS.items():
        pair = one(base, "SELECT pair_name FROM offer_log WHERE offer_id = ?", (o,))
        cause = one(base, "SELECT closure_reason FROM offer_closure_events WHERE "
                          "offer_id = ? AND observed_status = 'cancel_pending' "
                          "ORDER BY id LIMIT 1", (o,))
        v = per_offer[o]
        verdict = v["verdict"]
        ok = (len(verdict) == 1 and verdict[0][7] == h and verdict[0][9] is None
              and verdict[0][2] == pair)
        R.check(f"H3 {o[:12]}: status_update filled->cancelled '{VERDICT_REASON}' at {h} "
                f"(v0.10.26's dead verdict; previous_status 'filled' marks the repair "
                f"row)", ok, repr([e[3:8] for e in verdict]))
        archive_ok, detail = False, f"{len(v['corr'])} correction events"
        if len(v["corr"]) == 1:
            c = dict(zip(CCOLS, v["corr"][0], strict=True))
            reason = c["closure_reason"] or ""
            base_row = dict_rows(base, "SELECT * FROM trade_log WHERE id = ?", (tid,))
            try:
                start = reason.index("original row: ") + len("original row: ")
                archived, _ = json.JSONDecoder().raw_decode(reason, start)
            except ValueError as ex:
                archived, detail = None, f"no parsable archived row ({ex})"
            archive_ok = (c["previous_status"] == "filled"
                          and c["observed_status"] == "cancelled"
                          and reason.startswith(VERDICT_REASON)
                          and bool(cause) and cause in reason
                          and (REVERSAL_PREFIX + o) in reason
                          and c["resolved_block"] == h and c["fee_mojos"] is None
                          and c["pair_name"] == pair
                          and len(base_row) == 1 and archived == base_row[0])
            if archived is not None:
                detail = ("archived row == baseline trade_log row"
                          if base_row and archived == base_row[0]
                          else f"archived {archived} vs baseline {base_row}")
        R.check(f"H4 {o[:12]}: phantom_fill_correction archives trade_log {tid} exactly",
                archive_ok, detail)
        t_status = one(tgt, "SELECT status FROM offer_log WHERE offer_id = ?", (o,))
        if strict:
            last = rows(tgt, "SELECT observed_status FROM offer_closure_events WHERE "
                             "offer_id = ? ORDER BY id DESC LIMIT 1", (o,))
            R.check(f"H5 {o[:12]}: latest closure event says cancelled",
                    last == [("cancelled",)], repr(last))
        else:
            last = rows(tgt, "SELECT id, event_type, observed_status FROM "
                             "offer_closure_events WHERE offer_id = ? AND event_type "
                             "NOT IN ('status_observation', 'reconcile_observation') "
                             "ORDER BY id DESC LIMIT 1", (o,))
            R.check(f"H5 {o[:12]}: latest status-changing event matches the offer_log "
                    f"status", bool(last) and last[0][2] == t_status,
                    f"{last} vs offer_log '{t_status}'")
        R.check(f"H6 {o[:12]}: cancel-submit height (S14 fallback) unchanged",
                one(base, CANCEL_SUBMIT_SQL, (o,)) == one(tgt, CANCEL_SUBMIT_SQL, (o,)),
                f"{one(tgt, CANCEL_SUBMIT_SQL, (o,))}")
    rep_c = [e for v in per_offer.values() for e in v["verdict"] + v["corr"]]
    esc = [e[0] for e in rep_c if (e[6] or "").startswith("cancel_escalation_")]
    R.check("H6 no repair event counts as a cancel escalation", not esc, repr(esc))
    if after_boot:
        esc_e = [(e[0], e[1][:12], e[6]) for v in per_offer.values() for e in v["engine"]
                 if (e[6] or "").startswith("cancel_escalation_")]
        if esc_e:
            R.info("H6 engine cancel-escalation events for the offers (a fee may have "
                   "been paid on a reopened row)", repr(esc_e))

    # ---------------------------------------------------------------- I
    R.section("I. stores deliberately left alone")
    ref = rows(tgt, f"SELECT id FROM taker_fills WHERE trade_id IN ({qm(3)}) OR "
                    f"counterparty_offer_id IN ({qm(3)})", OFFERS + OFFERS)
    R.check("I  no taker_fills row references the three offers", not ref, repr(ref))
    if strict:
        for t, why in (("inventory_state", "quantities re-tied to the wallet by Step 11 "
                                           "once per asset per process; the basis "
                                           "residual is documented"),
                       ("taker_fills", "no row references the offers"),
                       ("snapshots", "derived history, never read back")):
            R.check(f"I  {t} identical ({why})", digest(base, t) == digest(tgt, t))
    else:
        R.skip("I  inventory_state/taker_fills/snapshots identical", ab_why)


def main(argv: list[str]) -> int:
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:  # noqa: S110 -- a stream without reconfigure keeps its default
        pass
    ap = argparse.ArgumentParser(
        description="Read-only checks for the phantom-fill repair.",
        epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("db_path", help="the repaired database")
    ap.add_argument("--baseline", required=True,
                    help="the pre-repair database: the apply's --backup-to file")
    ap.add_argument("--allow-live-readonly", action="store_true",
                    help="allow the target to be the live (or fake-live) database, "
                         "opened mode=ro")
    ap.add_argument("--after-boot", action="store_true",
                    help="the engine has run since the repair: check only the three "
                         "offers' records (see TWO MODES)")
    args = ap.parse_args(argv)

    if sys.platform != "win32":
        print("REFUSED: this one-off runs only on the Windows host.")
        return 2
    try:
        apr = load_apply_module()
    except Exception as ex:
        print(f"REFUSED: cannot load apply_phantom_repair.py from this directory "
              f"({type(ex).__name__}: {ex})")
        return 2
    print(f"checker  : {Path(__file__).resolve()}  "
          f"({'after-boot' if args.after_boot else 'strict (rollback window)'})")
    pins_ok, pin_lines = apr.pin_report()
    for line in pin_lines:
        print(line)
    if not pins_ok:
        print("WARNING  : the scripts do not match SHA256SUMS")

    try:
        fake = apr.fake_live_dir()
        norm = {}
        for label, p in (("target", args.db_path), ("baseline", args.baseline)):
            a = apr.plain_abspath(p, label)
            n = apr._nc(a)
            live = apr._under(n, apr._nc(apr.LIVE_DATA_DIR)) or bool(
                fake and apr._under(n, apr._nc(fake)))
            if live and label == "baseline":
                raise apr.Refused(f"baseline {a} is in the (fake) live data directory; "
                                  f"pass the pre-repair backup")
            if live and not args.allow_live_readonly:
                raise apr.Refused(f"target {a} is the (fake) live database; pass "
                                  f"--allow-live-readonly to read it (mode=ro)")
            norm[label] = (a, live)
        if norm["target"][1] and not args.after_boot:
            try:
                procs = apr.xoptrader_processes()
            except Exception as ex:
                raise apr.Refused(f"could not list processes ({ex})") from None
            if procs:
                raise apr.Refused("XOPTrader processes run, so the strict checks (valid "
                                  "only between COMMIT and the first engine start) would "
                                  "fail on the engine's own writes; after the first "
                                  "start use --after-boot:\n  " + "\n  ".join(procs))
        real_t = apr.verify_on_disk(norm["target"][0], "target")
        real_b = apr.verify_on_disk(norm["baseline"][0], "baseline")
        if os.path.samefile(real_t, real_b):
            raise apr.Refused("target and baseline are the same file")
        wal_b = real_b + "-wal"
        if os.path.exists(wal_b) and os.path.getsize(wal_b) > 0:
            raise apr.Refused(f"{wal_b} is not empty: the baseline is not a closed, "
                              f"self-contained copy")
    except apr.Refused as ex:
        print(f"REFUSED: {ex}")
        return 2

    t0 = time.time()
    print(f"target   : {real_t}\nbaseline : {real_b}  (sha256 {apr.file_sha256(real_b)})")
    # isolation_level=None: nothing implicit; the explicit BEGIN below plus the
    # first read pin ONE snapshot of the target for every check.
    tgt = sqlite3.connect(apr.sqlite_uri(real_t, "ro"), uri=True, isolation_level=None)
    base = sqlite3.connect(apr.sqlite_uri(real_b, "ro", immutable=True), uri=True)
    R = Report()
    try:
        tgt.execute("BEGIN")
        n_tables = one(tgt, "SELECT COUNT(*) FROM sqlite_master")   # pins the snapshot
        print(f"snapshot : every target read below is in one read transaction "
              f"({n_tables} schema objects at BEGIN)")
        run(tgt, base, R, apr, args.after_boot)
        held = tgt.in_transaction
        R.check("single read transaction held to the end", held,
                "" if held else "the target connection left its transaction early")
    except Exception as ex:
        R.check("run completed", False, f"{type(ex).__name__}: {ex}")
    finally:
        try:
            if tgt.in_transaction:
                tgt.execute("ROLLBACK")
        finally:
            tgt.close()
            base.close()
    print(f"\n{R.passed} PASS, {R.failed} FAIL  ({time.time() - t0:.0f}s)")
    return 0 if R.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
