"""The dashboard's RECENT ACTIVITY feed.

The panel was built but never connected: `DashboardWidget.update_trades` was
defined and called from nowhere in the repo, so it could not display a single
row however long the bot ran.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def app():
    yield QApplication.instance() or QApplication(sys.argv)


def _row(**over):
    row = {
        "timestamp": "2026-08-21T16:54:15.943Z",
        "pair_name": "XCH/wUSDC.b",
        "side": "bid",
        "price_mojos": 1486000000000,
        "size_mojos": 1000000000000,
    }
    row.update(over)
    return row


def test_a_fill_renders_with_the_trade_logs_own_scaling(app):
    from gui.widgets.main_window import activity_event

    event = activity_event(_row())
    assert event["timestamp"] == "16:54:15"
    # price_mojos / 1e12 = 1.486, and a stablecoin-quoted pair shows "$".
    assert "$1.4860" in event["message"]
    # size_mojos / base units = 1 XCH.
    assert "1.0000" in event["message"]
    assert "XCH/wUSDC.b" in event["message"]


def test_a_non_stablecoin_pair_is_not_given_a_dollar_sign(app):
    from gui.widgets.main_window import activity_event

    event = activity_event(_row(pair_name="XCH/BYC", side="ask"))
    assert "$" not in event["message"]
    assert event["message"].startswith("ASK")


def test_the_side_glyph_distinguishes_buys_from_sells(app):
    from gui.widgets.main_window import activity_event

    assert activity_event(_row(side="bid"))["icon"] != \
        activity_event(_row(side="ask"))["icon"]


def test_one_bad_row_does_not_empty_the_feed(app):
    from gui.widgets.main_window import activity_event

    assert activity_event(_row(price_mojos="not-a-number")) is None
    assert activity_event(_row()) is not None


def test_the_feed_replaces_rather_than_appends(app):
    """The source is a snapshot re-delivered whole on every refresh.

    Appending would repeat every row each time the query re-ran, so the feed
    would grow without bound and show each fill many times.
    """
    from gui.widgets.dashboard import DashboardWidget

    dash = DashboardWidget()
    events = [{"timestamp": "16:54:15", "icon": "x", "message": "FILL"}] * 3
    dash.update_trades(events)
    first = dash._activity_list.count()
    dash.update_trades(events)          # same snapshot again
    assert dash._activity_list.count() == first, "feed duplicated its rows"


def test_the_feed_is_actually_connected():
    """The original defect: a populate method nothing ever called."""
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "main_window.py").read_text(encoding="utf-8")
    assert "trades_loaded.connect(self._on_trades_for_activity)" in source


def test_scrolling_up_is_not_undone_by_the_next_snapshot(app):
    """clear() drops the scrollbar to 0/0, which reads as "at the bottom".

    Without preserving the flag, a user reading an older fill is yanked back
    to the bottom every refresh tick.
    """
    from gui.widgets.dashboard import DashboardWidget

    dash = DashboardWidget()
    dash.update_trades([{"timestamp": "16:54:15", "icon": "x",
                         "message": f"FILL {i}"} for i in range(40)])
    dash._activity_list.setFixedHeight(100)   # force a real scrollbar
    dash.show()
    QApplication.processEvents()
    bar = dash._activity_list.verticalScrollBar()
    bar.setValue(bar.maximum() // 2)          # user scrolls up to read
    dash._auto_scroll = False
    parked_at = bar.value()
    assert parked_at > 0, "fixture produced no scroll range"

    dash.update_trades([{"timestamp": "16:55:00", "icon": "x",
                         "message": f"FILL {i}"} for i in range(40)])
    QApplication.processEvents()
    assert dash._auto_scroll is False, "auto-scroll was silently re-armed"
    # The flag alone is not enough: clear() also loses the position, which
    # would drop the reader at the TOP of the rebuilt list.
    assert bar.value() == parked_at, "reader's position was not preserved"
    dash.hide()


def test_a_snapshot_that_renders_to_nothing_keeps_the_previous_feed(app):
    """Bad rows must not discard a good feed.

    activity_event already skips a malformed row; the caller must not then
    replace the whole feed with the resulting empty list.
    """
    from gui.widgets.main_window import activity_event

    bad = {"pair_name": "XCH/wUSDC.b", "side": "bid",
           "price_mojos": "nope", "size_mojos": "nope"}
    rows = [bad, bad]
    events = [e for e in (activity_event(t) for t in rows) if e is not None]
    assert rows and not events, "fixture no longer exercises the guard"

    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "main_window.py").read_text(encoding="utf-8")
    body = source.split("def _on_trades_for_activity")[1].split("\n    def ")[0]
    assert "if rows and not events" in body, (
        "an unrenderable snapshot would clear the feed"
    )


def test_the_feed_does_not_add_a_second_refresh_mechanism():
    """DatabaseService._on_auto_refresh already re-issues the trades query.

    A count-driven query duplicated it and pushed the same 1000 rows through
    the Trade Log and chart handlers again.
    """
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "main_window.py").read_text(encoding="utf-8")
    assert "_refresh_activity_on_new_fill" not in source
