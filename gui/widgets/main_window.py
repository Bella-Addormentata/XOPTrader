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
    from gui.widgets.dashboard import DashboardWidget
except ImportError:
    DashboardWidget = None  # type: ignore[assignment,misc]

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

# ---------------------------------------------------------------------------
# Theme constants -- sourced from the canonical CHIA palette singleton.
# ---------------------------------------------------------------------------
from gui.theme import COLORS as _C
from gui.utils import mojos_to_xch_float

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
_PAGE_SETTINGS: Final[int] = 7


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

        # -- Runtime state --------------------------------------------------
        self._connected: bool = False
        self._bot_running: bool = False
        self._bot_paused: bool = False
        self._dry_run: bool = dry_run
        self._start_time: float = time.monotonic()
        self._last_engine_start_failure: str = ""

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
        if self._trade_log is not None and hasattr(db, "trades_loaded"):
            db.trades_loaded.connect(self._trade_log.load_trades)

        # Kick off the initial offers query so the auto-refresh loop
        # has a set of parameters to re-issue on subsequent ticks.
        if hasattr(db, "query_offers"):
            db.query_offers()

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

        # Auto-populate the settings panel from the bridge's config file so
        # users can edit credentials without touching the file system manually.
        settings = self._unwrap(self._settings_widget)
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

        self._status_bar.update_metrics(
            pnl_mojos=pnl_total,
            spread_bps=avg_spread,
            inventory_ratio=0.5,
            block_height=block_height,
        )
        self._block_label.setText(f"Block: {block_height:,}")

        # Dashboard update -- translate bridge dict to card-keyed format.
        dashboard = self._unwrap(self._dashboard)
        if dashboard is not None and hasattr(dashboard, "update_metrics"):
            # Convert PnL values from raw mojos (int) to XCH (float) so
            # that MetricCard's {:+,.2f} formatter shows human-readable
            # amounts rather than 12-digit mojo integers.
            total_xch = mojos_to_xch_float(int(pnl.get("total", 0)))
            realized_xch = mojos_to_xch_float(int(pnl.get("realized", 0)))
            unrealized_xch = mojos_to_xch_float(int(pnl.get("unrealized", 0)))
            spread_xch = mojos_to_xch_float(int(pnl.get("spread", 0)))
            inventory_xch = mojos_to_xch_float(int(pnl.get("inventory", 0)))

            card_data = {
                "Total PnL": {"value": total_xch, "spark": total_xch},
                "Realized PnL": {"value": realized_xch, "spark": realized_xch},
                "Unrealized PnL": {"value": unrealized_xch, "spark": unrealized_xch},
                "Spread PnL": {"value": spread_xch, "spark": spread_xch},
                "Inventory PnL": {"value": inventory_xch, "spark": inventory_xch},
                "24h Fill Count": {
                    "value": data.get("offers", {}).get("filled", 0),
                    "spark": data.get("offers", {}).get("filled", 0),
                },
                "Fees Paid 24h": {
                    "value": mojos_to_xch_float(int(data.get("fees_paid_24h", 0))),
                    "spark": mojos_to_xch_float(int(data.get("fees_paid_24h", 0))),
                },
            }
            dashboard.update_metrics(card_data)
            if hasattr(dashboard, "update_bot_status"):
                status = data.get("bot_status", "Unknown")
                colour_map = {"Running": "green", "Stopped": "red", "Disconnected": "red"}
                dashboard.update_bot_status(status, colour=colour_map.get(status, "gray"))
            if hasattr(dashboard, "update_connection_status"):
                dashboard.update_connection_status({
                    "Full Node": health.get("node_synced", 0.0) >= 1.0,
                    "Wallet": health.get("wallet_connected", 0.0) >= 1.0,
                    "Dexie": True,
                })
            if hasattr(dashboard, "update_block_info"):
                # Use 0 timestamp as sentinel; dashboard handles it gracefully.
                dashboard.update_block_info(block_height, time.time() if block_height > 0 else 0.0)
            if hasattr(dashboard, "update_wallet_balances"):
                wallet_bals = data.get("wallet_balances", {})
                reserve = data.get("spendable_reserve", {})
                stuck = data.get("stuck_offers", 0)
                dashboard.update_wallet_balances(wallet_bals, reserve=reserve, stuck_offers=stuck)

        # Market analysis update -- forward analysis data to the widget.
        analysis_widget = self._unwrap(self._market_analysis)
        if analysis_widget is not None and hasattr(analysis_widget, "update_analysis"):
            analysis_data = data.get("analysis", {})
            if analysis_data:
                analysis_widget.update_analysis(analysis_data)

        # Wallet balances update -- forward to the wallet page widget.
        wallet_widget = self._unwrap(self._wallet_balances)
        if wallet_widget is not None and hasattr(wallet_widget, "update_balances"):
            wallet_bals = data.get("wallet_balances", {})
            reserve = data.get("spendable_reserve", {})
            stuck = data.get("stuck_offers", 0)
            wallet_widget.update_balances(wallet_bals, reserve=reserve, stuck_offers=stuck)

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
        can_pause = self._bot_running and not self._bot_paused
        can_resume = self._bot_paused
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
        self._settings_widget = self._create_page_widget(SettingsWidget, "Settings")
        self._stacked.addWidget(self._settings_widget)              # index 7
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
        self._save_state()
        super().closeEvent(event)
