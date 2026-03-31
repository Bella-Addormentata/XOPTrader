"""Active-offers management panel for XOPTrader.

Displays all outstanding market-making offers in a filterable,
sortable table with per-row cancel actions, right-click context
menus, and a live summary bar showing fill rate and locked value.

All monetary values are stored and transmitted as **mojos** (int64)
and formatted for display via :func:`mojos_to_xch`.
"""

from __future__ import annotations

from typing import Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QAction, QColor, QFont
from PySide6.QtWidgets import (
    QComboBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMenu,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS
from gui.utils import mojos_to_xch

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Human-readable tier names indexed by tier number (0-3).
TIER_NAMES: dict[int, str] = {
    0: "Tight",
    1: "Near",
    2: "Mid",
    3: "Wide",
}

# Column definitions for the order table (label, default width).
_COLUMNS: list[tuple[str, int]] = [
    ("Offer ID",      130),
    ("Pair",           90),
    ("Side",           60),
    ("Price",         140),
    ("Size",          140),
    ("Tier",           80),
    ("Status",         80),
    ("Created Block", 100),
    ("Age (blocks)",   90),
    ("Actions",        80),
]

# Offer status values used throughout the system.
_STATUSES: list[str] = ["All", "Pending", "Filled", "Cancelled", "Expired"]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _status_color(status: str) -> QColor:
    """Return the badge colour for a given offer status string.

    Colour mapping:
        pending   -> WARNING_YELLOW
        filled    -> PROFIT_GREEN
        cancelled -> TEXT_SECONDARY (gray)
        expired   -> LOSS_RED
    """
    key = status.lower()
    if key == "filled":
        return QColor(COLORS.PROFIT_GREEN)
    if key == "pending":
        return QColor(COLORS.WARNING_YELLOW)
    if key == "cancelled":
        return QColor(COLORS.TEXT_SECONDARY)
    if key == "expired":
        return QColor(COLORS.LOSS_RED)
    return QColor(COLORS.TEXT_PRIMARY)


def _side_color(side: str) -> QColor:
    """Return green for BID, red for ASK."""
    if side.lower() == "bid":
        return QColor(COLORS.PROFIT_GREEN)
    return QColor(COLORS.LOSS_RED)


# ---------------------------------------------------------------------------
# OrderPanel widget
# ---------------------------------------------------------------------------

class OrderPanel(QWidget):
    """Active-offers management widget.

    Signals
    -------
    cancel_offer_requested(str):
        Emitted when the user asks to cancel a single offer.  Payload
        is the offer_id string.
    cancel_all_requested():
        Emitted when the user confirms the *Cancel All* action.
    """

    cancel_offer_requested = Signal(str)
    cancel_all_requested = Signal()

    # Default offer TTL for age-warning colouring (blocks).
    _DEFAULT_TTL: int = 60

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)

        # Internal bookkeeping.
        self._current_block: int = 0
        self._offer_ttl: int = self._DEFAULT_TTL
        self._all_offers: list[dict] = []

        self._build_ui()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        """Construct the complete widget layout."""
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(8)

        # -- Top filter bar ------------------------------------------------
        root.addLayout(self._build_filter_bar())

        # -- Main table ----------------------------------------------------
        self._table = self._build_table()
        root.addWidget(self._table, stretch=1)

        # -- Bottom summary bar --------------------------------------------
        root.addLayout(self._build_summary_bar())

    def _build_filter_bar(self) -> QHBoxLayout:
        """Create the top filter / action bar.

        Contains combo-boxes for pair, side, and status filtering,
        a search text field, a refresh button, and a *Cancel All*
        danger button.
        """
        bar = QHBoxLayout()
        bar.setSpacing(10)

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

        # Status filter
        lbl_status = QLabel("Status:")
        self._combo_status = QComboBox()
        self._combo_status.addItems(_STATUSES)
        self._combo_status.currentIndexChanged.connect(self._apply_filters)
        bar.addWidget(lbl_status)
        bar.addWidget(self._combo_status)

        # Search box
        self._search = QLineEdit()
        self._search.setPlaceholderText("\U0001F50D Search offers...")
        self._search.setMinimumWidth(200)
        self._search.textChanged.connect(self._apply_filters)
        bar.addWidget(self._search)

        bar.addStretch()

        # Refresh button
        btn_refresh = QPushButton("Refresh")
        btn_refresh.clicked.connect(lambda: self._apply_filters())
        bar.addWidget(btn_refresh)

        # Cancel All button (danger variant)
        btn_cancel_all = QPushButton("Cancel All")
        btn_cancel_all.setObjectName("dangerButton")
        btn_cancel_all.clicked.connect(self._on_cancel_all)
        bar.addWidget(btn_cancel_all)

        return bar

    def _build_table(self) -> QTableWidget:
        """Create and configure the main order table."""
        table = QTableWidget(0, len(_COLUMNS))
        table.setHorizontalHeaderLabels([c[0] for c in _COLUMNS])

        # Appearance
        table.setAlternatingRowColors(True)
        table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
        table.verticalHeader().setVisible(False)
        table.setShowGrid(True)
        table.setSortingEnabled(True)

        # Column sizing
        header = table.horizontalHeader()
        for idx, (_, width) in enumerate(_COLUMNS):
            header.resizeSection(idx, width)
        # Stretch the Offer ID column to fill remaining space.
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)

        # Context menu
        table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        table.customContextMenuRequested.connect(self._show_context_menu)

        return table

    def _build_summary_bar(self) -> QHBoxLayout:
        """Create the bottom summary bar with aggregate statistics."""
        bar = QHBoxLayout()
        bar.setSpacing(24)

        self._lbl_total = QLabel("Total: 0")
        self._lbl_pending = QLabel("Pending: 0")
        self._lbl_filled = QLabel("Filled: 0")
        self._lbl_fill_rate = QLabel("Fill rate: 0.0%")
        self._lbl_locked = QLabel("Locked: 0.0000 XCH")

        for lbl in (
            self._lbl_total,
            self._lbl_pending,
            self._lbl_filled,
            self._lbl_fill_rate,
            self._lbl_locked,
        ):
            lbl.setStyleSheet(f"color: {COLORS.TEXT_SECONDARY}; font-size: 9pt;")
            bar.addWidget(lbl)

        bar.addStretch()
        return bar

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def update_offers(self, offers: list[dict]) -> None:
        """Refresh the table with a new list of offer dicts.

        Expected dict keys:
            offer_id, pair_name, side, price_mojos, size_mojos,
            tier, status, created_block, resolved_block

        Parameters
        ----------
        offers:
            Complete list of offers (active + historical) from the DB.
        """
        self._all_offers = list(offers)
        self._rebuild_pair_combo()
        self._apply_filters()

    def set_current_block(self, block: int) -> None:
        """Update the current block height used for age calculations.

        Parameters
        ----------
        block:
            Latest confirmed block height on the Chia blockchain.
        """
        self._current_block = block

    def set_offer_ttl(self, ttl_blocks: int) -> None:
        """Set the offer TTL threshold for age-warning colouring.

        Parameters
        ----------
        ttl_blocks:
            Number of blocks after which an offer is considered stale.
        """
        self._offer_ttl = ttl_blocks

    # ------------------------------------------------------------------
    # Internal: filtering
    # ------------------------------------------------------------------

    def _rebuild_pair_combo(self) -> None:
        """Repopulate the pair filter combo with unique pair names."""
        # Preserve current selection when possible.
        current_text = self._combo_pair.currentText()
        self._combo_pair.blockSignals(True)
        self._combo_pair.clear()
        self._combo_pair.addItem("All Pairs")
        # Collect unique pair names preserving insertion order.
        pairs_seen: set[str] = set()
        for offer in self._all_offers:
            pname = offer.get("pair_name", "")
            if pname and pname not in pairs_seen:
                pairs_seen.add(pname)
                self._combo_pair.addItem(pname)
        # Restore previous selection if still valid.
        idx = self._combo_pair.findText(current_text)
        self._combo_pair.setCurrentIndex(max(idx, 0))
        self._combo_pair.blockSignals(False)

    def _apply_filters(self) -> None:
        """Filter ``_all_offers`` according to the current UI state
        and repopulate the table rows.
        """
        pair_filter = self._combo_pair.currentText()
        side_filter = self._combo_side.currentText().lower()
        status_filter = self._combo_status.currentText().lower()
        search_text = self._search.text().lower().strip()

        filtered: list[dict] = []
        for offer in self._all_offers:
            # Pair filter
            if pair_filter != "All Pairs" and offer.get("pair_name") != pair_filter:
                continue
            # Side filter
            if side_filter != "all" and offer.get("side", "").lower() != side_filter:
                continue
            # Status filter
            if status_filter != "all" and offer.get("status", "").lower() != status_filter:
                continue
            # Free-text search (matches against offer_id and pair_name)
            if search_text:
                searchable = (
                    offer.get("offer_id", "")
                    + offer.get("pair_name", "")
                ).lower()
                if search_text not in searchable:
                    continue
            filtered.append(offer)

        self._populate_table(filtered)
        self._update_summary()

    # ------------------------------------------------------------------
    # Internal: table population
    # ------------------------------------------------------------------

    def _populate_table(self, offers: list[dict]) -> None:
        """Write *offers* into the QTableWidget rows.

        Temporarily disables sorting to avoid index corruption
        while rows are being inserted.
        """
        self._table.setSortingEnabled(False)
        self._table.setRowCount(0)  # Clear existing rows.

        mono_font = QFont("JetBrains Mono", 10)
        mono_font.setStyleHint(QFont.StyleHint.Monospace)

        for row_idx, offer in enumerate(offers):
            self._table.insertRow(row_idx)

            # -- Offer ID (truncated for readability) --
            oid: str = offer.get("offer_id", "")
            item_id = QTableWidgetItem(oid[:16] + "..." if len(oid) > 16 else oid)
            item_id.setToolTip(oid)
            item_id.setData(Qt.ItemDataRole.UserRole, oid)  # Store full ID.
            self._table.setItem(row_idx, 0, item_id)

            # -- Pair --
            self._table.setItem(
                row_idx, 1, QTableWidgetItem(offer.get("pair_name", ""))
            )

            # -- Side (coloured) --
            side: str = offer.get("side", "")
            item_side = QTableWidgetItem(side.upper())
            item_side.setForeground(_side_color(side))
            item_side.setFont(QFont("JetBrains Mono", 10, QFont.Weight.Bold))
            self._table.setItem(row_idx, 2, item_side)

            # -- Price (mojos -> XCH) --
            price_mojos: int = offer.get("price_mojos", 0)
            item_price = QTableWidgetItem(mojos_to_xch(price_mojos))
            item_price.setFont(mono_font)
            item_price.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            # Store raw mojos for correct numeric sorting.
            item_price.setData(Qt.ItemDataRole.UserRole, price_mojos)
            self._table.setItem(row_idx, 3, item_price)

            # -- Size (mojos -> XCH) --
            size_mojos: int = offer.get("size_mojos", 0)
            item_size = QTableWidgetItem(mojos_to_xch(size_mojos))
            item_size.setFont(mono_font)
            item_size.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            item_size.setData(Qt.ItemDataRole.UserRole, size_mojos)
            self._table.setItem(row_idx, 4, item_size)

            # -- Tier --
            tier: int = offer.get("tier", 0)
            tier_label = f"{tier} ({TIER_NAMES.get(tier, '?')})"
            self._table.setItem(row_idx, 5, QTableWidgetItem(tier_label))

            # -- Status (coloured badge) --
            status: str = offer.get("status", "")
            item_status = QTableWidgetItem(status.capitalize())
            item_status.setForeground(_status_color(status))
            item_status.setFont(QFont("JetBrains Mono", 10, QFont.Weight.Bold))
            self._table.setItem(row_idx, 6, item_status)

            # -- Created Block --
            created_block: int = offer.get("created_block", 0)
            item_block = QTableWidgetItem(str(created_block))
            item_block.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            item_block.setData(Qt.ItemDataRole.UserRole, created_block)
            self._table.setItem(row_idx, 7, item_block)

            # -- Age (blocks) --
            age: int = max(0, self._current_block - created_block) if created_block else 0
            item_age = QTableWidgetItem(str(age))
            item_age.setTextAlignment(
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
            )
            item_age.setData(Qt.ItemDataRole.UserRole, age)
            # Highlight stale offers that exceed the TTL threshold.
            if age > self._offer_ttl:
                item_age.setForeground(QColor(COLORS.LOSS_RED))
            elif age > int(self._offer_ttl * 0.8):
                item_age.setForeground(QColor(COLORS.WARNING_YELLOW))
            self._table.setItem(row_idx, 8, item_age)

            # -- Actions (cancel button for pending offers) --
            if status.lower() == "pending":
                btn_cancel = QPushButton("Cancel")
                btn_cancel.setObjectName("dangerButton")
                btn_cancel.setFixedHeight(24)
                # Capture offer_id by default argument to avoid late-binding.
                btn_cancel.clicked.connect(
                    lambda checked=False, oid_=oid: self._on_cancel_single(oid_)
                )
                self._table.setCellWidget(row_idx, 9, btn_cancel)
            else:
                self._table.setItem(row_idx, 9, QTableWidgetItem(""))

        self._table.setSortingEnabled(True)

    # ------------------------------------------------------------------
    # Internal: summary
    # ------------------------------------------------------------------

    def _update_summary(self) -> None:
        """Recompute the bottom summary bar from ``_all_offers``."""
        total = len(self._all_offers)
        pending = sum(1 for o in self._all_offers if o.get("status", "").lower() == "pending")
        filled = sum(1 for o in self._all_offers if o.get("status", "").lower() == "filled")
        fill_rate = (filled / total * 100.0) if total > 0 else 0.0

        # Total value locked = sum of (price * size) for pending offers.
        locked_mojos: int = 0
        for o in self._all_offers:
            if o.get("status", "").lower() == "pending":
                locked_mojos += int(o.get("size_mojos", 0))

        self._lbl_total.setText(f"Total: {total}")
        self._lbl_pending.setText(f"Pending: {pending}")
        self._lbl_filled.setText(f"Filled: {filled}")
        self._lbl_fill_rate.setText(f"Fill rate: {fill_rate:.1f}%")
        self._lbl_locked.setText(f"Locked: {mojos_to_xch(locked_mojos)} XCH")

    # ------------------------------------------------------------------
    # Context menu
    # ------------------------------------------------------------------

    def _show_context_menu(self, position) -> None:
        """Display a right-click context menu for the selected row.

        Menu items:
            - Copy Offer ID    -- copies full ID to clipboard.
            - Cancel Offer     -- only for pending offers.
            - View on Dexie    -- opens dexie.space in the browser.
        """
        row = self._table.rowAt(position.y())
        if row < 0:
            return

        # Retrieve the full offer_id stored in column 0's UserRole.
        item_id = self._table.item(row, 0)
        if item_id is None:
            return
        offer_id: str = item_id.data(Qt.ItemDataRole.UserRole) or ""
        status_item = self._table.item(row, 6)
        status_text: str = status_item.text().lower() if status_item else ""

        menu = QMenu(self)

        # -- Copy Offer ID --
        act_copy = QAction("Copy Offer ID", self)
        act_copy.triggered.connect(
            lambda: self._copy_to_clipboard(offer_id)
        )
        menu.addAction(act_copy)

        # -- Cancel Offer (pending only) --
        if status_text == "pending":
            act_cancel = QAction("Cancel Offer", self)
            act_cancel.triggered.connect(
                lambda: self._on_cancel_single(offer_id)
            )
            menu.addAction(act_cancel)

        menu.addSeparator()

        # -- View on Dexie --
        act_dexie = QAction("View on Dexie", self)
        act_dexie.triggered.connect(
            lambda: self._open_dexie(offer_id)
        )
        menu.addAction(act_dexie)

        menu.exec(self._table.viewport().mapToGlobal(position))

    # ------------------------------------------------------------------
    # Actions
    # ------------------------------------------------------------------

    def _on_cancel_single(self, offer_id: str) -> None:
        """Request cancellation of a single offer after confirmation."""
        reply = QMessageBox.question(
            self,
            "Cancel Offer",
            f"Cancel offer {offer_id[:16]}...?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if reply == QMessageBox.StandardButton.Yes:
            self.cancel_offer_requested.emit(offer_id)

    def _on_cancel_all(self) -> None:
        """Request cancellation of every pending offer after confirmation."""
        pending_count = sum(
            1 for o in self._all_offers if o.get("status", "").lower() == "pending"
        )
        if pending_count == 0:
            QMessageBox.information(self, "Cancel All", "No pending offers to cancel.")
            return

        reply = QMessageBox.warning(
            self,
            "Cancel All Offers",
            f"This will cancel {pending_count} pending offer(s).\n\nContinue?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if reply == QMessageBox.StandardButton.Yes:
            self.cancel_all_requested.emit()

    @staticmethod
    def _copy_to_clipboard(text: str) -> None:
        """Place *text* on the system clipboard."""
        from PySide6.QtWidgets import QApplication

        clipboard = QApplication.clipboard()
        if clipboard is not None:
            clipboard.setText(text)

    @staticmethod
    def _open_dexie(offer_id: str) -> None:
        """Open the offer's Dexie page in the default browser."""
        import webbrowser

        url = f"https://dexie.space/offers/{offer_id}"
        webbrowser.open(url)
