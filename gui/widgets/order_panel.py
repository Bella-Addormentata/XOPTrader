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
from PySide6.QtGui import QAction, QColor, QFont, QShowEvent
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

from gui.theme import COLORS, MONO_FONT_FAMILY, fit_row_height
from gui.utils import mojos_to_xch, mojos_per_unit_for_pair, format_price, num, text

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
    ("Filled At",     145),
    ("Created\nBlock", 100),
    ("Age\n(blocks)",   90),
    ("Actions",        80),
]

# Offer status values used throughout the system.
_STATUSES: list[str] = ["All", "Pending", "Filled", "Cancelled", "Expired"]

# ---------------------------------------------------------------------------
# Row budget
# ---------------------------------------------------------------------------
# offer_log grows without bound (14 k+ rows after a few weeks of trading, the
# vast majority of them cancels).  Building a QTableWidget row costs ~11 item
# objects plus a cell widget, and while the table is *visible* the
# content-fitted header re-measures every column on every insertion, which
# makes a full rebuild quadratic: 500 rows took ~12 s and 926 rows ~41 s on
# the operator's machine before this cap existed.
#
# This panel is an operational tool for live offers -- nobody scrolls 13 000
# cancels -- so render at most this many rows, newest-first, and tell the user
# when the view is truncated.
_MAX_VISIBLE_ROWS: int = 500

# Rows requested from SQL per query.  The worker thread applies the pair and
# status filters so only this many rows ever cross to the GUI thread; the
# side and search filters then narrow *within* this window client-side.
_QUERY_LIMIT: int = 500

# How many rows QHeaderView samples when fitting a column to its contents.
# Qt's default is 1000, i.e. every visible row gets measured for all 11
# columns on every layout pass.  The values here are monospaced and uniform
# in width, so a small sample produces identical widths for a fraction of
# the cost.
_RESIZE_PRECISION: int = 50


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


# Colour lookups are built once: constructing a QColor per cell was a
# measurable slice of the table rebuild.  setForeground() copies the value
# into a QBrush, so sharing these instances is safe.
_STATUS_COLORS: dict[str, QColor] = {}
_SIDE_COLORS: dict[str, QColor] = {}


def _status_color(status: str) -> QColor:
    """Return the badge colour for a given offer status string.

    Colour mapping:
        pending   -> WARNING_YELLOW
        filled    -> PROFIT_GREEN
        cancelled -> TEXT_SECONDARY (gray)
        expired   -> LOSS_RED
    """
    if not _STATUS_COLORS:
        _STATUS_COLORS.update({
            "filled": QColor(COLORS.PROFIT_GREEN),
            "pending": QColor(COLORS.WARNING_YELLOW),
            "cancelled": QColor(COLORS.TEXT_SECONDARY),
            "expired": QColor(COLORS.LOSS_RED),
            "": QColor(COLORS.TEXT_PRIMARY),
        })
    return _STATUS_COLORS.get(status.lower(), _STATUS_COLORS[""])


def _side_color(side: str) -> QColor:
    """Return green for BID, red for ASK."""
    if not _SIDE_COLORS:
        _SIDE_COLORS.update({
            "bid": QColor(COLORS.PROFIT_GREEN),
            "ask": QColor(COLORS.LOSS_RED),
        })
    return _SIDE_COLORS.get(side.lower(), _SIDE_COLORS["ask"])


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
    #: (pair, status, limit) -- ask the database service to re-query
    #: ``offer_log`` with the filters the user just selected.  Empty
    #: strings mean "no filter".  Connected to ``DatabaseService.query_offers``
    #: by MainWindow; when nothing is connected the panel still filters the
    #: data it already holds, client-side.
    offers_query_requested = Signal(str, str, int)

    # Default offer TTL for age-warning colouring (blocks).
    _DEFAULT_TTL: int = 60

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)

        # Internal bookkeeping.
        self._current_block: int = 0
        self._offer_ttl: int = self._DEFAULT_TTL
        self._all_offers: list[dict] = []

        # Aggregate offer counts straight from SQL (see
        # ``update_offer_summary``).  The summary bar and the Cancel-All
        # guard read these so they stay correct even when the payload
        # only holds one status.
        self._summary_stats: dict = {}

        # Pair names ever seen, in first-seen order.  Accumulated rather
        # than rebuilt so that filtering by status (which narrows the SQL
        # result set) cannot make pairs vanish from the combo.
        self._known_pairs: list[str] = []

        # True when data arrived while the widget was hidden and the
        # table render was deferred until the next showEvent.
        self._render_pending: bool = False

        # Rows that matched the current filters before the render cap,
        # and whether the side/search filters narrowed them client-side.
        self._matched_count: int = 0
        self._client_filtered: bool = False

        # pair_name -> mojos-per-base-unit, resolved once per pair
        # instead of once per row.
        self._mpu_cache: dict[str, int] = {}

        # Per-row objects hoisted out of the population loop: constructing
        # a QFont/QColor per cell dominated the rebuild cost.
        self._mono_font = QFont(MONO_FONT_FAMILY, 10)
        self._mono_font.setStyleHint(QFont.StyleHint.Monospace)
        self._mono_bold = QFont(MONO_FONT_FAMILY, 10, QFont.Weight.Bold)
        self._mono_bold.setStyleHint(QFont.StyleHint.Monospace)
        self._secondary_color = QColor(COLORS.TEXT_SECONDARY)
        self._warn_color = QColor(COLORS.WARNING_YELLOW)
        self._loss_color = QColor(COLORS.LOSS_RED)

        self._build_ui()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        """Construct the complete widget layout."""
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(12)

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
        bar.setSpacing(14)

        # Pair filter
        lbl_pair = QLabel("Pair:")
        self._combo_pair = QComboBox()
        self._combo_pair.addItem("All Pairs")
        self._combo_pair.setMinimumWidth(140)
        # Pair and status are pushed down to SQL (see _on_query_filter_changed);
        # side and search stay client-side over the rows already fetched.
        self._combo_pair.currentIndexChanged.connect(self._on_query_filter_changed)
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
        self._combo_status.currentIndexChanged.connect(self._on_query_filter_changed)
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
        btn_refresh.clicked.connect(lambda: self._on_query_filter_changed())
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
        fit_row_height(table)   # rows must fit the compact Cancel button
        table.setShowGrid(True)
        table.setSortingEnabled(True)
        # Sorting is re-enabled after every fill, which re-sorts by the
        # indicator column; Qt's default is column 0, so the table used
        # to present offers ordered by their *id hash*.  Point it at
        # Created Block instead so the rows shown are the newest ones the
        # row cap kept, in the order the cap implies.  It also keeps the
        # row->offer mapping stable between refreshes, which is what
        # makes row reuse in _fill_rows pay off.
        table.sortByColumn(8, Qt.SortOrder.DescendingOrder)

        # Column sizing: fit every column to its content so widths track the
        # data instead of the hardcoded guesses in _COLUMNS (which left Pair
        # cramped and Price/Size oversized).  Offer ID keeps Stretch so the
        # table still fills its viewport; the truncated hash absorbs the
        # leftover space gracefully.
        header = table.horizontalHeader()
        # Bound the per-column content measurement: without this Qt walks
        # every row of every column on each layout pass (see
        # _RESIZE_PRECISION).
        header.setResizeContentsPrecision(_RESIZE_PRECISION)
        header.setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        # A monospaced font keeps digit columns aligned and, because every
        # digit is equal width, stops ResizeToContents from jittering as
        # values change between refreshes.
        mono = QFont(MONO_FONT_FAMILY)
        mono.setStyleHint(QFont.StyleHint.Monospace)
        table.setFont(mono)
        # The global theme pads header sections 10px 16px -- 32px of dead
        # horizontal space per column, which dominates narrow columns like
        # Side and Age once widths are content-fitted.  Tighten locally
        # (this table only) and centre the now two-line header labels.
        header.setDefaultAlignment(Qt.AlignmentFlag.AlignCenter)
        table.setStyleSheet(
            "QHeaderView::section { padding: 4px 6px; }"
            " QTableWidget::item { padding: 2px 6px; }"
        )

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

        # Truncation notice -- only visible when the row cap hides rows.
        self._lbl_truncated = QLabel("")
        self._lbl_truncated.setStyleSheet(
            f"color: {COLORS.WARNING_YELLOW}; font-size: 9pt;"
        )
        self._lbl_truncated.setVisible(False)
        bar.addWidget(self._lbl_truncated)

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
            Offers from the DB.  Already narrowed by pair/status and
            capped at :data:`_QUERY_LIMIT` rows by the worker thread when
            :pyattr:`offers_query_requested` is connected.
        """
        self._all_offers = list(offers)

        # The DB auto-refresh delivers offers every 10 s regardless of
        # which page is shown; a full QTableWidget rebuild is wasted
        # work while this panel is hidden.  Cache the payload (above)
        # and defer the render until the next showEvent.
        if not self.isVisible():
            self._render_pending = True
            return
        self._render_now()

    def _render_now(self) -> None:
        """Rebuild the pair combo, table, and summary from cached data."""
        self._render_pending = False
        self._rebuild_pair_combo()
        self._apply_filters()

    def showEvent(self, event: QShowEvent) -> None:
        """Replay any render deferred while the widget was hidden."""
        super().showEvent(event)
        if self._render_pending:
            self._render_now()

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

    def update_offer_summary(self, stats: dict) -> None:
        """Adopt whole-table offer aggregates computed in SQL.

        The table payload only holds the rows for the selected status
        (capped at :data:`_QUERY_LIMIT`), so the summary bar, the
        truncation notice, and the *Cancel All* guard cannot be derived
        from it.  ``DatabaseService.offer_summary_loaded`` supplies the
        real counts instead.

        Parameters
        ----------
        stats:
            ``{"total", "pending", "filled", "cancelled", "expired",
            "locked_mojos"}`` -- counts over the whole ``offer_log``.
        """
        self._summary_stats = dict(stats or {})
        self._update_summary()
        # The "showing N of M" notice depends on these counts.
        self._update_truncation_notice()

    # ------------------------------------------------------------------
    # Internal: filtering
    # ------------------------------------------------------------------

    def _rebuild_pair_combo(self) -> None:
        """Add any newly seen pair names to the pair filter combo.

        Entries accumulate instead of being rebuilt: the payload is
        narrowed by the active status filter, so a rebuild would drop
        every pair that happens to have no offer in that status and
        silently reset the user's selection.
        """
        new_pairs = [
            pname
            for offer in self._all_offers
            if (pname := offer.get("pair_name", ""))
            and pname not in self._known_pairs
        ]
        if not new_pairs:
            return

        self._combo_pair.blockSignals(True)
        for pname in new_pairs:
            if pname not in self._known_pairs:
                self._known_pairs.append(pname)
                self._combo_pair.addItem(pname)
        self._combo_pair.blockSignals(False)

    def _on_query_filter_changed(self) -> None:
        """Handle a change to a filter that is resolved in SQL.

        Asks the database service for the newly selected pair/status
        slice (the worker thread does the filtering and the capping, so
        at most :data:`_QUERY_LIMIT` rows ever reach the GUI thread) and
        re-renders whatever is already cached so the table responds
        immediately rather than waiting for the query to come back.
        """
        pair = self._combo_pair.currentText()
        status = self._combo_status.currentText()
        self.offers_query_requested.emit(
            "" if pair == "All Pairs" else pair,
            "" if status.lower() == "all" else status.lower(),
            _QUERY_LIMIT,
        )
        self._apply_filters()

    def _apply_filters(self) -> None:
        """Filter ``_all_offers`` according to the current UI state
        and repopulate the table rows.

        Pair and status are normally applied in SQL already; repeating
        them here is harmless and keeps the panel correct when nothing
        is connected to :pyattr:`offers_query_requested` (tests, or a
        stale payload that arrived before the re-query returned).  Side
        and search always filter client-side, i.e. *within* the rows
        fetched for the selected pair/status.
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
            if side_filter != "all" and text(offer, "side").lower() != side_filter:
                continue
            # Status filter
            if status_filter != "all" and text(offer, "status").lower() != status_filter:
                continue
            # Free-text search (matches against offer_id and pair_name)
            if search_text:
                searchable = (
                    text(offer, "offer_id")
                    + text(offer, "pair_name")
                ).lower()
                if search_text not in searchable:
                    continue
            filtered.append(offer)

        # Newest first, then hard-cap the render.  The SQL side already
        # orders by created_block DESC; sorting again makes the cap
        # deterministic regardless of how the payload was produced.
        filtered.sort(
            key=lambda o: (o.get("created_block") or 0, o.get("id") or 0),
            reverse=True,
        )
        self._matched_count = len(filtered)
        self._client_filtered = bool(search_text) or side_filter != "all"
        self._populate_table(filtered[:_MAX_VISIBLE_ROWS])
        self._update_summary()
        self._update_truncation_notice()

    def _update_truncation_notice(self) -> None:
        """Show how many rows are hidden by the row cap, if any.

        Rows can be hidden twice over: the worker caps the query at
        :data:`_QUERY_LIMIT`, and the table caps the render at
        :data:`_MAX_VISIBLE_ROWS`.  The count of what exists in the DB
        comes from the SQL aggregates when they are available.
        """
        shown = self._table.rowCount()
        payload = len(self._all_offers)
        matched = self._matched_count
        status = self._combo_status.currentText().lower()

        # How many offers of this status exist in the database?
        if status == "all":
            available = int(self._summary_stats.get("total", 0) or 0)
        else:
            available = int(self._summary_stats.get(status, 0) or 0)
        # No aggregates yet -- fall back to what the payload showed.
        available = max(available, matched)

        label = "offers" if status == "all" else status
        if available > payload:
            # The query itself was capped: older offers were never fetched.
            text = (
                f"Showing newest {payload:,} of {available:,} {label} "
                f"-- narrow the pair or status filter to see older ones"
            )
        elif matched > shown:
            # Everything was fetched but the render cap hid the tail.
            text = f"Showing newest {shown:,} of {matched:,} {label}"
        else:
            self._lbl_truncated.setVisible(False)
            self._lbl_truncated.setText("")
            return

        if self._client_filtered:
            # Side/search run over the fetched window only -- say so, or
            # the row count looks like it contradicts the total.
            text += f"; {shown:,} match the side/search filter"
        self._lbl_truncated.setText(text)
        self._lbl_truncated.setVisible(True)

    # ------------------------------------------------------------------
    # Internal: table population
    # ------------------------------------------------------------------

    def _populate_table(self, offers: list[dict]) -> None:
        """Write *offers* into the QTableWidget rows.

        Callers must pass an already-capped list (see
        :data:`_MAX_VISIBLE_ROWS`); this method renders every row it is
        given.

        Four things make the rebuild cheap enough to run on the UI
        thread, and all four matter:

        * **Frozen header.**  The columns are content-fitted, so while
          the table is visible Qt re-measures every column after every
          single row insertion -- an O(rows^2) storm that cost ~12 s for
          500 rows.  The header is switched to a fixed mode for the
          duration of the fill and content-fitted exactly once at the end.
        * **Suspended updates and sorting**, restored in ``finally`` so a
          raising row cannot leave the table frozen or mis-sorted.
        * **Reused rows.**  Items and cell widgets are updated in place
          rather than destroyed and rebuilt (see :meth:`_fill_rows`);
          creating and parenting 500 link labels alone cost ~115 ms.
        * **Hoisted fonts, colours, and per-pair scale factors**, which
          were previously reconstructed for every cell.
        """
        table = self._table
        header = table.horizontalHeader()

        table.setUpdatesEnabled(False)
        table.setSortingEnabled(False)
        # Keep the current widths but stop Qt recomputing them per row.
        for col in range(len(_COLUMNS)):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.Fixed)
        try:
            self._fill_rows(offers)
        finally:
            # Restore content-fitted widths (one measuring pass) and the
            # stretch on the Offer ID column, then re-enable sorting.
            header.setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
            header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
            table.setSortingEnabled(True)
            table.setUpdatesEnabled(True)

    _RIGHT = Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter

    def _item(self, row: int, col: int, text: str) -> QTableWidgetItem:
        """Return the item at *row*/*col*, creating it only if absent.

        Reusing items across refreshes is what keeps a rebuild off the
        UI thread's critical path: allocating ~5 500 QTableWidgetItems
        and re-parenting 500 cell widgets is far more expensive than
        assigning new text to the objects already there.

        Static per-column presentation (font, alignment) is applied once
        at creation.  Everything that depends on the *data* -- text,
        sort keys, colours -- is (re)assigned by the caller on every
        pass, so a reused row can never show a previous offer's state.
        """
        item = self._table.item(row, col)
        if item is None:
            item = QTableWidgetItem(text)
            if col in (0, 1, 5, 7, 10):
                pass  # default alignment / font
            elif col in (2, 6):
                item.setFont(self._mono_bold)
            elif col in (3, 4):
                item.setFont(self._mono_font)
                item.setTextAlignment(self._RIGHT)
            else:  # 8, 9 -- numeric block columns
                item.setTextAlignment(self._RIGHT)
            self._table.setItem(row, col, item)
        else:
            item.setText(text)
        return item

    def _fill_rows(self, offers: list[dict]) -> None:
        """Write *offers* into the table, reusing existing rows.

        See :meth:`_populate_table` for the surrounding guards.
        """
        table = self._table
        # Shrinking first drops the surplus rows (and their cell
        # widgets); growing adds empty rows that _item() fills in.
        table.setRowCount(len(offers))

        # Rows arrive from `SELECT * FROM offer_log`, so nullable columns
        # (tier, fee_mojos, resolved_*, cancel_reason, the book/queue
        # analytics) are present with the value None.  Read through the
        # NULL-safe helpers, never `.get(key, default)`.
        for row_idx, offer in enumerate(offers):
            # -- Offer ID (clickable link to Dexie) --
            oid: str = text(offer, "offer_id")
            display_oid = oid[:16] + "..." if len(oid) > 16 else oid
            # Keep a plain item for sorting / UserRole data lookup, and
            # overlay a link label for the click-through to Dexie.
            item_id = self._item(row_idx, 0, display_oid)
            item_id.setData(Qt.ItemDataRole.UserRole, oid)
            link_label = table.cellWidget(row_idx, 0)
            if not isinstance(link_label, QLabel):
                link_label = QLabel()
                link_label.setOpenExternalLinks(True)
                link_label.setContentsMargins(4, 0, 4, 0)
                table.setCellWidget(row_idx, 0, link_label)
            # The tooltip holds the full id, so it doubles as the marker
            # for "this label already shows this offer" -- re-parsing the
            # rich text is the expensive part of a QLabel update.
            if link_label.toolTip() != oid:
                link_label.setText(
                    f'<a href="https://dexie.space/offers/{oid}" '
                    f'style="color:{COLORS.INFO_BLUE};">{display_oid}</a>'
                )
                link_label.setToolTip(oid)

            # -- Pair --
            pair_name: str = text(offer, "pair_name")
            self._item(row_idx, 1, pair_name)

            # -- Side (coloured) --
            side: str = text(offer, "side")
            self._item(row_idx, 2, side.upper()).setForeground(_side_color(side))

            # -- Price (mojos -> display units) --
            # Engine stores price_mojos = price × 10^12 (kMojosPerXch) for ALL
            # pairs, so prices always divide by MOJOS_PER_XCH (the default).
            # Stablecoin-quoted pairs get a "$" prefix.
            base_mpu = self._mpu_cache.get(pair_name)
            if base_mpu is None:
                base_mpu = mojos_per_unit_for_pair(pair_name, "base")
                self._mpu_cache[pair_name] = base_mpu
            price_mojos: int = num(offer, "price_mojos")
            # Store raw mojos for correct numeric sorting.
            self._item(row_idx, 3, format_price(price_mojos, pair_name)).setData(
                Qt.ItemDataRole.UserRole, price_mojos
            )

            # -- Size (mojos -> display units) --
            size_mojos: int = num(offer, "size_mojos")
            self._item(
                row_idx, 4, mojos_to_xch(size_mojos, mojos_per_unit=base_mpu)
            ).setData(Qt.ItemDataRole.UserRole, size_mojos)

            # -- Tier --
            tier: int = num(offer, "tier")
            self._item(row_idx, 5, f"{tier} ({TIER_NAMES.get(tier, '?')})")

            # -- Status (coloured badge) --
            status: str = text(offer, "status")
            self._item(row_idx, 6, status.capitalize()).setForeground(
                _status_color(status)
            )

            # -- Filled At (datetime for resolved offers) --
            resolved_at: str = text(offer, "resolved_at")
            self._item(row_idx, 7, resolved_at).setForeground(self._secondary_color)

            # -- Created Block --
            created_block: int = num(offer, "created_block")
            self._item(row_idx, 8, str(created_block)).setData(
                Qt.ItemDataRole.UserRole, created_block
            )

            # -- Age (blocks) --
            age: int = max(0, self._current_block - created_block) if created_block else 0
            item_age = self._item(row_idx, 9, str(age))
            item_age.setData(Qt.ItemDataRole.UserRole, age)
            # Highlight stale offers that exceed the TTL threshold.
            if age > self._offer_ttl:
                item_age.setForeground(self._loss_color)
            elif age > int(self._offer_ttl * 0.8):
                item_age.setForeground(self._warn_color)
            else:
                # Reused row: clear any warning colour from a previous offer.
                item_age.setData(Qt.ItemDataRole.ForegroundRole, None)

            # -- Actions (cancel button for pending offers) --
            self._item(row_idx, 10, "")
            btn_cancel = table.cellWidget(row_idx, 10)
            if status.lower() == "pending":
                if not isinstance(btn_cancel, QPushButton):
                    btn_cancel = QPushButton("Cancel")
                    btn_cancel.setObjectName("dangerButton")
                    # compact BEFORE the widget is polished: the property
                    # selector is evaluated when the style is applied, and
                    # the base rule's min-height would otherwise make this
                    # taller than the row it sits in.
                    btn_cancel.setProperty("compact", True)
                    # One connection for the button's lifetime; the offer
                    # it acts on is read from the property below, so a
                    # reused button can never cancel a stale offer.
                    btn_cancel.clicked.connect(self._on_cancel_button)
                    table.setCellWidget(row_idx, 10, btn_cancel)
                btn_cancel.setProperty("offer_id", oid)
            elif btn_cancel is not None:
                table.removeCellWidget(row_idx, 10)

    # ------------------------------------------------------------------
    # Internal: summary
    # ------------------------------------------------------------------

    def _update_summary(self) -> None:
        """Recompute the bottom summary bar.

        Prefers the whole-table SQL aggregates supplied by
        :meth:`update_offer_summary`; the table payload itself is only a
        capped slice of one status, so counting it would make *Pending*
        and *Locked* read zero whenever the user filters by, say,
        Cancelled.  Falls back to counting the payload when no
        aggregates have arrived (standalone use and tests).
        """
        stats = self._summary_stats
        if stats:
            total = int(stats.get("total", 0) or 0)
            pending = int(stats.get("pending", 0) or 0)
            filled = int(stats.get("filled", 0) or 0)
            locked_mojos = int(stats.get("locked_mojos", 0) or 0)
        else:
            total = len(self._all_offers)
            pending = sum(
                1 for o in self._all_offers if text(o, "status").lower() == "pending"
            )
            filled = sum(
                1 for o in self._all_offers if text(o, "status").lower() == "filled"
            )
            # Total value locked = sum of sizes of pending offers.
            locked_mojos = sum(
                int(num(o, "size_mojos"))
                for o in self._all_offers
                if text(o, "status").lower() == "pending"
            )
        fill_rate = (filled / total * 100.0) if total > 0 else 0.0

        self._lbl_total.setText(f"Total: {total}")
        self._lbl_pending.setText(f"Pending: {pending}")
        self._lbl_filled.setText(f"Filled: {filled}")
        self._lbl_fill_rate.setText(f"Fill rate: {fill_rate:.1f}%")
        self._lbl_locked.setText(f"Locked: {mojos_to_xch(locked_mojos)}")

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

    def _on_cancel_button(self) -> None:
        """Cancel the offer the clicked row's button currently stands for.

        Row buttons are reused across refreshes, so the offer id lives in
        a widget property that is rewritten on every fill rather than in
        a captured closure.
        """
        button = self.sender()
        offer_id = button.property("offer_id") if button is not None else ""
        if offer_id:
            self._on_cancel_single(str(offer_id))

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
        # Use the whole-table count: the payload only holds the selected
        # status, so counting it would refuse to cancel anything while
        # the user is looking at, e.g., the Filled list.
        if self._summary_stats:
            pending_count = int(self._summary_stats.get("pending", 0) or 0)
        else:
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
