"""Dashboard overview widget for XOPTrader GUI.

Presents key trading metrics, connection status, per-pair summaries, and
a live activity feed in a responsive card-based grid layout.  All data
flows in via the public ``update_*`` methods which the service layer calls
on a timer or in response to WebSocket/callback events.

ISO/IEC 27001:2022 -- no credentials or secrets are stored or displayed.
ISO/IEC 5055      -- all public APIs carry type hints and docstrings.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Any, Sequence

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer, Signal, Slot
from PySide6.QtGui import QAction, QColor, QFont
from PySide6.QtWidgets import (
    QAbstractItemView,
    QComboBox,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QScrollBar,
    QSizePolicy,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS


# ---------------------------------------------------------------------------
# CHIA palette aliases (derived from the canonical theme.COLORS singleton)
# ---------------------------------------------------------------------------

PRIMARY_GREEN: str  = COLORS.PRIMARY_GREEN
LIGHT_GREEN: str    = COLORS.LIGHT_GREEN
DARK_BG: str        = COLORS.DARK_BG
PANEL_BG: str       = COLORS.PANEL_BG
ELEVATED_BG: str    = COLORS.ELEVATED_BG
BORDER: str         = COLORS.BORDER
TEXT_PRIMARY: str   = COLORS.TEXT_PRIMARY
TEXT_SECONDARY: str = COLORS.TEXT_SECONDARY
PROFIT_GREEN: str   = COLORS.PROFIT_GREEN
LOSS_RED: str       = COLORS.LOSS_RED
WARNING: str        = COLORS.WARNING_YELLOW
INFO: str           = COLORS.INFO_BLUE

# Monospaced font used across all numeric readouts
_MONO_FAMILY: str = "Consolas, 'Courier New', monospace"

# Maximum number of sparkline data points retained per metric card
_SPARKLINE_MAX_POINTS: int = 100

# Maximum events shown in the activity feed
_ACTIVITY_FEED_MAX: int = 20


# ---------------------------------------------------------------------------
# MetricCard -- small KPI tile with sparkline
# ---------------------------------------------------------------------------

class MetricCard(QFrame):
    """Single metric card with title, value, change indicator, and sparkline.

    The card renders as a fixed-height elevated panel with a subtle border.
    The embedded pyqtgraph ``PlotWidget`` draws the last 100 data points as
    a thin line with no axes or decorations.

    Parameters
    ----------
    title:
        Human-readable metric name (rendered uppercase).
    parent:
        Optional parent QWidget.
    """

    def __init__(self, title: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)

        # --- frame appearance -----------------------------------------------
        self.setFrameShape(QFrame.Shape.StyledPanel)
        self.setFixedHeight(120)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setStyleSheet(
            f"MetricCard {{"
            f"  background-color: {ELEVATED_BG};"
            f"  border: 1px solid {BORDER};"
            f"  border-radius: 6px;"
            f"}}"
        )

        # --- layout ---------------------------------------------------------
        root = QVBoxLayout(self)
        root.setContentsMargins(10, 6, 10, 6)
        root.setSpacing(2)

        # Title row
        self._title_label = QLabel(title.upper())
        self._title_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 10px; font-weight: 600;"
        )
        root.addWidget(self._title_label)

        # Value + change row (side by side)
        value_row = QHBoxLayout()
        value_row.setSpacing(6)

        self._value_label = QLabel("--")
        self._value_label.setStyleSheet(
            f"color: {TEXT_PRIMARY};"
            f"font-family: {_MONO_FAMILY};"
            f"font-size: 18px;"
            f"font-weight: 700;"
        )
        value_row.addWidget(self._value_label)

        self._change_label = QLabel("")
        self._change_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        value_row.addWidget(self._change_label)
        value_row.addStretch()
        root.addLayout(value_row)

        # Sparkline (pyqtgraph, 80px tall, minimal chrome)
        self._sparkline = pg.PlotWidget()
        self._sparkline.setFixedHeight(36)
        self._sparkline.setBackground(ELEVATED_BG)
        self._sparkline.hideAxis("left")
        self._sparkline.hideAxis("bottom")
        self._sparkline.setMouseEnabled(x=False, y=False)
        self._sparkline.hideButtons()
        self._sparkline.setMenuEnabled(False)
        self._sparkline.getViewBox().setDefaultPadding(0)
        self._spark_curve = self._sparkline.plot(
            pen=pg.mkPen(color=PRIMARY_GREEN, width=1.5),
        )
        root.addWidget(self._sparkline)

        # Internal data ring buffer (list kept trimmed to _SPARKLINE_MAX_POINTS)
        self._spark_data: list[float] = []

    # -- Public API ----------------------------------------------------------

    def set_value(self, value: float, fmt: str = "{:+,.2f}") -> None:
        """Update the large numeric readout and colour-code by sign.

        Parameters
        ----------
        value:
            Numeric metric value.
        fmt:
            ``str.format`` pattern applied to *value*.
        """
        text = fmt.format(value)
        colour = PROFIT_GREEN if value >= 0 else LOSS_RED
        self._value_label.setStyleSheet(
            f"color: {colour};"
            f"font-family: {_MONO_FAMILY};"
            f"font-size: 18px;"
            f"font-weight: 700;"
        )
        self._value_label.setText(text)

    def set_change(self, pct: float) -> None:
        """Show the change-indicator arrow and percentage.

        Parameters
        ----------
        pct:
            Percentage change since the prior period (e.g. 1.23 for +1.23 %).
        """
        arrow = "\u25B2" if pct >= 0 else "\u25BC"  # ▲ or ▼
        colour = PROFIT_GREEN if pct >= 0 else LOSS_RED
        self._change_label.setStyleSheet(f"color: {colour}; font-size: 11px;")
        self._change_label.setText(f"{arrow} {abs(pct):.2f}%")

    def append_spark(self, value: float) -> None:
        """Push a new data point to the sparkline ring buffer and redraw.

        The buffer is capped at :data:`_SPARKLINE_MAX_POINTS`; oldest points
        are discarded when the limit is reached.
        """
        self._spark_data.append(value)
        if len(self._spark_data) > _SPARKLINE_MAX_POINTS:
            self._spark_data = self._spark_data[-_SPARKLINE_MAX_POINTS:]
        self._spark_curve.setData(self._spark_data)


# ---------------------------------------------------------------------------
# StatusDot -- tiny coloured circle used in status cards
# ---------------------------------------------------------------------------

class StatusDot(QLabel):
    """Coloured dot indicator (8 x 8 px) rendered via stylesheet border-radius."""

    _COLOUR_MAP: dict[str, str] = {
        "green":  PROFIT_GREEN,
        "red":    LOSS_RED,
        "yellow": WARNING,
        "blue":   INFO,
        "gray":   TEXT_SECONDARY,
    }

    def __init__(
        self, colour: str = "gray", parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.setFixedSize(8, 8)
        self.set_colour(colour)

    def set_colour(self, colour: str) -> None:
        """Apply a named colour from the palette.

        Parameters
        ----------
        colour:
            One of ``"green"``, ``"red"``, ``"yellow"``, ``"blue"``, ``"gray"``.
        """
        hex_val = self._COLOUR_MAP.get(colour, TEXT_SECONDARY)
        self.setStyleSheet(
            f"background-color: {hex_val};"
            f"border-radius: 4px;"
            f"min-width: 8px; max-width: 8px;"
            f"min-height: 8px; max-height: 8px;"
        )


# ---------------------------------------------------------------------------
# StatusCard -- bot / connection / block / pairs overview
# ---------------------------------------------------------------------------

class StatusCard(QFrame):
    """Elevated card used in the status row.

    Provides a title and a vertical content area where the caller can insert
    arbitrary widgets (labels, dots, etc.) via :meth:`add_row`.
    """

    def __init__(self, title: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setFrameShape(QFrame.Shape.StyledPanel)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setStyleSheet(
            f"StatusCard {{"
            f"  background-color: {ELEVATED_BG};"
            f"  border: 1px solid {BORDER};"
            f"  border-radius: 6px;"
            f"}}"
        )

        root = QVBoxLayout(self)
        root.setContentsMargins(10, 8, 10, 8)
        root.setSpacing(4)

        title_label = QLabel(title.upper())
        title_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 10px; font-weight: 600;"
        )
        root.addWidget(title_label)

        # Content area -- caller adds rows here
        self._content = QVBoxLayout()
        self._content.setSpacing(3)
        root.addLayout(self._content)

    def add_row(self, widget: QWidget) -> None:
        """Append *widget* to the content area."""
        self._content.addWidget(widget)


# ---------------------------------------------------------------------------
# _make_dot_label helper
# ---------------------------------------------------------------------------

def _make_dot_label(
    text: str, colour: str = "gray"
) -> tuple[QWidget, StatusDot, QLabel]:
    """Return a (container, dot, label) triple for a dot + text row.

    This is a layout helper so callers can keep references to the dot and
    label for later updates while inserting the container into the card.
    """
    container = QWidget()
    layout = QHBoxLayout(container)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(6)

    dot = StatusDot(colour)
    label = QLabel(text)
    label.setStyleSheet(f"color: {TEXT_PRIMARY}; font-size: 12px;")

    layout.addWidget(dot)
    layout.addWidget(label)
    layout.addStretch()
    return container, dot, label


# ---------------------------------------------------------------------------
# DashboardWidget -- top-level overview page
# ---------------------------------------------------------------------------

class DashboardWidget(QWidget):
    """Full-page dashboard presenting key trading metrics, status, pairs
    summary, and a live activity feed.

    Signals
    -------
    view_chart_requested:
        Emitted when the user selects *View Chart* on a pair row.
    view_orders_requested:
        Emitted when the user selects *View Orders* on a pair row.
    """

    # Custom signals for context-menu actions on the pairs table
    view_chart_requested  = Signal(str)   # pair_name
    view_orders_requested = Signal(str)   # pair_name

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setStyleSheet(f"background-color: {DARK_BG};")

        # Root layout -- scrollable grid
        self._grid = QGridLayout(self)
        self._grid.setContentsMargins(16, 16, 16, 16)
        self._grid.setSpacing(12)

        # ---- Row 0: Metric cards -----------------------------------------
        self._metric_cards: dict[str, MetricCard] = {}
        metric_defs: list[str] = [
            "Total PnL",
            "Realized PnL",
            "Unrealized PnL",
            "Spread PnL",
            "Inventory PnL",
            "24h Fill Count",
        ]
        for col, name in enumerate(metric_defs):
            card = MetricCard(name)
            self._metric_cards[name] = card
            # 3-column layout: row offset = col // 3, column = col % 3
            self._grid.addWidget(card, col // 3, col % 3)

        # ---- Row 1 (grid row 2): Status cards ----------------------------
        self._build_status_cards()

        # ---- Row 2 (grid row 3): Per-pair summary table -------------------
        self._build_pairs_table()

        # ---- Row 3 (grid row 4): Recent activity feed ---------------------
        self._build_activity_feed()

    # =====================================================================
    # Private build helpers
    # =====================================================================

    def _build_status_cards(self) -> None:
        """Construct the four status cards (bot, connection, block, pairs)."""
        status_row = 2  # grid row index

        # --- Bot Status card -----------------------------------------------
        self._bot_card = StatusCard("Bot Status")
        container, self._bot_dot, self._bot_label = _make_dot_label(
            "STOPPED", "red"
        )
        self._bot_card.add_row(container)
        self._grid.addWidget(self._bot_card, status_row, 0)

        # --- Connection Status card ----------------------------------------
        self._conn_card = StatusCard("Connection Status")
        self._conn_dots: dict[str, tuple[StatusDot, QLabel]] = {}
        for svc in ("Full Node", "Wallet", "Dexie"):
            container, dot, label = _make_dot_label(svc, "gray")
            self._conn_card.add_row(container)
            self._conn_dots[svc] = (dot, label)
        self._grid.addWidget(self._conn_card, status_row, 1)

        # --- Current Block card --------------------------------------------
        self._block_card = StatusCard("Current Block")
        self._block_height_label = QLabel("--")
        self._block_height_label.setStyleSheet(
            f"color: {TEXT_PRIMARY};"
            f"font-family: {_MONO_FAMILY};"
            f"font-size: 16px; font-weight: 700;"
        )
        self._block_card.add_row(self._block_height_label)

        self._block_age_label = QLabel("")
        self._block_age_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        self._block_card.add_row(self._block_age_label)
        self._grid.addWidget(self._block_card, status_row, 2)

        # --- Active Pairs card (grid row 3, spans if needed) ---------------
        self._pairs_card = StatusCard("Active Pairs")
        self._pairs_status_container = QVBoxLayout()
        wrapper = QWidget()
        wrapper.setLayout(self._pairs_status_container)
        self._pairs_card.add_row(wrapper)
        self._grid.addWidget(self._pairs_card, status_row + 1, 0)

    def _build_pairs_table(self) -> None:
        """Construct the per-pair summary QTableWidget."""
        self._pairs_table = QTableWidget()
        self._pairs_table.setColumnCount(8)
        self._pairs_table.setHorizontalHeaderLabels([
            "Pair", "Mid Price", "Spread (bps)", "Inventory",
            "Bid", "Ask", "Fills (24h)", "PnL",
        ])
        self._pairs_table.setSortingEnabled(True)
        self._pairs_table.setSelectionBehavior(
            QAbstractItemView.SelectionBehavior.SelectRows
        )
        self._pairs_table.setAlternatingRowColors(True)
        self._pairs_table.setContextMenuPolicy(
            Qt.ContextMenuPolicy.CustomContextMenu
        )
        self._pairs_table.customContextMenuRequested.connect(
            self._on_pairs_context_menu
        )
        self._pairs_table.horizontalHeader().setStretchLastSection(True)
        self._pairs_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )
        self._pairs_table.verticalHeader().setVisible(False)

        # Styling
        self._pairs_table.setStyleSheet(
            f"QTableWidget {{"
            f"  background-color: {PANEL_BG};"
            f"  alternate-background-color: {ELEVATED_BG};"
            f"  color: {TEXT_PRIMARY};"
            f"  border: 1px solid {BORDER};"
            f"  gridline-color: {BORDER};"
            f"  font-family: {_MONO_FAMILY};"
            f"  font-size: 12px;"
            f"}}"
            f"QTableWidget::item:selected {{"
            f"  background-color: {BORDER};"
            f"}}"
            f"QHeaderView::section {{"
            f"  background-color: {ELEVATED_BG};"
            f"  color: {TEXT_SECONDARY};"
            f"  border: 1px solid {BORDER};"
            f"  padding: 4px;"
            f"  font-size: 11px;"
            f"  font-weight: 600;"
            f"}}"
        )

        # Spans all 3 columns in its grid row
        self._grid.addWidget(self._pairs_table, 4, 0, 1, 3)

    def _build_activity_feed(self) -> None:
        """Construct the recent-activity QListWidget with auto-scroll."""
        feed_label = QLabel("RECENT ACTIVITY")
        feed_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 10px; font-weight: 600;"
            f"padding-top: 6px;"
        )
        self._grid.addWidget(feed_label, 5, 0, 1, 3)

        self._activity_list = QListWidget()
        self._activity_list.setStyleSheet(
            f"QListWidget {{"
            f"  background-color: {PANEL_BG};"
            f"  color: {TEXT_PRIMARY};"
            f"  border: 1px solid {BORDER};"
            f"  font-family: {_MONO_FAMILY};"
            f"  font-size: 12px;"
            f"}}"
            f"QListWidget::item {{"
            f"  padding: 4px 8px;"
            f"  border-bottom: 1px solid {BORDER};"
            f"}}"
        )
        self._activity_list.setMaximumHeight(260)

        # Track whether the user has scrolled away from the bottom
        self._auto_scroll: bool = True
        scrollbar: QScrollBar = self._activity_list.verticalScrollBar()
        scrollbar.valueChanged.connect(self._on_feed_scroll)

        self._grid.addWidget(self._activity_list, 6, 0, 1, 3)

    # =====================================================================
    # Context menu for the pairs table
    # =====================================================================

    @Slot("QPoint")
    def _on_pairs_context_menu(self, pos) -> None:
        """Show *View Chart* / *View Orders* context menu on right-click."""
        row = self._pairs_table.rowAt(pos.y())
        if row < 0:
            return

        pair_item = self._pairs_table.item(row, 0)
        if pair_item is None:
            return
        pair_name: str = pair_item.text()

        menu = QMenu(self)
        menu.setStyleSheet(
            f"QMenu {{"
            f"  background-color: {ELEVATED_BG};"
            f"  color: {TEXT_PRIMARY};"
            f"  border: 1px solid {BORDER};"
            f"}}"
            f"QMenu::item:selected {{"
            f"  background-color: {BORDER};"
            f"}}"
        )

        chart_action = QAction("View Chart", self)
        chart_action.triggered.connect(
            lambda checked=False, p=pair_name: self.view_chart_requested.emit(p)
        )
        menu.addAction(chart_action)

        orders_action = QAction("View Orders", self)
        orders_action.triggered.connect(
            lambda checked=False, p=pair_name: self.view_orders_requested.emit(p)
        )
        menu.addAction(orders_action)

        menu.exec(self._pairs_table.viewport().mapToGlobal(pos))

    # =====================================================================
    # Auto-scroll logic for the activity feed
    # =====================================================================

    @Slot(int)
    def _on_feed_scroll(self, value: int) -> None:
        """Pause auto-scroll when the user scrolls up; resume at bottom."""
        scrollbar: QScrollBar = self._activity_list.verticalScrollBar()
        self._auto_scroll = value >= scrollbar.maximum() - 5

    # =====================================================================
    # Public update API
    # =====================================================================

    def update_metrics(self, metrics: dict[str, Any]) -> None:
        """Refresh all metric cards from a flat dictionary.

        Expected keys mirror the card titles::

            {
                "Total PnL":      {"value": 1234.56, "change_pct": 0.5, "spark": 1234.56},
                "Realized PnL":   {...},
                "Unrealized PnL": {...},
                "Spread PnL":     {...},
                "Inventory PnL":  {...},
                "24h Fill Count": {"value": 42, "change_pct": 10.0, "spark": 42},
            }

        Parameters
        ----------
        metrics:
            Mapping of card title to a dict with ``"value"``, optional
            ``"change_pct"``, and optional ``"spark"`` entries.
        """
        for name, card in self._metric_cards.items():
            data = metrics.get(name)
            if data is None:
                continue
            value = data.get("value", 0.0)

            # Fill Count is always shown as an integer
            if "Fill Count" in name:
                card.set_value(value, fmt="{:,.0f}")
            else:
                card.set_value(value)

            if "change_pct" in data:
                card.set_change(data["change_pct"])
            if "spark" in data:
                card.append_spark(data["spark"])

    def update_bot_status(
        self, status: str, *, colour: str = "gray"
    ) -> None:
        """Update the Bot Status card.

        Parameters
        ----------
        status:
            Display string, e.g. ``"Running"``, ``"Stopped"``, ``"DryRun"``.
        colour:
            Dot colour name (``"green"``, ``"red"``, ``"yellow"``).
        """
        self._bot_dot.set_colour(colour)
        self._bot_label.setText(status.upper())
        self._bot_label.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 12px;"
        )

    def update_connection_status(
        self, statuses: dict[str, bool]
    ) -> None:
        """Set green/red dots for each service.

        Parameters
        ----------
        statuses:
            Mapping of service name (``"Full Node"``, ``"Wallet"``,
            ``"Dexie"``) to connection boolean.
        """
        for svc, connected in statuses.items():
            entry = self._conn_dots.get(svc)
            if entry is None:
                continue
            dot, label = entry
            dot.set_colour("green" if connected else "red")

    def update_block_info(
        self, height: int, last_block_ts: float
    ) -> None:
        """Update the Current Block card.

        Parameters
        ----------
        height:
            Latest block height.
        last_block_ts:
            Unix timestamp of the most recent block.
        """
        self._block_height_label.setText(f"{height:,}")
        age_s = time.time() - last_block_ts
        if age_s < 60:
            age_str = f"{age_s:.0f}s ago"
        elif age_s < 3600:
            age_str = f"{age_s / 60:.1f}m ago"
        else:
            age_str = f"{age_s / 3600:.1f}h ago"
        self._block_age_label.setText(age_str)

    def update_active_pairs(
        self, pairs: list[dict[str, Any]]
    ) -> None:
        """Populate the Active Pairs card.

        Parameters
        ----------
        pairs:
            List of ``{"name": "XCH/USDS", "enabled": True}`` dicts.
        """
        # Clear previous rows
        while self._pairs_status_container.count():
            item = self._pairs_status_container.takeAt(0)
            if item.widget():
                item.widget().deleteLater()

        for p in pairs:
            colour = "green" if p.get("enabled", False) else "gray"
            container, _, _ = _make_dot_label(p.get("name", "?"), colour)
            self._pairs_status_container.addWidget(container)

    def update_pairs_table(self, pairs_data: list[dict[str, Any]]) -> None:
        """Refresh the per-pair summary table.

        Parameters
        ----------
        pairs_data:
            Each dict must contain keys matching the table header:
            ``"pair"``, ``"mid_price"``, ``"spread_bps"``, ``"inventory"``,
            ``"bid"``, ``"ask"``, ``"fills_24h"``, ``"pnl"``.
        """
        # Temporarily disable sorting while updating to avoid index conflicts
        self._pairs_table.setSortingEnabled(False)
        self._pairs_table.setRowCount(len(pairs_data))

        for row, data in enumerate(pairs_data):
            values: list[tuple[str, str]] = [
                (data.get("pair", ""),              ""),
                (f"{data.get('mid_price', 0):.6f}", ""),
                (f"{data.get('spread_bps', 0):.1f}",""),
                (f"{data.get('inventory', 0):.4f}", ""),
                (f"{data.get('bid', 0):.6f}",       ""),
                (f"{data.get('ask', 0):.6f}",       ""),
                (f"{data.get('fills_24h', 0):,}",   ""),
                (f"{data.get('pnl', 0):+,.2f}",     ""),
            ]

            for col, (text, _) in enumerate(values):
                item = QTableWidgetItem(text)
                item.setFlags(
                    item.flags() & ~Qt.ItemFlag.ItemIsEditable
                )

                # Colour-code the PnL column
                if col == 7:
                    pnl_val = data.get("pnl", 0)
                    if pnl_val >= 0:
                        item.setForeground(QColor(PROFIT_GREEN))
                    else:
                        item.setForeground(QColor(LOSS_RED))

                self._pairs_table.setItem(row, col, item)

        self._pairs_table.setSortingEnabled(True)

    def update_trades(self, trades: list[dict[str, Any]]) -> None:
        """Append new events to the activity feed.

        Parameters
        ----------
        trades:
            List of event dicts, each containing ``"timestamp"`` (ISO or
            Unix float), ``"icon"`` (emoji / text glyph), and ``"message"``.
            Events are appended in order; the feed is capped at
            :data:`_ACTIVITY_FEED_MAX`.
        """
        for trade in trades:
            ts = trade.get("timestamp", "")
            icon = trade.get("icon", "\u2022")   # bullet fallback
            msg = trade.get("message", "")
            line = f"[{ts}]  {icon}  {msg}"

            item = QListWidgetItem(line)
            item.setForeground(QColor(TEXT_PRIMARY))
            self._activity_list.addItem(item)

        # Enforce maximum count
        while self._activity_list.count() > _ACTIVITY_FEED_MAX:
            self._activity_list.takeItem(0)

        # Auto-scroll if the user is at the bottom
        if self._auto_scroll:
            self._activity_list.scrollToBottom()
