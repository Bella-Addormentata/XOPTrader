#!/usr/bin/env python3
"""Fill-completeness sweep: every CONFIRMED wallet trade must be recorded.

The single invariant that catches both April-June era omission mechanisms
(maker fills misfiled as cancels, fixed cae2bfd; taker trades recorded
nowhere, fixed 62ee4cb) forever:

    Every wallet trade record that reached status CONFIRMED -- both
    is_my_offer=true (maker) and is_my_offer=false (taker) -- must exist
    in trade_log or taker_fills.

This script pages the full offer/trade history out of the local Chia wallet
RPC (read-only), loads the recorded trade_ids from the engine database
(read-only), and reports every CONFIRMED wallet trade that is absent from
both tables.  Nothing is ever written -- if the sweep FAILs, investigate the
missing trades; do not blindly backfill.

Usage:
    .venv/Scripts/python.exe scripts/verify_fill_completeness.py
    .venv/Scripts/python.exe scripts/verify_fill_completeness.py --since-days 7
    .venv/Scripts/python.exe scripts/verify_fill_completeness.py --since-height 9080132
    .venv/Scripts/python.exe scripts/verify_fill_completeness.py --strict

Exit codes: 0 = PASS, 1 = FAIL (missing trades listed), 2 = operational
error (wallet RPC unreachable, database missing, ...), 3 = --strict only:
nothing is missing, but at least one offer was excluded as dead on-chain
(see below).  Missing trades exit 1 with or without --strict.

Requires the Chia wallet to be running and synced (localhost:9256, client
certs from ~/.chia/mainnet/config/ssl/wallet).  A grace window (default 10
minutes, --grace-minutes) skips trades whose accepted_at_time (else
created_at_time) is that recent, which the engine may legitimately not have
persisted yet.  A maker offer posted earlier and taken moments ago is not in
it, so a sweep run straight after a take can list that take as missing: run
it again a few minutes later before investigating.

The --since-days window.  The wallet records no confirmation TIME.  A taker
trade carries accepted_at_time; a maker fill usually carries only
created_at_time, when the offer was posted, which can be hours or days before
it was taken.  Both are at or before the confirmation, so a time inside the
window puts a trade in scope, but a time before the window proves nothing.
The window therefore also has a start HEIGHT: the wallet's current height
minus N days at 4,608 blocks a day (Chia's peak-height cadence, one block per
18.75 s; NOT the ~52 s spacing of transaction blocks), widened by 10% so that
it reaches a little further back than N days, never less.  A CONFIRMED record
is checked when its time is inside the window OR its confirmed_at_index is at
or above the start height, so an offer posted before the window and taken
inside it IS checked.  Pick N by when the trades CONFIRMED, not when the
offers were posted.  A record with no confirmed_at_index (0) falls back to
its time alone.  --since-height H checks only records whose
confirmed_at_index is at least H (a record without one is not checked).

Offers proven dead on-chain are excluded, not missing.  The wallet's
CONFIRMED is its own bookkeeping, not evidence: on 2026-09-22 it reported
three offers CONFIRMED that nobody took (docs/PHANTOM-FILL-REPAIR-2026-09.md).
Since v0.10.26 (PR #171) the engine books a fill only on on-chain proof, and
it records an offer the chain proves was never taken as a closure event with
event_type 'status_update' and closure_reason exactly 'dead_on_chain'.  An
offer with such an event is listed separately as excluded and never counts
as missing.  The filter is on that exact event: the phantom-fill repair's
'phantom_fill_correction' events also have a reason starting 'dead_on_chain',
and offers are counted once each however many events they have.

The exclusion takes the engine's own verdict on trust, and this sweep exists
to check the engine.  So every excluded offer is printed in a WARN section
with the read-only command that re-proves it from the chain, independently
of the engine:

    python scripts/oneoff/phantom_fill_repair_2026_09/prove_fill_on_chain.py <trade_id>

It needs the local wallet and full node.  Expect "VERDICT: Dead".  Any other
verdict means the engine's verdict is in doubt and the offer may be a real,
unrecorded fill: investigate it as a missing one.  Without --strict an
exclusion does not change the exit code (a PASS still exits 0, and says
"PASS with WARN").  With --strict any exclusion exits nonzero: 3, or 1 when
something is also missing.  An automated caller that must not trust the
engine's verdict passes --strict.

A dead_on_chain verdict on an offer_log row that was already closed is
written as a 'status_observation' event instead (Database::
update_offer_status keeps a closed row's status).  Such an offer is NOT
excluded here.  If it is CONFIRMED and unrecorded, the sweep reports it
missing.  That errs towards a FAIL to investigate, never towards a false PASS.
"""

from __future__ import annotations

import argparse
import math
import os
import sqlite3
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import requests
import urllib3

REPO_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = REPO_ROOT / "data" / "xop_trader.db"
# Read-only on-chain proof of one offer (wallet get_offer + full node coin
# records), independent of the engine's verdict.  Printed for every
# excluded offer; never run from here.
PROVE_SCRIPT = (REPO_ROOT / "scripts" / "oneoff" / "phantom_fill_repair_2026_09"
                / "prove_fill_on_chain.py")

WALLET_RPC_URL = "https://localhost:9256/"
RPC_PAGE_SIZE = 200
RPC_MAX_RECORDS = 50_000

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_ERROR = 2
# --strict only: nothing is missing, but an offer was excluded on the
# engine's dead_on_chain verdict.  A missing trade exits EXIT_FAIL instead.
EXIT_STRICT_EXCLUDED = 3

# Chia's peak-height cadence: 32 blocks per 10 minutes, one per 18.75 s.
# confirmed_at_index counts these.  NOT the ~52 s transaction-block spacing
# (block_time_seconds in the engine config), which is a third of the rate.
BLOCKS_PER_DAY = 4608
# The --since-days start height reaches this much further back than the
# nominal cadence, so a chain running faster than target cannot shrink the
# window below N days: 7 days -> 35,482 blocks, about 7.7 days at target.
WINDOW_HEIGHT_MARGIN = 1.10

# Asset-id -> display name (verified against cat_get_asset_id during the
# 2026-08 forensic investigation).  Unknown ids display as their first 8 hex
# chars.  XCH uses 1e12 mojos/unit; every CAT uses 1e3.
ASSET_NAMES: dict[str, str] = {
    "xch": "XCH",
    "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d": "wUSDC.b",
    "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac": "BYC",
    "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20": "DBX",
}
MOJOS_PER_XCH = 1_000_000_000_000
MOJOS_PER_CAT = 1_000

# The engine's verdict for an offer the chain proved was never taken
# (engine.cpp step_process_fills, v0.10.26): update_offer_status(id,
# "cancelled", spent_height, "dead_on_chain") on a row it changes appends
# exactly this event.  Exact match on both columns: the repair's
# 'phantom_fill_correction' events carry a longer reason that STARTS with
# 'dead_on_chain', and must not be what excludes an offer.
DEAD_ON_CHAIN_EVENT_TYPE = "status_update"
DEAD_ON_CHAIN_REASON = "dead_on_chain"
DEAD_ON_CHAIN_SQL = (
    "SELECT DISTINCT offer_id FROM offer_closure_events "
    "WHERE event_type = ? AND closure_reason = ? AND offer_id IS NOT NULL"
)


def _norm_id(trade_id: str) -> str:
    tid = trade_id.lower()
    return tid[2:] if tid.startswith("0x") else tid


def _asset_name(key: str) -> str:
    k = _norm_id(key)
    return ASSET_NAMES.get(k, k[:8])


def _asset_units(name: str, value: object) -> float:
    if isinstance(value, dict):
        value = value.get("amount", 0)
    denom = MOJOS_PER_XCH if name == "XCH" else MOJOS_PER_CAT
    return int(value) / denom


def _summarize_flows(record: dict) -> str:
    summary = record.get("summary") or {}

    def fmt(section: str) -> str:
        parts = []
        for key, val in (summary.get(section) or {}).items():
            name = _asset_name(key)
            parts.append(f"{_asset_units(name, val):.6g} {name}")
        return " + ".join(parts) if parts else "?"

    return f"{fmt('offered')} -> {fmt('requested')}"


def _record_time(record: dict) -> int:
    """Best-effort unix time: accepted_at_time when set, else created_at_time.

    Some confirmed maker fills lack accepted_at_time (observed throughout the
    April-June era), so created_at_time is the fallback.  Either is at or
    BEFORE the confirmation: an offer can be posted days before it is taken.
    """
    return int(record.get("accepted_at_time") or record.get("created_at_time") or 0)


def _confirmed_height(record: dict) -> int:
    """The record's confirmed_at_index; 0 when the wallet gives none."""
    return int(record.get("confirmed_at_index") or 0)


def window_start_height(peak_height: int, days: float) -> int:
    """Lowest confirmed_at_index a --since-days window of `days` checks.

    `days` of blocks below the wallet's peak at the nominal cadence, widened
    by WINDOW_HEIGHT_MARGIN: the window may reach a little further back than
    asked, never less.
    """
    span = math.ceil(days * BLOCKS_PER_DAY * WINDOW_HEIGHT_MARGIN)
    return max(0, peak_height - span)


def _in_time_window(rec: dict, since_time: float | None,
                    window_height: int | None) -> bool:
    """Whether a record may have confirmed inside the --since-days window.

    A time (accepted, else created) inside the window settles it: the
    confirmation came at or after it.  A time before the window does not: a
    maker offer posted before the window can be taken inside it, and then only
    its confirmed_at_index says so.  A record without one (0) is below any
    window that starts above genesis, so its time alone decides.
    """
    if since_time is None:
        return True
    if _record_time(rec) >= since_time:
        return True
    return window_height is not None and _confirmed_height(rec) >= window_height


class WalletRpc:
    def __init__(self) -> None:
        ssl_dir = os.path.join(
            os.path.expanduser("~"), ".chia", "mainnet", "config", "ssl"
        )
        self._cert = (
            os.path.join(ssl_dir, "wallet", "private_wallet.crt"),
            os.path.join(ssl_dir, "wallet", "private_wallet.key"),
        )
        urllib3.disable_warnings()

    def call(self, method: str, payload: dict | None = None) -> dict:
        resp = requests.post(
            WALLET_RPC_URL + method,
            json=payload or {},
            cert=self._cert,
            verify=False,
            timeout=300,
        )
        resp.raise_for_status()
        return resp.json()

    def fetch_all_trade_records(self) -> list[dict]:
        """Page every trade record (maker and taker) out of the wallet."""
        records: list[dict] = []
        start = 0
        while start < RPC_MAX_RECORDS:
            resp = self.call(
                "get_all_offers",
                {
                    "start": start,
                    "end": start + RPC_PAGE_SIZE,
                    "include_completed": True,
                    "file_contents": False,
                },
            )
            page = resp.get("trade_records", [])
            records.extend(page)
            if len(page) < RPC_PAGE_SIZE:
                break
            start += RPC_PAGE_SIZE
        return records


def _load_dead_on_chain_ids(con: sqlite3.Connection) -> set[str]:
    """Distinct offer ids the engine recorded as proven dead on-chain.

    A database without offer_closure_events (older than the table) has no
    such verdicts, so it excludes nothing.  That makes the sweep stricter,
    never looser.
    """
    has_table = con.execute(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' "
        "AND name = 'offer_closure_events'"
    ).fetchone()
    if not has_table:
        return set()
    return {
        _norm_id(o)
        for (o,) in con.execute(
            DEAD_ON_CHAIN_SQL, (DEAD_ON_CHAIN_EVENT_TYPE, DEAD_ON_CHAIN_REASON)
        )
    }


def _load_recorded_ids(db_path: Path) -> tuple[set[str], set[str], set[str]]:
    """(trade_log ids, taker_fills ids, dead_on_chain offer ids), normalized."""
    uri = f"file:{db_path.as_posix()}?mode=ro"
    con = sqlite3.connect(uri, uri=True, timeout=10)
    try:
        con.execute("PRAGMA busy_timeout = 10000")
        cur = con.cursor()
        trade_log_ids = {
            _norm_id(t) for (t,) in cur.execute("SELECT trade_id FROM trade_log")
        }
        taker_fill_ids = {
            _norm_id(t)
            for (t,) in cur.execute(
                "SELECT trade_id FROM taker_fills WHERE trade_id IS NOT NULL"
            )
        }
        dead_on_chain_ids = _load_dead_on_chain_ids(con)
    finally:
        con.close()
    return trade_log_ids, taker_fill_ids, dead_on_chain_ids


@dataclass
class SweepResult:
    confirmed: int = 0
    in_grace: int = 0
    checked_makers: int = 0
    checked_takers: int = 0
    missing: list[dict] = field(default_factory=list)
    # CONFIRMED and unrecorded, but proven dead on-chain: one entry per offer.
    excluded_dead: list[dict] = field(default_factory=list)


def sweep_records(
    records: list[dict],
    recorded: set[str],
    dead_on_chain: set[str],
    *,
    since_height: int | None,
    since_time: float | None,
    window_height: int | None,
    grace_cutoff: float,
) -> SweepResult:
    """Classify every CONFIRMED wallet record in scope.  Pure: no I/O.

    since_time and window_height are the --since-days window: its start time
    and its start height (window_start_height).  A record before since_time
    is still checked when its confirmed_at_index is at or above window_height.
    """
    res = SweepResult()
    excluded_seen: set[str] = set()
    for rec in records:
        if rec.get("status") != "CONFIRMED":
            continue
        res.confirmed += 1
        if since_height is not None and _confirmed_height(rec) < since_height:
            continue
        if not _in_time_window(rec, since_time, window_height):
            continue
        rec_time = _record_time(rec)
        if rec_time > grace_cutoff:
            res.in_grace += 1
            continue
        if rec.get("is_my_offer"):
            res.checked_makers += 1
        else:
            res.checked_takers += 1
        tid = _norm_id(rec["trade_id"])
        if tid in recorded:
            continue
        if tid in dead_on_chain:
            if tid not in excluded_seen:
                excluded_seen.add(tid)
                res.excluded_dead.append(rec)
            continue
        res.missing.append(rec)
    res.missing.sort(key=_record_time)
    res.excluded_dead.sort(key=_record_time)
    return res


def _iso(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _describe(rec: dict) -> str:
    role = "maker" if rec.get("is_my_offer") else "taker"
    height = _confirmed_height(rec)
    return (f"  {rec['trade_id']}  {role:5s}  "
            f"{_iso(_record_time(rec))}  h={height}  "
            f"{_summarize_flows(rec)}")


def _print_excluded(excluded: list[dict]) -> None:
    """The WARN section: every excluded offer, and how to re-prove it."""
    print()
    print(f"WARN: Excluded, not checked -- {len(excluded)} CONFIRMED wallet "
          "offer(s) are unrecorded because the engine")
    print("recorded them proven dead on-chain (closure event status_update / "
          "dead_on_chain), never taken.")
    print("This sweep takes that verdict on trust.  Re-prove each one "
          "independently (read-only; needs")
    print('the local wallet and full node) and expect "VERDICT: Dead".  Any '
          "other verdict: the offer")
    print("may be a real, unrecorded fill -- investigate it as a missing one.")
    if not PROVE_SCRIPT.exists():
        print(f"(prove_fill_on_chain.py is not in this checkout: {PROVE_SCRIPT})")
    for rec in excluded:
        print(_describe(rec))
        print(f'      re-prove: python "{PROVE_SCRIPT}" {rec["trade_id"]}')


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify every CONFIRMED wallet trade is in trade_log/taker_fills."
    )
    parser.add_argument(
        "--since-height",
        type=int,
        default=None,
        help="Only check trades with confirmed_at_index >= this block height.",
    )
    parser.add_argument(
        "--since-days",
        type=float,
        default=None,
        help="Only check trades that may have confirmed in the last N days: "
        "accepted/created in that time, or confirmed at or above the window's "
        "start height (N days of blocks below the wallet's height, +10%%), so "
        "an offer posted earlier and taken inside the window is checked.",
    )
    parser.add_argument(
        "--grace-minutes",
        type=float,
        default=10.0,
        help="Ignore trades accepted (else created) within the last N minutes "
        "(engine may not have persisted them yet).  Default 10.",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit 3 when any offer is excluded as dead on-chain although "
        "nothing is missing (a missing trade still exits 1).  Without it an "
        "exclusion is a WARN and does not change the exit code.",
    )
    parser.add_argument(
        "--db",
        default=str(DB_PATH),
        help="Path to the engine SQLite database (opened read-only).",
    )
    args = parser.parse_args()
    if args.since_days is not None and not args.since_days > 0:
        parser.error("--since-days must be a positive number of days")

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"ERROR: database not found at {db_path}", file=sys.stderr)
        return EXIT_ERROR

    rpc = WalletRpc()
    try:
        sync = rpc.call("get_sync_status")
    except requests.RequestException as exc:
        print(f"ERROR: wallet RPC unreachable at {WALLET_RPC_URL}: {exc}",
              file=sys.stderr)
        return EXIT_ERROR
    if not sync.get("synced", False):
        print("WARNING: wallet reports NOT synced -- results may be "
              "incomplete; a PASS is not trustworthy until synced.",
              file=sys.stderr)

    now = time.time()
    grace_cutoff = now - args.grace_minutes * 60.0
    since_time = None
    window_height = None
    if args.since_days is not None:
        since_time = now - args.since_days * 86400.0
        # The window's start height, for trades created before it and
        # confirmed inside it (see the module docstring).
        try:
            peak = int(rpc.call("get_height_info").get("height") or 0)
        except requests.RequestException as exc:
            print(f"ERROR: wallet get_height_info failed: {exc}", file=sys.stderr)
            return EXIT_ERROR
        if peak <= 0:
            print("ERROR: the wallet reported no height, so --since-days cannot "
                  "find the window's start height; use --since-height instead.",
                  file=sys.stderr)
            return EXIT_ERROR
        window_height = window_start_height(peak, args.since_days)

    records = rpc.fetch_all_trade_records()
    trade_log_ids, taker_fill_ids, dead_on_chain_ids = _load_recorded_ids(db_path)
    recorded = trade_log_ids | taker_fill_ids

    res = sweep_records(
        records,
        recorded,
        dead_on_chain_ids,
        since_height=args.since_height,
        since_time=since_time,
        window_height=window_height,
        grace_cutoff=grace_cutoff,
    )
    missing = res.missing
    missing_makers = [r for r in missing if r.get("is_my_offer")]
    missing_takers = [r for r in missing if not r.get("is_my_offer")]
    excluded = res.excluded_dead

    scope = "full history"
    if args.since_height is not None:
        scope = f"height >= {args.since_height}"
    if since_time is not None:
        scope = (scope + ", " if args.since_height is not None else "") + (
            f"last {args.since_days:g} days (accepted/created since "
            f"{_iso(since_time)}, or confirmed at height >= {window_height})")

    print("Fill-completeness sweep")
    print(f"  scope:                    {scope}")
    print(f"  wallet trade records:     {len(records)}")
    print(f"  CONFIRMED total:          {res.confirmed}")
    print(f"  checked maker (mine):     {res.checked_makers}")
    print(f"  checked taker (theirs):   {res.checked_takers}")
    print(f"  skipped (grace window):   {res.in_grace}")
    print(f"  trade_log ids:            {len(trade_log_ids)}")
    print(f"  taker_fills ids:          {len(taker_fill_ids)}")
    print(f"  dead_on_chain offers:     {len(dead_on_chain_ids)}")
    print(f"  excluded (dead_on_chain): {len(excluded)}")
    print(f"  MISSING maker fills:      {len(missing_makers)}")
    print(f"  MISSING taker trades:     {len(missing_takers)}")

    if excluded:
        _print_excluded(excluded)

    if missing:
        print()
        print("Missing CONFIRMED trades (not in trade_log or taker_fills):")
        for rec in missing:
            print(_describe(rec))
        print()
        print(f"FAIL: {len(missing)} CONFIRMED wallet trades are unrecorded "
              f"({len(missing_makers)} maker, {len(missing_takers)} taker).")
        return EXIT_FAIL

    print()
    if excluded and args.strict:
        print(f"STRICT: nothing is missing, but {len(excluded)} offer(s) are "
              "excluded on the engine's dead_on_chain verdict; re-prove them "
              f"(WARN above).  Exit {EXIT_STRICT_EXCLUDED}.")
        return EXIT_STRICT_EXCLUDED
    if excluded:
        print(f"PASS with WARN: every CONFIRMED wallet trade in scope is "
              f"recorded, except {len(excluded)} excluded as proven dead "
              "on-chain -- re-prove them (WARN above).")
        return EXIT_PASS
    print("PASS: every CONFIRMED wallet trade in scope is recorded.")
    return EXIT_PASS


if __name__ == "__main__":
    raise SystemExit(main())
