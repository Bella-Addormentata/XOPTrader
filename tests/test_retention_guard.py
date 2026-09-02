"""The retention prune must refuse a bulk deletion, and say what it kept.

`scripts/maintain_snapshot_rollups.py` deletes raw history on a schedule.
Three behaviours shipped in that script without any test at all:

* the 25% proportion guard -- a run that would remove more than a quarter of
  a raw table refuses and exits 3, because a LONG GAP between runs silently
  converts a routine window into a bulk deletion;
* the WAL-consistent `--backup`, which must capture committed pages that are
  still only in the `-wal` file;
* the cutoff format, which must be the space-separated "YYYY-MM-DD HH:MM:SS"
  these tables actually store, not `_iso_utc()`'s "T"-form -- " " sorts below
  "T", so a "T"-form cutoff over-deleted by up to a day.

There is also a latent drift risk pinned here: the guard counts over a
`targets` list while the DELETEs name their tables literally. Today they
agree; nothing but this file makes them keep agreeing.

Every test builds its own throwaway database under `tmp_path`. Nothing here
opens `data/xop_trader.db` -- `_run()` always supplies `--db` so the
script's live default can never be reached.
"""

from __future__ import annotations

import importlib.util
import re
import shutil
import sqlite3
import sys
from datetime import datetime, time, timedelta, timezone
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT = REPO_ROOT / "scripts" / "maintain_snapshot_rollups.py"

# The stored form. Both tables default created_at to CURRENT_TIMESTAMP,
# which SQLite writes as UTC with a SPACE between date and time.
STORED_FMT = "%Y-%m-%d %H:%M:%S"

SNAPSHOTS_DDL = """
CREATE TABLE snapshots (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    block_height     INTEGER NOT NULL,
    pair_name        TEXT    NOT NULL,
    mid_price_mojos  INTEGER,
    spread_bps       REAL,
    inventory_ratio  REAL,
    sigma_block      REAL,
    regime           TEXT,
    pnl_total_mojos  INTEGER,
    created_at       TEXT    DEFAULT CURRENT_TIMESTAMP,
    xch_usd_rate     REAL DEFAULT 0,
    pnl_total_usd    REAL DEFAULT 0
)
"""

STRATEGY_QUOTES_DDL = """
CREATE TABLE strategy_quotes (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    block_height    INTEGER NOT NULL,
    pair_name       TEXT    NOT NULL,
    tier            INTEGER NOT NULL,
    side            TEXT    NOT NULL CHECK(side IN ('bid','ask')),
    price_mojos     INTEGER NOT NULL,
    size_mojos      INTEGER NOT NULL,
    created_at      TEXT    DEFAULT CURRENT_TIMESTAMP
)
"""


def _load_script():
    """Import the maintenance script by path, the way the operator runs it."""
    spec = importlib.util.spec_from_file_location("xop_rollups_test", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules["xop_rollups_test"] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop("xop_rollups_test", None)
        raise
    return module


def _stamp(days_ago: float) -> str:
    dt = datetime.now(tz=timezone.utc) - timedelta(days=days_ago)
    return dt.strftime(STORED_FMT)


def _make_db(
    path: Path,
    *,
    snap_old: int = 0,
    snap_new: int = 0,
    quotes_old: int = 0,
    quotes_new: int = 0,
    old_days: float = 300.0,
    new_days: float = 1.0,
) -> Path:
    """A synthetic two-table database. Never the live one."""
    conn = sqlite3.connect(str(path))
    conn.execute(SNAPSHOTS_DDL)
    conn.execute(STRATEGY_QUOTES_DDL)

    for count, days in ((snap_old, old_days), (snap_new, new_days)):
        for i in range(count):
            conn.execute(
                "INSERT INTO snapshots (block_height, pair_name, mid_price_mojos,"
                " spread_bps, inventory_ratio, sigma_block, regime,"
                " pnl_total_mojos, created_at, xch_usd_rate, pnl_total_usd)"
                " VALUES (?, 'XCH/BYC', ?, 40.0, 0.5, 0.01, 'NORMAL', 0, ?, 1.43, 0.0)",
                (1000 + i, 1_410_000_000 + i, _stamp(days)),
            )

    for count, days in ((quotes_old, old_days), (quotes_new, new_days)):
        for i in range(count):
            conn.execute(
                "INSERT INTO strategy_quotes (block_height, pair_name, tier, side,"
                " price_mojos, size_mojos, created_at)"
                " VALUES (?, 'XCH/BYC', 1, 'bid', 1400000000, 1000000, ?)",
                (1000 + i, _stamp(days)),
            )

    conn.commit()
    conn.close()
    return path


def _counts(path: Path) -> dict[str, int]:
    conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        return {
            t: int(conn.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0])
            for t in ("snapshots", "strategy_quotes")
        }
    finally:
        conn.close()


def _run(module, db: Path, *extra: str) -> int:
    """Invoke main() with --db always pinned to the synthetic database."""
    argv = ["maintain_snapshot_rollups.py", "--db", str(db), *extra]
    saved = sys.argv
    sys.argv = argv
    try:
        return module.main()
    finally:
        sys.argv = saved


def _refusal_block(out: str) -> str:
    """Only the REFUSED message -- the rollup summary names snapshots_1m."""
    assert "REFUSED: " in out, out
    return out.split("REFUSED: ", 1)[1]


# --------------------------------------------------------------------------
# The proportion guard
# --------------------------------------------------------------------------


def test_a_bulk_prune_refuses_and_changes_nothing(tmp_path, capsys):
    """60% of a table is the dangerous case: refuse, exit 3, delete nothing."""
    db = _make_db(tmp_path / "synthetic.db", snap_old=60, snap_new=40)
    before = _counts(db)

    rc = _run(_load_script(), db, "--raw-retention-days", "120")
    out = capsys.readouterr().out

    assert rc == 3
    assert _counts(db) == before
    body = _refusal_block(out)
    assert "60 of 100 rows (60.0%)" in body


def test_confirm_large_prune_proceeds(tmp_path, capsys):
    """The flag is the whole point of the refusal: it must actually work."""
    db = _make_db(tmp_path / "synthetic.db", snap_old=60, snap_new=40)

    rc = _run(
        _load_script(), db, "--raw-retention-days", "120", "--confirm-large-prune"
    )
    capsys.readouterr()

    assert rc == 0
    assert _counts(db)["snapshots"] == 40


def test_a_widened_retention_window_proceeds(tmp_path, capsys):
    """The other documented way out: keep the history instead of deleting it."""
    db = _make_db(tmp_path / "synthetic.db", snap_old=60, snap_new=40)
    before = _counts(db)

    rc = _run(_load_script(), db, "--raw-retention-days", "400")
    capsys.readouterr()

    assert rc == 0
    assert _counts(db) == before


def test_the_threshold_is_a_proportion_not_a_row_count(tmp_path, capsys):
    """A quarter is fine; the same absolute number out of fewer rows is not."""
    module = _load_script()

    # Exactly 25% of 80 rows: the bound is strict, so this proceeds.
    ok = _make_db(tmp_path / "ok.db", snap_old=20, snap_new=60)
    assert _run(module, ok, "--raw-retention-days", "120") == 0
    assert _counts(ok)["snapshots"] == 60

    # The same 20 rows, out of 70: 28.6%, and it refuses.
    bad = _make_db(tmp_path / "bad.db", snap_old=20, snap_new=50)
    before = _counts(bad)
    assert _run(module, bad, "--raw-retention-days", "120") == 3
    assert _counts(bad) == before
    capsys.readouterr()


# --------------------------------------------------------------------------
# What the refusal tells the operator
# --------------------------------------------------------------------------


def test_each_breached_table_reports_its_own_oldest_row(tmp_path, capsys):
    """The window has to clear whichever table reaches furthest back."""
    db = _make_db(
        tmp_path / "synthetic.db",
        snap_old=60,
        snap_new=40,
        quotes_old=60,
        quotes_new=40,
    )
    # Push strategy_quotes further back than snapshots so the two oldest
    # rows are visibly different dates.
    conn = sqlite3.connect(str(db))
    conn.execute(
        "UPDATE strategy_quotes SET created_at = ? WHERE id = 1", (_stamp(365),)
    )
    conn.commit()
    conn.close()

    oldest = {}
    ro = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    for t in ("snapshots", "strategy_quotes"):
        oldest[t] = ro.execute(f"SELECT MIN(created_at) FROM {t}").fetchone()[0]
    ro.close()
    assert oldest["snapshots"] != oldest["strategy_quotes"]

    rc = _run(_load_script(), db, "--raw-retention-days", "120")
    body = _refusal_block(capsys.readouterr().out)

    assert rc == 3
    assert "snapshots" in body and "strategy_quotes" in body
    # Each table's own MIN(created_at), not one table's stood in for both.
    assert body.count("oldest row") == 2
    for value in oldest.values():
        assert f"oldest row {value}" in body


def test_a_single_breached_table_is_reported_alone(tmp_path, capsys):
    """Naming an unrelated table misleads the very decision this is for."""
    db = _make_db(
        tmp_path / "synthetic.db",
        snap_old=5,
        snap_new=95,
        quotes_old=60,
        quotes_new=40,
    )
    before = _counts(db)

    rc = _run(_load_script(), db, "--raw-retention-days", "120")
    body = _refusal_block(capsys.readouterr().out)

    assert rc == 3
    assert _counts(db) == before
    assert "strategy_quotes : 60 of 100 rows (60.0%)" in body
    assert body.count("oldest row") == 1
    # snapshots was under the threshold, so it must not appear as a breach.
    assert not re.search(r"^\s+snapshots\s+:", body, re.MULTILINE)


# --------------------------------------------------------------------------
# The cutoff format
# --------------------------------------------------------------------------


def test_the_cutoff_is_formatted_the_way_the_rows_are_stored(tmp_path, capsys):
    """A "T"-form cutoff compares wrong against space-separated rows."""
    db = _make_db(tmp_path / "synthetic.db", snap_old=60, snap_new=40)

    _run(_load_script(), db, "--raw-retention-days", "120")
    body = _refusal_block(capsys.readouterr().out)

    match = re.search(r"cutoff\s+: (\S+ \S+)", body)
    assert match, body
    cutoff = match.group(1)
    assert re.fullmatch(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}", cutoff), cutoff
    assert "T" not in cutoff and "Z" not in cutoff


def test_a_row_late_on_the_cutoff_date_is_inside_the_window(tmp_path, capsys):
    """" " (0x20) sorts below "T", so the old cutoff ate a whole extra day.

    A row stored at 23:59:59 on the cutoff's own DATE is inside a 120-day
    window. Against a "T"-form cutoff it compared less-than regardless of
    its time and was deleted.
    """
    retention = 120
    cutoff_dt = datetime.now(tz=timezone.utc) - timedelta(days=retention)
    late = datetime.combine(cutoff_dt.date(), time(23, 59, 59), tzinfo=timezone.utc)
    if late <= cutoff_dt:  # only in the final second of a UTC day
        pytest.skip("cutoff landed at the very end of its own date")

    db = _make_db(tmp_path / "synthetic.db", snap_new=99)
    conn = sqlite3.connect(str(db))
    conn.execute(
        "INSERT INTO snapshots (block_height, pair_name, mid_price_mojos,"
        " spread_bps, inventory_ratio, sigma_block, regime, pnl_total_mojos,"
        " created_at, xch_usd_rate, pnl_total_usd)"
        " VALUES (1, 'XCH/BYC', 1410000000, 40.0, 0.5, 0.01, 'NORMAL', 0, ?, 1.43, 0.0)",
        (late.strftime(STORED_FMT),),
    )
    conn.commit()
    conn.close()

    rc = _run(_load_script(), db, "--raw-retention-days", str(retention))
    capsys.readouterr()

    assert rc == 0
    ro = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    survived = ro.execute(
        "SELECT COUNT(*) FROM snapshots WHERE created_at = ?",
        (late.strftime(STORED_FMT),),
    ).fetchone()[0]
    ro.close()
    assert survived == 1


# --------------------------------------------------------------------------
# The guard's scope and the DELETEs' scope must be the same set
# --------------------------------------------------------------------------


def test_the_guard_covers_exactly_the_tables_the_deletes_touch(tmp_path, capsys):
    """strategy_quotes is in the guard iff it is in the prune."""
    module = _load_script()

    # In scope: it alone breaches, and the run refuses.
    guarded = _make_db(
        tmp_path / "guarded.db",
        snap_old=5,
        snap_new=95,
        quotes_old=60,
        quotes_new=40,
    )
    before = _counts(guarded)
    assert _run(module, guarded, "--raw-retention-days", "120") == 3
    assert _counts(guarded) == before

    # Out of scope: the same database, the same window, no refusal -- and
    # strategy_quotes keeps every row.
    skipped = _make_db(
        tmp_path / "skipped.db",
        snap_old=5,
        snap_new=95,
        quotes_old=60,
        quotes_new=40,
    )
    rc = _run(
        module,
        skipped,
        "--raw-retention-days",
        "120",
        "--no-prune-strategy-quotes",
    )
    capsys.readouterr()
    assert rc == 0
    after = _counts(skipped)
    assert after["strategy_quotes"] == 100
    assert after["snapshots"] == 95


def test_the_targets_list_and_the_delete_statements_cannot_drift():
    """A third table added to one half and not the other is unguarded."""
    source = SCRIPT.read_text(encoding="utf-8")
    body = source.split("def _prune_raw_tables(", 1)[1].split("\ndef ", 1)[0]

    deleted = set(re.findall(r"DELETE FROM (\w+)", body))
    counted = set(re.findall(r'targets = \["(\w+)"\]', body))
    counted |= set(re.findall(r'targets\.append\("(\w+)"\)', body))

    assert deleted, body
    assert deleted == counted, (
        f"tables deleted {sorted(deleted)} but counted by the guard "
        f"{sorted(counted)}; a table in only one half is a table the "
        f"proportion guard cannot protect"
    )


# --------------------------------------------------------------------------
# The refusal must not discard the rollups
# --------------------------------------------------------------------------


def test_the_rollups_survive_a_refusal_and_are_reported(tmp_path, capsys):
    """Refusing the DELETEs is not a reason to throw away derived work.

    The rollup UPSERTs are idempotent and non-destructive. Sharing one
    transaction with the prune meant the refusal's rollback discarded them,
    and returned before the summary was printed, so the operator saw no
    sign they had run.
    """
    db = _make_db(tmp_path / "synthetic.db", snap_old=60, snap_new=40)

    rc = _run(_load_script(), db, "--raw-retention-days", "120")
    out = capsys.readouterr().out

    assert rc == 3
    assert "Rollup summary" in out
    assert out.index("Rollup summary") < out.index("REFUSED"), out

    ro = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        for table in ("snapshots_1m", "snapshots_15m", "snapshots_1h", "snapshots_1d"):
            rows = ro.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            assert rows > 0, f"{table} did not persist through the refusal"
    finally:
        ro.close()

    # And the raw tables are still untouched.
    assert _counts(db) == {"snapshots": 100, "strategy_quotes": 0}


def test_a_dry_run_says_the_rollup_summary_was_not_written(tmp_path, capsys):
    """The summary now prints before the commit decision -- label it."""
    db = _make_db(tmp_path / "synthetic.db", snap_old=5, snap_new=95)

    rc = _run(_load_script(), db, "--raw-retention-days", "120", "--dry-run")
    out = capsys.readouterr().out

    assert rc == 0
    assert "Rollup summary:  (dry-run; rolled back)" in out
    assert _counts(db) == {"snapshots": 100, "strategy_quotes": 0}
    ro = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        present = ro.execute(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table'"
            " AND name LIKE 'snapshots_%'"
        ).fetchone()[0]
    finally:
        ro.close()
    assert present == 0, "a dry run wrote rollup tables"


# --------------------------------------------------------------------------
# The backup
# --------------------------------------------------------------------------


def test_the_backup_captures_committed_pages_still_in_the_wal(tmp_path, capsys):
    """A plain file copy leaves the -wal behind; conn.backup() does not."""
    db = _make_db(tmp_path / "synthetic.db", snap_old=5, snap_new=95)

    # A writer that commits and stays open: the -wal is not checkpointed
    # while it holds the database, which is exactly the live engine's shape.
    keeper = sqlite3.connect(str(db))
    keeper.execute("PRAGMA journal_mode=WAL")
    for i in range(50):
        keeper.execute(
            "INSERT INTO snapshots (block_height, pair_name, mid_price_mojos,"
            " spread_bps, inventory_ratio, sigma_block, regime, pnl_total_mojos,"
            " created_at, xch_usd_rate, pnl_total_usd)"
            " VALUES (?, 'XCH/BYC', 1410000000, 40.0, 0.5, 0.01, 'NORMAL', 0, ?,"
            " 1.43, 0.0)",
            (9000 + i, _stamp(1)),
        )
    keeper.commit()

    try:
        assert (tmp_path / "synthetic.db-wal").exists()

        naive = tmp_path / "naive_copy.db"
        shutil.copy2(db, naive)
        naive_conn = sqlite3.connect(f"file:{naive}?mode=ro", uri=True)
        naive_rows = naive_conn.execute("SELECT COUNT(*) FROM snapshots").fetchone()[0]
        naive_conn.close()

        rc = _run(_load_script(), db, "--raw-retention-days", "400", "--backup")
        out = capsys.readouterr().out
        assert rc == 0
        assert "Backup created:" in out
    finally:
        keeper.close()

    backups = list(tmp_path.glob("synthetic.db.rollup.*.bak"))
    assert len(backups) == 1, backups
    backup_conn = sqlite3.connect(f"file:{backups[0]}?mode=ro", uri=True)
    backup_rows = backup_conn.execute("SELECT COUNT(*) FROM snapshots").fetchone()[0]
    backup_conn.close()

    assert backup_rows == 150
    assert naive_rows < backup_rows, (
        "the -wal held no uncheckpointed pages, so this test proved nothing"
    )
