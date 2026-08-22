"""Main application window for XOPTrader -- CHIA DEX Market Maker.

Assembles the menu bar, toolbar, sidebar, stacked content area,
bottom tab panel, and status bar into a cohesive dark-themed layout
with CHIA green accents.

Compliant with:
    - ISO/IEC 27001:2022  (no credential storage in UI layer)
    - ISO/IEC 5055       (bounded timers, no resource leaks)
    - ISO/IEC 25000      (keyboard shortcuts, geometry persistence)
"""

from __future__ import annotations

from collections import deque
from datetime import datetime
import logging
import time
from typing import Any, Final, Optional

from PySide6.QtCore import QSettings, QSize, Qt, QTimer
from PySide6.QtGui import QAction, QCloseEvent, QKeySequence
from PySide6.QtWidgets import (
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMenuBar,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSplitter,
    QStackedWidget,
    QTabWidget,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

# -- Local widgets ----------------------------------------------------------
from gui.widgets.sidebar import Sidebar
from gui.widgets.status_bar import StatusBar

# -- Placeholder imports for widgets that will be created later -------------
# Each of these will live in gui/widgets/<name>.py once implemented.
# Import guards let the window load even if the files are not yet present.
try:
    from gui.widgets.dashboard import (
        _ACTIVITY_FEED_MAX,
        DashboardWidget,
    )
except ImportError:
    DashboardWidget = None  # type: ignore[assignment,misc]
    _ACTIVITY_FEED_MAX = 20  # widget unavailable; keep the module importable

try:
    from gui.widgets.chart import ChartWidget
except ImportError:
    ChartWidget = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.order_panel import OrderPanel
except ImportError:
    OrderPanel = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.settings import SettingsWidget
except ImportError:
    SettingsWidget = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.order_book import OrderBookWidget
except ImportError:
    OrderBookWidget = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.trade_log import TradeLogWidget
except ImportError:
    TradeLogWidget = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.bot_log import BotLogWidget
except ImportError:
    BotLogWidget = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.market_analysis import MarketAnalysisWidget
except ImportError:
    MarketAnalysisWidget = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.wallet_balances import WalletBalancesWidget
except ImportError:
    WalletBalancesWidget = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.reports import ReportsWidget
except ImportError:
    ReportsWidget = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.warp import WarpWidget
except ImportError:
    WarpWidget = None  # type: ignore[assignment,misc]

try:
    from gui.widgets.base_wallet import BaseWalletWidget
except ImportError:
    BaseWalletWidget = None  # type: ignore[assignment,misc]

# ---------------------------------------------------------------------------
# Theme constants -- sourced from the canonical CHIA palette singleton.
# ---------------------------------------------------------------------------
from gui.theme import COLORS as _C
from gui.utils import (
    MOJOS_PER_XCH,
    format_price,
    mojos_per_unit_for_pair,
    mojos_to_xch,
    mojos_to_xch_float,
)

#: Rows converted for the dashboard's RECENT ACTIVITY feed.  Taken from the
#: widget's own cap rather than restated: a separate 25 here meant five rows
#: were built and then deterministically discarded by update_trades(), and
#: the documented count did not match what the panel rendered.
_ACTIVITY_FEED_ROWS: Final[int] = _ACTIVITY_FEED_MAX

PRIMARY_GREEN: Final[str] = _C.PRIMARY_GREEN
LIGHT_GREEN: Final[str] = _C.LIGHT_GREEN
DARK_BG: Final[str] = _C.DARK_BG
PANEL_BG: Final[str] = _C.PANEL_BG
ELEVATED_BG: Final[str] = _C.ELEVATED_BG
BORDER: Final[str] = _C.BORDER
TEXT_PRIMARY: Final[str] = _C.TEXT_PRIMARY
TEXT_SECONDARY: Final[str] = _C.TEXT_SECONDARY
PROFIT_GREEN: Final[str] = _C.PROFIT_GREEN
LOSS_RED: Final[str] = _C.LOSS_RED

# Module-level logger (ISO/IEC 5055 -- observable error handling)
_log = logging.getLogger(__name__)

# QSettings keys
_ORG_NAME: Final[str] = "XOPTrader"
_APP_NAME: Final[str] = "XOPTrader-GUI"
_KEY_GEOMETRY: Final[str] = "mainwindow/geometry"
_KEY_STATE: Final[str] = "mainwindow/state"
_KEY_SPLITTER: Final[str] = "mainwindow/splitter"

# Timer intervals (milliseconds)
_STATUS_INTERVAL_MS: Final[int] = 1_000
_METRICS_INTERVAL_MS: Final[int] = 5_000

# Default window dimensions
_DEFAULT_WIDTH: Final[int] = 1400
_DEFAULT_HEIGHT: Final[int] = 900

# Stacked-widget page indices — must match the order widgets are added in
# _build_central_area().
_PAGE_DASHBOARD: Final[int] = 0
_PAGE_CHARTS: Final[int] = 1
_PAGE_ORDERS: Final[int] = 2
_PAGE_ORDER_BOOK: Final[int] = 3
_PAGE_ANALYSIS: Final[int] = 4
_PAGE_WALLET: Final[int] = 5
_PAGE_REPORTS: Final[int] = 6
_PAGE_WARP: Final[int] = 7
_PAGE_BASE_WALLET: Final[int] = 8
_PAGE_SETTINGS: Final[int] = 9


def _fmt_usd(value: float) -> str:
    """Format a USD amount with an explicit sign for positive values."""
    sign = "+" if value > 0 else ""
    return f"{sign}${value:,.2f}"


def _placeholder_widget(label: str) -> QWidget:
    """Create a simple centred-label placeholder for unimplemented pages.

    Parameters
    ----------
    label : str
        Text to display inside the placeholder.

    Returns
    -------
    QWidget
        Styled placeholder widget.
    """
    widget = QWidget()
    layout = QVBoxLayout(widget)
    lbl = QLabel(label)
    lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
    lbl.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 16px;")
    layout.addWidget(lbl)
    return widget


def _pnl_usd_sort_key(pnl_quote: float, quote_symbol: str,
                      xch_usd: float) -> Optional[float]:
    """P&L converted to USD for SORTING, or None when the rate is unknown.

    wUSDC.b/wUSDC are $1 by construction; XCH uses the live rate when one
    exists.  Everything else returns None rather than a guess -- an unknown
    sorts to the bottom, it does not masquerade as a small number.
    """
    symbol = (quote_symbol or "").strip().upper()
    if symbol in ("WUSDC.B", "WUSDC"):
        return pnl_quote
    if symbol == "XCH" and xch_usd > 0:
        return pnl_quote * xch_usd
    return None


def activity_event(trade: dict) -> Optional[dict]:
    """One activity-feed event from a ``trade_log`` row, or None if unusable.

    Amounts go through the same helpers the Trade Log uses rather than being
    re-derived: ``price_mojos`` is scaled by 10^12 for EVERY pair, while
    ``size_mojos`` uses the pair's own base units, and duplicating that rule
    is how two views of one fill drift apart.

    Returns None for a row that cannot be rendered; one malformed row must
    not empty the whole feed.
    """
    pair = str(trade.get("pair_name", "") or "")
    side = str(trade.get("side", "") or "").lower()
    try:
        price = format_price(int(trade.get("price_mojos", 0) or 0), pair)
        size = mojos_to_xch(
            int(trade.get("size_mojos", 0) or 0),
            mojos_per_unit=mojos_per_unit_for_pair(pair, "base"),
        )
    except (TypeError, ValueError, KeyError):
        return None
    stamp = str(trade.get("timestamp", "") or "")
    # trade_log stamps are UTC ISO ("2026-08-21T16:54:15.943Z").  Convert to
    # LOCAL time before display: every other clock on the dashboard is local,
    # and a bare "16:54:15" that is five hours off the status line reads as a
    # different (wrong) event time, not as a timezone.
    clock = stamp
    try:
        parsed = datetime.fromisoformat(stamp.replace("Z", "+00:00"))
        if parsed.tzinfo is not None:
            clock = parsed.astimezone().strftime("%H:%M:%S")
        elif len(stamp) >= 19:
            clock = stamp[11:19]      # naive stamp: display as recorded
    except ValueError:
        if len(stamp) >= 19:
            clock = stamp[11:19]
    return {
        "timestamp": clock,
        "icon": "▲" if side == "bid" else "▼",
        "message": f"{side.upper():4s} {size} @ {price}  {pair}",
    }


class MainWindow(QMainWindow):
    """Top-level application window for the XOPTrader GUI.

    Parameters
    ----------
    config_service : object | None
        Service providing configuration read/write (injected dependency).
    metrics_service : object | None
        Service providing live metrics from Prometheus (injected dependency).
    db_service : object | None
        Service providing trade / offer database access (injected dependency).
    parent : QWidget | None
        Parent widget.
    """

    def __init__(
        self,
        config_service: Optional[Any] = None,
        metrics_service: Optional[Any] = None,
        db_service: Optional[Any] = None,
        dry_run: bool = False,
        parent: Optional[QWidget] = None,
    ) -> None:
        super().__init__(parent)

        # -- Service references (used by child widgets / timers) ------------
        self.config_service = config_service
        self.metrics_service = metrics_service
        self.db_service = db_service
        self._bridge: Optional[Any] = None  # Set via set_bridge()
        # Dashboard per-pair table inputs: our own book/fills from the DB,
        # plus the latest live snapshot to pair them with.
        self._pair_summary: dict = {}
        self._last_market_data: dict = {}
        self._last_xch_usd: float = 0.0
        # Whether the metrics scrape is currently live.  Gauge-derived
        # columns are withheld when it is not; wallet- and DB-derived ones
        # remain valid because they do not come through Prometheus.
        self._metrics_live: bool = True

        # -- Runtime state --------------------------------------------------
        self._connected: bool = False
        self._bot_running: bool = False
        self._bot_paused: bool = False
        # True when the current Paused status is owned by the GUI flag (the
        # pause Resume can clear), False when a risk breaker holds it.
        self._gui_pause_owns_it: bool = True
        self._dry_run: bool = dry_run
        self._start_time: float = time.monotonic()
        self._last_engine_start_failure: str = ""
        self._chart_last_block_by_pair: dict[str, int] = {}
        self._chart_volume_by_pair_block: dict[tuple[str, int], tuple[float, float]] = {}
        self._chart_seen_trade_keys: set[tuple] = set()
        self._chart_seen_trade_fifo: deque[tuple] = deque(maxlen=10_000)
        # [PNL-DISPLAY 2026-08-02] Latest restart-proof P&L figures derived
        # from trade_log by DatabaseService (lifetime + trailing 24h).
        # Empty until the first pnl_display_loaded signal arrives.
        self._pnl_display: dict[str, Any] = {}

        # -- Child widget references (populated in _build_central_area) -----
        self._dashboard: Optional[QWidget] = None
        self._chart: Optional[QWidget] = None
        self._order_panel: Optional[QWidget] = None
        self._order_book: Optional[QWidget] = None
        self._market_analysis: Optional[QWidget] = None
        self._wallet_balances: Optional[QWidget] = None
        self._reports: Optional[QWidget] = None
        self._settings_widget: Optional[QWidget] = None
        self._trade_log: Optional[QWidget] = None
        self._bot_log: Optional[QWidget] = None
        self._tab_order_panel: Optional[QWidget] = None
        self._warp_widget: Optional[QWidget] = None
        self._base_wallet_widget: Optional[QWidget] = None

        # -- Settings persistence -------------------------------------------
        self._settings = QSettings(_ORG_NAME, _APP_NAME)

        # -- Build the full UI hierarchy ------------------------------------
        self.setWindowTitle("XOPTrader \u2014 CHIA DEX Market Maker")
        self.resize(_DEFAULT_WIDTH, _DEFAULT_HEIGHT)

        self._build_menu_bar()
        self._build_toolbar()
        self._build_central_area()
        self._build_status_bar()
        # NOTE: Global stylesheet is applied by gui/app.py via theme.py.
        # Duplicate stylesheet was removed to avoid conflicts (font-family
        # order, scrollbar width).
        self._setup_keyboard_shortcuts()
        self._setup_timers()

        # Restore persisted geometry & splitter state
        self._restore_state()

    # ===================================================================== #
    #  Bridge / service wiring                                               #
    # ===================================================================== #

    def set_bridge(self, bridge: Any) -> None:
        """Inject the EngineBridge and wire all service signals to widgets.

        Parameters
        ----------
        bridge : EngineBridge
            Unified service facade owning config, metrics, and database
            services.
        """
        self._bridge = bridge
        self.config_service = bridge.config_service
        self.metrics_service = bridge.metrics_service
        self.db_service = bridge.database_service

        # -- Bridge-level signals ------------------------------------------
        bridge.data_updated.connect(self._on_bridge_data)
        bridge.bot_status_changed.connect(self._on_bot_status_changed)
        bridge.error.connect(self._on_bridge_error)
        if hasattr(bridge, "engine_start_failed"):
            bridge.engine_start_failed.connect(self._on_engine_start_failed)

        # -- Database -> widget signals ------------------------------------
        db = bridge.database_service
        if self._order_panel is not None and hasattr(db, "offers_loaded"):
            db.offers_loaded.connect(self._order_panel.update_offers)
        if self._tab_order_panel is not None and hasattr(db, "offers_loaded"):
            db.offers_loaded.connect(self._tab_order_panel.update_offers)
        # [ORDERS-PERF] The Orders panel pushes its pair/status filters down
        # into SQL so the worker thread does the filtering and only a capped
        # slice ever reaches the GUI thread; whole-table counts for its
        # summary bar come from a separate aggregate query.
        for _panel in (self._order_panel, self._tab_order_panel):
            if _panel is None:
                continue
            if hasattr(_panel, "offers_query_requested") and hasattr(db, "query_offers"):
                _panel.offers_query_requested.connect(db.query_offers)
            if hasattr(_panel, "update_offer_summary") and hasattr(
                db, "offer_summary_loaded"
            ):
                db.offer_summary_loaded.connect(_panel.update_offer_summary)
        if self._trade_log is not None and hasattr(db, "trades_loaded"):
            db.trades_loaded.connect(self._trade_log.load_trades)
        if self._chart is not None and hasattr(db, "trades_loaded"):
            db.trades_loaded.connect(self._on_trades_for_chart)
        # The dashboard's RECENT ACTIVITY feed.  It was built but never
        # connected to anything, so it could not display a single row.
        if hasattr(db, "trades_loaded"):
            db.trades_loaded.connect(self._on_trades_for_activity)
        # Per-pair table: OUR resting book and OUR fills.
        if hasattr(db, "pair_summary_loaded"):
            db.pair_summary_loaded.connect(self._on_pair_summary)
        if hasattr(db, "query_pair_summary"):
            ttl_blocks = 0
            try:
                cfg = bridge.config_service.get_full_config() or {}
                ttl_blocks = int(
                    (cfg.get("strategy") or {}).get("offer_ttl_blocks", 0) or 0
                )
            except Exception:      # config not loaded yet; the default applies
                ttl_blocks = 0
            db.query_pair_summary(ttl_blocks)
        # [DEPLOYED 2026-08-04] Per-asset resting-offer amounts for the
        # Balances tab's Deployed % column and summary line.
        _wallet_widget = self._unwrap(self._wallet_balances)
        if (_wallet_widget is not None
                and hasattr(_wallet_widget, "update_deployed")
                and hasattr(db, "deployed_loaded")):
            db.deployed_loaded.connect(_wallet_widget.update_deployed)

        # [PNL-DISPLAY 2026-08-02] Restart-proof P&L: headline figures and
        # chart history are derived from the persistent DB, not from GUI
        # memory or the engine's since-boot gauges.
        if hasattr(db, "pnl_display_loaded"):
            db.pnl_display_loaded.connect(self._on_pnl_display)
        if hasattr(db, "pnl_history_loaded"):
            db.pnl_history_loaded.connect(self._on_pnl_history)

        # Kick off the initial offers query so the auto-refresh loop
        # has a set of parameters to re-issue on subsequent ticks.
        if hasattr(db, "query_offers"):
            db.query_offers()
        if hasattr(db, "query_offer_summary"):
            # Whole-table offer counts for the Orders panel summary bar
            # (its row payload is a capped, status-filtered slice).
            db.query_offer_summary()
        if hasattr(db, "query_trades"):
            # Seed trade auto-refresh so charts can build per-block
            # volume/fill overlays from existing DB snapshots.
            db.query_trades(limit=1000)
        if hasattr(db, "query_pnl_display"):
            # Headline P&L (lifetime + 24h), honouring any display reset
            # baseline the user persisted in Settings.
            db.query_pnl_display(self._read_pnl_baseline())
        if hasattr(db, "query_pnl_history"):
            # Rebuild the chart's P&L curve from the DB (90-day window)
            # so it survives GUI restarts.
            db.query_pnl_history(90)
        if hasattr(db, "query_deployed_capital"):
            # Seed the Balances tab's deployed-capital figures; the
            # auto-refresh loop re-issues this on every DB tick.
            db.query_deployed_capital()

        # -- Reports widget signal -----------------------------------------
        reports_widget = self._unwrap(self._reports)
        if reports_widget is not None and hasattr(db, "reports_loaded"):
            db.reports_loaded.connect(reports_widget.update_reports)

        # -- Order book data signals ---------------------------------------
        if self._order_book is not None:
            if hasattr(db, "offers_loaded"):
                db.offers_loaded.connect(self._on_offers_for_order_book)
            # Feed aggregated market data snapshots to the order book widget
            # on every bridge refresh tick.
            bridge.data_updated.connect(self._on_bridge_data_for_order_book)

        # -- Widget -> bridge command signals ------------------------------
        if self._order_panel is not None:
            self._order_panel.cancel_offer_requested.connect(bridge.cancel_offer)
            self._order_panel.cancel_all_requested.connect(bridge.cancel_all_offers)
        if self._tab_order_panel is not None:
            self._tab_order_panel.cancel_offer_requested.connect(bridge.cancel_offer)
            self._tab_order_panel.cancel_all_requested.connect(bridge.cancel_all_offers)
        if self._settings_widget is not None and hasattr(self._settings_widget, "config_saved"):
            self._settings_widget.config_saved.connect(bridge.update_config_path)
            # Connected AFTER update_config_path so it runs once the bridge
            # has switched: saving a config from Settings changes where the
            # advisory calculator must read, and a one-time snapshot taken at
            # set_bridge() would keep pointing at the previous file.
            self._settings_widget.config_saved.connect(
                lambda *_: self._push_sizing_paths()
            )
        wallet_widget = self._unwrap(self._wallet_balances)
        if (wallet_widget is not None
            and hasattr(wallet_widget, "allocation_targets_applied")
            and hasattr(bridge, "apply_wallet_allocation_targets")):
            wallet_widget.allocation_targets_applied.connect(
                bridge.apply_wallet_allocation_targets
            )

        # -- Warp widget -> WarpService command signals --------------------
        # "Bridge now" and the per-job Retry/Sweep/Cancel context menu drive the
        # background bridge worker. Guarded so a build without the warp service
        # (or with the widget stubbed to None) simply skips the wiring.
        warp_widget = self._unwrap(self._warp_widget)
        warp_svc = getattr(bridge, "warp_service", None)
        if warp_widget is not None and warp_svc is not None:
            if hasattr(warp_widget, "bridge_now_requested"):
                warp_widget.bridge_now_requested.connect(warp_svc.request_bridge)
            if hasattr(warp_widget, "unwrap_requested"):
                warp_widget.unwrap_requested.connect(warp_svc.request_unwrap)
            if hasattr(warp_widget, "job_action_requested"):
                warp_widget.job_action_requested.connect(warp_svc.job_action)

        # -- Base Wallet widget -> WarpService wallet commands -------------
        # Create / confirm-backup / send / rotate run on the same warp worker
        # thread (the wallet is the bridge's hot wallet); guarded identically.
        base_wallet_widget = self._unwrap(self._base_wallet_widget)
        if base_wallet_widget is not None and warp_svc is not None:
            if hasattr(base_wallet_widget, "wallet_action_requested"):
                base_wallet_widget.wallet_action_requested.connect(
                    warp_svc.wallet_action
                )
            if (hasattr(base_wallet_widget, "present_key_backup")
                    and hasattr(warp_svc, "key_backup_ready")):
                warp_svc.key_backup_ready.connect(
                    base_wallet_widget.present_key_backup
                )

        # Auto-populate the settings panel from the bridge's config file so
        # users can edit credentials without touching the file system manually.
        settings = self._unwrap(self._settings_widget)
        if settings is not None and hasattr(settings, "pnl_baseline_changed"):
            # Re-query the DB-derived P&L display whenever the user resets
            # or clears the display baseline on the Settings page.
            settings.pnl_baseline_changed.connect(self._on_pnl_baseline_changed)
        # Unconditionally, BEFORE the Settings-widget branch below: the
        # wallet page needs these paths too, and SettingsWidget is a guarded
        # import.  Nesting this inside that branch meant a failed Settings
        # import silently left the calculator on its bundle-relative
        # defaults -- exactly the fault this change exists to remove.
        self._push_sizing_paths()

        if settings is not None and hasattr(settings, "load_config"):
            cfg_path = bridge.config_service.path
            if cfg_path.is_file():
                settings.load_config(str(cfg_path))
            else:
                # No config file loaded — guide the user to the Settings tab
                # so they can configure credentials before starting the engine.
                self._switch_page(_PAGE_SETTINGS)
                self._on_bridge_error(
                    "No configuration file found. "
                    "Please fill in your credentials here and click Save."
                )

        # -- Dashboard context menu -> page switching ----------------------
        dashboard = self._unwrap(self._dashboard)
        if dashboard is not None:
            if hasattr(dashboard, "view_chart_requested"):
                dashboard.view_chart_requested.connect(self._on_view_chart)
            if hasattr(dashboard, "view_orders_requested"):
                dashboard.view_orders_requested.connect(self._on_view_orders)

        # -- Bot log error forwarding --------------------------------------
        bot_log = self._unwrap(self._bot_log)
        if bot_log is not None and hasattr(bot_log, "error_detected"):
            bot_log.error_detected.connect(self._on_bot_error)

    def _on_bridge_data(self, data: dict) -> None:
        """Handle aggregated data snapshot from EngineBridge.

        Distributes metrics to dashboard, charts, status bar, and toolbar.

        Parameters
        ----------
        data : dict
            Aggregated snapshot with keys: pnl, health, offers, risk,
            market_data, trade_summary, config, bot_status.
        """
        pnl = data.get("pnl", {})
        health = data.get("health", {})

        # Status bar update.
        pnl_total = int(pnl.get("total", 0))
        block_height = int(health.get("block_height", 0))

        # Compute average spread from all pairs.
        market_data = data.get("market_data", {})
        spreads = [v.get("spread_bps", 0.0) for v in market_data.values() if v]
        avg_spread = sum(spreads) / len(spreads) if spreads else 0.0

        # Derive XCH/USD rate from the XCH/wUSDC.b mid-price.
        # The engine publishes `mid_price` as `display_price * 1e12` for
        # every pair (see cpp/src/execution/market_data.cpp,
        # MarketDataFeed::publish_snapshot's `to_mojos` lambda), where
        # display_price is quote-units per base-unit.  Since wUSDC.b is
        # a $1 stablecoin, dividing by 1e12 yields USD per XCH directly.
        xch_usd = 0.0
        for pair_key in ("XCH/wUSDC.b", "XCH/wUSDC"):
            pair_md = market_data.get(pair_key, {})
            mid = pair_md.get("mid_price", 0.0)
            # A last_trade backfill is NOT a live rate.  The pairs table
            # already refuses to show one as a Mid Price; converting P&L to
            # dollars with the same months-old fill would reintroduce it as a
            # headline number, which is worse for being denominated in $.
            if str(pair_md.get("mid_price_source", "")) == "last_trade":
                continue
            if mid > 0:
                xch_usd = mid / 1_000_000_000_000.0
                break

        # Feed the dashboard's per-pair table: the live half (mid, spread,
        # our inventory) pairs with the DB half already cached.
        self._last_market_data = market_data
        # MetricsService leaves _latest untouched when a scrape fails and only
        # flips this flag, so without it the last good snapshot would be
        # presented as live indefinitely after the endpoint drops.
        self._metrics_live = bool(data.get("metrics_connected", True))
        # A rate derived from a dead feed must not price P&L either.
        self._last_xch_usd = xch_usd if self._metrics_live else 0.0
        self._refresh_pairs_table()

        # Charts update -- feed pair-aware snapshots with timestamp X-axis.
        chart = self._unwrap(self._chart)
        if chart is not None and market_data:
            pair_names = [p for p in market_data.keys() if p]
            if hasattr(chart, "add_pairs"):
                chart.add_pairs(pair_names)

            ts_now = time.time()
            # [PNL-USD-TOTALS 2026-08-01] Plot the engine's USD gauges.  The
            # xop_pnl_mojos "total"/"realized" components sum quote mojos
            # across pairs with different quote currencies (a DBX fill worth
            # ~$0.04 moved the raw "total" by ~70x its value), so the curves
            # were meaningless.  Fall back to the raw values only for engines
            # that predate the xop_pnl_usd gauge family.
            usd_total = pnl.get("usd")
            usd_realized = pnl.get("usd_realized")
            total_pnl = (
                float(usd_total)
                if usd_total is not None
                else float(pnl.get("total", 0.0))
            )
            realized_pnl = (
                float(usd_realized)
                if usd_realized is not None
                else float(pnl.get("realized", 0.0))
            )

            for pair_name, md in market_data.items():
                if not md:
                    continue

                last_block = self._chart_last_block_by_pair.get(pair_name, -1)
                if block_height <= 0 or block_height == last_block:
                    continue

                mid_v = float(md.get("mid_price", 0.0))
                spread_bps_v = float(md.get("spread_bps", 0.0))
                if mid_v <= 0:
                    continue

                best_bid = float(md.get("best_bid", 0.0) or 0.0)
                best_ask = float(md.get("best_ask", 0.0) or 0.0)
                if best_bid <= 0.0 or best_ask <= 0.0:
                    half_frac = spread_bps_v / 20000.0
                    best_bid = mid_v * (1.0 - half_frac)
                    best_ask = mid_v * (1.0 + half_frac)

                if hasattr(chart, "append_price_data_for_pair"):
                    chart.append_price_data_for_pair(
                        pair_name,
                        block_height,
                        mid_v,
                        best_bid,
                        best_ask,
                        ts_now,
                    )
                if hasattr(chart, "append_pnl_data_for_pair"):
                    chart.append_pnl_data_for_pair(
                        pair_name,
                        block_height,
                        total_pnl,
                        realized_pnl,
                        ts_now,
                    )

                self._chart_last_block_by_pair[pair_name] = block_height

        reports_widget = self._unwrap(self._reports)
        if reports_widget is not None and hasattr(reports_widget, "set_live_context"):
            reports_widget.set_live_context(
                xch_usd_rate=xch_usd,
                market_data=market_data,
                wallet_balances=data.get("wallet_balances", {}),
                pnl=pnl,
            )

        # [PNL-DISPLAY 2026-08-02] The status-bar headline shows the
        # restart-proof lifetime realized P&L from trade_log when the DB
        # figure has loaded; the engine's since-boot USD gauge is only a
        # fallback for the first seconds after GUI start.
        status_pnl_usd = self._pnl_display.get("lifetime_usd", pnl.get("usd"))
        self._status_bar.update_metrics(
            pnl_mojos=pnl_total,
            spread_bps=avg_spread,
            inventory_ratio=0.5,
            block_height=block_height,
            xch_usd_rate=xch_usd,
            pnl_usd=status_pnl_usd,
        )
        self._block_label.setText(f"Block: {block_height:,}")

        # Dashboard update -- translate bridge dict to card-keyed format.
        dashboard = self._unwrap(self._dashboard)
        if dashboard is not None and hasattr(dashboard, "update_metrics"):
            # Convert PnL values from raw mojos (int) to XCH (float) so
            # that MetricCard's {:+,.2f} formatter shows human-readable
            # amounts rather than 12-digit mojo integers.
            wallet_balances = data.get("wallet_balances", {})
            offers = data.get("offers", {})
            # [PNL-DISPLAY 2026-08-02] Headline cards come from the
            # persistent trade log (DatabaseService.query_pnl_display):
            # "Total P&L" is the lifetime realized figure and "24h P&L"
            # the trailing 24-hour realized figure.  Both survive GUI and
            # engine restarts because the engine's Prometheus gauges are
            # since-boot until rehydration, whereas trade_log is durable.
            # "Unrealized PnL" remains the engine's live mark-to-market
            # USD gauge (None on engines predating the gauge family).
            fees_xch = mojos_to_xch_float(int(data.get("fees_paid_24h", 0)))
            fees_usdc = fees_xch * xch_usd

            def _metric_payload(usd_value: float | None) -> dict[str, float | str]:
                if usd_value is None:
                    return {
                        "value": 0.0,
                        "spark": 0.0,
                        "display_text": "—",
                        "secondary_text": "engine predates USD gauges",
                    }
                return {
                    "value": usd_value,
                    "spark": usd_value,
                    "display_text": _fmt_usd(usd_value),
                    "secondary_text": "",
                }

            fill_count_24h = int(
                self._pnl_display.get("fills_24h", offers.get("filled", 0))
            )

            card_data = {
                "Total P&L": self._total_pnl_payload(),
                "24h P&L": self._pnl_24h_payload(),
                "Unrealized PnL": _metric_payload(pnl.get("usd_unrealized")),
                "24h Fill Count": {
                    "value": fill_count_24h,
                    "spark": fill_count_24h,
                },
                "Fees Paid 24h": {
                    "value": fees_usdc if xch_usd > 0 else fees_xch,
                    "spark": fees_usdc if xch_usd > 0 else fees_xch,
                    "display_text": f"${fees_usdc:,.2f}" if xch_usd > 0 else f"{fees_xch:,.4f} XCH",
                    "secondary_text": f"{fees_xch:,.4f} XCH" if xch_usd > 0 else "",
                },
            }
            dashboard.update_metrics(card_data, xch_usd_rate=xch_usd)
            if hasattr(dashboard, "update_bot_status"):
                status = data.get("bot_status", "Unknown")
                colour_map = {"Running": "green", "Stopped": "red", "Disconnected": "red"}
                dashboard.update_bot_status(status, colour=colour_map.get(status, "gray"))
            if hasattr(dashboard, "update_connection_status"):
                full_node_connected = (
                    health.get("node_synced", 0.0) >= 1.0
                    or block_height > 0
                    or bool(data.get("metrics_connected", False))
                )
                wallet_connected = (
                    health.get("wallet_connected", 0.0) >= 1.0
                    or bool(wallet_balances)
                )
                dashboard.update_connection_status({
                    "Full Node": full_node_connected,
                    "Wallet": wallet_connected,
                    "Dexie": True,
                })
            if hasattr(dashboard, "update_block_info"):
                # Use 0 timestamp as sentinel; dashboard handles it gracefully.
                dashboard.update_block_info(block_height, time.time() if block_height > 0 else 0.0)
            if hasattr(dashboard, "update_wallet_balances"):
                reserve = data.get("spendable_reserve", {})
                stuck = data.get("stuck_offers", 0)
                dashboard.update_wallet_balances(
                    wallet_balances,
                    reserve=reserve,
                    stuck_offers=stuck,
                )
            if hasattr(dashboard, "update_diagnostics"):
                dashboard.update_diagnostics(
                    metrics_connected=data.get("metrics_connected", False),
                    filled=int(offers.get("filled", 0)),
                    cancelled=int(offers.get("cancelled", 0)),
                    expired=int(offers.get("expired", 0)),
                    pending=int(offers.get("pending", 0)),
                    fees_24h_xch=fees_xch,
                    fees_24h_usdc=fees_usdc,
                )
            if hasattr(dashboard, "update_reserve_card"):
                dashboard.update_reserve_card(data.get("spendable_reserve", {}))

        # Market analysis update -- forward analysis data to the widget.
        analysis_widget = self._unwrap(self._market_analysis)
        if analysis_widget is not None:
            if hasattr(analysis_widget, "set_engine_status"):
                analysis_widget.set_engine_status(data.get("bot_status", ""))

        if analysis_widget is not None and hasattr(analysis_widget, "update_analysis"):
            analysis_data = data.get("analysis", {})
            if analysis_data:
                analysis_widget.update_analysis(analysis_data)

        # Wallet balances update -- forward to the wallet page widget.
        wallet_widget = self._unwrap(self._wallet_balances)
        if wallet_widget is not None and hasattr(wallet_widget, "update_balances"):
            wallet_bals = data.get("wallet_balances", {})
            reserve = data.get("spendable_reserve", {})
            market_data = data.get("market_data", {})
            stuck = data.get("stuck_offers", 0)
            pairs_cfg = data.get("config", {}).get("pairs", []) or []
            wallet_widget.update_balances(
                wallet_bals,
                reserve=reserve,
                market_data=market_data,
                stuck_offers=stuck,
                pairs=pairs_cfg,
            )

        # Warp Bridge tab -- feed the live warp snapshot (hot-wallet balances,
        # bridge config, and job list) sourced from data["warp"].
        warp_widget = self._unwrap(self._warp_widget)
        if warp_widget is not None and hasattr(warp_widget, "update_data"):
            warp_widget.update_data(data)

        # Base Wallet tab -- same snapshot; consumes data["warp"]["base_wallet"]
        # plus the wallet_notice / wallet_action_error companions.
        base_wallet_widget = self._unwrap(self._base_wallet_widget)
        if base_wallet_widget is not None and hasattr(base_wallet_widget, "update_data"):
            base_wallet_widget.update_data(data)

    def _on_bot_status_changed(self, status: str) -> None:
        """Update toolbar when bridge reports bot status change.

        Parameters
        ----------
        status : str
            New status string (Running, Stopped, Disconnected, etc.).
        """
        self._bot_status_label.setText(status)
        if status in ("Running",):
            colour = LIGHT_GREEN
            self._bot_running = True
            self._bot_paused = False
        elif status in ("Paused",):
            colour = _C.WARNING_YELLOW
            self._bot_running = True
            self._bot_paused = True
            # Which pause?  Resume can only clear the GUI flag; a breaker
            # pause needs a restart, and offering Resume for it makes the
            # button a lie.  The gauge is command-side truth.
            gui_flag = True
            try:
                if self._bridge is not None:
                    gui_flag = self._bridge.metrics_service.is_paused()
            except Exception:
                pass
            self._gui_pause_owns_it = gui_flag
            if not gui_flag:
                # Owned by a breaker or protection gate (flash-crash,
                # wallet circuit, recovery, dry-run) -- not by the GUI
                # flag, so Resume cannot clear it.
                self._bot_status_label.setText("Paused (protection)")
        elif status in ("Analyzing",):
            colour = _C.INFO_BLUE
            self._bot_running = False
            self._bot_paused = False
        elif status in ("Disconnected",):
            colour = LOSS_RED
            self._bot_running = False
            self._bot_paused = False
        else:
            colour = TEXT_SECONDARY
            self._bot_running = False
            self._bot_paused = False
        self._bot_status_label.setStyleSheet(f"color: {colour}; font-weight: bold;")
        self._style_start_stop_button()
        self._style_pause_resume_button()

        # Keep the connection indicator in sync with engine reachability.
        if status in ("Running", "Analyzing", "Paused"):
            self._connected = True
            self._conn_dot.setStyleSheet(f"color: {PRIMARY_GREEN}; font-size: 18px;")
            self._conn_label.setText("Connected")
            self._act_connect.setEnabled(False)
            self._act_disconnect.setEnabled(True)
        elif status in ("Disconnected",):
            self._connected = False
            self._conn_dot.setStyleSheet(f"color: {LOSS_RED}; font-size: 18px;")
            self._conn_label.setText("Disconnected")
            self._act_connect.setEnabled(True)
            self._act_disconnect.setEnabled(False)

    def _on_bridge_error(self, msg: str) -> None:
        """Display bridge error in status bar briefly.

        Parameters
        ----------
        msg : str
            Error message.
        """
        self._status_bar.showMessage(msg, 5000)

    def _on_engine_start_failed(self, msg: str) -> None:
        """Show a detailed dialog when the managed engine exits on startup."""
        self._status_bar.showMessage("Engine startup failed.", 10_000)
        if msg == self._last_engine_start_failure:
            return

        self._last_engine_start_failure = msg
        QMessageBox.critical(
            self,
            "XOPTrader — Engine Startup Failed",
            msg,
        )

    def _on_view_chart(self, pair_name: str) -> None:
        """Switch to chart page for the given pair.

        Parameters
        ----------
        pair_name : str
            Trading pair to display.
        """
        self._stacked.setCurrentIndex(_PAGE_CHARTS)
        self._sidebar.select_page(_PAGE_CHARTS)
        if self._chart is not None and hasattr(self._chart, "set_pair"):
            self._chart.set_pair(pair_name)

    def _on_view_orders(self, pair_name: str) -> None:
        """Switch to orders page filtered for the given pair.

        Also updates the order book widget's active pair when available
        so the depth view stays synchronised with the selected pair.

        Parameters
        ----------
        pair_name : str
            Trading pair to filter.
        """
        self._stacked.setCurrentIndex(_PAGE_ORDERS)
        self._sidebar.select_page(_PAGE_ORDERS)
        # Keep the order book widget in sync with the selected pair.
        if self._order_book is not None and hasattr(self._order_book, "set_pair"):
            self._order_book.set_pair(pair_name)

    def _on_bot_error(self, msg: str) -> None:
        """Handle error detected in bot log.

        Parameters
        ----------
        msg : str
            Error message from bot log.
        """
        self._status_bar.showMessage(f"ERROR: {msg[:100]}", 10000)

    def _on_offers_for_order_book(self, offers: list) -> None:
        """Forward active offers to the order book widget as own-order highlights.

        Only pending offers are forwarded so the depth visualisation can
        mark the bot's resting orders on the book.

        Parameters
        ----------
        offers : list
            List of offer dicts from the database service.
        """
        if self._order_book is None or not hasattr(self._order_book, "set_own_orders"):
            return
        # Filter to pending offers only
        own = [o for o in offers if o.get("status") == "pending"]
        self._order_book.set_own_orders(own)

    def _on_bridge_data_for_order_book(self, data: dict) -> None:
        """Forward market data from the bridge refresh to the order book.

        Extracts the ``order_book`` and ``market_data`` sections from the
        aggregated data snapshot and passes them to the widget.

        Parameters
        ----------
        data : dict
            Aggregated bridge data snapshot.
        """
        if self._order_book is None:
            return
        # Prefer the dedicated order_book key if present, fall back to
        # market_data for backward compatibility.
        ob_data = data.get("order_book", data.get("market_data", {}))
        if hasattr(self._order_book, "update_market_data"):
            self._order_book.update_market_data(ob_data)

    @staticmethod
    def _parse_trade_timestamp(value: Any) -> float:
        """Best-effort conversion of DB trade timestamp to Unix seconds."""
        if isinstance(value, (int, float)):
            return float(value)
        if isinstance(value, str):
            try:
                ts = value.replace("Z", "+00:00")
                return datetime.fromisoformat(ts).timestamp()
            except ValueError:
                return time.time()
        return time.time()

    def _remember_chart_trade_key(self, key: tuple) -> None:
        """Track seen trade keys with bounded memory."""
        if key in self._chart_seen_trade_keys:
            return
        self._chart_seen_trade_keys.add(key)
        self._chart_seen_trade_fifo.append(key)

        # deque maxlen auto-evicts oldest; keep set in sync lazily.
        if len(self._chart_seen_trade_keys) > self._chart_seen_trade_fifo.maxlen + 500:
            self._chart_seen_trade_keys = set(self._chart_seen_trade_fifo)

    def _on_trades_for_chart(self, trades: list) -> None:
        """Feed chart volume bars and fill markers from DB trades."""
        chart = self._unwrap(self._chart)
        if chart is None or not trades:
            return

        for tr in reversed(trades):
            pair_name = str(tr.get("pair_name", "") or "")
            side_raw = str(tr.get("side", "") or "").lower()
            block_height = int(tr.get("block_height", 0) or 0)
            price_mojos = float(tr.get("price_mojos", 0.0) or 0.0)
            size_mojos = float(tr.get("size_mojos", 0.0) or 0.0)

            if not pair_name or block_height <= 0 or price_mojos <= 0.0 or size_mojos <= 0.0:
                continue

            side = "buy" if side_raw == "bid" else "sell" if side_raw == "ask" else ""
            if not side:
                continue

            ts = self._parse_trade_timestamp(tr.get("timestamp"))
            sig = (
                pair_name,
                block_height,
                side,
                int(price_mojos),
                int(size_mojos),
                str(tr.get("timestamp", "")),
            )
            if sig in self._chart_seen_trade_keys:
                continue
            self._remember_chart_trade_key(sig)

            # Fill marker (price in mojos/XCH, matching chart scale).
            if hasattr(chart, "add_fill_marker_for_pair"):
                chart.add_fill_marker_for_pair(
                    pair_name,
                    block_height,
                    float(price_mojos),
                    side,
                    float(size_mojos),
                )

            # Per-block volume bars (values in base units ~= XCH).
            size_units = float(size_mojos) / 1_000_000_000_000.0
            key = (pair_name, block_height)
            buy_prev, sell_prev = self._chart_volume_by_pair_block.get(key, (0.0, 0.0))
            if side == "buy":
                buy_prev += size_units
            else:
                sell_prev += size_units
            self._chart_volume_by_pair_block[key] = (buy_prev, sell_prev)

            if hasattr(chart, "append_volume_data_for_pair"):
                chart.append_volume_data_for_pair(
                    pair_name,
                    block_height,
                    buy_prev,
                    sell_prev,
                    ts,
                )

    # ===================================================================== #
    #  Restart-proof P&L display (trade_log-derived)                         #
    # ===================================================================== #

    @staticmethod
    def _read_pnl_baseline() -> str:
        """Return the persisted P&L display-reset baseline ("" = none).

        Stored by the Settings page under the shared GUI QSettings
        identity ("XOP", "XOPTrader") as a UTC ``YYYY-MM-DD HH:MM:SS``
        string.  Read directly here so the dashboard works even when the
        settings widget failed to construct.
        """
        settings = QSettings("XOP", "XOPTrader")
        settings.beginGroup("pnl_display")
        value = str(settings.value("baseline_utc", "") or "")
        settings.endGroup()
        return value

    def _total_pnl_payload(self) -> dict[str, Any]:
        """Build the "Total P&L" card payload from the DB-derived figures."""
        display = self._pnl_display
        if not display:
            return {
                "value": 0.0,
                "spark": 0.0,
                "display_text": "—",
                "secondary_text": "loading from trade log…",
            }
        usd = float(display.get("lifetime_usd", 0.0))
        trades = int(display.get("lifetime_trades", 0))
        baseline = str(display.get("baseline_iso", "") or "")
        if baseline:
            secondary = f"since display reset {baseline} UTC"
        else:
            secondary = f"lifetime realized · {trades:,} fills"
        return {
            "value": usd,
            "spark": usd,
            "display_text": _fmt_usd(usd),
            "secondary_text": secondary,
        }

    def _pnl_24h_payload(self) -> dict[str, Any]:
        """Build the "24h P&L" card payload from the DB-derived figures."""
        display = self._pnl_display
        if not display:
            return {
                "value": 0.0,
                "spark": 0.0,
                "display_text": "—",
                "secondary_text": "loading from trade log…",
            }
        usd = float(display.get("pnl_24h_usd", 0.0))
        fills = int(display.get("fills_24h", 0))
        return {
            "value": usd,
            "spark": usd,
            "display_text": _fmt_usd(usd),
            "secondary_text": f"realized · {fills:,} fills in 24h",
        }

    def _refresh_pnl_cards(self) -> None:
        """Push the latest DB-derived P&L figures to the dashboard cards.

        ``DashboardWidget.update_metrics`` ignores card names missing from
        the payload, so this partial update never disturbs the other
        cards between bridge ticks.
        """
        dashboard = self._unwrap(self._dashboard)
        if dashboard is None or not hasattr(dashboard, "update_metrics"):
            return
        dashboard.update_metrics({
            "Total P&L": self._total_pnl_payload(),
            "24h P&L": self._pnl_24h_payload(),
        })

    def _on_pair_summary(self, summary: dict) -> None:
        """Cache our own per-pair book/fill figures for the dashboard table."""
        self._pair_summary = dict(summary or {})
        self._refresh_pairs_table()

    def _refresh_pairs_table(self) -> None:
        """Rebuild the dashboard's per-pair table.

        Combines the live market snapshot (mid, spread, our inventory) with
        the database view of our own resting offers and fills.  Every price
        here is OURS, not the third-party book: bid and ask are the best
        prices we are currently showing.

        Price conversion happens here so the rule lives in one place:
        price_mojos is scaled by 1e12 for every pair.  Inventory arrives
        ALREADY in display units from the wallet service (which divides by
        each asset's own factor) and must not be converted again.
        """
        dashboard = self._unwrap(self._dashboard)
        if dashboard is None or not hasattr(dashboard, "update_pairs_table"):
            return
        market = self._last_market_data or {}
        if not market:
            return

        rows: list[dict] = []
        for pair_name in sorted(market):
            md = market.get(pair_name) or {}
            ours = self._pair_summary.get(pair_name, {})
            quote_symbol = (pair_name.split("/", 1)[1]
                            if "/" in pair_name else "")
            rows.append({
                "pair": pair_name,
                # Only the engine's live gauge counts as a Mid Price.  The
                # bridge backfills this field from the last fill when the
                # gauge is absent and marks it mid_price_source=last_trade;
                # that fill is NOT age-bounded (XCH/wUSDC's most recent is
                # from April), so presenting it here would label a
                # four-month-old trade as the current mid -- the same
                # failure S5 fixed in the engine's own blend.  Absent gauge
                # renders as an em dash instead.
                # Gauge-derived, so withheld entirely when the metrics
                # scrape is down -- a stale mid is indistinguishable from a
                # live one on screen, and this column is read as "now".
                "mid_price": (
                    0.0
                    if (not self._metrics_live
                        or str(md.get("mid_price_source", "")) == "last_trade")
                    else float(md.get("mid_price", 0.0) or 0.0) / MOJOS_PER_XCH
                ),
                # None, not 0.0: a withheld gauge must render as an em
                # dash.  A hard zero is a real spread claim -- and a
                # confidently wrong one -- exactly what the liveness gate
                # exists to prevent.
                "spread_bps": (float(md.get("spread_bps", 0.0) or 0.0)
                               if self._metrics_live else None),
                # Already display units (the wallet service divides by the
                # asset's own factor), so it must NOT be divided again.
                # None (key absent) means the wallet snapshot could not
                # answer; the widget renders that as an em dash rather than
                # as a holding of zero.
                "inventory": (float(md["inventory_units"])
                              if "inventory_units" in md else None),
                "bid": float(ours.get("bid_mojos", 0.0) or 0.0) / MOJOS_PER_XCH,
                "ask": float(ours.get("ask_mojos", 0.0) or 0.0) / MOJOS_PER_XCH,
                "fills_24h": int(ours.get("fills_24h", 0) or 0),
                # realized_pnl is QUOTE mojos (engine.cpp computes it via
                # quote_mojos_for with quote_denom), so a CAT-quoted pair
                # divides by 1000, not 1e12 -- using the XCH divisor
                # understated wUSDC.b and BYC P&L by a factor of a billion.
                "pnl": (float(ours.get("pnl_mojos", 0.0) or 0.0)
                        / float(mojos_per_unit_for_pair(pair_name, "quote"))),
                # Sort key in USD.  The column displays figures in four
                # different quote currencies, so sorting the raw numbers
                # ranks magnitudes, not value: 0.6 wUSDC.b would outrank
                # 0.5 XCH.  Convert where the quote's USD rate is known;
                # unknown quotes sort below, alongside the em dashes.
                "pnl_sort_usd": _pnl_usd_sort_key(
                    (float(ours.get("pnl_mojos", 0.0) or 0.0)
                     / float(mojos_per_unit_for_pair(pair_name, "quote"))),
                    quote_symbol, self._last_xch_usd),
                "quote_symbol": quote_symbol,
            })

        dashboard.update_pairs_table(rows, self._last_xch_usd)

    def _on_trades_for_activity(self, trades: list) -> None:
        """Render recent fills into the dashboard's activity feed.

        The query returns newest-first; the feed reads top-down oldest-first,
        so the slice is reversed.  Only the newest few are shown -- this is a
        glance panel, and the Trade Log tab is the full record.

        Amounts go through the same helpers the Trade Log uses rather than
        being re-derived here: price_mojos is scaled by 10^12 for every pair
        while size_mojos uses the pair's own base units, and duplicating that
        rule is how the two views drift apart.
        """
        dashboard = self._unwrap(self._dashboard)
        if dashboard is None or not hasattr(dashboard, "update_trades"):
            return

        rows = list(trades)[:_ACTIVITY_FEED_ROWS][::-1]
        events = [e for e in (activity_event(t) for t in rows) if e is not None]
        if rows and not events:
            # Input existed but nothing rendered.  Replacing the feed with an
            # empty list here would discard a good snapshot because of bad
            # rows -- the opposite of the "one malformed row must not empty
            # the feed" rule that activity_event follows.
            return
        dashboard.update_trades(events)

    def _on_pnl_display(self, display: dict) -> None:
        """Receive restart-proof P&L figures from the database service."""
        self._pnl_display = dict(display or {})
        self._refresh_pnl_cards()

    def _on_pnl_baseline_changed(self, baseline_iso: str) -> None:
        """Re-query the P&L display after a Settings-page reset/clear."""
        if self._bridge is None:
            return
        db = self._bridge.database_service
        if hasattr(db, "query_pnl_display"):
            db.query_pnl_display(baseline_iso)

    def _on_pnl_history(self, points: list) -> None:
        """Seed the chart's P&L curve with DB-rebuilt history.

        Called once shortly after startup with the global USD P&L series
        reconstructed from snapshots + trade_log, so the curve no longer
        starts empty on every GUI restart.  The same global series is
        seeded into every configured pair's store, mirroring how live
        updates append the engine's global USD gauges per pair.

        The per-pair merge (~13k points x N pairs) used to run in one
        synchronous burst on the UI thread at startup.  It is now
        chunked: one pair per zero-length QTimer slice, so each
        event-loop stall is bounded to a single pair's merge and input
        events stay responsive.
        """
        chart = self._unwrap(self._chart)
        if chart is None or not points or not hasattr(chart, "seed_pnl_history"):
            return

        seed: list[tuple[int, float, float, float]] = []
        for point in points:
            try:
                seed.append((
                    int(point.get("block", 0)),
                    float(point.get("ts", 0.0)),
                    float(point.get("total_usd", 0.0)),
                    float(point.get("realized_usd", 0.0)),
                ))
            except (TypeError, ValueError, AttributeError):
                continue
        if not seed:
            return

        pair_names: list[str] = []
        if self._bridge is not None:
            try:
                pair_names = [
                    p.get("name", "")
                    for p in self._bridge.config_service.get_pairs()
                    if p.get("name")
                ]
            except Exception:  # noqa: BLE001 -- config shape is external
                pair_names = []
        if not pair_names:
            return

        self._pnl_seed_points = seed
        self._pnl_seed_queue = list(pair_names)
        self._pnl_seed_total = len(pair_names)
        QTimer.singleShot(0, self._seed_next_pnl_pair)

    def _seed_next_pnl_pair(self) -> None:
        """Seed one pair's P&L history, then yield back to the event loop."""
        queue: list[str] = getattr(self, "_pnl_seed_queue", [])
        seed = getattr(self, "_pnl_seed_points", [])
        if not queue or not seed:
            return

        chart = self._unwrap(self._chart)
        if chart is None or not hasattr(chart, "seed_pnl_history"):
            self._pnl_seed_queue = []
            self._pnl_seed_points = []
            return

        pair_name = queue.pop(0)
        try:
            chart.seed_pnl_history(pair_name, seed)
        except RuntimeError:
            # Chart widget destroyed mid-chain (window closing).
            self._pnl_seed_queue = []
            self._pnl_seed_points = []
            return

        if queue:
            QTimer.singleShot(0, self._seed_next_pnl_pair)
        else:
            _log.info(
                "Chart P&L history seeded from DB: %d points × %d pairs.",
                len(seed), getattr(self, "_pnl_seed_total", 0),
            )
            self._pnl_seed_points = []

    # ===================================================================== #
    #  Menu bar                                                              #
    # ===================================================================== #

    def _build_menu_bar(self) -> None:
        """Construct File, View, Settings, and Help menus."""
        menu_bar: QMenuBar = self.menuBar()
        menu_bar.setStyleSheet(
            f"""
            QMenuBar {{
                background-color: {PANEL_BG};
                color: {TEXT_PRIMARY};
                border-bottom: 1px solid {BORDER};
                padding: 2px 0;
                min-height: 24px;
            }}
            QMenuBar::item:selected {{
                background-color: {ELEVATED_BG};
                border-radius: 0px;
            }}
            QMenu {{
                background-color: {PANEL_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
                border-radius: 0px;
            }}
            QMenu::item {{
                padding: 4px 12px 4px 8px;
            }}
            QMenu::item:selected {{
                background-color: {PRIMARY_GREEN};
                color: white;
                border-radius: 0px;
            }}
            """
        )

        # -- File menu ------------------------------------------------------
        file_menu = menu_bar.addMenu("&File")

        self._act_connect = QAction("Connect to &Engine", self)
        self._act_connect.triggered.connect(self._on_connect)
        file_menu.addAction(self._act_connect)

        self._act_disconnect = QAction("&Disconnect from Engine", self)
        self._act_disconnect.setEnabled(False)
        self._act_disconnect.triggered.connect(self._on_disconnect)
        file_menu.addAction(self._act_disconnect)

        file_menu.addSeparator()

        self._act_start_trading = QAction("&Start Trading", self)
        self._act_start_trading.triggered.connect(self._on_start_stop)
        file_menu.addAction(self._act_start_trading)

        self._act_stop_trading = QAction("S&top Trading", self)
        self._act_stop_trading.setEnabled(False)
        self._act_stop_trading.triggered.connect(self._on_start_stop)
        file_menu.addAction(self._act_stop_trading)

        self._act_pause_trading = QAction("&Pause Trading", self)
        self._act_pause_trading.setEnabled(False)
        self._act_pause_trading.triggered.connect(self._on_pause_resume)
        file_menu.addAction(self._act_pause_trading)

        self._act_resume_trading = QAction("&Resume Trading", self)
        self._act_resume_trading.setEnabled(False)
        self._act_resume_trading.triggered.connect(self._on_pause_resume)
        file_menu.addAction(self._act_resume_trading)

        file_menu.addSeparator()

        act_export = QAction("&Export Trades CSV", self)
        act_export.triggered.connect(self._on_export_csv)
        file_menu.addAction(act_export)

        file_menu.addSeparator()

        act_quit = QAction("&Quit", self)
        act_quit.setShortcut(QKeySequence("Ctrl+Q"))
        act_quit.triggered.connect(self.close)
        file_menu.addAction(act_quit)

        # -- View menu ------------------------------------------------------
        view_menu = menu_bar.addMenu("&View")

        self._act_toggle_sidebar = QAction("Toggle &Sidebar", self)
        self._act_toggle_sidebar.setShortcut(QKeySequence("Ctrl+B"))
        self._act_toggle_sidebar.triggered.connect(self._on_toggle_sidebar)
        view_menu.addAction(self._act_toggle_sidebar)

        self._act_toggle_statusbar = QAction("Toggle Status &Bar", self)
        self._act_toggle_statusbar.triggered.connect(self._on_toggle_statusbar)
        view_menu.addAction(self._act_toggle_statusbar)

        view_menu.addSeparator()

        self._act_fullscreen = QAction("&Full Screen", self)
        self._act_fullscreen.setShortcut(QKeySequence("F11"))
        self._act_fullscreen.triggered.connect(self._on_toggle_fullscreen)
        view_menu.addAction(self._act_fullscreen)

        # -- Settings menu --------------------------------------------------
        settings_menu = menu_bar.addMenu("S&ettings")

        act_open_settings = QAction("&Open Settings Panel", self)
        act_open_settings.triggered.connect(lambda: self._switch_page(_PAGE_SETTINGS))
        settings_menu.addAction(act_open_settings)

        # -- Help menu ------------------------------------------------------
        help_menu = menu_bar.addMenu("&Help")

        act_about = QAction("&About", self)
        act_about.triggered.connect(self._on_about)
        help_menu.addAction(act_about)

        act_docs = QAction("&Documentation", self)
        act_docs.triggered.connect(self._on_open_docs)
        help_menu.addAction(act_docs)

        help_menu.addSeparator()

        act_updates = QAction("Check for &Updates\u2026", self)
        act_updates.triggered.connect(self._on_check_updates)
        help_menu.addAction(act_updates)

    # ===================================================================== #
    #  Toolbar                                                               #
    # ===================================================================== #

    def _build_toolbar(self) -> None:
        """Build the top toolbar with connection indicator, bot status,
        block height, uptime, and start/stop button."""
        toolbar: QToolBar = QToolBar("Main Toolbar", self)
        toolbar.setMovable(False)
        toolbar.setIconSize(QSize(24, 24))
        toolbar.setStyleSheet(
            f"""
            QToolBar {{
                background-color: {PANEL_BG};
                border-bottom: 1px solid {BORDER};
                spacing: 12px;
                padding: 8px 16px;
                min-height: 48px;
            }}
            QLabel {{
                color: {TEXT_PRIMARY};
                font-size: 13px;
                padding: 0 6px;
            }}
            """
        )
        self.addToolBar(toolbar)

        # Connection indicator (coloured dot + label)
        self._conn_dot = QLabel("\u25CF")  # filled circle
        self._conn_dot.setStyleSheet(f"color: {LOSS_RED}; font-size: 18px;")
        self._conn_dot.setToolTip("Connection to the XOPTrader C++ engine")
        toolbar.addWidget(self._conn_dot)

        self._conn_label = QLabel("Disconnected")
        self._conn_label.setToolTip("Connection to the XOPTrader C++ engine")
        toolbar.addWidget(self._conn_label)

        toolbar.addSeparator()

        # Bot status label
        self._bot_status_label = QLabel("Stopped")
        self._bot_status_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-weight: bold;"
        )
        self._bot_status_label.setToolTip("Current bot operating status")
        toolbar.addWidget(self._bot_status_label)

        # Spacer pushes remaining items to the right
        spacer = QWidget()
        spacer.setSizePolicy(
            QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred
        )
        toolbar.addWidget(spacer)

        # Block height
        self._block_label = QLabel("Block: --")
        self._block_label.setToolTip("Latest processed block height")
        toolbar.addWidget(self._block_label)

        toolbar.addSeparator()

        # Uptime
        self._uptime_label = QLabel("Uptime: 00:00:00")
        self._uptime_label.setToolTip("Time since GUI started")
        toolbar.addWidget(self._uptime_label)

        toolbar.addSeparator()

        # Start / Stop button
        self._start_stop_btn = QPushButton("Start Trading")
        self._start_stop_btn.setFixedSize(130, 36)
        self._start_stop_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._start_stop_btn.setToolTip("Start or stop live trading on the CHIA DEX")
        self._start_stop_btn.clicked.connect(self._on_start_stop)
        self._style_start_stop_button()
        toolbar.addWidget(self._start_stop_btn)

        # Pause / Resume button
        self._pause_resume_btn = QPushButton("Pause Trading")
        self._pause_resume_btn.setFixedSize(130, 36)
        self._pause_resume_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._pause_resume_btn.setToolTip(
            "Pause or resume offer posting (engine keeps running)"
        )
        self._pause_resume_btn.clicked.connect(self._on_pause_resume)
        self._pause_resume_btn.setEnabled(False)
        self._style_pause_resume_button()
        toolbar.addWidget(self._pause_resume_btn)

    def _style_start_stop_button(self) -> None:
        """Apply the correct colour to the start/stop button."""
        if self._bot_running:
            self._start_stop_btn.setText("Stop Trading")
            self._start_stop_btn.setStyleSheet(
                f"""
                QPushButton {{
                    background-color: {LOSS_RED};
                    color: white;
                    border: none;
                    border-radius: 8px;
                    font-weight: bold;
                    font-size: 13px;
                    padding: 8px 16px;
                }}
                QPushButton:hover {{ background-color: #F06060; }}
                """
            )
        else:
            self._start_stop_btn.setText("Start Trading")
            self._start_stop_btn.setStyleSheet(
                f"""
                QPushButton {{
                    background-color: {PRIMARY_GREEN};
                    color: white;
                    border: none;
                    border-radius: 8px;
                    font-weight: bold;
                    font-size: 13px;
                    padding: 8px 16px;
                }}
                QPushButton:hover {{ background-color: {LIGHT_GREEN}; }}
                """
            )

        # Keep File menu items in sync with the toolbar button.
        self._act_start_trading.setEnabled(not self._bot_running)
        self._act_stop_trading.setEnabled(self._bot_running)

    def _style_pause_resume_button(self) -> None:
        """Apply the correct colour and label to the pause/resume button."""
        if self._bot_paused:
            self._pause_resume_btn.setText("Resume Trading")
            self._pause_resume_btn.setStyleSheet(
                f"""
                QPushButton {{
                    background-color: {_C.WARNING_YELLOW};
                    color: {DARK_BG};
                    border: none;
                    border-radius: 8px;
                    font-weight: bold;
                    font-size: 13px;
                    padding: 8px 16px;
                }}
                QPushButton:hover {{ background-color: #FFD54F; }}
                """
            )
        else:
            self._pause_resume_btn.setText("Pause Trading")
            self._pause_resume_btn.setStyleSheet(
                f"""
                QPushButton {{
                    background-color: {ELEVATED_BG};
                    color: {TEXT_PRIMARY};
                    border: 1px solid {BORDER};
                    border-radius: 8px;
                    font-weight: bold;
                    font-size: 13px;
                    padding: 8px 16px;
                }}
                QPushButton:hover {{ background-color: {PANEL_BG}; }}
                """
            )

        # Enable pause only when the bot is running and not already paused.
        # Resume only clears the GUI flag, so it is offered only for a pause
        # the GUI actually owns -- a breaker pause requires a restart.
        can_pause = self._bot_running and not self._bot_paused
        can_resume = self._bot_paused and getattr(
            self, "_gui_pause_owns_it", True)
        self._pause_resume_btn.setEnabled(can_pause or can_resume)
        self._act_pause_trading.setEnabled(can_pause)
        self._act_resume_trading.setEnabled(can_resume)

    # ===================================================================== #
    #  Central area (sidebar + stacked widget + bottom tabs)                 #
    # ===================================================================== #

    def _build_central_area(self) -> None:
        """Assemble the sidebar, stacked content pages, and bottom tab panel.

        When dry-run mode is active a prominent yellow banner is displayed
        across the full window width above all other content.
        """
        central = QWidget(self)
        self.setCentralWidget(central)

        # Wrap everything in a vertical layout so the dry-run banner can
        # span the full width above the sidebar + content columns.
        root_layout = QVBoxLayout(central)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(0)

        # -- Dry-run banner (visible only when self._dry_run is True) -------
        if self._dry_run:
            banner = QLabel(
                "  DRY RUN \u2014 NO REAL ORDERS WILL BE PLACED  "
            )
            banner.setAlignment(Qt.AlignmentFlag.AlignCenter)
            banner.setStyleSheet(
                f"background-color: {_C.WARNING_YELLOW}; color: {DARK_BG}; "
                f"font-weight: bold; font-size: 11pt; padding: 4px 0;"
            )
            root_layout.addWidget(banner)

        # Horizontal container for sidebar + splitter
        h_container = QWidget(self)
        outer_layout = QHBoxLayout(h_container)
        outer_layout.setContentsMargins(0, 0, 0, 0)
        outer_layout.setSpacing(0)
        root_layout.addWidget(h_container, 1)  # stretch factor 1

        # -- Sidebar --------------------------------------------------------
        self._sidebar = Sidebar(self)
        self._sidebar.page_changed.connect(self._on_page_changed)
        outer_layout.addWidget(self._sidebar)

        # -- Vertical splitter (top content / bottom tabs) ------------------
        self._splitter = QSplitter(Qt.Orientation.Vertical, self)
        self._splitter.setHandleWidth(3)
        self._splitter.setStyleSheet(
            f"""
            QSplitter::handle {{
                background-color: {BORDER};
            }}
            QSplitter::handle:hover {{
                background-color: {PRIMARY_GREEN};
            }}
            """
        )

        # Top area: stacked widget (65 %)
        self._stacked = QStackedWidget(self)
        self._dashboard = self._create_page_widget(
            DashboardWidget, "Dashboard", scrollable=True
        )
        self._stacked.addWidget(self._dashboard)                    # index 0
        self._chart = self._create_page_widget(ChartWidget, "Charts")
        self._stacked.addWidget(self._chart)                        # index 1
        self._order_panel = self._create_page_widget(OrderPanel, "Orders")
        self._stacked.addWidget(self._order_panel)                  # index 2
        self._order_book = self._create_page_widget(OrderBookWidget, "Order Book")
        self._stacked.addWidget(self._order_book)                   # index 3
        self._market_analysis = self._create_page_widget(           # index 4
            MarketAnalysisWidget, "Market Analysis", scrollable=True
        )
        self._stacked.addWidget(self._market_analysis)
        self._wallet_balances = self._create_page_widget(           # index 5
            WalletBalancesWidget, "Wallet Balances", scrollable=True
        )
        self._stacked.addWidget(self._wallet_balances)
        self._reports = self._create_page_widget(                   # index 6
            ReportsWidget, "Reports", scrollable=True
        )
        self._stacked.addWidget(self._reports)
        self._warp_widget = self._create_page_widget(               # index 7
            WarpWidget, "Warp Bridge"
        )
        self._stacked.addWidget(self._warp_widget)
        self._base_wallet_widget = self._create_page_widget(        # index 8
            BaseWalletWidget, "Base Wallet"
        )
        self._stacked.addWidget(self._base_wallet_widget)
        self._settings_widget = self._create_page_widget(SettingsWidget, "Settings")
        self._stacked.addWidget(self._settings_widget)              # index 9
        self._splitter.addWidget(self._stacked)

        # Bottom area: tab widget (35 %)
        self._bottom_tabs = QTabWidget(self)
        self._bottom_tabs.setStyleSheet(
            f"""
            QTabWidget::pane {{
                background-color: {DARK_BG};
                border: 1px solid {BORDER};
                border-top: none;
            }}
            QTabBar::tab {{
                background-color: {PANEL_BG};
                color: {TEXT_SECONDARY};
                border: 1px solid {BORDER};
                border-bottom: none;
                padding: 4px 10px;
                margin-right: 1px;
                border-top-left-radius: 0px;
                border-top-right-radius: 0px;
                font-size: 11px;
                min-width: 60px;
            }}
            QTabBar::tab:selected {{
                background-color: {DARK_BG};
                color: {TEXT_PRIMARY};
                border-bottom: 2px solid {PRIMARY_GREEN};
            }}
            QTabBar::tab:hover {{
                color: {TEXT_PRIMARY};
                background-color: {ELEVATED_BG};
            }}
            """
        )
        self._bottom_tabs.addTab(
            _placeholder_widget("Live Configuration View"),
            "Configuration",
        )

        self._splitter.addWidget(self._bottom_tabs)

        # Default split ratio: 65 % top, 35 % bottom
        self._splitter.setStretchFactor(0, 65)
        self._splitter.setStretchFactor(1, 35)

        outer_layout.addWidget(self._splitter)

    def _push_sizing_paths(self) -> None:
        """Hand the advisory calculator the bridge's CURRENT paths.

        Called at wiring time and again whenever Settings saves a config, so
        a switched config file is picked up.  The calculator is loaded by
        path out of the application bundle and cannot derive these from its
        own __file__.
        """
        bridge = getattr(self, "_bridge", None)
        if bridge is None:
            return
        try:
            cfg_path = bridge.config_service.path
        except Exception:          # bridge still starting up
            return
        db = str(getattr(bridge, "db_path", "") or "") or None
        cfg = str(cfg_path) if cfg_path and cfg_path.is_file() else None

        settings = self._unwrap(self._settings_widget)
        if settings is not None and hasattr(settings, "set_sizing_db_path"):
            settings.set_sizing_db_path(db)
        wallet = self._unwrap(self._wallet_balances)
        if wallet is not None and hasattr(wallet, "set_sizing_paths"):
            wallet.set_sizing_paths(cfg, db)

    @staticmethod
    def _create_page_widget(
        widget_class: Optional[type],
        fallback_label: str,
        scrollable: bool = False,
    ) -> QWidget:
        """Instantiate *widget_class* if available, otherwise return a
        placeholder.

        Parameters
        ----------
        widget_class : type | None
            The widget class to instantiate.  ``None`` when the module
            has not been created yet.
        fallback_label : str
            Label text for the placeholder widget.
        scrollable : bool
            If true, wraps the widget in a QScrollArea.

        Returns
        -------
        QWidget
            The instantiated widget or a placeholder.
        """
        if widget_class is not None:
            try:
                w = widget_class()
                if scrollable:
                    scroll = QScrollArea()
                    scroll.setWidgetResizable(True)
                    scroll.setWidget(w)
                    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")
                    scroll._inner_widget = w  # type: ignore[attr-defined]
                    return scroll
                return w
            except Exception as exc:
                _log.warning("Failed to create widget %s: %s", widget_class, exc)
        return _placeholder_widget(f"{fallback_label} (not yet implemented)")

    # ===================================================================== #
    #  Status bar                                                            #
    # ===================================================================== #

    @staticmethod
    def _unwrap(widget: Optional[QWidget]) -> Optional[QWidget]:
        """Return the inner widget if *widget* is a QScrollArea wrapper."""
        if widget is not None and hasattr(widget, "_inner_widget"):
            return widget._inner_widget  # type: ignore[attr-defined]
        return widget

    def _build_status_bar(self) -> None:
        """Install the custom CHIA-branded status bar."""
        self._status_bar = StatusBar(self)
        self.setStatusBar(self._status_bar)

    # ===================================================================== #
    #  Keyboard shortcuts                                                    #
    # ===================================================================== #

    def _setup_keyboard_shortcuts(self) -> None:
        """Register global keyboard shortcuts.

        Ctrl+Q  -- Quit (also in File menu)
        Ctrl+1  -- Dashboard page
        Ctrl+2  -- Charts page
        Ctrl+3  -- Orders page
        Ctrl+4  -- Order Book page
        Ctrl+5  -- Settings page
        F11     -- Toggle full screen
        Ctrl+B  -- Toggle sidebar
        Space   -- Play/Pause Market Maker Bot
        Ctrl+X  -- Emergency Cancel All Orders
        Esc     -- Jump back to Dashboard
        """
        # Page switching: Ctrl+1 through Ctrl+5
        for index in range(5):
            action = QAction(self)
            action.setShortcut(QKeySequence(f"Ctrl+{index + 1}"))
            action.triggered.connect(
                lambda checked, idx=index: self._switch_page(idx)
            )
            self.addAction(action)

        # Space -> Start/Stop Bot
        play_action = QAction(self)
        play_action.setShortcut(QKeySequence("Space"))
        play_action.triggered.connect(self._on_start_stop)
        self.addAction(play_action)

        # Ctrl+X -> Cancel All Orders
        cancel_action = QAction(self)
        cancel_action.setShortcut(QKeySequence("Ctrl+X"))
        cancel_action.triggered.connect(self._on_emergency_cancel)
        self.addAction(cancel_action)

        # Esc -> Jump to Dashboard
        esc_action = QAction(self)
        esc_action.setShortcut(QKeySequence("Esc"))
        esc_action.triggered.connect(lambda: self._switch_page(0))
        self.addAction(esc_action)

    def _on_emergency_cancel(self) -> None:
        """Globally trigger cancel all offers if the bot is running."""
        try:
            if self._bot_running and self._order_panel:
                self._order_panel.cancel_all_requested.emit()
        except Exception:
            pass

    def _switch_page(self, index: int) -> None:
        """Switch both the stacked widget and the sidebar selection.

        Parameters
        ----------
        index : int
            Zero-based page index.
        """
        self._stacked.setCurrentIndex(index)
        self._sidebar.select_page(index)

    def open_settings_page(self) -> None:
        """Navigate to the Settings page.

        This is used by first-run onboarding code to direct users to
        configuration without relying on private page-index internals.
        """
        self._switch_page(_PAGE_SETTINGS)

    # ===================================================================== #
    #  Timers                                                                #
    # ===================================================================== #

    def _setup_timers(self) -> None:
        """Create the 1-second status timer and 5-second metrics timer."""

        # 1-second timer: PnL, block height, uptime, clock
        self._status_timer = QTimer(self)
        self._status_timer.setInterval(_STATUS_INTERVAL_MS)
        self._status_timer.timeout.connect(self._on_status_tick)
        self._status_timer.start()

        # 5-second timer: full metrics pull from Prometheus
        self._metrics_timer = QTimer(self)
        self._metrics_timer.setInterval(_METRICS_INTERVAL_MS)
        self._metrics_timer.timeout.connect(self._on_metrics_tick)
        self._metrics_timer.start()

    # ===================================================================== #
    #  Timer slots                                                           #
    # ===================================================================== #

    def _on_status_tick(self) -> None:
        """Called every second to update toolbar / status bar readouts."""
        # Uptime
        elapsed: float = time.monotonic() - self._start_time
        hours, remainder = divmod(int(elapsed), 3600)
        minutes, seconds = divmod(remainder, 60)
        self._uptime_label.setText(f"Uptime: {hours:02d}:{minutes:02d}:{seconds:02d}")

        # Clock and memory in the status bar
        self._status_bar.refresh_clock_and_memory()

    def _on_metrics_tick(self) -> None:
        """Called every 5 seconds as a fallback metrics refresh.

        When the EngineBridge is wired (via set_bridge), the bridge's own
        data_updated signal drives all widget updates through _on_bridge_data.
        This timer serves only as a keep-alive check when no bridge is set.
        """
        if self._bridge is not None:
            # Bridge handles metrics delivery; nothing to do here.
            return

        # No bridge connected -- status bar shows stale/placeholder data.
        pass

    # ===================================================================== #
    #  Slot handlers                                                         #
    # ===================================================================== #

    def _on_page_changed(self, index: int) -> None:
        """Respond to sidebar page-change signal."""
        self._stacked.setCurrentIndex(index)

    def _on_connect(self) -> None:
        """Handle File > Connect to Engine.

        Starts the bridge's metrics polling timer so the GUI
        receives live data from the running C++ engine.
        """
        if self._bridge is not None:
            if not self._bridge._master_timer.isActive():
                self._bridge._master_timer.start()
                _log.info("Reconnected to engine metrics polling.")
        self._connected = True
        self._conn_dot.setStyleSheet(f"color: {PRIMARY_GREEN}; font-size: 18px;")
        self._conn_label.setText("Connected")
        self._act_connect.setEnabled(False)
        self._act_disconnect.setEnabled(True)

    def _on_disconnect(self) -> None:
        """Handle File > Disconnect from Engine.

        Stops the bridge's metrics polling timer.  The C++ engine
        continues to run independently; only the GUI feed is paused.
        """
        if self._bridge is not None:
            if self._bridge._master_timer.isActive():
                self._bridge._master_timer.stop()
                _log.info("Disconnected from engine metrics polling.")
        self._connected = False
        self._conn_dot.setStyleSheet(f"color: {LOSS_RED}; font-size: 18px;")
        self._conn_label.setText("Disconnected")
        self._act_connect.setEnabled(True)
        self._act_disconnect.setEnabled(False)

    def _on_start_stop(self) -> None:
        """Toggle bot running state after user confirmation.

        A confirmation dialog is shown before starting or stopping the
        trading engine to guard against accidental clicks
        (ISO/IEC 25000 -- error prevention).
        """
        if self._bot_running:
            # Currently running -- confirm stop
            reply = QMessageBox.question(
                self,
                "Stop Trading",
                "Stop trading? Active offers will be cancelled.",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
        else:
            # Currently stopped -- confirm start
            if self._dry_run:
                prompt = "Start trading in dry-run mode? (No real offers will be placed.)"
            else:
                prompt = (
                    "Start trading? "
                    "(Offers will be placed on the CHIA DEX.)"
                )
            reply = QMessageBox.question(
                self,
                "Start Trading",
                prompt,
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if reply != QMessageBox.StandardButton.Yes:
                return

        # Delegate to bridge (Phase 1 stubs emit user-facing messages).
        if self._bridge is not None:
            if self._bot_running:
                self._bridge.stop_engine()
            else:
                self._bridge.start_engine()

        # Toggle local state for immediate visual feedback.
        self._bot_running = not self._bot_running
        self._bot_paused = False  # Reset pause when starting/stopping.
        self._style_start_stop_button()
        self._style_pause_resume_button()

        if self._bot_running:
            status_text = "Dry Run" if self._dry_run else "Running"
            colour = LIGHT_GREEN
        else:
            status_text = "Stopped"
            colour = TEXT_SECONDARY

        self._bot_status_label.setText(status_text)
        self._bot_status_label.setStyleSheet(
            f"color: {colour}; font-weight: bold;"
        )

    def _on_pause_resume(self) -> None:
        """Toggle pause/resume state for trading.

        When paused, the engine keeps running (market data, analytics,
        metrics) but skips offer posting (Step 8).  Resume removes the
        pause flag and the engine resumes posting on the next block.
        """
        if self._bot_paused:
            # Currently paused -- resume
            if self._bridge is not None:
                self._bridge.resume_trading()
            self._bot_paused = False
        else:
            # Currently running -- pause
            if self._bridge is not None:
                self._bridge.pause_trading()
            self._bot_paused = True

        self._style_pause_resume_button()

        if self._bot_paused:
            self._bot_status_label.setText("Paused")
            self._bot_status_label.setStyleSheet(
                f"color: {_C.WARNING_YELLOW}; font-weight: bold;"
            )
        else:
            status_text = "Dry Run" if self._dry_run else "Running"
            self._bot_status_label.setText(status_text)
            self._bot_status_label.setStyleSheet(
                f"color: {LIGHT_GREEN}; font-weight: bold;"
            )

    def _on_toggle_sidebar(self) -> None:
        """Toggle sidebar expansion via View menu or Ctrl+B."""
        self._sidebar.toggle()

    def _on_toggle_statusbar(self) -> None:
        """Toggle visibility of the status bar."""
        bar: StatusBar = self.statusBar()  # type: ignore[assignment]
        bar.setVisible(not bar.isVisible())

    def _on_toggle_fullscreen(self) -> None:
        """Toggle between full-screen and normal window state."""
        if self.isFullScreen():
            self.showNormal()
        else:
            self.showFullScreen()

    def _on_export_csv(self) -> None:
        """Export trade history to CSV via a file dialog.

        Opens a save-file dialog and delegates to the trade-log widget's
        ``export_csv`` method if available.
        """
        if self._trade_log is None or not hasattr(self._trade_log, "export_csv"):
            QMessageBox.information(
                self,
                "Export Unavailable",
                "Trade log widget is not available for CSV export.",
            )
            return

        path, _ = QFileDialog.getSaveFileName(
            self,
            "Export Trades CSV",
            "trades.csv",
            "CSV Files (*.csv);;All Files (*)",
        )
        if path:
            try:
                self._trade_log.export_csv(path)
            except Exception as exc:
                _log.warning("CSV export failed: %s", exc)
                QMessageBox.warning(
                    self,
                    "Export Failed",
                    f"Could not export trades: {exc}",
                )

    def _on_about(self) -> None:
        """Display the About dialog.

        A lightweight message box is used instead of a dedicated widget
        to keep the dependency footprint small.
        """
        QMessageBox.about(
            self,
            "About XOPTrader",
            (
                "<h3>XOPTrader</h3>"
                "<p>CHIA DEX Market-Maker Control Panel</p>"
                f"<p>Version {self._get_version()}</p>"
                "<p>Built with PySide6 (Qt 6)</p>"
            ),
        )

    @staticmethod
    def _on_open_docs() -> None:
        """Open the online documentation in the default browser."""
        from PySide6.QtCore import QUrl
        from PySide6.QtGui import QDesktopServices

        QDesktopServices.openUrl(
            QUrl("https://github.com/XOPTrader/xoptrader/wiki")
        )

    def _on_check_updates(self) -> None:
        """Launch a background check for newer releases on GitHub."""
        from gui.services.update_service import UpdateService

        svc = UpdateService(self._get_version(), parent=self)
        svc.update_available.connect(self._on_update_available)
        svc.up_to_date.connect(
            lambda: QMessageBox.information(
                self, "Up to Date",
                f"You are running the latest version ({self._get_version()}).",
            )
        )
        svc.check_failed.connect(
            lambda err: QMessageBox.warning(
                self, "Update Check Failed",
                f"Could not check for updates:\n{err}",
            )
        )
        # Store a reference so the service isn't garbage-collected.
        self._update_svc = svc
        svc.check()

    def _on_update_available(self, version: str, url: str) -> None:
        """Show a dialog when a newer version is found."""
        from PySide6.QtCore import QUrl
        from PySide6.QtGui import QDesktopServices

        result = QMessageBox.information(
            self,
            "Update Available",
            (
                f"<p>A new version of XOPTrader is available: "
                f"<b>v{version}</b></p>"
                f"<p>You are running v{self._get_version()}.</p>"
                f"<p>Would you like to open the release page?</p>"
            ),
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.Yes,
        )
        if result == QMessageBox.StandardButton.Yes:
            QDesktopServices.openUrl(QUrl(url))

    @staticmethod
    def _get_version() -> str:
        """Return the GUI package version string."""
        try:
            from gui import __version__

            return __version__
        except ImportError:
            return "0.0.0"

    # ===================================================================== #
    #  State persistence                                                     #
    # ===================================================================== #

    def _restore_state(self) -> None:
        """Restore window geometry and splitter positions from QSettings."""
        geometry = self._settings.value(_KEY_GEOMETRY)
        if geometry is not None:
            self.restoreGeometry(geometry)  # type: ignore[arg-type]

        state = self._settings.value(_KEY_STATE)
        if state is not None:
            self.restoreState(state)  # type: ignore[arg-type]

        splitter_state = self._settings.value(_KEY_SPLITTER)
        if splitter_state is not None:
            self._splitter.restoreState(splitter_state)  # type: ignore[arg-type]

    def _save_state(self) -> None:
        """Persist window geometry and splitter positions to QSettings."""
        self._settings.setValue(_KEY_GEOMETRY, self.saveGeometry())
        self._settings.setValue(_KEY_STATE, self.saveState())
        self._settings.setValue(_KEY_SPLITTER, self._splitter.saveState())

    # ===================================================================== #
    #  Overrides                                                             #
    # ===================================================================== #

    def closeEvent(self, event: QCloseEvent) -> None:
        """Save state and stop timers before closing.

        If the settings widget has unsaved changes (``_dirty`` flag), the
        user is prompted to save, discard, or cancel the close
        (ISO/IEC 25000 -- error prevention / data-loss guard).

        Parameters
        ----------
        event : QCloseEvent
            The close event from the windowing system.
        """
        # -- Check for unsaved settings ------------------------------------
        if (
            self._settings_widget is not None
            and getattr(self._settings_widget, "_dirty", False)
        ):
            reply = QMessageBox.question(
                self,
                "Unsaved Settings",
                "You have unsaved settings changes. Save before closing?",
                (
                    QMessageBox.StandardButton.Save
                    | QMessageBox.StandardButton.Discard
                    | QMessageBox.StandardButton.Cancel
                ),
                QMessageBox.StandardButton.Save,
            )
            if reply == QMessageBox.StandardButton.Cancel:
                event.ignore()
                return
            if reply == QMessageBox.StandardButton.Save:
                if hasattr(self._settings_widget, "save_config"):
                    self._settings_widget.save_config()

        self._status_timer.stop()
        self._metrics_timer.stop()
        # Join widget-owned worker threads BEFORE teardown.  Qt aborts the
        # process (qFatal) if a QThread is destroyed while still running, and
        # nothing else knows about these: EngineBridge.shutdown() owns the
        # services, not the pages' own threads.
        for page in (self._wallet_balances, self._settings_widget):
            widget = self._unwrap(page)
            if widget is not None and hasattr(widget, "stop_background_work"):
                widget.stop_background_work()
        self._save_state()
        super().closeEvent(event)
