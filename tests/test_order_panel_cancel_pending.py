"""[S14 2026-09-13] offer_log 'cancel_pending' rows in the Orders panel.

An accepted cancel is not a cancel that happened: the row stays
'cancel_pending' until the wallet reports it terminal, and the offer can still
be taken meanwhile.  The panel shows such a row as Cancelling with its cancel
control disabled, ages it against the tip, filters it under "Cancelling",
keeps its wording apart from the cancel-all latch, and still lets Cancel All
run when only such rows remain -- the wallet-wide cancel re-fires a
PENDING_CANCEL trade.
"""

from __future__ import annotations

import os
import sys
import types

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    instance.setStyle("Fusion")
    yield instance


def _offer(**over):
    row = {"offer_id": "0x17840f2180f0ce45", "pair_name": "XCH/BYC", "side": "bid",
           "price_mojos": 1_486_000_000_000, "size_mojos": 1_000_000_000_000,
           "tier": 0, "status": "cancel_pending", "resolved_at": "",
           "created_block": 9_224_185, "resolved_block": 0,
           "created_at": "2026-08-30 23:39:00", "fee_mojos": 0}
    row.update(over)
    return row


def _panel(app):
    """A SHOWN panel: update_offers defers rendering while hidden."""
    from gui.widgets.order_panel import OrderPanel
    panel = OrderPanel()
    panel.show()
    QApplication.processEvents()
    return panel


def test_cancel_pending_row_renders_cancelling_and_disables_cancel(app):
    from PySide6.QtWidgets import QPushButton

    panel = _panel(app)
    panel.update_offers([_offer()])
    try:
        assert panel._table.item(0, 6).text() == "Cancelling"
        button = panel._table.cellWidget(0, 11)
        assert isinstance(button, QPushButton)
        assert button.text() == "Cancelling..."
        assert button.isEnabled() is False, (
            "a second cancel of a submitted cancel only pays a second fee"
        )
    finally:
        panel.hide()


def test_cancelling_filter_queries_cancel_pending(app):
    panel = _panel(app)
    statuses: list[str] = []
    panel.offers_query_requested.connect(
        lambda _pair, status, _limit: statuses.append(status))
    try:
        panel._combo_status.setCurrentText("Cancelling")
        assert statuses and statuses[-1] == "cancel_pending"
    finally:
        panel.hide()


def test_cancelling_filter_keeps_only_cancel_pending_rows(app):
    panel = _panel(app)
    panel.update_offers([_offer(offer_id="0x111"),
                         _offer(offer_id="0x222", status="pending")])
    try:
        panel._combo_status.setCurrentText("Cancelling")
        assert panel._table.rowCount() == 1
        assert panel._table.item(0, 6).text() == "Cancelling"
    finally:
        panel.hide()


def test_cancel_pending_ages_against_the_tip(app):
    panel = _panel(app)
    panel.set_current_block(9_224_285)
    panel.update_offers([_offer()])
    try:
        assert panel._table.item(0, 9).text() == "100", (
            "a cancel_pending offer is unresolved: it ages against the tip"
        )
    finally:
        panel.hide()


def test_summary_counts_cancel_pending_without_the_latch_wording(app):
    panel = _panel(app)
    try:
        panel.update_offer_summary({"total": 9, "pending": 2, "cancel_pending": 3,
                                    "filled": 4, "cancelled": 0, "expired": 0,
                                    "locked_mojos": 5_000_000_000_000})
        label = panel._lbl_pending.text()
        assert "Pending: 2" in label
        assert "Cancel pending: 3" in label
        assert not label.startswith("Cancelling"), (
            "'Cancelling: N' is the cancel-all latch's wording"
        )
    finally:
        panel.hide()


def test_cancel_all_is_offered_when_only_cancel_pending_offers_rest(app, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    panel = _panel(app)
    refusals: list[tuple] = []
    monkeypatch.setattr(QMessageBox, "information",
                        staticmethod(lambda *a, **k: refusals.append(a)))
    monkeypatch.setattr(QMessageBox, "warning",
                        staticmethod(lambda *a, **k: QMessageBox.StandardButton.Yes))
    requested: list[bool] = []
    panel.cancel_all_requested.connect(lambda: requested.append(True))
    try:
        panel.update_offer_summary({"total": 3, "pending": 0, "cancel_pending": 3,
                                    "filled": 0, "cancelled": 0, "expired": 0,
                                    "locked_mojos": 0})
        panel._on_cancel_all()
        assert not refusals, "Cancel All refused while three offers are still takeable"
        assert requested == [True]
    finally:
        panel.set_cancel_all_pending(False)
        panel.hide()


def test_own_order_highlight_includes_cancel_pending(app):
    from gui.widgets.main_window import MainWindow

    class _Book:
        def __init__(self):
            self.own = None

        def set_own_orders(self, orders):
            self.own = orders

    window = types.SimpleNamespace(_order_book=_Book())
    MainWindow._on_offers_for_order_book(window, [
        {"status": "pending"}, {"status": "cancel_pending"}, {"status": "cancelled"},
    ])
    assert [o["status"] for o in window._order_book.own] == ["pending", "cancel_pending"]
