"""Widget sub-package for XOPTrader GUI components.

Public API:
    MainWindow      -- top-level application window.
    Sidebar         -- collapsible left-hand navigation rail.
    SidebarButton   -- individual nav button inside the sidebar.
    StatusBar       -- CHIA-branded status bar with live readouts.
    DashboardWidget -- overview page with metrics, status, pairs table.
    MetricCard      -- individual KPI tile with sparkline.
    ChartWidget     -- real-time price / PnL / volume chart.
    OrderPanel      -- active-offers management table.
    TradeLogWidget  -- historical trade viewer with CSV export.
    BotLogWidget    -- live structured log viewer.
"""

from gui.widgets.bot_log import BotLogWidget
from gui.widgets.chart import ChartWidget
from gui.widgets.dashboard import DashboardWidget, MetricCard
from gui.widgets.main_window import MainWindow
from gui.widgets.order_panel import OrderPanel
from gui.widgets.sidebar import Sidebar, SidebarButton
from gui.widgets.status_bar import StatusBar
from gui.widgets.settings import SettingsWidget
from gui.widgets.trade_log import TradeLogWidget

__all__: list[str] = [
    "BotLogWidget",
    "ChartWidget",
    "DashboardWidget",
    "MainWindow",
    "MetricCard",
    "OrderPanel",
    "SettingsWidget",
    "Sidebar",
    "SidebarButton",
    "StatusBar",
    "TradeLogWidget",
]
