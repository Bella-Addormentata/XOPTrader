"""SQLite database reader for XOPTrader GUI.

Opens the engine's WAL-mode SQLite database in **read-only** mode and
provides non-blocking query methods that run on a dedicated ``QThread``
worker.  Results are delivered via Qt signals so the GUI event loop is
never blocked.

Tables accessed (all read-only from the GUI):
    - ``trade_log``   -- executed fills
    - ``offer_log``   -- order lifecycle
    - ``snapshots``   -- periodic market / portfolio snapshots

Compliant with:
    - ISO/IEC 27001:2022  (read-only DB access; no SQL injection via
      parameterised queries)
    - ISO/IEC 5055       (bounded result sets via LIMIT; no resource leaks)
    - ISO/IEC 25000      (graceful degradation on locked DB)
"""

from __future__ import annotations

import logging
import sqlite3
from pathlib import Path
from typing import Any, Final, Optional

from PySide6.QtCore import (
    QMutex,
    QMutexLocker,
    QObject,
    QThread,
    QTimer,
    Signal,
    Slot,
)

# ---------------------------------------------------------------------------
# Module-level logger and constants
# ---------------------------------------------------------------------------
_log: logging.Logger = logging.getLogger(__name__)

# Default auto-refresh interval (milliseconds).
_DEFAULT_REFRESH_MS: Final[int] = 10_000

# Maximum rows returned by any single query (safety cap).
_MAX_ROWS: Final[int] = 10_000

# SQLite busy-timeout (milliseconds) -- how long to wait when the DB is
# locked by the writer (the C++ engine) before raising OperationalError.
_BUSY_TIMEOUT_MS: Final[int] = 3_000

# Retry delay after a locked-DB error (milliseconds).
_RETRY_DELAY_MS: Final[int] = 2_000


# ===================================================================
# Worker -- runs queries on a background QThread
# ===================================================================

class _DatabaseWorker(QObject):
    """Background worker that executes SQLite queries and emits results.

    This object is moved to a ``QThread`` and should **never** be called
    directly from the main thread.  Use signal/slot queued connections
    to dispatch work.
    """

    # -- Result signals (one per query type) --------------------------------
    trades_ready = Signal(list)
    offers_ready = Signal(list)
    snapshots_ready = Signal(list)
    trade_summary_ready = Signal(dict)
    pairs_list_ready = Signal(list)
    latest_snapshot_ready = Signal(dict)
    query_error = Signal(str)

    def __init__(self, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._conn: Optional[sqlite3.Connection] = None
        self._db_path: str = ""

    # -- Connection management ----------------------------------------------

    def open(self, db_path: str) -> None:
        """Open a read-only SQLite connection.

        Parameters
        ----------
        db_path : str
            Filesystem path to the SQLite database file.
        """
        self._db_path = db_path
        uri = f"file:{db_path}?mode=ro"
        try:
            self._conn = sqlite3.connect(
                uri,
                uri=True,
                timeout=_BUSY_TIMEOUT_MS / 1_000.0,
                check_same_thread=False,
            )
            # Return rows as sqlite3.Row for dict-like access.
            self._conn.row_factory = sqlite3.Row
            # Enable WAL read mode -- allows concurrent reads while the
            # engine writes.
            self._conn.execute("PRAGMA journal_mode=WAL;")
            self._conn.execute(f"PRAGMA busy_timeout={_BUSY_TIMEOUT_MS};")
            _log.info("Database opened (read-only): %s", db_path)
        except sqlite3.Error as exc:
            msg = f"Failed to open database: {exc}"
            _log.error(msg)
            self.query_error.emit(msg)

    def close(self) -> None:
        """Close the database connection if open."""
        if self._conn is not None:
            try:
                self._conn.close()
            except sqlite3.Error as exc:
                _log.warning("Error closing database: %s", exc)
            finally:
                self._conn = None
                _log.info("Database connection closed.")

    # -- Query slots (invoked from main thread via queued connection) --------

    @Slot(str, str, object, object, int)
    def fetch_trades(
        self,
        pair: str,
        side: str,
        from_block: Optional[int],
        to_block: Optional[int],
        limit: int,
    ) -> None:
        """Query ``trade_log`` with optional filters.

        Parameters
        ----------
        pair : str
            Filter by pair_name (empty string = no filter).
        side : str
            Filter by side (empty string = no filter).
        from_block : int | None
            Minimum block_height (inclusive).
        to_block : int | None
            Maximum block_height (inclusive).
        limit : int
            Max rows to return.
        """
        clauses: list[str] = []
        params: list[Any] = []

        if pair:
            clauses.append("pair_name = ?")
            params.append(pair)
        if side:
            clauses.append("side = ?")
            params.append(side)
        if from_block is not None:
            clauses.append("block_height >= ?")
            params.append(from_block)
        if to_block is not None:
            clauses.append("block_height <= ?")
            params.append(to_block)

        where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
        safe_limit = min(max(1, limit), _MAX_ROWS)
        sql = (
            f"SELECT * FROM trade_log{where} "
            f"ORDER BY block_height DESC LIMIT ?"
        )
        params.append(safe_limit)

        self._execute_and_emit(sql, params, self.trades_ready)

    @Slot(str, str, int)
    def fetch_offers(
        self,
        pair: str,
        status: str,
        limit: int,
    ) -> None:
        """Query ``offer_log`` with optional filters.

        Parameters
        ----------
        pair : str
            Filter by pair_name (empty string = no filter).
        status : str
            Filter by offer status (empty string = no filter).
        limit : int
            Max rows to return.
        """
        clauses: list[str] = []
        params: list[Any] = []

        if pair:
            clauses.append("pair_name = ?")
            params.append(pair)
        if status:
            clauses.append("status = ?")
            params.append(status)

        where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
        safe_limit = min(max(1, limit), _MAX_ROWS)
        sql = (
            f"SELECT * FROM offer_log{where} "
            f"ORDER BY created_block DESC LIMIT ?"
        )
        params.append(safe_limit)

        self._execute_and_emit(sql, params, self.offers_ready)

    @Slot(str, int, int)
    def fetch_snapshots(
        self,
        pair: str,
        from_block: int,
        to_block: int,
    ) -> None:
        """Query ``snapshots`` for a pair within a block range.

        Parameters
        ----------
        pair : str
            Pair name.
        from_block : int
            Start block (inclusive).
        to_block : int
            End block (inclusive).
        """
        sql = (
            "SELECT * FROM snapshots "
            "WHERE pair_name = ? AND block_height >= ? AND block_height <= ? "
            "ORDER BY block_height ASC"
        )
        self._execute_and_emit(sql, [pair, from_block, to_block], self.snapshots_ready)

    @Slot()
    def fetch_trade_summary(self) -> None:
        """Compute aggregate trade statistics.

        Emits ``trade_summary_ready`` with a dict containing:
        ``total_trades``, ``total_pnl``, ``win_count``, ``loss_count``,
        ``avg_fill_size``.
        """
        sql = """
            SELECT
                COUNT(*)                                      AS total_trades,
                COALESCE(SUM(realized_pnl_mojos), 0)         AS total_pnl,
                COALESCE(SUM(CASE WHEN realized_pnl_mojos > 0 THEN 1 ELSE 0 END), 0) AS win_count,
                COALESCE(SUM(CASE WHEN realized_pnl_mojos < 0 THEN 1 ELSE 0 END), 0) AS loss_count,
                COALESCE(AVG(size_mojos), 0)                  AS avg_fill_size
            FROM trade_log
        """
        rows = self._execute_query(sql, [])
        if rows is None:
            return

        if rows:
            row = rows[0]
            result = {
                "total_trades": row["total_trades"],
                "total_pnl": row["total_pnl"],
                "win_count": row["win_count"],
                "loss_count": row["loss_count"],
                "avg_fill_size": row["avg_fill_size"],
            }
        else:
            result = {
                "total_trades": 0,
                "total_pnl": 0,
                "win_count": 0,
                "loss_count": 0,
                "avg_fill_size": 0,
            }
        self.trade_summary_ready.emit(result)

    @Slot()
    def fetch_pairs_list(self) -> None:
        """Query distinct pair names from ``trade_log``.

        Emits ``pairs_list_ready`` with a list of strings.
        """
        sql = "SELECT DISTINCT pair_name FROM trade_log ORDER BY pair_name"
        rows = self._execute_query(sql, [])
        if rows is None:
            return
        self.pairs_list_ready.emit([row["pair_name"] for row in rows])

    @Slot(str)
    def fetch_latest_snapshot(self, pair: str) -> None:
        """Fetch the most recent snapshot for a trading pair.

        Parameters
        ----------
        pair : str
            Pair name.
        """
        sql = (
            "SELECT * FROM snapshots "
            "WHERE pair_name = ? "
            "ORDER BY block_height DESC LIMIT 1"
        )
        rows = self._execute_query(sql, [pair])
        if rows is None:
            return

        if rows:
            self.latest_snapshot_ready.emit(dict(rows[0]))
        else:
            self.latest_snapshot_ready.emit({})

    # -- Internal helpers ---------------------------------------------------

    def _execute_query(
        self,
        sql: str,
        params: list[Any],
    ) -> Optional[list[sqlite3.Row]]:
        """Execute *sql* with *params* and return rows, or None on error.

        Parameters
        ----------
        sql : str
            Parameterised SQL statement.
        params : list
            Bind parameters.

        Returns
        -------
        list[sqlite3.Row] | None
            Result rows, or None if an error occurred (error is emitted
            via ``query_error`` signal).
        """
        if self._conn is None:
            self.query_error.emit("Database not connected.")
            return None
        try:
            cursor = self._conn.execute(sql, params)
            return cursor.fetchall()
        except sqlite3.OperationalError as exc:
            msg = f"Database query error: {exc}"
            _log.warning(msg)
            self.query_error.emit(msg)
            return None
        except sqlite3.Error as exc:
            msg = f"Database error: {exc}"
            _log.error(msg)
            self.query_error.emit(msg)
            return None

    def _execute_and_emit(
        self,
        sql: str,
        params: list[Any],
        signal: Signal,
    ) -> None:
        """Execute a query and emit the results as a list of dicts.

        Parameters
        ----------
        sql : str
            Parameterised SQL statement.
        params : list
            Bind parameters.
        signal : Signal
            Qt signal to emit with the list[dict] result.
        """
        rows = self._execute_query(sql, params)
        if rows is None:
            return
        signal.emit([dict(row) for row in rows])


# ===================================================================
# Main service -- lives on the GUI thread
# ===================================================================

class DatabaseService(QObject):
    """Non-blocking SQLite reader with auto-refresh.

    All heavy I/O runs on a background ``QThread``.  Widget code
    connects to the result signals and calls the ``query_*`` methods,
    which dispatch work to the worker.

    Parameters
    ----------
    db_path : Path | str
        Path to the SQLite database file.
    refresh_interval_ms : int
        Auto-refresh period in milliseconds (default 10 000).
    parent : QObject | None
        Optional Qt parent.

    Signals
    -------
    trades_loaded(list)
        List of trade dicts from ``trade_log``.
    offers_loaded(list)
        List of offer dicts from ``offer_log``.
    snapshots_loaded(list)
        List of snapshot dicts from ``snapshots``.
    query_error(str)
        Human-readable error message on any query failure.
    """

    # -- Qt signals (forwarded from worker) ---------------------------------
    trades_loaded = Signal(list)
    offers_loaded = Signal(list)
    snapshots_loaded = Signal(list)
    trade_summary_loaded = Signal(dict)
    pairs_list_loaded = Signal(list)
    latest_snapshot_loaded = Signal(dict)
    query_error = Signal(str)

    # -- Internal trigger signals (queued connections to worker thread) ------
    _trigger_open = Signal(str)
    _trigger_close = Signal()
    _trigger_trades = Signal(str, str, object, object, int)
    _trigger_offers = Signal(str, str, int)
    _trigger_snapshots = Signal(str, int, int)
    _trigger_summary = Signal()
    _trigger_pairs = Signal()
    _trigger_latest_snapshot = Signal(str)

    def __init__(
        self,
        db_path: Path | str,
        refresh_interval_ms: int = _DEFAULT_REFRESH_MS,
        parent: Optional[QObject] = None,
    ) -> None:
        super().__init__(parent)

        self._db_path: Path = Path(db_path).resolve()
        self._refresh_ms: int = max(1_000, refresh_interval_ms)

        # -- Worker thread --------------------------------------------------
        self._thread: QThread = QThread(self)
        self._thread.setObjectName("DatabaseWorkerThread")

        self._worker: _DatabaseWorker = _DatabaseWorker()
        self._worker.moveToThread(self._thread)

        # Forward worker signals to the service's public signals.
        self._worker.trades_ready.connect(self.trades_loaded)
        self._worker.offers_ready.connect(self.offers_loaded)
        self._worker.snapshots_ready.connect(self.snapshots_loaded)
        self._worker.trade_summary_ready.connect(self.trade_summary_loaded)
        self._worker.pairs_list_ready.connect(self.pairs_list_loaded)
        self._worker.latest_snapshot_ready.connect(self.latest_snapshot_loaded)
        self._worker.query_error.connect(self._on_worker_error)

        # Queued connections: emit trigger signals to dispatch work to
        # the worker thread rather than calling worker methods directly
        # (which would execute SQLite I/O on the GUI thread).
        self._trigger_open.connect(self._worker.open)
        self._trigger_close.connect(self._worker.close)
        self._trigger_trades.connect(self._worker.fetch_trades)
        self._trigger_offers.connect(self._worker.fetch_offers)
        self._trigger_snapshots.connect(self._worker.fetch_snapshots)
        self._trigger_summary.connect(self._worker.fetch_trade_summary)
        self._trigger_pairs.connect(self._worker.fetch_pairs_list)
        self._trigger_latest_snapshot.connect(self._worker.fetch_latest_snapshot)

        # -- Auto-refresh timer ---------------------------------------------
        self._refresh_timer: QTimer = QTimer(self)
        self._refresh_timer.setTimerType(QTimer.TimerType.CoarseTimer)
        self._refresh_timer.timeout.connect(self._on_auto_refresh)

        # Track the last auto-refresh query parameters so the timer
        # can re-issue the most recently requested data set.
        self._mutex: QMutex = QMutex()
        self._last_trade_params: Optional[tuple[str, str, Optional[int], Optional[int], int]] = None
        self._last_offer_params: Optional[tuple[str, str, int]] = None

        _log.info(
            "DatabaseService created: path=%s, refresh=%d ms",
            self._db_path,
            self._refresh_ms,
        )

    # ===================================================================
    # Lifecycle
    # ===================================================================

    def start(self) -> bool:
        """Open the database and start the worker thread.

        Returns
        -------
        bool
            ``True`` if the database file exists and the thread started.
        """
        if not self._db_path.is_file():
            msg = f"Database file not found: {self._db_path}"
            _log.error(msg)
            self.query_error.emit(msg)
            return False

        _log.info("Starting DatabaseService.")
        self._thread.start()

        # Open the connection on the worker thread via queued signal.
        self._trigger_open.emit(str(self._db_path))

        # Start auto-refresh.
        self._refresh_timer.start(self._refresh_ms)
        return True

    def stop(self) -> None:
        """Close the database and shut down the worker thread."""
        _log.info("Stopping DatabaseService.")
        self._refresh_timer.stop()
        self._trigger_close.emit()
        self._thread.quit()
        if not self._thread.wait(5_000):
            _log.warning("DatabaseService worker thread did not exit in time.")

    # ===================================================================
    # Public query API (non-blocking, results via signals)
    # ===================================================================

    def query_trades(
        self,
        pair: Optional[str] = None,
        side: Optional[str] = None,
        from_block: Optional[int] = None,
        to_block: Optional[int] = None,
        limit: int = 500,
    ) -> None:
        """Request trades from ``trade_log``.

        Results arrive on :pyattr:`trades_loaded`.

        Parameters
        ----------
        pair : str | None
            Filter by pair name (``None`` = all pairs).
        side : str | None
            Filter by side (``None`` = both).
        from_block : int | None
            Minimum block height (inclusive).
        to_block : int | None
            Maximum block height (inclusive).
        limit : int
            Maximum number of rows (default 500).
        """
        safe_pair = pair or ""
        safe_side = side or ""
        safe_limit = min(max(1, limit), _MAX_ROWS)

        # Remember params for auto-refresh re-queries.
        with QMutexLocker(self._mutex):
            self._last_trade_params = (safe_pair, safe_side, from_block, to_block, safe_limit)

        self._trigger_trades.emit(safe_pair, safe_side, from_block, to_block, safe_limit)

    def query_offers(
        self,
        pair: Optional[str] = None,
        status: Optional[str] = None,
        limit: int = 500,
    ) -> None:
        """Request offers from ``offer_log``.

        Results arrive on :pyattr:`offers_loaded`.

        Parameters
        ----------
        pair : str | None
            Filter by pair name (``None`` = all pairs).
        status : str | None
            Filter by offer status (``None`` = all statuses).
        limit : int
            Maximum number of rows (default 500).
        """
        safe_pair = pair or ""
        safe_status = status or ""
        safe_limit = min(max(1, limit), _MAX_ROWS)

        with QMutexLocker(self._mutex):
            self._last_offer_params = (safe_pair, safe_status, safe_limit)

        self._trigger_offers.emit(safe_pair, safe_status, safe_limit)

    def query_snapshots(
        self,
        pair: str,
        from_block: int,
        to_block: int,
    ) -> None:
        """Request snapshots for a pair in a block range.

        Results arrive on :pyattr:`snapshots_loaded`.

        Parameters
        ----------
        pair : str
            Pair name.
        from_block : int
            Start block (inclusive).
        to_block : int
            End block (inclusive).
        """
        self._trigger_snapshots.emit(pair, from_block, to_block)

    def get_trade_summary(self) -> None:
        """Request aggregate trade statistics.

        Results arrive on :pyattr:`trade_summary_loaded`.
        """
        self._trigger_summary.emit()

    def get_pairs_list(self) -> None:
        """Request distinct pair names.

        Results arrive on :pyattr:`pairs_list_loaded`.
        """
        self._trigger_pairs.emit()

    def get_latest_snapshot(self, pair: str) -> None:
        """Request the most recent snapshot for a pair.

        Results arrive on :pyattr:`latest_snapshot_loaded`.

        Parameters
        ----------
        pair : str
            Pair name.
        """
        self._trigger_latest_snapshot.emit(pair)

    # ===================================================================
    # Internal slots
    # ===================================================================

    @Slot()
    def _on_auto_refresh(self) -> None:
        """Re-issue the most recently requested trades and offers query.

        Called by the refresh timer so the UI stays current with the
        database without explicit user action.
        """
        with QMutexLocker(self._mutex):
            trade_params = self._last_trade_params
            offer_params = self._last_offer_params

        if trade_params is not None:
            self._trigger_trades.emit(*trade_params)

        if offer_params is not None:
            self._trigger_offers.emit(*offer_params)

        # Also refresh the trade summary on each tick.
        self._trigger_summary.emit()

    @Slot(str)
    def _on_worker_error(self, msg: str) -> None:
        """Forward worker errors to the service-level signal.

        Parameters
        ----------
        msg : str
            Error description from the worker.
        """
        _log.warning("DatabaseService error: %s", msg)
        self.query_error.emit(msg)
