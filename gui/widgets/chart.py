"""Real-time price and PnL chart widget for XOPTrader GUI.

Three vertically-stacked pyqtgraph plots share a common X-axis (block
height or timestamp) and a linked crosshair.  The toolbar at the top
provides pair selection, time-range zoom, chart-type toggling, auto-scroll,
and PNG export.

ISO/IEC 27001:2022 -- no credentials or secrets are handled.
ISO/IEC 5055      -- all public APIs carry type hints and docstrings.
"""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Deque, Sequence

import numpy as np
import pyqtgraph as pg
from pyqtgraph import DateAxisItem
from PySide6.QtCore import Qt, QTimer, Signal, Slot
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QPushButton,
    QSizePolicy,
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

# Apply global pyqtgraph defaults matching the dark theme
pg.setConfigOptions(antialias=True, background=DARK_BG, foreground=TEXT_PRIMARY)

# Maximum data points retained in each ring buffer before oldest are trimmed
_MAX_DATA_POINTS: int = 10_000


# ---------------------------------------------------------------------------
# Lightweight data containers (kept in ring-buffer deques)
# ---------------------------------------------------------------------------

@dataclass(slots=True)
class PriceTick:
    """Single price snapshot indexed by block height."""
    block: int
    timestamp: float
    mid: float
    bid: float
    ask: float


@dataclass(slots=True)
class PnLTick:
    """Single PnL snapshot indexed by block height."""
    block: int
    timestamp: float
    total_pnl: float
    realized_pnl: float


@dataclass(slots=True)
class VolumeTick:
    """Aggregated buy/sell volume for a single block."""
    block: int
    timestamp: float
    buy_vol: float
    sell_vol: float


@dataclass(slots=True)
class FillMarker:
    """A single fill event displayed on the price chart."""
    block: int
    price: float
    side: str       # "buy" or "sell"
    size: float


# ---------------------------------------------------------------------------
# _CrosshairOverlay -- linked crosshair across multiple PlotItems
# ---------------------------------------------------------------------------

class _CrosshairOverlay:
    """Draws a vertical + horizontal crosshair across a group of plots.

    Moving the mouse on any plot moves the vertical line on all linked
    plots and shows a coordinate label on the active plot.

    Parameters
    ----------
    plots:
        Sequence of ``PlotItem`` objects that share the same X axis.
    """

    def __init__(self, plots: Sequence[pg.PlotItem]) -> None:
        self._plots = plots
        self._v_lines: list[pg.InfiniteLine] = []
        self._h_lines: list[pg.InfiniteLine] = []
        self._labels: list[pg.TextItem] = []

        pen = pg.mkPen(color=TEXT_SECONDARY, width=1, style=Qt.PenStyle.DashLine)

        for plot in plots:
            v_line = pg.InfiniteLine(angle=90, movable=False, pen=pen)
            h_line = pg.InfiniteLine(angle=0, movable=False, pen=pen)
            plot.addItem(v_line, ignoreBounds=True)
            plot.addItem(h_line, ignoreBounds=True)
            self._v_lines.append(v_line)
            self._h_lines.append(h_line)

            label = pg.TextItem(
                text="", color=TEXT_PRIMARY, anchor=(0, 1)
            )
            label.setFont(QFont("Consolas", 9))
            plot.addItem(label, ignoreBounds=True)
            self._labels.append(label)

        # Connect proxy signals from each plot's scene
        for idx, plot in enumerate(plots):
            proxy = pg.SignalProxy(
                plot.scene().sigMouseMoved,
                rateLimit=60,
                slot=lambda evt, _idx=idx: self._on_mouse_moved(evt, _idx),
            )
            # Store reference to prevent garbage collection
            setattr(self, f"_proxy_{idx}", proxy)

    def _on_mouse_moved(self, event: tuple, plot_idx: int) -> None:
        """Update crosshair positions across all linked plots.

        Parameters
        ----------
        event:
            Single-element tuple containing the ``QPointF`` position.
        plot_idx:
            Index of the source plot in :attr:`_plots`.
        """
        pos = event[0]
        source_plot = self._plots[plot_idx]
        vb = source_plot.vb

        if not source_plot.sceneBoundingRect().contains(pos):
            return

        mouse_point = vb.mapSceneToView(pos)
        x = mouse_point.x()
        y = mouse_point.y()

        # Move vertical line on ALL plots, horizontal on source only
        for i, (v_line, h_line) in enumerate(
            zip(self._v_lines, self._h_lines)
        ):
            v_line.setPos(x)
            if i == plot_idx:
                h_line.setPos(y)
                self._labels[i].setText(
                    f"x={x:.0f}  y={y:.6f}"
                )
                self._labels[i].setPos(x, y)
            else:
                self._labels[i].setText("")


# ---------------------------------------------------------------------------
# ChartWidget -- main real-time chart view
# ---------------------------------------------------------------------------

class ChartWidget(QWidget):
    """Real-time price, PnL, and volume chart with toolbar controls.

    The widget holds three vertically-stacked pyqtgraph plots sharing
    a common X-axis, a linked crosshair, and a top toolbar for pair
    selection, time-range zoom, chart-type toggling, auto-scroll, and
    PNG export.

    Signals
    -------
    pair_changed:
        Emitted when the user picks a different pair in the combo box.
    """

    pair_changed = Signal(str)

    # Time-range presets (label -> hours, 0 = all)
    _TIME_RANGES: dict[str, int] = {
        "1H": 1, "4H": 4, "12H": 12, "1D": 24, "7D": 168, "All": 0,
    }

    # Chart-type tabs
    _CHART_TYPES: tuple[str, ...] = (
        "Price", "PnL", "Spread", "Volume", "Inventory",
    )

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setStyleSheet(f"background-color: {DARK_BG};")

        self._current_pair: str = ""
        self._auto_scroll: bool = True

        # --- Per-pair data stores ------------------------------------------
        # Keyed by pair name; each value is a dict of deques.
        self._data: dict[str, dict[str, deque]] = {}

        # --- Paint coalescing ----------------------------------------------
        # Instead of calling _repaint() on every data append, we mark the
        # chart dirty and let a 50 ms timer flush a single repaint.  This
        # collapses many rapid-fire appends into one repaint cycle.
        self._dirty: bool = False
        self._repaint_timer: QTimer = QTimer(self)
        self._repaint_timer.setInterval(50)
        self._repaint_timer.timeout.connect(self._maybe_repaint)
        self._repaint_timer.start()

        # --- O(1) block-to-timestamp lookup --------------------------------
        # Maps (pair, block_height) -> timestamp.  Updated on each
        # append_price_data call so _block_to_ts avoids a linear scan.
        self._block_ts_map: dict[tuple[str, int], float] = {}

        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(6)

        self._build_toolbar(root)
        self._build_chart_area(root)

    # =====================================================================
    # Private build helpers
    # =====================================================================

    def _build_toolbar(self, parent_layout: QVBoxLayout) -> None:
        """Create the top toolbar with selectors, toggles, and buttons."""
        toolbar = QHBoxLayout()
        toolbar.setSpacing(8)

        # Pair selector
        self._pair_combo = QComboBox()
        self._pair_combo.setMinimumWidth(140)
        self._pair_combo.setStyleSheet(self._combo_style())
        self._pair_combo.currentTextChanged.connect(self._on_pair_changed)
        toolbar.addWidget(self._pair_combo)

        # Time-range selector
        self._range_combo = QComboBox()
        self._range_combo.addItems(list(self._TIME_RANGES.keys()))
        self._range_combo.setCurrentText("1D")
        self._range_combo.setStyleSheet(self._combo_style())
        self._range_combo.currentTextChanged.connect(self._on_range_changed)
        toolbar.addWidget(self._range_combo)

        # Chart-type toggle
        self._type_combo = QComboBox()
        self._type_combo.addItems(list(self._CHART_TYPES))
        self._type_combo.setStyleSheet(self._combo_style())
        self._type_combo.currentTextChanged.connect(self._on_type_changed)
        toolbar.addWidget(self._type_combo)

        toolbar.addStretch()

        # Auto-scroll checkbox
        self._auto_cb = QCheckBox("Auto-scroll")
        self._auto_cb.setChecked(True)
        self._auto_cb.setStyleSheet(
            f"QCheckBox {{ color: {TEXT_PRIMARY}; font-size: 12px; }}"
            f"QCheckBox::indicator {{ width: 14px; height: 14px; }}"
        )
        self._auto_cb.toggled.connect(self._on_auto_scroll_toggled)
        toolbar.addWidget(self._auto_cb)

        # Export button
        self._export_btn = QPushButton("Export PNG")
        self._export_btn.setStyleSheet(self._button_style())
        self._export_btn.clicked.connect(self._on_export)
        toolbar.addWidget(self._export_btn)

        parent_layout.addLayout(toolbar)

    def _build_chart_area(self, parent_layout: QVBoxLayout) -> None:
        """Construct the three stacked pyqtgraph plots."""
        self._graphics = pg.GraphicsLayoutWidget()
        self._graphics.setBackground(DARK_BG)
        parent_layout.addWidget(self._graphics)

        # Shared X axis via DateAxisItem (bottom axis only on lowest plot)
        # Plot 1 -- Price (60 % height)
        self._price_plot: pg.PlotItem = self._graphics.addPlot(
            row=0, col=0,
            axisItems={"bottom": DateAxisItem(orientation="bottom")},
        )
        self._price_plot.setLabel("left", "Price (mojos/XCH)")
        self._price_plot.showGrid(x=True, y=True, alpha=0.15)
        self._price_plot.getAxis("left").setWidth(70)
        self._price_plot.hideAxis("bottom")  # hidden; shared via plot 3
        self._style_plot(self._price_plot)

        # Price series
        self._mid_curve = self._price_plot.plot(
            pen=pg.mkPen(color=TEXT_PRIMARY, width=1.5), name="Mid",
        )
        self._bid_curve = self._price_plot.plot(
            pen=pg.mkPen(
                color=PROFIT_GREEN, width=1, style=Qt.PenStyle.DashLine,
            ),
            name="Bid",
        )
        self._ask_curve = self._price_plot.plot(
            pen=pg.mkPen(
                color=LOSS_RED, width=1, style=Qt.PenStyle.DashLine,
            ),
            name="Ask",
        )

        # Fill markers (scatter overlay)
        self._buy_scatter = pg.ScatterPlotItem(
            symbol="t1", size=10,  # upward triangle
            brush=pg.mkBrush(PROFIT_GREEN),
            pen=pg.mkPen(None),
        )
        self._sell_scatter = pg.ScatterPlotItem(
            symbol="t", size=10,   # downward triangle
            brush=pg.mkBrush(LOSS_RED),
            pen=pg.mkPen(None),
        )
        self._price_plot.addItem(self._buy_scatter)
        self._price_plot.addItem(self._sell_scatter)

        # Plot 2 -- PnL (25 % height)
        self._pnl_plot: pg.PlotItem = self._graphics.addPlot(row=1, col=0)
        self._pnl_plot.setLabel("left", "PnL")
        self._pnl_plot.showGrid(x=True, y=True, alpha=0.15)
        self._pnl_plot.getAxis("left").setWidth(70)
        self._pnl_plot.hideAxis("bottom")
        self._style_plot(self._pnl_plot)

        # PnL area fill (green above zero, red below) via two FillBetween
        self._pnl_total_curve = self._pnl_plot.plot(
            pen=pg.mkPen(color=PROFIT_GREEN, width=1.2),
        )
        self._pnl_zero_line = self._pnl_plot.plot(
            pen=pg.mkPen(None),  # invisible baseline at y=0
        )
        # Green fill for positive values
        self._pnl_fill_pos = pg.FillBetweenItem(
            self._pnl_total_curve, self._pnl_zero_line,
            brush=pg.mkBrush(QColor(PROFIT_GREEN).lighter(160)),
        )
        self._pnl_fill_pos.setOpacity(0.25)
        self._pnl_plot.addItem(self._pnl_fill_pos)

        # Realized PnL line
        self._pnl_realized_curve = self._pnl_plot.plot(
            pen=pg.mkPen(color=PRIMARY_GREEN, width=1, style=Qt.PenStyle.SolidLine),
            name="Realized",
        )

        # Plot 3 -- Volume / Inventory (15 % height, visible bottom axis)
        self._vol_plot: pg.PlotItem = self._graphics.addPlot(
            row=2, col=0,
            axisItems={"bottom": DateAxisItem(orientation="bottom")},
        )
        self._vol_plot.setLabel("left", "Volume")
        self._vol_plot.showGrid(x=True, y=True, alpha=0.15)
        self._vol_plot.getAxis("left").setWidth(70)
        self._style_plot(self._vol_plot)

        # Volume bars (stacked: buy green, sell red)
        self._vol_buy_bars = pg.BarGraphItem(
            x=[], height=[], width=0.6,
            brush=pg.mkBrush(PROFIT_GREEN),
        )
        self._vol_sell_bars = pg.BarGraphItem(
            x=[], height=[], width=0.6,
            brush=pg.mkBrush(LOSS_RED),
        )
        self._vol_plot.addItem(self._vol_buy_bars)
        self._vol_plot.addItem(self._vol_sell_bars)

        # Inventory ratio overlay (right Y axis)
        self._inv_curve = self._vol_plot.plot(
            pen=pg.mkPen(color=WARNING, width=1.2, style=Qt.PenStyle.DashDotLine),
            name="Inventory",
        )

        # Link X axes so all three scroll and zoom together
        self._pnl_plot.setXLink(self._price_plot)
        self._vol_plot.setXLink(self._price_plot)

        # Set relative height ratios (60 / 25 / 15)
        self._graphics.ci.layout.setRowStretchFactor(0, 60)
        self._graphics.ci.layout.setRowStretchFactor(1, 25)
        self._graphics.ci.layout.setRowStretchFactor(2, 15)

        # Linked crosshair
        self._crosshair = _CrosshairOverlay(
            [self._price_plot, self._pnl_plot, self._vol_plot]
        )

    # =====================================================================
    # Paint coalescing
    # =====================================================================

    @Slot()
    def _maybe_repaint(self) -> None:
        """Flush a pending repaint if the dirty flag is set.

        Connected to ``_repaint_timer`` (50 ms interval) so that many
        rapid-fire data appends are collapsed into a single redraw.
        """
        if self._dirty:
            self._dirty = False
            self._repaint()

    # =====================================================================
    # Styling helpers
    # =====================================================================

    @staticmethod
    def _style_plot(plot: pg.PlotItem) -> None:
        """Apply CHIA dark theme to a PlotItem's axes and grid."""
        for axis_name in ("left", "bottom"):
            axis = plot.getAxis(axis_name)
            axis.setPen(pg.mkPen(color=BORDER))
            axis.setTextPen(pg.mkPen(color=TEXT_SECONDARY))
            axis.setStyle(tickLength=-5)

    @staticmethod
    def _combo_style() -> str:
        """Return QSS for a themed QComboBox."""
        return (
            f"QComboBox {{"
            f"  background-color: {ELEVATED_BG};"
            f"  color: {TEXT_PRIMARY};"
            f"  border: 1px solid {BORDER};"
            f"  border-radius: 4px;"
            f"  padding: 4px 8px;"
            f"  font-size: 12px;"
            f"}}"
            f"QComboBox::drop-down {{"
            f"  border: none;"
            f"}}"
            f"QComboBox QAbstractItemView {{"
            f"  background-color: {PANEL_BG};"
            f"  color: {TEXT_PRIMARY};"
            f"  selection-background-color: {BORDER};"
            f"}}"
        )

    @staticmethod
    def _button_style() -> str:
        """Return QSS for a themed QPushButton."""
        return (
            f"QPushButton {{"
            f"  background-color: {ELEVATED_BG};"
            f"  color: {TEXT_PRIMARY};"
            f"  border: 1px solid {BORDER};"
            f"  border-radius: 4px;"
            f"  padding: 4px 12px;"
            f"  font-size: 12px;"
            f"}}"
            f"QPushButton:hover {{"
            f"  background-color: {BORDER};"
            f"}}"
            f"QPushButton:pressed {{"
            f"  background-color: {PRIMARY_GREEN};"
            f"  color: {DARK_BG};"
            f"}}"
        )

    # =====================================================================
    # Toolbar slot handlers
    # =====================================================================

    @Slot(str)
    def _on_pair_changed(self, pair_name: str) -> None:
        """Switch displayed pair when the combo changes."""
        if pair_name and pair_name != self._current_pair:
            self.set_pair(pair_name)
            self.pair_changed.emit(pair_name)

    @Slot(str)
    def _on_range_changed(self, label: str) -> None:
        """Zoom the X-axis to the selected time range."""
        hours = self._TIME_RANGES.get(label, 0)
        self.set_time_range(hours)

    @Slot(str)
    def _on_type_changed(self, chart_type: str) -> None:
        """Toggle visibility of the three sub-plots based on selected type.

        ``"Price"`` shows all three.  Other types show only the relevant
        plot at full height so the user gets maximum detail.
        """
        show_price = chart_type in ("Price", "Spread")
        show_pnl   = chart_type in ("Price", "PnL")
        show_vol   = chart_type in ("Price", "Volume", "Inventory")

        self._price_plot.setVisible(show_price)
        self._pnl_plot.setVisible(show_pnl)
        self._vol_plot.setVisible(show_vol)

    @Slot(bool)
    def _on_auto_scroll_toggled(self, checked: bool) -> None:
        """Enable or disable automatic right-edge tracking."""
        self._auto_scroll = checked

    @Slot()
    def _on_export(self) -> None:
        """Save the current chart view as a PNG file via file dialog."""
        path, _ = QFileDialog.getSaveFileName(
            self, "Export Chart", str(Path.home() / "chart.png"),
            "PNG Images (*.png)",
        )
        if path:
            exporter = pg.exporters.ImageExporter(self._graphics.scene())
            exporter.export(path)

    # =====================================================================
    # Internal data helpers
    # =====================================================================

    def _ensure_pair_store(self, pair: str) -> dict[str, deque]:
        """Lazily create and return the data store for *pair*.

        Each pair gets independent deques so switching pairs is instant.
        """
        if pair not in self._data:
            self._data[pair] = {
                "price":  deque(maxlen=_MAX_DATA_POINTS),
                "pnl":    deque(maxlen=_MAX_DATA_POINTS),
                "volume": deque(maxlen=_MAX_DATA_POINTS),
                "fills":  deque(maxlen=_MAX_DATA_POINTS),
            }
        return self._data[pair]

    def _repaint(self) -> None:
        """Redraw all curves from the current pair's data store."""
        store = self._data.get(self._current_pair)
        if store is None:
            return

        # --- Price plot ----------------------------------------------------
        price_ticks: deque[PriceTick] = store["price"]
        if price_ticks:
            ts = [t.timestamp for t in price_ticks]
            self._mid_curve.setData(ts, [t.mid for t in price_ticks])
            self._bid_curve.setData(ts, [t.bid for t in price_ticks])
            self._ask_curve.setData(ts, [t.ask for t in price_ticks])
        else:
            self._mid_curve.setData([], [])
            self._bid_curve.setData([], [])
            self._ask_curve.setData([], [])

        # Fill markers
        fills: deque[FillMarker] = store["fills"]
        buys  = [f for f in fills if f.side == "buy"]
        sells = [f for f in fills if f.side == "sell"]

        if buys:
            # Resolve timestamps from price ticks by block height
            buy_ts    = [self._block_to_ts(store, f.block) for f in buys]
            buy_price = [f.price for f in buys]
            self._buy_scatter.setData(buy_ts, buy_price)
        else:
            self._buy_scatter.setData([], [])

        if sells:
            sell_ts    = [self._block_to_ts(store, f.block) for f in sells]
            sell_price = [f.price for f in sells]
            self._sell_scatter.setData(sell_ts, sell_price)
        else:
            self._sell_scatter.setData([], [])

        # --- PnL plot ------------------------------------------------------
        pnl_ticks: deque[PnLTick] = store["pnl"]
        if pnl_ticks:
            ts_pnl = [t.timestamp for t in pnl_ticks]
            total   = [t.total_pnl for t in pnl_ticks]
            realzd  = [t.realized_pnl for t in pnl_ticks]
            self._pnl_total_curve.setData(ts_pnl, total)
            self._pnl_zero_line.setData(ts_pnl, [0.0] * len(ts_pnl))
            self._pnl_realized_curve.setData(ts_pnl, realzd)
        else:
            self._pnl_total_curve.setData([], [])
            self._pnl_zero_line.setData([], [])
            self._pnl_realized_curve.setData([], [])

        # --- Volume plot ---------------------------------------------------
        vol_ticks: deque[VolumeTick] = store["volume"]
        if vol_ticks:
            ts_vol  = [t.timestamp for t in vol_ticks]
            buy_h   = [t.buy_vol for t in vol_ticks]
            sell_h  = [t.sell_vol for t in vol_ticks]
            self._vol_buy_bars.setOpts(x=ts_vol, height=buy_h, width=0.6)
            # Sell bars rendered as negative to stack below zero
            self._vol_sell_bars.setOpts(
                x=ts_vol,
                height=[-v for v in sell_h],
                width=0.6,
            )
        else:
            self._vol_buy_bars.setOpts(x=[], height=[], width=0.6)
            self._vol_sell_bars.setOpts(x=[], height=[], width=0.6)

        # Auto-scroll: keep the right edge pinned to the latest data
        if self._auto_scroll and price_ticks:
            latest_ts = price_ticks[-1].timestamp
            # Show the last portion of data that fits the current range
            x_min, x_max = self._price_plot.viewRange()[0]
            span = x_max - x_min if x_max > x_min else 3600
            self._price_plot.setXRange(latest_ts - span, latest_ts, padding=0)

    def _block_to_ts(self, store: dict[str, deque], block: int) -> float:
        """Resolve a block height to a timestamp via O(1) dict lookup.

        Falls back to ``time.time()`` if the block is not found in the
        pre-built ``_block_ts_map``.

        Parameters
        ----------
        store:
            Per-pair data dict (unused after the optimisation but kept
            for API compatibility).
        block:
            Chia blockchain block height to resolve.
        """
        key = (self._current_pair, block)
        return self._block_ts_map.get(key, time.time())

    # =====================================================================
    # Public data API
    # =====================================================================

    def append_price_data(
        self,
        block_height: int,
        mid: float,
        bid: float,
        ask: float,
        timestamp: float,
    ) -> None:
        """Append a price snapshot for the current pair.

        Parameters
        ----------
        block_height:
            Chia blockchain block height.
        mid:
            Mid-market price in mojos/XCH.
        bid:
            Best bid price.
        ask:
            Best ask price.
        timestamp:
            Unix timestamp of the block.
        """
        store = self._ensure_pair_store(self._current_pair)
        store["price"].append(
            PriceTick(block=block_height, timestamp=timestamp,
                      mid=mid, bid=bid, ask=ask)
        )
        # Maintain the O(1) block-to-timestamp lookup table.
        self._block_ts_map[(self._current_pair, block_height)] = timestamp
        self._dirty = True

    def append_pnl_data(
        self,
        block_height: int,
        total_pnl: float,
        realized_pnl: float,
        timestamp: float | None = None,
    ) -> None:
        """Append a PnL snapshot for the current pair.

        Parameters
        ----------
        block_height:
            Chia blockchain block height.
        total_pnl:
            Mark-to-market total PnL.
        realized_pnl:
            Realized (closed) PnL.
        timestamp:
            Unix timestamp; defaults to ``time.time()`` if omitted.
        """
        ts = timestamp if timestamp is not None else time.time()
        store = self._ensure_pair_store(self._current_pair)
        store["pnl"].append(
            PnLTick(block=block_height, timestamp=ts,
                    total_pnl=total_pnl, realized_pnl=realized_pnl)
        )
        self._dirty = True

    def append_volume_data(
        self,
        block_height: int,
        buy_volume: float,
        sell_volume: float,
        timestamp: float | None = None,
    ) -> None:
        """Append volume data for the current pair.

        Parameters
        ----------
        block_height:
            Chia blockchain block height.
        buy_volume:
            Buy-side fill volume in base units for this block.
        sell_volume:
            Sell-side fill volume in base units for this block.
        timestamp:
            Unix timestamp; defaults to ``time.time()`` if omitted.
        """
        ts = timestamp if timestamp is not None else time.time()
        store = self._ensure_pair_store(self._current_pair)
        store["volume"].append(
            VolumeTick(block=block_height, timestamp=ts,
                       buy_vol=buy_volume, sell_vol=sell_volume)
        )
        self._dirty = True

    def add_fill_marker(
        self,
        block_height: int,
        price: float,
        side: str,
        size: float,
    ) -> None:
        """Place a fill triangle on the price chart.

        Parameters
        ----------
        block_height:
            Block at which the fill occurred.
        price:
            Execution price.
        side:
            ``"buy"`` (green upward triangle) or ``"sell"`` (red downward).
        size:
            Fill size in base units (stored for tooltip use).
        """
        store = self._ensure_pair_store(self._current_pair)
        store["fills"].append(
            FillMarker(block=block_height, price=price, side=side, size=size)
        )
        self._dirty = True

    def set_pair(self, pair_name: str) -> None:
        """Switch the chart to display data for *pair_name*.

        If *pair_name* is not yet in the combo box it is added.  Data
        already recorded for this pair is drawn immediately.

        Parameters
        ----------
        pair_name:
            Trading pair identifier, e.g. ``"XCH/USDS"``.
        """
        self._current_pair = pair_name

        # Ensure the pair exists in the combo box
        if self._pair_combo.findText(pair_name) == -1:
            self._pair_combo.addItem(pair_name)
        self._pair_combo.setCurrentText(pair_name)

        # Ensure store exists (may be empty)
        self._ensure_pair_store(pair_name)
        self._repaint()

    def set_time_range(self, hours: int) -> None:
        """Zoom the X-axis to show the last *hours* of data.

        Parameters
        ----------
        hours:
            Number of hours to display.  ``0`` means show everything
            (auto-range).
        """
        if hours <= 0:
            # Show all data
            self._price_plot.enableAutoRange(axis="x")
            return

        now = time.time()
        x_min = now - hours * 3600
        self._price_plot.setXRange(x_min, now, padding=0.02)

    def clear_data(self) -> None:
        """Reset all series for the current pair and clear the charts."""
        store = self._data.get(self._current_pair)
        if store is not None:
            for dq in store.values():
                dq.clear()
        self._repaint()

    def add_pairs(self, pairs: Sequence[str]) -> None:
        """Populate the pair combo box.

        Parameters
        ----------
        pairs:
            Ordered list of pair names to appear in the dropdown.
        """
        self._pair_combo.blockSignals(True)
        self._pair_combo.clear()
        for p in pairs:
            self._pair_combo.addItem(p)
        self._pair_combo.blockSignals(False)

        if pairs:
            self.set_pair(pairs[0])
