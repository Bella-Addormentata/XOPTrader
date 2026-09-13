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
        # Missing stays at the BOTTOM in either direction: the item reads
        # the live sort indicator, since a static -inf sentinel would float
        # em dashes to the top of every ascending sort.
        assert col == ["9.0", "100.0", "—"], f"ascending: {col}"
    finally:
        panel.hide()


def test_cancel_all_pending_displays_cancelling_status_and_disables_button(app):
    from PySide6.QtWidgets import QPushButton

    panel = _panel(app)
    panel.update_offers([_offer(offer_id="0x111", status="pending")])
    try:
        # Initially pending with active cancel button
        assert panel._table.item(0, 6).text() == "Pending"
        btn = panel._table.cellWidget(0, 11)
        assert isinstance(btn, QPushButton)
        assert btn.text() == "Cancel"
        assert btn.isEnabled() is True

        # Activate cancel-all pending
        panel.set_cancel_all_pending(True)
        assert panel._table.item(0, 6).text() == "Cancelling"
        btn = panel._table.cellWidget(0, 11)
        assert isinstance(btn, QPushButton)
        assert btn.text() == "Cancelling..."
        assert btn.isEnabled() is False

        # Reset cancel-all pending
        panel.set_cancel_all_pending(False)
        assert panel._table.item(0, 6).text() == "Pending"
        btn = panel._table.cellWidget(0, 11)
        assert btn.text() == "Cancel"
        assert btn.isEnabled() is True
    finally:
        panel.hide()


# [S33 2026-09-05] test_single_cancelling_offer_displays_cancelling_status
# is gone with the mechanism it exercised.  It drove the per-offer
# "Cancelling" badge through set_cancelling_offers(), which c20 left with no
# production caller once the optimistic click-latch was removed; the setter,
# _cancelling_offer_ids, and the three display paths that read it have all
# been deleted.  Cancel-all's badge is still covered above.


def test_pending_filter_retains_cancelling_offers(app):
    panel = _panel(app)
    panel.update_offers([
        _offer(offer_id="0x111", status="pending"),
        _offer(offer_id="0x222", status="filled", resolved_block=9_184_050),
    ])
    try:
        panel._combo_status.setCurrentText("Pending")
        panel.set_cancel_all_pending(True)
        # Filter is "Pending", but cancelling offers should still be visible
        assert panel._table.rowCount() == 1
        assert panel._table.item(0, 6).text() == "Cancelling"
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


def test_a_terminal_offer_without_a_resolution_block_shows_unknown_age(app):
    """The engine persists resolved_block=0 on some cancel paths.  Aging
    such a row against the live tip forever is the original bug in a
    subtler form; unknown renders as an em dash."""
    panel = _panel(app)
    panel.set_current_block(9_999_999)
    panel.update_offers([_offer(status="cancelled", resolved_block=0,
                                resolved_at="2026-08-22 10:00:00")])
    try:
        assert panel._table.item(0, 9).text() == "—"
    finally:
        panel.hide()


def test_age_sorts_numerically(app):
    from PySide6.QtCore import Qt

    panel = _panel(app)
    panel.set_current_block(9_184_500)
    panel.update_offers([
        _offer(offer_id="0x1", created_block=9_184_491),   # age 9
        _offer(offer_id="0x2", created_block=9_184_400),   # age 100
    ])
    try:
        panel._table.sortItems(9, Qt.SortOrder.DescendingOrder)
        ages = [panel._table.item(r, 9).text() for r in range(2)]
        assert ages == ["100", "9"], f"lexicographic sort: {ages}"
    finally:
        panel.hide()


def test_no_tip_yet_means_unknown_not_zero(app):
    """Before the first health payload the tip is 0; a numeric 0 would be
    indistinguishable from a genuinely new offer."""
    panel = _panel(app)          # set_current_block never called
    panel.update_offers([_offer()])
    try:
        assert panel._table.item(0, 9).text() == "—"
    finally:
        panel.hide()


def test_single_cancel_click_does_not_latch_cancelling_state(app, monkeypatch):
    """[S33 2026-09-05] Confirming a single-offer cancel must NOT mark the
    row "Cancelling..." optimistically.  The signal lands on
    EngineBridge.cancel_offer, a Phase-1 stub that submits nothing and only
    emits an error, and nothing on that path ever unwinds the latch -- so
    latching greys out the row's cancel button (and hides the context-menu
    Cancel action, which is gated on the DISPLAYED status text) for a still
    resting offer, permanently."""
    from PySide6.QtWidgets import QMessageBox, QPushButton

    panel = _panel(app)
    panel.update_offers([_offer(offer_id="0x111", status="pending")])
    emitted: list[str] = []
    panel.cancel_offer_requested.connect(emitted.append)
    monkeypatch.setattr(
        QMessageBox, "question",
        staticmethod(lambda *a, **k: QMessageBox.StandardButton.Yes),
    )
    try:
        panel._on_cancel_single("0x111")

        # The request itself is still made ...
        assert emitted == ["0x111"]
        # ... but no display state is latched off it.  (The per-offer latch
        # set itself is gone; these assert the observable consequence, which
        # is what any reintroduced latch would break.)
        assert panel._table.item(0, 6).text() == "Pending", (
            "the badge must not claim 'Cancelling' for an unsubmitted request"
        )
        btn = panel._table.cellWidget(0, 11)
        assert isinstance(btn, QPushButton)
        assert btn.text() == "Cancel"
        assert btn.isEnabled() is True, (
            "the row's retry control must survive -- disabling it is what "
            "makes the false latch unrecoverable"
        )
    finally:
        panel.hide()


def test_declining_the_single_cancel_confirmation_emits_nothing(app, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    panel = _panel(app)
    panel.update_offers([_offer(offer_id="0x111", status="pending")])
    emitted: list[str] = []
    panel.cancel_offer_requested.connect(emitted.append)
    monkeypatch.setattr(
        QMessageBox, "question",
        staticmethod(lambda *a, **k: QMessageBox.StandardButton.No),
    )
    try:
        panel._on_cancel_single("0x111")
        assert emitted == []
        assert panel._table.item(0, 6).text() == "Pending"
    finally:
        panel.hide()
