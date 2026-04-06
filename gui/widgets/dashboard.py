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
        self.setFixedHeight(160)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setStyleSheet(
            f"MetricCard {{"
            f"  background-color: {ELEVATED_BG};"
            f"  border: 1px solid {BORDER};"
            f"  border-radius: 0px;"
            f"}}"
        )

        # --- layout ---------------------------------------------------------
        root = QVBoxLayout(self)
        root.setContentsMargins(8, 6, 8, 6)
        root.setSpacing(4)

        # Title row
        self._title_label = QLabel(title.upper())
        self._title_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 12px; font-weight: 600; letter-spacing: 1px;"
        )
        root.addWidget(self._title_label)

        # Value + change row (side by side)
        value_row = QHBoxLayout()
        value_row.setSpacing(12)

        self._value_label = QLabel("--")
        self._value_label.setStyleSheet(
            f"color: {TEXT_PRIMARY};"
            f"font-family: {_MONO_FAMILY};"
            f"font-size: 24px;"
            f"font-weight: 700;"
        )
        value_row.addWidget(self._value_label)

        self._change_label = QLabel("")
        self._change_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 13px;"
        )
        value_row.addWidget(self._change_label)
        value_row.addStretch()
        root.addLayout(value_row)

        # Sparkline (pyqtgraph, 40px tall, minimal chrome)
        self._sparkline = pg.PlotWidget()
        self._sparkline.setFixedHeight(40)
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

        # USD equivalent label (shown below sparkline for monetary cards)
        self._usd_label = QLabel("")
        self._usd_label.setStyleSheet(
            f"color: {TEXT_SECONDARY};"
            f"font-family: {_MONO_FAMILY};"
            f"font-size: 12px;"
        )
        self._usd_label.setVisible(False)
        root.addWidget(self._usd_label)

        # Annotation label (e.g. "No trades yet", "Stale data")
        self._annotation_label = QLabel("")
        self._annotation_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 10px; font-style: italic;"
        )
        self._annotation_label.setVisible(False)
        root.addWidget(self._annotation_label)

        # Internal data ring buffer (list kept trimmed to _SPARKLINE_MAX_POINTS)
        self._spark_data: list[float] = []
        # Track whether this card has ever received a non-zero value.
        self._ever_nonzero: bool = False

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
        if value != 0.0:
            self._ever_nonzero = True
        text = fmt.format(value)
        colour = PROFIT_GREEN if value >= 0 else LOSS_RED
        # Dim the value when it has never been non-zero to signal "no data yet".
        if not self._ever_nonzero and value == 0.0:
            colour = TEXT_SECONDARY
        self._value_label.setStyleSheet(
            f"color: {colour};"
            f"font-family: {_MONO_FAMILY};"
            f"font-size: 22px;"
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

    def set_usd_value(self, usd: float, signed: bool = True) -> None:
        """Show a secondary USD equivalent below the sparkline.

        Parameters
        ----------
        usd:
            Dollar value to display.
        signed:
            If True, prefix with +/- sign.
        """
        if signed:
            text = f"\u2248 ${usd:+,.2f} USD"
        else:
            text = f"\u2248 ${usd:,.2f} USD"
        self._usd_label.setText(text)
        self._usd_label.setVisible(True)

    def set_annotation(self, text: str) -> None:
        """Set or clear the annotation text below the sparkline.

        Parameters
        ----------
        text:
            Annotation string (e.g. "No trades yet"). Empty string hides it.
        """
        if text:
            self._annotation_label.setText(text)
            self._annotation_label.setVisible(True)
        else:
            self._annotation_label.setVisible(False)


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
            f"  border-radius: 12px;"
            f"}}"
        )

        root = QVBoxLayout(self)
        root.setContentsMargins(14, 12, 14, 12)
        root.setSpacing(6)

        title_label = QLabel(title.upper())
        title_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px; font-weight: 600; letter-spacing: 1px;"
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
    label.setStyleSheet(f"color: {TEXT_PRIMARY}; font-size: 13px;")

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
        self._grid.setContentsMargins(8, 8, 8, 8)
        self._grid.setSpacing(6)

        # ---- Row 0: Metric cards -----------------------------------------
        self._metric_cards: dict[str, MetricCard] = {}
        metric_defs: list[str] = [
            "Total PnL",
            "Realized PnL",
            "Unrealized PnL",
            "Spread PnL",
            "Inventory PnL",
            "24h Fill Count",
            "Fees Paid 24h",
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
            f"font-size: 20px; font-weight: 700;"
        )
        self._block_card.add_row(self._block_height_label)

        self._block_age_label = QLabel("")
        self._block_age_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 12px;"
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

        # --- Diagnostics card (grid row 3, column 1) -----------------------
        self._diag_card = StatusCard("Data Diagnostics")
        self._diag_metrics_dot, self._diag_metrics_label = self._add_diag_row(
            "Metrics", "gray",
        )
        self._diag_offers_label = QLabel("Offers: --")
        self._diag_offers_label.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 12px;"
            f" font-family: {_MONO_FAMILY};"
        )
        self._diag_card.add_row(self._diag_offers_label)

        self._diag_fills_label = QLabel("Fills: --")
        self._diag_fills_label.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 12px;"
            f" font-family: {_MONO_FAMILY};"
        )
        self._diag_card.add_row(self._diag_fills_label)

        self._diag_fees_label = QLabel("Fees 24h: --")
        self._diag_fees_label.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 12px;"
            f" font-family: {_MONO_FAMILY};"
        )
        self._diag_card.add_row(self._diag_fees_label)

        self._diag_last_update_label = QLabel("Last update: --")
        self._diag_last_update_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        self._diag_card.add_row(self._diag_last_update_label)
        self._grid.addWidget(self._diag_card, status_row + 1, 1)

        # --- Spendable Reserve card (grid row 3, column 2) -----------------
        self._reserve_card = StatusCard("Spendable Reserve")
        self._reserve_labels: dict[str, QLabel] = {}
        self._reserve_card_container = QVBoxLayout()
        reserve_wrapper = QWidget()
        reserve_wrapper.setLayout(self._reserve_card_container)
        self._reserve_card.add_row(reserve_wrapper)
        self._grid.addWidget(self._reserve_card, status_row + 1, 2)

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
            f"  border-radius: 8px;"
            f"  gridline-color: {BORDER};"
            f"  font-family: {_MONO_FAMILY};"
            f"  font-size: 13px;"
            f"}}"
            f"QTableWidget::item {{"
            f"  padding: 6px 10px;"
            f"  min-height: 28px;"
            f"}}"
            f"QTableWidget::item:selected {{"
            f"  background-color: {BORDER};"
            f"}}"
            f"QHeaderView::section {{"
            f"  background-color: {ELEVATED_BG};"
            f"  color: {TEXT_SECONDARY};"
            f"  border: 1px solid {BORDER};"
            f"  padding: 8px 10px;"
            f"  font-size: 12px;"
            f"  font-weight: 600;"
            f"}}"
        )

        # Spans all 3 columns in its grid row
        self._grid.addWidget(self._pairs_table, 4, 0, 1, 3)

    def _build_activity_feed(self) -> None:
        """Construct the recent-activity QListWidget with auto-scroll."""
        feed_label = QLabel("RECENT ACTIVITY")
        feed_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px; font-weight: 600;"
            f" letter-spacing: 1px; padding-top: 8px;"
        )
        self._grid.addWidget(feed_label, 5, 0, 1, 3)

        self._activity_list = QListWidget()
        self._activity_list.setStyleSheet(
            f"QListWidget {{"
            f"  background-color: {PANEL_BG};"
            f"  color: {TEXT_PRIMARY};"
            f"  border: 1px solid {BORDER};"
            f"  border-radius: 8px;"
            f"  font-family: {_MONO_FAMILY};"
            f"  font-size: 13px;"
            f"}}"
            f"QListWidget::item {{"
            f"  padding: 8px 12px;"
            f"  border-bottom: 1px solid {BORDER};"
            f"}}"
        )
        self._activity_list.setMaximumHeight(300)

        # Track whether the user has scrolled away from the bottom
        self._auto_scroll: bool = True
        scrollbar: QScrollBar = self._activity_list.verticalScrollBar()
        scrollbar.valueChanged.connect(self._on_feed_scroll)

        self._grid.addWidget(self._activity_list, 6, 0, 1, 3)

        # -- Row 7: Wallet Balances card --------------------------------------
        wallet_title = QLabel("Wallet Balances")
        wallet_title.setStyleSheet(f"color: {TEXT_PRIMARY}; font-weight: bold; font-size: 14px;")
        self._grid.addWidget(wallet_title, 7, 0, 1, 3)

        self._wallet_label = QLabel("Waiting for data\u2026")
        self._wallet_label.setWordWrap(True)
        self._wallet_label.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 12px;")
        self._grid.addWidget(self._wallet_label, 8, 0, 1, 3)

    def _add_diag_row(
        self, text: str, colour: str,
    ) -> tuple[StatusDot, QLabel]:
        """Add a dot+label diagnostic row to the diagnostics card."""
        container, dot, label = _make_dot_label(text, colour)
        self._diag_card.add_row(container)
        return dot, label

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

    def update_metrics(
        self,
        metrics: dict[str, Any],
        xch_usd_rate: float = 0.0,
    ) -> None:
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
        xch_usd_rate:
            Current XCH price in USD for conversion display.  When > 0 the
            cards show a secondary USD equivalent line.
        """
        fill_count = 0
        fill_data = metrics.get("24h Fill Count")
        if fill_data is not None:
            fill_count = fill_data.get("value", 0)

        for name, card in self._metric_cards.items():
            data = metrics.get(name)
            if data is None:
                continue
            value = data.get("value", 0.0)

            # Fill Count is always shown as an integer
            if "Fill Count" in name:
                card.set_value(value, fmt="{:,.0f}")
            elif "Fee" in name:
                card.set_value(value, fmt="{:,.4f} XCH")
                if xch_usd_rate > 0:
                    card.set_usd_value(value * xch_usd_rate, signed=False)
            else:
                card.set_value(value, fmt="{:+,.4f} XCH")
                if xch_usd_rate > 0:
                    card.set_usd_value(value * xch_usd_rate)

            if "change_pct" in data:
                card.set_change(data["change_pct"])
            if "spark" in data:
                card.append_spark(data["spark"])

            # Annotate PnL cards when no fills have occurred.
            if "PnL" in name:
                if fill_count == 0 and value == 0.0:
                    card.set_annotation("No trades filled yet")
                else:
                    card.set_annotation("")
            elif "Fill Count" in name and fill_count == 0:
                card.set_annotation("Awaiting first fill")

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

        When *height* is 0 or *last_block_ts* is 0 the block chain
        state is unknown; the card shows placeholder text instead of
        computing a nonsensical age from epoch zero.

        Parameters
        ----------
        height:
            Latest block height.
        last_block_ts:
            Unix timestamp of the most recent block.
        """
        # Guard: unknown block state produces placeholder display.
        if height == 0 or last_block_ts == 0.0:
            self._block_height_label.setText("Block: --")
            self._block_age_label.setText("-- ago")
            return

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

    def update_pairs_table(
        self,
        pairs_data: list[dict[str, Any]],
        xch_usd_rate: float = 0.0,
    ) -> None:
        """Refresh the per-pair summary table.

        Parameters
        ----------
        pairs_data:
            Each dict must contain keys matching the table header:
            ``"pair"``, ``"mid_price"``, ``"spread_bps"``, ``"inventory"``,
            ``"bid"``, ``"ask"``, ``"fills_24h"``, ``"pnl"``.
        xch_usd_rate:
            Current XCH price in USD.  When > 0 the PnL column includes a
            USD equivalent.
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
            ]
            # PnL column: append USD equivalent when rate is available
            pnl_val = data.get('pnl', 0)
            pnl_text = f"{pnl_val:+,.4f} XCH"
            if xch_usd_rate > 0:
                pnl_text += f" (${pnl_val * xch_usd_rate:+,.2f})"
            values.append((pnl_text, ""))

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

    def update_wallet_balances(
        self,
        balances: dict[str, dict[str, float]],
        reserve: dict[str, float] | None = None,
        stuck_offers: int = 0,
    ) -> None:
        """Update the wallet balances section of the dashboard.

        Parameters
        ----------
        balances:
            Mapping of wallet label to a dict with ``spendable``,
            ``confirmed``, ``pending_change``, etc.
        reserve:
            Optional mapping of wallet label to spendable reserve ratio (0–1).
        stuck_offers:
            Number of stuck offers (beyond TTL + stuck-age threshold).
        """
        if not hasattr(self, "_wallet_label"):
            return

        lines: list[str] = []
        for wallet, bal in balances.items():
            spendable = bal.get("spendable", 0.0)
            confirmed = bal.get("confirmed", 0.0)
            pending = bal.get("pending_change", 0.0)

            # Reserve percentage for this wallet.
            res_pct = (reserve or {}).get(wallet, 1.0) * 100.0

            # Color-code based on reserve level.
            if res_pct < 10.0:
                color = "#ff4444"
            elif res_pct < 25.0:
                color = "#ffaa00"
            else:
                color = "#44ff44"

            detail = (
                f"<span style='color:{color}'><b>{wallet}</b></span>: "
                f"spendable={spendable:,.0f}  confirmed={confirmed:,.0f}  "
                f"pending={pending:,.0f}  Reserve: {res_pct:.0f}%"
            )
            lines.append(detail)

        if stuck_offers > 0:
            lines.append(
                f"<span style='color:#ff4444'>\u26a0 {stuck_offers} stuck offer(s) "
                f"beyond TTL — check fee levels</span>"
            )

        self._wallet_label.setText("<br>".join(lines) if lines else "No data")

    def update_diagnostics(
        self,
        *,
        metrics_connected: bool,
        filled: int,
        cancelled: int,
        expired: int,
        pending: int,
        fees_24h_xch: float,
    ) -> None:
        """Update the data diagnostics card.

        Parameters
        ----------
        metrics_connected:
            Whether the Prometheus metrics endpoint is reachable.
        filled:
            Total offers filled since engine start.
        cancelled:
            Total offers cancelled since engine start.
        expired:
            Total offers expired since engine start.
        pending:
            Currently pending offers.
        fees_24h_xch:
            Rolling 24h fees paid in XCH.
        """
        if not hasattr(self, "_diag_card"):
            return

        # Metrics connection dot.
        if metrics_connected:
            self._diag_metrics_dot.set_colour("green")
            self._diag_metrics_label.setText("Metrics: Connected")
        else:
            self._diag_metrics_dot.set_colour("red")
            self._diag_metrics_label.setText("Metrics: Disconnected")

        # Offer lifecycle.
        self._diag_offers_label.setText(
            f"Offers: {pending} pending / {cancelled} cancelled / {expired} expired"
        )

        fill_color = PROFIT_GREEN if filled > 0 else LOSS_RED
        self._diag_fills_label.setText(f"Fills: {filled}")
        self._diag_fills_label.setStyleSheet(
            f"color: {fill_color}; font-size: 12px;"
            f" font-family: {_MONO_FAMILY};"
        )

        # Fees.
        self._diag_fees_label.setText(f"Fees 24h: {fees_24h_xch:.6f} XCH")

        # Timestamp.
        import datetime as _dt

        now_str = _dt.datetime.now().strftime("%H:%M:%S")
        self._diag_last_update_label.setText(f"Last update: {now_str}")

    def update_reserve_card(
        self,
        reserve: dict[str, float],
    ) -> None:
        """Update the spendable reserve card.

        Parameters
        ----------
        reserve:
            Mapping of wallet label to reserve ratio (0.0–1.0).
        """
        if not hasattr(self, "_reserve_card"):
            return

        # Clear previous labels.
        for lbl in self._reserve_labels.values():
            self._reserve_card_container.removeWidget(lbl)
            lbl.deleteLater()
        self._reserve_labels.clear()

        for wallet, ratio in reserve.items():
            pct = ratio * 100.0
            if pct < 10.0:
                colour = LOSS_RED
            elif pct < 25.0:
                colour = "#ffaa00"
            else:
                colour = PROFIT_GREEN

            short_name = wallet[:8] + "\u2026" if len(wallet) > 12 else wallet
            lbl = QLabel(f"{short_name}: {pct:.0f}%")
            lbl.setStyleSheet(
                f"color: {colour}; font-size: 12px;"
                f" font-family: {_MONO_FAMILY};"
            )
            lbl.setToolTip(wallet)
            self._reserve_card_container.addWidget(lbl)
            self._reserve_labels[wallet] = lbl
