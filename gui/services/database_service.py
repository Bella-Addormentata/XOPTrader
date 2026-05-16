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
from datetime import date, datetime, timedelta
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
    reports_ready = Signal(dict)
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
        # We convert per-pair using the quote-asset's mojos-per-unit and
        # USD-per-unit:
        #   wUSDC.b / wUSDC : 1e3 mojos/unit, $1.00/unit  -> divide by 1e3
        #   BYC             : 1e3 mojos/unit, $1.00/unit  -> divide by 1e3
        #                     (BYC is a Chia-native USD stablecoin; treated
        #                      as 1:1 for accounting until a feed is added)
        #   DBX             : not USD-pegged and no on-DB rate -> 0.0 (will
        #                     be reported as "USD value unknown")
        #   anything else   : 0.0 (future pairs require an explicit mapping)
        #
        # Fees are paid on-chain in XCH mojos, so the fee_usdc_expr below
        # keeps the legacy XCH/USD conversion.
        pnl_usdc_expr = (
            "CASE "
            "WHEN t.pair_name LIKE '%/wUSDC%' THEN t.realized_pnl_mojos / 1000.0 "
            "WHEN t.pair_name LIKE '%/BYC%'   THEN t.realized_pnl_mojos / 1000.0 "
            "WHEN t.pair_name LIKE '%/USDS%'  THEN t.realized_pnl_mojos / 1000.0 "
            "ELSE 0.0 END"
        )
        fee_usdc_expr = (
            f"({fee_usdc_expr})"
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
    snapshots_loaded = Signal(list)
    trade_summary_loaded = Signal(dict)
    pairs_list_loaded = Signal(list)
    latest_snapshot_loaded = Signal(dict)
    reports_loaded = Signal(dict)
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
    _trigger_reports = Signal()

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
        self._worker.reports_ready.connect(self.reports_loaded)
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
        self._trigger_reports.connect(self._worker.fetch_reports)

        # -- Auto-refresh timer ---------------------------------------------
        self._refresh_timer: QTimer = QTimer(self)
        self._refresh_timer.setTimerType(Qt.TimerType.CoarseTimer)
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
