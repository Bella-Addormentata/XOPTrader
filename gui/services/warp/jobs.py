"""Crash-safe persistence for warp bridge jobs.

A bridge job is a long-lived, multi-step state machine (deposit → approve →
``bridgeToChia`` → attestation → signature collection → Chia claim). The GUI can
be hard-killed at any moment -- ``_kill_old_instances`` terminates the previous
process on relaunch -- so every step must be durable *before* it takes effect
and idempotent on resume. This module is that durable substrate.

Design choices
--------------
* **Its own database file** (``data/warp_jobs.db``), never the engine's
  ``xop_trader.db``. The GUI opens the engine DB strictly read-only; the warp
  store is the only warp-owned writable database, so the two never contend and
  the engine's schema/build stay untouched.
* **One open job at a time**, enforced structurally by a partial unique index
  (``WHERE active = 1``) rather than by a read-then-write check -- so two racing
  ``create_job`` calls cannot both win. A job occupies the single slot until it
  reaches :data:`CLOSED_STATES` (COMPLETED or CANCELLED).
* **Transactional update+event**: :meth:`WarpJobStore.update_job` changes job
  columns, merges the JSON state payload, and appends an audit row to
  ``warp_events`` inside a single SQLite transaction. A crash leaves either the
  whole transition or none of it.
* **Stable schema, evolving payload**: columns the service and GUI filter or
  sort on are explicit; the deep protocol state (raw signed tx bytes, predicted
  coin ids, collected signatures, the DPAPI-wrapped ephemeral BLS blob) lives in
  a single JSON ``state`` column so later commits add fields without migrations.

Standard library only -- no third-party imports -- so importing this module is
free and it works anywhere pytest runs.
"""

from __future__ import annotations

import json
import sqlite3
import threading
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Optional

# Default on-disk filename, created alongside the engine DB in ``data/``.
DB_FILENAME = "warp_jobs.db"

_BUSY_TIMEOUT_MS = 5_000
_SCHEMA_VERSION = 1


# --------------------------------------------------------------------------- #
# Job status vocabulary.
# --------------------------------------------------------------------------- #

class JobStatus:
    """String constants for every job state (persisted verbatim).

    A plain namespace of ``str`` constants rather than an ``Enum`` so the values
    round-trip through SQLite and JSON with no conversion, and other warp modules
    can compare against them directly.
    """

    AWAITING_DEPOSIT = "AWAITING_DEPOSIT"
    DEPOSIT_SEEN = "DEPOSIT_SEEN"
    APPROVING = "APPROVING"
    BRIDGING = "BRIDGING"
    BRIDGE_CONFIRMED = "BRIDGE_CONFIRMED"
    MESSAGE_SENT = "MESSAGE_SENT"
    FUNDING_CLAIM = "FUNDING_CLAIM"
    CLAIM_FUNDED = "CLAIM_FUNDED"
    COLLECTING_SIGS = "COLLECTING_SIGS"
    CLAIMING = "CLAIMING"
    COMPLETED = "COMPLETED"
    FAILED = "FAILED"
    CANCELLED = "CANCELLED"


ALL_STATES: frozenset[str] = frozenset(
    v for k, v in vars(JobStatus).items() if not k.startswith("_") and isinstance(v, str)
)

# A job in one of these states no longer occupies the single "open" slot, so a
# new job may be created. FAILED is deliberately NOT closed: a failed job still
# holds the slot until the operator resolves it (Retry to completion, or Sweep
# and dismiss to CANCELLED), preventing a second job from touching the same hot
# wallet while funds may still be recoverable.
CLOSED_STATES: frozenset[str] = frozenset({JobStatus.COMPLETED, JobStatus.CANCELLED})

# States the worker no longer auto-advances (machine-terminal), for the service
# layer's benefit; the store itself only distinguishes open vs closed.
TERMINAL_STATES: frozenset[str] = frozenset(
    {JobStatus.COMPLETED, JobStatus.FAILED, JobStatus.CANCELLED}
)


def is_open_status(status: str) -> bool:
    """Whether a job in *status* still occupies the single active-job slot."""
    return status not in CLOSED_STATES


# Columns a caller may set through create_job/update_job. ``status`` and the
# bookkeeping columns (id, active, created_at, updated_at, state) are handled
# specially; anything else must be routed through the JSON ``state`` payload.
_SETTABLE_COLUMNS: frozenset[str] = frozenset({
    "network",
    "receiver_address",
    "receiver_ph",
    "amount_usdc_micros",
    "amount_mojos",
    "post_tip_mojos",
    "bridge_tx_hash",
    "bridge_nonce",
    "last_error",
    "retry_count",
    "next_retry_at",
})


# --------------------------------------------------------------------------- #
# Errors.
# --------------------------------------------------------------------------- #

class WarpJobStoreError(RuntimeError):
    """Base class for job store failures."""


class ActiveJobExists(WarpJobStoreError):
    """A new job was requested while one is already open (single-slot rule)."""


class JobNotFound(WarpJobStoreError):
    """The referenced job id does not exist."""


class StaleJobError(WarpJobStoreError):
    """An optimistic ``expected_status`` guard did not match the stored row.

    Raised when a resumed step tries to transition from a state the job has
    already left -- the transition is rolled back rather than double-applied.
    """


# --------------------------------------------------------------------------- #
# Row types.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class WarpJob:
    """A single bridge job: its indexed columns plus the parsed state payload."""

    id: int
    status: str
    network: str
    created_at: str
    updated_at: str
    receiver_address: Optional[str]
    receiver_ph: Optional[str]
    amount_usdc_micros: Optional[int]
    amount_mojos: Optional[int]
    post_tip_mojos: Optional[int]
    bridge_tx_hash: Optional[str]
    bridge_nonce: Optional[str]
    last_error: Optional[str]
    retry_count: int
    next_retry_at: Optional[str]
    state: dict[str, Any] = field(default_factory=dict)

    @property
    def is_open(self) -> bool:
        return is_open_status(self.status)

    @property
    def is_terminal(self) -> bool:
        return self.status in TERMINAL_STATES


@dataclass(frozen=True)
class WarpEvent:
    """One audit-log row for a job."""

    id: int
    job_id: int
    ts: str
    kind: str
    status: Optional[str]
    message: str
    data: dict[str, Any] = field(default_factory=dict)


# --------------------------------------------------------------------------- #
# Store.
# --------------------------------------------------------------------------- #

_CREATE_JOBS = """
CREATE TABLE IF NOT EXISTS warp_jobs (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    status             TEXT    NOT NULL,
    active             INTEGER,               -- 1 while open, NULL once closed
    network            TEXT    NOT NULL,
    created_at         TEXT    NOT NULL,
    updated_at         TEXT    NOT NULL,
    receiver_address   TEXT,
    receiver_ph        TEXT,
    amount_usdc_micros INTEGER,
    amount_mojos       INTEGER,
    post_tip_mojos     INTEGER,
    bridge_tx_hash     TEXT,
    bridge_nonce       TEXT,
    last_error         TEXT,
    retry_count        INTEGER NOT NULL DEFAULT 0,
    next_retry_at      TEXT,
    state              TEXT    NOT NULL DEFAULT '{}'
)
"""

# At most one row may have active = 1: the single-open-job invariant, enforced
# by the database rather than by application logic.
_CREATE_ACTIVE_INDEX = """
CREATE UNIQUE INDEX IF NOT EXISTS ux_warp_jobs_active
    ON warp_jobs(active) WHERE active = 1
"""

_CREATE_EVENTS = """
CREATE TABLE IF NOT EXISTS warp_events (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id  INTEGER NOT NULL REFERENCES warp_jobs(id) ON DELETE CASCADE,
    ts      TEXT    NOT NULL,
    kind    TEXT    NOT NULL,
    status  TEXT,
    message TEXT    NOT NULL DEFAULT '',
    data    TEXT    NOT NULL DEFAULT '{}'
)
"""

_CREATE_EVENTS_INDEX = """
CREATE INDEX IF NOT EXISTS ix_warp_events_job ON warp_events(job_id, id)
"""

_CREATE_META = """
CREATE TABLE IF NOT EXISTS warp_meta (
    key   TEXT PRIMARY KEY,
    value TEXT
)
"""

_JOB_COLUMNS = (
    "id, status, network, created_at, updated_at, receiver_address, receiver_ph, "
    "amount_usdc_micros, amount_mojos, post_tip_mojos, bridge_tx_hash, "
    "bridge_nonce, last_error, retry_count, next_retry_at, state"
)


def _utcnow_iso() -> str:
    """Second-resolution UTC timestamp, e.g. ``2026-08-07T12:34:56+00:00``."""
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class WarpJobStore:
    """SQLite-backed store for warp bridge jobs and their audit events.

    A single connection is shared across the worker and GUI threads
    (``check_same_thread=False``) and every operation is serialized by a
    reentrant lock, so SQLite never sees concurrent access from two threads.
    Reads are cheap; the GUI polls :meth:`list_jobs` / :meth:`get_active_job`.

    Parameters
    ----------
    path:
        Database file path, or ``":memory:"`` for an ephemeral store (tests).
        Parent directories are created as needed.
    now:
        Injectable clock returning an ISO-8601 string; defaults to UTC now.
        Tests pass a deterministic clock.
    """

    def __init__(
        self,
        path: str | Path = DB_FILENAME,
        *,
        now: Optional[Callable[[], str]] = None,
    ) -> None:
        self._path = str(path)
        self._now = now or _utcnow_iso
        self._lock = threading.RLock()

        if self._path != ":memory:":
            Path(self._path).parent.mkdir(parents=True, exist_ok=True)

        self._conn = sqlite3.connect(
            self._path,
            timeout=_BUSY_TIMEOUT_MS / 1_000.0,
            check_same_thread=False,
        )
        self._conn.row_factory = sqlite3.Row
        # WAL keeps GUI reads from blocking worker writes; foreign_keys enforces
        # the events→jobs link; busy_timeout absorbs momentary contention.
        self._conn.execute("PRAGMA journal_mode=WAL;")
        self._conn.execute("PRAGMA foreign_keys=ON;")
        self._conn.execute(f"PRAGMA busy_timeout={_BUSY_TIMEOUT_MS};")
        self._init_schema()

    # -- lifecycle ----------------------------------------------------------- #

    def _init_schema(self) -> None:
        with self._lock, self._conn:
            self._conn.execute(_CREATE_JOBS)
            self._conn.execute(_CREATE_ACTIVE_INDEX)
            self._conn.execute(_CREATE_EVENTS)
            self._conn.execute(_CREATE_EVENTS_INDEX)
            self._conn.execute(_CREATE_META)
            row = self._conn.execute(
                "SELECT value FROM warp_meta WHERE key = 'schema_version'"
            ).fetchone()
            if row is None:
                self._conn.execute(
                    "INSERT INTO warp_meta(key, value) VALUES('schema_version', ?)",
                    (str(_SCHEMA_VERSION),),
                )

    def close(self) -> None:
        with self._lock:
            self._conn.close()

    def __enter__(self) -> "WarpJobStore":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    # -- creation ------------------------------------------------------------ #

    def create_job(
        self,
        network: str,
        *,
        status: str = JobStatus.AWAITING_DEPOSIT,
        columns: Optional[dict[str, Any]] = None,
        state: Optional[dict[str, Any]] = None,
        event_message: str = "job created",
    ) -> WarpJob:
        """Create a new job, enforcing the single-open-job rule atomically.

        A job may start at a non-default *status* (e.g. ``BRIDGE_CONFIRMED`` for
        the "claim by tx hash" recovery path). Raises :class:`ActiveJobExists`
        if another open job already holds the slot, :class:`WarpJobStoreError`
        for an unknown status or column.
        """
        if status not in ALL_STATES:
            raise WarpJobStoreError(f"unknown job status: {status!r}")
        cols = self._validate_columns(columns)
        active = 1 if is_open_status(status) else None
        ts = self._now()
        payload = json.dumps(state or {}, sort_keys=True)

        names = ["status", "active", "network", "created_at", "updated_at", "state"]
        values: list[Any] = [status, active, network, ts, ts, payload]
        for name, value in cols.items():
            names.append(name)
            values.append(value)
        placeholders = ", ".join("?" for _ in names)

        with self._lock, self._conn:
            try:
                cur = self._conn.execute(
                    f"INSERT INTO warp_jobs ({', '.join(names)}) VALUES ({placeholders})",
                    values,
                )
            except sqlite3.IntegrityError as exc:
                raise ActiveJobExists(
                    "an active warp job already exists; finish, cancel, or "
                    "sweep it before starting another"
                ) from exc
            job_id = int(cur.lastrowid)
            self._append_event_locked(
                job_id, kind="transition", status=status, message=event_message,
                data=None, ts=ts,
            )
        return self.get_job(job_id)

    # -- updates ------------------------------------------------------------- #

    def update_job(
        self,
        job_id: int,
        *,
        status: Optional[str] = None,
        expected_status: Optional[str] = None,
        columns: Optional[dict[str, Any]] = None,
        state_patch: Optional[dict[str, Any]] = None,
        event: Optional[tuple[str, str, Optional[dict[str, Any]]]] = None,
    ) -> WarpJob:
        """Transition and/or update a job, appending an event, in one transaction.

        Parameters
        ----------
        status:
            New status; the ``active`` flag is recomputed from it. Omit to keep
            the current status.
        expected_status:
            Optimistic guard. If given and it does not equal the job's current
            status, the whole call is rolled back and :class:`StaleJobError` is
            raised -- so a resumed step never double-applies a transition.
        columns:
            Explicit column values (restricted to :data:`_SETTABLE_COLUMNS`).
        state_patch:
            Shallow-merged into the JSON ``state`` payload. To drop a key, set
            it to ``None`` in the patch (the key is stored as null, not removed).
        event:
            ``(kind, message, data)`` appended to ``warp_events``. If omitted and
            *status* changed, a ``transition`` event is recorded automatically.
        """
        if status is not None and status not in ALL_STATES:
            raise WarpJobStoreError(f"unknown job status: {status!r}")
        cols = self._validate_columns(columns)
        ts = self._now()

        with self._lock, self._conn:
            row = self._conn.execute(
                f"SELECT {_JOB_COLUMNS} FROM warp_jobs WHERE id = ?", (job_id,)
            ).fetchone()
            if row is None:
                raise JobNotFound(f"no warp job with id {job_id}")
            if expected_status is not None and row["status"] != expected_status:
                raise StaleJobError(
                    f"job {job_id} is {row['status']!r}, expected "
                    f"{expected_status!r}; transition skipped"
                )

            new_status = status if status is not None else row["status"]
            active = 1 if is_open_status(new_status) else None

            set_names = ["status = ?", "active = ?", "updated_at = ?"]
            set_values: list[Any] = [new_status, active, ts]
            for name, value in cols.items():
                set_names.append(f"{name} = ?")
                set_values.append(value)

            if state_patch:
                merged = json.loads(row["state"] or "{}")
                merged.update(state_patch)
                set_names.append("state = ?")
                set_values.append(json.dumps(merged, sort_keys=True))

            set_values.append(job_id)
            self._conn.execute(
                f"UPDATE warp_jobs SET {', '.join(set_names)} WHERE id = ?",
                set_values,
            )

            if event is not None:
                kind, message, data = event
                self._append_event_locked(
                    job_id, kind=kind, status=new_status, message=message,
                    data=data, ts=ts,
                )
            elif status is not None and status != row["status"]:
                self._append_event_locked(
                    job_id, kind="transition", status=new_status,
                    message=f"{row['status']} -> {new_status}", data=None, ts=ts,
                )
        return self.get_job(job_id)

    def append_event(
        self,
        job_id: int,
        kind: str,
        message: str = "",
        data: Optional[dict[str, Any]] = None,
        status: Optional[str] = None,
    ) -> None:
        """Append a standalone audit event (does not touch the job row)."""
        with self._lock, self._conn:
            exists = self._conn.execute(
                "SELECT 1 FROM warp_jobs WHERE id = ?", (job_id,)
            ).fetchone()
            if exists is None:
                raise JobNotFound(f"no warp job with id {job_id}")
            self._append_event_locked(
                job_id, kind=kind, status=status, message=message, data=data,
                ts=self._now(),
            )

    def _append_event_locked(
        self,
        job_id: int,
        *,
        kind: str,
        status: Optional[str],
        message: str,
        data: Optional[dict[str, Any]],
        ts: str,
    ) -> None:
        self._conn.execute(
            "INSERT INTO warp_events (job_id, ts, kind, status, message, data) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (job_id, ts, kind, status, message, json.dumps(data or {}, sort_keys=True)),
        )

    # -- queries ------------------------------------------------------------- #

    def get_job(self, job_id: int) -> WarpJob:
        with self._lock:
            row = self._conn.execute(
                f"SELECT {_JOB_COLUMNS} FROM warp_jobs WHERE id = ?", (job_id,)
            ).fetchone()
        if row is None:
            raise JobNotFound(f"no warp job with id {job_id}")
        return _row_to_job(row)

    def get_active_job(self) -> Optional[WarpJob]:
        """Return the single open job, or ``None`` if the slot is free."""
        with self._lock:
            row = self._conn.execute(
                f"SELECT {_JOB_COLUMNS} FROM warp_jobs WHERE active = 1"
            ).fetchone()
        return _row_to_job(row) if row is not None else None

    def list_jobs(self, limit: int = 50) -> list[WarpJob]:
        """Most-recent jobs first (open and closed), for the GUI table."""
        with self._lock:
            rows = self._conn.execute(
                f"SELECT {_JOB_COLUMNS} FROM warp_jobs ORDER BY id DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [_row_to_job(r) for r in rows]

    def get_events(self, job_id: int, limit: int = 200) -> list[WarpEvent]:
        """Audit events for a job, oldest first."""
        with self._lock:
            rows = self._conn.execute(
                "SELECT id, job_id, ts, kind, status, message, data "
                "FROM warp_events WHERE job_id = ? ORDER BY id ASC LIMIT ?",
                (job_id, limit),
            ).fetchall()
        return [_row_to_event(r) for r in rows]

    def count_jobs(self) -> int:
        with self._lock:
            return int(
                self._conn.execute("SELECT COUNT(*) FROM warp_jobs").fetchone()[0]
            )

    # -- meta (portal-coin hint, schema version, misc singletons) ------------ #

    def get_meta(self, key: str, default: Optional[str] = None) -> Optional[str]:
        with self._lock:
            row = self._conn.execute(
                "SELECT value FROM warp_meta WHERE key = ?", (key,)
            ).fetchone()
        return row["value"] if row is not None else default

    def set_meta(self, key: str, value: str) -> None:
        with self._lock, self._conn:
            self._conn.execute(
                "INSERT INTO warp_meta(key, value) VALUES(?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                (key, value),
            )

    # -- helpers ------------------------------------------------------------- #

    @staticmethod
    def _validate_columns(columns: Optional[dict[str, Any]]) -> dict[str, Any]:
        if not columns:
            return {}
        unknown = set(columns) - _SETTABLE_COLUMNS
        if unknown:
            raise WarpJobStoreError(
                f"unknown job column(s): {', '.join(sorted(unknown))}; route "
                f"non-indexed fields through state_patch"
            )
        return dict(columns)


def _row_to_job(row: sqlite3.Row) -> WarpJob:
    return WarpJob(
        id=row["id"],
        status=row["status"],
        network=row["network"],
        created_at=row["created_at"],
        updated_at=row["updated_at"],
        receiver_address=row["receiver_address"],
        receiver_ph=row["receiver_ph"],
        amount_usdc_micros=row["amount_usdc_micros"],
        amount_mojos=row["amount_mojos"],
        post_tip_mojos=row["post_tip_mojos"],
        bridge_tx_hash=row["bridge_tx_hash"],
        bridge_nonce=row["bridge_nonce"],
        last_error=row["last_error"],
        retry_count=row["retry_count"],
        next_retry_at=row["next_retry_at"],
        state=json.loads(row["state"] or "{}"),
    )


def _row_to_event(row: sqlite3.Row) -> WarpEvent:
    return WarpEvent(
        id=row["id"],
        job_id=row["job_id"],
        ts=row["ts"],
        kind=row["kind"],
        status=row["status"],
        message=row["message"],
        data=json.loads(row["data"] or "{}"),
    )
