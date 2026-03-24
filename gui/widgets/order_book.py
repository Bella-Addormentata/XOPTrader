"""Order-book depth visualisation widget for XOPTrader GUI.

Provides a two-sided order-book table (bids/asks) alongside a pyqtgraph
depth chart.  The bot's own orders are highlighted in both views so the
operator can see exactly where the market-maker is positioned relative
to the rest of the book.

All monetary values are stored and transmitted as **mojos** (int64) and
formatted for display via :func:`gui.utils.mojos_to_xch`.

ISO/IEC 27001:2022 -- no credentials or secrets are handled.
ISO/IEC 5055      -- all public APIs carry type hints and docstrings.
ISO/IEC 25000     -- widget degrades gracefully on empty or partial data.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Optional

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt, Signal, Slot
from PySide6.QtGui import QBrush, QColor, QFont, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QSpinBox,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C
from gui.utils import MOJOS_PER_XCH, mojos_to_xch, mojos_to_xch_float

# ---------------------------------------------------------------------------
# pyqtgraph defaults matching the CHIA dark theme
# ---------------------------------------------------------------------------
pg.setConfigOptions(antialias=True, background=_C.DARK_BG, foreground=_C.TEXT_PRIMARY)

# ---------------------------------------------------------------------------
# Aggregation basis-point presets (label -> bps value, 0 = no aggregation)
# ---------------------------------------------------------------------------
_AGGREGATION_OPTIONS: list[tuple[str, int]] = [
    ("None",    0),
    ("10 bps",  10),
    ("25 bps",  25),
    ("50 bps",  50),
    ("100 bps", 100),
]

# ---------------------------------------------------------------------------
# Table column indices (for readability in table-population code)
# ---------------------------------------------------------------------------
_COL_PRICE:      int = 0
_COL_SIZE:       int = 1
_COL_CUMULATIVE: int = 2
_COL_MY_SIZE:    int = 3

# Column header labels and minimum widths.
_COLUMNS: list[tuple[str, int]] = [
    ("Price (XCH)", 130),
    ("Size",        110),
    ("Cumulative",  110),
    ("My Size",      90),
]


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass(slots=True)
class OrderBookLevel:
    """Single price level inside the order book.

    Attributes
    ----------
    price_mojos:
        Price in mojos (int64).
    size_mojos:
        Total size available at this price level.
    cumulative_mojos:
        Running cumulative from best price outward.
    is_own:
        ``True`` when the bot has at least one order at this level.
    own_size_mojos:
        Size of the bot's own order(s) at this level (0 if not own).
    tier:
        Bot's tier index for this level (-1 if not own).
    """

    price_mojos: int = 0
    size_mojos: int = 0
    cumulative_mojos: int = 0
    is_own: bool = False
    own_size_mojos: int = 0
    tier: int = -1


@dataclass(slots=True)
class OrderBookSnapshot:
    """Immutable snapshot of the current order-book state.

    Attributes
    ----------
    pair_name:
        Trading pair label (e.g. "XCH/USDS").
    mid_price_mojos:
        Midpoint price in mojos.
    spread_bps:
        Spread expressed in basis points.
    bids:
        Bid levels sorted best (highest price) first.
    asks:
        Ask levels sorted best (lowest price) first.
    timestamp:
        Unix epoch timestamp of this snapshot.
    """

    pair_name: str = ""
    mid_price_mojos: int = 0
    spread_bps: float = 0.0
    bids: list[OrderBookLevel] = field(default_factory=list)
    asks: list[OrderBookLevel] = field(default_factory=list)
    timestamp: float = 0.0


# ---------------------------------------------------------------------------
# Monospaced / right-aligned table-item helper
# ---------------------------------------------------------------------------

def _mono_item(text: str, *, fg: str = _C.TEXT_PRIMARY) -> QTableWidgetItem:
    """Create a right-aligned, monospaced QTableWidgetItem.

    Parameters
    ----------
    text:
        Display string.
    fg:
        Foreground colour (CSS hex).

    Returns
    -------
    Configured QTableWidgetItem (non-editable).
    """
    item = QTableWidgetItem(text)
    item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
    item.setForeground(QColor(fg))
    # Monospaced font for numeric data readability.
    font = QFont("JetBrains Mono", 10)
    font.setStyleHint(QFont.StyleHint.Monospace)
    item.setFont(font)
    # Prevent accidental edits.
    item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
    return item


# ===================================================================
# OrderBookWidget
# ===================================================================

class OrderBookWidget(QWidget):
    """Combined order-book table and depth chart for CHIA DEX pairs.

    Signals
    -------
    pair_changed(str):
        Emitted when the user selects a different trading pair.
    level_clicked(int, str):
        Emitted when a table row is clicked.  Carries the price in
        mojos and the side ("bid" or "ask").
    """

    # -- Qt signals --
    pair_changed = Signal(str)
    level_clicked = Signal(int, str)  # (price_mojos, side)

    # -----------------------------------------------------------------
    # Construction
    # -----------------------------------------------------------------

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        """Initialise the order-book widget.

        Parameters
        ----------
        parent:
            Optional parent widget for Qt ownership.
        """
        super().__init__(parent)

        # Internal snapshot cache (replaced on each update_book call).
        self._snapshot: Optional[OrderBookSnapshot] = None
        # Separate own-order overlay list for set_own_orders().
        self._own_orders: list[dict] = []
        # Depth-level count currently in effect.
        self._depth_levels: int = 20
        # Whether the own-order overlay is visible.
        self._show_own: bool = True

        # Build the complete widget tree.
        self._build_ui()

    # -----------------------------------------------------------------
    # UI construction
    # -----------------------------------------------------------------

    def _build_ui(self) -> None:
        """Assemble toolbar, table, and depth chart into the layout."""
        root_layout = QVBoxLayout(self)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(4)

        # -- Top toolbar --
        root_layout.addLayout(self._build_toolbar())

        # -- Horizontal splitter: table (left) | chart (right) --
        self._splitter = QSplitter(Qt.Orientation.Horizontal, self)
        self._splitter.setHandleWidth(3)

        # Left pane: order-book table
        self._table = self._build_table()
        self._splitter.addWidget(self._table)

        # Right pane: depth chart (pyqtgraph PlotWidget)
        self._chart_container = self._build_depth_chart()
        self._splitter.addWidget(self._chart_container)

        # Equal 50/50 split.
        self._splitter.setSizes([500, 500])
        root_layout.addWidget(self._splitter, stretch=1)

    # -- Toolbar -------------------------------------------------------

    def _build_toolbar(self) -> QHBoxLayout:
        """Create the top toolbar with controls.

        Returns
        -------
        Configured QHBoxLayout ready to be added to the root layout.
        """
        toolbar = QHBoxLayout()
        toolbar.setContentsMargins(6, 4, 6, 4)
        toolbar.setSpacing(8)

        # Pair selector
        lbl_pair = QLabel("Pair:")
        self._combo_pair = QComboBox()
        self._combo_pair.setMinimumWidth(120)
        self._combo_pair.currentTextChanged.connect(self._on_pair_changed)
        toolbar.addWidget(lbl_pair)
        toolbar.addWidget(self._combo_pair)

        # Depth-levels spinner
        lbl_depth = QLabel("Depth:")
        self._spin_depth = QSpinBox()
        self._spin_depth.setRange(5, 50)
        self._spin_depth.setValue(self._depth_levels)
        self._spin_depth.setToolTip("Number of price levels to display (5-50)")
        self._spin_depth.valueChanged.connect(self._on_depth_changed)
        toolbar.addWidget(lbl_depth)
        toolbar.addWidget(self._spin_depth)

        # Aggregation selector
        lbl_agg = QLabel("Agg:")
        self._combo_agg = QComboBox()
        for label, _bps in _AGGREGATION_OPTIONS:
            self._combo_agg.addItem(label, _bps)
        self._combo_agg.setToolTip("Price aggregation in basis points")
        self._combo_agg.currentIndexChanged.connect(self._on_aggregation_changed)
        toolbar.addWidget(lbl_agg)
        toolbar.addWidget(self._combo_agg)

        # Auto-refresh toggle
        self._chk_auto_refresh = QCheckBox("Auto-refresh")
        self._chk_auto_refresh.setChecked(True)
        self._chk_auto_refresh.setToolTip("Automatically refresh the book on new data")
        toolbar.addWidget(self._chk_auto_refresh)

        # Highlight own orders toggle
        self._chk_highlight = QCheckBox("Highlight My Orders")
        self._chk_highlight.setChecked(True)
        self._chk_highlight.setToolTip("Show/hide own-order overlay on table and chart")
        self._chk_highlight.stateChanged.connect(self._on_highlight_toggled)
        toolbar.addWidget(self._chk_highlight)

        # Push remaining space to the right so widgets sit on the left.
        toolbar.addStretch(1)

        return toolbar

    # -- Order-book table ----------------------------------------------

    def _build_table(self) -> QTableWidget:
        """Construct the two-sided order-book QTableWidget.

        Returns
        -------
        Configured QTableWidget (empty; populated by update_book).
        """
        table = QTableWidget(0, len(_COLUMNS), self)
        table.setObjectName("orderBookTable")
        table.setAlternatingRowColors(True)

        # Configure header labels and minimum column widths.
        headers = [label for label, _w in _COLUMNS]
        table.setHorizontalHeaderLabels(headers)
        header = table.horizontalHeader()
        for col_idx, (_label, width) in enumerate(_COLUMNS):
            header.resizeSection(col_idx, width)
        # Stretch the price column to fill remaining space.
        header.setSectionResizeMode(
            _COL_PRICE, QHeaderView.ResizeMode.Stretch
        )

        # Disable horizontal scrollbar; vertical scroll suffices.
        table.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff
        )
        table.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded
        )

        # Selection & interaction settings.
        table.setSelectionBehavior(
            QTableWidget.SelectionBehavior.SelectRows
        )
        table.setSelectionMode(
            QTableWidget.SelectionMode.SingleSelection
        )
        table.verticalHeader().setVisible(False)

        # Emit level_clicked when a row is clicked.
        table.cellClicked.connect(self._on_table_cell_clicked)

        return table

    # -- Depth chart (pyqtgraph) ---------------------------------------

    def _build_depth_chart(self) -> QWidget:
        """Build the pyqtgraph depth-chart panel.

        Returns
        -------
        Container QWidget holding the PlotWidget and legend.
        """
        container = QWidget(self)
        layout = QVBoxLayout(container)
        layout.setContentsMargins(0, 0, 0, 0)

        # Create the PlotWidget with CHIA dark background.
        self._plot_widget = pg.PlotWidget(
            background=QColor(_C.DARK_BG),
        )
        self._plot_widget.setMouseEnabled(x=True, y=True)
        self._plot_widget.showGrid(x=True, y=True, alpha=0.15)

        # Configure axes.
        plot_item: pg.PlotItem = self._plot_widget.getPlotItem()
        plot_item.setLabel("bottom", "Price (XCH)")
        plot_item.setLabel("left", "Cumulative Size (XCH)")

        # Style the grid lines to match CHIA border colour.
        for axis_name in ("bottom", "left"):
            axis = plot_item.getAxis(axis_name)
            axis.setPen(pg.mkPen(color=_C.BORDER, width=1))
            axis.setTextPen(pg.mkPen(color=_C.TEXT_SECONDARY))

        # --- Persistent plot items (re-used on each redraw) ---

        # Bid depth bars (green).
        self._bid_bars = pg.BarGraphItem(
            x=[], height=[], width=0.0001,
            brush=pg.mkBrush(QColor(_C.PROFIT_GREEN)),
            pen=pg.mkPen(None),
            name="Bids",
        )
        self._plot_widget.addItem(self._bid_bars)

        # Ask depth bars (red).
        self._ask_bars = pg.BarGraphItem(
            x=[], height=[], width=0.0001,
            brush=pg.mkBrush(QColor(_C.LOSS_RED)),
            pen=pg.mkPen(None),
            name="Asks",
        )
        self._plot_widget.addItem(self._ask_bars)

        # Own-order overlay bars (bright CHIA green with 80% opacity).
        own_color = QColor(_C.LIGHT_GREEN)
        own_color.setAlphaF(0.80)
        self._own_bars = pg.BarGraphItem(
            x=[], height=[], width=0.0001,
            brush=pg.mkBrush(own_color),
            pen=pg.mkPen(QColor("#FFFFFF"), width=0.5),
            name="My Orders",
        )
        self._plot_widget.addItem(self._own_bars)

        # Mid-price vertical reference line (white, dashed).
        self._mid_line = pg.InfiniteLine(
            pos=0,
            angle=90,
            pen=pg.mkPen(color="#FFFFFF", width=1, style=Qt.PenStyle.DashLine),
            label="Mid",
            labelOpts={
                "position": 0.95,
                "color": QColor(_C.TEXT_PRIMARY),
                "fill": QColor(_C.PANEL_BG),
            },
        )
        self._plot_widget.addItem(self._mid_line)

        # Crosshair (vertical + horizontal thin grey lines).
        self._crosshair_v = pg.InfiniteLine(
            angle=90, movable=False,
            pen=pg.mkPen(color=_C.TEXT_SECONDARY, width=0.5,
                         style=Qt.PenStyle.DotLine),
        )
        self._crosshair_h = pg.InfiniteLine(
            angle=0, movable=False,
            pen=pg.mkPen(color=_C.TEXT_SECONDARY, width=0.5,
                         style=Qt.PenStyle.DotLine),
        )
        self._plot_widget.addItem(self._crosshair_v, ignoreBounds=True)
        self._plot_widget.addItem(self._crosshair_h, ignoreBounds=True)

        # Crosshair readout label (top-right corner).
        self._crosshair_label = pg.TextItem(
            text="", anchor=(1, 0),
            color=QColor(_C.TEXT_PRIMARY),
            fill=QColor(_C.PANEL_BG),
        )
        self._plot_widget.addItem(self._crosshair_label, ignoreBounds=True)

        # Connect mouse-move for crosshair tracking.
        self._plot_widget.scene().sigMouseMoved.connect(
            self._on_mouse_moved
        )

        # Legend (upper-left).
        legend = plot_item.addLegend(
            offset=(10, 10),
            brush=pg.mkBrush(QColor(_C.PANEL_BG)),
            pen=pg.mkPen(color=_C.BORDER),
            labelTextColor=QColor(_C.TEXT_PRIMARY),
        )
        # Force legend items to reflect the bar-graph names.
        legend.clear()
        legend.addItem(self._bid_bars, "Market Depth (Bids)")
        legend.addItem(self._ask_bars, "Market Depth (Asks)")
        legend.addItem(self._own_bars, "My Orders")

        layout.addWidget(self._plot_widget, stretch=1)
        return container

    # -----------------------------------------------------------------
    # Public API
    # -----------------------------------------------------------------

    @Slot(object)
    def update_book(self, snapshot: OrderBookSnapshot) -> None:
        """Replace the displayed order book with *snapshot*.

        Parameters
        ----------
        snapshot:
            Complete order-book snapshot to render.
        """
        # Skip redundant redraws when auto-refresh is disabled.
        if not self._chk_auto_refresh.isChecked():
            return

        self._snapshot = snapshot
        self._refresh_table()
        self._refresh_chart()

    def set_pair(self, pair_name: str) -> None:
        """Programmatically switch the active pair.

        Parameters
        ----------
        pair_name:
            Pair label (e.g. "XCH/USDS").  If it does not exist in the
            combo box it will be added automatically.
        """
        idx = self._combo_pair.findText(pair_name)
        if idx < 0:
            self._combo_pair.addItem(pair_name)
            idx = self._combo_pair.findText(pair_name)
        self._combo_pair.setCurrentIndex(idx)

    def set_own_orders(self, orders: list[dict]) -> None:
        """Update the bot's own-order overlay data.

        Parameters
        ----------
        orders:
            List of dicts, each with keys:
            ``price_mojos`` (int), ``size_mojos`` (int),
            ``side`` ("bid" | "ask"), ``tier`` (int).
        """
        self._own_orders = list(orders)
        # Re-render if a snapshot is already loaded.
        if self._snapshot is not None:
            self._refresh_table()
            self._refresh_chart()

    def set_depth_levels(self, n: int) -> None:
        """Change visible depth without touching the spinner.

        Parameters
        ----------
        n:
            New depth level count (clamped to 5-50).
        """
        clamped = max(5, min(50, n))
        self._depth_levels = clamped
        self._spin_depth.blockSignals(True)
        self._spin_depth.setValue(clamped)
        self._spin_depth.blockSignals(False)
        # Re-render with new depth.
        if self._snapshot is not None:
            self._refresh_table()
            self._refresh_chart()

    def add_pair(self, pair_name: str) -> None:
        """Add a pair to the combo box if not already present.

        Parameters
        ----------
        pair_name:
            Pair label to insert.
        """
        if self._combo_pair.findText(pair_name) < 0:
            self._combo_pair.addItem(pair_name)

    def update_market_data(self, data: dict) -> None:
        """Accept a market-data dict from the bridge and update the book.

        The bridge emits a flat dict keyed by pair name, e.g.::

            {
                "XCH/USDS": {
                    "best_bid": 12345,
                    "best_ask": 12346,
                    "mid_price": 12345,
                    "spread_bps": 5.0,
                    ...
                },
                ...
            }

        This method looks up the currently selected pair, extracts its
        sub-dict, builds an :class:`OrderBookSnapshot` with whatever
        fields are available, and forwards it to :meth:`update_book`.

        Parameters
        ----------
        data:
            Per-pair market data dict from the engine bridge.
        """
        if not data:
            return

        # Determine which pair is currently active in the selector.
        current_pair: str = self._combo_pair.currentText()
        if not current_pair:
            return

        pair_data: dict = data.get(current_pair, {})
        if not pair_data:
            return

        # Build an OrderBookSnapshot from the available fields.
        # The bridge provides integer mojo values for prices and
        # float basis-point values for the spread.
        mid_price = int(pair_data.get("mid_price", 0))
        spread_bps = float(pair_data.get("spread_bps", 0.0))

        # Reconstruct bid/ask levels if the bridge supplies them.
        bids: list[OrderBookLevel] = []
        asks: list[OrderBookLevel] = []

        for raw_lvl in pair_data.get("bids", []):
            bids.append(OrderBookLevel(
                price_mojos=int(raw_lvl.get("price_mojos", 0)),
                size_mojos=int(raw_lvl.get("size_mojos", 0)),
                cumulative_mojos=int(raw_lvl.get("cumulative_mojos", 0)),
                is_own=bool(raw_lvl.get("is_own", False)),
                own_size_mojos=int(raw_lvl.get("own_size_mojos", 0)),
                tier=int(raw_lvl.get("tier", -1)),
            ))

        for raw_lvl in pair_data.get("asks", []):
            asks.append(OrderBookLevel(
                price_mojos=int(raw_lvl.get("price_mojos", 0)),
                size_mojos=int(raw_lvl.get("size_mojos", 0)),
                cumulative_mojos=int(raw_lvl.get("cumulative_mojos", 0)),
                is_own=bool(raw_lvl.get("is_own", False)),
                own_size_mojos=int(raw_lvl.get("own_size_mojos", 0)),
                tier=int(raw_lvl.get("tier", -1)),
            ))

        snapshot = OrderBookSnapshot(
            pair_name=current_pair,
            mid_price_mojos=mid_price,
            spread_bps=spread_bps,
            bids=bids,
            asks=asks,
            timestamp=float(pair_data.get("timestamp", 0.0)),
        )
        self.update_book(snapshot)

    # -----------------------------------------------------------------
    # Internal: slot handlers
    # -----------------------------------------------------------------

    @Slot(str)
    def _on_pair_changed(self, pair_name: str) -> None:
        """Handle user selecting a different pair in the combo box."""
        self.pair_changed.emit(pair_name)

    @Slot(int)
    def _on_depth_changed(self, value: int) -> None:
        """Handle depth-spinner value change."""
        self._depth_levels = value
        if self._snapshot is not None:
            self._refresh_table()
            self._refresh_chart()

    @Slot(int)
    def _on_aggregation_changed(self, _index: int) -> None:
        """Handle aggregation combo change -- triggers full re-render."""
        if self._snapshot is not None:
            self._refresh_table()
            self._refresh_chart()

    @Slot(int)
    def _on_highlight_toggled(self, state: int) -> None:
        """Toggle visibility of the own-order overlay.

        Uses ``isChecked()`` instead of comparing the raw *state*
        parameter to avoid int-vs-enum mismatches across PySide6
        versions (ISO/IEC 5055 -- defensive type handling).
        """
        self._show_own = self._chk_highlight.isChecked()
        # Immediately toggle the chart overlay visibility.
        self._own_bars.setVisible(self._show_own)
        # Re-render the table to add/remove My Size column content.
        if self._snapshot is not None:
            self._refresh_table()

    @Slot(int, int)
    def _on_table_cell_clicked(self, row: int, _col: int) -> None:
        """Emit level_clicked when a row is clicked in the table."""
        # Retrieve stored price / side from the row (first column's data).
        item = self._table.item(row, _COL_PRICE)
        if item is None:
            return
        price = item.data(Qt.ItemDataRole.UserRole)
        side = item.data(Qt.ItemDataRole.UserRole + 1)
        if price is not None and side is not None:
            self.level_clicked.emit(int(price), str(side))

    @Slot(object)
    def _on_mouse_moved(self, pos) -> None:
        """Update crosshair lines and readout label on mouse move."""
        plot_item: pg.PlotItem = self._plot_widget.getPlotItem()
        vb = plot_item.vb
        if not vb.sceneBoundingRect().contains(pos):
            return
        mouse_point = vb.mapSceneToView(pos)
        px = mouse_point.x()
        py = mouse_point.y()
        self._crosshair_v.setPos(px)
        self._crosshair_h.setPos(py)
        # Format readout.
        self._crosshair_label.setText(
            f"Price: {px:.6f} XCH\nSize:  {py:.4f} XCH"
        )
        # Anchor readout near the upper-right of the visible range.
        view_range = vb.viewRange()
        self._crosshair_label.setPos(view_range[0][1], view_range[1][1])

    # -----------------------------------------------------------------
    # Internal: aggregation helpers
    # -----------------------------------------------------------------

    def _selected_agg_bps(self) -> int:
        """Return the currently selected aggregation in basis points."""
        return int(self._combo_agg.currentData() or 0)

    def _aggregate_levels(
        self,
        levels: list[OrderBookLevel],
        bps: int,
        *,
        is_bid: bool,
    ) -> list[OrderBookLevel]:
        """Aggregate raw levels into coarser price buckets.

        Parameters
        ----------
        levels:
            Raw order-book levels (sorted best-first).
        bps:
            Aggregation width in basis points.  0 means no aggregation.
        is_bid:
            ``True`` for bids (rounds price *down* to bucket boundary);
            ``False`` for asks (rounds price *up*).

        Returns
        -------
        New list of aggregated OrderBookLevel objects, sorted best-first.
        """
        if bps <= 0 or not levels:
            return levels

        # Aggregation bucket width as a fraction (e.g. 25 bps -> 0.0025).
        bucket_frac: float = bps / 10_000.0

        buckets: dict[int, OrderBookLevel] = {}
        for lvl in levels:
            # Compute bucket boundary price in mojos.
            price_xch = mojos_to_xch_float(lvl.price_mojos)
            if price_xch <= 0:
                continue
            # Round to the nearest bucket boundary.
            bucket_width_xch = price_xch * bucket_frac
            if bucket_width_xch <= 0:
                continue
            if is_bid:
                # Bids: round down to lower bucket boundary.
                bucket_xch = math.floor(price_xch / bucket_width_xch) * bucket_width_xch
            else:
                # Asks: round up to upper bucket boundary.
                bucket_xch = math.ceil(price_xch / bucket_width_xch) * bucket_width_xch

            # Convert bucket boundary back to mojos (integer key).
            bucket_mojos = int(round(bucket_xch * MOJOS_PER_XCH))

            if bucket_mojos not in buckets:
                buckets[bucket_mojos] = OrderBookLevel(
                    price_mojos=bucket_mojos,
                    size_mojos=0,
                    cumulative_mojos=0,
                    is_own=False,
                    own_size_mojos=0,
                    tier=-1,
                )
            agg = buckets[bucket_mojos]
            agg.size_mojos += lvl.size_mojos
            if lvl.is_own:
                agg.is_own = True
                agg.own_size_mojos += lvl.own_size_mojos
                # Keep the lowest tier if multiple tiers aggregate.
                if agg.tier < 0 or lvl.tier < agg.tier:
                    agg.tier = lvl.tier

        # Sort buckets best-first (bids descending, asks ascending).
        result = list(buckets.values())
        result.sort(key=lambda l: l.price_mojos, reverse=is_bid)

        # Recompute cumulative from best outward.
        running = 0
        for lvl in result:
            running += lvl.size_mojos
            lvl.cumulative_mojos = running

        return result

    # -----------------------------------------------------------------
    # Internal: table rendering
    # -----------------------------------------------------------------

    def _refresh_table(self) -> None:
        """Re-populate the QTableWidget from the cached snapshot."""
        snap = self._snapshot
        if snap is None:
            return

        agg_bps = self._selected_agg_bps()
        depth = self._depth_levels

        # Aggregate and trim to visible depth.
        bids = self._aggregate_levels(snap.bids, agg_bps, is_bid=True)[:depth]
        asks = self._aggregate_levels(snap.asks, agg_bps, is_bid=False)[:depth]

        # Total rows = asks (top, reversed so worst ask is row 0) + 1 mid-row + bids (bottom).
        ask_count = len(asks)
        bid_count = len(bids)
        total_rows = ask_count + 1 + bid_count  # +1 for mid-price row

        self._table.setRowCount(total_rows)

        # Determine maximum size for proportional bar widths.
        all_sizes = [lvl.size_mojos for lvl in bids + asks]
        max_size = max(all_sizes) if all_sizes else 1

        # --- Asks (top section, worst -> best = reversed) ---
        # Display asks in reverse order: worst (highest price) at top, best (lowest) near mid.
        for display_row, lvl in enumerate(reversed(asks)):
            self._populate_level_row(
                row=display_row,
                level=lvl,
                side="ask",
                max_size=max_size,
                color_price=_C.LOSS_RED,
                color_bar=_C.LOSS_RED,
            )

        # --- Mid-price row (separator) ---
        mid_row = ask_count
        spread_xch = mojos_to_xch_float(snap.mid_price_mojos) * (snap.spread_bps / 10_000.0)
        spread_text = (
            f"Spread: {snap.spread_bps:.0f} bps "
            f"({mojos_to_xch(int(round(spread_xch * 1_000_000_000_000)), decimals=4)} XCH)"
        )
        mid_item = QTableWidgetItem(
            f"  Mid: {mojos_to_xch(snap.mid_price_mojos, decimals=6)}  |  {spread_text}"
        )
        mid_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
        mid_item.setForeground(QColor(_C.TEXT_PRIMARY))
        mid_item.setBackground(QColor(_C.ELEVATED_BG))
        mid_font = QFont("JetBrains Mono", 10)
        mid_font.setStyleHint(QFont.StyleHint.Monospace)
        mid_font.setBold(True)
        mid_item.setFont(mid_font)
        mid_item.setFlags(mid_item.flags() & ~Qt.ItemFlag.ItemIsEditable)
        self._table.setItem(mid_row, 0, mid_item)
        self._table.setSpan(mid_row, 0, 1, len(_COLUMNS))

        # --- Bids (bottom section, best first) ---
        for i, lvl in enumerate(bids):
            display_row = ask_count + 1 + i
            self._populate_level_row(
                row=display_row,
                level=lvl,
                side="bid",
                max_size=max_size,
                color_price=_C.PROFIT_GREEN,
                color_bar=_C.PROFIT_GREEN,
            )

        # Scroll so that the mid-price row is visible.
        mid_item_ref = self._table.item(mid_row, 0)
        if mid_item_ref is not None:
            self._table.scrollToItem(
                mid_item_ref,
                QTableWidget.ScrollHint.PositionAtCenter,
            )

    def _populate_level_row(
        self,
        *,
        row: int,
        level: OrderBookLevel,
        side: str,
        max_size: int,
        color_price: str,
        color_bar: str,
    ) -> None:
        """Fill a single table row with level data.

        Parameters
        ----------
        row:
            Row index in the QTableWidget.
        level:
            The order-book level to display.
        side:
            ``"bid"`` or ``"ask"``.
        max_size:
            Largest size across all levels (for proportional bar width).
        color_price:
            Foreground CSS colour for the price column.
        color_bar:
            Background tint CSS colour for the size bar fill.
        """
        # -- Price column --
        price_item = _mono_item(mojos_to_xch(level.price_mojos, decimals=6), fg=color_price)
        # Store price and side for level_clicked signal.
        price_item.setData(Qt.ItemDataRole.UserRole, level.price_mojos)
        price_item.setData(Qt.ItemDataRole.UserRole + 1, side)
        self._table.setItem(row, _COL_PRICE, price_item)

        # -- Size column (with inline proportional bar) --
        size_item = _mono_item(mojos_to_xch(level.size_mojos, decimals=4))
        # Compute proportional bar fill as background gradient.
        fill_ratio = level.size_mojos / max_size if max_size > 0 else 0.0
        bar_color = QColor(color_bar)
        bar_color.setAlphaF(0.25 * fill_ratio + 0.05)  # subtle tint
        size_item.setBackground(QBrush(bar_color))
        self._table.setItem(row, _COL_SIZE, size_item)

        # -- Cumulative column --
        cum_item = _mono_item(mojos_to_xch(level.cumulative_mojos, decimals=4))
        self._table.setItem(row, _COL_CUMULATIVE, cum_item)

        # -- My Size column --
        if self._show_own and level.is_own and level.own_size_mojos > 0:
            my_item = _mono_item(
                mojos_to_xch(level.own_size_mojos, decimals=4),
                fg=_C.PRIMARY_GREEN,
            )
            my_item.setBackground(QBrush(QColor(_C.PRIMARY_GREEN).lighter(150)))
        else:
            my_item = _mono_item("")

        self._table.setItem(row, _COL_MY_SIZE, my_item)

        # -- Row-level own-order highlight (3 px left border via background tint) --
        if self._show_own and level.is_own:
            # Apply a subtle CHIA-green left-border effect by tinting the
            # entire row background with a low-opacity green overlay.  Qt
            # QTableWidget does not support per-cell borders, so we use a
            # distinguishable background instead and rely on the My Size
            # column for definitive identification.
            highlight_bg = QColor(_C.PRIMARY_GREEN)
            highlight_bg.setAlphaF(0.08)
            for col in range(len(_COLUMNS)):
                existing = self._table.item(row, col)
                if existing is not None:
                    existing.setBackground(
                        QBrush(highlight_bg)
                        if col != _COL_SIZE
                        else existing.background()
                    )

        # -- Alternating row background (dark theme) --
        if not (self._show_own and level.is_own):
            if row % 2 == 0:
                bg = QColor(_C.PANEL_BG)
            else:
                bg = QColor(_C.ELEVATED_BG)
            for col in range(len(_COLUMNS)):
                existing = self._table.item(row, col)
                if existing is not None and col != _COL_SIZE:
                    existing.setBackground(QBrush(bg))

    # -----------------------------------------------------------------
    # Internal: depth-chart rendering
    # -----------------------------------------------------------------

    def _refresh_chart(self) -> None:
        """Redraw the pyqtgraph depth chart from the cached snapshot."""
        snap = self._snapshot
        if snap is None:
            return

        agg_bps = self._selected_agg_bps()
        depth = self._depth_levels

        # Aggregate and trim.
        bids = self._aggregate_levels(snap.bids, agg_bps, is_bid=True)[:depth]
        asks = self._aggregate_levels(snap.asks, agg_bps, is_bid=False)[:depth]

        mid_xch = mojos_to_xch_float(snap.mid_price_mojos)

        # --- Compute bar positions and heights ---
        # Bids: prices descending from mid; cumulative ascending.
        bid_prices = np.array(
            [mojos_to_xch_float(l.price_mojos) for l in bids], dtype=np.float64
        )
        bid_cum = np.array(
            [mojos_to_xch_float(l.cumulative_mojos) for l in bids], dtype=np.float64
        )
        # Asks: prices ascending from mid; cumulative ascending.
        ask_prices = np.array(
            [mojos_to_xch_float(l.price_mojos) for l in asks], dtype=np.float64
        )
        ask_cum = np.array(
            [mojos_to_xch_float(l.cumulative_mojos) for l in asks], dtype=np.float64
        )

        # Compute a sensible bar width (half the average price gap, or a default).
        bar_width = self._compute_bar_width(bid_prices, ask_prices)

        # Update bid bars.
        self._bid_bars.setOpts(
            x=bid_prices if len(bid_prices) else np.array([]),
            height=bid_cum if len(bid_cum) else np.array([]),
            width=bar_width,
        )

        # Update ask bars.
        self._ask_bars.setOpts(
            x=ask_prices if len(ask_prices) else np.array([]),
            height=ask_cum if len(ask_cum) else np.array([]),
            width=bar_width,
        )

        # --- Own-order overlay ---
        own_prices: list[float] = []
        own_heights: list[float] = []
        if self._show_own:
            for lvl in bids + asks:
                if lvl.is_own and lvl.own_size_mojos > 0:
                    own_prices.append(mojos_to_xch_float(lvl.price_mojos))
                    own_heights.append(mojos_to_xch_float(lvl.own_size_mojos))

        self._own_bars.setOpts(
            x=np.array(own_prices, dtype=np.float64) if own_prices else np.array([]),
            height=np.array(own_heights, dtype=np.float64) if own_heights else np.array([]),
            width=bar_width,
        )
        self._own_bars.setVisible(self._show_own and len(own_prices) > 0)

        # --- Mid-price line ---
        self._mid_line.setValue(mid_xch)

        # Auto-range after redraw.
        self._plot_widget.getPlotItem().enableAutoRange()

    @staticmethod
    def _compute_bar_width(
        bid_prices: np.ndarray,
        ask_prices: np.ndarray,
    ) -> float:
        """Derive a reasonable bar width from the price arrays.

        Uses half the median inter-level gap, with a sensible fallback
        when there are too few levels.

        Parameters
        ----------
        bid_prices:
            Bid prices in XCH (descending).
        ask_prices:
            Ask prices in XCH (ascending).

        Returns
        -------
        Bar width in XCH units.
        """
        gaps: list[float] = []
        if len(bid_prices) > 1:
            gaps.extend(np.abs(np.diff(bid_prices)).tolist())
        if len(ask_prices) > 1:
            gaps.extend(np.abs(np.diff(ask_prices)).tolist())
        if gaps:
            median_gap = float(np.median(gaps))
            return max(median_gap * 0.5, 1e-8)
        # Fallback: tiny width relative to mid-price.
        if len(bid_prices) > 0:
            return bid_prices[0] * 0.0005
        if len(ask_prices) > 0:
            return ask_prices[0] * 0.0005
        return 0.0001
