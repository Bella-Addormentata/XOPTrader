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

import math
import logging
import sqlite3
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Final, Optional

from PySide6.QtCore import (
    QMutex,
    QMutexLocker,
    QObject,
    Qt,
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

# Approximate blocks per hour on Chia (52s block target).
_BLOCKS_PER_HOUR: Final[int] = 69

# [PNL-DISPLAY 2026-08-02] Per-pair USD conversion for realized_pnl_mojos.
#
# The engine stores realized_pnl_mojos in QUOTE-asset mojos (the currency
# the seller receives), not in XCH mojos.  Conversion is per-pair using the
# quote-asset's mojos-per-unit and USD-per-unit:
#   wUSDC.b / wUSDC : 1e3 mojos/unit, $1.00/unit  -> divide by 1e3
#   BYC             : 1e3 mojos/unit, $1.00/unit  -> divide by 1e3
#   USDS            : 1e3 mojos/unit, $1.00/unit  -> divide by 1e3
#   DBX             : not USD-pegged and no on-DB rate -> 0.0
#   anything else   : 0.0 (future pairs require an explicit mapping)
#
# Shared by fetch_reports and the restart-proof P&L display queries so the
# dashboard and Reports page always agree.  Requires the trade_log table to
# be aliased as ``t``.
_PNL_USDC_EXPR: Final[str] = (
    "CASE "
    "WHEN t.pair_name LIKE '%/wUSDC%' THEN t.realized_pnl_mojos / 1000.0 "
    "WHEN t.pair_name LIKE '%/BYC%'   THEN t.realized_pnl_mojos / 1000.0 "
    "WHEN t.pair_name LIKE '%/USDS%'  THEN t.realized_pnl_mojos / 1000.0 "
    "ELSE 0.0 END"
)

# Sentinel baseline meaning "no display reset applied" (include everything).
_EPOCH_ISO: Final[str] = "1970-01-01 00:00:00"

# Mojos-per-unit denominators (engine convention: XCH = 1e12, CATs = 1e3).
_MOJOS_PER_XCH: Final[float] = 1_000_000_000_000.0
_MOJOS_PER_CAT: Final[float] = 1_000.0


def _mojos_per_unit(symbol: str) -> float:
    """Return the mojos-per-unit denominator for an asset display symbol."""
    return _MOJOS_PER_XCH if symbol.strip().upper() == "XCH" else _MOJOS_PER_CAT


# ===================================================================
# Worker -- runs queries on a background QThread
# ===================================================================

#: Fallback window for "is this pending row still our live quote", used when
#: the caller supplies no TTL-derived value.
#:
#: Module scope, not a class attribute: a bare name inside a method does NOT
#: resolve from the enclosing class namespace, so referring to it that way
#: raised NameError before either query ran and left the table empty.
_OUR_BOOK_WINDOW_H: float = 6.0

#: Mainnet target block time, for converting a block-denominated TTL to a
#: wall-clock window.
_SECONDS_PER_BLOCK: float = 18.75

#: Safety factor on the derived window.  The engine permits an offer until a
#: HARD ttl of 2x the configured soft TTL, and block time is a target rather
#: than a guarantee, so the window must sit comfortably beyond both.
_BOOK_WINDOW_SAFETY: float = 1.5


def our_book_window_hours(offer_ttl_blocks: int | None) -> float:
    """Hours a 'pending' row may age before it stops counting as our quote.

    Derived from the configured TTL rather than fixed, because a fixed window
    has a false-negative mode: Settings allows offer_ttl_blocks up to 1000,
    whose 2x hard TTL is ~10.4h, so a legitimately resting quote would age
    out of a 6h window and its side would render as absent.

    NOTE the limitation this does not solve: age cannot distinguish a ghost
    row from a live offer.  offer_log holds rows still marked 'pending' that
    were never resolved (oldest here: 2026-08-08), and only the offer
    reconciliation path can authoritatively retire those -- tracked as S11.
    This window is a display heuristic sized so it never hides a valid quote.
    """
    if not offer_ttl_blocks or offer_ttl_blocks <= 0:
        return _OUR_BOOK_WINDOW_H
    hard_ttl_blocks = offer_ttl_blocks * 2
    hours = (hard_ttl_blocks * _SECONDS_PER_BLOCK / 3600.0) * _BOOK_WINDOW_SAFETY
    return max(_OUR_BOOK_WINDOW_H, hours)


class _DatabaseWorker(QObject):
    """Background worker that executes SQLite queries and emits results.

    This object is moved to a ``QThread`` and should **never** be called
    directly from the main thread.  Use signal/slot queued connections
    to dispatch work.
    """

    # -- Result signals (one per query type) --------------------------------
    trades_ready = Signal(list)
    offers_ready = Signal(list)
    offer_summary_ready = Signal(dict)
    snapshots_ready = Signal(list)
    trade_summary_ready = Signal(dict)
    pairs_list_ready = Signal(list)
    latest_snapshot_ready = Signal(dict)
    reports_ready = Signal(dict)
    pnl_display_ready = Signal(dict)
    pnl_history_ready = Signal(list)
    deployed_ready = Signal(dict)
    pair_summary_ready = Signal(dict)
    last_trade_prices_ready = Signal(dict)
    query_error = Signal(str)

    def __init__(self, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._conn: Optional[sqlite3.Connection] = None
        self._db_path: str = ""
        #: Configured offer TTL in blocks, supplied by the caller so the
        #: live-quote window tracks the setting instead of a magic constant.
        self._book_ttl_blocks: int = 0

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
            # Log available tables for debugging.
            try:
                cursor = self._conn.execute(
                    "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"
                )
                tables = [row[0] for row in cursor.fetchall()]
                _log.info("Database tables: %s", ", ".join(tables) if tables else "(none)")
            except sqlite3.Error:
                pass
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
        # id DESC is the tie-breaker, matching the latest-row query below.
        # block_height alone leaves same-block fills in undefined order, so
        # a consumer taking the newest N (the dashboard feed does) could
        # shuffle them and drop the newest rows at the LIMIT cutoff.
        sql = (
            f"SELECT * FROM trade_log{where} "
            f"ORDER BY block_height DESC, id DESC LIMIT ?"
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

    @Slot()
    def fetch_offer_summary(self) -> None:
        """Aggregate ``offer_log`` counts and locked size for the GUI.

        The Orders panel only fetches a capped slice of one status at a
        time, so its summary bar cannot be derived from the rows it
        holds.  This runs the counting in SQL instead -- one indexed
        scan on the worker thread rather than a Python loop over every
        offer on the GUI thread.
        """
        sql = """
            SELECT
                COUNT(*)                                                          AS total,
                COALESCE(SUM(CASE WHEN status = 'pending'   THEN 1 ELSE 0 END), 0) AS pending,
                COALESCE(SUM(CASE WHEN status = 'filled'    THEN 1 ELSE 0 END), 0) AS filled,
                COALESCE(SUM(CASE WHEN status = 'cancelled' THEN 1 ELSE 0 END), 0) AS cancelled,
                COALESCE(SUM(CASE WHEN status = 'expired'   THEN 1 ELSE 0 END), 0) AS expired,
                COALESCE(SUM(CASE WHEN status = 'pending'
                                  THEN size_mojos ELSE 0 END), 0)                  AS locked_mojos
            FROM offer_log
        """
        rows = self._execute_query(sql, [])
        if not rows:
            return
        self.offer_summary_ready.emit(dict(rows[0]))

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
        if from_block > to_block:
            from_block, to_block = to_block, from_block

        block_span = max(0, to_block - from_block)
        table = self._select_snapshot_source_table(block_span)

        if table == "snapshots":
            sql = (
                "SELECT * FROM snapshots "
                "WHERE pair_name = ? AND block_height >= ? AND block_height <= ? "
                "ORDER BY block_height ASC"
            )
        else:
            sql = (
                f"SELECT "
                f"source_last_block AS block_height, "
                f"pair_name, "
                f"close_mid_price_mojos AS mid_price_mojos, "
                f"avg_spread_bps AS spread_bps, "
                f"avg_inventory_ratio AS inventory_ratio, "
                f"avg_sigma_block AS sigma_block, "
                f"close_regime AS regime, "
                f"close_pnl_total_mojos AS pnl_total_mojos, "
                f"avg_xch_usd_rate AS xch_usd_rate, "
                f"close_pnl_total_usd AS pnl_total_usd, "
                f"bucket_start_iso AS created_at "
                f"FROM {table} "
                "WHERE pair_name = ? AND source_last_block >= ? AND source_last_block <= ? "
                "ORDER BY source_last_block ASC"
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

    @Slot()
    def fetch_last_trade_prices(self) -> None:
        """Query the most recent fill price per pair from ``trade_log``.

        Used by EngineBridge as a price fallback for the wallet
        allocation panel when live metrics report no mid price.  Runs on
        the worker thread so the (measured 171-208 ms) query never
        touches the GUI thread.

        Emits ``last_trade_prices_ready`` with ``{pair_name:
        price_mojos}``.
        """
        sql = (
            "SELECT pair_name, price_mojos "
            "FROM trade_log t "
            "WHERE id = ("
            "  SELECT id FROM trade_log "
            "  WHERE pair_name = t.pair_name "
            "  ORDER BY block_height DESC, id DESC LIMIT 1"
            ")"
        )
        rows = self._execute_query(sql, [])
        if rows is None:
            return

        prices: dict[str, float] = {}
        for row in rows:
            pair_name = row["pair_name"]
            price_mojos = row["price_mojos"]
            if pair_name and price_mojos is not None:
                try:
                    prices[str(pair_name)] = float(price_mojos)
                except (TypeError, ValueError):
                    continue
        self.last_trade_prices_ready.emit(prices)

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

    @Slot(str)
    def fetch_pnl_display(self, baseline_iso: str) -> None:
        """Compute restart-proof headline P&L figures from ``trade_log``.

        [PNL-DISPLAY 2026-08-02] The dashboard's headline numbers must be
        LONG-TERM: they may never reset across GUI or engine restarts.
        The Prometheus gauges are owned by the engine process and read
        since-boot values until the engine finishes rehydrating, so the
        headline is derived here from the persistent trade log instead.

        Emits ``pnl_display_ready`` with:
            ``lifetime_usd``     -- SUM of realized P&L (USD-normalized per
                                    quote asset via _PNL_USDC_EXPR) since
                                    *baseline_iso* (all-time when empty).
            ``lifetime_trades``  -- number of realized fills in that span.
            ``pnl_24h_usd``      -- realized P&L over the trailing 24 hours.
            ``fills_24h``        -- total fills over the trailing 24 hours.
            ``baseline_iso``     -- echo of the applied display baseline
                                    ("" when showing full history).

        Parameters
        ----------
        baseline_iso : str
            Optional display-reset baseline as UTC ``YYYY-MM-DD HH:MM:SS``
            (matches the trade_log ``created_at`` format).  Empty string
            means no reset: include the full history.  This is a pure
            display filter -- no engine data is modified.
        """
        baseline = (baseline_iso or "").strip() or _EPOCH_ISO

        lifetime_sql = f"""
            SELECT
                COUNT(*)                                AS lifetime_trades,
                COALESCE(SUM({_PNL_USDC_EXPR}), 0.0)    AS lifetime_usd
            FROM trade_log t
            WHERE t.realized_pnl_mojos IS NOT NULL
              AND t.created_at >= ?
        """
        rows = self._execute_query(lifetime_sql, [baseline])
        if rows is None:
            return
        lifetime_usd = float(rows[0]["lifetime_usd"] or 0.0) if rows else 0.0
        lifetime_trades = int(rows[0]["lifetime_trades"] or 0) if rows else 0

        # Trailing 24h window; also clipped to the baseline so a fresh
        # display reset zeroes the 24h card consistently.
        day_sql = f"""
            SELECT
                COUNT(*) AS fills_24h,
                COALESCE(SUM(CASE WHEN t.realized_pnl_mojos IS NOT NULL
                                  THEN {_PNL_USDC_EXPR}
                                  ELSE 0.0 END), 0.0) AS pnl_24h_usd
            FROM trade_log t
            WHERE t.created_at >= datetime('now', '-1 day')
              AND t.created_at >= ?
        """
        rows = self._execute_query(day_sql, [baseline])
        if rows is None:
            return
        pnl_24h_usd = float(rows[0]["pnl_24h_usd"] or 0.0) if rows else 0.0
        fills_24h = int(rows[0]["fills_24h"] or 0) if rows else 0

        # [S19 2026-08-23] Net external capital via the warp bridge, so a
        # deposit is never mistaken for profit.  The engine books completed
        # bridge jobs as bridge_deposit / bridge_withdrawal ledger rows;
        # wUSDC.b is the $1.00 numeraire, so mojos / 1e3 IS the dollar
        # figure.  NOT filtered by the display baseline: capital is a
        # balance-sheet fact, not a performance figure a reset should hide.
        # The table may not exist on a fresh DB -- that is "no flows yet",
        # not a reason to drop the whole P&L payload, and (round 26) not a
        # reason to emit query_error on every refresh either: probe
        # sqlite_master first so the fallback stays silent.
        bridge_sql = """
            SELECT COALESCE(SUM(delta_mojos), 0) AS net_mojos,
                   COUNT(*)                      AS flow_count
            FROM ledger_entries
            WHERE event_type IN ('bridge_deposit', 'bridge_withdrawal')
        """
        net_deposits_usd = 0.0
        bridge_flows = 0
        rows = (self._execute_query(bridge_sql, [])
                if self._table_exists("ledger_entries") else None)
        if rows:
            net_deposits_usd = float(rows[0]["net_mojos"] or 0) / 1e3
            bridge_flows = int(rows[0]["flow_count"] or 0)

        self.pnl_display_ready.emit({
            "lifetime_usd": lifetime_usd,
            "lifetime_trades": lifetime_trades,
            "pnl_24h_usd": pnl_24h_usd,
            "fills_24h": fills_24h,
            "net_deposits_usd": net_deposits_usd,
            "bridge_flows": bridge_flows,
            "baseline_iso": "" if baseline == _EPOCH_ISO else baseline,
        })

    @Slot(int)
    def fetch_pnl_history(self, days: int) -> None:
        """Rebuild the global P&L (USD) time series from persisted data.

        [PNL-DISPLAY 2026-08-02] The chart's P&L curve used to live only in
        GUI memory, so every GUI restart started it empty.  This query
        reconstructs the curve from the database instead of a sidecar file:

        - The *total* (mark-to-market) series comes from the per-pair
          ``snapshots.pnl_total_usd`` column, summed across pairs at each
          snapshot block (matching the engine's global USD total gauge).
        - The *realized* series is the cumulative sum of trade_log realized
          P&L (USD-normalized via _PNL_USDC_EXPR) evaluated at each
          snapshot timestamp.

        Emits ``pnl_history_ready`` with a chronologically ordered list of
        ``{"block": int, "ts": float, "total_usd": float,
        "realized_usd": float}`` dicts.

        Parameters
        ----------
        days : int
            Retention window in days (clamped to 1..365).
        """
        safe_days = max(1, min(int(days), 365))
        cutoff = f"-{safe_days} day"

        snap_sql = """
            SELECT block_height, created_at,
                   COALESCE(SUM(pnl_total_usd), 0.0) AS total_usd
            FROM snapshots
            WHERE created_at >= datetime('now', ?)
            GROUP BY block_height, created_at
            ORDER BY created_at ASC, block_height ASC
        """
        snap_rows = self._execute_query(snap_sql, [cutoff])
        if snap_rows is None:
            return

        realized_sql = f"""
            SELECT t.created_at AS created_at,
                   COALESCE(SUM({_PNL_USDC_EXPR}), 0.0) AS usd
            FROM trade_log t
            WHERE t.realized_pnl_mojos IS NOT NULL
            GROUP BY t.created_at
            ORDER BY t.created_at ASC
        """
        realized_rows = self._execute_query(realized_sql, [])
        if realized_rows is None:
            realized_rows = []

        def _to_unix(created_at: str) -> float:
            """Parse a UTC 'YYYY-MM-DD HH:MM:SS' string to Unix seconds."""
            try:
                dt = datetime.strptime(created_at, "%Y-%m-%d %H:%M:%S")
                return dt.replace(tzinfo=timezone.utc).timestamp()
            except (ValueError, TypeError):
                return 0.0

        points: list[dict[str, Any]] = []
        realized_cum = 0.0
        r_idx = 0
        n_realized = len(realized_rows)
        for row in snap_rows:
            created = str(row["created_at"] or "")
            if not created:
                continue
            # Advance cumulative realized P&L up to this snapshot time.
            # Both columns share the 'YYYY-MM-DD HH:MM:SS' format, so
            # lexicographic comparison equals chronological comparison.
            while (r_idx < n_realized
                   and str(realized_rows[r_idx]["created_at"] or "") <= created):
                realized_cum += float(realized_rows[r_idx]["usd"] or 0.0)
                r_idx += 1
            ts = _to_unix(created)
            if ts <= 0.0:
                continue
            points.append({
                "block": int(row["block_height"] or 0),
                "ts": ts,
                "total_usd": float(row["total_usd"] or 0.0),
                "realized_usd": realized_cum,
            })

        _log.info("PnL history rebuilt from DB: %d points (%d days).",
                  len(points), safe_days)
        self.pnl_history_ready.emit(points)

    @Slot()
    def fetch_deployed_capital(self) -> None:
        """Aggregate resting (pending) offers into per-asset offered amounts.

        [DEPLOYED 2026-08-04] Feeds the Balances tab's "Deployed %" column.
        For each ``offer_log`` row with status='pending' on pair BASE/QUOTE:

        - an ASK locks the BASE asset: ``size_mojos`` base-asset mojos;
        - a BID locks the QUOTE asset: the engine pseudo-price is
          ``quote_units_per_base_unit * 1e12`` and sizes are base-asset
          mojos, so (per ``xop::quote_mojos_for`` in cpp types.hpp)
          ``quote_units = size_mojos * price_mojos
          / (base_mojos_per_unit * 1e12)``.

        Amounts are converted to whole asset units (XCH = 1e12 mojos/unit,
        CATs = 1e3) and summed per display symbol.  NOTE: these are OFFER
        sizes; on-chain offers lock whole coins, so the wallet's true
        locked value (confirmed - spendable) can exceed these figures.

        Emits ``deployed_ready`` with::

            {"offered_units": {SYMBOL: units, ...},
             "offer_counts":  {SYMBOL: count, ...},
             "pending_offers": total_pending_count}

        Symbols are upper-cased pair-name components (e.g. "XCH",
        "WUSDC.B", "BYC", "DBX").
        """
        # CAST to REAL before multiplying: size*price reaches ~1.5e24 for
        # XCH pairs, which overflows SQLite's 64-bit integers.
        sql = """
            SELECT pair_name, side,
                   COALESCE(SUM(size_mojos), 0)                    AS size_sum,
                   COALESCE(SUM(CAST(size_mojos AS REAL)
                                * CAST(price_mojos AS REAL)), 0.0) AS size_price_sum,
                   COUNT(*)                                        AS offer_count
            FROM offer_log
            WHERE status = 'pending'
            GROUP BY pair_name, side
        """
        rows = self._execute_query(sql, [])
        if rows is None:
            return

        offered_units: dict[str, float] = {}
        offer_counts: dict[str, int] = {}
        pending_total = 0
        for row in rows:
            pair = str(row["pair_name"] or "")
            if "/" not in pair:
                continue
            base_sym, quote_sym = (part.strip() for part in pair.split("/", 1))
            side = str(row["side"] or "").strip().lower()
            count = int(row["offer_count"] or 0)
            base_denom = _mojos_per_unit(base_sym)
            if side == "ask":
                symbol = base_sym.upper()
                units = float(row["size_sum"] or 0) / base_denom
            elif side == "bid":
                symbol = quote_sym.upper()
                units = (float(row["size_price_sum"] or 0.0)
                         / (base_denom * _MOJOS_PER_XCH))
            else:
                continue
            pending_total += count
            offered_units[symbol] = offered_units.get(symbol, 0.0) + units
            offer_counts[symbol] = offer_counts.get(symbol, 0) + count

        self.deployed_ready.emit({
            "offered_units": offered_units,
            "offer_counts": offer_counts,
            "pending_offers": pending_total,
        })

    @Slot(int)
    def fetch_pair_summary_for(self, offer_ttl_blocks: int) -> None:
        """Set the TTL and run the summary, as ONE queued operation.

        Two connections -- a setter plus a lambda -- were wrong twice over.
        A lambda has no receiver object, so Qt runs it in the EMITTING
        thread: both SQLite aggregates executed on the UI thread every
        refresh instead of on the worker. And splitting the state update
        from the query left them separately queued, so the fetch could run
        before the connection was open or with the previous TTL.
        """
        self._book_ttl_blocks = int(offer_ttl_blocks or 0)
        self.fetch_pair_summary()

    @Slot()
    def fetch_pair_summary(self) -> None:
        """Per-pair view of OUR OWN book and fills, for the dashboard table.

        Deliberately not the third-party market: ``bid``/``ask`` are the best
        prices WE are currently resting, taken from ``offer_log`` rows still
        pending.  Best bid is the HIGHEST price we will pay; best ask the
        LOWEST we will accept.

        Emits ``pair_summary_ready`` with
        ``{pair: {"bid_mojos", "ask_mojos", "fills_24h", "pnl_mojos"}}``.
        Prices stay in mojos here; the display layer converts, so the scaling
        rule lives in one place.
        """
        summary: dict[str, dict[str, float]] = {}

        # RECENCY WINDOW.  offer_log holds rows still marked 'pending' that
        # were never resolved -- measured 2026-08-21: an XCH/BYC "ask" from
        # 2026-08-13 and a bid from 2026-08-08, eight days stale at block
        # 9144382 against a chain at 9182224.  Taking the best price over all
        # pending rows therefore reported a SELF-CROSSED book (bid 1.4857
        # above ask 1.4534), which is alarming and false.  Only offers posted
        # recently describe what we are actually showing the market.
        #
        # created_at here is space-separated ("2026-08-21 20:46:24"), NOT the
        # ISO form trade_log uses; the cutoff below must match this table's
        # format or the text comparison silently misbehaves.
        window_h = our_book_window_hours(self._book_ttl_blocks)
        book_cutoff = (
            datetime.now(timezone.utc) - timedelta(hours=window_h)
        ).strftime("%Y-%m-%d %H:%M:%S")
        book = self._execute_query(
            """
            SELECT pair_name, side,
                   MAX(price_mojos) AS best_bid,
                   MIN(price_mojos) AS best_ask
            FROM offer_log
            WHERE status = 'pending' AND created_at >= ?
            GROUP BY pair_name, side
            """,
            [book_cutoff],
        )
        if book is None:
            # Query error (already logged by _execute_query).  Emitting an
            # empty summary would render every pair as "0 fills, +0.0000"
            # in profit green -- a confident claim of a quiet book, when the
            # truth is "the database could not be read".  Keep the previous
            # data on screen instead.
            return
        for row in book:
            pair = str(row["pair_name"] or "")
            if not pair:
                continue
            entry = summary.setdefault(pair, {})
            side = str(row["side"] or "").strip().lower()
            if side == "bid":
                entry["bid_mojos"] = float(row["best_bid"] or 0.0)
            elif side == "ask":
                entry["ask_mojos"] = float(row["best_ask"] or 0.0)

        # 24h window. The bound MUST be ISO with a "T": timestamps are stored
        # as "2026-08-21T16:54:15.943Z" and compared as text, so SQLite's own
        # datetime('now','-1 day') -- which yields a SPACE separator -- sorts
        # below every stamp on the boundary day and silently widens the
        # window (measured: 49 rows returned where 35 were in range).
        cutoff = (
            datetime.now(timezone.utc) - timedelta(days=1)
        ).strftime("%Y-%m-%dT%H:%M:%S")
        fills = self._execute_query(
            """
            SELECT pair_name,
                   COUNT(*)                                AS fills,
                   COALESCE(SUM(realized_pnl_mojos), 0)    AS pnl_mojos
            FROM trade_log
            WHERE timestamp >= ?
            GROUP BY pair_name
            """,
            [cutoff],
        )
        if fills is None:
            return          # same reasoning as the book query above
        for row in fills:
            pair = str(row["pair_name"] or "")
            if not pair:
                continue
            entry = summary.setdefault(pair, {})
            entry["fills_24h"] = float(row["fills"] or 0)
            entry["pnl_mojos"] = float(row["pnl_mojos"] or 0.0)

        self.pair_summary_ready.emit(summary)

    @Slot()
    def fetch_reports(self) -> None:
        """Compute comprehensive P&L and trade analytics for the reports page.

        Emits ``reports_ready`` with a dict containing time-period P&L,
        per-pair breakdown, capital gains estimates, and offer statistics.
        """
        result: dict[str, Any] = {
            "periods": {},
            "per_pair": [],
            "capital_gains": {},
            "offer_stats": {},
            "top_trades": [],
            "worst_trades": [],
            "daily_pnl": [],
            "forecast": {},
        }

        snapshot_fx_available = self._snapshot_has_xch_usd_rate()
        fallback_xch_usd = self._get_latest_snapshot_xch_usd_rate() if snapshot_fx_available else 0.0
        if snapshot_fx_available:
            usd_rate_cte = """
                WITH daily_rates AS (
                    SELECT
                        DATE(created_at) AS rate_date,
                        AVG(CASE WHEN xch_usd_rate > 0 THEN xch_usd_rate END) AS xch_usd_rate
                    FROM snapshots
                    GROUP BY DATE(created_at)
                ),
                fallback_rate AS (
                    SELECT ? AS xch_usd_rate
                )
            """
        else:
            usd_rate_cte = """
                WITH daily_rates AS (
                    SELECT NULL AS rate_date, NULL AS xch_usd_rate
                    WHERE 0
                ),
                fallback_rate AS (
                    SELECT NULL AS xch_usd_rate
                )
            """
        usd_rate_expr = "COALESCE(dr.xch_usd_rate, fr.xch_usd_rate, 0.0)"

        # [PNL-UNIT-FIX] Per-pair USDC conversion for realized_pnl_mojos.
        #
        # The engine stores realized_pnl_mojos in QUOTE-asset mojos (the
        # currency the seller receives), not in XCH mojos.  The previous
        # SQL treated the value as XCH mojos and multiplied by xch_usd_rate,
        # which produced numbers ~1e9x too large for CAT-quoted pairs
        # ("billions of dollars" in the GUI).
        #
        # The conversion table lives in the module-level _PNL_USDC_EXPR so
        # the dashboard's restart-proof P&L display uses the exact same
        # mapping (see fetch_pnl_display / fetch_pnl_history).
        #
        # Fees are paid on-chain in XCH mojos, so the fee_usdc_expr below
        # keeps the legacy XCH/USD conversion.
        pnl_usdc_expr = _PNL_USDC_EXPR
        # [REPORTS-CRASH-FIX 2026-07-30] This line previously read
        # `fee_usdc_expr = (f"({fee_usdc_expr})")` -- a self-reference before
        # assignment introduced in the v0.7.46 refactor.  Every fetch_reports
        # call since 2026-04-21 died here with UnboundLocalError before
        # running any SQL, so the Reports page silently showed placeholders
        # for three months.  Fees are on-chain XCH mojos; convert via the
        # daily XCH/USD rate (0 when no rate row exists for that day).
        fee_usdc_expr = (
            f"((t.fee_mojos / 1000000000000.0) * {usd_rate_expr})"
        )

        # -- Period-based P&L -------------------------------------------------
        period_sql = usd_rate_cte + f"""
            SELECT
                COUNT(*)                                                    AS trade_count,
                COALESCE(SUM(realized_pnl_mojos), 0)                       AS total_pnl,
                COALESCE(SUM(CASE WHEN realized_pnl_mojos > 0 THEN 1 ELSE 0 END), 0) AS wins,
                COALESCE(SUM(CASE WHEN realized_pnl_mojos < 0 THEN 1 ELSE 0 END), 0) AS losses,
                COALESCE(SUM(size_mojos), 0)                               AS total_volume,
                COALESCE(SUM(fee_mojos), 0)                                AS total_fees,
                COALESCE(AVG(realized_pnl_mojos), 0)                       AS avg_pnl,
                COALESCE(MAX(realized_pnl_mojos), 0)                       AS best_trade,
                COALESCE(MIN(realized_pnl_mojos), 0)                       AS worst_trade,
                COALESCE(SUM({pnl_usdc_expr}), 0.0)
                    AS total_pnl_usdc,
                COALESCE(SUM({fee_usdc_expr}), 0.0)
                    AS total_fees_usdc,
                COALESCE(AVG({pnl_usdc_expr}), 0.0)
                    AS avg_pnl_usdc,
                COALESCE(MAX({pnl_usdc_expr}), 0.0)
                    AS best_trade_usdc,
                COALESCE(MIN({pnl_usdc_expr}), 0.0)
                    AS worst_trade_usdc
            FROM trade_log t
            LEFT JOIN daily_rates dr ON dr.rate_date = DATE(t.timestamp)
            CROSS JOIN fallback_rate fr
            WHERE t.timestamp >= ?
        """

        now = datetime.utcnow()
        today_start = now.replace(hour=0, minute=0, second=0, microsecond=0)

        periods = {
            "today": today_start.isoformat(),
            "7d": (now - timedelta(days=7)).isoformat(),
            "30d": (now - timedelta(days=30)).isoformat(),
            "90d": (now - timedelta(days=90)).isoformat(),
            "ytd": now.replace(month=1, day=1, hour=0, minute=0, second=0, microsecond=0).isoformat(),
            "1y": (now - timedelta(days=365)).isoformat(),
            "all": "2000-01-01T00:00:00",
        }

        for label, since_ts in periods.items():
            params = [since_ts]
            if snapshot_fx_available:
                params.insert(0, fallback_xch_usd)

            rows = self._execute_query(period_sql, params)
            if rows and rows[0]:
                r = rows[0]
                result["periods"][label] = {
                    "trade_count": r["trade_count"],
                    "total_pnl": r["total_pnl"],
                    "wins": r["wins"],
                    "losses": r["losses"],
                    "total_volume": r["total_volume"],
                    "total_fees": r["total_fees"],
                    "avg_pnl": r["avg_pnl"],
                    "best_trade": r["best_trade"],
                    "worst_trade": r["worst_trade"],
                    "total_pnl_usdc": r["total_pnl_usdc"] if snapshot_fx_available else None,
                    "total_fees_usdc": r["total_fees_usdc"] if snapshot_fx_available else None,
                    "avg_pnl_usdc": r["avg_pnl_usdc"] if snapshot_fx_available else None,
                    "best_trade_usdc": r["best_trade_usdc"] if snapshot_fx_available else None,
                    "worst_trade_usdc": r["worst_trade_usdc"] if snapshot_fx_available else None,
                }

        # -- Per-pair breakdown -----------------------------------------------
        pair_sql = usd_rate_cte + f"""
            SELECT
                t.pair_name,
                COUNT(*)                                                    AS trade_count,
                COALESCE(SUM(realized_pnl_mojos), 0)                       AS total_pnl,
                COALESCE(SUM(CASE WHEN realized_pnl_mojos > 0 THEN 1 ELSE 0 END), 0) AS wins,
                COALESCE(SUM(CASE WHEN realized_pnl_mojos < 0 THEN 1 ELSE 0 END), 0) AS losses,
                COALESCE(SUM(size_mojos), 0)                               AS total_volume,
                COALESCE(SUM(fee_mojos), 0)                                AS total_fees,
                COALESCE(AVG(cost_basis_mojos), 0)                         AS avg_cost_basis,
                COALESCE(SUM({pnl_usdc_expr}), 0.0)
                    AS total_pnl_usdc,
                COALESCE(SUM({fee_usdc_expr}), 0.0)
                    AS total_fees_usdc
            FROM trade_log t
            LEFT JOIN daily_rates dr ON dr.rate_date = DATE(t.timestamp)
            CROSS JOIN fallback_rate fr
            GROUP BY t.pair_name
            ORDER BY total_pnl DESC
        """
        rows = self._execute_query(pair_sql, [fallback_xch_usd] if snapshot_fx_available else [])
        if rows:
            pair_rows = [dict(r) for r in rows]
            if not snapshot_fx_available:
                for row in pair_rows:
                    row["total_pnl_usdc"] = None
                    row["total_fees_usdc"] = None
            result["per_pair"] = pair_rows

        # -- Capital gains (short-term vs long-term) --------------------------
        # Short-term: held < 365 days.  Long-term: held >= 365 days.
        # Since trades are fills (not lot-matching), we estimate using
        # timestamp-based age of realized PnL.
        one_year_ago = (now - timedelta(days=365)).isoformat()
        current_year_start = now.replace(
            month=1, day=1, hour=0, minute=0, second=0, microsecond=0
        ).isoformat()

        cg_sql = usd_rate_cte + f"""
            SELECT
                COALESCE(SUM(CASE WHEN t.timestamp >= ? THEN realized_pnl_mojos ELSE 0 END), 0)
                    AS short_term_pnl,
                COALESCE(SUM(CASE WHEN t.timestamp <  ? THEN realized_pnl_mojos ELSE 0 END), 0)
                    AS long_term_pnl,
                COALESCE(SUM(realized_pnl_mojos), 0) AS total_pnl,
                COALESCE(SUM(fee_mojos), 0)          AS total_fees,
                COALESCE(SUM(CASE WHEN t.timestamp >= ?
                                  THEN {pnl_usdc_expr}
                                  ELSE 0 END), 0.0) AS short_term_pnl_usdc,
                COALESCE(SUM(CASE WHEN t.timestamp <  ?
                                  THEN {pnl_usdc_expr}
                                  ELSE 0 END), 0.0) AS long_term_pnl_usdc,
                COALESCE(SUM({pnl_usdc_expr}), 0.0)
                    AS total_pnl_usdc,
                COALESCE(SUM({fee_usdc_expr}), 0.0)
                    AS total_fees_usdc
            FROM trade_log t
            LEFT JOIN daily_rates dr ON dr.rate_date = DATE(t.timestamp)
            CROSS JOIN fallback_rate fr
            WHERE t.timestamp >= ?
        """
        cg_params: list[Any] = [one_year_ago, one_year_ago, one_year_ago, one_year_ago, current_year_start]
        if snapshot_fx_available:
            cg_params.insert(0, fallback_xch_usd)

        rows = self._execute_query(cg_sql, cg_params)
        if rows and rows[0]:
            r = rows[0]
            result["capital_gains"] = {
                "short_term": r["short_term_pnl"],
                "long_term": r["long_term_pnl"],
                "total": r["total_pnl"],
                "fees_deductible": r["total_fees"],
                "short_term_usdc": r["short_term_pnl_usdc"] if snapshot_fx_available else None,
                "long_term_usdc": r["long_term_pnl_usdc"] if snapshot_fx_available else None,
                "total_usdc": r["total_pnl_usdc"] if snapshot_fx_available else None,
                "fees_deductible_usdc": r["total_fees_usdc"] if snapshot_fx_available else None,
                "year": now.year,
            }

        # -- Offer statistics -------------------------------------------------
        offer_sql = """
            SELECT
                COUNT(*)                                                         AS total_offers,
                COALESCE(SUM(CASE WHEN status = 'filled'    THEN 1 ELSE 0 END), 0) AS filled,
                COALESCE(SUM(CASE WHEN status = 'cancelled' THEN 1 ELSE 0 END), 0) AS cancelled,
                COALESCE(SUM(CASE WHEN status = 'expired'   THEN 1 ELSE 0 END), 0) AS expired,
                COALESCE(SUM(CASE WHEN status = 'pending'   THEN 1 ELSE 0 END), 0) AS pending
            FROM offer_log
        """
        rows = self._execute_query(offer_sql, [])
        if rows and rows[0]:
            r = rows[0]
            total = r["total_offers"] or 1
            result["offer_stats"] = {
                "total": r["total_offers"],
                "filled": r["filled"],
                "cancelled": r["cancelled"],
                "expired": r["expired"],
                "pending": r["pending"],
                "fill_rate": r["filled"] / total * 100.0,
            }

        # -- Top 5 best and worst trades --------------------------------------
        best_sql = usd_rate_cte + f"""
            SELECT t.timestamp, t.pair_name, t.side, t.price_mojos, t.size_mojos,
                   t.realized_pnl_mojos, t.cost_basis_mojos,
                   ({pnl_usdc_expr}) AS realized_pnl_usdc
            FROM trade_log t
            LEFT JOIN daily_rates dr ON dr.rate_date = DATE(t.timestamp)
            CROSS JOIN fallback_rate fr
            WHERE t.realized_pnl_mojos > 0
            ORDER BY t.realized_pnl_mojos DESC LIMIT 5
        """
        rows = self._execute_query(best_sql, [fallback_xch_usd] if snapshot_fx_available else [])
        if rows:
            top_rows = [dict(r) for r in rows]
            if not snapshot_fx_available:
                for row in top_rows:
                    row["realized_pnl_usdc"] = None
            result["top_trades"] = top_rows

        worst_sql = usd_rate_cte + f"""
            SELECT t.timestamp, t.pair_name, t.side, t.price_mojos, t.size_mojos,
                   t.realized_pnl_mojos, t.cost_basis_mojos,
                   ({pnl_usdc_expr}) AS realized_pnl_usdc
            FROM trade_log t
            LEFT JOIN daily_rates dr ON dr.rate_date = DATE(t.timestamp)
            CROSS JOIN fallback_rate fr
            WHERE t.realized_pnl_mojos < 0
            ORDER BY t.realized_pnl_mojos ASC LIMIT 5
        """
        rows = self._execute_query(worst_sql, [fallback_xch_usd] if snapshot_fx_available else [])
        if rows:
            worst_rows = [dict(r) for r in rows]
            if not snapshot_fx_available:
                for row in worst_rows:
                    row["realized_pnl_usdc"] = None
            result["worst_trades"] = worst_rows

        # -- Daily P&L (last 30 days) -----------------------------------------
        daily_sql = usd_rate_cte + f"""
            SELECT
                DATE(t.timestamp) AS trade_date,
                COUNT(*)        AS trade_count,
                COALESCE(SUM(t.realized_pnl_mojos), 0) AS daily_pnl,
                COALESCE(SUM(t.fee_mojos), 0)          AS daily_fees,
                AVG({usd_rate_expr})                  AS xch_usd_rate,
                COALESCE(SUM({pnl_usdc_expr}), 0.0)
                    AS daily_pnl_usdc,
                COALESCE(SUM({fee_usdc_expr}), 0.0)
                    AS daily_fees_usdc,
                COALESCE(SUM(({pnl_usdc_expr}) - ({fee_usdc_expr})), 0.0)
                    AS daily_net_usdc
            FROM trade_log t
            LEFT JOIN daily_rates dr ON dr.rate_date = DATE(t.timestamp)
            CROSS JOIN fallback_rate fr
            WHERE t.timestamp >= ?
            GROUP BY DATE(t.timestamp)
            ORDER BY trade_date ASC
        """
        thirty_days_ago = (now - timedelta(days=30)).isoformat()
        daily_params: list[Any] = [thirty_days_ago]
        if snapshot_fx_available:
            daily_params.insert(0, fallback_xch_usd)

        rows = self._execute_query(daily_sql, daily_params)
        if rows:
            daily_pnl_rows = [dict(r) for r in rows]
            if not snapshot_fx_available:
                for row in daily_pnl_rows:
                    row["xch_usd_rate"] = None
                    row["daily_pnl_usdc"] = None
                    row["daily_fees_usdc"] = None
                    row["daily_net_usdc"] = None
            result["daily_pnl"] = daily_pnl_rows

        # -- Forecast inputs (trailing calendar-day net P&L) ------------------
        first_trade_sql = "SELECT MIN(DATE(timestamp)) AS first_trade_date FROM trade_log"
        rows = self._execute_query(first_trade_sql, [])
        first_trade_date: Optional[date] = None
        if rows and rows[0] and rows[0]["first_trade_date"]:
            try:
                first_trade_date = date.fromisoformat(rows[0]["first_trade_date"])
            except ValueError:
                first_trade_date = None

        if first_trade_date is not None:
            forecast_start = max(first_trade_date, now.date() - timedelta(days=89))
            forecast_params: list[Any] = [forecast_start.isoformat()]
            if snapshot_fx_available:
                forecast_params.insert(0, fallback_xch_usd)

            rows = self._execute_query(daily_sql, forecast_params)
            if rows:
                result["forecast"] = self._build_income_forecast(
                    rows,
                    now_date=now.date(),
                    first_trade_date=first_trade_date,
                    use_usdc=snapshot_fx_available,
                )

        self.reports_ready.emit(result)

    def _build_income_forecast(
        self,
        daily_rows: list[sqlite3.Row],
        *,
        now_date: date,
        first_trade_date: date,
        use_usdc: bool,
    ) -> dict[str, Any]:
        """Estimate forward net income from trailing calendar-day P&L.

        The model intentionally stays conservative and transparent:
        it uses daily net realised P&L (realised P&L minus fees), fills
        missing calendar days with zero, and extrapolates the mean with a
        normal approximation for uncertainty bands.
        """
        lookback_start = max(first_trade_date, now_date - timedelta(days=89))

        history_by_day: dict[date, dict[str, float | int]] = {}
        for row in daily_rows:
            trade_date_raw = row["trade_date"]
            if not trade_date_raw:
                continue
            try:
                trade_day = date.fromisoformat(str(trade_date_raw))
            except ValueError:
                continue

            gross_pnl = int(row["daily_pnl"] or 0)
            fees = int(row["daily_fees"] or 0)
            net_usdc = row["daily_net_usdc"]
            if net_usdc is None:
                net_usdc = 0.0
            history_by_day[trade_day] = {
                "net_usdc": float(net_usdc),
                "trades": int(row["trade_count"] or 0),
                "net_mojos": gross_pnl - fees,
            }

        if not history_by_day:
            return {}

        daily_values: list[float] = []
        active_days = 0
        profitable_days = 0
        total_trades = 0

        cursor = lookback_start
        while cursor <= now_date:
            entry = history_by_day.get(
                cursor,
                {"net_usdc": 0.0, "trades": 0, "net_mojos": 0},
            )
            if use_usdc:
                net_value = float(entry["net_usdc"])
            else:
                net_value = float(entry["net_mojos"])
            trade_count = int(entry["trades"])

            daily_values.append(net_value)
            total_trades += trade_count
            if trade_count > 0 or net_value != 0.0:
                active_days += 1
            if net_value > 0.0:
                profitable_days += 1

            cursor += timedelta(days=1)

        sample_days = len(daily_values)
        if sample_days == 0:
            return {}

        mean_daily = sum(daily_values) / float(sample_days)
        if sample_days > 1:
            variance = sum(
                (value - mean_daily) ** 2 for value in daily_values
            ) / float(sample_days - 1)
            stdev_daily = math.sqrt(max(variance, 0.0))
        else:
            stdev_daily = 0.0

        def horizon_stats(days: int) -> dict[str, float]:
            expected = mean_daily * float(days)
            sigma = stdev_daily * math.sqrt(float(days))
            lower = expected - 1.96 * sigma
            upper = expected + 1.96 * sigma

            if sigma > 0.0:
                z_score = expected / sigma
                prob_positive = 0.5 * (1.0 + math.erf(z_score / math.sqrt(2.0)))
            elif expected > 0.0:
                prob_positive = 1.0
            elif expected < 0.0:
                prob_positive = 0.0
            else:
                prob_positive = 0.5

            horizon = {"prob_positive_pct": prob_positive * 100.0}
            if use_usdc:
                horizon.update({
                    "expected_usdc": expected,
                    "low_95_usdc": lower,
                    "high_95_usdc": upper,
                })
            else:
                horizon.update({
                    "expected": expected,
                    "low_95": lower,
                    "high_95": upper,
                })
            return horizon

        if sample_days >= 60 and active_days >= 20:
            confidence = "medium"
        elif sample_days >= 30 and active_days >= 10:
            confidence = "low"
        else:
            confidence = "very low"

        result = {
            "method": "calendar_day_net_realized_pnl_minus_fees",
            "unit": "USDC" if use_usdc else "XCH-mojos",
            "lookback_days": sample_days,
            "active_days": active_days,
            "total_trades": total_trades,
            "profit_day_rate_pct": (
                profitable_days / float(sample_days) * 100.0
            ),
            "confidence": confidence,
            "next_1d": horizon_stats(1),
            "next_7d": horizon_stats(7),
            "next_30d": horizon_stats(30),
        }
        if use_usdc:
            result["mean_daily_net_usdc"] = mean_daily
            result["daily_net_volatility_usdc"] = stdev_daily
        else:
            result["mean_daily_net_pnl"] = mean_daily
            result["daily_net_volatility"] = stdev_daily
        return result

    def _get_latest_snapshot_xch_usd_rate(self) -> float:
        """Return the latest non-zero XCH/USD mark persisted in snapshots."""
        if not self._snapshot_has_xch_usd_rate():
            return 0.0
        rows = self._execute_query(
            """
            SELECT xch_usd_rate
            FROM snapshots
            WHERE xch_usd_rate > 0
            ORDER BY block_height DESC
            LIMIT 1
            """,
            [],
        )
        if rows and rows[0]:
            return float(rows[0]["xch_usd_rate"] or 0.0)
        return 0.0

    def _snapshot_has_xch_usd_rate(self) -> bool:
        """Return ``True`` when the snapshots table has the FX-rate column."""
        rows = self._execute_query("PRAGMA table_info(snapshots)", [])
        if not rows:
            return False
        return any(str(row["name"]) == "xch_usd_rate" for row in rows)

    def _select_snapshot_source_table(self, block_span: int) -> str:
        """Pick raw snapshots or a rollup table based on requested span.

        The returned table always preserves the public snapshots payload
        shape via column aliases in :meth:`fetch_snapshots`.
        """
        one_day_blocks = 24 * _BLOCKS_PER_HOUR
        seven_day_blocks = 7 * one_day_blocks
        thirty_day_blocks = 30 * one_day_blocks

        if block_span >= thirty_day_blocks and self._table_exists("snapshots_1h"):
            return "snapshots_1h"
        if block_span >= seven_day_blocks and self._table_exists("snapshots_15m"):
            return "snapshots_15m"
        if block_span >= one_day_blocks and self._table_exists("snapshots_1m"):
            return "snapshots_1m"
        return "snapshots"

    def _table_exists(self, table_name: str) -> bool:
        """Return ``True`` if a table exists in this database."""
        rows = self._execute_query(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
            [table_name],
        )
        return bool(rows)

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
        result = [dict(row) for row in rows]
        _log.debug("Query returned %d rows: %s", len(result), sql[:80])
        signal.emit(result)


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
    offer_summary_loaded = Signal(dict)
    snapshots_loaded = Signal(list)
    trade_summary_loaded = Signal(dict)
    pairs_list_loaded = Signal(list)
    latest_snapshot_loaded = Signal(dict)
    reports_loaded = Signal(dict)
    pnl_display_loaded = Signal(dict)
    pnl_history_loaded = Signal(list)
    deployed_loaded = Signal(dict)
    pair_summary_loaded = Signal(dict)
    last_trade_prices_loaded = Signal(dict)
    query_error = Signal(str)

    # -- Internal trigger signals (queued connections to worker thread) ------
    _trigger_open = Signal(str)
    _trigger_close = Signal()
    _trigger_trades = Signal(str, str, object, object, int)
    _trigger_offers = Signal(str, str, int)
    _trigger_offer_summary = Signal()
    _trigger_snapshots = Signal(str, int, int)
    _trigger_summary = Signal()
    _trigger_pairs = Signal()
    _trigger_latest_snapshot = Signal(str)
    _trigger_reports = Signal()
    _trigger_pnl_display = Signal(str)
    _trigger_pnl_history = Signal(int)
    _trigger_deployed = Signal()
    _trigger_pair_summary = Signal(int)
    _trigger_last_trade_prices = Signal()

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
        self._worker.offer_summary_ready.connect(self.offer_summary_loaded)
        self._worker.snapshots_ready.connect(self.snapshots_loaded)
        self._worker.trade_summary_ready.connect(self.trade_summary_loaded)
        self._worker.pairs_list_ready.connect(self.pairs_list_loaded)
        self._worker.latest_snapshot_ready.connect(self.latest_snapshot_loaded)
        self._worker.reports_ready.connect(self.reports_loaded)
        self._worker.pnl_display_ready.connect(self.pnl_display_loaded)
        self._worker.pnl_history_ready.connect(self.pnl_history_loaded)
        self._worker.deployed_ready.connect(self.deployed_loaded)
        self._worker.pair_summary_ready.connect(self.pair_summary_loaded)
        self._worker.last_trade_prices_ready.connect(self.last_trade_prices_loaded)
        self._worker.query_error.connect(self._on_worker_error)

        # Queued connections: emit trigger signals to dispatch work to
        # the worker thread rather than calling worker methods directly
        # (which would execute SQLite I/O on the GUI thread).
        self._trigger_open.connect(self._worker.open)
        self._trigger_close.connect(self._worker.close)
        self._trigger_trades.connect(self._worker.fetch_trades)
        self._trigger_offers.connect(self._worker.fetch_offers)
        self._trigger_offer_summary.connect(self._worker.fetch_offer_summary)
        self._trigger_snapshots.connect(self._worker.fetch_snapshots)
        self._trigger_summary.connect(self._worker.fetch_trade_summary)
        self._trigger_pairs.connect(self._worker.fetch_pairs_list)
        self._trigger_latest_snapshot.connect(self._worker.fetch_latest_snapshot)
        self._trigger_reports.connect(self._worker.fetch_reports)
        self._trigger_pnl_display.connect(self._worker.fetch_pnl_display)
        self._trigger_pnl_history.connect(self._worker.fetch_pnl_history)
        self._trigger_deployed.connect(self._worker.fetch_deployed_capital)
        # Bound worker slot, like every other trigger above: that is what
        # makes Qt queue it onto the worker thread.
        self._trigger_pair_summary.connect(self._worker.fetch_pair_summary_for)
        self._trigger_last_trade_prices.connect(self._worker.fetch_last_trade_prices)

        # -- Auto-refresh timer ---------------------------------------------
        self._refresh_timer: QTimer = QTimer(self)
        self._refresh_timer.setTimerType(Qt.TimerType.CoarseTimer)
        self._refresh_timer.timeout.connect(self._on_auto_refresh)

        # Track the last auto-refresh query parameters so the timer
        # can re-issue the most recently requested data set.
        self._mutex: QMutex = QMutex()
        self._last_trade_params: Optional[tuple[str, str, Optional[int], Optional[int], int]] = None
        self._last_offer_params: Optional[tuple[str, str, int]] = None
        # Last requested P&L display baseline (None until first query;
        # "" means "no reset / full history").  Auto-refresh re-issues it.
        self._last_pnl_baseline: Optional[str] = None
        # Whether deployed-capital data was ever requested; auto-refresh
        # keeps it current once the Balances tab has asked for it.
        self._deployed_requested: bool = False
        self._pair_summary_requested: bool = False
        self._book_ttl_blocks: int = 0
        # Same for the Orders panel's whole-table offer aggregates.
        self._offer_summary_requested: bool = False

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
        """Close the database and shut down the worker thread.

        Safe to call even if the service was never started -- the thread
        quit/wait is skipped when the thread is not running.
        """
        _log.info("Stopping DatabaseService.")
        self._refresh_timer.stop()
        if self._thread.isRunning():
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

    def query_offer_summary(self) -> None:
        """Request whole-table ``offer_log`` aggregates.

        Results arrive on :pyattr:`offer_summary_loaded` as
        ``{"total", "pending", "filled", "cancelled", "expired",
        "locked_mojos"}``.  Once requested, the auto-refresh timer keeps
        the figures current.
        """
        with QMutexLocker(self._mutex):
            self._offer_summary_requested = True
        self._trigger_offer_summary.emit()

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

    def get_reports(self) -> None:
        """Request comprehensive report data.

        Results arrive on :pyattr:`reports_loaded`.
        """
        self._trigger_reports.emit()

    def query_pnl_display(self, baseline_iso: str = "") -> None:
        """Request restart-proof headline P&L figures from ``trade_log``.

        Results arrive on :pyattr:`pnl_display_loaded`.  The query is
        re-issued on every auto-refresh tick with the same baseline until
        a new baseline is supplied.

        Parameters
        ----------
        baseline_iso : str
            Display-reset baseline (UTC ``YYYY-MM-DD HH:MM:SS``), or an
            empty string for the full lifetime history.
        """
        safe_baseline = (baseline_iso or "").strip()
        with QMutexLocker(self._mutex):
            self._last_pnl_baseline = safe_baseline
        self._trigger_pnl_display.emit(safe_baseline)

    def query_deployed_capital(self) -> None:
        """Request per-asset resting-offer ("offered") amounts.

        Results arrive on :pyattr:`deployed_loaded`.  Once requested, the
        query is re-issued on every auto-refresh tick so the Balances
        tab's Deployed % figures stay current.
        """
        with QMutexLocker(self._mutex):
            self._deployed_requested = True
        self._trigger_deployed.emit()

    def query_pair_summary(self, offer_ttl_blocks: int = 0) -> None:
        """Request the per-pair view of our own book and fills.

        Results arrive on :pyattr:`pair_summary_loaded`.  Re-issued on every
        auto-refresh tick once requested, like the Deployed % figures.

        ``offer_ttl_blocks`` sizes the window in which a pending row still
        counts as our live quote; see :func:`our_book_window_hours`.
        """
        with QMutexLocker(self._mutex):
            self._pair_summary_requested = True
            self._book_ttl_blocks = int(offer_ttl_blocks or 0)
        self._trigger_pair_summary.emit(int(offer_ttl_blocks or 0))

    def query_last_trade_prices(self) -> None:
        """Request the most recent fill price per pair.

        Results arrive on :pyattr:`last_trade_prices_loaded` as
        ``{pair_name: price_mojos}``.  Not auto-refreshed; EngineBridge
        re-requests when its 30 s cache expires.
        """
        self._trigger_last_trade_prices.emit()

    def query_pnl_history(self, days: int = 90) -> None:
        """Request the persisted global P&L (USD) time series.

        Results arrive on :pyattr:`pnl_history_loaded`.  Intended to be
        called once at startup to rebuild the chart's P&L curve after a
        GUI restart; it is not auto-refreshed (live gauge samples extend
        the curve from there).

        Parameters
        ----------
        days : int
            Retention window in days (default 90, clamped to 1..365).
        """
        self._trigger_pnl_history.emit(int(days))

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
            pnl_baseline = self._last_pnl_baseline
            deployed_requested = self._deployed_requested
            pair_summary_requested = self._pair_summary_requested
            offer_summary_requested = self._offer_summary_requested

        if trade_params is not None:
            self._trigger_trades.emit(*trade_params)

        if offer_params is not None:
            self._trigger_offers.emit(*offer_params)

        # Keep the Orders panel's summary bar current.
        if offer_summary_requested:
            self._trigger_offer_summary.emit()

        # Keep the restart-proof P&L display current.
        if pnl_baseline is not None:
            self._trigger_pnl_display.emit(pnl_baseline)

        # Keep the Balances tab's Deployed % figures current.
        if deployed_requested:
            self._trigger_deployed.emit()

        # Keep the dashboard's per-pair table current.
        if pair_summary_requested:
            self._trigger_pair_summary.emit(self._book_ttl_blocks)

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
