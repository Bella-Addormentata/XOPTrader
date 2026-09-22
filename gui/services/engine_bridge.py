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
import time
from pathlib import Path
from typing import Any, Final, Optional

from PySide6.QtCore import QObject, Qt, QTimer, Signal, Slot

from gui import shutdown_flag, stop_offers
from gui.services.config_service import ConfigService
from gui.services.config_split import split_and_save
from gui.services.database_service import DatabaseService
from gui.services.metrics_service import MetricsService
from gui.services.wallet_service import WalletService
from gui.services.warp.service import WarpService

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
STATUS_PAUSED: Final[str] = "Paused"
STATUS_SHUTTING_DOWN: Final[str] = "ShuttingDown"
STATUS_DISCONNECTED: Final[str] = "Disconnected"

# Default filesystem paths (relative to project root).
_DEFAULT_CONFIG_PATH: Final[str] = "config.yaml"
_DEFAULT_DB_PATH: Final[str] = "data/xop_trader.db"
_DEFAULT_METRICS_URL: Final[str] = "http://localhost:9090/metrics"

# [shutdown-flag-race] How long _stop_engine_process waits for the engine to
# honour its addressed shutdown.flag before terminating it.
_GRACEFUL_STOP_WAIT_S: Final[int] = 30

# How long the per-pair last-trade-price cache stays warm before we
# re-query trade_log.  Trades arrive at human pace (seconds to minutes),
# so a 30 s window keeps the wallet allocation panel responsive while
# avoiding redundant SQLite hits on every master tick.
_LAST_TRADE_CACHE_TTL_S: Final[float] = 30.0


def _holding_for_asset(balances: dict, base_asset_id: str):
    """Our confirmed holding of *base_asset_id* in display units, or None.

    The standard XCH wallet carries no asset id -- wallet_service resolves
    ids for CAT wallets only -- so "xch" is matched by wallet_type instead.

    Returns None for UNKNOWN rather than 0.0: the balance cache is empty
    until the first async RPC lands, a failed fetch yields an empty result,
    and a partial fetch can omit one wallet.  Reporting 0.0 in those cases
    would state that we hold nothing, which is a much stronger claim than
    "not known yet" -- and on this column a confident zero is exactly the
    kind of wrong number an operator would act on.  A wallet that matched
    and really holds nothing still returns 0.0.
    """
    if not balances:
        return None
    want = str(base_asset_id or "").strip().lower()
    for _name, row in (balances or {}).items():
        if not isinstance(row, dict):
            continue
        if want == "xch":
            # NOT `or -1`: the standard wallet's type IS 0, which is falsy,
            # so that idiom turned every XCH wallet into -1 and matched
            # nothing -- Inventory would have read zero on every XCH pair.
            raw_type = row.get("wallet_type", -1)
            try:
                wallet_type = int(raw_type) if raw_type is not None else -1
            except (TypeError, ValueError):
                wallet_type = -1
            if wallet_type == 0:
                return float(row.get("confirmed", 0.0) or 0.0)
        elif str(row.get("asset_id", "") or "").lower() == want:
            return float(row.get("confirmed", 0.0) or 0.0)
    return None


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

        # Resolve paths with sensible defaults. Frozen builds default into
        # the per-user data dir (gui.utils.user_data_dir) -- the CWD of an
        # installed launch is Program Files, where nothing may be written.
        from gui.utils import default_config_path as _dcp

        self._config_path: Path = (
            Path(config_path).resolve() if config_path else _dcp()
        )
        self._db_path: Path = (
            Path(db_path).resolve()
            if db_path
            else (self._config_path.parent / _DEFAULT_DB_PATH).resolve()
        )
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
        self._wallet_svc: WalletService = WalletService(
            config={},
            parent=self,
        )
        # Warp bridge worker (background USDC(Base) -> wUSDC.b). Constructed
        # here but inert until warp.enabled is set in config; see the
        # enabled-gate in WarpService's worker (_build_engine).
        self._warp_svc: WarpService = WarpService(
            config={},
            parent=self,
            # The Base hot-wallet's key writes (create / rotate) go straight to
            # secrets.yaml beside the config file, comment-preservingly.
            secrets_path=self._config_path.parent / "secrets.yaml",
        )

        # -- Internal state -------------------------------------------------
        self._bot_status: str = STATUS_UNKNOWN
        self._last_data: dict[str, Any] = {}
        self._engine_process: subprocess.Popen | None = None
        self._engine_log_fh: Any = None
        self._engine_log_path: Path | None = None
        self._engine_launch_dir: Path | None = None
        # [S74] Whether the engine binary THIS bridge launched advertises the
        # stop policy in --help (stop_offers.ENGINE_HELP_TOKEN). False until a
        # launch proves otherwise: an engine that predates the policy cancels
        # the book on a request that says keep.
        self._engine_keep_supported: bool = False
        # The binary that probe was run against, and whether a "no" has
        # already been asked a second time (engine_supports_keep_offers).
        self._engine_binary_path: Path | None = None
        self._engine_keep_reprobed: bool = False
        # [S74] The operator's answer to the close prompt, held from
        # MainWindow.closeEvent until aboutToQuit runs shutdown(). None means
        # "nobody answered": the request carries no offers line and the
        # engine's own engine.shutdown_offers decides.
        self._close_offers_policy: Optional[str] = None
        self._tick_count: int = 0

        # Cache for the most recent price_mojos per pair, looked up from
        # trade_log on demand.  Refreshed every _LAST_TRADE_CACHE_TTL_S
        # seconds so the wallet allocation widget has a price source even
        # when xop_market_mid_price is 0 (engine just restarted, market
        # is quiet, or metrics endpoint is briefly unreachable).
        # The query itself runs on the DatabaseService worker thread
        # (fetch_last_trade_prices); the bridge only consumes the cached
        # result and must never touch sqlite3 on the UI thread.
        self._last_trade_prices: dict[str, float] = {}
        self._last_trade_cache_ts: float = 0.0
        self._last_trade_request_ts: float = 0.0

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
        self._database_svc.last_trade_prices_loaded.connect(self._on_last_trade_prices)
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

    @property
    def db_path(self) -> Path:
        """The resolved database path, as ConfigService.path is for config.

        Exposed so widgets can hand real paths to the offer-sizing
        calculator: loaded from the application bundle, that module cannot
        derive them from its own __file__.
        """
        return self._db_path

    @property
    def warp_service(self) -> WarpService:
        """Return the owned WarpService instance."""
        return self._warp_svc

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
                _log.info("Metrics URL overridden from config: %s", self._metrics_url)
            else:
                _log.info("Metrics URL kept (user-supplied): %s", self._metrics_url)

        self._metrics_svc.start()

        # Wallet RPC fetches run on their own worker thread; the master
        # tick only triggers fetches (never blocks on them).
        self._wallet_svc.start()

        # Warp bridge worker starts alongside the wallet.  It is an idle
        # no-op unless warp.enabled is set in config, so this is always safe
        # for the live bot; a disabled tick never touches the network.
        self._warp_svc.start()

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
        self._wallet_svc.stop()
        self._warp_svc.stop()
        # [S74] The close prompt's answer, if the operator gave one. A session
        # end or a signal reaches here with None, and the engine's own
        # engine.shutdown_offers decides.
        self._stop_engine_process(
            offers_policy=getattr(self, "_close_offers_policy", None))
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

    def _get_last_trade_prices(self) -> dict[str, float]:
        """Return ``{pair_name: price_mojos}`` from the most recent fill per pair.

        Non-blocking: always returns the cached dict immediately.  When
        the cache is older than ``_LAST_TRADE_CACHE_TTL_S`` an async
        refresh is dispatched to the DatabaseService worker thread; the
        result lands in :meth:`_on_last_trade_prices`.  On any query
        error the previously cached value simply stays in place, so
        callers always get a usable dict.
        """
        now = time.monotonic()
        cache_fresh = (
            self._last_trade_prices
            and (now - self._last_trade_cache_ts) < _LAST_TRADE_CACHE_TTL_S
        )
        if cache_fresh:
            return self._last_trade_prices

        # Rate-limit refresh requests: at most one outstanding request
        # per master-tick period, so a failing worker query (DB locked,
        # missing table) cannot pile up triggers while still allowing a
        # retry on the next tick.
        if (now - self._last_trade_request_ts) >= 5.0:
            self._last_trade_request_ts = now
            self._database_svc.query_last_trade_prices()

        return self._last_trade_prices

    @Slot(dict)
    def _on_last_trade_prices(self, prices: dict) -> None:
        """Cache the worker-fetched last-trade prices (GUI thread)."""
        if prices:
            self._last_trade_prices = dict(prices)
        # Mark the cache warm even for an empty result (no fills yet)
        # so we do not hammer the worker with a re-query every tick.
        self._last_trade_cache_ts = time.monotonic()

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
        # One snapshot, reused per pair and re-used for the payload below.
        wallet_balances_snapshot = self._wallet_svc.get_balances()
        market_data: dict[str, dict[str, float]] = {}
        last_trade_prices = self._get_last_trade_prices()
        for pair_cfg in pairs:
            pair_name = pair_cfg.get("name", "")
            if pair_name:
                pair_md = self._metrics_svc.get_market_data(pair_name)
                last_px = last_trade_prices.get(pair_name, 0.0)
                if last_px > 0.0:
                    # Always expose so downstream widgets can pick the best
                    # available signal.
                    pair_md["last_trade_price"] = last_px
                    # Backfill mid_price when the live metric hasn't
                    # published yet so anything reading mid_price (e.g. the
                    # USD price graph in wallet_balances) still gets a
                    # sensible value derived from the most recent fill.
                    if float(pair_md.get("mid_price", 0.0) or 0.0) <= 0.0:
                        pair_md["mid_price"] = last_px
                        pair_md["mid_price_source"] = "last_trade"
                # Our own holding of the pair's BASE asset, in that
                # asset's mojos.  Keyed by asset_id rather than by wallet
                # name: the display name is whatever the Chia client calls
                # it, while the id comes from the pair config and cannot
                # drift.  MetricsService.get_inventory existed but nothing
                # had ever called it.
                # Our holding of the pair's BASE asset, taken from the
                # WALLET rather than from xop_inventory_balance.  That gauge
                # is published from State::get_all_positions(), while
                # balance-changing paths (reward receipts, wallet
                # reconciliation) update InventoryTracker without
                # synchronising State -- so it can report a pre-change
                # balance until the engine restarts.  The wallet RPC is
                # authoritative and already keyed by asset id.
                #
                # Ids are lower-cased on both sides: the Settings UI accepts
                # uppercase while wallet_service lower-cases what it
                # resolves, and a case-sensitive comparison would silently
                # find nothing and show zero.
                #
                # These balances are already in DISPLAY UNITS (wallet_service
                # divides by 1e12 for XCH and 1000 for a CAT), so they must
                # not be divided again downstream.
                base_id = str(pair_cfg.get("base_asset_id", "") or "").strip().lower()
                if base_id:
                    holding = _holding_for_asset(
                        wallet_balances_snapshot, base_id
                    )
                    # Omit the key entirely when unknown, so a consumer that
                    # defaults a missing key cannot turn it into a zero.
                    if holding is not None:
                        pair_md["inventory_units"] = holding
                market_data[pair_name] = pair_md

        # Build per-pair order book data from the latest market-data
        # snapshots.  This provides the depth information that the
        # OrderBookWidget uses to render bid/ask levels.
        # T8-16: Reuse the market_data dict instead of calling
        # get_market_data() a second time for each pair.
        order_book: dict[str, dict[str, float]] = dict(market_data)

        # Collect startup market analysis data.  The C++ engine publishes
        # xop_analysis / xop_analysis_pair metrics throughout the analysis
        # window and retains them after completion, so always fetch them
        # when pair names are available.
        # Only enabled pairs participate in startup analysis on the engine
        # side; querying disabled pairs here fabricates default "incomplete"
        # entries and can keep the GUI stuck in analysis mode.
        pair_names = [
            p.get("name", "")
            for p in pairs
            if p.get("name") and p.get("enabled", True)
        ]
        if pair_names:
            analysis_data = self._metrics_svc.get_analysis(pair_names)
        else:
            analysis_data = {}

        health = self._metrics_svc.get_health()
        wallet_sync = self._wallet_svc.get_sync_status() if hasattr(self._wallet_svc, "get_sync_status") else {}
        if wallet_sync:
            # [S33 2026-09-05] Copy the direct-RPC triple AUTHORITATIVELY,
            # false values included.  The engine gauges are NOT a safe
            # fallback for a "no" answer: xop_node wallet_connected is only
            # ChiaRPCBase::is_open() (the client object was opened and never
            # closed -- never a reachability check), and wallet_synced_ is
            # written solely inside Engine::step_manage_offers, so it holds
            # its last value for as long as an earlier gate short-circuits
            # Step 8 (dry run, breaker pause, wallet circuit, GUI pause).
            # Writing only the optimistic branches left a desynced or
            # unreachable wallet painted "Wallet: Synced" green off those
            # stale gauges.  wallet_service always publishes a COMPLETE
            # triple per pass and caches it only when non-empty, so an empty
            # dict (no fetch yet) still falls through to the engine gauges.
            synced = bool(wallet_sync.get("synced"))
            syncing = bool(wallet_sync.get("syncing")) and not synced
            health["wallet_connected"] = 1.0 if wallet_sync.get("connected") else 0.0
            health["wallet_synced"] = 1.0 if synced else 0.0
            health["wallet_syncing"] = 1.0 if syncing else 0.0

        data: dict[str, Any] = {
            "pnl": self._metrics_svc.get_pnl(),
            "health": health,
            "offers": self._metrics_svc.get_offers_summary(),
            "risk": self._metrics_svc.get_risk(),
            "market_data": market_data,
            "order_book": order_book,
            "analysis": analysis_data,
            "trade_summary": self._last_data.get("trade_summary", {}),
            "config": self._config_svc.get_full_config(),
            "bot_status": self._bot_status,
            "spendable_reserve": self._metrics_svc.get_spendable_reserve(),
            "stuck_offers": self._metrics_svc.get_stuck_offers(),
            "fees_paid_24h": self._metrics_svc.get_fees_paid_24h(),
            "wallet_balances": wallet_balances_snapshot,
            "warp": self._warp_svc.get_snapshot(),
            "metrics_connected": self._metrics_svc.is_connected,
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

    def stop_engine(self, offers_policy: Optional[str] = None) -> None:
        """Gracefully stop the managed C++ engine subprocess.

        [S74] *offers_policy* is the operator's answer to the stop prompt:
        ``"keep"`` leaves the resting offers on the book, ``"cancel"`` cancels
        them, and None writes no policy into the request, so the engine's own
        ``engine.shutdown_offers`` decides. A ``"keep"`` this engine build
        cannot honour stops NOTHING: that engine would cancel the book, which
        is the one outcome the operator just declined.
        """
        if (stop_offers.parse_policy(offers_policy) == stop_offers.POLICY_KEEP
                and not self.engine_supports_keep_offers):
            _log.error(
                "Stop with offers kept was requested, but this engine build "
                "does not advertise the stop policy; NOT stopping -- it would "
                "cancel the book.")
            self.error.emit(
                "This engine build cannot keep offers across a stop (it "
                "predates the stop policy and would cancel them). The engine "
                "was NOT stopped.")
            return
        outcome = self._stop_engine_process(offers_policy=offers_policy)
        if outcome is shutdown_flag.StopOutcome.STILL_RUNNING:
            # [shutdown-flag-race] The Stop Trading button shows "Stopped"
            # whatever happens here; say so when that is not true.
            self.error.emit(
                "The engine did not stop -- it is still running after "
                "terminate and kill. See gui.log.")

    def pause_trading(self) -> None:
        """Pause trading by creating the signal file the engine watches.

        The engine continues running (market data, analytics, metrics)
        but skips Step 8 (offer posting) while the flag file exists.
        """
        flag_path = self._db_path.parent / "pause.flag"
        try:
            flag_path.parent.mkdir(parents=True, exist_ok=True)
            flag_path.touch(exist_ok=True)
            _log.info("Pause flag created at %s", flag_path)
        except OSError as exc:
            _log.error("Failed to create pause flag: %s", exc)
            self.error.emit(f"Could not pause trading: {exc}")

    @property
    def launched_config_path(self):
        """[RELOAD] The --config this bridge launched the engine with, or
        None when no engine was launched by this GUI (attached engines
        included). Lets the Save path be honest about whether a reload can
        reach the running process."""
        return getattr(self, "_launched_config_path", None)

    @property
    def engine_running_locally(self) -> bool:
        """True while a GUI-launched engine subprocess is alive."""
        proc = getattr(self, "_engine_process", None)
        return proc is not None and proc.poll() is None

    # -- [S74] the stop prompt's inputs -------------------------------------

    @property
    def engine_supports_keep_offers(self) -> bool:
        """True when the launched engine binary advertises the stop policy.

        The launch-time probe runs ``--help`` under a 2 s timeout, and a false
        "no" sends the operator straight back to hard-killing the engine to
        keep offers -- on exactly the overloaded machine (full blocks, a busy
        wallet) where a keep stop matters most. So a "no" is asked ONCE more,
        the first time anyone needs the answer. A second "no" stands.
        """
        if getattr(self, "_engine_keep_supported", False):
            return True
        path = getattr(self, "_engine_binary_path", None)
        if path is not None and not getattr(self, "_engine_keep_reprobed", False):
            self._engine_keep_reprobed = True
            self._engine_keep_supported = self._engine_supports_flag(
                path, stop_offers.ENGINE_HELP_TOKEN)
            if self._engine_keep_supported:
                _log.info(
                    "Engine %s advertises the stop policy after all (the "
                    "launch-time --help probe had timed out or failed).", path)
        return bool(getattr(self, "_engine_keep_supported", False))

    @property
    def stop_offers_default(self) -> str:
        """``engine.shutdown_offers`` as this GUI's config model has it -- what
        the prompt preselects. The ENGINE's loaded value is what actually
        applies to a stop with no policy; the two differ only when config.yaml
        changed after the engine started."""
        try:
            config = self._config_svc.get_full_config()
        except Exception:  # noqa: BLE001 -- a prompt default must never raise
            config = None
        return stop_offers.configured_default(config)

    def resting_offers_summary(self) -> Optional[stop_offers.RestingSummary]:
        """What offer_log says is resting right now, read-only and
        synchronously (one SELECT) for the stop prompt. None when the database
        cannot be read -- the prompt then says so."""
        rows = stop_offers.read_resting_rows(getattr(self, "_db_path", None))
        if rows is None:
            return None
        try:
            config = self._config_svc.get_full_config()
        except Exception:  # noqa: BLE001
            config = None
        return stop_offers.summarise_resting_offers(rows, config)

    def _policy_for_request(self, requested: Optional[str]) -> Optional[str]:
        """The policy that may be written for *requested*.

        [review #165] The capability is consulted ONLY for an actual keep:
        :attr:`engine_supports_keep_offers` may run a ``--help`` subprocess
        under a 2 s timeout, and a session-end close (no policy) or a cancel
        must not spend the OS's few shutdown seconds on a probe whose answer
        cannot matter."""
        if stop_offers.parse_policy(requested) != stop_offers.POLICY_KEEP:
            return stop_offers.policy_to_send(requested, keep_supported=False)
        return stop_offers.policy_to_send(
            requested, keep_supported=self.engine_supports_keep_offers)

    def set_close_offers_policy(self, policy: Optional[str]) -> None:
        """Hold the close prompt's answer for :meth:`shutdown`. ``"keep"`` is
        held only for an engine that can honour it."""
        self._close_offers_policy = self._policy_for_request(policy)

    def request_config_reload(self) -> None:
        """[RELOAD] Ask the running engine to re-read config.yaml.

        Same channel as the pause and the peg re-enable: a flag file beside
        the database, consumed and deleted by the engine on its next
        heartbeat. The engine applies pair DISABLES live (and cancels their
        resting offers); everything else it logs as restart-required. With
        no engine running the flag is consumed harmlessly at the next
        startup, when the file is the config being loaded anyway.
        """
        flag_path = self._db_path.parent / "config_reload.flag"
        try:
            flag_path.parent.mkdir(parents=True, exist_ok=True)
            with open(flag_path, "w", encoding="utf-8") as fh:
                fh.write("reload" + chr(10))
            _log.info("Config reload requested via %s", flag_path)
        except OSError as exc:
            _log.error("Could not write config reload flag %s: %s",
                       flag_path, exc)
            # A silent failure here would leave the Settings label promising
            # live application that cannot happen.
            self.error.emit(
                "Could not signal the engine to reload config -- the "
                "running engine will NOT pick up this save. See gui.log.")

    def reenable_peg(self, asset_id: str) -> None:
        """[PEGSUSPEND] Ask the engine to re-enable a suspended peg.

        Same channel as the pause: a flag file beside the database, one
        asset id per line, consumed and deleted by the engine on its next
        cycle. Appending rather than truncating lets two clicks in one
        cycle both land.
        """
        flag_path = self._db_path.parent / "peg_reenable.flag"
        try:
            flag_path.parent.mkdir(parents=True, exist_ok=True)
            with open(flag_path, "a", encoding="utf-8") as fh:
                fh.write(asset_id.strip() + "\n")
            _log.warning("Peg re-enable requested for %s via %s",
                         asset_id, flag_path)
        except OSError as exc:
            _log.error("Failed to write peg re-enable flag: %s", exc)

    def resume_trading(self) -> None:
        """Resume trading by removing the pause signal file."""
        flag_path = self._db_path.parent / "pause.flag"
        try:
            if flag_path.exists():
                flag_path.unlink()
                _log.info("Pause flag removed at %s", flag_path)
            else:
                _log.debug("Pause flag not present; nothing to remove.")
        except OSError as exc:
            _log.error("Failed to remove pause flag: %s", exc)
            self.error.emit(f"Could not resume trading: {exc}")

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

    #: Whether this bridge can actually command the running engine.
    #:
    #: True since the flag-file channel landed: start_engine/stop_engine
    #: manage a real subprocess, and pause/resume/reload/cancel-all are
    #: flag files the engine consumes -- so "off means flat" is a promise
    #: this bridge can keep.
    #:
    #: [S33 2026-09-05] It does NOT cover cancel_offer(): the single-offer
    #: path above is still a Phase-1 stub that submits nothing and only
    #: emits an error. This flag was written when everything was a stub and
    #: its old text ("False while start/stop/cancel are stubs", "the dexie
    #: switch reads this") outlived both facts -- the switch reads the
    #: engine's published gates, and today the only reader in the tree is
    #: tests/test_intent_switch.py, which pins the value. Callers that gain
    #: a use for it MUST consult it rather than checking that a method
    #: exists: every one of them does exist, and a stub call succeeds while
    #: doing nothing -- so a UI that probes with hasattr concludes it
    #: retracted a book it never touched.
    SUPPORTS_DIRECT_CONTROL: bool = True

    def cancel_all_offers(self) -> bool:
        """[STOPDRAIN] Ask the engine to cancel every resting offer NOW.

        Returns True only when the flag write succeeded.

        Same channel as pause/reload: a flag file beside the database,
        consumed on the engine's FAST poll path (not the slow heartbeat),
        which co-spawns the cancel. Submission is not confirmation -- the
        offers die when the cancel spends confirm on chain, and the TTL
        sweep keeps draining anything a failed cancel leaves behind.
        """
        flag_path = self._db_path.parent / "cancel_all.flag"
        try:
            flag_path.parent.mkdir(parents=True, exist_ok=True)
            with open(flag_path, "w", encoding="utf-8") as fh:
                fh.write("cancel_all" + chr(10))
            _log.warning("Cancel-all requested via %s", flag_path)
            return True
        except OSError as exc:
            _log.error("Could not write cancel-all flag %s: %s",
                       flag_path, exc)
            self.error.emit(
                "Could not signal the engine to cancel offers -- see "
                "gui.log. Resting offers still drain by TTL.")
            return False

    # -- [review #8] the cancel-all-pending latch survives GUI restarts ---
    def _cancel_all_pending_path(self):
        return self._db_path.parent / "cancel_all_pending.marker"

    def mark_cancel_all_pending(self) -> None:
        try:
            p = self._cancel_all_pending_path()
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("pending", encoding="utf-8")
        except OSError as exc:  # noqa: BLE001
            _log.warning("could not persist cancel-all latch: %s", exc)

    def clear_cancel_all_pending(self) -> None:
        try:
            self._cancel_all_pending_path().unlink(missing_ok=True)
        except OSError as exc:  # noqa: BLE001
            _log.warning("could not clear cancel-all latch: %s", exc)

    #: [R2 review] Cancel spends confirm or fail within ~1-2 blocks; a
    #: marker older than this outlived any possible confirm window and
    #: must not re-latch a fresh GUI forever.
    _CANCEL_ALL_MARKER_TTL_S = 15 * 60

    def cancel_all_pending(self) -> bool:
        try:
            p = self._cancel_all_pending_path()
            if not p.exists():
                return False
            import time
            if (time.time() - p.stat().st_mtime
                    > self._CANCEL_ALL_MARKER_TTL_S):
                _log.info("cancel-all marker outlived its confirm window "
                          "-- retiring it")
                self.clear_cancel_all_pending()
                return False
            return True
        except OSError:  # noqa: BLE001
            return False

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
            # The database path is config-relative and was resolved ONCE at
            # construction, so without this a switched config left _db_path
            # pointing into the previous config's directory -- handing
            # consumers a new config paired with the old database.
            self._apply_configured_database_path()
        if success:
            _log.info("Config path updated and reloaded.")
        else:
            self.error.emit(
                f"Could not load config from {resolved}. "
                "See logs for details."
            )

    @Slot(dict, dict, dict, dict)
    def apply_wallet_allocation_targets(
        self,
        asset_targets: dict[str, float],
        pair_targets: dict[str, float],
        asset_tolerances: dict[str, float] | None = None,
        pair_band_enters: dict[str, float] | None = None,
    ) -> None:
        """Persist wallet allocation targets to strategy config and reload.

        Parameters
        ----------
        asset_targets : dict[str, float]
            Asset symbol -> fraction of portfolio value (0..1).
        pair_targets : dict[str, float]
            Pair name -> base-value target ratio (0..1).
        asset_tolerances : dict[str, float] | None
            Asset symbol -> +/- tolerance as fraction (0..0.5).  Recorded
            on the strategy config for diagnostic purposes; the C++ engine
            consumes the derived per-pair bands below.
        pair_band_enters : dict[str, float] | None
            Pair name -> per-pair ratio-band-enter override (0..0.5).
            Replaces ``strategy.ratio_band_enter`` for that specific pair,
            implementing a deadband where the asset is "close enough" to
            its target and rebalancing is not influenced.
        """
        try:
            cfg = self._config_svc.get_full_config()
            strategy = cfg.setdefault("strategy", {})
            strategy["ratio_rebalance_enabled"] = True
            strategy["asset_target_allocations"] = {
                str(k): float(v) for k, v in asset_targets.items()
            }
            strategy["ratio_target_by_pair"] = {
                str(k): float(v) for k, v in pair_targets.items()
                if 0.0 < float(v) < 1.0
            }
            if asset_tolerances:
                strategy["asset_target_tolerances"] = {
                    str(k): float(v) for k, v in asset_tolerances.items()
                    if float(v) > 0.0
                }
            else:
                strategy.pop("asset_target_tolerances", None)
            if pair_band_enters:
                strategy["ratio_band_enter_by_pair"] = {
                    str(k): float(v) for k, v in pair_band_enters.items()
                    if 0.0 < float(v) < 0.5
                }
            else:
                strategy.pop("ratio_band_enter_by_pair", None)

            split_and_save(self._config_path, cfg)
            _log.info(
                "Applied wallet allocation targets: %d assets, %d pairs, "
                "%d pair band overrides",
                len(strategy["asset_target_allocations"]),
                len(strategy["ratio_target_by_pair"]),
                len(strategy.get("ratio_band_enter_by_pair", {})),
            )
            self.update_config_path(str(self._config_path))
        except Exception as exc:
            _log.error("Failed to apply wallet allocation targets: %s", exc)
            self.error.emit(
                f"Failed to apply wallet allocation targets: {exc}"
            )

    # ===================================================================
    # Internal slots
    # ===================================================================

    @Slot()
    def _on_master_tick(self) -> None:
        """Master refresh timer handler.

        Aggregates the latest service data and emits ``data_updated``.
        """
        self._tick_count += 1

        # Trigger an async wallet balance fetch every 6th tick (~30s at
        # 5s interval).  The RPC pass runs on WalletService's worker
        # thread; this call returns immediately and overlapping fetches
        # are skipped by the service.
        if self._tick_count % 6 == 1:
            self._wallet_svc.fetch_balances()

        # Refresh reports every 6th tick (~30s), offset from wallet.
        if self._tick_count % 6 == 3:
            self._database_svc.get_reports()

        # Advance the warp bridge one bounded step every 6th tick (~30s),
        # offset from wallet and reports.  The step is a cheap no-op when
        # warp is disabled or has no active job; overlapping ticks are
        # dropped by the service's in-flight guard.
        if self._tick_count % 6 == 5:
            self._warp_svc.tick()

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
        self._wallet_svc.update_config(config)
        self._warp_svc.update_config(config)

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

        # Some engine builds can temporarily report xop_node gauges as
        # zero even while the process is healthy and exporting metrics.
        # If the metrics endpoint is live, treat the bot as running.
        if self._metrics_svc.is_connected or node_synced >= 1.0 or wallet_connected >= 1.0:
            new_status = STATUS_RUNNING
        else:
            new_status = STATUS_STOPPED

        # Check if the engine is currently in the startup analysis phase.
        # Infer Analyzing from Prometheus metrics: blocks_target > 0 and
        # at least one pair has not yet completed its analysis window.
        if new_status == STATUS_RUNNING and self._metrics_svc.is_analysis_active():
            new_status = STATUS_ANALYZING

        # Any standing posting gate EXCEPT dry-run surfaces as Paused.
        # Dry-run is an operating mode, not a pause: folding it in showed a
        # healthy dry-run engine as "Paused (protection)".  The window
        # distinguishes WHO owns a pause via posting_gate_reasons().
        gate_reasons = self._metrics_svc.posting_gate_reasons() - {"dry_run"}
        if new_status in (STATUS_RUNNING, STATUS_ANALYZING) and gate_reasons:
            new_status = STATUS_PAUSED

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

    def _engine_binary_candidates(self) -> list[Path]:
        """Return ordered engine binary candidates.

        Candidates are ordered by preference. Only existing files are
        included in the returned list.
        """
        engine_name = "xop_trader.exe" if sys.platform == "win32" else "xop_trader"

        # Optional override for power users and troubleshooting.
        override_path = os.environ.get("XOP_ENGINE_PATH", "").strip()
        if override_path:
            candidate = Path(override_path).resolve()
            if candidate.is_file():
                return [candidate]

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
            cwd = Path.cwd()
            # Prefer the build output directory so a stale root-level
            # copy is never chosen over a freshly-built binary.
            candidates.append(cwd / "cpp" / "build" / "Release" / engine_name)
            candidates.append(cwd / "cpp" / "build" / "Debug" / engine_name)
            candidates.append(cwd / "cpp" / "build" / engine_name)
            # Fallback: root-level copy (e.g. deployed / manually placed).
            candidates.append(cwd / engine_name)

        return [candidate for candidate in candidates if candidate.is_file()]

    def _find_engine_binary(self) -> Path | None:
        """Locate the preferred C++ engine binary.

        When running from a PyInstaller bundle the binary is expected
        next to ``sys.executable``.  For one-file bundles, fallback to
        ``sys._MEIPASS`` where PyInstaller extracts bundled binaries.
        In development mode the current working directory is checked.
        """
        candidates = self._engine_binary_candidates()
        if not candidates:
            return None
        return candidates[0]

    def _is_engine_reachable(self) -> bool:
        """Return True if the Prometheus metrics endpoint already responds.

        Prevents spawning a duplicate engine when one is already
        running (e.g. started manually or by a service manager).

        T8-08: Uses a short timeout (0.5 s) for the probe so the
        main-thread GUI event loop is not blocked noticeably.
        """
        import requests as _req  # noqa: WPS433 — keep top-level imports light

        try:
            resp = _req.get(self._metrics_url, timeout=0.5)
            return resp.status_code == 200
        except Exception:
            return False

    def _start_engine_process(self) -> bool:
        """Launch the C++ engine as a child process.

        Engine stdout/stderr is redirected to ``engine.log`` next to
        the binary so users can inspect output without a console window.
        Returns *True* on success.
        """
        engine_candidates = self._engine_binary_candidates()
        if not engine_candidates:
            return False

        launch_args: list[str] = []
        if self._config_path.is_file():
            launch_args.extend(["--config", str(self._config_path)])

        for engine_path in engine_candidates:
            candidate_args = list(launch_args)

            # Pass secrets file only when the selected engine supports it.
            secrets_path = self._config_path.parent / "secrets.yaml"
            if secrets_path.is_file() and self._engine_supports_flag(engine_path, "--secrets"):
                candidate_args.extend(["--secrets", str(secrets_path)])

            # [S74] Same probe, for the stop policy: only an engine whose
            # --help carries the token reads "offers=keep". Asked BEFORE the
            # launch -- --help returns before main() touches anything.
            keep_supported = self._engine_supports_flag(
                engine_path, stop_offers.ENGINE_HELP_TOKEN)

            cmd: list[str] = [str(engine_path), *candidate_args]
            try:
                launch_dir = self._determine_engine_launch_dir(engine_path)
                self._ensure_engine_runtime_dirs(launch_dir)

                # Frozen: the log goes into the launch dir (the writable
                # per-user data home). It used to go next to the GUI
                # executable -- Program Files for an installed copy -- where
                # open() raised PermissionError and the ONLY engine candidate
                # was abandoned: the exact "engine never started" failure of
                # the v0.9.x installers. Never _MEIPASS either (ephemeral).
                if getattr(sys, "frozen", False):
                    log_path = launch_dir / "engine.log"
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
                # [RELOAD] Remember which config THIS engine actually
                # runs, so a later Save can tell whether it reaches it.
                self._launched_config_path = (
                    self._config_path if self._config_path.is_file() else None
                )
                self._engine_keep_supported = keep_supported
                self._engine_binary_path = engine_path
                self._engine_keep_reprobed = False
                if not keep_supported:
                    _log.warning(
                        "Engine %s did not advertise the stop policy in "
                        "--help: unless a second probe says otherwise, a stop "
                        "can only CANCEL its offers and the stop prompt will "
                        "not offer Keep.", engine_path)
                QTimer.singleShot(3_000, self._check_engine_startup_result)
                return True
            except Exception:
                _log.exception(
                    "Failed to start C++ engine subprocess with candidate: %s",
                    engine_path,
                )
                if self._engine_log_fh is not None:
                    self._engine_log_fh.close()
                    self._engine_log_fh = None
                self._engine_log_path = None
                self._engine_launch_dir = None
                self._engine_process = None
                self._engine_keep_supported = False
                self._engine_binary_path = None

        return False

    def _engine_supports_flag(self, engine_path: Path, flag: str) -> bool:
        """Return True if the engine binary advertises a CLI flag in --help."""
        try:
            kwargs: dict[str, Any] = {
                "stdout": subprocess.PIPE,
                "stderr": subprocess.STDOUT,
                "text": True,
                "timeout": 2,
            }
            if sys.platform == "win32":
                kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW

            result = subprocess.run([str(engine_path), "--help"], **kwargs)
            output = result.stdout or ""
            return flag in output
        except Exception:
            return False

    def _stop_engine_process(
        self,
        offers_policy: Optional[str] = None,
    ) -> Optional[shutdown_flag.StopOutcome]:
        """Stop the managed engine subprocess and report truthfully how it ended.

        [S74] *offers_policy* (``"cancel"``/``"keep"``/None) is written into
        the stop request as its ``offers=`` line; None writes none and the
        engine applies its own ``engine.shutdown_offers``. A ``"keep"`` for an
        engine that does not advertise the stop policy is withheld
        (``stop_offers.policy_to_send``) and said so.

        [review #7] Windows terminate() is a hard kill the engine never sees
        -- closing the GUI mid-drain left the book resting unmanaged -- so a
        GRACEFUL stop is requested first through shutdown.flag, and
        terminate/kill are only the escalation.

        [shutdown-flag-race 2026-09-12] The request is addressed to this
        engine's PID and written atomically. The outcome is classified from
        what can be observed -- the exit code, and whether the engine removed
        the flag -- instead of assumed. At 22:41:14.587 this method reported a
        graceful exit 6 ms after another GUI had terminated the engine, and it
        reported a clean exit after its own TerminateProcess. A request the
        engine never removed is deleted here once the engine is gone, so no
        later engine can inherit it.

        [review] For the whole stop this GUI also keeps the stop marker beside
        shutdown.flag, and removes it however the stop ends, exceptions
        included. The engine consumes the flag within one poll and only then
        cancels its book, so without the marker a GUI relaunched mid-cancel
        (gui/main.py _await_stop_in_progress) would see no stop under way and
        terminate the engine and this GUI at once.

        Returns the outcome, or None when no process was being managed. On
        STILL_RUNNING the process handle is kept: clearing it would let
        start_engine() launch a second engine beside the live one.
        """
        proc = self._engine_process
        if proc is None:
            return None

        outcome: Optional[shutdown_flag.StopOutcome] = None
        if proc.poll() is None:
            pid = proc.pid
            flag = self._db_path.parent / shutdown_flag.FLAG_NAME
            marker: Optional[Path] = shutdown_flag.stop_marker_path(flag)
            try:
                shutdown_flag.write_shutdown_request(marker, pid)
            except (OSError, ValueError) as exc:
                marker = None
                _log.warning(
                    "Could not write %s (%s); a GUI launched after the engine "
                    "consumes shutdown.flag will not wait for this stop.",
                    shutdown_flag.STOP_MARKER_NAME, exc)
            policy = self._policy_for_request(offers_policy)
            if (stop_offers.parse_policy(offers_policy) == stop_offers.POLICY_KEEP
                    and policy is None):
                _log.error(
                    "Keep was requested for engine PID %d, but this engine "
                    "build does not advertise the stop policy: the request "
                    "carries NO offers line and that engine will CANCEL its "
                    "book.", pid)
            try:
                outcome = self._stop_running_engine(proc, pid, flag, policy)
            finally:
                if marker is not None:
                    shutdown_flag.remove_if_addressed_to(marker, pid)
            if outcome is shutdown_flag.StopOutcome.STILL_RUNNING:
                return outcome
        else:
            _log.info("Engine process already exited (rc=%s).", proc.returncode)

        self._engine_process = None
        if self._engine_log_fh is not None:
            self._engine_log_fh.close()
            self._engine_log_fh = None
        self._engine_log_path = None
        self._engine_launch_dir = None
        self._engine_keep_supported = False
        self._engine_binary_path = None
        return outcome

    def _stop_running_engine(
        self,
        proc: subprocess.Popen,
        pid: int,
        flag: Path,
        offers_policy: Optional[str] = None,
    ) -> shutdown_flag.StopOutcome:
        """Request a graceful stop of *proc*, escalate if needed, and classify it.

        Writes the addressed request, waits, terminates and kills as needed,
        logs the one outcome line, and removes a request the engine never
        consumed once it is gone. The caller owns the stop marker and the
        process handle.
        """
        forced = False
        request_written = False
        if offers_policy == stop_offers.POLICY_KEEP:
            intent = "leave its offers RESTING (offers=keep) and exit"
        elif offers_policy == stop_offers.POLICY_CANCEL:
            intent = "cancel its book (offers=cancel) and exit"
        else:
            intent = ("apply its own engine.shutdown_offers to its book (no "
                      "offers line) and exit")
        try:
            shutdown_flag.write_shutdown_request(
                flag, pid, offers_policy=offers_policy)
            request_written = True
            _log.info(
                "Wrote shutdown.flag addressed to engine PID %d; waiting up "
                "to %d s for it to %s.",
                pid, _GRACEFUL_STOP_WAIT_S, intent)
            proc.wait(timeout=_GRACEFUL_STOP_WAIT_S)
        except subprocess.TimeoutExpired:
            forced = True
            _log.warning(
                "Engine PID %d did not exit within %d s of shutdown.flag; "
                "terminating.", pid, _GRACEFUL_STOP_WAIT_S)
        except (OSError, ValueError) as exc:
            forced = True
            _log.warning(
                "Could not write shutdown.flag (%s); terminating engine "
                "PID %d.", exc, pid)

        if forced:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                _log.warning(
                    "Engine PID %d did not exit within 10 s of terminate; "
                    "killing it.", pid)
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass  # still running: classified STILL_RUNNING below

        outcome = shutdown_flag.classify_stop_outcome(
            proc.returncode,
            request_written=request_written,
            flag_still_names_target=shutdown_flag.flag_names_pid(flag, pid),
            forced=forced,
        )
        self._log_stop_outcome(outcome, pid, proc.returncode)

        if (shutdown_flag.outcome_leaves_undelivered_flag(outcome)
                and shutdown_flag.remove_if_addressed_to(flag, pid)):
            _log.warning(
                "Removed the undelivered shutdown.flag addressed to engine "
                "PID %d so no later engine can inherit it.", pid)
        return outcome

    @staticmethod
    def _log_stop_outcome(
        outcome: shutdown_flag.StopOutcome,
        pid: int,
        returncode: Optional[int],
    ) -> None:
        """Exactly one line per stop, saying only what was observed.

        "shutdown.flag was removed" means the ENGINE removed it -- consumed, or
        discarded as not addressed to it -- which is why no line but the rc=0
        one claims a graceful exit.
        """
        kinds = shutdown_flag.StopOutcome
        if outcome is kinds.GRACEFUL:
            _log.info(
                "Engine exited gracefully (PID %d removed shutdown.flag and "
                "exited rc=0).", pid)
        elif outcome is kinds.EXITED_WITHOUT_CONSUMING:
            _log.warning(
                "Engine PID %d exited (rc=%s) but shutdown.flag still names it: "
                "the request was NOT consumed, so this is NOT a confirmed "
                "graceful exit. Most likely something else stopped it (a newly "
                "launched GUI or engine terminating old instances, taskkill, a "
                "crash); only if engine.log says 'could not remove "
                "shutdown.flag' did the engine honour it. The next engine "
                "reconciles the book.", pid, returncode)
        elif outcome is kinds.FLAG_GONE_ABNORMAL_EXIT:
            _log.warning(
                "Engine PID %d exited with rc=%s after shutdown.flag was removed "
                "by the engine (consumed, or discarded as not addressed to it -- "
                "see engine.log) -- NOT a clean shutdown; the next engine "
                "verifies any cancel intent it left (data/uncancelled.txt).",
                pid, returncode)
        elif outcome is kinds.TERMINATED_BEFORE_CONSUMING:
            _log.warning(
                "Engine PID %d was terminated (rc=%s) before it consumed "
                "shutdown.flag -- NOT a graceful exit; this stop cancelled "
                "nothing.", pid, returncode)
        elif outcome is kinds.TERMINATED_AFTER_CONSUMING:
            _log.warning(
                "Engine PID %d had not exited within %d s and was terminated "
                "(rc=%s); shutdown.flag had been removed by the engine "
                "(consumed, or discarded as not addressed to it -- see "
                "engine.log) -- NOT a graceful exit: the book may be partly "
                "cancelled, and the next engine verifies the cancel intent "
                "(data/uncancelled.txt).", pid, _GRACEFUL_STOP_WAIT_S,
                returncode)
        elif outcome is kinds.TERMINATED_WITHOUT_REQUEST:
            _log.warning(
                "Engine PID %d was terminated (rc=%s) without a graceful "
                "request.", pid, returncode)
        else:
            _log.error(
                "Engine PID %d is still running after terminate and kill; "
                "shutdown.flag is left in place for it and its process handle "
                "is kept.", pid)

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
        self._database_svc.last_trade_prices_loaded.connect(self._on_last_trade_prices)
        self._database_svc.query_error.connect(self._on_service_error)
        # The replaced service's price cache may describe a different DB;
        # invalidate so the next tick re-queries the new database.
        self._last_trade_cache_ts = 0.0
        self._last_trade_request_ts = 0.0

    def _resolve_config_relative_path(self, raw_path: str) -> Path:
        """Resolve config paths relative to the config file directory."""
        candidate = Path(raw_path).expanduser()
        if candidate.is_absolute():
            return candidate.resolve()
        return (self._config_path.parent / candidate).resolve()

    def _determine_engine_launch_dir(self, engine_path: Path) -> Path:
        """Return the working directory to use for the engine process.

        The engine writes ``data/`` and ``logs/`` relative to its CWD, so
        the CWD must be writable. Config parent when a config exists (the
        per-user data dir for installed builds); otherwise the engine's own
        directory in dev, but never Program Files when frozen."""
        if self._config_path.is_file():
            return self._config_path.parent
        if getattr(sys, "frozen", False):
            from gui.utils import user_data_dir

            return user_data_dir()
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
