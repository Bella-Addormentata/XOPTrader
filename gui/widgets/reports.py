"""Reports & Analytics page for XOPTrader GUI.

Provides brokerage-style P&L reports with time-period breakdowns,
per-pair performance, capital gains estimates, offer statistics,
top/worst trades, and daily P&L history.

Data is fetched from the SQLite database via ``DatabaseService`` on a
background thread and pushed to this widget via the ``update_reports``
slot.

ISO/IEC 27001:2022 -- no credentials stored or displayed.
ISO/IEC 5055      -- all public APIs carry type hints and docstrings.
ISO/IEC 25000     -- degrades gracefully on empty or partial data.
"""

from __future__ import annotations

from typing import Any, Final, Optional

from PySide6.QtCore import Qt, Slot
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QScrollArea,
    QSizePolicy,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C
from gui.utils import mojos_to_xch, mojos_to_xch_float, mojos_per_unit_for_pair

# ---------------------------------------------------------------------------
# Palette aliases
# ---------------------------------------------------------------------------
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
WARNING: Final[str] = _C.WARNING_YELLOW
INFO: Final[str] = _C.INFO_BLUE

_MONO: Final[str] = "Consolas, 'Courier New', monospace"

# Period display labels.
_PERIOD_LABELS: Final[dict[str, str]] = {
    "today": "Today",
    "7d": "Last 7 Days",
    "30d": "Last 30 Days",
    "90d": "Last 90 Days",
    "ytd": "Year to Date",
    "1y": "Last 12 Months",
    "all": "All Time",
}

# Table styling shared across all tables.
_TABLE_STYLE: Final[str] = f"""
    QTableWidget {{
        background-color: {DARK_BG};
        color: {TEXT_PRIMARY};
        border: 1px solid {BORDER};
        border-radius: 4px;
        gridline-color: {BORDER};
        font-family: {_MONO};
        font-size: 12px;
    }}
    QTableWidget::item {{
        padding: 6px 10px;
    }}
    QTableWidget::item:alternate {{
        background-color: {PANEL_BG};
    }}
    QHeaderView::section {{
        background-color: {ELEVATED_BG};
        color: {TEXT_SECONDARY};
        border: 1px solid {BORDER};
        padding: 6px 10px;
        font-size: 11px;
        font-weight: bold;
    }}
"""


def _pnl_color(mojos: int) -> QColor:
    """Return green/red/gray based on PnL sign."""
    if mojos > 0:
        return QColor(PROFIT_GREEN)
    if mojos < 0:
        return QColor(LOSS_RED)
    return QColor(TEXT_SECONDARY)


def _pnl_item(mojos: int, decimals: int = 4) -> QTableWidgetItem:
    """Create a right-aligned, color-coded PnL table item."""
    text = mojos_to_xch(int(mojos), decimals)
    if mojos > 0:
        text = "+" + text
    item = QTableWidgetItem(text)
    item.setForeground(_pnl_color(int(mojos)))
    item.setTextAlignment(
        Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
    )
    return item


def _num_item(value: Any, fmt: str = "{:,.0f}") -> QTableWidgetItem:
    """Create a right-aligned numeric table item."""
    item = QTableWidgetItem(fmt.format(value))
    item.setTextAlignment(
        Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
    )
    return item


def _text_item(text: str) -> QTableWidgetItem:
    """Create a left-aligned text table item."""
    item = QTableWidgetItem(str(text))
    item.setTextAlignment(
        Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
    )
    return item


def _pct_item(value: float) -> QTableWidgetItem:
    """Create a right-aligned percentage item."""
    text = f"{value:.1f}%"
    item = QTableWidgetItem(text)
    color = QColor(PROFIT_GREEN) if value > 0 else (
        QColor(LOSS_RED) if value < 0 else QColor(TEXT_SECONDARY)
    )
    item.setForeground(color)
    item.setTextAlignment(
        Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
    )
    return item


class ReportsWidget(QWidget):
    """Full-page reports and analytics widget.

    Organized as a tabbed interface:
      - **Performance** — period-based P&L summary cards
      - **Per-Pair** — breakdown by trading pair
      - **Capital Gains** — short-term / long-term tax estimate
      - **Offer Stats** — fill rate, cancel rate, etc.
      - **Top Trades** — best and worst individual fills
      - **Daily P&L** — day-by-day realized P&L (last 30 days)

    Parameters
    ----------
    parent : QWidget | None
        Optional parent widget.
    """

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(16, 16, 16, 16)
        root.setSpacing(12)

        # -- Header --
        header = QLabel("Reports & Analytics")
        header.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 18px; font-weight: bold;"
        )
        root.addWidget(header)

        desc = QLabel(
            "Brokerage-style performance reports generated from trade history.  "
            "Refreshes automatically every 30 seconds."
        )
        desc.setWordWrap(True)
        desc.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 12px;")
        root.addWidget(desc)

        # -- Tab widget --
        self._tabs = QTabWidget()
        self._tabs.setStyleSheet(
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
                margin-right: 1px;
                font-size: 12px;
                min-width: 80px;
            }}
            QTabBar::tab:selected {{
                background-color: {DARK_BG};
                color: {TEXT_PRIMARY};
                border-bottom: 2px solid {PRIMARY_GREEN};
            }}
            """
        )

        self._perf_tab = self._build_performance_tab()
        self._tabs.addTab(self._perf_tab, "Performance")

        self._pair_tab = self._build_pair_tab()
        self._tabs.addTab(self._pair_tab, "Per Pair")

        self._capgains_tab = self._build_capgains_tab()
        self._tabs.addTab(self._capgains_tab, "Capital Gains")

        self._offers_tab = self._build_offers_tab()
        self._tabs.addTab(self._offers_tab, "Offer Stats")

        self._trades_tab = self._build_top_trades_tab()
        self._tabs.addTab(self._trades_tab, "Top Trades")

        self._daily_tab = self._build_daily_tab()
        self._tabs.addTab(self._daily_tab, "Daily P&&L")

        root.addWidget(self._tabs, stretch=1)

        # -- Status --
        self._status_label = QLabel("Waiting for report data…")
        self._status_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        root.addWidget(self._status_label)

    # -----------------------------------------------------------------------
    # Tab builders
    # -----------------------------------------------------------------------

    def _build_performance_tab(self) -> QWidget:
        """Build the period P&L summary tab."""
        w = QWidget()
        layout = QVBoxLayout(w)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(8)

        # Summary cards grid.
        self._perf_cards: dict[str, dict[str, QLabel]] = {}
        grid = QGridLayout()
        grid.setSpacing(8)

        for idx, (key, label) in enumerate(_PERIOD_LABELS.items()):
            card = self._make_period_card(key, label)
            row, col = divmod(idx, 3)
            grid.addWidget(card, row, col)

        layout.addLayout(grid, stretch=1)
        return w

    def _make_period_card(self, key: str, title: str) -> QFrame:
        """Create a single period summary card."""
        frame = QFrame()
        frame.setStyleSheet(
            f"QFrame {{ background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 8px; padding: 12px; }}"
        )
        frame.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        vbox = QVBoxLayout(frame)
        vbox.setSpacing(4)

        title_lbl = QLabel(title)
        title_lbl.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px; font-weight: bold; border: none;"
        )
        vbox.addWidget(title_lbl)

        pnl_lbl = QLabel("—")
        pnl_lbl.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 20px; font-weight: bold; "
            f"font-family: {_MONO}; border: none;"
        )
        vbox.addWidget(pnl_lbl)

        details_lbl = QLabel("")
        details_lbl.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 10px; font-family: {_MONO}; border: none;"
        )
        details_lbl.setWordWrap(True)
        vbox.addWidget(details_lbl)

        self._perf_cards[key] = {"pnl": pnl_lbl, "details": details_lbl}
        return frame

    def _build_pair_tab(self) -> QWidget:
        """Build the per-pair breakdown tab."""
        w = QWidget()
        layout = QVBoxLayout(w)
        layout.setContentsMargins(8, 8, 8, 8)

        cols = [
            "Pair", "Trades", "Realized P&L", "Win Rate",
            "Volume", "Fees", "Avg Cost Basis", "Best", "Worst",
        ]
        self._pair_table = QTableWidget(0, len(cols))
        self._pair_table.setHorizontalHeaderLabels(cols)
        self._pair_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._pair_table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._pair_table.setAlternatingRowColors(True)
        self._pair_table.verticalHeader().setVisible(False)
        self._pair_table.horizontalHeader().setStretchLastSection(True)
        self._pair_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )
        self._pair_table.setStyleSheet(_TABLE_STYLE)
        layout.addWidget(self._pair_table)
        return w

    def _build_capgains_tab(self) -> QWidget:
        """Build the capital gains estimation tab."""
        w = QWidget()
        layout = QVBoxLayout(w)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(12)

        note = QLabel(
            "⚠ Capital gains estimates are approximate and based on realized "
            "P&L timestamps.  Consult a tax professional for filing purposes."
        )
        note.setWordWrap(True)
        note.setStyleSheet(
            f"color: {WARNING}; font-size: 11px; padding: 8px; "
            f"background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 4px;"
        )
        layout.addWidget(note)

        self._cg_frame = QFrame()
        self._cg_frame.setStyleSheet(
            f"QFrame {{ background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 8px; padding: 16px; }}"
        )
        cg_layout = QVBoxLayout(self._cg_frame)
        cg_layout.setSpacing(8)

        self._cg_year_label = QLabel("Tax Year: —")
        self._cg_year_label.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 14px; font-weight: bold; border: none;"
        )
        cg_layout.addWidget(self._cg_year_label)

        self._cg_labels: dict[str, QLabel] = {}
        for key, label in [
            ("short_term", "Short-Term Gains (held < 1 year)"),
            ("long_term", "Long-Term Gains (held ≥ 1 year)"),
            ("total", "Total Realized Gains"),
            ("fees_deductible", "Total Fees (potentially deductible)"),
        ]:
            row_layout = QHBoxLayout()
            desc_lbl = QLabel(label)
            desc_lbl.setStyleSheet(
                f"color: {TEXT_SECONDARY}; font-size: 12px; border: none;"
            )
            val_lbl = QLabel("—")
            val_lbl.setStyleSheet(
                f"color: {TEXT_PRIMARY}; font-size: 14px; font-weight: bold; "
                f"font-family: {_MONO}; border: none;"
            )
            val_lbl.setAlignment(Qt.AlignmentFlag.AlignRight)
            row_layout.addWidget(desc_lbl, stretch=1)
            row_layout.addWidget(val_lbl)
            cg_layout.addLayout(row_layout)
            self._cg_labels[key] = val_lbl

        layout.addWidget(self._cg_frame)
        layout.addStretch(1)
        return w

    def _build_offers_tab(self) -> QWidget:
        """Build the offer statistics tab."""
        w = QWidget()
        layout = QVBoxLayout(w)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(12)

        self._offer_cards_frame = QFrame()
        self._offer_cards_frame.setStyleSheet(
            f"QFrame {{ background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 8px; padding: 16px; }}"
        )
        cards_layout = QGridLayout(self._offer_cards_frame)
        cards_layout.setSpacing(16)

        self._offer_labels: dict[str, QLabel] = {}
        items = [
            ("total", "Total Offers", 0, 0),
            ("filled", "Filled", 0, 1),
            ("cancelled", "Cancelled", 0, 2),
            ("expired", "Expired", 1, 0),
            ("pending", "Pending", 1, 1),
            ("fill_rate", "Fill Rate", 1, 2),
        ]
        for key, label, row, col in items:
            card_vbox = QVBoxLayout()
            title_lbl = QLabel(label)
            title_lbl.setStyleSheet(
                f"color: {TEXT_SECONDARY}; font-size: 10px; border: none;"
            )
            title_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
            card_vbox.addWidget(title_lbl)
            val_lbl = QLabel("—")
            val_lbl.setStyleSheet(
                f"color: {TEXT_PRIMARY}; font-size: 18px; font-weight: bold; "
                f"font-family: {_MONO}; border: none;"
            )
            val_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
            card_vbox.addWidget(val_lbl)
            cards_layout.addLayout(card_vbox, row, col)
            self._offer_labels[key] = val_lbl

        layout.addWidget(self._offer_cards_frame)
        layout.addStretch(1)
        return w

    def _build_top_trades_tab(self) -> QWidget:
        """Build the top/worst trades tab."""
        w = QWidget()
        layout = QVBoxLayout(w)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(12)

        # Best trades.
        best_lbl = QLabel("🏆 Top 5 Best Trades")
        best_lbl.setStyleSheet(
            f"color: {PROFIT_GREEN}; font-size: 13px; font-weight: bold;"
        )
        layout.addWidget(best_lbl)

        cols = ["Timestamp", "Pair", "Side", "Price", "Size", "Realized P&L", "Cost Basis"]
        self._best_table = QTableWidget(0, len(cols))
        self._best_table.setHorizontalHeaderLabels(cols)
        self._best_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._best_table.setAlternatingRowColors(True)
        self._best_table.verticalHeader().setVisible(False)
        self._best_table.horizontalHeader().setStretchLastSection(True)
        self._best_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )
        self._best_table.setStyleSheet(_TABLE_STYLE)
        self._best_table.setMaximumHeight(200)
        layout.addWidget(self._best_table)

        # Worst trades.
        worst_lbl = QLabel("📉 Top 5 Worst Trades")
        worst_lbl.setStyleSheet(
            f"color: {LOSS_RED}; font-size: 13px; font-weight: bold;"
        )
        layout.addWidget(worst_lbl)

        self._worst_table = QTableWidget(0, len(cols))
        self._worst_table.setHorizontalHeaderLabels(cols)
        self._worst_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._worst_table.setAlternatingRowColors(True)
        self._worst_table.verticalHeader().setVisible(False)
        self._worst_table.horizontalHeader().setStretchLastSection(True)
        self._worst_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )
        self._worst_table.setStyleSheet(_TABLE_STYLE)
        self._worst_table.setMaximumHeight(200)
        layout.addWidget(self._worst_table)

        layout.addStretch(1)
        return w

    def _build_daily_tab(self) -> QWidget:
        """Build the daily P&L history tab."""
        w = QWidget()
        layout = QVBoxLayout(w)
        layout.setContentsMargins(8, 8, 8, 8)

        cols = ["Date", "Trades", "Realized P&L", "Fees", "Net P&L"]
        self._daily_table = QTableWidget(0, len(cols))
        self._daily_table.setHorizontalHeaderLabels(cols)
        self._daily_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._daily_table.setAlternatingRowColors(True)
        self._daily_table.verticalHeader().setVisible(False)
        self._daily_table.horizontalHeader().setStretchLastSection(True)
        self._daily_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )
        self._daily_table.setStyleSheet(_TABLE_STYLE)
        layout.addWidget(self._daily_table)
        return w

    # -----------------------------------------------------------------------
    # Public API
    # -----------------------------------------------------------------------

    @Slot(dict)
    def update_reports(self, data: dict[str, Any]) -> None:
        """Update all report tabs with fresh data.

        Parameters
        ----------
        data:
            Report dict from ``DatabaseService.reports_loaded``, with
            keys: ``periods``, ``per_pair``, ``capital_gains``,
            ``offer_stats``, ``top_trades``, ``worst_trades``,
            ``daily_pnl``.
        """
        if not data:
            return

        self._update_performance(data.get("periods", {}))
        self._update_pairs(data.get("per_pair", []))
        self._update_capgains(data.get("capital_gains", {}))
        self._update_offers(data.get("offer_stats", {}))
        self._update_top_trades(
            data.get("top_trades", []),
            data.get("worst_trades", []),
        )
        self._update_daily(data.get("daily_pnl", []))

        self._status_label.setText("Reports updated")

    # -----------------------------------------------------------------------
    # Internal update methods
    # -----------------------------------------------------------------------

    def _update_performance(self, periods: dict[str, dict]) -> None:
        """Update the period P&L summary cards."""
        for key, labels in self._perf_cards.items():
            p = periods.get(key, {})
            if not p:
                labels["pnl"].setText("—")
                labels["details"].setText("No data")
                continue

            pnl = int(p.get("total_pnl", 0))
            pnl_text = mojos_to_xch(pnl, 4)
            if pnl > 0:
                pnl_text = "+" + pnl_text
                color = PROFIT_GREEN
            elif pnl < 0:
                color = LOSS_RED
            else:
                color = TEXT_SECONDARY

            labels["pnl"].setText(f"{pnl_text}")
            labels["pnl"].setStyleSheet(
                f"color: {color}; font-size: 20px; font-weight: bold; "
                f"font-family: {_MONO}; border: none;"
            )

            trades = p.get("trade_count", 0)
            wins = p.get("wins", 0)
            losses = p.get("losses", 0)
            win_rate = (wins / trades * 100) if trades > 0 else 0
            fees = mojos_to_xch(int(p.get("total_fees", 0)), 4)
            vol = mojos_to_xch(int(p.get("total_volume", 0)), 2)

            labels["details"].setText(
                f"Trades: {trades}  |  W/L: {wins}/{losses}  |  "
                f"Win Rate: {win_rate:.0f}%\n"
                f"Volume: {vol}  |  Fees: {fees}"
            )

    def _update_pairs(self, pairs: list[dict]) -> None:
        """Update the per-pair breakdown table."""
        self._pair_table.setRowCount(len(pairs))

        for row, p in enumerate(pairs):
            trades = p.get("trade_count", 0)
            wins = p.get("wins", 0)
            win_rate = (wins / trades * 100) if trades > 0 else 0

            self._pair_table.setItem(row, 0, _text_item(p.get("pair_name", "")))
            self._pair_table.setItem(row, 1, _num_item(trades))
            self._pair_table.setItem(row, 2, _pnl_item(p.get("total_pnl", 0)))
            self._pair_table.setItem(row, 3, _pct_item(win_rate))
            self._pair_table.setItem(row, 4, _pnl_item(p.get("total_volume", 0), 2))
            self._pair_table.setItem(row, 5, _pnl_item(p.get("total_fees", 0)))
            self._pair_table.setItem(
                row, 6,
                _num_item(mojos_to_xch_float(int(p.get("avg_cost_basis", 0))), "{:,.6f}"),
            )
            # Best/Worst per pair — would require per-pair sub-query;
            # show N/A for now.
            self._pair_table.setItem(row, 7, _text_item("—"))
            self._pair_table.setItem(row, 8, _text_item("—"))

    def _update_capgains(self, cg: dict) -> None:
        """Update the capital gains estimation display."""
        if not cg:
            return

        year = cg.get("year", "—")
        self._cg_year_label.setText(f"Tax Year: {year}")

        for key in ("short_term", "long_term", "total", "fees_deductible"):
            val = int(cg.get(key, 0))
            text = mojos_to_xch(val, 4)
            if val > 0 and key != "fees_deductible":
                text = "+" + text
            self._cg_labels[key].setText(text)

            if key == "fees_deductible":
                color = TEXT_PRIMARY
            elif val > 0:
                color = PROFIT_GREEN
            elif val < 0:
                color = LOSS_RED
            else:
                color = TEXT_SECONDARY

            self._cg_labels[key].setStyleSheet(
                f"color: {color}; font-size: 14px; font-weight: bold; "
                f"font-family: {_MONO}; border: none;"
            )

    def _update_offers(self, stats: dict) -> None:
        """Update the offer statistics display."""
        if not stats:
            return

        for key, lbl in self._offer_labels.items():
            val = stats.get(key, 0)
            if key == "fill_rate":
                lbl.setText(f"{val:.1f}%")
                color = PROFIT_GREEN if val > 50 else (
                    WARNING if val > 20 else LOSS_RED
                )
            else:
                lbl.setText(f"{int(val):,}")
                color = TEXT_PRIMARY

            lbl.setStyleSheet(
                f"color: {color}; font-size: 18px; font-weight: bold; "
                f"font-family: {_MONO}; border: none;"
            )

    def _update_top_trades(
        self,
        best: list[dict],
        worst: list[dict],
    ) -> None:
        """Update the top/worst trades tables."""
        self._fill_trades_table(self._best_table, best)
        self._fill_trades_table(self._worst_table, worst)

    def _fill_trades_table(
        self, table: QTableWidget, trades: list[dict]
    ) -> None:
        """Populate a trade table (best or worst)."""
        table.setRowCount(len(trades))

        for row, t in enumerate(trades):
            ts = str(t.get("timestamp", ""))[:19]
            table.setItem(row, 0, _text_item(ts))
            table.setItem(row, 1, _text_item(t.get("pair_name", "")))

            side = t.get("side", "")
            side_item = _text_item(side.upper())
            side_item.setForeground(
                QColor(PROFIT_GREEN) if side.lower() == "bid" else QColor(LOSS_RED)
            )
            table.setItem(row, 2, side_item)

            table.setItem(row, 3, _pnl_item(t.get("price_mojos", 0), 6))
            table.setItem(row, 4, _pnl_item(t.get("size_mojos", 0), 4))
            table.setItem(row, 5, _pnl_item(t.get("realized_pnl_mojos", 0)))
            table.setItem(row, 6, _pnl_item(t.get("cost_basis_mojos", 0), 6))

    def _update_daily(self, daily: list[dict]) -> None:
        """Update the daily P&L table."""
        self._daily_table.setRowCount(len(daily))

        for row, d in enumerate(daily):
            self._daily_table.setItem(row, 0, _text_item(d.get("trade_date", "")))
            self._daily_table.setItem(row, 1, _num_item(d.get("trade_count", 0)))

            pnl = int(d.get("daily_pnl", 0))
            self._daily_table.setItem(row, 2, _pnl_item(pnl))

            fees = int(d.get("daily_fees", 0))
            self._daily_table.setItem(row, 3, _pnl_item(fees))

            net = pnl - fees
            self._daily_table.setItem(row, 4, _pnl_item(net))

    def clear(self) -> None:
        """Reset all tabs to their initial empty state."""
        for labels in self._perf_cards.values():
            labels["pnl"].setText("—")
            labels["details"].setText("")
        self._pair_table.setRowCount(0)
        for lbl in self._cg_labels.values():
            lbl.setText("—")
        for lbl in self._offer_labels.values():
            lbl.setText("—")
        self._best_table.setRowCount(0)
        self._worst_table.setRowCount(0)
        self._daily_table.setRowCount(0)
        self._status_label.setText("Waiting for report data…")
