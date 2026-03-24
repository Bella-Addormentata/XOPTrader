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
    QSizePolicy,
    QSplitter,
    QStackedWidget,
    QStatusBar,
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

# ---------------------------------------------------------------------------
# Theme constants -- sourced from the canonical CHIA palette singleton.
# ---------------------------------------------------------------------------
from gui.theme import COLORS as _C

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
        self._dry_run: bool = dry_run
        self._start_time: float = time.monotonic()

        # -- Child widget references (populated in _build_central_area) -----
        self._dashboard: Optional[QWidget] = None
        self._chart: Optional[QWidget] = None
        self._order_panel: Optional[QWidget] = None
        self._order_book: Optional[QWidget] = None
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

        # -- Database -> widget signals ------------------------------------
        db = bridge.database_service
        if self._order_panel is not None and hasattr(db, "offers_loaded"):
            db.offers_loaded.connect(self._order_panel.update_offers)
        if self._tab_order_panel is not None and hasattr(db, "offers_loaded"):
            db.offers_loaded.connect(self._tab_order_panel.update_offers)
        if self._trade_log is not None and hasattr(db, "trades_loaded"):
            db.trades_loaded.connect(self._trade_log.load_trades)

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
            self._settings_widget.config_saved.connect(
                lambda path: bridge.reload_config()
            )

        # -- Dashboard context menu -> page switching ----------------------
        if self._dashboard is not None:
            if hasattr(self._dashboard, "view_chart_requested"):
                self._dashboard.view_chart_requested.connect(self._on_view_chart)
            if hasattr(self._dashboard, "view_orders_requested"):
                self._dashboard.view_orders_requested.connect(self._on_view_orders)

        # -- Bot log error forwarding --------------------------------------
        if self._bot_log is not None and hasattr(self._bot_log, "error_detected"):
            self._bot_log.error_detected.connect(self._on_bot_error)

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
        if self._dashboard is not None and hasattr(self._dashboard, "update_metrics"):
            card_data = {
                "Total PnL": {"value": pnl.get("total", 0), "spark": pnl.get("total", 0)},
                "Realized PnL": {"value": pnl.get("realized", 0), "spark": pnl.get("realized", 0)},
                "Unrealized PnL": {"value": pnl.get("unrealized", 0), "spark": pnl.get("unrealized", 0)},
                "Spread PnL": {"value": pnl.get("spread", 0), "spark": pnl.get("spread", 0)},
                "Inventory PnL": {"value": pnl.get("inventory", 0), "spark": pnl.get("inventory", 0)},
                "24h Fill Count": {
                    "value": data.get("offers", {}).get("filled", 0),
                    "spark": data.get("offers", {}).get("filled", 0),
                },
            }
            self._dashboard.update_metrics(card_data)
            if hasattr(self._dashboard, "update_bot_status"):
                status = data.get("bot_status", "Unknown")
                colour_map = {"Running": "green", "Stopped": "red", "Disconnected": "red"}
                self._dashboard.update_bot_status(status, colour=colour_map.get(status, "gray"))
            if hasattr(self._dashboard, "update_connection_status"):
                self._dashboard.update_connection_status({
                    "Full Node": health.get("node_synced", 0.0) >= 1.0,
                    "Wallet": health.get("wallet_connected", 0.0) >= 1.0,
                    "Dexie": True,
                })
            if hasattr(self._dashboard, "update_block_info"):
                # Use 0 timestamp as sentinel; dashboard handles it gracefully.
                self._dashboard.update_block_info(block_height, time.time() if block_height > 0 else 0.0)

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
        elif status in ("Disconnected",):
            colour = LOSS_RED
            self._bot_running = False
        else:
            colour = TEXT_SECONDARY
            self._bot_running = False
        self._bot_status_label.setStyleSheet(f"color: {colour}; font-weight: bold;")
        self._style_start_stop_button()

    def _on_bridge_error(self, msg: str) -> None:
        """Display bridge error in status bar briefly.

        Parameters
        ----------
        msg : str
            Error message.
        """
        self._status_bar.showMessage(msg, 5000)

    def _on_view_chart(self, pair_name: str) -> None:
        """Switch to chart page for the given pair.

        Parameters
        ----------
        pair_name : str
            Trading pair to display.
        """
        self._stacked.setCurrentIndex(1)
        self._sidebar.select_page(1)
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
        self._stacked.setCurrentIndex(2)
        self._sidebar.select_page(2)
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
            }}
            QMenuBar::item:selected {{
                background-color: {ELEVATED_BG};
            }}
            QMenu {{
                background-color: {PANEL_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
            }}
            QMenu::item:selected {{
                background-color: {PRIMARY_GREEN};
                color: white;
            }}
            """
        )

        # -- File menu ------------------------------------------------------
        file_menu = menu_bar.addMenu("&File")

        self._act_connect = QAction("&Connect", self)
        self._act_connect.triggered.connect(self._on_connect)
        file_menu.addAction(self._act_connect)

        self._act_disconnect = QAction("&Disconnect", self)
        self._act_disconnect.setEnabled(False)
        self._act_disconnect.triggered.connect(self._on_disconnect)
        file_menu.addAction(self._act_disconnect)

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
        act_open_settings.triggered.connect(lambda: self._stacked.setCurrentIndex(4))
        settings_menu.addAction(act_open_settings)

        # -- Help menu ------------------------------------------------------
        help_menu = menu_bar.addMenu("&Help")

        act_about = QAction("&About", self)
        act_about.triggered.connect(self._on_about)
        help_menu.addAction(act_about)

        act_docs = QAction("&Documentation", self)
        act_docs.triggered.connect(self._on_open_docs)
        help_menu.addAction(act_docs)

    # ===================================================================== #
    #  Toolbar                                                               #
    # ===================================================================== #

    def _build_toolbar(self) -> None:
        """Build the top toolbar with connection indicator, bot status,
        block height, uptime, and start/stop button."""
        toolbar: QToolBar = QToolBar("Main Toolbar", self)
        toolbar.setMovable(False)
        toolbar.setIconSize(QSize(18, 18))
        toolbar.setStyleSheet(
            f"""
            QToolBar {{
                background-color: {PANEL_BG};
                border-bottom: 1px solid {BORDER};
                spacing: 8px;
                padding: 4px 8px;
            }}
            QLabel {{
                color: {TEXT_PRIMARY};
                font-size: 12px;
                padding: 0 4px;
            }}
            """
        )
        self.addToolBar(toolbar)

        # Connection indicator (coloured dot + label)
        self._conn_dot = QLabel("\u25CF")  # filled circle
        self._conn_dot.setStyleSheet(f"color: {LOSS_RED}; font-size: 14px;")
        self._conn_dot.setToolTip("Connection status to CHIA full node")
        toolbar.addWidget(self._conn_dot)

        self._conn_label = QLabel("Disconnected")
        self._conn_label.setToolTip("Connection status to CHIA full node")
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
        self._start_stop_btn = QPushButton("Start")
        self._start_stop_btn.setFixedSize(80, 28)
        self._start_stop_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._start_stop_btn.setToolTip("Start or stop the trading engine")
        self._start_stop_btn.clicked.connect(self._on_start_stop)
        self._style_start_stop_button()
        toolbar.addWidget(self._start_stop_btn)

    def _style_start_stop_button(self) -> None:
        """Apply the correct colour to the start/stop button."""
        if self._bot_running:
            self._start_stop_btn.setText("Stop")
            self._start_stop_btn.setStyleSheet(
                f"""
                QPushButton {{
                    background-color: {LOSS_RED};
                    color: white;
                    border: none;
                    border-radius: 4px;
                    font-weight: bold;
                    font-size: 12px;
                }}
                QPushButton:hover {{ background-color: #F06060; }}
                """
            )
        else:
            self._start_stop_btn.setText("Start")
            self._start_stop_btn.setStyleSheet(
                f"""
                QPushButton {{
                    background-color: {PRIMARY_GREEN};
                    color: white;
                    border: none;
                    border-radius: 4px;
                    font-weight: bold;
                    font-size: 12px;
                }}
                QPushButton:hover {{ background-color: {LIGHT_GREEN}; }}
                """
            )

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
        self._dashboard = self._create_page_widget(DashboardWidget, "Dashboard")
        self._stacked.addWidget(self._dashboard)
        self._chart = self._create_page_widget(ChartWidget, "Charts")
        self._stacked.addWidget(self._chart)
        self._order_panel = self._create_page_widget(OrderPanel, "Orders")
        self._stacked.addWidget(self._order_panel)
        self._order_book = self._create_page_widget(OrderBookWidget, "Order Book")
        self._stacked.addWidget(self._order_book)
        self._settings_widget = self._create_page_widget(SettingsWidget, "Settings")
        self._stacked.addWidget(self._settings_widget)
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
                padding: 6px 14px;
                margin-right: 2px;
                border-top-left-radius: 4px;
                border-top-right-radius: 4px;
            }}
            QTabBar::tab:selected {{
                background-color: {DARK_BG};
                color: {TEXT_PRIMARY};
                border-bottom: 2px solid {PRIMARY_GREEN};
            }}
            QTabBar::tab:hover {{
                color: {TEXT_PRIMARY};
            }}
            """
        )

        # Tab 0 -- Active Offers (second OrderPanel for bottom panel)
        self._tab_order_panel = self._create_page_widget(OrderPanel, "Active Offers")
        self._bottom_tabs.addTab(self._tab_order_panel, "Active Offers")
        # Tab 1 -- Trade History
        self._trade_log = self._create_page_widget(TradeLogWidget, "Trade History")
        self._bottom_tabs.addTab(self._trade_log, "Trade History")
        # Tab 2 -- Bot Log
        self._bot_log = self._create_page_widget(BotLogWidget, "Bot Log")
        self._bottom_tabs.addTab(self._bot_log, "Bot Log")
        # Tab 3 -- Configuration (live view)
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

        Returns
        -------
        QWidget
            The instantiated widget or a placeholder.
        """
        if widget_class is not None:
            try:
                return widget_class()
            except Exception as exc:
                _log.warning(
                    "Failed to create widget %s: %s",
                    widget_class.__name__,
                    exc,
                )
        return _placeholder_widget(f"{fallback_label} (not yet implemented)")

    # ===================================================================== #
    #  Status bar                                                            #
    # ===================================================================== #

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
        """
        # Page switching: Ctrl+1 through Ctrl+5
        for index in range(5):
            action = QAction(self)
            action.setShortcut(QKeySequence(f"Ctrl+{index + 1}"))
            action.triggered.connect(
                lambda checked, idx=index: self._switch_page(idx)
            )
            self.addAction(action)

    def _switch_page(self, index: int) -> None:
        """Switch both the stacked widget and the sidebar selection.

        Parameters
        ----------
        index : int
            Zero-based page index.
        """
        self._stacked.setCurrentIndex(index)
        self._sidebar.select_page(index)

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
        """Handle File > Connect."""
        self._connected = True
        self._conn_dot.setStyleSheet(f"color: {PRIMARY_GREEN}; font-size: 14px;")
        self._conn_label.setText("Connected")
        self._act_connect.setEnabled(False)
        self._act_disconnect.setEnabled(True)

    def _on_disconnect(self) -> None:
        """Handle File > Disconnect."""
        self._connected = False
        self._conn_dot.setStyleSheet(f"color: {LOSS_RED}; font-size: 14px;")
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
                "Stop Engine",
                "Stop the trading engine? Active offers will be cancelled.",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
        else:
            # Currently stopped -- confirm start
            if self._dry_run:
                prompt = "Start in dry-run mode?"
            else:
                prompt = (
                    "Start the trading engine? "
                    "(Offers will be placed on the CHIA DEX.)"
                )
            reply = QMessageBox.question(
                self,
                "Start Engine",
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
        self._style_start_stop_button()

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

    def _on_toggle_sidebar(self) -> None:
        """Toggle sidebar expansion via View menu or Ctrl+B."""
        self._sidebar.toggle()

    def _on_toggle_statusbar(self) -> None:
        """Toggle visibility of the status bar."""
        bar: QStatusBar = self.statusBar()
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
