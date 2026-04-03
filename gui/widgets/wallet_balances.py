"""Wallet balances page widget for XOPTrader GUI.

Displays per-wallet balance information (spendable, confirmed, pending,
unconfirmed) fetched from the Chia wallet RPC.  Data is pushed via the
``update_balances`` slot which the ``MainWindow`` calls on every bridge
refresh tick.

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
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C

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

# Table column headers.
_COLUMNS: Final[list[str]] = [
    "Wallet",
    "Spendable",
    "Confirmed",
    "Pending",
    "Unconfirmed",
    "Reserve %",
]

# Well-known wallet type names.
_WALLET_TYPES: Final[dict[int, str]] = {
    0: "XCH",
    6: "CAT",
}


class WalletBalancesWidget(QWidget):
    """Full-page widget showing wallet balances in a styled table.

    Parameters
    ----------
    parent : QWidget | None
        Optional parent widget.
    """

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._reserve: dict[str, float] = {}
        self._stuck_offers: int = 0
        self._build_ui()

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(16, 16, 16, 16)
        root.setSpacing(12)

        # -- Header --
        header = QLabel("Wallet Balances")
        header.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 18px; font-weight: bold;"
        )
        root.addWidget(header)

        desc = QLabel(
            "Live balances queried from the Chia wallet RPC.  "
            "Updates every ~30 seconds."
        )
        desc.setWordWrap(True)
        desc.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 12px;")
        root.addWidget(desc)

        # -- Summary cards row --
        self._summary_frame = QFrame()
        self._summary_frame.setStyleSheet(
            f"QFrame {{ background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 8px; padding: 12px; }}"
        )
        summary_layout = QHBoxLayout(self._summary_frame)
        summary_layout.setSpacing(24)

        self._total_spendable_label = self._make_summary_card(
            "Total Spendable", "—", summary_layout
        )
        self._total_confirmed_label = self._make_summary_card(
            "Total Confirmed", "—", summary_layout
        )
        self._total_pending_label = self._make_summary_card(
            "Total Pending", "—", summary_layout
        )
        self._wallet_count_label = self._make_summary_card(
            "Wallets", "0", summary_layout
        )
        root.addWidget(self._summary_frame)

        # -- Stuck offers warning --
        self._stuck_label = QLabel()
        self._stuck_label.setStyleSheet(
            f"color: {LOSS_RED}; font-size: 12px; font-weight: bold;"
        )
        self._stuck_label.setVisible(False)
        root.addWidget(self._stuck_label)

        # -- Balance table --
        self._table = QTableWidget(0, len(_COLUMNS))
        self._table.setHorizontalHeaderLabels(_COLUMNS)
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._table.setAlternatingRowColors(True)
        self._table.verticalHeader().setVisible(False)
        self._table.horizontalHeader().setStretchLastSection(True)
        self._table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )

        self._table.setStyleSheet(
            f"""
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
        )
        root.addWidget(self._table, stretch=1)

        # -- Status label --
        self._status_label = QLabel("Waiting for wallet data…")
        self._status_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        root.addWidget(self._status_label)

    def _make_summary_card(
        self, title: str, value: str, layout: QHBoxLayout
    ) -> QLabel:
        """Create a small summary card with a title and value label."""
        card = QVBoxLayout()
        card.setSpacing(2)
        title_lbl = QLabel(title)
        title_lbl.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 10px; border: none;"
        )
        title_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        card.addWidget(title_lbl)
        value_lbl = QLabel(value)
        value_lbl.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 16px; font-weight: bold; "
            f"font-family: {_MONO}; border: none;"
        )
        value_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        card.addWidget(value_lbl)
        layout.addLayout(card)
        return value_lbl

    @Slot(dict)
    def update_balances(
        self,
        balances: dict[str, dict[str, float]],
        reserve: dict[str, float] | None = None,
        stuck_offers: int = 0,
    ) -> None:
        """Update the widget with fresh wallet balance data.

        Parameters
        ----------
        balances:
            Mapping of wallet name to balance dict with keys
            ``spendable``, ``confirmed``, ``pending_change``,
            ``unconfirmed``, ``wallet_type``.
        reserve:
            Optional mapping of wallet name to spendable reserve ratio (0–1).
        stuck_offers:
            Number of stuck offers beyond TTL.
        """
        self._reserve = reserve or {}
        self._stuck_offers = stuck_offers

        if not balances:
            self._status_label.setText(
                "No wallet data — check Chia wallet connection"
            )
            return

        self._table.setRowCount(len(balances))

        total_spendable = 0.0
        total_confirmed = 0.0
        total_pending = 0.0

        for row, (wallet_name, bal) in enumerate(sorted(balances.items())):
            spendable = bal.get("spendable", 0.0)
            confirmed = bal.get("confirmed", 0.0)
            pending = bal.get("pending_change", 0.0)
            unconfirmed = bal.get("unconfirmed", 0.0)
            wallet_type = int(bal.get("wallet_type", 0))
            res_pct = self._reserve.get(wallet_name, -1.0)

            # Determine display suffix.
            type_name = _WALLET_TYPES.get(wallet_type, "Token")
            display_name = f"{wallet_name}  ({type_name})"

            # Reserve colour coding.
            if res_pct >= 0:
                res_display = f"{res_pct * 100:.1f}%"
                if res_pct < 0.10:
                    res_color = QColor(LOSS_RED)
                elif res_pct < 0.25:
                    res_color = QColor(WARNING)
                else:
                    res_color = QColor(PROFIT_GREEN)
            else:
                res_display = "—"
                res_color = QColor(TEXT_SECONDARY)

            # Only accumulate XCH for totals (avoid mixing units).
            if wallet_type == 0:
                total_spendable += spendable
                total_confirmed += confirmed
                total_pending += pending

            # Populate row.
            items: list[tuple[str, QColor | None]] = [
                (display_name, None),
                (self._fmt(spendable), None),
                (self._fmt(confirmed), None),
                (self._fmt(pending), QColor(WARNING) if pending != 0 else None),
                (self._fmt(unconfirmed), None),
                (res_display, res_color),
            ]

            for col, (text, color) in enumerate(items):
                item = QTableWidgetItem(text)
                item.setTextAlignment(
                    Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
                    if col > 0
                    else Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
                )
                if color:
                    item.setForeground(color)
                self._table.setItem(row, col, item)

        # Update summary cards.
        self._total_spendable_label.setText(f"{total_spendable:,.6f} XCH")
        self._total_confirmed_label.setText(f"{total_confirmed:,.6f} XCH")
        self._total_pending_label.setText(f"{total_pending:,.6f} XCH")
        self._wallet_count_label.setText(str(len(balances)))

        # Stuck offers warning.
        if stuck_offers > 0:
            self._stuck_label.setText(
                f"\u26a0 {stuck_offers} stuck offer(s) beyond TTL — "
                f"check fee levels"
            )
            self._stuck_label.setVisible(True)
        else:
            self._stuck_label.setVisible(False)

        self._status_label.setText(
            f"Last update: {len(balances)} wallet(s) loaded"
        )

    @staticmethod
    def _fmt(value: float) -> str:
        """Format a balance value for display."""
        if abs(value) < 0.000001:
            return "0"
        return f"{value:,.6f}"

    def clear(self) -> None:
        """Reset the widget to its initial empty state."""
        self._table.setRowCount(0)
        self._total_spendable_label.setText("—")
        self._total_confirmed_label.setText("—")
        self._total_pending_label.setText("—")
        self._wallet_count_label.setText("0")
        self._stuck_label.setVisible(False)
        self._status_label.setText("Waiting for wallet data…")
