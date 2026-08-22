"""The orders table's Age (blocks) and Fill (min) columns.

Age was broken since the panel was built: set_current_block existed with no
caller, so _current_block stayed 0 and every age rendered as
max(0, 0 - created_block) = 0.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    instance.setStyle("Fusion")
    yield instance


def _offer(**over):
    row = {"offer_id": "0xabc123", "pair_name": "XCH/wUSDC.b", "side": "bid",
           "price_mojos": 1_486_000_000_000, "size_mojos": 1_000_000_000_000,
           "tier": 0, "status": "pending", "resolved_at": "",
           "created_block": 9_184_000, "resolved_block": 0,
           "created_at": "2026-08-22 09:00:00", "fee_mojos": 0}
    row.update(over)
    return row


def _panel(app, request=None):
    """A SHOWN panel: update_offers defers rendering while hidden, so a
    hidden fixture populates nothing and every cell reads None."""
    from gui.widgets.order_panel import OrderPanel
    panel = OrderPanel()
    panel.show()
    QApplication.processEvents()
    return panel


def test_a_live_offer_ages_against_the_supplied_tip(app):
    panel = _panel(app)
    panel.set_current_block(9_184_100)
    panel.update_offers([_offer()])
    try:
        assert panel._table.item(0, 9).text() == "100"
    finally:
        panel.hide()


def test_the_wiring_defect_is_pinned(app):
    """Without the tip the column must not silently show 0 as an age...
    it does render 0, which is exactly the defect -- so the wiring in
    main_window is asserted instead."""
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "main_window.py").read_text(encoding="utf-8")
    assert "set_current_block(block_height)" in source, (
        "nothing feeds the tip to the orders panels"
    )


def test_a_resolved_offer_freezes_at_its_resolution_block(app):
    panel = _panel(app)
    panel.set_current_block(9_999_999)          # far future tip
    panel.update_offers([_offer(status="filled", resolved_block=9_184_050,
                              resolved_at="2026-08-22 09:20:00")])
    try:
        assert panel._table.item(0, 9).text() == "50", (
            "a filled offer's age must not keep growing with the tip"
        )
    finally:
        panel.hide()


def test_fill_latency_only_for_filled_offers(app):
    from gui.widgets.order_panel import _fill_latency_minutes

    filled = _offer(status="filled",
                    created_at="2026-08-22 09:29:21",
                    resolved_at="2026-08-22 09:51:26")
    text, value = _fill_latency_minutes(filled)
    assert text == "22.1" and abs(value - 22.083) < 0.02

    # Cancelled measures how long it sat unwanted -- a different quantity.
    cancelled = _offer(status="cancelled",
                       created_at="2026-08-22 09:29:21",
                       resolved_at="2026-08-22 10:00:00")
    assert _fill_latency_minutes(cancelled)[0] == "—"
    assert _fill_latency_minutes(_offer())[0] == "—"      # pending


def test_clock_skew_is_refused_not_negative(app):
    from gui.widgets.order_panel import _fill_latency_minutes

    skew = _offer(status="filled",
                  created_at="2026-08-22 09:29:21",
                  resolved_at="2026-08-22 09:00:00")
    assert _fill_latency_minutes(skew)[0] == "—"


def test_the_cancel_button_lives_in_the_new_last_column(app):
    from PySide6.QtWidgets import QPushButton

    panel = _panel(app)
    panel.update_offers([_offer()])
    try:
        assert isinstance(panel._table.cellWidget(0, 11), QPushButton), (
            "cancel button did not move with the inserted column"
        )
        assert panel._table.cellWidget(0, 10) is None
    finally:
        panel.hide()


def test_fill_minutes_sort_numerically_in_both_directions(app):
    """The default comparator sorts DisplayRole strings: '9.0' above
    '100.0', and em dashes floating above real values descending."""
    from PySide6.QtCore import Qt

    panel = _panel(app)
    panel.set_current_block(9_184_100)
    panel.update_offers([
        _offer(offer_id="0xaaa", status="filled",
               created_at="2026-08-22 09:00:00",
               resolved_at="2026-08-22 09:09:00"),      # 9.0 min
        _offer(offer_id="0xbbb", status="filled",
               created_at="2026-08-22 09:00:00",
               resolved_at="2026-08-22 10:40:00"),      # 100.0 min
        _offer(offer_id="0xccc"),                        # pending: em dash
    ])
    table = panel._table
    try:
        table.sortItems(10, Qt.SortOrder.DescendingOrder)
        col = [table.item(r, 10).text() for r in range(3)]
        assert col == ["100.0", "9.0", "—"], f"descending: {col}"
        table.sortItems(10, Qt.SortOrder.AscendingOrder)
        col = [table.item(r, 10).text() for r in range(3)]
        assert col == ["—", "9.0", "100.0"], f"ascending: {col}"
    finally:
        panel.hide()


def test_fill_minutes_are_right_aligned_like_the_other_numerics(app):
    from PySide6.QtCore import Qt

    panel = _panel(app)
    panel.update_offers([_offer(status="filled",
                                created_at="2026-08-22 09:00:00",
                                resolved_at="2026-08-22 09:09:00")])
    try:
        flags = panel._table.item(0, 10).textAlignment()
        assert flags & Qt.AlignmentFlag.AlignRight, "Fill (min) left-aligned"
    finally:
        panel.hide()
