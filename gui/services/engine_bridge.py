"""Unified abstraction layer between the GUI and the C++ trading engine.

``EngineBridge`` owns the three data-source services (config, metrics,
database) and exposes a single cohesive API that GUI widgets call to
read bot state, market data, and trade history.

Phase 1 (current) is strictly **read-only** -- all data flows from the
engine to the GUI via Prometheus scraping and SQLite queries.  Direct
engine control (start/stop, cancel offers) is stubbed for future IPC
integration.

Compliant with:
    - ISO/IEC 27001:2022  (no credentials in memory beyond config load)
    - ISO/IEC 5055       (no unreachable code, deterministic shutdown)
    - ISO/IEC 25000      (clear status reporting for all failure modes)
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any, Final, Optional

from PySide6.QtCore import QObject, QTimer, Signal, Slot

from gui.services.config_service import ConfigService
from gui.services.database_service import DatabaseService
from gui.services.metrics_service import MetricsService

# ---------------------------------------------------------------------------
# Module-level logger and constants
# ---------------------------------------------------------------------------
_log: logging.Logger = logging.getLogger(__name__)

# Default master refresh interval (milliseconds).
# This coordinates the overall polling cadence; individual services may
# run at different rates (metrics faster, DB slower).
_DEFAULT_MASTER_REFRESH_MS: Final[int] = 5_000

# Bot status strings.
STATUS_UNKNOWN: Final[str] = "Unknown"
STATUS_RUNNING: Final[str] = "Running"
STATUS_STOPPED: Final[str] = "Stopped"
STATUS_SHUTTING_DOWN: Final[str] = "ShuttingDown"
STATUS_DISCONNECTED: Final[str] = "Disconnected"

# Default filesystem paths (relative to project root).
_DEFAULT_CONFIG_PATH: Final[str] = "config.yaml"
_DEFAULT_DB_PATH: Final[str] = "data/xop_trader.db"
_DEFAULT_METRICS_URL: Final[str] = "http://localhost:9090/metrics"


class EngineBridge(QObject):
    """Facade that aggregates all data-source services for the GUI.

    Widgets interact with ``EngineBridge`` rather than touching
    ``MetricsService``, ``DatabaseService``, or ``ConfigService``
    directly.  This keeps coupling low and makes it straightforward to
    swap the passive polling backend for a real IPC channel later.

    Parameters
    ----------
    config_path : Path | str | None
        Path to YAML config file.  Falls back to ``config.yaml``.
    db_path : Path | str | None
        Path to SQLite database.  Falls back to ``data/xop_trader.db``.
    metrics_url : str | None
        Prometheus endpoint URL.  Falls back to
        ``http://localhost:9090/metrics``.
    master_refresh_ms : int
        Master coordination timer interval (default 5 000 ms).
    parent : QObject | None
        Optional Qt parent.

    Signals
    -------
    data_updated(dict)
        Emitted on every master refresh tick with an aggregated snapshot
        of the latest metrics, config, and DB summary data.
    bot_status_changed(str)
        Emitted when the inferred bot status changes (e.g. Running ->
        Disconnected).
    ready()
        Emitted after the startup sequence completes successfully.
    error(str)
        Emitted on any non-fatal service-level error.
    """

    # -- Qt signals ---------------------------------------------------------
    data_updated = Signal(dict)
    bot_status_changed = Signal(str)
    ready = Signal()
    error = Signal(str)

    def __init__(
        self,
        config_path: Path | str | None = None,
        db_path: Path | str | None = None,
        metrics_url: Optional[str] = None,
        master_refresh_ms: int = _DEFAULT_MASTER_REFRESH_MS,
        parent: Optional[QObject] = None,
    ) -> None:
        super().__init__(parent)

        # Resolve paths with sensible defaults.
        self._config_path: Path = Path(config_path or _DEFAULT_CONFIG_PATH).resolve()
        self._db_path: Path = Path(db_path or _DEFAULT_DB_PATH).resolve()
        self._metrics_url: str = metrics_url or _DEFAULT_METRICS_URL

        # -- Child services -------------------------------------------------
        self._config_svc: ConfigService = ConfigService(
            config_path=self._config_path,
            parent=self,
        )
        self._metrics_svc: MetricsService = MetricsService(
            url=self._metrics_url,
            parent=self,
        )
        self._database_svc: DatabaseService = DatabaseService(
            db_path=self._db_path,
            parent=self,
        )

        # -- Internal state -------------------------------------------------
        self._bot_status: str = STATUS_UNKNOWN
        self._last_data: dict[str, Any] = {}

        # -- Master refresh timer -------------------------------------------
        self._master_timer: QTimer = QTimer(self)
        self._master_timer.setTimerType(QTimer.TimerType.CoarseTimer)
        self._master_timer.setInterval(max(1_000, master_refresh_ms))
        self._master_timer.timeout.connect(self._on_master_tick)

        # -- Wire child service signals -------------------------------------
        self._config_svc.config_loaded.connect(self._on_config_loaded)
        self._config_svc.config_error.connect(self._on_service_error)

        self._metrics_svc.metrics_updated.connect(self._on_metrics_updated)
        self._metrics_svc.connection_lost.connect(self._on_metrics_lost)
        self._metrics_svc.connection_restored.connect(self._on_metrics_restored)

        self._database_svc.trade_summary_loaded.connect(self._on_trade_summary)
        self._database_svc.query_error.connect(self._on_service_error)

        _log.info(
            "EngineBridge created: config=%s, db=%s, metrics=%s",
            self._config_path,
            self._db_path,
            self._metrics_url,
        )

    # ===================================================================
    # Properties -- direct access to child services for advanced use
    # ===================================================================

    @property
    def config_service(self) -> ConfigService:
        """Return the owned ConfigService instance."""
        return self._config_svc

    @property
    def metrics_service(self) -> MetricsService:
        """Return the owned MetricsService instance."""
        return self._metrics_svc

    @property
    def database_service(self) -> DatabaseService:
        """Return the owned DatabaseService instance."""
        return self._database_svc

    # ===================================================================
    # Startup / shutdown
    # ===================================================================

    def initialise(self) -> None:
        """Run the startup sequence: config -> metrics -> DB -> ready.

        Each step is allowed to fail independently; the bridge logs
        warnings but continues so the GUI can display partial data.
        """
        _log.info("EngineBridge: starting initialisation sequence.")

        # Step 1 -- Load configuration.
        config_ok: bool = self._config_svc.load()
        if not config_ok:
            _log.warning("Config load failed; continuing with defaults.")

        # Step 2 -- Start metrics poller.  The URL may be overridden by
        # config if config loaded successfully.
        if config_ok:
            prom_port = self._config_svc.get_int("monitoring", "prometheus_port", 9090)
            # Only override if the user did not pass an explicit URL.
            if self._metrics_url == _DEFAULT_METRICS_URL:
                self._metrics_url = f"http://localhost:{prom_port}/metrics"
                # Update the worker's URL via the public service API.
                self._metrics_svc.set_url(self._metrics_url)

        self._metrics_svc.start()

        # Step 3 -- Open database.  If the config specifies a custom
        # path, use that instead of the constructor default.
        if config_ok:
            cfg_db_path = self._config_svc.get_str("database", "path", "")
            if cfg_db_path:
                resolved = Path(cfg_db_path).resolve()
                if resolved.is_file():
                    self._db_path = resolved
                    # Recreate the database service with the config path.
                    self._database_svc.stop()
                    self._database_svc = DatabaseService(
                        db_path=self._db_path,
                        parent=self,
                    )
                    self._database_svc.trade_summary_loaded.connect(
                        self._on_trade_summary
                    )
                    self._database_svc.query_error.connect(
                        self._on_service_error
                    )

        db_ok: bool = self._database_svc.start()
        if not db_ok:
            _log.warning("Database open failed; DB features disabled.")

        # Step 4 -- Start master refresh timer.
        self._master_timer.start()

        _log.info("EngineBridge initialisation complete.")
        self.ready.emit()

    def shutdown(self) -> None:
        """Gracefully shut down all services and timers."""
        _log.info("EngineBridge: shutting down.")
        self._master_timer.stop()
        self._metrics_svc.stop()
        self._database_svc.stop()
        _log.info("EngineBridge shutdown complete.")

    # ===================================================================
    # Public API -- unified data access
    # ===================================================================

    def get_bot_status(self) -> str:
        """Infer the bot's running status from metrics.

        Returns one of:
            ``"Running"`` -- metrics endpoint is up and reporting data.
            ``"Stopped"`` -- endpoint is up but reports inactive state.
            ``"Disconnected"`` -- endpoint is unreachable.
            ``"Unknown"`` -- no data yet.

        Returns
        -------
        str
        """
        return self._bot_status

    def get_all_data(self) -> dict[str, Any]:
        """Aggregate the latest data from all services into one dict.

        Returns
        -------
        dict
            Keys: ``pnl``, ``health``, ``offers``, ``risk``,
            ``market_data`` (dict of pair -> data), ``trade_summary``,
            ``config``, ``bot_status``.
        """
        # Collect per-pair market data from configured pairs.
        pairs = self._config_svc.get_pairs()
        market_data: dict[str, dict[str, float]] = {}
        for pair_cfg in pairs:
            pair_name = pair_cfg.get("name", "")
            if pair_name:
                market_data[pair_name] = self._metrics_svc.get_market_data(pair_name)

        data: dict[str, Any] = {
            "pnl": self._metrics_svc.get_pnl(),
            "health": self._metrics_svc.get_health(),
            "offers": self._metrics_svc.get_offers_summary(),
            "risk": self._metrics_svc.get_risk(),
            "market_data": market_data,
            "trade_summary": self._last_data.get("trade_summary", {}),
            "config": self._config_svc.get_full_config(),
            "bot_status": self._bot_status,
        }
        return data

    # ===================================================================
    # Public API -- engine control stubs (Phase 1: passive)
    # ===================================================================

    def start_engine(self) -> None:
        """Request the C++ engine to start.

        Phase 1 stub -- emits a status signal for the GUI to display
        a prompt asking the user to start the engine manually.
        """
        _log.warning(
            "start_engine() called but direct engine control is not "
            "yet available.  Please start the engine process manually."
        )
        self.error.emit(
            "Direct engine control not yet available. "
            "Please start the engine process manually."
        )

    def stop_engine(self) -> None:
        """Request the C++ engine to stop.

        Phase 1 stub -- logs a warning.
        """
        _log.warning(
            "stop_engine() called but direct engine control is not "
            "yet available.  Please stop the engine process manually."
        )
        self.error.emit(
            "Direct engine control not yet available. "
            "Please stop the engine process manually."
        )

    def cancel_offer(self, offer_id: str) -> None:
        """Request cancellation of a single offer.

        Parameters
        ----------
        offer_id : str
            The offer identifier to cancel.

        Phase 1 stub -- logs a warning.
        """
        _log.warning(
            "cancel_offer(%s) called but direct control is not yet "
            "available.",
            offer_id,
        )
        self.error.emit(
            f"Cannot cancel offer {offer_id}: direct engine control "
            f"not yet available."
        )

    def cancel_all_offers(self) -> None:
        """Request cancellation of all outstanding offers.

        Phase 1 stub -- logs a warning.
        """
        _log.warning(
            "cancel_all_offers() called but direct control is not yet "
            "available."
        )
        self.error.emit(
            "Cannot cancel offers: direct engine control not yet available."
        )

    def reload_config(self) -> None:
        """Re-read the YAML configuration and signal the engine.

        In Phase 1 this only reloads the in-memory config; there is no
        IPC to notify the engine.
        """
        _log.info("Reloading configuration from %s", self._config_path)
        success = self._config_svc.reload()
        if success:
            _log.info("Config reloaded.  Engine notification not yet available.")
        else:
            self.error.emit("Config reload failed -- see logs for details.")

    # ===================================================================
    # Internal slots
    # ===================================================================

    @Slot()
    def _on_master_tick(self) -> None:
        """Master refresh timer handler.

        Aggregates the latest service data and emits ``data_updated``.
        """
        data = self.get_all_data()
        self._last_data = data
        self.data_updated.emit(data)

    @Slot(dict)
    def _on_config_loaded(self, config: dict[str, Any]) -> None:
        """Handle successful config load.

        Parameters
        ----------
        config : dict
            Full parsed configuration.
        """
        _log.info("Config loaded into EngineBridge (%d keys).", len(config))

    @Slot(dict)
    def _on_metrics_updated(self, metrics: dict) -> None:
        """Handle a fresh metrics scrape.

        Updates the inferred bot status based on health indicators.

        Parameters
        ----------
        metrics : dict
            Parsed Prometheus metrics.
        """
        health = self._metrics_svc.get_health()
        node_synced = health.get("node_synced", 0.0)
        wallet_connected = health.get("wallet_connected", 0.0)

        # Infer running status: if both node and wallet are up, the
        # engine is running.  If the endpoint is reachable but the
        # node is not synced, we report it as running (degraded) --
        # the status bar can show the detail.
        if node_synced >= 1.0 and wallet_connected >= 1.0:
            new_status = STATUS_RUNNING
        elif node_synced >= 1.0 or wallet_connected >= 1.0:
            # Partially connected -- still consider it running.
            new_status = STATUS_RUNNING
        else:
            new_status = STATUS_STOPPED

        self._update_status(new_status)

    @Slot()
    def _on_metrics_lost(self) -> None:
        """Handle metrics connection loss."""
        _log.warning("Metrics connection lost.")
        self._update_status(STATUS_DISCONNECTED)

    @Slot()
    def _on_metrics_restored(self) -> None:
        """Handle metrics connection restoration."""
        _log.info("Metrics connection restored.")
        # Status will be updated on the next metrics_updated signal.

    @Slot(dict)
    def _on_trade_summary(self, summary: dict[str, Any]) -> None:
        """Cache the latest trade summary from the database.

        Parameters
        ----------
        summary : dict
            Aggregated trade statistics.
        """
        self._last_data["trade_summary"] = summary

    @Slot(str)
    def _on_service_error(self, msg: str) -> None:
        """Forward child service errors to the bridge-level signal.

        Parameters
        ----------
        msg : str
            Error description.
        """
        _log.warning("Service error: %s", msg)
        self.error.emit(msg)

    # ===================================================================
    # Internal helpers
    # ===================================================================

    def _update_status(self, new_status: str) -> None:
        """Update cached bot status and emit a signal on change.

        Parameters
        ----------
        new_status : str
            New status string.
        """
        if new_status != self._bot_status:
            old = self._bot_status
            self._bot_status = new_status
            _log.info("Bot status changed: %s -> %s", old, new_status)
            self.bot_status_changed.emit(new_status)
