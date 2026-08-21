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
