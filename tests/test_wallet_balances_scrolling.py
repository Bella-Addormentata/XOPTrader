"""The wallet page must have ONE scrollbar, not three.

main_window builds this page with ``scrollable=True``, so it already lives in
a QScrollArea. When the balances and target-allocation tables also scrolled
internally, the page carried nested scrollbars: the wheel acted on whichever
widget sat under the pointer, and rows could stay hidden inside a panel that
looked complete -- on the page whose whole job is showing how much money is
where.

Both tables are now sized to their contents; the page's own scroll area does
the scrolling.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication, QScrollArea  # noqa: E402

import gui.theme as theme  # noqa: E402
from gui.widgets.wallet_balances import (  # noqa: E402
    WalletBalancesWidget, _fit_table_to_contents,
)


@pytest.fixture(scope="module")
def app():
    yield QApplication.instance() or QApplication(sys.argv)


def _balances(n: int) -> dict:
    return {
        "Wallet %d" % i: {
            "spendable": 1.5 * i, "confirmed": 2.0 * i,
            "pending_change": 0.0, "asset_id": "xch",
        }
        for i in range(1, n + 1)
    }


def _content_height(table) -> int:
    rows = sum(table.rowHeight(r) for r in range(table.rowCount()))
    return rows + table.horizontalHeader().height() + 2 * table.frameWidth()


def test_the_balances_table_shows_every_row_without_scrolling(app):
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    w.update_balances(_balances(12), market_data={}, stuck_offers=0)

    assert w._table.rowCount() == 12
    # Exactly its contents: no clipping (which would hide rows) and no slack.
    assert w._table.height() == _content_height(w._table)


def test_neither_table_keeps_its_own_scrollbar(app):
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    w.update_balances(_balances(20), market_data={}, stuck_offers=0)

    from PySide6.QtCore import Qt
    for table in (w._table, w._alloc_table):
        assert (table.verticalScrollBarPolicy()
                == Qt.ScrollBarPolicy.ScrollBarAlwaysOff)


def test_the_page_grows_so_the_outer_area_scrolls_instead(app):
    """The single remaining scrollbar belongs to the page."""
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    w.update_balances(_balances(20), market_data={}, stuck_offers=0)

    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setWidget(w)          # ownership moves to the scroll area
    scroll.resize(1100, 700)
    scroll.show()
    app.processEvents()

    try:
        assert w.height() > scroll.viewport().height()
        assert not w._table.verticalScrollBar().isVisible()
    finally:
        # Hand ownership back before teardown.  Leaving it with the scroll
        # area lets Qt and Python both delete the widget, which aborts the
        # interpreter -- the test then "passes" by killing the run.
        scroll.takeWidget()
        scroll.deleteLater()
        app.processEvents()


@pytest.mark.parametrize("delta", [-2, 0, 4])
def test_it_holds_when_the_font_size_changes(app, delta):
    """Height comes from real row heights, not a per-row constant."""
    app.setStyleSheet(theme.get_stylesheet(font_size_delta=delta))
    w = WalletBalancesWidget()
    w.update_balances(_balances(8), market_data={}, stuck_offers=0)
    assert w._table.height() == _content_height(w._table)


def test_clearing_collapses_the_tables_again(app):
    """A stale tall table would leave a gap the page still scrolled past."""
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    w.update_balances(_balances(15), market_data={}, stuck_offers=0)
    tall = w._table.height()
    w.clear()
    assert w._table.height() < tall
    assert w._table.height() == _content_height(w._table)


def test_the_helper_tracks_rows_being_added(app):
    """Guards the allocation table, whose rows arrive from a later refresh."""
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    table = w._alloc_table
    # No manual fit here any more: the widget fits both tables at
    # construction, so this baseline is the real empty height.  Fitting it by
    # hand was masking the startup case the next test covers.
    empty = table.height()
    table.setRowCount(6)
    _fit_table_to_contents(table)
    assert table.height() > empty
    assert table.height() == _content_height(table)


def test_the_page_opens_fitted_before_any_wallet_data(app):
    """Startup forwards an empty mapping; the tables must already be sized.

    Unfitted they keep a ~480px default size hint each, so the page opened
    with a large outer scroll range holding nothing.
    """
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    for table in (w._table, w._alloc_table):
        assert table.height() == _content_height(table)
        assert table.height() < 200, "still at the unfitted default"


def test_the_startup_empty_update_leaves_the_table_fitted(app):
    """update_balances returns early when there is nothing to show.

    That early return skips the fit at the end of the method, which is the
    path the page takes on startup.  Note it deliberately does NOT clear an
    existing table -- an empty payload keeps the last known balances and only
    changes the status label -- so this asserts the no-data-yet case.
    """
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    w.update_balances({}, market_data={}, stuck_offers=0)
    assert w._table.height() == _content_height(w._table)
    assert w._table.height() < 200, "left at the unfitted default"
