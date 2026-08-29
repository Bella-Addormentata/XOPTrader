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

import json
import logging
from typing import Any, Final, Optional

from PySide6.QtCore import QObject, QSettings, Qt, QThread, Signal, Slot
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.theme import scaled_px, COLORS as _C
from gui.theme import MONO_FONT_FAMILY

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
    "Deployed %",
]

# Index of the "Deployed %" column (filled by _refresh_deployed_view).
_DEPLOYED_COL: Final[int] = _COLUMNS.index("Deployed %")

# Operator's recommended liquidity floor: warn when less than 20% of
# USD-weighted capital remains liquid (i.e. overall deployment > 80%).
_LIQUID_FLOOR_FRAC: Final[float] = 0.20

# Well-known wallet type names.
_WALLET_TYPES: Final[dict[int, str]] = {
    0: "XCH",
    6: "CAT",
}


def _is_stablecoin_symbol(symbol: str) -> bool:
    normalized = symbol.strip().upper()
    return normalized in {"WUSDC.B", "WUSDC", "USDC", "USDS", "USDT"}


def _split_pair(pair_name: str) -> tuple[str, str]:
    if "/" not in pair_name:
        return pair_name.strip(), ""
    base, quote = pair_name.split("/", 1)
    return base.strip(), quote.strip()


# QSettings identity for persisted GUI state.  Must match the
# (org, app) tuple used elsewhere in the GUI so all preferences live
# in one backing store.
_QSETTINGS_ORG: Final[str] = "XOP"
_QSETTINGS_APP: Final[str] = "XOPTrader"
_ALLOC_TARGETS_KEY: Final[str] = "wallet/allocation_targets"
_ALLOC_TOLERANCES_KEY: Final[str] = "wallet/allocation_tolerances"

_log = logging.getLogger(__name__)

# Tooltip base for the read-only "Suggested %" allocation column.
_SUGGESTED_ALLOC_TOOLTIP: Final[str] = (
    "Flow-keyed advisory: the best allocation of EXISTING capital -- ideal "
    "holdings from the offer-sizing calculator (dexie 7-day volume vs our "
    "fills, holdings cap inverted), normalized to 100%.  Re-computed each "
    "time this page opens.  Advisory only -- never applied automatically."
)


def _fit_table_to_contents(table: QTableWidget) -> None:
    """Size *table* to its rows so it never scrolls on its own.

    This page is already inside a QScrollArea (main_window builds it with
    scrollable=True), so a table that scrolls internally puts a second
    scrollbar inside the first.  Nested scrollbars are worse than untidy: the
    wheel acts on whichever widget happens to sit under the pointer, and rows
    can stay hidden inside a panel that looks complete -- on a page whose
    whole job is showing how much money is where.

    Height is summed from the CURRENT row heights rather than a per-row
    constant, so it follows whatever the rows actually measure.

    That is a statement about row GEOMETRY: the fit reads the rows rather
    than assuming a height.  Since S10 was fixed the local QSS derives its
    pixel sizes from theme.scaled_px(), so the font-size setting reaches
    this page too -- rows grow with the text and the fit follows them.
    """
    table.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    rows = sum(
        table.rowHeight(r)
        for r in range(table.rowCount())
        if not table.isRowHidden(r)
    )
    header = table.horizontalHeader().height()

    # Reserve the HORIZONTAL scrollbar when it is showing.  Stretch mode does
    # not rule one out: sections still honour minimumSectionSize, so a narrow
    # enough window brings the bar back -- reproduced at a 300px table width.
    # Since the vertical bar is off, any height it steals clips the last row
    # outright instead of becoming scrollable.
    # Reserve from the RANGE, not from isVisible().  This page normally sits
    # hidden in the stack while balance updates keep arriving, and a hidden
    # widget reports isVisible() False even once its content overflows --
    # measured: range 0..1 while hidden, then the bar appears on opening with
    # no further rangeChanged to trigger a refit, leaving the last row
    # clipped.  A nonzero range means the content overflows, hidden or not.
    hbar = table.horizontalScrollBar()
    overflows = bool(hbar) and hbar.maximum() > hbar.minimum()
    reserved = hbar.sizeHint().height() if overflows else 0

    wanted = header + rows + 2 * table.frameWidth() + reserved
    if table.height() != wanted:
        # Guarded: setFixedHeight can itself change the scroll range, and an
        # unguarded refit on that signal would recurse.
        table.setFixedHeight(wanted)

    if not table.property("_refit_connected"):
        table.setProperty("_refit_connected", True)
        # The bar appears and disappears as the window is resized, so the
        # reservation has to be re-evaluated when the range changes.
        hbar.rangeChanged.connect(lambda *_: _fit_table_to_contents(table))


class _SuggestedAllocationWorker(QObject):
    """Computes the advisory per-asset portfolio allocation off the UI
    thread (dexie HTTP fetch + read-only SQLite reads), following the
    database_service worker pattern: a QObject moved to a QThread whose
    results come back via queued signals.  Never blocks the event loop.
    """

    ready = Signal(dict)   # payload of offer_sizing.suggested_portfolio_allocation
    failed = Signal(str)

    def __init__(self, config_path: Optional[str] = None,
                 db_path: Optional[str] = None) -> None:
        super().__init__()
        # Passed explicitly because offer_sizing is loaded BY PATH out of the
        # PyInstaller bundle: its __file__-relative defaults point at the
        # per-launch _MEIPASS temp directory, which holds no config.yaml and
        # no database.  Calling with no paths is what produced
        # "Suggested % unavailable: [Errno 2] ... \_MEI00004b102\config.yaml".
        self._config_path = config_path
        self._db_path = db_path

    @Slot()
    def run(self) -> None:
        try:
            from gui.utils import load_offer_sizing  # noqa: WPS433

            sizing = load_offer_sizing()
            self.ready.emit(dict(sizing.suggested_portfolio_allocation(
                config_path=self._config_path,
                db_path=self._db_path,
            )))
        except Exception as exc:  # fail soft -> "n/a" in the table
            self.failed.emit(str(exc))


class WalletBalancesWidget(QWidget):
    """Full-page widget showing wallet balances in a styled table.

    Parameters
    ----------
    parent : QWidget | None
        Optional parent widget.
    """

    allocation_targets_applied = Signal(dict, dict, dict, dict)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._reserve: dict[str, float] = {}
        self._stuck_offers: int = 0
        self._last_balances: dict[str, dict[str, float]] = {}
        self._last_market_data: dict[str, dict[str, float]] = {}
        # [DEPLOYED 2026-08-04] Per-asset resting-offer amounts from
        # DatabaseService.query_deployed_capital (offer_log pending rows,
        # keyed by upper-cased symbol, in whole asset units).
        self._offered_units: dict[str, float] = {}
        self._offer_counts: dict[str, int] = {}
        self._pending_offers: int = 0
        # Wallet name per balances-table row (populated by update_balances)
        # so the Deployed % column can be refreshed without a full rebuild.
        self._row_wallets: list[str] = []
        # Pair configs from config.yaml (each with at least "name" and
        # "enabled" keys).  Used to restrict the Target Allocation table
        # to assets that participate in at least one enabled pair.
        self._last_pairs_cfg: list[dict[str, Any]] = []
        self._target_allocations: dict[str, float] = {}
        self._target_tolerances: dict[str, float] = {}
        self._alloc_updating = False
        # Advisory suggested allocation (flow-keyed calculator output).
        # None until the first async compute lands; _suggested_status is
        # "pending", "ready" or "failed: <msg>".
        self._suggested_alloc: Optional[dict[str, Any]] = None
        self._suggested_status: str = "pending"
        self._suggest_thread: Optional[QThread] = None
        # Keep a Python reference to the worker while it runs: PySide6
        # would otherwise garbage-collect the unparented QObject before
        # the thread invokes it.
        self._suggest_worker: Optional[_SuggestedAllocationWorker] = None
        # Real config/database locations, supplied by main_window.  Without
        # them the calculator falls back to bundle-relative defaults that do
        # not exist in an installed build (see _SuggestedAllocationWorker).
        self._sizing_config_path: Optional[str] = None
        self._sizing_db_path: Optional[str] = None
        # Restore persisted targets before the UI is built so the first
        # render already has them.
        self._load_target_allocations()
        self._load_target_tolerances()
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

        # -- Deployed-capital summary line --
        self._deployed_label = QLabel("Deployed: —")
        self._deployed_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 12px; "
            f"font-family: {MONO_FONT_FAMILY};"
        )
        self._deployed_label.setToolTip(
            "Locked = confirmed − spendable (the wallet's real committed "
            "value; offers lock whole coins, so this can exceed offer "
            "sizes).\n"
            "Offered = sum of resting offer sizes from offer_log "
            "(status='pending').\n"
            "Percentages are USD-weighted across priced assets."
        )
        root.addWidget(self._deployed_label)

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
        # Rows follow the scaled font: 12px text sits in a 30px row at the
        # default delta (12 + 18), and grows with it so enlarged text is not
        # clipped by a fixed row.
        self._table.verticalHeader().setDefaultSectionSize(
            max(30, scaled_px(12) + 18)
        )
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
                font-size: {scaled_px(12)}px;
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
                font-size: {scaled_px(11)}px;
                font-weight: bold;
            }}
            """
        )
        # No stretch: the table now takes exactly its content height and the
        # page's own scroll area supplies the scrolling.
        root.addWidget(self._table)

        # -- Target allocation panel --
        self._alloc_frame = QFrame()
        self._alloc_frame.setStyleSheet(
            f"QFrame {{ background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 8px; padding: 10px; }}"
        )
        alloc_layout = QVBoxLayout(self._alloc_frame)
        alloc_layout.setSpacing(8)

        alloc_header = QLabel("Target Portfolio Allocation")
        alloc_header.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 14px; font-weight: bold;"
        )
        alloc_layout.addWidget(alloc_header)

        alloc_desc = QLabel(
            "Set target percentages by asset (confirmed value basis). "
            "Targets should sum to 100%."
        )
        alloc_desc.setWordWrap(True)
        alloc_desc.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 11px;")
        alloc_layout.addWidget(alloc_desc)

        self._alloc_table = QTableWidget(0, 6)
        self._alloc_table.setHorizontalHeaderLabels(
            [
                "Asset", "Current %", "Target %", "Suggested %",
                "Target % +/-", "Delta %",
            ]
        )
        self._alloc_table.setSelectionBehavior(
            QTableWidget.SelectionBehavior.SelectRows
        )
        self._alloc_table.verticalHeader().setVisible(False)
        self._alloc_table.verticalHeader().setDefaultSectionSize(
            max(30, scaled_px(12) + 18)   # same rule as the balances table
        )
        self._alloc_table.horizontalHeader().setStretchLastSection(True)
        self._alloc_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )
        self._alloc_table.setStyleSheet(
            f"""
            QTableWidget {{
                background-color: {DARK_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
                border-radius: 4px;
                gridline-color: {BORDER};
                font-family: {_MONO};
                font-size: {scaled_px(12)}px;
            }}
            QHeaderView::section {{
                background-color: {ELEVATED_BG};
                color: {TEXT_SECONDARY};
                border: 1px solid {BORDER};
                padding: 4px 8px;
                font-size: {scaled_px(11)}px;
                font-weight: bold;
            }}
            """
        )
        self._alloc_table.cellChanged.connect(self._on_alloc_cell_changed)
        alloc_layout.addWidget(self._alloc_table)

        # Advisory footnote for the Suggested % column.  States the
        # marginal-priority tension explicitly once results arrive.
        self._alloc_suggest_note = QLabel("")
        self._alloc_suggest_note.setWordWrap(True)
        self._alloc_suggest_note.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        self._alloc_suggest_note.setVisible(False)
        alloc_layout.addWidget(self._alloc_suggest_note)

        alloc_controls = QHBoxLayout()
        self._alloc_sum_label = QLabel("Target sum: —")
        self._alloc_sum_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        self._alloc_hint_label = QLabel("")
        self._alloc_hint_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        normalize_btn = QPushButton("Normalize Targets to 100%")
        normalize_btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {ELEVATED_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 11px;
            }}
            QPushButton:hover {{
                border-color: {PRIMARY_GREEN};
            }}
            """
        )
        normalize_btn.clicked.connect(self._normalize_targets)
        self._apply_alloc_btn = QPushButton("Apply Targets To Strategy")
        self._apply_alloc_btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {PRIMARY_GREEN};
                color: {DARK_BG};
                border: 1px solid {PRIMARY_GREEN};
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 11px;
                font-weight: bold;
            }}
            QPushButton:hover {{
                background-color: {LIGHT_GREEN};
                border-color: {LIGHT_GREEN};
            }}
            """
        )
        self._apply_alloc_btn.clicked.connect(self._apply_targets)
        alloc_controls.addWidget(self._alloc_sum_label)
        alloc_controls.addWidget(self._alloc_hint_label, stretch=1)
        alloc_controls.addWidget(normalize_btn)
        alloc_controls.addWidget(self._apply_alloc_btn)
        alloc_layout.addLayout(alloc_controls)

        root.addWidget(self._alloc_frame)

        # -- Status label --
        self._status_label = QLabel("Waiting for wallet data…")
        self._status_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        root.addWidget(self._status_label)

        # Absorbs the leftover space when the tables are short; without it
        # the fixed-height panels would spread down the page.
        root.addStretch(1)

        # Fit them EMPTY too.  This page starts with no wallet data -- the
        # main window forwards an empty mapping on startup -- and an unfitted
        # table keeps its ~480px default size hint, so the page would open
        # with a large outer scroll range holding nothing.
        _fit_table_to_contents(self._table)
        _fit_table_to_contents(self._alloc_table)

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
        market_data: dict[str, dict[str, float]] | None = None,
        stuck_offers: int = 0,
        pairs: list[dict[str, Any]] | None = None,
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
        self._last_balances = dict(balances or {})
        self._last_market_data = dict(market_data or {})
        if pairs is not None:
            self._last_pairs_cfg = list(pairs)

        if not balances:
            self._status_label.setText(
                "No wallet data — check Chia wallet connection"
            )
            # Returning here skips the fit at the end of this method, so the
            # table must be sized on the way out as well.
            _fit_table_to_contents(self._table)
            return

        self._table.setRowCount(len(balances))
        self._row_wallets = [name for name, _ in sorted(balances.items())]

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

            # Populate row.  The Deployed % placeholder is filled by
            # _refresh_deployed_view() below.
            items: list[tuple[str, QColor | None]] = [
                (display_name, None),
                (self._fmt(spendable), None),
                (self._fmt(confirmed), None),
                (self._fmt(pending), QColor(WARNING) if pending != 0 else None),
                (self._fmt(unconfirmed), None),
                (res_display, res_color),
                ("—", None),
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
        _fit_table_to_contents(self._table)
        self._refresh_deployed_view()
        self._refresh_allocation_table()

    @Slot(dict)
    def update_deployed(self, payload: dict[str, Any]) -> None:
        """Receive per-asset resting-offer amounts from the database.

        Parameters
        ----------
        payload:
            Dict from ``DatabaseService.deployed_loaded`` with keys
            ``offered_units`` (symbol -> whole asset units),
            ``offer_counts`` (symbol -> pending offer count) and
            ``pending_offers`` (total pending count).
        """
        self._offered_units = {
            str(key).upper(): float(value)
            for key, value in (payload.get("offered_units") or {}).items()
        }
        self._offer_counts = {
            str(key).upper(): int(value)
            for key, value in (payload.get("offer_counts") or {}).items()
        }
        self._pending_offers = int(payload.get("pending_offers", 0) or 0)
        self._refresh_deployed_view()

    def _refresh_deployed_view(self) -> None:
        """Fill the Deployed % column and the deployment summary line.

        Two measures per asset:

        - **locked**  = confirmed − spendable from the wallet RPC.  This is
          the real committed value: on-chain offers lock WHOLE coins, so a
          2.8-XCH coin backing a 1-XCH offer locks all 2.8 XCH.
        - **offered** = sum of resting offer sizes from ``offer_log``
          (status='pending'), via DatabaseService.

        The column shows the locked-based percentage (the honest number);
        cell tooltips carry both figures.  The summary line is USD-weighted
        with the same price graph used by the Target Allocation panel and
        switches to the warning colour when less than 20% of capital
        remains liquid.
        """
        balances = self._last_balances
        if not balances:
            self._deployed_label.setText("Deployed: — (waiting for wallet data)")
            return

        asset_prices = self._asset_prices_usdc()
        asset_id_map = self._asset_id_symbol_map()

        # Resolve each wallet to a display symbol and aggregate confirmed
        # units per symbol (needed to express "offered" as a percentage).
        wallet_symbols: dict[str, Optional[str]] = {}
        confirmed_by_symbol: dict[str, float] = {}
        for wallet_name, bal in balances.items():
            symbol = self._wallet_asset_symbol(
                wallet_name, bal, asset_prices, asset_id_map
            )
            key = symbol.upper() if symbol else None
            wallet_symbols[wallet_name] = key
            if key:
                confirmed_by_symbol[key] = (
                    confirmed_by_symbol.get(key, 0.0)
                    + float(bal.get("confirmed", 0.0) or 0.0)
                )

        # -- Per-row column ------------------------------------------------
        for row, wallet_name in enumerate(self._row_wallets):
            item = self._table.item(row, _DEPLOYED_COL)
            if item is None:
                continue
            bal = balances.get(wallet_name, {})
            confirmed = float(bal.get("confirmed", 0.0) or 0.0)
            spendable = float(bal.get("spendable", 0.0) or 0.0)
            locked = max(0.0, confirmed - spendable)

            key = wallet_symbols.get(wallet_name)
            offered = self._offered_units.get(key, 0.0) if key else 0.0
            offer_count = self._offer_counts.get(key, 0) if key else 0
            symbol_confirmed = (
                confirmed_by_symbol.get(key, confirmed) if key else confirmed
            )

            if confirmed > 0.0:
                locked_pct = locked / confirmed * 100.0
                item.setText(f"{locked_pct:.1f}%")
                # Subtle cue when this asset itself is below the 20%
                # liquidity floor; default colour otherwise.
                if locked_pct > (1.0 - _LIQUID_FLOOR_FRAC) * 100.0:
                    item.setForeground(QColor(WARNING))
                else:
                    item.setForeground(QColor(TEXT_PRIMARY))
            else:
                item.setText("—")
                item.setForeground(QColor(TEXT_SECONDARY))

            tooltip_lines = [
                f"Locked (confirmed − spendable): {self._fmt(locked)}",
            ]
            if key:
                offered_pct = (
                    f" = {offered / symbol_confirmed * 100.0:.1f}% of {key}"
                    if symbol_confirmed > 0.0 else ""
                )
                tooltip_lines.append(
                    f"Offered ({offer_count} resting offer(s)): "
                    f"{self._fmt(offered)}{offered_pct}"
                )
            tooltip_lines.append(
                "Offers lock whole coins, so locked can exceed offered."
            )
            item.setToolTip("\n".join(tooltip_lines))

        # -- USD-weighted summary line -------------------------------------
        total_usd = 0.0
        locked_usd = 0.0
        for wallet_name, bal in balances.items():
            key = wallet_symbols.get(wallet_name)
            price = asset_prices.get(key, 0.0) if key else 0.0
            if price <= 0.0:
                continue
            confirmed = float(bal.get("confirmed", 0.0) or 0.0)
            spendable = float(bal.get("spendable", 0.0) or 0.0)
            total_usd += confirmed * price
            locked_usd += max(0.0, confirmed - spendable) * price

        offered_usd = sum(
            units * asset_prices.get(key, 0.0)
            for key, units in self._offered_units.items()
            if asset_prices.get(key, 0.0) > 0.0
        )

        if total_usd <= 0.0:
            self._deployed_label.setText(
                "Deployed: — (waiting for market data)"
            )
            self._deployed_label.setStyleSheet(
                f"color: {TEXT_SECONDARY}; font-size: 12px; "
                f"font-family: {MONO_FONT_FAMILY};"
            )
            return

        deployed_frac = locked_usd / total_usd
        liquid_frac = 1.0 - deployed_frac
        offered_pct = offered_usd / total_usd * 100.0
        self._deployed_label.setText(
            f"Deployed: {deployed_frac * 100.0:.1f}% of "
            f"${total_usd:,.2f} total (locked) · "
            f"offered {offered_pct:.1f}% "
            f"({self._pending_offers} offer(s)) · "
            f"liquid {liquid_frac * 100.0:.1f}%"
        )
        # Subtle cue when overall deployment leaves <20% liquid.
        colour = WARNING if liquid_frac < _LIQUID_FLOOR_FRAC else TEXT_SECONDARY
        self._deployed_label.setStyleSheet(
            f"color: {colour}; font-size: 12px; "
            f"font-family: {MONO_FONT_FAMILY};"
        )

    def _asset_prices_usdc(self) -> dict[str, float]:
        prices: dict[str, float] = {}

        # Seed stablecoin anchors at 1 USDC.  Pull from BOTH pair names
        # and wallet names so a stablecoin wallet still gets priced even
        # if no pair references it directly.
        for pair_name in self._last_market_data:
            base_asset, quote_asset = _split_pair(pair_name)
            if _is_stablecoin_symbol(base_asset):
                prices[base_asset.upper()] = 1.0
            if _is_stablecoin_symbol(quote_asset):
                prices[quote_asset.upper()] = 1.0
        for wallet_name in self._last_balances:
            normalized = wallet_name.strip().upper()
            compact = normalized.replace(" ", "")
            if _is_stablecoin_symbol(normalized):
                prices.setdefault(normalized, 1.0)
            elif _is_stablecoin_symbol(compact):
                prices.setdefault(compact, 1.0)

        # Iteratively propagate price graph from pair mids.
        for _ in range(4):
            changed = False
            for pair_name, md in self._last_market_data.items():
                mid_price = float(md.get("mid_price", 0.0) or 0.0)
                if mid_price <= 0.0:
                    continue
                base_asset, quote_asset = _split_pair(pair_name)
                base_key = base_asset.upper()
                quote_key = quote_asset.upper()

                # The engine emits `mid_price` as `display_price * 1e12`
                # for every pair regardless of which side is XCH (see
                # cpp/src/execution/market_data.cpp publish_snapshot —
                # to_mojos(price) := round(price * kMojosPerXch)).  The
                # "display price" is quote-units per base-unit, so
                # dividing by 1e12 always yields the human-readable
                # ratio.  Using the quote asset's mojo divisor here (as
                # earlier code did) was correct for raw on-chain
                # `quote_mojos/base_mojo` prices but is wrong for the
                # engine's already-scaled metric and inflates non-XCH
                # asset prices by ~1e9.
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

    def _asset_id_symbol_map(self) -> dict[str, str]:
        """Build a mapping of CAT asset_id (hex) -> base symbol from pairs.

        Uses ``base_asset_id`` / ``quote_asset_id`` paired with the
        ``name`` field (``BASE/QUOTE``) of every configured pair so we
        can resolve a wallet's symbol from its on-chain asset id, even
        when the user has left the wallet's display name as Chia's
        default ``CAT abcd...`` placeholder.
        """
        mapping: dict[str, str] = {}
        for pair_cfg in self._last_pairs_cfg:
            pair_name = str(pair_cfg.get("name", ""))
            if not pair_name:
                continue
            base_sym, quote_sym = _split_pair(pair_name)
            base_id = str(pair_cfg.get("base_asset_id", "") or "").lower()
            quote_id = str(pair_cfg.get("quote_asset_id", "") or "").lower()
            if base_id and base_id != "xch" and base_sym:
                mapping[base_id] = base_sym.upper()
            if quote_id and quote_id != "xch" and quote_sym:
                mapping[quote_id] = quote_sym.upper()
        return mapping

    def _wallet_asset_symbol(
        self,
        wallet_name: str,
        wallet_data: dict[str, float],
        asset_prices: dict[str, float],
        asset_id_map: dict[str, str] | None = None,
    ) -> Optional[str]:
        if int(wallet_data.get("wallet_type", -1)) == 0:
            return "XCH"

        # Prefer on-chain asset_id lookup so the user's wallet display
        # name (e.g. Chia's default "CAT abcd..." placeholder) doesn't
        # matter.  Falls through to name-based matching if the wallet
        # has no asset_id or the id isn't in any configured pair.
        if asset_id_map:
            wallet_asset_id = str(wallet_data.get("asset_id", "") or "").lower()
            if wallet_asset_id and wallet_asset_id in asset_id_map:
                return asset_id_map[wallet_asset_id]

        normalized = wallet_name.strip().upper()
        compact = normalized.replace(" ", "")

        # Each stablecoin is its own asset — never merge wUSDC.b with
        # wUSDC, USDS, etc.  Return the wallet's own normalized symbol
        # so multi-stablecoin wallets stay in separate allocation rows.
        if _is_stablecoin_symbol(normalized):
            return normalized
        if _is_stablecoin_symbol(compact):
            return compact

        if normalized in asset_prices:
            return normalized
        if compact in asset_prices:
            return compact
        for candidate in asset_prices:
            # Skip stablecoin candidates in the substring fallback to
            # avoid pulling unrelated USDC-named wallets into one bucket.
            if _is_stablecoin_symbol(candidate):
                continue
            if candidate in compact or compact in candidate:
                return candidate
        return None

    def _enabled_pair_assets(self) -> set[str]:
        """Upper-cased symbols participating in at least one ENABLED pair.

        Empty when pairs config hasn't arrived yet -- callers treat that
        as "unknown", not as "nothing is enabled".
        """
        assets: set[str] = set()
        for pair_cfg in self._last_pairs_cfg:
            if not pair_cfg.get("enabled", True):
                continue
            pair_name = str(pair_cfg.get("name", ""))
            if not pair_name:
                continue
            base_asset, quote_asset = _split_pair(pair_name)
            if base_asset:
                assets.add(base_asset.upper())
            if quote_asset:
                assets.add(quote_asset.upper())
        return assets

    def _enabled_pair_names(self) -> set[str]:
        """Names of pairs currently enabled in config.  Empty = unknown."""
        return {
            str(p.get("name", "")) for p in self._last_pairs_cfg
            if p.get("enabled", True) and p.get("name")
        }

    def _refresh_allocation_table(self) -> None:
        asset_prices = self._asset_prices_usdc()
        current_values: dict[str, float] = {}
        # One-shot diagnostic so we can see in the GUI log why the
        # Target Allocation panel chose the symbols it did.  Logged at
        # INFO only when the resolved set materially changes between
        # refreshes (avoids per-tick spam).
        _diag_snapshot: dict[str, str] = {}
        # Track every recognized-asset wallet so we can still list assets
        # that have no priced market data yet (e.g. XCH right after engine
        # restart, before XCH/wUSDC.b has published a mid_price).  Without
        # this, XCH would silently vanish from the Target Allocation table
        # whenever its price graph hadn't resolved.
        all_assets: set[str] = set()

        # Assets that participate in at least one enabled trading pair.
        # Assets only present in disabled pairs (or in wallets not traded
        # at all) are hidden from the allocation table so the user only
        # manages targets for assets the bot is actually trading.  When
        # pairs config hasn't arrived yet (empty set), fall through to the
        # legacy "show everything" behaviour so startup isn't blank.
        enabled_assets = self._enabled_pair_assets()

        asset_id_map = self._asset_id_symbol_map()

        for wallet_name, wallet_data in self._last_balances.items():
            symbol = self._wallet_asset_symbol(
                wallet_name, wallet_data, asset_prices, asset_id_map
            )
            _diag_snapshot[wallet_name] = (
                f"type={int(wallet_data.get('wallet_type', -1))} "
                f"aid={str(wallet_data.get('asset_id','') or '')[:10]} "
                f"sym={symbol} "
                f"price={asset_prices.get((symbol or '').upper(), 0.0)}"
            )
            if not symbol:
                _log.debug(
                    "Allocation: skipping unmatched wallet %r "
                    "(type=%s, asset_id=%r)",
                    wallet_name,
                    wallet_data.get("wallet_type"),
                    wallet_data.get("asset_id", ""),
                )
                continue
            asset_key = symbol.upper()
            if enabled_assets and asset_key not in enabled_assets:
                _log.debug(
                    "Allocation: wallet %r resolved to %s but not in "
                    "enabled pairs %s",
                    wallet_name, asset_key, sorted(enabled_assets),
                )
                continue
            all_assets.add(asset_key)
            price = asset_prices.get(asset_key, asset_prices.get(symbol, 0.0))
            if price <= 0.0:
                _log.debug(
                    "Allocation: wallet %r (%s) has no price yet "
                    "(known prices: %s)",
                    wallet_name, asset_key, sorted(asset_prices),
                )
                continue
            confirmed = float(wallet_data.get("confirmed", 0.0) or 0.0)
            current_values[asset_key] = (
                current_values.get(asset_key, 0.0) + confirmed * price
            )

        # Always surface every enabled-pair asset, even if the user has
        # no wallet for it yet.  This lets them pre-set a target % for
        # an asset they intend to start holding (or for one whose wallet
        # hasn't been auto-created yet).  Rows with no balance show 0%
        # current and "—" delta until a wallet appears.
        for asset_key in enabled_assets:
            all_assets.add(asset_key)

        # One-shot diagnostic emission.  Logged at INFO only when the
        # resolved snapshot changes so we get a single visible line per
        # state change, instead of one per refresh tick.
        diag_key = (
            tuple(sorted(_diag_snapshot.items())),
            tuple(sorted(enabled_assets)),
            tuple(sorted(asset_id_map.items())),
            tuple(sorted((k, round(v, 4)) for k, v in current_values.items())),
        )
        if diag_key != getattr(self, "_alloc_diag_last", None):
            self._alloc_diag_last = diag_key
            _log.info(
                "Allocation diag | enabled=%s | asset_id_map=%s | "
                "wallets=%s | current_values=%s",
                sorted(enabled_assets),
                asset_id_map,
                _diag_snapshot,
                {k: round(v, 4) for k, v in current_values.items()},
            )

        if not all_assets:
            self._alloc_table.setRowCount(0)
            _fit_table_to_contents(self._alloc_table)
            self._alloc_sum_label.setText("Target sum: —")
            self._alloc_hint_label.setText("No wallets detected yet")
            return

        total_value = sum(current_values.values())

        self._alloc_updating = True
        assets = sorted(all_assets)
        self._alloc_table.setRowCount(len(assets))

        for row, asset in enumerate(assets):
            priced = asset in current_values and total_value > 0.0
            if priced:
                current_pct = (current_values[asset] / total_value) * 100.0
                current_text = f"{current_pct:.2f}%"
            else:
                current_pct = 0.0
                current_text = "—"

            if asset not in self._target_allocations:
                # Seed target from current % when priced, otherwise 0 so
                # the user can dial it in manually.
                self._target_allocations[asset] = current_pct if priced else 0.0
            target_pct = self._target_allocations.get(asset, 0.0)
            tolerance_pct = self._target_tolerances.get(asset, 0.0)

            if priced:
                delta_pct = target_pct - current_pct
                # Mark the delta as "within tolerance" when |delta| <= tolerance.
                if tolerance_pct > 0.0 and abs(delta_pct) <= tolerance_pct:
                    delta_text = f"{delta_pct:+.2f}% (in band)"
                else:
                    delta_text = f"{delta_pct:+.2f}%"
            else:
                delta_pct = 0.0
                delta_text = "—"

            suggested_text, suggested_tooltip, suggested_color = \
                self._suggested_cell_for(asset)
            suggested_item = QTableWidgetItem(suggested_text)
            suggested_item.setToolTip(suggested_tooltip)
            suggested_item.setForeground(QColor(suggested_color))

            cols = [
                QTableWidgetItem(asset),
                QTableWidgetItem(current_text),
                QTableWidgetItem(f"{target_pct:.2f}"),
                suggested_item,
                QTableWidgetItem(f"{tolerance_pct:.2f}"),
                QTableWidgetItem(delta_text),
            ]

            cols[0].setFlags(cols[0].flags() & ~Qt.ItemFlag.ItemIsEditable)
            cols[1].setFlags(cols[1].flags() & ~Qt.ItemFlag.ItemIsEditable)
            cols[3].setFlags(cols[3].flags() & ~Qt.ItemFlag.ItemIsEditable)
            cols[5].setFlags(cols[5].flags() & ~Qt.ItemFlag.ItemIsEditable)

            cols[0].setTextAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
            for idx in (1, 2, 3, 4, 5):
                cols[idx].setTextAlignment(
                    Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
                )

            if not priced:
                cols[5].setForeground(QColor(TEXT_SECONDARY))
            elif tolerance_pct > 0.0 and abs(delta_pct) <= tolerance_pct:
                cols[5].setForeground(QColor(PROFIT_GREEN))
            elif abs(delta_pct) > 5.0:
                cols[5].setForeground(QColor(WARNING))
            else:
                cols[5].setForeground(QColor(PROFIT_GREEN))

            for col, item in enumerate(cols):
                self._alloc_table.setItem(row, col, item)

        self._alloc_updating = False
        _fit_table_to_contents(self._alloc_table)
        self._update_allocation_sum_status()
        unpriced = sorted(all_assets - set(current_values))
        if total_value <= 0.0:
            self._alloc_hint_label.setText(
                "Waiting for market data — set targets now; current % will "
                "fill in once mid prices arrive"
            )
        elif unpriced:
            # PARTIAL pricing is the dangerous case: percentages are computed
            # over the priced subset only, so a single priced asset reads as
            # a confident "100%" while real holdings are missing entirely.
            # Observed in the field -- wUSDC.b (hard $1 peg) showed 100% while
            # XCH/BYC/DBX were unpriced because the engine could not reach the
            # wallet RPC. Say so instead of presenting a plausible lie.
            self._alloc_hint_label.setText(
                "⚠ Current % covers only priced assets — "
                f"no price yet for {', '.join(unpriced)}. "
                "Percentages are NOT your true allocation until every asset "
                "is priced (check the engine's wallet/market connection)."
            )

        # Surface the diagnostic snapshot directly in the panel so it's
        # visible without the GUI log file (pythonw.exe drops stderr).
        # Tooltip on the hint label shows the full per-wallet resolution
        # for easy copy/paste when debugging Target Allocation issues.
        diag_lines = [
            f"enabled_pairs={sorted(enabled_assets)}",
            f"asset_id_map={ {k[:8]+'..': v for k, v in asset_id_map.items()} }",
            "wallets:",
        ]
        for wname, info in _diag_snapshot.items():
            diag_lines.append(f"  {wname!r}: {info}")
        diag_lines.append(
            f"current_values={ {k: round(v, 2) for k, v in current_values.items()} }"
        )
        self._alloc_hint_label.setToolTip("\n".join(diag_lines))

    # ------------------------------------------------------------------
    # Suggested allocation (advisory, async)
    # ------------------------------------------------------------------

    def showEvent(self, event: Any) -> None:  # noqa: N802 -- Qt override
        """Re-compute the advisory suggested allocation whenever the page
        becomes visible, per the column's "re-computed on open" contract."""
        super().showEvent(event)
        self._refresh_suggested_allocation()

    def _refresh_suggested_allocation(self) -> None:
        """Kick off the async suggested-allocation computation.

        The dexie fetch and the read-only DB queries run on a dedicated
        QThread (database_service worker pattern); results arrive via
        queued signals, so the UI thread never blocks.  A run already in
        flight is left alone.
        """
        if self._suggest_thread is not None:
            return
        self._suggested_status = "pending"
        thread = QThread(self)
        worker = _SuggestedAllocationWorker(
            self._sizing_config_path, self._sizing_db_path
        )
        worker.moveToThread(thread)
        thread.started.connect(worker.run)
        worker.ready.connect(self._on_suggested_ready)
        worker.failed.connect(self._on_suggested_failed)
        worker.ready.connect(thread.quit)
        worker.failed.connect(thread.quit)
        thread.finished.connect(worker.deleteLater)
        thread.finished.connect(self._on_suggest_thread_finished)
        thread.finished.connect(thread.deleteLater)
        self._suggest_thread = thread
        self._suggest_worker = worker
        thread.start()

    def stop_background_work(self, timeout_ms: int = 2000) -> None:
        """Join the advisory worker thread before this widget is destroyed.

        A QThread still running when its C++ object is destroyed makes Qt
        call qFatal("QThread: Destroyed while thread is still running") and
        the process ABORTS -- reproduced against PySide6 6.11: the run exits
        via the MSVC abort path instead of returning from main().

        The worker blocks on a dexie fetch with a 30s timeout, so quit()
        alone cannot return promptly: it only asks the thread's event loop to
        exit, while run() is mid-request.  Wait briefly, then terminate as a
        last resort -- an abrupt stop of a READ-ONLY advisory query during
        shutdown is strictly better than aborting the application.

        Child widgets do not receive closeEvent when the top-level window
        closes, which is why this is public and called by MainWindow.
        """
        thread = self._suggest_thread
        if thread is None:
            return
        thread.quit()
        if not thread.wait(timeout_ms):
            thread.terminate()
            thread.wait(1000)
        self._suggest_thread = None
        self._suggest_worker = None

    def set_sizing_paths(self, config_path: Optional[str],
                         db_path: Optional[str]) -> None:
        """Tell the advisory calculator where config and the database live.

        Called by main_window once the bridge has resolved both.  A refresh
        already in flight keeps its own paths; the next one picks these up.
        """
        self._sizing_config_path = config_path
        self._sizing_db_path = db_path

    @Slot()
    def _on_suggest_thread_finished(self) -> None:
        self._suggest_thread = None
        self._suggest_worker = None

    @Slot(dict)
    def _on_suggested_ready(self, payload: dict) -> None:
        self._suggested_alloc = payload
        self._suggested_status = "ready"
        note = str(payload.get("marginal_note", ""))
        self._alloc_suggest_note.setText(
            "Suggested % = flow-keyed best split of EXISTING capital "
            f"(advisory, {payload.get('computed_at', '?')}).  {note}"
        )
        self._alloc_suggest_note.setToolTip(self._suggested_tooltip())
        self._alloc_suggest_note.setVisible(True)
        self._refresh_allocation_table()

    @Slot(str)
    def _on_suggested_failed(self, message: str) -> None:
        _log.warning("Suggested allocation unavailable: %s", message)
        self._suggested_alloc = None
        self._suggested_status = f"failed: {message}"
        self._alloc_suggest_note.setText(
            f"Suggested % unavailable: {message}"
        )
        self._alloc_suggest_note.setVisible(True)
        self._refresh_allocation_table()

    def _suggested_tooltip(self) -> str:
        """Full advisory tooltip: method, corrections, marginal note."""
        lines = [_SUGGESTED_ALLOC_TOOLTIP]
        if self._suggested_alloc:
            corrections = self._suggested_alloc.get("corrections") or []
            if corrections:
                lines.append("")
                lines.append("Corrections applied:")
                lines.extend(f"  - {c}" for c in corrections)
            note = self._suggested_alloc.get("marginal_note")
            if note:
                lines.append("")
                lines.append(str(note))
        elif self._suggested_status.startswith("failed"):
            lines.append("")
            lines.append(self._suggested_status)
        return "\n".join(lines)

    def _suggested_cell_for(self, asset: str) -> tuple[str, str, str]:
        """(text, tooltip, colour) for the Suggested % cell of *asset*."""
        tooltip = self._suggested_tooltip()
        if self._suggested_status == "pending":
            return "…", tooltip + "\n\nComputing…", TEXT_SECONDARY
        if self._suggested_alloc is None:
            return "n/a", tooltip, TEXT_SECONDARY
        info = (self._suggested_alloc.get("assets") or {}).get(
            asset.strip().upper()
        )
        if info is None:
            return (
                "—",
                tooltip + "\n\nNot part of the flow-keyed derivation "
                "(no enabled pair trades this asset).",
                TEXT_SECONDARY,
            )
        return (
            f"{float(info.get('suggested_pct', 0.0)):.2f}",
            tooltip,
            TEXT_PRIMARY,
        )

    @Slot(int, int)
    def _on_alloc_cell_changed(self, row: int, col: int) -> None:
        # Editable columns: 2 = Target %, 4 = Target % +/- (tolerance).
        # Column 3 (Suggested %) is read-only advisory.
        if self._alloc_updating or col not in (2, 4):
            return
        asset_item = self._alloc_table.item(row, 0)
        edited_item = self._alloc_table.item(row, col)
        if asset_item is None or edited_item is None:
            return

        asset = asset_item.text().strip().upper()
        raw_text = edited_item.text().replace("%", "").strip()
        if col == 2:
            try:
                value = float(raw_text)
            except ValueError:
                value = self._target_allocations.get(asset, 0.0)
            value = max(0.0, min(100.0, value))
            self._target_allocations[asset] = value
            self._save_target_allocations()
        else:  # col == 4 -- tolerance
            try:
                value = float(raw_text)
            except ValueError:
                value = self._target_tolerances.get(asset, 0.0)
            # Tolerance is a percentage-point half-window; clamp to [0, 50].
            value = max(0.0, min(50.0, value))
            self._target_tolerances[asset] = value
            self._save_target_tolerances()
        self._refresh_allocation_table()

    def _update_allocation_sum_status(self) -> None:
        # [ALLOCZERO] Validate the sum Apply would actually WRITE, not the
        # saved dict: hidden (disabled-pair) percents are omitted at apply
        # time, and a green "balanced" over numbers that will not be
        # applied is a lie in both directions.  The hidden remainder is
        # disclosed on this label because it survives the ~5s refresh that
        # overwrites the hint label.
        hidden_total = 0.0
        if self._last_pairs_cfg:
            enabled_assets = self._enabled_pair_assets()
            target_sum = sum(
                v for k, v in self._target_allocations.items()
                if k in enabled_assets)
            hidden_total = sum(
                v for k, v in self._target_allocations.items()
                if k not in enabled_assets and v > 0.0)
        else:
            target_sum = sum(self._target_allocations.values())
        hidden_note = (
            f"  (+{hidden_total:.2f}% saved on disabled pairs, not applied)"
            if hidden_total > 0.0 else "")
        self._alloc_sum_label.setText(
            f"Target sum: {target_sum:.2f}%{hidden_note}")
        if abs(target_sum - 100.0) <= 0.5:
            self._alloc_sum_label.setStyleSheet(
                f"color: {PROFIT_GREEN}; font-size: 11px; font-weight: bold;"
            )
            self._alloc_hint_label.setText("Allocation is balanced")
            self._alloc_hint_label.setStyleSheet(
                f"color: {TEXT_SECONDARY}; font-size: 11px;"
            )
        else:
            self._alloc_sum_label.setStyleSheet(
                f"color: {WARNING}; font-size: 11px; font-weight: bold;"
            )
            diff = 100.0 - target_sum
            if diff > 0:
                self._alloc_hint_label.setText(f"Add {diff:.2f}% to reach 100%")
            else:
                self._alloc_hint_label.setText(f"Reduce {-diff:.2f}% to reach 100%")
            self._alloc_hint_label.setStyleSheet(
                f"color: {WARNING}; font-size: 11px;"
            )

    @Slot()
    def _normalize_targets(self) -> None:
        target_sum = sum(self._target_allocations.values())
        if target_sum <= 0.0:
            return
        scale = 100.0 / target_sum
        for asset in list(self._target_allocations.keys()):
            self._target_allocations[asset] = max(
                0.0, min(100.0, self._target_allocations[asset] * scale)
            )
        self._refresh_allocation_table()

    def _pair_ratio_targets_from_allocations(self) -> dict[str, float]:
        pair_targets: dict[str, float] = {}
        # [ALLOCZERO] A disabled pair gets no ratio target: the engine
        # ignores it anyway, and writing one into config.yaml would present
        # a stale number as live intent.
        enabled_pairs = self._enabled_pair_names()
        for pair_name in self._last_market_data:
            if self._last_pairs_cfg and pair_name not in enabled_pairs:
                continue
            base_asset, quote_asset = _split_pair(pair_name)
            base_key = base_asset.upper()
            quote_key = quote_asset.upper()
            base_pct = self._target_allocations.get(base_key)
            quote_pct = self._target_allocations.get(quote_key)
            if base_pct is None or quote_pct is None:
                continue
            denom = base_pct + quote_pct
            if denom <= 0.0:
                continue
            pair_targets[pair_name] = base_pct / denom
        return pair_targets

    def _pair_band_enter_from_tolerances(self) -> dict[str, float]:
        """Map asset-level "Target % +/-" inputs to a per-pair ratio band.

        For each pair, project the asset tolerances into ratio space by
        evaluating the pair's target ratio at (base+tol)/(quote-tol) and
        (base-tol)/(quote+tol), then taking the widest deviation from
        the nominal target.  The resulting fraction is clamped to (0, 0.49)
        so the C++ side accepts it as a valid enter band.
        """
        bands: dict[str, float] = {}
        enabled_pairs = self._enabled_pair_names()
        for pair_name in self._last_market_data:
            if self._last_pairs_cfg and pair_name not in enabled_pairs:
                continue
            base_asset, quote_asset = _split_pair(pair_name)
            base_key = base_asset.upper()
            quote_key = quote_asset.upper()
            base_pct = self._target_allocations.get(base_key)
            quote_pct = self._target_allocations.get(quote_key)
            if base_pct is None or quote_pct is None:
                continue
            denom = base_pct + quote_pct
            if denom <= 0.0:
                continue
            base_tol = self._target_tolerances.get(base_key, 0.0)
            quote_tol = self._target_tolerances.get(quote_key, 0.0)
            if base_tol <= 0.0 and quote_tol <= 0.0:
                continue
            target_ratio = base_pct / denom

            def _ratio(b: float, q: float) -> float:
                b = max(0.0, b)
                q = max(0.0, q)
                d = b + q
                return (b / d) if d > 0.0 else target_ratio

            upper_r = _ratio(base_pct + base_tol, quote_pct - quote_tol)
            lower_r = _ratio(base_pct - base_tol, quote_pct + quote_tol)
            band = max(abs(upper_r - target_ratio),
                       abs(target_ratio - lower_r))
            band = max(0.0, min(0.49, band))
            if band > 0.0:
                bands[pair_name] = band
        return bands

    @Slot()
    def _apply_targets(self) -> None:
        if not self._target_allocations:
            self._alloc_hint_label.setText("No target allocations to apply")
            self._alloc_hint_label.setStyleSheet(
                f"color: {WARNING}; font-size: 11px;"
            )
            return
        asset_targets = {
            key: float(value) / 100.0
            for key, value in self._target_allocations.items()
        }
        # [ALLOCZERO 2026-08-29] An asset whose every pair is disabled is
        # hidden from the table, but its SAVED percent used to ride along
        # into config.yaml -- the engine kept steering the portfolio toward
        # a target the bot could not trade.  OMIT it while disabled: absence
        # is the engine's neutral semantic (no acquisition taper, no drift
        # classification), whereas an explicit 0.0 would make a later
        # re-enable + restart actively SELL the asset toward 0% until the
        # operator remembered a second Apply.  The saved percent itself is
        # untouched (QSettings keeps it), so re-enabling the pair brings the
        # old target back on the next Apply.  An empty _last_pairs_cfg means
        # pairs config hasn't arrived (unknown -- omit nothing); a KNOWN
        # config whose every pair is disabled omits everything.
        omitted: list[str] = []
        if self._last_pairs_cfg:
            enabled_assets = self._enabled_pair_assets()
            for key in [k for k, v in asset_targets.items()
                        if k not in enabled_assets and v > 0.0]:
                del asset_targets[key]
                omitted.append(key)
        pair_targets = self._pair_ratio_targets_from_allocations()
        asset_tolerances = {
            key: float(value) / 100.0
            for key, value in self._target_tolerances.items()
            if float(value) > 0.0
        }
        pair_band_enters = self._pair_band_enter_from_tolerances()
        self.allocation_targets_applied.emit(
            asset_targets, pair_targets, asset_tolerances, pair_band_enters
        )
        omitted_note = (
            "; omitted " + ", ".join(sorted(omitted)) + " (pairs disabled)"
            if omitted else ""
        )
        self._alloc_hint_label.setText(
            f"Applied {len(asset_targets)} asset target(s), "
            f"{len(pair_targets)} pair ratio target(s), "
            f"{len(pair_band_enters)} pair band override(s)" + omitted_note
        )
        self._alloc_hint_label.setStyleSheet(
            f"color: {INFO}; font-size: 11px;"
        )

    @staticmethod
    def _fmt(value: float) -> str:
        """Format a balance value for display."""
        if abs(value) < 0.000001:
            return "0"
        return f"{value:,.6f}"

    # ------------------------------------------------------------------
    # Persistence helpers
    # ------------------------------------------------------------------

    def _load_target_allocations(self) -> None:
        """Restore previously persisted target allocations from QSettings."""
        try:
            settings = QSettings(_QSETTINGS_ORG, _QSETTINGS_APP)
            raw = settings.value(_ALLOC_TARGETS_KEY, "", type=str)
            if not raw:
                return
            data = json.loads(raw)
            if not isinstance(data, dict):
                return
            restored: dict[str, float] = {}
            for key, value in data.items():
                try:
                    restored[str(key).upper()] = max(
                        0.0, min(100.0, float(value))
                    )
                except (TypeError, ValueError):
                    continue
            self._target_allocations.update(restored)
        except (ValueError, OSError) as exc:
            _log.warning("Failed to load allocation targets: %s", exc)

    def _save_target_allocations(self) -> None:
        """Persist target allocations to QSettings as a JSON blob."""
        try:
            settings = QSettings(_QSETTINGS_ORG, _QSETTINGS_APP)
            payload = json.dumps(
                {k: float(v) for k, v in self._target_allocations.items()},
                sort_keys=True,
            )
            settings.setValue(_ALLOC_TARGETS_KEY, payload)
        except (TypeError, ValueError, OSError) as exc:
            _log.warning("Failed to save allocation targets: %s", exc)

    def _load_target_tolerances(self) -> None:
        """Restore previously persisted target tolerances from QSettings."""
        try:
            settings = QSettings(_QSETTINGS_ORG, _QSETTINGS_APP)
            raw = settings.value(_ALLOC_TOLERANCES_KEY, "", type=str)
            if not raw:
                return
            data = json.loads(raw)
            if not isinstance(data, dict):
                return
            restored: dict[str, float] = {}
            for key, value in data.items():
                try:
                    restored[str(key).upper()] = max(
                        0.0, min(50.0, float(value))
                    )
                except (TypeError, ValueError):
                    continue
            self._target_tolerances.update(restored)
        except (ValueError, OSError) as exc:
            _log.warning("Failed to load allocation tolerances: %s", exc)

    def _save_target_tolerances(self) -> None:
        """Persist target tolerances to QSettings as a JSON blob."""
        try:
            settings = QSettings(_QSETTINGS_ORG, _QSETTINGS_APP)
            payload = json.dumps(
                {k: float(v) for k, v in self._target_tolerances.items()},
                sort_keys=True,
            )
            settings.setValue(_ALLOC_TOLERANCES_KEY, payload)
        except (TypeError, ValueError, OSError) as exc:
            _log.warning("Failed to save allocation tolerances: %s", exc)

    def clear(self) -> None:
        """Reset the widget to its initial empty state."""
        self._table.setRowCount(0)
        self._alloc_table.setRowCount(0)
        _fit_table_to_contents(self._table)
        _fit_table_to_contents(self._alloc_table)
        self._target_allocations.clear()
        self._target_tolerances.clear()
        self._last_balances = {}
        self._last_market_data = {}
        self._offered_units = {}
        self._offer_counts = {}
        self._pending_offers = 0
        self._row_wallets = []
        self._deployed_label.setText("Deployed: —")
        self._deployed_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 12px; "
            f"font-family: {MONO_FONT_FAMILY};"
        )
        self._alloc_sum_label.setText("Target sum: —")
        self._alloc_hint_label.setText("")
        self._total_spendable_label.setText("—")
        self._total_confirmed_label.setText("—")
        self._total_pending_label.setText("—")
        self._wallet_count_label.setText("0")
        self._stuck_label.setVisible(False)
        self._status_label.setText("Waiting for wallet data…")
