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
        lambda frac, mode, **kw: started.update(
            frac=frac, mode=mode, legs=kw.get("approved_legs")))
    widget._close_fraction = 0.5
    legs = [{"market": "QQQ-VOL-PERP", "side": "buy", "size": 100.0,
             "reduce_only": True}]
    widget._on_close_planned((legs, "  QQQ-VOL-PERP BUY 100"))
    assert started["frac"] == 0.5
    assert started["mode"] == "send"
    assert started["legs"] == legs, \
        "the confirmed legs must be forwarded so send cannot exceed the plan"


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


# --------------------------------------------------------------------------- #
# Session contention -- two clients for one identity fight over the token
# --------------------------------------------------------------------------- #

def test_the_close_is_refused_while_quoting_is_live(widget):
    """[review] The close worker opens its OWN venue session.

    PermutoClient documents that concurrent renewals install different
    tokens which invalidate each other, so pressing Close while the loop
    runs can put both into alternating 401/reauth -- at exactly the moment
    the operator is trying to get out of a position.
    """
    widget.set_quoting_live(True)
    assert not any(b.isEnabled() for b in widget._close_btns.values())
    assert "Stop quoting first" in widget._close_note.text()


def test_stopping_quoting_returns_the_close_control(widget):
    widget.set_quoting_live(True)
    widget.set_quoting_live(False)
    assert all(b.isEnabled() for b in widget._close_btns.values())
    assert "Stop quoting first" not in widget._close_note.text()


def test_the_guard_outranks_a_plain_re_enable(widget):
    """A failure handler calling _set_close_enabled(True) must not hand the
    buttons back while the loop still owns the session."""
    widget.set_quoting_live(True)
    widget._on_close_failed("boom")          # re-enables on the normal path
    assert not any(b.isEnabled() for b in widget._close_btns.values()),         "a failure re-enabled the close control during live quoting"


# --------------------------------------------------------------------------- #
# Thread supersession -- a late `finished` must not disown a running thread
# --------------------------------------------------------------------------- #

class _FakeThread:
    """Stands in for a QThread; records whether it was told to delete."""

    def __init__(self, name):
        self.name = name
        self.deleted = False

    def deleteLater(self):
        self.deleted = True


def test_a_late_finish_does_not_clear_the_running_thread(widget, monkeypatch):
    """[review] A fast confirmation starts the SEND thread before the PLAN
    thread's queued `finished` arrives.

    The callback used to clear whatever thread was current, so that late
    signal disowned the running send thread -- losing close_in_flight()
    protection, so quoting could start beside a live close, and risking
    Qt's fatal "QThread: Destroyed while thread is still running" on the
    one control an operator reaches for in an emergency.
    """
    stale, running = _FakeThread("plan"), _FakeThread("send")
    widget._close_thread = running
    widget._close_worker = object()
    monkeypatch.setattr(widget, "sender", lambda: stale)

    widget._on_close_thread_finished()

    assert widget._close_thread is running,         "a late finish from a superseded thread disowned the running one"
    assert widget.close_in_flight() is True, "lost in-flight protection"
    # Deletion is wired to each thread's own finished signal now, so the
    # callback must not delete anything itself -- doing so would schedule a
    # second deletion of the same object.
    assert running.deleted is False, "deleted a thread that is still running"


def test_the_current_thread_finishing_does_clear_state(widget, monkeypatch):
    running = _FakeThread("send")
    widget._close_thread = running
    widget._close_worker = object()
    monkeypatch.setattr(widget, "sender", lambda: running)

    widget._on_close_thread_finished()

    assert widget._close_thread is None
    assert widget.close_in_flight() is False


def test_a_partial_failure_still_reports_what_went_out(widget):
    """[review] Legs that already filled moved the position.

    Reporting only "Failed: ..." let an operator believe nothing happened
    while some legs were already live -- on a control whose whole purpose
    is knowing your exposure.
    """
    widget._on_close_sent({"ok": False, "sent": 2,
                           "note": "partial: TSLA-VOL-PERP: margin"})
    text = widget._close_note.text()
    assert "2 leg(s)" in text, text
    assert "does not retry" in text, text
    assert "margin" in text, text


def test_a_success_with_skipped_legs_still_reports_the_sent_count(widget):
    """[review] `note or "%d leg(s) sent"` dropped the count.

    `note` is non-empty whenever ANY leg was skipped, so a partial success
    showed only the skip detail: the operator saw "skipped B(already
    flat)" with no indication that a leg had in fact gone out. On a close
    control the count is the whole message -- it is what tells them
    whether they still have exposure.
    """
    widget._on_close_sent({"ok": True, "sent": 2,
                           "note": "skipped B(already flat)",
                           "skipped": ["B(already flat)"]})
    text = widget._close_note.text()
    assert "2 leg(s) sent" in text, text
    assert "already flat" in text, text


def test_a_total_failure_does_not_claim_legs_went_out(widget):
    widget._on_close_sent({"ok": False, "sent": 0, "note": "session expired"})
    text = widget._close_note.text()
    assert text.startswith("Failed:"), text
    assert "leg(s) went out" not in text


# --------------------------------------------------------------------------- #
# [review] The INVERSE guard, pinned at the MainWindow wiring
# --------------------------------------------------------------------------- #

class _FakePage:
    """Minimal stand-in for the Permuto page."""

    def __init__(self, in_flight):
        self._in_flight = in_flight
        self.told = []

    def close_in_flight(self):
        return self._in_flight

    def set_quoting_live(self, live):
        self.told.append(live)


def _wire(monkeypatch, in_flight):
    """A MainWindow-shaped object carrying only what the guard touches."""
    from gui.widgets.main_window import MainWindow

    page = _FakePage(in_flight)
    mw = MainWindow.__new__(MainWindow)          # no Qt construction
    mw._permuto_widget = page
    mw._permuto_runner = None
    mw._permuto_desired_on = False
    mw._unwrap = staticmethod(lambda w: w)
    refusals, built = [], []
    mw._on_switch_refused = lambda msg: refusals.append(msg)
    mw._refresh_venue_switches = lambda: None
    mw._make_permuto_live = lambda: built.append(True)
    # may_turn_on() runs first and must ALLOW, or the test would pass for
    # the wrong reason -- refused by the ordinary gate rather than by the
    # in-flight guard under test.
    mw._gather_permuto = lambda: {}
    monkeypatch.setattr("gui.services.venue_control.may_turn_on",
                        lambda _s: (True, ""))
    return mw, page, refusals, built


def test_quoting_is_refused_while_a_close_is_in_flight(monkeypatch):
    """[review] The guard existed but nothing exercised it.

    Starting the runner while a close worker is live opens a SECOND venue
    session for the same identity, and the two invalidate each other's
    tokens -- with the close being the one that must not lose that fight.
    The widget tests only covered the other direction.
    """
    from gui.widgets.main_window import MainWindow

    mw, page, refusals, built = _wire(monkeypatch, in_flight=True)
    MainWindow._on_permuto_toggle(mw, True)

    assert not built, "a runner was constructed during an in-flight close"
    assert mw._permuto_runner is None
    assert mw._permuto_desired_on is False
    assert refusals and "close is in flight" in refusals[0], refusals


def test_quoting_starts_normally_when_no_close_is_running(monkeypatch):
    """And the guard must not be a permanent block."""
    from gui.widgets.main_window import MainWindow

    mw, page, refusals, built = _wire(monkeypatch, in_flight=False)
    try:
        MainWindow._on_permuto_toggle(mw, True)
    except Exception:
        # Construction goes further than this fake supports; what matters
        # is that the guard did NOT refuse before getting there.
        pass
    assert not [r for r in refusals if "close is in flight" in r], refusals
