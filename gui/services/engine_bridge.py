"""Unified abstraction layer between the GUI and the C++ trading engine.

``EngineBridge`` owns the three data-source services (config, metrics,
database) and exposes a single cohesive API that GUI widgets call to
read bot state, market data, and trade history.

Phase 1 data flows are **read-only** -- metrics via Prometheus scraping
and history via SQLite queries.  The bridge can auto-launch the C++
engine as a managed subprocess when the binary is co-located with the
GUI (e.g. after running the Windows installer).  Cancel-offer control
is stubbed for future IPC integration.

Compliant with:
    - ISO/IEC 27001:2022  (no credentials in memory beyond config load)
    - ISO/IEC 5055       (no unreachable code, deterministic shutdown)
    - ISO/IEC 25000      (clear status reporting for all failure modes)
"""

from __future__ import annotations

import logging
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Final, Optional

from PySide6.QtCore import QObject, Qt, QTimer, Signal, Slot

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
STATUS_ANALYZING: Final[str] = "Analyzing"
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
    engine_start_failed(str)
        Emitted when the managed engine process exits during startup and
        a user-actionable diagnostic message is available.
    """

    # -- Qt signals ---------------------------------------------------------
    data_updated = Signal(dict)
    bot_status_changed = Signal(str)
    ready = Signal()
    error = Signal(str)
    engine_start_failed = Signal(str)

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
        self._engine_process: subprocess.Popen | None = None
        self._engine_log_fh: Any = None
        self._engine_log_path: Path | None = None
        self._engine_launch_dir: Path | None = None

        # -- Master refresh timer -------------------------------------------
        self._master_timer: QTimer = QTimer(self)
        self._master_timer.setTimerType(Qt.TimerType.CoarseTimer)
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
        else:
            self._apply_configured_database_path()

        # Step 1.5 -- Auto-start C++ engine if a co-located binary exists
        # and no engine is already responding on the metrics endpoint.
        if not self._is_engine_reachable():
            engine_ok = self._start_engine_process()
            if engine_ok:
                _log.info("C++ engine auto-started; waiting for metrics endpoint.")
            else:
                _log.info("C++ engine binary not found; GUI-only monitoring mode.")

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
        self._stop_engine_process()
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
            ``market_data`` (dict of pair -> data), ``order_book``,
            ``analysis`` (startup analysis data, empty dict when not in
            Analyzing phase), ``trade_summary``, ``config``, ``bot_status``.
        """
        # Collect per-pair market data from configured pairs.
        pairs = self._config_svc.get_pairs()
        market_data: dict[str, dict[str, float]] = {}
        for pair_cfg in pairs:
            pair_name = pair_cfg.get("name", "")
            if pair_name:
                market_data[pair_name] = self._metrics_svc.get_market_data(pair_name)

        # Build per-pair order book data from the latest market-data
        # snapshots.  This provides the depth information that the
        # OrderBookWidget uses to render bid/ask levels.
        order_book: dict[str, dict[str, float]] = {
            pair_name: self._metrics_svc.get_market_data(pair_name)
            for pair_name in (
                p.get("name", "") for p in pairs if p.get("name")
            )
            if pair_name
        }

        # Collect startup market analysis data — only when the bot is
        # actively in the Analyzing phase to avoid rendering a non-existent
        # analysis session in the GUI.
        pair_names = [p.get("name", "") for p in pairs if p.get("name")]
        if self._bot_status == STATUS_ANALYZING and pair_names:
            analysis_data = self._metrics_svc.get_analysis(pair_names)
        else:
            analysis_data = {}

        data: dict[str, Any] = {
            "pnl": self._metrics_svc.get_pnl(),
            "health": self._metrics_svc.get_health(),
            "offers": self._metrics_svc.get_offers_summary(),
            "risk": self._metrics_svc.get_risk(),
            "market_data": market_data,
            "order_book": order_book,
            "analysis": analysis_data,
            "trade_summary": self._last_data.get("trade_summary", {}),
            "config": self._config_svc.get_full_config(),
            "bot_status": self._bot_status,
        }
        return data

    # ===================================================================
    # Public API -- engine control
    # ===================================================================

    def start_engine(self) -> None:
        """Start the C++ engine as a managed subprocess.

        Locates the engine binary next to the GUI executable and
        launches it.  Emits an error signal if the binary is missing
        or the process fails to start.
        """
        if self._engine_process is not None and self._engine_process.poll() is None:
            _log.info("Engine already running (PID %d).", self._engine_process.pid)
            return

        if self._start_engine_process():
            _log.info("Engine started via start_engine().")
        else:
            self.error.emit(
                "Could not start the engine.  Make sure xop_trader"
                + (".exe" if sys.platform == "win32" else "")
                + " is in the same folder as the GUI."
            )

    def stop_engine(self) -> None:
        """Gracefully stop the managed C++ engine subprocess."""
        self._stop_engine_process()

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

    def update_config_path(self, new_path: str) -> None:
        """Switch the active configuration file and reload it.

        Called when the Settings panel saves or loads a file that differs
        from the bridge's current path.  Keeps ``self._config_path`` and
        the ``ConfigService`` in sync so subsequent ``reload_config()``
        calls and engine restarts use the new file.

        Parameters
        ----------
        new_path : str
            Absolute path of the YAML file that was just saved/loaded.
        """
        resolved = Path(new_path).resolve()
        if resolved == self._config_path:
            # Path unchanged; just reload in case the file changed on disk.
            self.reload_config()
            return

        _log.info(
            "Config path changed: %s → %s",
            self._config_path,
            resolved,
        )
        self._config_path = resolved
        success = self._config_svc.switch_path(resolved)
        if success:
            _log.info("Config path updated and reloaded.")
        else:
            self.error.emit(
                f"Could not load config from {resolved}. "
                "See logs for details."
            )

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
        if node_synced >= 1.0 or wallet_connected >= 1.0:
            new_status = STATUS_RUNNING
        else:
            new_status = STATUS_STOPPED

        # Check if the engine is currently in the startup analysis phase.
        # Infer Analyzing from Prometheus metrics: blocks_target > 0 and
        # at least one pair has not yet completed its analysis window.
        if new_status == STATUS_RUNNING and self._metrics_svc.is_analysis_active():
            new_status = STATUS_ANALYZING

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

    @Slot()
    def _check_engine_startup_result(self) -> None:
        """Detect early engine exit and emit a detailed diagnostic."""
        if self._engine_process is None:
            return

        if self._engine_process.poll() is None:
            return

        if self._is_engine_reachable():
            return

        return_code = self._engine_process.returncode
        message = self._build_engine_start_failure_message(return_code)
        self._stop_engine_process()
        self.engine_start_failed.emit(message)
        self.error.emit("Engine startup failed. See the error dialog for details.")

    # ===================================================================
    # Engine subprocess management
    # ===================================================================

    def _find_engine_binary(self) -> Path | None:
        """Locate the C++ engine binary relative to the GUI executable.

        When running from a PyInstaller bundle the binary is expected
        next to ``sys.executable``.  For one-file bundles, fallback to
        ``sys._MEIPASS`` where PyInstaller extracts bundled binaries.
        In development mode the current working directory is checked.
        """
        engine_name = "xop_trader.exe" if sys.platform == "win32" else "xop_trader"

        # Optional override for power users and troubleshooting.
        override_path = os.environ.get("XOP_ENGINE_PATH", "").strip()
        if override_path:
            candidate = Path(override_path).resolve()
            if candidate.is_file():
                return candidate

        candidates: list[Path] = []

        if getattr(sys, "frozen", False):
            exe_dir = Path(sys.executable).parent
            candidates.append(exe_dir / engine_name)

            # PyInstaller one-file bundles extract resources here at runtime.
            meipass = getattr(sys, "_MEIPASS", None)
            if meipass:
                meipass_dir = Path(str(meipass))
                candidates.append(meipass_dir / engine_name)
                candidates.append(meipass_dir / "engine-runtime" / engine_name)
        else:
            candidates.append(Path.cwd() / engine_name)

        for candidate in candidates:
            if candidate.is_file():
                return candidate

        return None

    def _is_engine_reachable(self) -> bool:
        """Return True if the Prometheus metrics endpoint already responds.

        Prevents spawning a duplicate engine when one is already
        running (e.g. started manually or by a service manager).
        """
        import requests as _req  # noqa: WPS433 — keep top-level imports light

        try:
            resp = _req.get(self._metrics_url, timeout=2)
            return resp.status_code == 200
        except Exception:
            return False

    def _start_engine_process(self) -> bool:
        """Launch the C++ engine as a child process.

        Engine stdout/stderr is redirected to ``engine.log`` next to
        the binary so users can inspect output without a console window.
        Returns *True* on success.
        """
        engine_path = self._find_engine_binary()
        if engine_path is None:
            return False

        cmd: list[str] = [str(engine_path)]
        if self._config_path.is_file():
            cmd.extend(["--config", str(self._config_path)])

        try:
            launch_dir = self._determine_engine_launch_dir(engine_path)
            self._ensure_engine_runtime_dirs(launch_dir)

            # Keep logs next to the GUI executable when frozen.  This avoids
            # writing logs into ephemeral extraction directories.
            if getattr(sys, "frozen", False):
                log_path = Path(sys.executable).parent / "engine.log"
            else:
                log_path = engine_path.parent / "engine.log"
            log_path.parent.mkdir(parents=True, exist_ok=True)
            self._engine_log_fh = open(log_path, "a")  # noqa: SIM115
            self._engine_log_path = log_path
            self._engine_launch_dir = launch_dir

            kwargs: dict[str, Any] = {}
            if sys.platform == "win32":
                kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
            kwargs["cwd"] = str(launch_dir)

            self._engine_process = subprocess.Popen(
                cmd,
                stdout=self._engine_log_fh,
                stderr=subprocess.STDOUT,
                **kwargs,
            )
            _log.info(
                "Started C++ engine (PID %d): %s  log → %s",
                self._engine_process.pid,
                cmd,
                log_path,
            )
            QTimer.singleShot(3_000, self._check_engine_startup_result)
            return True
        except Exception:
            _log.exception("Failed to start C++ engine subprocess.")
            if self._engine_log_fh is not None:
                self._engine_log_fh.close()
                self._engine_log_fh = None
            self._engine_log_path = None
            self._engine_launch_dir = None
            return False

    def _stop_engine_process(self) -> None:
        """Terminate the managed engine subprocess if it is still running."""
        if self._engine_process is None:
            return

        if self._engine_process.poll() is None:
            _log.info(
                "Terminating C++ engine (PID %d).",
                self._engine_process.pid,
            )
            self._engine_process.terminate()
            try:
                self._engine_process.wait(timeout=10)
                _log.info("Engine process exited cleanly.")
            except subprocess.TimeoutExpired:
                _log.warning("Engine did not exit in 10 s; sending SIGKILL.")
                self._engine_process.kill()
                try:
                    self._engine_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    _log.error("Engine process could not be killed.")
        else:
            _log.info(
                "Engine process already exited (rc=%d).",
                self._engine_process.returncode,
            )

        self._engine_process = None
        if self._engine_log_fh is not None:
            self._engine_log_fh.close()
            self._engine_log_fh = None
        self._engine_log_path = None
        self._engine_launch_dir = None

    # ===================================================================
    # Internal helpers
    # ===================================================================

    def _apply_configured_database_path(self) -> None:
        """Sync the GUI database service path to the active config file."""
        cfg_db_path = self._config_svc.get_str("database", "path", "").strip()
        if not cfg_db_path:
            return

        resolved = self._resolve_config_relative_path(cfg_db_path)
        try:
            resolved.parent.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            _log.warning("Could not create database directory %s: %s", resolved.parent, exc)

        if resolved == self._db_path:
            return

        self._db_path = resolved
        self._database_svc.stop()
        self._database_svc = DatabaseService(
            db_path=self._db_path,
            parent=self,
        )
        self._database_svc.trade_summary_loaded.connect(self._on_trade_summary)
        self._database_svc.query_error.connect(self._on_service_error)

    def _resolve_config_relative_path(self, raw_path: str) -> Path:
        """Resolve config paths relative to the config file directory."""
        candidate = Path(raw_path).expanduser()
        if candidate.is_absolute():
            return candidate.resolve()
        return (self._config_path.parent / candidate).resolve()

    def _determine_engine_launch_dir(self, engine_path: Path) -> Path:
        """Return the working directory to use for the engine process."""
        if self._config_path.is_file():
            return self._config_path.parent
        return engine_path.parent

    def _ensure_engine_runtime_dirs(self, launch_dir: Path) -> None:
        """Create common runtime directories expected by the engine."""
        try:
            (launch_dir / "logs").mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            _log.warning("Could not create logs directory %s: %s", launch_dir / "logs", exc)

        cfg_db_path = self._config_svc.get_str("database", "path", "").strip()
        if not cfg_db_path:
            return

        db_path = self._resolve_config_relative_path(cfg_db_path)
        try:
            db_path.parent.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            _log.warning("Could not create database directory %s: %s", db_path.parent, exc)

    def _build_engine_start_failure_message(self, return_code: int | None) -> str:
        """Build a detailed diagnostic for early managed-engine failures."""
        engine_path = self._find_engine_binary()
        log_tail = self._read_log_tail(self._engine_log_path)
        lowered_log_tail = log_tail.lower()

        if (
            "application control policy has blocked this file" in lowered_log_tail
            or "smart app control" in lowered_log_tail
        ):
            lines = [
                "Windows blocked the XOPTrader engine from starting.",
                "",
                "Recommended fixes:",
                "1. Install or run XOPTrader from a trusted local folder.",
                "2. If Windows shows a security prompt for the engine, allow or unblock the app if you trust it.",
                "3. Prefer the signed installer or an allow-list rule over disabling Smart App Control globally.",
                "",
            ]

            if engine_path is not None:
                lines.append(f"Engine binary: {engine_path}")
            if self._engine_launch_dir is not None:
                lines.append(f"Working directory: {self._engine_launch_dir}")
            if self._config_path:
                lines.append(f"Config file: {self._config_path}")
            if self._engine_log_path is not None:
                lines.append(f"Launch log: {self._engine_log_path}")

            lines.extend([
                "",
                "Recent log output:",
                log_tail,
            ])
            return "\n".join(lines)

        lines = [
            "XOPTrader could not start the engine automatically.",
            "",
        ]

        if engine_path is not None:
            lines.append(f"Engine binary: {engine_path}")
        if self._engine_launch_dir is not None:
            lines.append(f"Working directory: {self._engine_launch_dir}")
        if self._config_path:
            lines.append(f"Config file: {self._config_path}")
        if return_code is not None:
            lines.append(f"Exit code: {return_code}")
        if self._engine_log_path is not None:
            lines.append(f"Launch log: {self._engine_log_path}")

        lines.extend([
            "",
            "Recent log output:",
            log_tail,
        ])
        return "\n".join(lines)

    def _read_log_tail(self, log_path: Path | None, max_lines: int = 20) -> str:
        """Return the last few log lines for user-facing diagnostics."""
        if log_path is None or not log_path.is_file():
            return "(No log output available.)"

        try:
            lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return "(Could not read log output.)"

        tail = lines[-max_lines:]
        return "\n".join(tail) if tail else "(Log file was empty.)"

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
