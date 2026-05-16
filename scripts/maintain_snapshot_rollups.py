"""Build long-term snapshot rollups and prune old high-frequency rows.

This maintenance script keeps chart history fast while bounding DB growth.
It creates four aggregate tables derived from ``snapshots``:

- snapshots_1m
- snapshots_15m
- snapshots_1h
- snapshots_1d

Each rollup stores OHLC mid-price, average microstructure fields, and the
closing PnL state for each bucket and pair.

By default, the script also prunes old rows from ``snapshots`` and
``strategy_quotes`` after rollups are updated.
"""

from __future__ import annotations

import argparse
import shutil
import sqlite3
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path


@dataclass(frozen=True)
class RollupSpec:
    table: str
    bucket_seconds: int


ROLLUP_SPECS: tuple[RollupSpec, ...] = (
    RollupSpec("snapshots_1m", 60),
    RollupSpec("snapshots_15m", 15 * 60),
    RollupSpec("snapshots_1h", 60 * 60),
    RollupSpec("snapshots_1d", 24 * 60 * 60),
)


def _parse_iso_utc(text: str) -> datetime:
    normalized = text.replace("Z", "+00:00")
    dt = datetime.fromisoformat(normalized)
    if dt.tzinfo is None:
        return dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def _iso_utc(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def _ensure_rollup_table(conn: sqlite3.Connection, spec: RollupSpec) -> None:
    conn.execute(
        f"""
        CREATE TABLE IF NOT EXISTS {spec.table} (
            pair_name TEXT NOT NULL,
            bucket_start_unix INTEGER NOT NULL,
            bucket_start_iso TEXT NOT NULL,
            open_mid_price_mojos INTEGER,
            high_mid_price_mojos INTEGER,
            low_mid_price_mojos INTEGER,
            close_mid_price_mojos INTEGER,
            avg_spread_bps REAL,
            avg_inventory_ratio REAL,
            avg_sigma_block REAL,
            close_regime TEXT,
            close_pnl_total_mojos INTEGER,
            avg_xch_usd_rate REAL,
            close_pnl_total_usd REAL,
            sample_count INTEGER NOT NULL,
            source_first_block INTEGER,
            source_last_block INTEGER,
            updated_at TEXT NOT NULL,
            PRIMARY KEY (pair_name, bucket_start_unix)
        )
        """
    )
    conn.execute(
        f"CREATE INDEX IF NOT EXISTS idx_{spec.table}_bucket ON {spec.table}(bucket_start_unix)"
    )


def _latest_bucket_start(conn: sqlite3.Connection, table: str) -> int | None:
    row = conn.execute(
        f"SELECT MAX(bucket_start_unix) AS max_bucket FROM {table}"
    ).fetchone()
    if row is None:
        return None
    value = row[0]
    return int(value) if value is not None else None


def _build_rollup(conn: sqlite3.Connection, spec: RollupSpec) -> tuple[int, int]:
    _ensure_rollup_table(conn, spec)

    latest_bucket = _latest_bucket_start(conn, spec.table)
    params: list[object] = []
    where_clause = ""

    if latest_bucket is not None:
        backtrack = max(0, latest_bucket - spec.bucket_seconds)
        where_clause = "WHERE created_at >= datetime(?, 'unixepoch')"
        params.append(backtrack)

    rows = conn.execute(
        f"""
        SELECT
            pair_name,
            block_height,
            mid_price_mojos,
            spread_bps,
            inventory_ratio,
            sigma_block,
            regime,
            pnl_total_mojos,
            xch_usd_rate,
            pnl_total_usd,
            created_at
        FROM snapshots
        {where_clause}
        ORDER BY pair_name ASC, created_at ASC, block_height ASC
        """,
        params,
    ).fetchall()

    if not rows:
        return (0, 0)

    upserts: list[tuple[object, ...]] = []

    current_pair = None
    current_bucket = None
    open_mid = high_mid = low_mid = close_mid = None
    spread_sum = 0.0
    inv_sum = 0.0
    sigma_sum = 0.0
    fx_sum = 0.0
    fx_count = 0
    sample_count = 0
    close_regime = None
    close_pnl_mojos = None
    close_pnl_usd = None
    source_first_block = None
    source_last_block = None

    def flush() -> None:
        if current_pair is None or current_bucket is None or sample_count == 0:
            return

        bucket_dt = datetime.fromtimestamp(current_bucket, tz=timezone.utc)
        updated_at = _iso_utc(datetime.now(tz=timezone.utc))
        avg_fx = (fx_sum / fx_count) if fx_count > 0 else 0.0
        upserts.append(
            (
                current_pair,
                current_bucket,
                _iso_utc(bucket_dt),
                open_mid,
                high_mid,
                low_mid,
                close_mid,
                spread_sum / sample_count,
                inv_sum / sample_count,
                sigma_sum / sample_count,
                close_regime,
                close_pnl_mojos,
                avg_fx,
                close_pnl_usd,
                sample_count,
                source_first_block,
                source_last_block,
                updated_at,
            )
        )

    for row in rows:
        pair_name = str(row[0])
        block_height = int(row[1] or 0)
        mid_price = int(row[2] or 0)
        spread_bps = float(row[3] or 0.0)
        inventory_ratio = float(row[4] or 0.0)
        sigma_block = float(row[5] or 0.0)
        regime = str(row[6] or "")
        pnl_mojos = int(row[7] or 0)
        xch_usd_rate = float(row[8] or 0.0)
        pnl_usd = float(row[9] or 0.0)
        created_at = _parse_iso_utc(str(row[10]))
        bucket_start = int(created_at.timestamp())
        bucket_start -= bucket_start % spec.bucket_seconds

        if (pair_name != current_pair) or (bucket_start != current_bucket):
            flush()
            current_pair = pair_name
            current_bucket = bucket_start
            open_mid = mid_price
            high_mid = mid_price
            low_mid = mid_price
            close_mid = mid_price
            spread_sum = spread_bps
            inv_sum = inventory_ratio
            sigma_sum = sigma_block
            fx_sum = xch_usd_rate if xch_usd_rate > 0 else 0.0
            fx_count = 1 if xch_usd_rate > 0 else 0
            sample_count = 1
            close_regime = regime
            close_pnl_mojos = pnl_mojos
            close_pnl_usd = pnl_usd
            source_first_block = block_height
            source_last_block = block_height
            continue

        high_mid = max(int(high_mid), mid_price)
        low_mid = min(int(low_mid), mid_price)
        close_mid = mid_price
        spread_sum += spread_bps
        inv_sum += inventory_ratio
        sigma_sum += sigma_block
        if xch_usd_rate > 0:
            fx_sum += xch_usd_rate
            fx_count += 1
        sample_count += 1
        close_regime = regime
        close_pnl_mojos = pnl_mojos
        close_pnl_usd = pnl_usd
        source_last_block = block_height

    flush()

    conn.executemany(
        f"""
        INSERT INTO {spec.table} (
            pair_name,
            bucket_start_unix,
            bucket_start_iso,
            open_mid_price_mojos,
            high_mid_price_mojos,
            low_mid_price_mojos,
            close_mid_price_mojos,
            avg_spread_bps,
            avg_inventory_ratio,
            avg_sigma_block,
            close_regime,
            close_pnl_total_mojos,
            avg_xch_usd_rate,
            close_pnl_total_usd,
            sample_count,
            source_first_block,
            source_last_block,
            updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(pair_name, bucket_start_unix) DO UPDATE SET
            bucket_start_iso = excluded.bucket_start_iso,
            open_mid_price_mojos = excluded.open_mid_price_mojos,
            high_mid_price_mojos = excluded.high_mid_price_mojos,
            low_mid_price_mojos = excluded.low_mid_price_mojos,
            close_mid_price_mojos = excluded.close_mid_price_mojos,
            avg_spread_bps = excluded.avg_spread_bps,
            avg_inventory_ratio = excluded.avg_inventory_ratio,
            avg_sigma_block = excluded.avg_sigma_block,
            close_regime = excluded.close_regime,
            close_pnl_total_mojos = excluded.close_pnl_total_mojos,
            avg_xch_usd_rate = excluded.avg_xch_usd_rate,
            close_pnl_total_usd = excluded.close_pnl_total_usd,
            sample_count = excluded.sample_count,
            source_first_block = excluded.source_first_block,
            source_last_block = excluded.source_last_block,
            updated_at = excluded.updated_at
        """,
        upserts,
    )

    return (len(rows), len(upserts))


def _prune_raw_tables(
    conn: sqlite3.Connection,
    *,
    raw_retention_days: int,
    prune_strategy_quotes: bool,
) -> dict[str, int]:
    cutoff = datetime.now(tz=timezone.utc) - timedelta(days=raw_retention_days)
    cutoff_iso = _iso_utc(cutoff)

    deleted: dict[str, int] = {}

    cur = conn.execute(
        "DELETE FROM snapshots WHERE created_at < ?",
        [cutoff_iso],
    )
    deleted["snapshots"] = int(cur.rowcount or 0)

    if prune_strategy_quotes:
        cur = conn.execute(
            "DELETE FROM strategy_quotes WHERE created_at < ?",
            [cutoff_iso],
        )
        deleted["strategy_quotes"] = int(cur.rowcount or 0)

    return deleted


def _db_size_bytes(path: Path) -> int:
    if not path.exists():
        return 0
    return path.stat().st_size


def _human_mb(size_bytes: int) -> float:
    return float(size_bytes) / (1024.0 * 1024.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--db",
        default=r"c:\GitHub\XOPTrader\data\xop_trader.db",
        help="Path to SQLite database.",
    )
    parser.add_argument(
        "--raw-retention-days",
        type=int,
        default=120,
        help="Retain this many days of high-frequency rows in snapshots and strategy_quotes.",
    )
    parser.add_argument(
        "--no-prune-strategy-quotes",
        action="store_true",
        help="Do not delete old rows from strategy_quotes.",
    )
    parser.add_argument(
        "--vacuum",
        action="store_true",
        help="Run VACUUM after pruning to reclaim disk space.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report expected changes without writing.",
    )
    parser.add_argument(
        "--backup",
        action="store_true",
        help="Create timestamped backup copy before writes.",
    )
    args = parser.parse_args()

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"ERROR: database not found: {db_path}")
        return 2

    if args.raw_retention_days < 7:
        print("ERROR: raw retention under 7 days is not allowed for safety.")
        return 2

    if args.backup and not args.dry_run:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        backup = db_path.with_suffix(db_path.suffix + f".rollup.{stamp}.bak")
        shutil.copy2(db_path, backup)
        print(f"Backup created: {backup}")

    before_size = _db_size_bytes(db_path)

    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=5000")

    try:
        if args.dry_run:
            conn.execute("BEGIN")
        else:
            conn.execute("BEGIN IMMEDIATE")

        print("Building rollups...")
        rollup_stats: list[tuple[str, int, int]] = []
        for spec in ROLLUP_SPECS:
            source_rows, upserts = _build_rollup(conn, spec)
            rollup_stats.append((spec.table, source_rows, upserts))

        print("Pruning raw tables...")
        deleted = _prune_raw_tables(
            conn,
            raw_retention_days=args.raw_retention_days,
            prune_strategy_quotes=not args.no_prune_strategy_quotes,
        )

        if args.vacuum and not args.dry_run:
            conn.commit()
            conn.execute("VACUUM")
            conn.execute("BEGIN IMMEDIATE")

        if args.dry_run:
            conn.rollback()
            print("Dry-run complete; no changes written.")
        else:
            conn.commit()

    finally:
        conn.close()

    after_size = _db_size_bytes(db_path)

    print("\nRollup summary:")
    for table, source_rows, upserts in rollup_stats:
        print(f"  {table:<14} source_rows={source_rows:>8}  upserts={upserts:>8}")

    print("\nPrune summary:")
    for table, rows in deleted.items():
        print(f"  {table:<14} deleted_rows={rows}")

    print("\nDB file size:")
    print(f"  before: {_human_mb(before_size):.2f} MB")
    print(f"  after : {_human_mb(after_size):.2f} MB")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
