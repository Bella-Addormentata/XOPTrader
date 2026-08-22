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

    from datetime import datetime

    event = activity_event(_row())
    # The clock is LOCAL time now; compute the expectation the same way.
    expected = (datetime.fromisoformat("2026-08-21T16:54:15.943+00:00")
                .astimezone().strftime("%H:%M:%S"))
    assert event["timestamp"] == expected
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


def test_same_block_fills_come_back_newest_first(app, tmp_path):
    """block_height alone is not a total order.

    Several fills routinely land in one block, so without an id tie-breaker
    the newest-N slice the feed takes could shuffle them -- or drop the
    newest at the LIMIT cutoff.
    """
    import sqlite3
    from gui.services.database_service import _DatabaseWorker

    db = tmp_path / "trades.db"
    con = sqlite3.connect(str(db))
    con.execute("""CREATE TABLE trade_log (id INTEGER PRIMARY KEY,
                   timestamp TEXT, pair_name TEXT, side TEXT,
                   price_mojos INTEGER, size_mojos INTEGER,
                   block_height INTEGER)""")
    con.executemany(
        "INSERT INTO trade_log VALUES (?,?,?,?,?,?,?)",
        [(i, "2026-08-21T16:0%d:00.000Z" % i, "XCH/wUSDC.b", "bid",
          1_486_000_000_000, 1_000_000_000_000, 9181181) for i in range(1, 6)],
    )
    con.commit()
    con.close()

    worker = _DatabaseWorker()
    worker.open(str(db))
    got = []
    worker.trades_ready.connect(lambda rows: got.extend(rows))
    worker.fetch_trades("", "", None, None, 3)

    ids = [r["id"] for r in got]
    assert ids == [5, 4, 3], f"same-block fills not newest-first: {ids}"


def test_the_feed_limits_are_one_source_of_truth():
    """Converting more rows than the widget keeps is silently wasted work.

    main_window converted 25 while update_trades() capped at 20, so five
    events were built and then deterministically discarded -- and the
    documented count did not match what the panel rendered.
    """
    from gui.widgets.dashboard import _ACTIVITY_FEED_MAX
    from gui.widgets.main_window import _ACTIVITY_FEED_ROWS

    assert _ACTIVITY_FEED_ROWS == _ACTIVITY_FEED_MAX


def test_the_feed_clock_is_local_time(app):
    """trade_log stamps are UTC; every other dashboard clock is local.

    A bare UTC HH:MM:SS five hours off the status line reads as a different
    event time, not as a timezone.
    """
    from datetime import datetime, timezone
    from gui.widgets.main_window import activity_event

    stamp = "2026-08-22T15:16:22.883Z"
    event = activity_event({**{"timestamp": stamp}, "pair_name": "XCH/BYC",
                            "side": "bid", "price_mojos": 1_486_000_000_000,
                            "size_mojos": 1_000_000_000_000})
    expected = (datetime.fromisoformat(stamp.replace("Z", "+00:00"))
                .astimezone().strftime("%H:%M:%S"))
    assert event["timestamp"] == expected
    # And on any box not running UTC, that differs from the raw slice.
    if expected != "15:16:22":
        assert event["timestamp"] != "15:16:22"


def test_the_pipeline_from_signal_to_widget_actually_runs(app):
    """No test invoked _on_trades_for_activity; the greps could not notice a
    broken slice, reversal, or dashboard handoff."""
    from gui.widgets.main_window import MainWindow

    window = MainWindow()
    try:
        captured = []

        class _Dash:
            def update_trades(self, events):
                captured.append(events)

        window._dashboard = _Dash()
        # NEWEST FIRST, as fetch_trades orders them (block DESC, id DESC).
        # The first fixture had them ascending while claiming this, so the
        # order assertion tested the reversal against an inverted premise.
        rows = [{"timestamp": f"2026-08-22T15:16:{i:02d}.000Z",
                 "pair_name": "XCH/wUSDC.b", "side": "bid",
                 "price_mojos": 1_486_000_000_000,
                 "size_mojos": 1_000_000_000_000}
                for i in range(29, -1, -1)]
        window._on_trades_for_activity(rows)   # newest-first, as the DB emits

        assert len(captured) == 1, "the feed was never handed the events"
        events = captured[0]
        from gui.widgets.dashboard import _ACTIVITY_FEED_MAX
        assert len(events) == _ACTIVITY_FEED_MAX, "cap not applied"
        # DB emits newest-first; the feed reads top-down oldest-first.
        assert events[0]["timestamp"] < events[-1]["timestamp"], (
            "slice was not reversed into chronological order"
        )
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()
