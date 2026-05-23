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
from gui.utils import format_price, mojos_to_xch, mojos_to_xch_float, mojos_per_unit_for_pair

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


def _split_pair(pair_name: str) -> tuple[str, str]:
    """Split a pair label into ``(base, quote)`` tokens."""
    base, _, quote = pair_name.partition("/")
    return base.strip(), quote.strip()


def _is_stablecoin_symbol(symbol: str) -> bool:
    """Return ``True`` when *symbol* should be treated as USD-like."""
    normalized = symbol.strip().upper()
    return normalized in {"WUSDC.B", "WUSDC", "USDC", "USDS", "USDT"}


def _format_usdc(value: float, *, signed: bool = False, decimals: int = 2) -> str:
    """Format a numeric value as a USDC amount."""
    sign = "+" if signed and value > 0 else ""
    return f"{sign}${value:,.{decimals}f}"


def _money_text_from_usdc(value: float, *, signed: bool = True, as_cost: bool = False) -> str:
    """Format a USDC value with optional sign/cost semantics."""
    display = -abs(float(value)) if as_cost else float(value)
    return _format_usdc(display, signed=signed)


def _money_item_from_usdc(
    value: float,
    *,
    signed: bool = True,
    as_cost: bool = False,
) -> QTableWidgetItem:
    """Create a right-aligned table item for USDC-denominated values."""
    display = -abs(float(value)) if as_cost else float(value)
    item = QTableWidgetItem(_format_usdc(display, signed=signed))
    if display > 0:
        color = QColor(PROFIT_GREEN)
    elif display < 0:
        color = QColor(LOSS_RED)
    else:
        color = QColor(TEXT_SECONDARY)
    item.setForeground(color)
    item.setTextAlignment(
        Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
    )
    return item


def _money_text_from_mojos(
    mojos: int,
    xch_usd_rate: float,
    *,
    signed: bool = True,
    as_cost: bool = False,
    decimals: int = 2,
) -> str:
    """Format engine PnL mojos as USDC, falling back to XCH if needed."""
    display_mojos = -abs(int(mojos)) if as_cost else int(mojos)
    if xch_usd_rate > 0:
        value_usdc = mojos_to_xch_float(display_mojos) * xch_usd_rate
        return _format_usdc(value_usdc, signed=signed, decimals=decimals)

    text = mojos_to_xch(display_mojos, 4)
    if signed and display_mojos > 0:
        text = "+" + text
    return f"{text} XCH"


def _money_item_from_mojos(
    mojos: int,
    xch_usd_rate: float,
    *,
    signed: bool = True,
    as_cost: bool = False,
) -> QTableWidgetItem:
    """Create a right-aligned table item for monetary values."""
    display_mojos = -abs(int(mojos)) if as_cost else int(mojos)
    item = QTableWidgetItem(
        _money_text_from_mojos(
            display_mojos,
            xch_usd_rate,
            signed=signed,
            as_cost=False,
        )
    )
    item.setForeground(_pnl_color(display_mojos))
    item.setTextAlignment(
        Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
    )
    return item


def _size_item(pair_name: str, size_mojos: int) -> QTableWidgetItem:
    """Format a base-asset size for tables."""
    base_asset, _ = _split_pair(pair_name)
    divisor = mojos_per_unit_for_pair(pair_name, "base")
    decimals = 4 if divisor > 1_000 else 3
    amount = mojos_to_xch_float(int(size_mojos), divisor)
    item = QTableWidgetItem(f"{amount:,.{decimals}f} {base_asset}")
    item.setTextAlignment(
        Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
    )
    return item


def _price_item(pair_name: str, price_mojos: int) -> QTableWidgetItem:
    """Format a price or cost basis using the pair's quote conventions."""
    item = QTableWidgetItem(format_price(int(price_mojos), pair_name))
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
        self._xch_usd_rate: float = 0.0
        self._market_data: dict[str, dict[str, float]] = {}
        self._wallet_balances: dict[str, dict[str, float]] = {}
        self._live_pnl: dict[str, Any] = {}
        self._last_reports: dict[str, Any] = {}
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
            "Historical P&L is normalised into USDC using the latest "
            "persisted daily XCH/wUSDC marks when available. Live portfolio value uses current wallet balances "
            "and market prices. Refreshes automatically every 30 seconds."
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

        self._portfolio_frame = self._build_live_portfolio_frame()
        layout.addWidget(self._portfolio_frame)

        self._forecast_frame = self._build_forecast_frame()
        layout.addWidget(self._forecast_frame)

        layout.addStretch(1)
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

    def _build_live_portfolio_frame(self) -> QFrame:
        """Build the live portfolio summary panel."""
        frame = QFrame()
        frame.setStyleSheet(
            f"QFrame {{ background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 8px; padding: 12px; }}"
        )
        layout = QVBoxLayout(frame)
        layout.setSpacing(8)

        title = QLabel("Live Portfolio Summary")
        title.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 13px; font-weight: bold; border: none;"
        )
        layout.addWidget(title)

        self._portfolio_labels: dict[str, QLabel] = {}
        grid = QGridLayout()
        grid.setSpacing(12)
        metrics = [
            ("confirmed_value", "Confirmed Portfolio Value"),
            ("spendable_value", "Spendable Portfolio Value"),
            ("total_pnl", "Live Total P&L"),
            ("inventory_pnl", "Inventory Revaluation"),
            ("realized_pnl", "Live Realized P&L"),
            ("unrealized_pnl", "Live Unrealized P&L"),
        ]
        for idx, (key, label) in enumerate(metrics):
            row, col = divmod(idx, 3)
            cell = QVBoxLayout()
            desc = QLabel(label)
            desc.setStyleSheet(
                f"color: {TEXT_SECONDARY}; font-size: 10px; border: none;"
            )
            value = QLabel("—")
            value.setStyleSheet(
                f"color: {TEXT_PRIMARY}; font-size: 16px; font-weight: bold; "
                f"font-family: {_MONO}; border: none;"
            )
            cell.addWidget(desc)
            cell.addWidget(value)
            grid.addLayout(cell, row, col)
            self._portfolio_labels[key] = value

        layout.addLayout(grid)

        self._portfolio_note = QLabel("Waiting for live wallet and market data…")
        self._portfolio_note.setWordWrap(True)
        self._portfolio_note.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 10px; border: none;"
        )
        layout.addWidget(self._portfolio_note)
        return frame

    def _build_forecast_frame(self) -> QFrame:
        """Build the income forecast panel."""
        frame = QFrame()
        frame.setStyleSheet(
            f"QFrame {{ background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 8px; padding: 12px; }}"
        )
        layout = QVBoxLayout(frame)
        layout.setSpacing(8)

        title = QLabel("Income Forecast")
        title.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 13px; font-weight: bold; border: none;"
        )
        layout.addWidget(title)

        self._forecast_labels: dict[str, QLabel] = {}
        grid = QGridLayout()
        grid.setSpacing(12)
        metrics = [
            ("lookback", "Lookback"),
            ("daily_avg", "Avg Daily Net"),
            ("daily_vol", "Daily Volatility"),
            ("next_1d", "Expected Next 24h"),
            ("next_7d", "Expected Next 7d"),
            ("next_30d", "Expected Next 30d"),
            ("range_30d", "95% Range (30d)"),
            ("prob_positive", "P(30d > 0)"),
        ]
        for idx, (key, label) in enumerate(metrics):
            row, col = divmod(idx, 4)
            cell = QVBoxLayout()
            desc = QLabel(label)
            desc.setStyleSheet(
                f"color: {TEXT_SECONDARY}; font-size: 10px; border: none;"
            )
            value = QLabel("—")
            value.setStyleSheet(
                f"color: {TEXT_PRIMARY}; font-size: 15px; font-weight: bold; "
                f"font-family: {_MONO}; border: none;"
            )
            cell.addWidget(desc)
            cell.addWidget(value)
            grid.addLayout(cell, row, col)
            self._forecast_labels[key] = value

        layout.addLayout(grid)

        self._forecast_note = QLabel(
            "Forecast uses trailing calendar-day realised net P&L "
            "(realised P&L minus fees), with zero-fill days included. "
            "Inventory mark-to-market is shown separately above."
        )
        self._forecast_note.setWordWrap(True)
        self._forecast_note.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 10px; border: none;"
        )
        layout.addWidget(self._forecast_note)
        return frame

    def _build_pair_tab(self) -> QWidget:
        """Build the per-pair breakdown tab."""
        w = QWidget()
        layout = QVBoxLayout(w)
        layout.setContentsMargins(8, 8, 8, 8)

        cols = [
            "Pair", "Trades", "Realized P&L (USDC)", "Win Rate",
            "Base Volume", "Fees (USDC)", "Avg Cost Basis", "Best", "Worst",
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

        cols = ["Date", "Trades", "Gross P&L (USDC)", "Fees (USDC)", "Net P&L (USDC)"]
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

    def set_live_context(
        self,
        *,
        xch_usd_rate: float,
        market_data: dict[str, dict[str, float]],
        wallet_balances: dict[str, dict[str, float]],
        pnl: dict[str, Any],
    ) -> None:
        """Inject current market, wallet, and live P&L context."""
        self._xch_usd_rate = max(0.0, float(xch_usd_rate or 0.0))
        self._market_data = dict(market_data or {})
        self._wallet_balances = dict(wallet_balances or {})
        self._live_pnl = dict(pnl or {})

        if self._last_reports:
            self._update_performance(self._last_reports.get("periods", {}))
            self._update_pairs(self._last_reports.get("per_pair", []))
            self._update_capgains(self._last_reports.get("capital_gains", {}))
            self._update_top_trades(
                self._last_reports.get("top_trades", []),
                self._last_reports.get("worst_trades", []),
            )
            self._update_daily(self._last_reports.get("daily_pnl", []))
            self._update_forecast(self._last_reports.get("forecast", {}))

        self._update_portfolio_summary()

    @Slot(dict)
    def update_reports(self, data: dict[str, Any]) -> None:
        """Update all report tabs with fresh data.

        Parameters
        ----------
        data:
            Report dict from ``DatabaseService.reports_loaded``, with
            keys: ``periods``, ``per_pair``, ``capital_gains``,
            ``offer_stats``, ``top_trades``, ``worst_trades``,
            ``daily_pnl``, ``forecast``.
        """
        if not data:
            return

        self._last_reports = dict(data)

        self._update_performance(data.get("periods", {}))
        self._update_pairs(data.get("per_pair", []))
        self._update_capgains(data.get("capital_gains", {}))
        self._update_offers(data.get("offer_stats", {}))
        self._update_top_trades(
            data.get("top_trades", []),
            data.get("worst_trades", []),
        )
        self._update_daily(data.get("daily_pnl", []))
        self._update_forecast(data.get("forecast", {}))
        self._update_portfolio_summary()

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
            pnl_usdc = p.get("total_pnl_usdc")
            if pnl_usdc is not None:
                pnl_text = _money_text_from_usdc(float(pnl_usdc), signed=True)
                sign_value = float(pnl_usdc)
            else:
                pnl_text = _money_text_from_mojos(
                    pnl,
                    self._xch_usd_rate,
                    signed=True,
                )
                sign_value = float(pnl)

            if sign_value > 0:
                color = PROFIT_GREEN
            elif sign_value < 0:
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
            if p.get("total_fees_usdc") is not None:
                fees = _money_text_from_usdc(
                    float(p.get("total_fees_usdc", 0.0)),
                    signed=False,
                    as_cost=True,
                )
            else:
                fees = _money_text_from_mojos(
                    int(p.get("total_fees", 0)),
                    self._xch_usd_rate,
                    signed=False,
                    as_cost=True,
                )

            if p.get("avg_pnl_usdc") is not None:
                avg_pnl = _money_text_from_usdc(
                    float(p.get("avg_pnl_usdc", 0.0)),
                    signed=True,
                )
            else:
                avg_pnl = _money_text_from_mojos(
                    int(p.get("avg_pnl", 0)),
                    self._xch_usd_rate,
                    signed=True,
                )

            labels["details"].setText(
                f"Trades: {trades}  |  W/L: {wins}/{losses}  |  "
                f"Win Rate: {win_rate:.0f}%\n"
                f"Avg Trade: {avg_pnl}  |  Fees: {fees}"
            )

    def _update_pairs(self, pairs: list[dict]) -> None:
        """Update the per-pair breakdown table."""
        self._pair_table.setRowCount(len(pairs))

        for row, p in enumerate(pairs):
            trades = p.get("trade_count", 0)
            wins = p.get("wins", 0)
            win_rate = (wins / trades * 100) if trades > 0 else 0

            pair_name = p.get("pair_name", "")

            self._pair_table.setItem(row, 0, _text_item(pair_name))
            self._pair_table.setItem(row, 1, _num_item(trades))
            if p.get("total_pnl_usdc") is not None:
                self._pair_table.setItem(
                    row,
                    2,
                    _money_item_from_usdc(
                        float(p.get("total_pnl_usdc", 0.0)),
                        signed=True,
                    ),
                )
            else:
                self._pair_table.setItem(
                    row,
                    2,
                    _money_item_from_mojos(
                        p.get("total_pnl", 0),
                        self._xch_usd_rate,
                        signed=True,
                    ),
                )
            self._pair_table.setItem(row, 3, _pct_item(win_rate))
            self._pair_table.setItem(
                row,
                4,
                _size_item(pair_name, int(p.get("total_volume", 0))),
            )
            if p.get("total_fees_usdc") is not None:
                self._pair_table.setItem(
                    row,
                    5,
                    _money_item_from_usdc(
                        float(p.get("total_fees_usdc", 0.0)),
                        signed=False,
                        as_cost=True,
                    ),
                )
            else:
                self._pair_table.setItem(
                    row,
                    5,
                    _money_item_from_mojos(
                        p.get("total_fees", 0),
                        self._xch_usd_rate,
                        signed=False,
                        as_cost=True,
                    ),
                )
            self._pair_table.setItem(
                row, 6,
                _price_item(pair_name, int(round(float(p.get("avg_cost_basis", 0) or 0)))),
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
            usdc_key = f"{key}_usdc"
            usdc_value = cg.get(usdc_key)
            if usdc_value is not None:
                text = _money_text_from_usdc(
                    float(usdc_value),
                    signed=(key != "fees_deductible"),
                    as_cost=(key == "fees_deductible"),
                )
                value_sign = float(usdc_value)
            else:
                text = _money_text_from_mojos(
                    val,
                    self._xch_usd_rate,
                    signed=(key != "fees_deductible"),
                    as_cost=(key == "fees_deductible"),
                )
                value_sign = float(val)
            self._cg_labels[key].setText(text)

            if key == "fees_deductible":
                color = TEXT_PRIMARY
            elif value_sign > 0:
                color = PROFIT_GREEN
            elif value_sign < 0:
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

            pair_name = t.get("pair_name", "")
            table.setItem(row, 3, _price_item(pair_name, int(t.get("price_mojos", 0))))
            table.setItem(row, 4, _size_item(pair_name, int(t.get("size_mojos", 0))))
            if t.get("realized_pnl_usdc") is not None:
                table.setItem(
                    row,
                    5,
                    _money_item_from_usdc(
                        float(t.get("realized_pnl_usdc", 0.0)),
                        signed=True,
                    ),
                )
            else:
                table.setItem(
                    row,
                    5,
                    _money_item_from_mojos(
                        t.get("realized_pnl_mojos", 0),
                        self._xch_usd_rate,
                        signed=True,
                    ),
                )
            table.setItem(
                row,
                6,
                _price_item(pair_name, int(t.get("cost_basis_mojos", 0))),
            )

    def _update_daily(self, daily: list[dict]) -> None:
        """Update the daily P&L table."""
        self._daily_table.setRowCount(len(daily))

        for row, d in enumerate(daily):
            self._daily_table.setItem(row, 0, _text_item(d.get("trade_date", "")))
            self._daily_table.setItem(row, 1, _num_item(d.get("trade_count", 0)))

            pnl = int(d.get("daily_pnl", 0))
            if d.get("daily_pnl_usdc") is not None:
                self._daily_table.setItem(
                    row,
                    2,
                    _money_item_from_usdc(
                        float(d.get("daily_pnl_usdc", 0.0)),
                        signed=True,
                    ),
                )
            else:
                self._daily_table.setItem(
                    row,
                    2,
                    _money_item_from_mojos(pnl, self._xch_usd_rate, signed=True),
                )

            fees = int(d.get("daily_fees", 0))
            if d.get("daily_fees_usdc") is not None:
                self._daily_table.setItem(
                    row,
                    3,
                    _money_item_from_usdc(
                        float(d.get("daily_fees_usdc", 0.0)),
                        signed=False,
                        as_cost=True,
                    ),
                )
            else:
                self._daily_table.setItem(
                    row,
                    3,
                    _money_item_from_mojos(
                        fees,
                        self._xch_usd_rate,
                        signed=False,
                        as_cost=True,
                    ),
                )

            net = pnl - fees
            if d.get("daily_net_usdc") is not None:
                self._daily_table.setItem(
                    row,
                    4,
                    _money_item_from_usdc(
                        float(d.get("daily_net_usdc", 0.0)),
                        signed=True,
                    ),
                )
            else:
                self._daily_table.setItem(
                    row,
                    4,
                    _money_item_from_mojos(net, self._xch_usd_rate, signed=True),
                )

    def _set_metric_value(self, label: QLabel, value_text: str, value_sign: int) -> None:
        """Apply consistent styling to summary metric values."""
        if value_sign > 0:
            color = PROFIT_GREEN
        elif value_sign < 0:
            color = LOSS_RED
        else:
            color = TEXT_PRIMARY

        label.setText(value_text)
        label.setStyleSheet(
            f"color: {color}; font-size: 16px; font-weight: bold; "
            f"font-family: {_MONO}; border: none;"
        )

    def _asset_prices_usdc(self) -> dict[str, float]:
        """Derive a best-effort USDC price map from live market data."""
        prices: dict[str, float] = {}
        if self._xch_usd_rate > 0:
            prices["XCH"] = self._xch_usd_rate

        for pair_name in self._market_data:
            base_asset, quote_asset = _split_pair(pair_name)
            if _is_stablecoin_symbol(base_asset):
                prices[base_asset.upper()] = 1.0
            if _is_stablecoin_symbol(quote_asset):
                prices[quote_asset.upper()] = 1.0

        for _ in range(max(1, len(self._market_data) * 2)):
            changed = False
            for pair_name, md in self._market_data.items():
                mid_price = float(md.get("mid_price", 0.0) or 0.0)
                if mid_price <= 0.0:
                    continue

                base_asset, quote_asset = _split_pair(pair_name)
                base_key = base_asset.upper()
                quote_key = quote_asset.upper()

                # The engine encodes `mid_price` as `display_price * 1e12`
                # for every pair regardless of which side is XCH (see
                # cpp/src/execution/market_data.cpp publish_snapshot).
                # The quote asset's mojo-per-unit scale is NOT used here
                # -- always divide by 1e12 to recover the display price
                # (quote-units per base-unit).
                price_in_quote_units = mid_price / 1_000_000_000_000.0
                if price_in_quote_units <= 0.0:
                    continue

                if quote_key in prices and base_key not in prices:
                    prices[base_key] = price_in_quote_units * prices[quote_key]
                    changed = True

                if base_key in prices and quote_key not in prices:
                    prices[quote_key] = prices[base_key] / price_in_quote_units
                    changed = True

            if not changed:
                break

        return prices

    def _wallet_asset_symbol(
        self,
        wallet_name: str,
        wallet_data: dict[str, float],
        asset_prices: dict[str, float],
    ) -> Optional[str]:
        """Infer the asset symbol represented by a wallet balance row."""
        if int(wallet_data.get("wallet_type", -1)) == 0:
            return "XCH"

        normalized = wallet_name.strip().upper()
        compact = normalized.replace(" ", "")

        if _is_stablecoin_symbol(normalized) or "USDC" in compact:
            for candidate in asset_prices:
                if _is_stablecoin_symbol(candidate):
                    return candidate
            return normalized

        if normalized in asset_prices:
            return normalized

        for candidate in asset_prices:
            if candidate in compact or compact in candidate:
                return candidate

        return None

    def _portfolio_values_usdc(self) -> tuple[float, float, int, int]:
        """Return confirmed/spendable portfolio values and pricing coverage."""
        asset_prices = self._asset_prices_usdc()
        confirmed_total = 0.0
        spendable_total = 0.0
        priced_wallets = 0
        total_wallets = len(self._wallet_balances)

        for wallet_name, wallet_data in self._wallet_balances.items():
            asset_symbol = self._wallet_asset_symbol(wallet_name, wallet_data, asset_prices)
            if not asset_symbol:
                continue

            price_usdc = asset_prices.get(asset_symbol.upper(), asset_prices.get(asset_symbol, 0.0))
            if price_usdc <= 0.0:
                continue

            priced_wallets += 1
            confirmed_total += float(wallet_data.get("confirmed", 0.0)) * price_usdc
            spendable_total += float(wallet_data.get("spendable", 0.0)) * price_usdc

        return confirmed_total, spendable_total, priced_wallets, total_wallets

    def _update_portfolio_summary(self) -> None:
        """Refresh live portfolio-value and mark-to-market labels."""
        confirmed_value, spendable_value, priced_wallets, total_wallets = (
            self._portfolio_values_usdc()
        )
        live_total = int(self._live_pnl.get("total", 0) or 0)
        live_realized = int(self._live_pnl.get("realized", 0) or 0)
        live_unrealized = int(self._live_pnl.get("unrealized", 0) or 0)
        live_inventory = int(self._live_pnl.get("inventory", 0) or 0)

        self._set_metric_value(
            self._portfolio_labels["confirmed_value"],
            _format_usdc(confirmed_value, signed=False),
            1 if confirmed_value > 0 else 0,
        )
        self._set_metric_value(
            self._portfolio_labels["spendable_value"],
            _format_usdc(spendable_value, signed=False),
            1 if spendable_value > 0 else 0,
        )
        self._set_metric_value(
            self._portfolio_labels["total_pnl"],
            _money_text_from_mojos(live_total, self._xch_usd_rate, signed=True),
            live_total,
        )
        self._set_metric_value(
            self._portfolio_labels["inventory_pnl"],
            _money_text_from_mojos(live_inventory, self._xch_usd_rate, signed=True),
            live_inventory,
        )
        self._set_metric_value(
            self._portfolio_labels["realized_pnl"],
            _money_text_from_mojos(live_realized, self._xch_usd_rate, signed=True),
            live_realized,
        )
        self._set_metric_value(
            self._portfolio_labels["unrealized_pnl"],
            _money_text_from_mojos(live_unrealized, self._xch_usd_rate, signed=True),
            live_unrealized,
        )

        if total_wallets > 0:
            coverage = f"Priced wallets: {priced_wallets}/{total_wallets}"
        else:
            coverage = "No live wallet data yet"

        if self._xch_usd_rate > 0:
            mark_text = f"XCH mark: {_format_usdc(self._xch_usd_rate, signed=False, decimals=4)}"
        else:
            mark_text = "No live XCH/USDC mark available"

        self._portfolio_note.setText(
            f"Inventory revaluation includes XCH price drift. {coverage}. {mark_text}."
        )

    def _update_forecast(self, forecast: dict[str, Any]) -> None:
        """Update the forecast panel from trailing net P&L statistics."""
        if not forecast:
            for label in self._forecast_labels.values():
                label.setText("—")
            self._forecast_note.setText(
                "Need more trade history to estimate future income. "
                "Forecast will appear after enough daily P&L history accumulates."
            )
            return

        lookback_days = int(forecast.get("lookback_days", 0) or 0)
        active_days = int(forecast.get("active_days", 0) or 0)
        total_trades = int(forecast.get("total_trades", 0) or 0)
        use_usdc = str(forecast.get("unit", "")).upper() == "USDC"
        mean_daily = float(
            forecast.get("mean_daily_net_usdc", forecast.get("mean_daily_net_pnl", 0.0)) or 0.0
        )
        daily_vol = float(
            forecast.get("daily_net_volatility_usdc", forecast.get("daily_net_volatility", 0.0)) or 0.0
        )
        profit_day_rate = float(forecast.get("profit_day_rate_pct", 0.0) or 0.0)
        confidence = str(forecast.get("confidence", "very low"))

        next_1d = dict(forecast.get("next_1d", {}))
        next_7d = dict(forecast.get("next_7d", {}))
        next_30d = dict(forecast.get("next_30d", {}))

        def _forecast_money_text(bucket: dict[str, Any], usdc_key: str, mojo_key: str) -> str:
            if use_usdc:
                return _money_text_from_usdc(float(bucket.get(usdc_key, 0.0) or 0.0), signed=True)
            return _money_text_from_mojos(
                int(round(float(bucket.get(mojo_key, 0.0) or 0.0))),
                self._xch_usd_rate,
                signed=True,
            )

        def _forecast_scalar_text(value: float, *, signed: bool) -> str:
            if use_usdc:
                return _money_text_from_usdc(value, signed=signed)
            return _money_text_from_mojos(
                int(round(value)),
                self._xch_usd_rate,
                signed=signed,
            )

        self._forecast_labels["lookback"].setText(
            f"{lookback_days}d / {active_days} active / {total_trades} fills"
        )
        self._forecast_labels["daily_avg"].setText(_forecast_scalar_text(mean_daily, signed=True))
        self._forecast_labels["daily_vol"].setText(_forecast_scalar_text(abs(daily_vol), signed=False))
        self._forecast_labels["next_1d"].setText(
            _forecast_money_text(next_1d, "expected_usdc", "expected")
        )
        self._forecast_labels["next_7d"].setText(
            _forecast_money_text(next_7d, "expected_usdc", "expected")
        )
        self._forecast_labels["next_30d"].setText(
            _forecast_money_text(next_30d, "expected_usdc", "expected")
        )
        range_text = (
            f"{_forecast_money_text(next_30d, 'low_95_usdc', 'low_95')}"
            f" to "
            f"{_forecast_money_text(next_30d, 'high_95_usdc', 'high_95')}"
        )
        self._forecast_labels["range_30d"].setText(range_text)
        self._forecast_labels["prob_positive"].setText(
            f"{float(next_30d.get('prob_positive_pct', 0.0) or 0.0):.1f}%"
        )

        self._forecast_note.setText(
            "Forecast uses trailing calendar-day realised net P&L (realised P&L minus fees), "
            f"including zero-fill days. Positive-day rate: {profit_day_rate:.1f}%. "
            f"Confidence: {confidence}. Inventory mark-to-market is excluded from the forecast."
        )

    def clear(self) -> None:
        """Reset all tabs to their initial empty state."""
        for labels in self._perf_cards.values():
            labels["pnl"].setText("—")
            labels["details"].setText("")
        for label in self._portfolio_labels.values():
            label.setText("—")
        self._portfolio_note.setText("Waiting for live wallet and market data…")
        for label in self._forecast_labels.values():
            label.setText("—")
        self._forecast_note.setText(
            "Forecast uses trailing calendar-day realised net P&L (realised P&L minus fees), "
            "with zero-fill days included. Inventory mark-to-market is shown separately above."
        )
        self._pair_table.setRowCount(0)
        for lbl in self._cg_labels.values():
            lbl.setText("—")
        for lbl in self._offer_labels.values():
            lbl.setText("—")
        self._best_table.setRowCount(0)
        self._worst_table.setRowCount(0)
        self._daily_table.setRowCount(0)
        self._last_reports = {}
        self._status_label.setText("Waiting for report data…")
