"""Historical trade / fill viewer for XOPTrader.

Displays completed trades in a filterable, sortable table with
date-range selection, CSV export, and an aggregate PnL summary bar.

All monetary values are stored as **mojos** (int64, 1 XCH = 10^12)
and formatted for display via :func:`mojos_to_xch`.
"""

from __future__ import annotations

import csv
import io
from datetime import date, datetime
from typing import Optional

from PySide6.QtCore import QDate, Qt, Signal
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QComboBox,
    QDateEdit,
    QFileDialog,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS
from gui.utils import mojos_to_xch, mojos_per_unit_for_pair, format_price

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Column definitions (label, default width).
_COLUMNS: list[tuple[str, int]] = [
    ("Timestamp",     150),
    ("Trade ID",      130),
    ("Pair",           90),
    ("Side",           60),
    ("Price",         120),
    ("Size",          120),
    ("Fee",           100),
    ("Cost Basis",    120),
    ("Realized PnL",  120),
    ("Block",          80),
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _side_color(side: str) -> QColor:
    """Return PROFIT_GREEN for BID, LOSS_RED for ASK."""
    if side.lower() == "bid":
        return QColor(COLORS.PROFIT_GREEN)
    return QColor(COLORS.LOSS_RED)


def _pnl_color(pnl_mojos: int) -> QColor:
    """Return green for positive PnL, red for negative, gray for zero."""
    if pnl_mojos > 0:
        return QColor(COLORS.PROFIT_GREEN)
    if pnl_mojos < 0:
        return QColor(COLORS.LOSS_RED)
    return QColor(COLORS.TEXT_SECONDARY)


# ---------------------------------------------------------------------------
# TradeLogWidget
# ---------------------------------------------------------------------------

class TradeLogWidget(QWidget):
    """Historical trade viewer with filtering, sorting, and CSV export.

    Signals
    -------
    trade_selected(str):
        Emitted when the user clicks a row.  Payload is the trade_id.
    """

    trade_selected = Signal(str)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)

        # Master data set (unfiltered).
        self._all_trades: list[dict] = []

        self._build_ui()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        """Assemble the complete widget layout."""
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(8)

        root.addLayout(self._build_filter_bar())
        self._table = self._build_table()
        root.addWidget(self._table, stretch=1)
        root.addLayout(self._build_summary_bar())

    def _build_filter_bar(self) -> QHBoxLayout:
        """Create the top filter / action bar.

        Contains date-range pickers, pair and side combo-boxes,
        an export button, and a trade-count label.
        """
        bar = QHBoxLayout()
        bar.setSpacing(10)

        # Date range: From
        lbl_from = QLabel("From:")
        self._date_from = QDateEdit()
        self._date_from.setCalendarPopup(True)
        self._date_from.setDisplayFormat("yyyy-MM-dd")
        # Default to 30 days ago.
        self._date_from.setDate(QDate.currentDate().addDays(-30))
        bar.addWidget(lbl_from)
        bar.addWidget(self._date_from)

        # Date range: To
        lbl_to = QLabel("To:")
        self._date_to = QDateEdit()
        self._date_to.setCalendarPopup(True)
        self._date_to.setDisplayFormat("yyyy-MM-dd")
        self._date_to.setDate(QDate.currentDate())
        bar.addWidget(lbl_to)
        bar.addWidget(self._date_to)

        # Apply button
        btn_apply = QPushButton("Apply")
        btn_apply.setObjectName("primaryButton")
        btn_apply.clicked.connect(self._apply_filters)
        bar.addWidget(btn_apply)

        # Pair filter
        lbl_pair = QLabel("Pair:")
        self._combo_pair = QComboBox()
        self._combo_pair.addItem("All Pairs")
        self._combo_pair.setMinimumWidth(140)
        self._combo_pair.currentIndexChanged.connect(self._apply_filters)
        bar.addWidget(lbl_pair)
        bar.addWidget(self._combo_pair)

        # Side filter
        lbl_side = QLabel("Side:")
        self._combo_side = QComboBox()
        self._combo_side.addItems(["All", "Bid", "Ask"])
        self._combo_side.currentIndexChanged.connect(self._apply_filters)
        bar.addWidget(lbl_side)
        bar.addWidget(self._combo_side)

        bar.addStretch()

        # Trade count label
        self._lbl_count = QLabel("Trades: 0")
        self._lbl_count.setStyleSheet(
            f"color: {COLORS.TEXT_SECONDARY}; font-size: 10pt;"
        )
        bar.addWidget(self._lbl_count)

        # Export CSV button
        btn_export = QPushButton("Export CSV")
        btn_export.clicked.connect(self._on_export_csv)
        bar.addWidget(btn_export)

        return bar

    def _build_table(self) -> QTableWidget:
        """Create and configure the trade history table."""
        table = QTableWidget(0, len(_COLUMNS))
        table.setHorizontalHeaderLabels([c[0] for c in _COLUMNS])

        table.setAlternatingRowColors(True)
        table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
        table.verticalHeader().setVisible(False)
        table.setShowGrid(True)
        table.setSortingEnabled(True)

        # Column widths
        header = table.horizontalHeader()
        for idx, (_, width) in enumerate(_COLUMNS):
            header.resizeSection(idx, width)
        # Stretch the Timestamp column to use remaining space.
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)

        # Emit trade_selected on row click.
        table.cellClicked.connect(self._on_cell_clicked)

        return table

    def _build_summary_bar(self) -> QHBoxLayout:
        """Create the bottom summary bar with aggregate trade stats."""
        bar = QHBoxLayout()
        bar.setSpacing(16)

        self._lbl_total_trades = QLabel("Total trades: 0")
        self._lbl_total_pnl = QLabel("Total realized PnL: 0.0000 XCH")
        self._lbl_avg_size = QLabel("Avg fill size: 0.0000 XCH")
        self._lbl_win_rate = QLabel("Win rate: 0.0%")

        for lbl in (
            self._lbl_total_trades,
            self._lbl_total_pnl,
            self._lbl_avg_size,
            self._lbl_win_rate,
        ):
            lbl.setStyleSheet(f"color: {COLORS.TEXT_SECONDARY}; font-size: 9pt;")
            bar.addWidget(lbl)

        bar.addStretch()
        return bar

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def load_trades(self, trades: list[dict]) -> None:
        """Populate the widget from a database query result.

        Expected dict keys:
            timestamp, trade_id, pair_name, side, price_mojos,
            size_mojos, fee_mojos, cost_basis_mojos,
            realized_pnl_mojos, block_height

        Parameters
        ----------
        trades:
            List of trade dictionaries from the database layer.
        """
        self._all_trades = list(trades)
        self._rebuild_pair_combo()
        self._apply_filters()

    def export_csv(self, filepath: str) -> None:
        """Write the currently visible (filtered) trades to a CSV file.

        Parameters
        ----------
        filepath:
            Destination path for the CSV file.  Parent directory must
            exist.
        """
        visible = self._filtered_trades()

        with open(filepath, "w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            # Header row.
            writer.writerow([
                "timestamp", "trade_id", "pair", "side",
                "price_xch", "size_xch", "fee_xch",
                "cost_basis_xch", "realized_pnl_xch", "block",
            ])
            for trade in visible:
                pn = trade.get("pair_name", "")
                b_mpu = mojos_per_unit_for_pair(pn, "base")
                writer.writerow([
                    trade.get("timestamp", ""),
                    trade.get("trade_id", ""),
                    pn,
                    trade.get("side", ""),
                    mojos_to_xch(trade.get("price_mojos", 0), 12),
                    mojos_to_xch(trade.get("size_mojos", 0), 12, mojos_per_unit=b_mpu),
                    mojos_to_xch(trade.get("fee_mojos", 0), 12),
                    mojos_to_xch(trade.get("cost_basis_mojos", 0), 12),
                    mojos_to_xch(trade.get("realized_pnl_mojos", 0), 12),
                    trade.get("block_height", ""),
                ])

    # ------------------------------------------------------------------
    # Internal: filtering
    # ------------------------------------------------------------------

    def _rebuild_pair_combo(self) -> None:
        """Repopulate the pair filter combo with unique pair names."""
        current_text = self._combo_pair.currentText()
        self._combo_pair.blockSignals(True)
        self._combo_pair.clear()
        self._combo_pair.addItem("All Pairs")
        pairs_seen: set[str] = set()
        for trade in self._all_trades:
            pname = trade.get("pair_name", "")
            if pname and pname not in pairs_seen:
                pairs_seen.add(pname)
                self._combo_pair.addItem(pname)
        idx = self._combo_pair.findText(current_text)
        self._combo_pair.setCurrentIndex(max(idx, 0))
        self._combo_pair.blockSignals(False)

    def _filtered_trades(self) -> list[dict]:
        """Return the subset of ``_all_trades`` matching current filters.

        Applies date range, pair, and side filters.
        """
        pair_filter = self._combo_pair.currentText()
        side_filter = self._combo_side.currentText().lower()
        date_from: date = self._date_from.date().toPython()
        date_to: date = self._date_to.date().toPython()

        result: list[dict] = []
        for trade in self._all_trades:
            # Date range filter -- parse the timestamp string.
            ts_raw = trade.get("timestamp", "")
            if ts_raw:
                try:
                    ts_date = datetime.fromisoformat(str(ts_raw)).date()
                except (ValueError, TypeError):
                    ts_date = None
                if ts_date is not None:
                    if ts_date < date_from or ts_date > date_to:
                        continue

            # Pair filter
            if pair_filter != "All Pairs" and trade.get("pair_name") != pair_filter:
                continue

            # Side filter
            if side_filter != "all" and trade.get("side", "").lower() != side_filter:
                continue

            result.append(trade)
        return result

    def _apply_filters(self) -> None:
        """Filter trades and repopulate the table and summary bar."""
        filtered = self._filtered_trades()
        self._populate_table(filtered)
        self._update_summary(filtered)
        self._lbl_count.setText(f"Trades: {len(filtered)}")

    # ------------------------------------------------------------------
    # Internal: table population
    # ------------------------------------------------------------------

    def _populate_table(self, trades: list[dict]) -> None:
        """Write *trades* into the QTableWidget rows.

        Sorting is temporarily disabled to prevent index corruption
        during row insertion.
        """
        self._table.setSortingEnabled(False)
        self._table.setRowCount(0)

        mono_font = QFont("JetBrains Mono", 10)
        mono_font.setStyleHint(QFont.StyleHint.Monospace)

        for row_idx, trade in enumerate(trades):
            self._table.insertRow(row_idx)

            # -- Timestamp --
            ts = str(trade.get("timestamp", ""))
            item_ts = QTableWidgetItem(ts)
            item_ts.setForeground(QColor(COLORS.TEXT_SECONDARY))
            self._table.setItem(row_idx, 0, item_ts)

            # -- Trade ID (truncated) --
            tid: str = trade.get("trade_id", "")
            item_tid = QTableWidgetItem(tid[:16] + "..." if len(tid) > 16 else tid)
            item_tid.setToolTip(tid)
            item_tid.setData(Qt.ItemDataRole.UserRole, tid)
            self._table.setItem(row_idx, 1, item_tid)

            # -- Pair --
            pair_name: str = trade.get("pair_name", "")
            self._table.setItem(
                row_idx, 2, QTableWidgetItem(pair_name)
            )

            # -- Side (coloured) --
            side: str = trade.get("side", "")
            item_side = QTableWidgetItem(side.upper())
            item_side.setForeground(_side_color(side))
            item_side.setFont(QFont("JetBrains Mono", 10, QFont.Weight.Bold))
            self._table.setItem(row_idx, 3, item_side)

            # Resolve mojos-per-unit for sizes (base asset).
            # Engine stores price_mojos = price × 10^12 (kMojosPerXch) for ALL
            # pairs, so prices always divide by MOJOS_PER_XCH (the default).
            base_mpu = mojos_per_unit_for_pair(pair_name, "base")

            # -- Price --
            price_mojos: int = trade.get("price_mojos", 0)
            item_price = QTableWidgetItem(format_price(price_mojos, pair_name))
            item_price.setFont(mono_font)
            item_price.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            item_price.setData(Qt.ItemDataRole.UserRole, price_mojos)
            self._table.setItem(row_idx, 4, item_price)

            # -- Size --
            size_mojos: int = trade.get("size_mojos", 0)
            item_size = QTableWidgetItem(mojos_to_xch(size_mojos, mojos_per_unit=base_mpu))
            item_size.setFont(mono_font)
            item_size.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            item_size.setData(Qt.ItemDataRole.UserRole, size_mojos)
            self._table.setItem(row_idx, 5, item_size)

            # -- Fee --
            fee_mojos: int = trade.get("fee_mojos", 0)
            item_fee = QTableWidgetItem(mojos_to_xch(fee_mojos))
            item_fee.setFont(mono_font)
            item_fee.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            item_fee.setData(Qt.ItemDataRole.UserRole, fee_mojos)
            self._table.setItem(row_idx, 6, item_fee)

            # -- Cost Basis --
            cb_mojos: int = trade.get("cost_basis_mojos", 0)
            item_cb = QTableWidgetItem(mojos_to_xch(cb_mojos))
            item_cb.setFont(mono_font)
            item_cb.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            item_cb.setData(Qt.ItemDataRole.UserRole, cb_mojos)
            self._table.setItem(row_idx, 7, item_cb)

            # -- Realized PnL (coloured) --
            pnl_mojos: int = trade.get("realized_pnl_mojos", 0)
            pnl_text = mojos_to_xch(pnl_mojos)
            # Prefix with '+' for positive values.
            if pnl_mojos > 0:
                pnl_text = "+" + pnl_text
            item_pnl = QTableWidgetItem(pnl_text)
            item_pnl.setFont(mono_font)
            item_pnl.setForeground(_pnl_color(pnl_mojos))
            item_pnl.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            item_pnl.setData(Qt.ItemDataRole.UserRole, pnl_mojos)
            self._table.setItem(row_idx, 8, item_pnl)

            # -- Block --
            block_h: int = trade.get("block_height", 0)
            item_block = QTableWidgetItem(str(block_h))
            item_block.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            item_block.setData(Qt.ItemDataRole.UserRole, block_h)
            self._table.setItem(row_idx, 9, item_block)

        self._table.setSortingEnabled(True)

    # ------------------------------------------------------------------
    # Internal: summary
    # ------------------------------------------------------------------

    def _update_summary(self, trades: list[dict]) -> None:
        """Recompute aggregate statistics from the filtered trade list."""
        total = len(trades)

        total_pnl: int = 0
        total_size: int = 0
        wins: int = 0

        for t in trades:
            pnl = t.get("realized_pnl_mojos", 0)
            total_pnl += pnl
            total_size += t.get("size_mojos", 0)
            if pnl > 0:
                wins += 1

        avg_size = total_size // total if total > 0 else 0
        win_rate = (wins / total * 100.0) if total > 0 else 0.0

        self._lbl_total_trades.setText(f"Total trades: {total}")

        # Colour the PnL label to reflect overall performance.
        pnl_colour = COLORS.PROFIT_GREEN if total_pnl >= 0 else COLORS.LOSS_RED
        self._lbl_total_pnl.setText(
            f"Total realized PnL: {mojos_to_xch(total_pnl)}"
        )
        self._lbl_total_pnl.setStyleSheet(
            f"color: {pnl_colour}; font-size: 9pt;"
        )

        self._lbl_avg_size.setText(f"Avg fill size: {mojos_to_xch(avg_size)}")
        self._lbl_win_rate.setText(f"Win rate: {win_rate:.1f}%")

    # ------------------------------------------------------------------
    # Slots
    # ------------------------------------------------------------------

    def _on_cell_clicked(self, row: int, _column: int) -> None:
        """Emit ``trade_selected`` when the user clicks a table row."""
        item = self._table.item(row, 1)
        if item is not None:
            trade_id: str = item.data(Qt.ItemDataRole.UserRole) or ""
            if trade_id:
                self.trade_selected.emit(trade_id)

    def _on_export_csv(self) -> None:
        """Open a file dialog and write the filtered trades to CSV."""
        filepath, _ = QFileDialog.getSaveFileName(
            self,
            "Export Trades to CSV",
            "xop_trades.csv",
            "CSV Files (*.csv);;All Files (*)",
        )
        if filepath:
            self.export_csv(filepath)
