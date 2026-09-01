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

    # [review] ASSERT IT GOT THERE. The absence of a refusal proves
    # nothing on its own -- the blanket except above swallows any
    # earlier regression, and a startup that died before reaching the
    # guard also records no refusal. Only `built` distinguishes "the
    # guard let it through" from "the guard was never consulted".
    assert built, (
        "startup never attempted runner construction, so this proves "
        "nothing about the close guard: refusals=%r" % (refusals,))
    assert not [r for r in refusals if "close is in flight" in r], refusals


def test_lot_sizes_reject_anything_not_finite_and_positive(monkeypatch):
    """[review] `lot == lot` rejects NaN and lets +inf through.

    An infinite lot is not a harmless oddity here: plan_close divides by
    it, so every size floors to zero, every market rounds out, and a fully
    loaded account renders as nothing to close -- on the control an
    operator reaches for to get out. Junk venue metadata must fall back to
    the published grid, not silently empty the plan.
    """
    from gui.widgets.permuto import _default_venue_snapshot

    monkeypatch.setattr(
        "gui.services.permuto.live._default_venue_state",
        lambda: {"oracles": {"QQQ-VOL-PERP": 0.07}, "flags": {"specs": {
            "GOOD-PERP": {"lot_size": 5.0},
            "INF-PERP": {"lot_size": float("inf")},
            "NAN-PERP": {"lot_size": float("nan")},
            "ZERO-PERP": {"lot_size": 0.0},
            "NEG-PERP": {"lot_size": -1.0},
            "TEXT-PERP": {"lot_size": "big"},
            "NULL-PERP": {"lot_size": None},
            "JUNK-PERP": "not a dict",
        }}})
    prices, lots = _default_venue_snapshot()
    assert lots == {"GOOD-PERP": 5.0}
    # ...and the same single read supplies the prices.
    assert prices == {"QQQ-VOL-PERP": 0.07}


def test_the_close_worker_cannot_outlive_its_join_budget():
    """[review] A live-order thread was terminable mid-flight.

    The client's default request timeout is 30s and _join() waited 10s
    before terminate(), so shutdown could kill the worker AFTER the venue
    had received an order and BEFORE the answer arrived -- leaving the
    operator with no record of whether their close went out. That is the
    one failure this control exists to prevent.

    The arithmetic, not just the constants: worst case is session +
    account + the fresh re-read + one order per market, each bounded by
    CLOSE_REQUEST_TIMEOUT_S, and the join must cover all of it.
    """
    from gui.widgets import permuto as permuto_mod
    from gui.services.permuto.live import MARKETS

    # [review] STATED HERE, not imported. The first version of this test
    # computed the worst case from the very constants that produce
    # CLOSE_JOIN_MS, so the two moved together and the assertion could
    # never fail -- reverting CLOSE_CALLS_PER_REQUEST to 1 left it green.
    # The protocol facts belong in the test, independently:
    #
    #   a cold ensure_session() is 2 calls (challenge, auth)
    #   every authenticated request can become 4 on the 401 path
    #     (request, challenge, re-auth, retry)
    #   send mode makes 1 fresh position read + one order per market
    session_calls = 2
    calls_per_request = 4
    worst_case_calls = session_calls + calls_per_request * (1 + len(MARKETS))
    worst_case_ms = (worst_case_calls
                     * permuto_mod.CLOSE_REQUEST_TIMEOUT_S * 1000)
    assert permuto_mod.CLOSE_JOIN_MS >= worst_case_ms, (
        "join budget %dms is under the %.0fms worst case (%d HTTP calls "
        "at %.1fs) -- a live order thread can still be terminated"
        % (permuto_mod.CLOSE_JOIN_MS, worst_case_ms, worst_case_calls,
           permuto_mod.CLOSE_REQUEST_TIMEOUT_S))


def test_the_close_client_is_built_with_the_bounded_timeout(monkeypatch):
    """The constant is worthless if the worker does not pass it."""
    from gui.widgets import permuto as permuto_mod

    seen = {}

    class _FakeClient:
        def __init__(self, identity, user_id="", timeout=None, **kw):
            seen["timeout"] = timeout

        def ensure_session(self, now_s):
            raise RuntimeError("stop here -- construction is the assertion")

    monkeypatch.setattr("gui.services.permuto.client.PermutoClient",
                        _FakeClient)
    worker = permuto_mod._CloseWorker(object(), 1.0, mode="plan")
    failures = []
    worker.failed.connect(lambda msg: failures.append(msg))
    worker.run()

    assert seen.get("timeout") == permuto_mod.CLOSE_REQUEST_TIMEOUT_S, (
        "the close worker used the venue default, not the bounded timeout")
    assert failures, "the fake should have surfaced its error"


def test_more_legs_than_the_budget_covers_are_refused(monkeypatch):
    """[review] The join budget is DERIVED from CLOSE_MAX_LEGS, so sending
    more than that silently invalidates the arithmetic that keeps
    terminate() away from a live order. Refuse instead."""
    from gui.widgets import permuto as permuto_mod

    legs = [{"market": "M%d" % i, "side": "buy", "size": 1.0,
             "reduce_only": True}
            for i in range(permuto_mod.CLOSE_MAX_LEGS + 1)]
    worker = permuto_mod._CloseWorker(object(), 1.0, mode="send",
                                      approved_legs=legs)
    refusals, sent = [], []
    worker.failed.connect(lambda m: refusals.append(m))
    worker.sent.connect(lambda r: sent.append(r))

    class _NeverCalled:
        def __init__(self, *a, **k):
            raise AssertionError("a client was built for a refused close")

    monkeypatch.setattr("gui.services.permuto.client.PermutoClient",
                        _NeverCalled)
    worker.run()

    assert not sent, "an over-budget close was sent anyway"
    assert refusals, "no refusal was surfaced at all"
    # Specific: the refusal must name BOTH counts, so a generic
    # exception message cannot satisfy it by containing a word.
    assert str(len(legs)) in refusals[0], refusals
    assert str(permuto_mod.CLOSE_MAX_LEGS) in refusals[0], refusals
    assert "refusing" in refusals[0].lower(), refusals


def test_a_live_order_thread_is_parked_never_terminated():
    """[review] The join budget cannot be a guarantee, so safety must not
    rest on it.

    urlopen(timeout=) bounds each socket OPERATION, not the request, so a
    response that keeps trickling outlasts any budget derived from it.
    terminate() is TerminateThread on Windows -- the frame dies without
    unwinding, mid-request -- and whatever the venue then does with that
    order is never recorded anywhere.

    So the live-order path must never reach terminate(), however long the
    wait was. Parking costs a leaked thread on a pathological shutdown,
    which is cheaper than losing the record of a live order.
    """
    from gui.widgets import permuto as permuto_mod

    class _StuckThread:
        def __init__(self):
            self.quit_called = False
            self.terminated = False
            self.parent_set_to = "never called"

        def quit(self):
            self.quit_called = True

        def wait(self, ms):
            return False            # never stops

        def terminate(self):
            self.terminated = True

        def setParent(self, parent):
            self.parent_set_to = parent

    before = len(permuto_mod._ORPHANED_LIVE_THREADS)
    stuck = _StuckThread()
    permuto_mod.PermutoWidget._join(stuck, "operator close", wait_ms=1,
                                    live_orders=True)
    assert stuck.quit_called
    assert not stuck.terminated, (
        "a thread that may hold a live order was terminated")
    assert len(permuto_mod._ORPHANED_LIVE_THREADS) == before + 1, (
        "the thread was neither terminated nor kept referenced -- Qt will "
        "abort when it is destroyed while running")
    # [review] AND DETACHED. The thread is a QObject child of the
    # widget (QThread(self)), so the parent destructor deletes it
    # whatever Python holds -- keeping a reference alone left the
    # destroyed-while-running abort exactly where it was.
    assert stuck.parent_set_to is None, (
        "the parked thread is still parented to the widget, so its "
        "parent will delete it mid-run: setParent -> %r"
        % (stuck.parent_set_to,))

    # A worker with no orders in flight is still terminated: leaking those
    # would be a slow resource leak for no safety gain.
    ordinary = _StuckThread()
    permuto_mod.PermutoWidget._join(ordinary, "market poll", wait_ms=1)
    assert ordinary.terminated, "an ordinary worker was leaked instead"
    assert len(permuto_mod._ORPHANED_LIVE_THREADS) == before + 1

    permuto_mod._ORPHANED_LIVE_THREADS[:] = \
        permuto_mod._ORPHANED_LIVE_THREADS[:before]


def test_an_empty_plan_is_not_logged_as_a_flat_account(widget):
    """[review] plan_close() also returns nothing when every position
    rounds below one lot -- and the summary says the exposure is still
    open. Asserting "no open positions" into the permanent record is the
    opposite of what happened."""
    summary = ("Nothing can be closed at this size -- every position rounds "
               "below one lot: B (0.25 below one lot of 1). The exposure is "
               "still open; try a larger fraction.")
    widget._on_close_planned(([], summary))
    # The ACTIVITY LOG, not the note. An earlier version of this test
    # fell back to _close_note when it could not find the log, and the
    # note is set to the summary either way -- so it passed against the
    # bug it was written for.
    logged = widget._activity.toPlainText()
    assert "no open positions" not in logged, logged
    assert "one lot" in logged, logged


def test_the_session_stays_reserved_while_the_operator_decides(widget,
                                                               monkeypatch):
    """[review] QMessageBox.exec() runs a NESTED EVENT LOOP.

    The plan worker finishes before the operator answers, so
    _close_thread goes None while the dialog is still on screen. The
    deferred startup timer could read that as permission, open a quoting
    session, and pressing OK would then start the send worker beside it
    -- two clients renewing the same identity, which is the exact failure
    close_in_flight() exists to prevent.
    """
    seen = {}

    def _fake_confirm(summary):
        # Exactly the moment the old guard went false: the worker is gone
        # and the operator has not answered yet.
        widget._close_thread = None
        seen["in_flight_during_dialog"] = widget.close_in_flight()
        return False            # cancel, so nothing is sent

    monkeypatch.setattr(widget, "_confirm_close", _fake_confirm)
    widget._on_close_planned(([{"market": "A", "side": "buy", "size": 1.0,
                                "reduce_only": True}], "one leg"))

    assert seen.get("in_flight_during_dialog") is True, (
        "the session was unreserved while the confirmation dialog was open")
    # ...and released afterwards, or the control locks itself out forever.
    assert widget.close_in_flight() is False


def test_quoting_going_live_during_confirmation_cancels_the_send(widget,
                                                                 monkeypatch):
    """The reservation stops a session being opened while we hold it; one
    that was ALREADY live when the dialog opened is still live now, so the
    send has to re-check rather than assume."""
    started = []
    monkeypatch.setattr(widget, "_confirm_close", lambda s: True)
    monkeypatch.setattr(widget, "_start_close_worker",
                        lambda *a, **k: started.append(a))
    widget._quoting_live = True

    widget._on_close_planned(([{"market": "A", "side": "buy", "size": 1.0,
                                "reduce_only": True}], "one leg"))

    assert not started, "a send was started while quoting was live"
    assert "Quoting started" in widget._close_note.text()


def test_an_unresolved_close_is_not_shown_as_failed(widget):
    """[review] The widget was the half of the tri-state I left undone.

    send_close() separates a refusal -- the position is untouched -- from
    an answer that never arrived, where the order MAY HAVE EXECUTED. The
    screen collapsed both into "Failed", which reads as "nothing
    happened" and invites the second press that doubles the close. The
    service-layer distinction is worth nothing if the UI erases it.
    """
    widget._on_close_sent({
        "ok": False, "sent": 0,
        "note": "UNRESOLVED -- no verdict for QQQ-VOL-PERP: timeout",
        "unknown": ["QQQ-VOL-PERP: timeout"], "partial": [], "skipped": []})
    text = widget._close_note.text()
    assert not text.startswith("Failed"), text
    assert "UNRESOLVED" in text, text
    assert "MAY HAVE EXECUTED" in text, text
    assert "Check the position" in text, text
    logged = widget._activity.toPlainText()
    assert "UNRESOLVED" in logged and "FAILED" not in logged, logged


def test_a_mixed_unresolved_close_reports_both_counts(widget):
    """[review] The dropped-count bug, rebuilt next door.

    Earlier in this PR the success path was fixed for exactly this --
    `note or "%d leg(s) sent"` swallowed the count whenever a leg was
    skipped. Adding the UNRESOLVED branch reintroduced it: with one
    leg acknowledged and another timed out, the operator saw the
    unverified count and was never told one leg had definitely gone
    out -- half the state they need to decide what to do next."""
    widget._on_close_sent({
        "ok": False, "sent": 1,
        "note": "UNRESOLVED -- no verdict for NVDA-VOL-PERP: timeout",
        "unknown": ["NVDA-VOL-PERP: timeout"],
        "partial": [], "skipped": []})
    text = widget._close_note.text()
    assert "1 leg(s) sent" in text, text
    assert "1 got no verdict" in text, text
    assert "MAY HAVE EXECUTED" in text, text


def test_a_plain_refusal_is_still_shown_as_failed(widget):
    """The other side of it: a refusal leaves the position exactly where
    it was, and must not be dressed up as uncertainty."""
    widget._on_close_sent({"ok": False, "sent": 0,
                           "note": "all 1 leg(s) refused: margin",
                           "unknown": [], "partial": [], "skipped": []})
    text = widget._close_note.text()
    assert text.startswith("Failed"), text
    assert "MAY HAVE EXECUTED" not in text, text
