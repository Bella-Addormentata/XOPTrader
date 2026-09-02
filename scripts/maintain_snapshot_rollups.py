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


class LargePruneRefused(RuntimeError):
    """A prune would remove more history than an unattended run should."""


# Fraction of a table a single unattended run may delete before it refuses.
#
# [2026-09-01] Retention had not run since 2026-05-16, so raw history reached
# back to 2026-04-03 while the default window was 120 days.  The next run --
# with no flag typed differently, no warning, and nothing to review -- would
# have deleted 90,968 of 206,694 snapshots rows (44%) and 363,374
# strategy_quotes rows, including all of April: the densest month of the BYC
# book history that docs/price-discovery-from-trade-history.md and
# scripts/byc_price_diagnostic.py both depend on.
#
# The hazard is not the retention number.  It is that a LONG GAP between runs
# silently converts a routine window into a bulk deletion, and the longer the
# gap the bigger the loss -- the failure mode gets worse exactly when nobody
# is watching.  A steady-state daily run deletes a day at a time and never
# comes near this bound; only the dangerous case trips it.
MAX_UNCONFIRMED_PRUNE_FRACTION = 0.25


def _prune_raw_tables(
    conn: sqlite3.Connection,
    *,
    raw_retention_days: int,
    prune_strategy_quotes: bool,
    confirm_large_prune: bool = False,
) -> dict[str, int]:
    cutoff = datetime.now(tz=timezone.utc) - timedelta(days=raw_retention_days)
    # [review 2026-09-01] Format the cutoff the way these two tables actually
    # STORE their timestamps -- "YYYY-MM-DD HH:MM:SS", space-separated -- not
    # as _iso_utc()'s "...THH:MM:SS.ffffffZ".
    #
    # Both comparisons here are TEXT comparisons, and " " (0x20) sorts below
    # "T" (0x54). So against a "T"-form cutoff, every row sharing the cutoff's
    # DATE compares less-than regardless of its time: a row stored
    # "2026-05-04 23:59:59" was deleted by a cutoff of
    # "2026-05-04T11:56:08.326330Z". The prune therefore reached up to a full
    # extra day past the window the operator asked for, silently and always in
    # the deleting direction.
    #
    # The count and the DELETE shared the format, so the guard's percentages
    # were honest about what would be removed -- this was over-deletion, not
    # mis-reporting. Fixed here rather than left as pre-existing because it
    # lives in the function this change hardened, and it errs toward data loss.
    cutoff_iso = cutoff.astimezone(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")

    targets = ["snapshots"]
    if prune_strategy_quotes:
        targets.append("strategy_quotes")

    # COUNT BEFORE DELETING.  cur.rowcount reports the damage after it is
    # done, which is no use to a guard, and inside the caller's transaction
    # a refusal has to happen before any DELETE runs so the rollback is
    # empty rather than merely correct.
    planned: dict[str, tuple[int, int, str | None]] = {}
    for table in targets:
        total = int(
            conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        )
        doomed = int(
            conn.execute(
                f"SELECT COUNT(*) FROM {table} WHERE created_at < ?",
                [cutoff_iso],
            ).fetchone()[0]
        )
        # The oldest row is captured PER TABLE, in the same pass as the
        # counts.  The refusal exists so the operator can choose a retention
        # window, and the window has to clear whichever table's history
        # actually reaches furthest back.  Reporting the snapshots row while
        # strategy_quotes is the table in breach points at unrelated history
        # and misleads exactly the decision this message is for.  These
        # tables are not pruned independently, but they do not start at the
        # same date, and either one can breach alone.
        oldest = conn.execute(
            f"SELECT MIN(created_at) FROM {table}"
        ).fetchone()[0]
        planned[table] = (doomed, total, oldest)

    if not confirm_large_prune:
        breaches = [
            (t, d, n, oldest)
            for t, (d, n, oldest) in planned.items()
            if n > 0 and (d / n) > MAX_UNCONFIRMED_PRUNE_FRACTION
        ]
        if breaches:
            width = max(len(t) for t, _, _, _ in breaches)
            msg = [
                # Explicit '+', not adjacent literals: CodeQL flags implicit
                # concatenation inside a list because a missing comma between
                # two intended elements is indistinguishable from it, and this
                # list is the operator's only warning before a bulk delete.
                "this run would delete more than "
                + f"{MAX_UNCONFIRMED_PRUNE_FRACTION:.0%} of a raw table:",
            ]
            for t, d, n, oldest in breaches:
                msg.append(f"    {t:<{width}} : {d:,} of {n:,} rows ({d / n:.1%})")
                # Printed verbatim.  snapshots.created_at stores a SPACE
                # between date and time while other tables may store a "T",
                # and the operator has to be able to see which form the
                # rows in front of them use.  Nothing here compares the two
                # forms; MIN() is over the stored text of one table only.
                msg.append(f"    {'':<{width}}   oldest row {oldest}")
            msg += [
                f"  cutoff        : {cutoff_iso}",
                f"  retention set : {raw_retention_days} days",
                ("  A jump this large means retention has not run in a "
                 + "long time, not that this much data is stale."),
                ("  Widen --raw-retention-days to keep the history, or "
                 + "pass --confirm-large-prune to delete it on purpose."),
                "  Take a --backup either way, and --dry-run first.",
            ]
            raise LargePruneRefused(chr(10).join(msg))

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
        "--confirm-large-prune",
        action="store_true",
        help=(
            "Permit deleting more than "
            f"{MAX_UNCONFIRMED_PRUNE_FRACTION:.0%} of a raw table in one run. "
            "Without this the run REFUSES and changes nothing."
        ),
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
        # sqlite3's own backup API, NOT shutil.copy2.
        #
        # [2026-09-01] This database runs in WAL mode against a live engine,
        # and a plain file copy takes only xop_trader.db -- leaving the
        # -wal file behind. Measured at the time of this change the WAL held
        # 15 MB of committed pages that had not yet been checkpointed into
        # the main file, so the "backup" would silently have been missing
        # the most recent writes AND been a torn read of a file being
        # written underneath it. A backup you take before a destructive
        # operation is the one thing that must not be quietly wrong.
        #
        # conn.backup() holds a read transaction for the copy, so it sees a
        # single consistent snapshot including the WAL, and it is safe
        # against a concurrent writer.
        src = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        try:
            dst = sqlite3.connect(str(backup))
            try:
                src.backup(dst)
            finally:
                dst.close()
        finally:
            src.close()
        print(f"Backup created: {backup} ({_human_mb(_db_size_bytes(backup)):.1f} MB)")

    before_size = _db_size_bytes(db_path)

    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=5000")

    deleted: dict[str, int] = {}

    try:
        # TWO TRANSACTIONS, not one.
        #
        # [review 2026-09-01] The rollups and the prune used to share a
        # single transaction, so the refusal's rollback -- which must
        # discard the DELETEs -- also discarded four tables' worth of
        # rollup UPSERTs, and returned before the summary was printed. The
        # operator who hit the guard saw no sign the rollups had run, and
        # they had not persisted.
        #
        # The two halves do not need shared atomicity. The rollup UPSERTs
        # are idempotent and non-destructive: they derive from `snapshots`
        # rows that are still there, and re-running reproduces them. The
        # DELETEs are neither, so they keep a transaction of their own and
        # a refusal still leaves the raw tables exactly as it found them.
        conn.execute("BEGIN" if args.dry_run else "BEGIN IMMEDIATE")

        print("Building rollups...")
        rollup_stats: list[tuple[str, int, int]] = []
        for spec in ROLLUP_SPECS:
            source_rows, upserts = _build_rollup(conn, spec)
            rollup_stats.append((spec.table, source_rows, upserts))

        if args.dry_run:
            conn.rollback()
        else:
            conn.commit()

        # Reported here, between the phases, so the refusal path shows it
        # too -- and so it only ever describes work that is already
        # durable.
        suffix = "  (dry-run; rolled back)" if args.dry_run else ""
        print(f"\nRollup summary:{suffix}")
        for table, source_rows, upserts in rollup_stats:
            print(f"  {table:<14} source_rows={source_rows:>8}  upserts={upserts:>8}")

        conn.execute("BEGIN" if args.dry_run else "BEGIN IMMEDIATE")

        print("\nPruning raw tables...")
        deleted = _prune_raw_tables(
            conn,
            raw_retention_days=args.raw_retention_days,
            prune_strategy_quotes=not args.no_prune_strategy_quotes,
            confirm_large_prune=args.confirm_large_prune,
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

    except LargePruneRefused as exc:
        # Roll back explicitly rather than relying on close(). Nothing has
        # been deleted -- the guard raises before the first DELETE -- but
        # the prune transaction is still open, and refusing has to leave
        # the raw tables exactly as they were, not merely usually.
        conn.rollback()
        conn.close()
        print()
        print("REFUSED: " + str(exc))
        return 3
    finally:
        conn.close()

    after_size = _db_size_bytes(db_path)

    print("\nPrune summary:")
    for table, rows in deleted.items():
        print(f"  {table:<14} deleted_rows={rows}")

    print("\nDB file size:")
    print(f"  before: {_human_mb(before_size):.2f} MB")
    print(f"  after : {_human_mb(after_size):.2f} MB")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
