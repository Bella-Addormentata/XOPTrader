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

Exit codes: 0 = PASS, 1 = FAIL (missing trades listed), 2 = operational
error (wallet RPC unreachable, database missing, ...).

Requires the Chia wallet to be running and synced (localhost:9256, client
certs from ~/.chia/mainnet/config/ssl/wallet).  A grace window (default 10
minutes, --grace-minutes) excludes trades confirmed moments ago that the
engine legitimately has not persisted yet.
"""

from __future__ import annotations

import argparse
import os
import sqlite3
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import requests
import urllib3

REPO_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = REPO_ROOT / "data" / "xop_trader.db"

WALLET_RPC_URL = "https://localhost:9256/"
RPC_PAGE_SIZE = 200
RPC_MAX_RECORDS = 50_000

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
    April-June era), so created_at_time is the fallback.
    """
    return int(record.get("accepted_at_time") or record.get("created_at_time") or 0)


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


def _load_recorded_ids(db_path: Path) -> tuple[set[str], set[str]]:
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
    finally:
        con.close()
    return trade_log_ids, taker_fill_ids


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
        help="Only check trades accepted/created within the last N days.",
    )
    parser.add_argument(
        "--grace-minutes",
        type=float,
        default=10.0,
        help="Ignore trades confirmed within the last N minutes (engine may "
        "not have persisted them yet).  Default 10.",
    )
    parser.add_argument(
        "--db",
        default=str(DB_PATH),
        help="Path to the engine SQLite database (opened read-only).",
    )
    args = parser.parse_args()

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"ERROR: database not found at {db_path}", file=sys.stderr)
        return 2

    rpc = WalletRpc()
    try:
        sync = rpc.call("get_sync_status")
    except requests.RequestException as exc:
        print(f"ERROR: wallet RPC unreachable at {WALLET_RPC_URL}: {exc}",
              file=sys.stderr)
        return 2
    if not sync.get("synced", False):
        print("WARNING: wallet reports NOT synced -- results may be "
              "incomplete; a PASS is not trustworthy until synced.",
              file=sys.stderr)

    records = rpc.fetch_all_trade_records()
    trade_log_ids, taker_fill_ids = _load_recorded_ids(db_path)
    recorded = trade_log_ids | taker_fill_ids

    now = time.time()
    grace_cutoff = now - args.grace_minutes * 60.0
    since_time = now - args.since_days * 86400.0 if args.since_days else None

    confirmed = 0
    in_grace = 0
    checked_makers = 0
    checked_takers = 0
    missing: list[dict] = []

    for rec in records:
        if rec.get("status") != "CONFIRMED":
            continue
        confirmed += 1
        if args.since_height is not None:
            if int(rec.get("confirmed_at_index") or 0) < args.since_height:
                continue
        rec_time = _record_time(rec)
        if since_time is not None and rec_time < since_time:
            continue
        if rec_time > grace_cutoff:
            in_grace += 1
            continue
        if rec.get("is_my_offer"):
            checked_makers += 1
        else:
            checked_takers += 1
        if _norm_id(rec["trade_id"]) not in recorded:
            missing.append(rec)

    missing.sort(key=_record_time)
    missing_makers = [r for r in missing if r.get("is_my_offer")]
    missing_takers = [r for r in missing if not r.get("is_my_offer")]

    scope = "full history"
    if args.since_height is not None:
        scope = f"height >= {args.since_height}"
    if args.since_days is not None:
        scope = (scope + ", " if args.since_height is not None else "") + \
            f"last {args.since_days:g} days"

    print("Fill-completeness sweep")
    print(f"  scope:                    {scope}")
    print(f"  wallet trade records:     {len(records)}")
    print(f"  CONFIRMED total:          {confirmed}")
    print(f"  checked maker (mine):     {checked_makers}")
    print(f"  checked taker (theirs):   {checked_takers}")
    print(f"  skipped (grace window):   {in_grace}")
    print(f"  trade_log ids:            {len(trade_log_ids)}")
    print(f"  taker_fills ids:          {len(taker_fill_ids)}")
    print(f"  MISSING maker fills:      {len(missing_makers)}")
    print(f"  MISSING taker trades:     {len(missing_takers)}")

    if missing:
        print()
        print("Missing CONFIRMED trades (not in trade_log or taker_fills):")
        for rec in missing:
            role = "maker" if rec.get("is_my_offer") else "taker"
            when = datetime.fromtimestamp(_record_time(rec), tz=timezone.utc)
            height = int(rec.get("confirmed_at_index") or 0)
            print(f"  {rec['trade_id']}  {role:5s}  "
                  f"{when.strftime('%Y-%m-%dT%H:%M:%SZ')}  h={height}  "
                  f"{_summarize_flows(rec)}")
        print()
        print(f"FAIL: {len(missing)} CONFIRMED wallet trades are unrecorded "
              f"({len(missing_makers)} maker, {len(missing_takers)} taker).")
        return 1

    print()
    print("PASS: every CONFIRMED wallet trade in scope is recorded.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
