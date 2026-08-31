"""The operator close button.

The Permuto page offered Create/Restore/Register/Check/Recover/Discard/
Start polling/Start quoting -- and nothing that could reduce a position. So
`risk.py`'s doctrine that crossing the spread to close is "an operator
decision" reserved a choice for a human the software gave no way to make.

These tests cover the part that can go wrong quietly: that the button shows
the operator the real plan before sending, that cancelling sends nothing,
and that a refusal is reported rather than swallowed.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication, QMessageBox  # noqa: E402

from gui.widgets.permuto import PermutoWidget  # noqa: E402


@pytest.fixture(scope="module")
def app():
    yield QApplication.instance() or QApplication(sys.argv)


@pytest.fixture()
def widget(app):
    w = PermutoWidget()
    yield w
    w.deleteLater()


def test_the_page_offers_a_close_control_at_all(widget):
    """The gap this closes: there was no such control."""
    assert sorted(widget._close_btns) == [0.25, 0.50, 1.00]


def test_an_empty_plan_sends_nothing_and_says_so(widget, monkeypatch):
    called = {}
    monkeypatch.setattr(widget, "_start_close_worker",
                        lambda *a, **k: called.setdefault("started", True))
    widget._on_close_planned(([], "Nothing to close -- no open positions."))
    assert "started" not in called, "sent a batch with nothing to close"
    assert "Nothing to close" in widget._close_note.text()


def test_cancelling_the_dialog_sends_nothing(widget, monkeypatch):
    monkeypatch.setattr(QMessageBox, "exec",
                        lambda self: QMessageBox.Cancel)
    started = {}
    monkeypatch.setattr(widget, "_start_close_worker",
                        lambda *a, **k: started.setdefault("yes", True))
    legs = [{"market": "QQQ-VOL-PERP", "side": "buy", "size": 100.0,
             "reduce_only": True}]
    widget._on_close_planned((legs, "  QQQ-VOL-PERP BUY 100"))
    assert "yes" not in started, "Cancel still sent the orders"
    assert "Cancelled" in widget._close_note.text()


def test_confirming_the_dialog_sends(widget, monkeypatch):
    monkeypatch.setattr(QMessageBox, "exec", lambda self: QMessageBox.Ok)
    started = {}
    monkeypatch.setattr(
        widget, "_start_close_worker",
        lambda frac, mode: started.update(frac=frac, mode=mode))
    widget._close_fraction = 0.5
    legs = [{"market": "QQQ-VOL-PERP", "side": "buy", "size": 100.0,
             "reduce_only": True}]
    widget._on_close_planned((legs, "  QQQ-VOL-PERP BUY 100"))
    assert started == {"frac": 0.5, "mode": "send"}


def test_a_venue_refusal_is_shown_not_swallowed(widget):
    widget._on_close_sent({"ok": False, "note": "insufficient margin"})
    assert "insufficient margin" in widget._close_note.text()
    assert "Failed" in widget._close_note.text()


def test_a_worker_exception_is_shown(widget):
    widget._on_close_failed("session expired")
    assert "session expired" in widget._close_note.text()


def test_the_buttons_come_back_after_a_failure(widget):
    """A dead button after one bad press would strand the operator in
    exactly the situation the control exists for."""
    widget._set_close_enabled(False)
    widget._on_close_failed("boom")
    assert all(b.isEnabled() for b in widget._close_btns.values())


def test_a_partial_fill_is_reported_as_such(widget):
    widget._on_close_sent({"ok": True, "sent": 2, "note": ""})
    text = widget._close_note.text()
    assert "part-fill" in text and "does not retry" in text, \
        "the operator must know this is one shot, not a managed close"
