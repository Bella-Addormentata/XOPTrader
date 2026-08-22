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
    """What the fit should produce: rows, header, frame, and any reserved
    horizontal scrollbar.

    Mirrors the production formula deliberately, including the reservation.
    Omitting it made these assertions disagree with the implementation by
    exactly the scrollbar height whenever the content overflowed.
    """
    rows = sum(table.rowHeight(r) for r in range(table.rowCount()))
    bar = table.horizontalScrollBar()
    reserved = (bar.sizeHint().height()
                if bar and bar.maximum() > bar.minimum() else 0)
    return (rows + table.horizontalHeader().height()
            + 2 * table.frameWidth() + reserved)


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

    # showEvent() kicks off the real suggested-allocation worker, which
    # fetches from dexie and reads SQLite on its own QThread.  A unit test
    # must not depend on an external service, and a thread outliving the
    # widget aborts the interpreter -- the same failure mode as the
    # ownership bug handled below.  Stub it before anything is shown.
    w._refresh_suggested_allocation = lambda: None

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


def test_the_fit_tracks_real_row_heights(app):
    """Height comes from real row heights, not a per-row constant.

    Historical note: before S10 the local QSS pinned 12px/11px fonts, so
    the GLOBAL delta could not reach these tables and an earlier version of
    this test passed vacuously.  S10 derives the local QSS from
    theme.scaled_px(), so the delta DOES reach them now (covered by
    test_the_font_size_setting_reaches_this_page).  This test keeps driving
    row height directly because its subject is narrower: the fit must track
    whatever the rows measure, however they came to measure it.
    """
    app.setStyleSheet(theme.get_stylesheet())
    heights = []
    for row_px in (18, 30, 44):
        w = WalletBalancesWidget()
        # Drive the row height directly.  Neither the global delta nor
        # setFont() reaches these tables -- the widget's local QSS pins the
        # font size and overrides both -- and this is the quantity the fit
        # actually sums, so it is the honest lever.
        w._table.verticalHeader().setDefaultSectionSize(row_px)
        w.update_balances(_balances(8), market_data={}, stuck_offers=0)
        assert w._table.height() == _content_height(w._table)
        heights.append(w._table.height())

    assert len(set(heights)) > 1, (
        "row geometry never changed across font sizes, so this test would "
        "pass even if the fit ignored row heights entirely"
    )


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
        assert table.height() < 220, "still at the unfitted default"


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
    assert w._table.height() < 220, "left at the unfitted default"


def test_a_narrow_window_does_not_clip_the_last_row(app):
    """Stretch mode does not rule out a horizontal scrollbar.

    Sections still honour minimumSectionSize, so a narrow enough window
    brings the bar back -- reproduced at a 300px table width. With the
    vertical bar switched off, any height it steals clips the last row
    outright instead of becoming scrollable.
    """
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    w._refresh_suggested_allocation = lambda: None
    w.update_balances(_balances(6), market_data={}, stuck_offers=0)
    table = w._table
    w.show()
    app.processEvents()

    def needed() -> int:
        rows = sum(table.rowHeight(r) for r in range(table.rowCount()))
        bar = table.horizontalScrollBar()
        extra = bar.sizeHint().height() if bar.isVisible() else 0
        return (rows + table.horizontalHeader().height()
                + 2 * table.frameWidth() + extra)

    try:
        saw_scrollbar = False
        for width in (1000, 300, 150, 1000):
            table.setFixedWidth(width)
            app.processEvents()
            saw_scrollbar |= table.horizontalScrollBar().isVisible()
            assert table.height() >= needed(), (
                f"width {width}px: {table.height()}px cannot show "
                f"{needed()}px of content -- last row clipped"
            )
        assert saw_scrollbar, "fixture never produced a horizontal scrollbar"
    finally:
        w.hide()


def test_a_page_that_overflows_while_hidden_is_not_clipped_when_opened(app):
    """This page sits hidden in the stack while updates keep arriving.

    A hidden widget reports isVisible() False even once its content
    overflows, and opening it need not emit another rangeChanged -- so a
    visibility-based reservation left the newly shown bar eating the last
    row. The range is nonzero whether or not anything is on screen.
    """
    from PySide6.QtWidgets import QStackedWidget

    app.setStyleSheet(theme.get_stylesheet())
    stack = QStackedWidget()
    front = WalletBalancesWidget()
    front._refresh_suggested_allocation = lambda: None
    page = WalletBalancesWidget()
    page._refresh_suggested_allocation = lambda: None
    stack.addWidget(front)
    stack.addWidget(page)
    stack.setCurrentIndex(0)              # the wallet page is HIDDEN
    stack.resize(300, 800)
    stack.show()
    app.processEvents()

    try:
        page.update_balances(_balances(6), market_data={}, stuck_offers=0)
        table = page._table
        table.setFixedWidth(250)          # force horizontal overflow
        app.processEvents()
        bar = table.horizontalScrollBar()

        def needed() -> int:
            rows = sum(table.rowHeight(r) for r in range(table.rowCount()))
            extra = (bar.sizeHint().height()
                     if bar.maximum() > bar.minimum() else 0)
            return (rows + table.horizontalHeader().height()
                    + 2 * table.frameWidth() + extra)

        assert bar.maximum() > bar.minimum(), "fixture produced no overflow"
        assert not bar.isVisible(), "fixture did not keep the page hidden"
        assert table.height() >= needed(), "space not reserved while hidden"

        stack.setCurrentIndex(1)          # user opens the page
        app.processEvents()
        assert table.height() >= needed(), "last row clipped on opening"
    finally:
        stack.hide()


def test_narrowing_after_population_grows_the_height_via_rangechanged(app):
    """The ONLY thing that refits on a later resize is the rangeChanged
    connection -- deleting it left every test in this file green while a
    narrowed window clipped the last row in production.
    """
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    w._refresh_suggested_allocation = lambda: None
    w.update_balances(_balances(6), market_data={}, stuck_offers=0)
    table = w._table
    w.show()
    app.processEvents()
    try:
        table.setFixedWidth(1000)        # wide: no horizontal overflow
        app.processEvents()
        before = table.height()
        bar = table.horizontalScrollBar()
        assert bar.maximum() <= bar.minimum(), "fixture overflowed while wide"

        table.setFixedWidth(250)         # narrow: overflow appears
        app.processEvents()
        assert bar.maximum() > bar.minimum(), "fixture produced no overflow"
        assert table.height() > before, (
            "no refit followed the range change -- the reservation only "
            "happens if something re-runs the fit, and nothing did"
        )
        assert table.height() == _content_height(table)
    finally:
        w.hide()


def test_the_early_return_fit_is_what_restores_a_wrong_height(app):
    """update_balances({}) returns before the main fit; the fit on that path
    is a separate line, and the previous test passed with it deleted because
    construction had already fitted the table.
    """
    app.setStyleSheet(theme.get_stylesheet())
    w = WalletBalancesWidget()
    w._refresh_suggested_allocation = lambda: None
    table = w._table
    table.setFixedHeight(480)            # knock it out of fit deliberately
    assert table.height() != _content_height(table)

    w.update_balances({}, market_data={}, stuck_offers=0)
    assert table.height() == _content_height(table), (
        "the empty-payload path did not refit the table"
    )


def test_the_font_size_setting_reaches_this_page(app):
    """S10: local QSS pinned 12px/11px fonts, overriding the application
    stylesheet -- the operator's font setting changed nothing here. The
    pixel sizes now derive from theme.scaled_px(), and rows follow.
    """
    try:
        theme.apply_theme(app, font_size_delta=0)
        base = WalletBalancesWidget()
        base._refresh_suggested_allocation = lambda: None
        base.update_balances(_balances(4), market_data={}, stuck_offers=0)
        base_row, base_h = base._table.rowHeight(0), base._table.height()

        theme.apply_theme(app, font_size_delta=4)
        bigger = WalletBalancesWidget()
        bigger._refresh_suggested_allocation = lambda: None
        bigger.update_balances(_balances(4), market_data={}, stuck_offers=0)

        assert bigger._table.rowHeight(0) > base_row, (
            "rows ignored the font-size delta -- S10 has regressed"
        )
        assert bigger._table.height() > base_h, "the fit did not follow"

        # The QSS itself must scale -- row height alone would still pass if
        # the stylesheets reverted to pinned 12px/11px, which IS the S10
        # failure.  The applied stylesheet is the mechanism, so assert on it
        # for BOTH tables.
        for table in (bigger._table, bigger._alloc_table):
            sheet = table.styleSheet()
            assert "font-size: 16px" in sheet, (
                "table QSS is not scaled at delta 4 (expected 12+4)"
            )
            assert "font-size: 15px" in sheet, (
                "header QSS is not scaled at delta 4 (expected 11+4)"
            )
            assert "font-size: 12px" not in sheet
        # And the allocation table's geometry follows its scaled rows.
        bigger._alloc_table.setRowCount(3)
        from gui.widgets.wallet_balances import _fit_table_to_contents
        _fit_table_to_contents(bigger._alloc_table)
        base._alloc_table.setRowCount(3)
        _fit_table_to_contents(base._alloc_table)
        assert (bigger._alloc_table.height() > base._alloc_table.height()), (
            "allocation table did not grow with the delta"
        )
    finally:
        theme.apply_theme(app, font_size_delta=0)
        app.setStyleSheet(theme.get_stylesheet())
