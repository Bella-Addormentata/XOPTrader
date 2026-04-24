"""Fill-history monitor (v0.7.48).

Reports the rolling fill rate per pair across multiple windows and shows
the gap to the engine's PID target (`comp_pid_target_fill_rate`).  Use
this to verify that the adaptive competitiveness controller is doing
useful work after a deploy:

    py -3 scripts/monitor_fill_history.py                # one-shot
    py -3 scripts/monitor_fill_history.py --watch 60     # refresh every 60 s
    py -3 scripts/monitor_fill_history.py --json         # machine-readable

Fill rate definition matches the PID: blocks-with-at-least-one-fill /
total-blocks-in-window.  We approximate "total blocks" using the Chia
mean block time of 18.75 s (per CHIP-12) so the script stays
self-contained -- no chain RPC required.

ISO/IEC 27001:2022 -- read-only access to the engine's local SQLite db.
ISO/IEC 5055      -- deterministic SQL, no UB, no network calls.
"""
from __future__ import annotations

import argparse
import json
import sqlite3
import sys
import time
from dataclasses import dataclass, asdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Iterable

# Approximate Chia block cadence.  Used to convert window length -> block count
# for the fill-rate denominator so the metric is comparable to the PID input.
CHIA_BLOCK_SECONDS = 18.75

# Default windows shown by the monitor.  Keep aligned with engine logging
# cadences (heartbeat, 1h diag) so the numbers are easy to cross-check.
DEFAULT_WINDOWS = [
    ("1h",  timedelta(hours=1)),
    ("6h",  timedelta(hours=6)),
    ("24h", timedelta(days=1)),
    ("7d",  timedelta(days=7)),
]

# Engine default; keep in sync with StrategyConfig::comp_pid_target_fill_rate.
DEFAULT_PID_TARGET = 0.05


@dataclass
class WindowReport:
    pair: str
    window: str
    fills: int
    cancels: int
    posted: int
    avg_competitiveness: float
    blocks_estimate: int
    fill_rate: float           # fills / blocks  (matches PID input)
    gap_to_target: float       # fill_rate - target


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _isoformat(td_ago: timedelta) -> str:
    """SQLite's `datetime('now', '-N seconds')` equivalent."""
    return (_utcnow() - td_ago).strftime("%Y-%m-%d %H:%M:%S")


def collect_windows(
    cur: sqlite3.Cursor,
    windows: Iterable[tuple[str, timedelta]],
    pid_target: float,
) -> list[WindowReport]:
    out: list[WindowReport] = []

    # Discover all pairs that appear in either log within the largest window.
    longest = max(td for _, td in windows)
    cur.execute(
        "SELECT DISTINCT pair_name FROM offer_log "
        "WHERE created_at >= ? "
        "UNION SELECT DISTINCT pair_name FROM trade_log WHERE timestamp >= ?",
        (_isoformat(longest), _isoformat(longest)),
    )
    pairs = sorted(r[0] for r in cur.fetchall() if r[0])

    for pair in pairs:
        for label, td in windows:
            since = _isoformat(td)

            cur.execute(
                "SELECT COUNT(*) FROM trade_log "
                "WHERE pair_name = ? AND timestamp >= ?",
                (pair, since),
            )
            fills = int(cur.fetchone()[0])

            cur.execute(
                "SELECT "
                "  SUM(CASE WHEN status = 'cancelled' THEN 1 ELSE 0 END), "
                "  COUNT(*), "
                "  AVG(competitiveness_score) "
                "FROM offer_log "
                "WHERE pair_name = ? AND created_at >= ?",
                (pair, since),
            )
            row = cur.fetchone()
            cancels = int(row[0] or 0)
            posted  = int(row[1] or 0)
            avg_comp = float(row[2] or 0.0)

            blocks = max(1, int(td.total_seconds() / CHIA_BLOCK_SECONDS))
            # PID input is a *binary per-block* signal.  Without a per-block
            # join here we approximate `blocks_with_fill ~= min(fills, blocks)`,
            # which is exact when fills are sparse (the regime that actually
            # matters for the underfilling controller).
            fill_rate = min(fills, blocks) / blocks
            gap = fill_rate - pid_target

            out.append(WindowReport(
                pair=pair,
                window=label,
                fills=fills,
                cancels=cancels,
                posted=posted,
                avg_competitiveness=avg_comp,
                blocks_estimate=blocks,
                fill_rate=fill_rate,
                gap_to_target=gap,
            ))
    return out


def render_text(reports: list[WindowReport], pid_target: float) -> str:
    if not reports:
        return "(no data in any window)"

    lines = [
        f"=== XOP fill-history monitor (PID target fill_rate = {pid_target:.4f}) ===",
        f"=== {_utcnow().isoformat(timespec='seconds')} ===",
        "",
        f"{'pair':<22} {'win':<5} {'fills':>6} {'cancels':>8} {'posted':>7} "
        f"{'avg_comp':>9} {'fill_rate':>10} {'gap':>9}  hint",
    ]
    for r in reports:
        if r.gap_to_target < -pid_target * 0.5:
            hint = "underfilling -> PID should LOWER gate"
        elif r.gap_to_target > pid_target:
            hint = "overfilling -> PID should RAISE gate"
        else:
            hint = "near target"
        lines.append(
            f"{r.pair:<22} {r.window:<5} {r.fills:>6d} {r.cancels:>8d} "
            f"{r.posted:>7d} {r.avg_competitiveness:>9.2f} "
            f"{r.fill_rate:>10.4f} {r.gap_to_target:>+9.4f}  {hint}"
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--db",
        default="data/xop_trader.db",
        help="Path to the engine SQLite database.",
    )
    parser.add_argument(
        "--target",
        type=float,
        default=DEFAULT_PID_TARGET,
        help="PID target fill rate to compare against (default %(default).3f).",
    )
    parser.add_argument(
        "--watch",
        type=int,
        default=0,
        help="If > 0, refresh every N seconds; otherwise one-shot.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit JSON instead of human-readable text.",
    )
    args = parser.parse_args(argv)

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"ERROR: database not found: {db_path}", file=sys.stderr)
        return 2

    while True:
        with sqlite3.connect(str(db_path)) as db:
            reports = collect_windows(db.cursor(), DEFAULT_WINDOWS, args.target)

        if args.json:
            print(json.dumps(
                {"target": args.target, "reports": [asdict(r) for r in reports]},
                indent=2,
            ))
        else:
            print(render_text(reports, args.target))

        if args.watch <= 0:
            return 0
        time.sleep(args.watch)


if __name__ == "__main__":
    raise SystemExit(main())
