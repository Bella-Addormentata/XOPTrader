"""Cross-platform scheduled DB backup + rollup maintenance.

Runs forever in a simple interval loop:
1) create timestamped SQLite backup copy
2) prune old backups (keep-count and/or keep-days)
3) execute rollup + retention maintenance

This avoids OS-specific schedulers and works anywhere Python runs.
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path


@dataclass(frozen=True)
class BackupPolicy:
    keep_last: int
    keep_days: int


def _now_utc() -> datetime:
    return datetime.now(tz=timezone.utc)


def _timestamp_slug() -> str:
    return _now_utc().strftime("%Y%m%d_%H%M%S")


def _backup_file_name(db_path: Path) -> str:
    return f"{db_path.stem}.{_timestamp_slug()}.sqlite3"


def _create_backup(db_path: Path, backup_dir: Path, *, compress: bool) -> Path:
    backup_dir.mkdir(parents=True, exist_ok=True)
    backup_path = backup_dir / _backup_file_name(db_path)

    src = sqlite3.connect(str(db_path))
    dst = sqlite3.connect(str(backup_path))
    try:
        src.backup(dst)
    finally:
        dst.close()
        src.close()

    if compress:
        compressed = backup_path.with_suffix(backup_path.suffix + ".gz")
        with backup_path.open("rb") as src_file, gzip.open(compressed, "wb") as dst_file:
            shutil.copyfileobj(src_file, dst_file)
        backup_path.unlink(missing_ok=True)
        return compressed

    return backup_path


def _iter_backups(backup_dir: Path, db_path: Path) -> list[Path]:
    patterns = (f"{db_path.stem}.*.sqlite3", f"{db_path.stem}.*.sqlite3.gz")
    files: list[Path] = []
    for pattern in patterns:
        files.extend([p for p in backup_dir.glob(pattern) if p.is_file()])
    files.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return files


def _write_status(status_path: Path, payload: dict[str, object]) -> None:
    status_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = status_path.with_suffix(status_path.suffix + ".tmp")
    temp_path.write_text(
        json.dumps(payload, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    os.replace(temp_path, status_path)


def _lock_is_stale(lock_path: Path, stale_minutes: int) -> bool:
    if stale_minutes <= 0 or not lock_path.exists():
        return False
    age_seconds = time.time() - lock_path.stat().st_mtime
    return age_seconds > stale_minutes * 60


def _acquire_lock(lock_path: Path, stale_minutes: int) -> bool:
    lock_path.parent.mkdir(parents=True, exist_ok=True)

    if lock_path.exists() and _lock_is_stale(lock_path, stale_minutes):
        lock_path.unlink(missing_ok=True)

    flags = os.O_CREAT | os.O_EXCL | os.O_WRONLY
    try:
        fd = os.open(str(lock_path), flags)
    except FileExistsError:
        return False

    with os.fdopen(fd, "w", encoding="utf-8") as lock_file:
        lock_file.write(
            json.dumps(
                {
                    "pid": os.getpid(),
                    "created_at": _now_utc().isoformat().replace("+00:00", "Z"),
                    "program": Path(__file__).name,
                }
            )
        )
    return True


def _release_lock(lock_path: Path) -> None:
    lock_path.unlink(missing_ok=True)


def _prune_backups(backup_dir: Path, db_path: Path, policy: BackupPolicy) -> list[Path]:
    backups = _iter_backups(backup_dir, db_path)
    kept: set[Path] = set()

    # Keep newest N first.
    for p in backups[: max(0, policy.keep_last)]:
        kept.add(p)

    # Keep files newer than keep_days cutoff.
    if policy.keep_days > 0:
        cutoff = _now_utc() - timedelta(days=policy.keep_days)
        for p in backups:
            mtime = datetime.fromtimestamp(p.stat().st_mtime, tz=timezone.utc)
            if mtime >= cutoff:
                kept.add(p)

    deleted: list[Path] = []
    for p in backups:
        if p not in kept:
            p.unlink(missing_ok=True)
            deleted.append(p)

    return deleted


def _run_rollup_maintenance(
    *,
    python_exe: str,
    repo_root: Path,
    db_path: Path,
    raw_retention_days: int,
    vacuum: bool,
    prune_strategy_quotes: bool,
) -> int:
    script_path = repo_root / "scripts" / "maintain_snapshot_rollups.py"
    cmd = [
        python_exe,
        str(script_path),
        "--db",
        str(db_path),
        "--raw-retention-days",
        str(raw_retention_days),
    ]
    if vacuum:
        cmd.append("--vacuum")
    if not prune_strategy_quotes:
        cmd.append("--no-prune-strategy-quotes")

    result = subprocess.run(
        cmd,
        cwd=str(repo_root),
        check=False,
        text=True,
    )
    return int(result.returncode)


def _log(msg: str) -> None:
    ts = _now_utc().isoformat().replace("+00:00", "Z")
    print(f"[{ts}] {msg}", flush=True)


def _run_cycle(
    *,
    db_path: Path,
    backup_dir: Path,
    policy: BackupPolicy,
    python_exe: str,
    repo_root: Path,
    raw_retention_days: int,
    vacuum_every: int,
    prune_strategy_quotes: bool,
    compress_backups: bool,
    status_path: Path,
    cycle_number: int,
) -> int:
    started_at = _now_utc()
    _log("Starting maintenance cycle")

    backup_path = _create_backup(db_path, backup_dir, compress=compress_backups)
    _log(f"Backup created: {backup_path}")

    deleted = _prune_backups(backup_dir, db_path, policy)
    if deleted:
        _log(f"Pruned {len(deleted)} old backups")
    else:
        _log("No backup pruning needed")

    run_vacuum = vacuum_every > 0 and (cycle_number % vacuum_every == 0)
    exit_code = _run_rollup_maintenance(
        python_exe=python_exe,
        repo_root=repo_root,
        db_path=db_path,
        raw_retention_days=raw_retention_days,
        vacuum=run_vacuum,
        prune_strategy_quotes=prune_strategy_quotes,
    )

    if exit_code == 0:
        _log("Rollup maintenance completed successfully")
    else:
        _log(f"Rollup maintenance failed (exit={exit_code})")

    completed_at = _now_utc()
    status_payload: dict[str, object] = {
        "cycle": cycle_number,
        "status": "ok" if exit_code == 0 else "error",
        "exit_code": exit_code,
        "started_at": started_at.isoformat().replace("+00:00", "Z"),
        "completed_at": completed_at.isoformat().replace("+00:00", "Z"),
        "duration_seconds": round((completed_at - started_at).total_seconds(), 3),
        "database": str(db_path),
        "backup_created": str(backup_path),
        "compress_backups": compress_backups,
        "backups_pruned": len(deleted),
        "raw_retention_days": raw_retention_days,
        "vacuum_ran": run_vacuum,
        "prune_strategy_quotes": prune_strategy_quotes,
        "pid": os.getpid(),
    }
    _write_status(status_path, status_payload)
    _log(f"Status updated: {status_path}")

    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--db",
        default="data/xop_trader.db",
        help="Path to SQLite database.",
    )
    parser.add_argument(
        "--backup-dir",
        default="data/backups",
        help="Directory where timestamped backups are stored.",
    )
    parser.add_argument(
        "--interval-minutes",
        type=int,
        default=360,
        help="Minutes between maintenance cycles.",
    )
    parser.add_argument(
        "--keep-last",
        type=int,
        default=30,
        help="Always keep at least this many newest backups.",
    )
    parser.add_argument(
        "--keep-days",
        type=int,
        default=30,
        help="Also keep backups newer than this many days.",
    )
    parser.add_argument(
        "--raw-retention-days",
        type=int,
        default=120,
        help="Raw-row retention for snapshots/strategy_quotes rollup maintenance.",
    )
    parser.add_argument(
        "--vacuum-every",
        type=int,
        default=24,
        help="Run VACUUM every N cycles (0 disables VACUUM).",
    )
    parser.add_argument(
        "--no-prune-strategy-quotes",
        action="store_true",
        help="Keep all strategy_quotes rows.",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="Run one cycle and exit.",
    )
    parser.add_argument(
        "--compress-backups",
        action="store_true",
        help="Compress backup files to .sqlite3.gz after each copy.",
    )
    parser.add_argument(
        "--status-file",
        default="data/backups/db_maintenance_status.json",
        help="Path to JSON status file updated after each cycle.",
    )
    parser.add_argument(
        "--lock-file",
        default="data/backups/db_maintenance.lock",
        help="Path to lock file used to prevent concurrent scheduler instances.",
    )
    parser.add_argument(
        "--stale-lock-minutes",
        type=int,
        default=720,
        help="Treat lock older than this as stale and replace it (0 disables stale lock recovery).",
    )
    args = parser.parse_args()

    if args.interval_minutes < 1:
        print("ERROR: --interval-minutes must be >= 1", file=sys.stderr)
        return 2
    if args.keep_last < 1:
        print("ERROR: --keep-last must be >= 1", file=sys.stderr)
        return 2
    if args.raw_retention_days < 7:
        print("ERROR: --raw-retention-days must be >= 7", file=sys.stderr)
        return 2

    repo_root = Path(__file__).resolve().parents[1]
    db_path = (repo_root / args.db).resolve() if not Path(args.db).is_absolute() else Path(args.db)
    backup_dir = (
        (repo_root / args.backup_dir).resolve()
        if not Path(args.backup_dir).is_absolute()
        else Path(args.backup_dir)
    )
    status_path = (
        (repo_root / args.status_file).resolve()
        if not Path(args.status_file).is_absolute()
        else Path(args.status_file)
    )
    lock_path = (
        (repo_root / args.lock_file).resolve()
        if not Path(args.lock_file).is_absolute()
        else Path(args.lock_file)
    )

    if not db_path.exists():
        print(f"ERROR: database not found: {db_path}", file=sys.stderr)
        return 2

    policy = BackupPolicy(keep_last=args.keep_last, keep_days=args.keep_days)
    python_exe = sys.executable
    interval_seconds = args.interval_minutes * 60

    _log(f"Database: {db_path}")
    _log(f"Backup dir: {backup_dir}")
    _log(f"Status file: {status_path}")
    _log(f"Lock file: {lock_path}")
    _log(
        "Policy: "
        f"keep_last={policy.keep_last}, keep_days={policy.keep_days}, "
        f"raw_retention_days={args.raw_retention_days}, interval_minutes={args.interval_minutes}"
    )

    if not _acquire_lock(lock_path, args.stale_lock_minutes):
        _log("Another scheduler instance appears to be running; lock acquisition failed")
        return 1

    try:
        cycle = 1
        while True:
            rc = _run_cycle(
                db_path=db_path,
                backup_dir=backup_dir,
                policy=policy,
                python_exe=python_exe,
                repo_root=repo_root,
                raw_retention_days=args.raw_retention_days,
                vacuum_every=args.vacuum_every,
                prune_strategy_quotes=not args.no_prune_strategy_quotes,
                compress_backups=args.compress_backups,
                status_path=status_path,
                cycle_number=cycle,
            )

            if args.once:
                return rc

            cycle += 1
            _log(f"Sleeping for {args.interval_minutes} minutes")
            time.sleep(interval_seconds)
    finally:
        _release_lock(lock_path)


if __name__ == "__main__":
    raise SystemExit(main())
