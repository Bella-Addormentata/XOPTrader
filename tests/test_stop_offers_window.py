"""[S74 2026-09-20] The main window asks before it stops the engine -- or does not.

Drives the real ``MainWindow`` with a bridge double and a patched prompt; no
engine is spawned and no modal dialog is ever exec()'d. What is pinned:

* Stop Trading and a window close both ask, with the bridge's live inputs;
* "Don't stop" calls the stop off -- and on a close, keeps the window open;
* a close nobody at the machine started (OS session end, a signal) shows NO
  prompt and sends no policy;
* with no engine of ours running there is nothing to ask.

The decision table itself is tests/test_stop_offers.py; this file is the
wiring between it and the window.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402

pytest.importorskip("PySide6")

from PySide6.QtCore import QCoreApplication, QEvent  # noqa: E402
from PySide6.QtGui import QCloseEvent  # noqa: E402
from PySide6.QtWidgets import QApplication, QMessageBox  # noqa: E402

from gui import stop_offers  # noqa: E402
from gui.stop_offers import RestingSummary, StopChoice  # noqa: E402

SENTINEL = object()


class BridgeDouble:
    """Only what the stop paths touch."""

    def __init__(self, *, running=True, keep_supported=True, default="cancel"):
        self.engine_running_locally = running
        self.engine_supports_keep_offers = keep_supported
        self.stop_offers_default = default
        self.summary = RestingSummary(resting=3, per_pair=(("XCH/DBX", 3),))
        self.stops = []
        self.starts = 0
        self.close_policy = SENTINEL

    def resting_offers_summary(self):
        return self.summary

    def stop_engine(self, offers_policy=SENTINEL):
        self.stops.append(offers_policy)

    def start_engine(self):
        self.starts += 1

    def set_close_offers_policy(self, policy):
        self.close_policy = policy


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    yield instance


def _destroy(widget, app) -> None:
    """Really delete *widget*, now.

    ``deleteLater()`` alone never runs in a test: no event loop is running, and
    ``processEvents()`` does not deliver DeferredDelete. A leaked top-level
    widget is not harmless -- every later ``app.setStyleSheet()``
    (tests/test_ui_sizing.py applies about twenty) re-polishes every widget of
    every window still alive. Measured: that file takes 0.2 s alone, 190 s
    after the six windows the smoke tests leave behind, and did not finish in
    ten minutes after eleven more.
    """
    widget.deleteLater()
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    app.processEvents()


@pytest.fixture(scope="module")
def shared_window(app):
    """ONE MainWindow for the whole module, really destroyed at the end."""
    from gui.widgets.main_window import MainWindow

    w = MainWindow()
    yield w
    w._bridge = None          # a teardown close must never prompt
    w.close()
    _destroy(w, app)


@pytest.fixture
def window(shared_window, monkeypatch):
    stop_offers.reset_noninteractive_quit()
    # Any OTHER modal box would hang an offscreen run instead of failing it.
    monkeypatch.setattr(QMessageBox, "question", staticmethod(
        lambda *a, **k: (_ for _ in ()).throw(
            AssertionError("the old yes/no stop confirmation was shown"))))
    shared_window._bridge = None
    shared_window._bot_running = False
    yield shared_window
    stop_offers.reset_noninteractive_quit()
    shared_window._bridge = None


def _prompt_returns(monkeypatch, choice):
    """Patch the modal prompt; return the list its calls are recorded in."""
    calls = []

    def fake(parent, summary, *, default_policy, keep_supported, closing):
        calls.append(dict(summary=summary, default_policy=default_policy,
                          keep_supported=keep_supported, closing=closing))
        return choice

    import gui.widgets.stop_engine_dialog as dlg
    monkeypatch.setattr(dlg, "ask_stop_offers", fake)
    return calls


def _no_prompt(monkeypatch):
    """A prompt that must not be shown. It RECORDS the call and answers Cancel
    rather than raising: decide_stop swallows a raising prompt by design (a
    broken dialog must not trap a close) and proceeds with no policy -- the very
    outcome these tests expect -- so a raising double stayed green with the
    "not interactive" guard deleted (mutation check, 2026-09-20). Assert the
    returned list is empty."""
    calls = _prompt_returns(monkeypatch, StopChoice.CANCEL)
    return calls


# --------------------------------------------------------------------------- #
# Stop Trading
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize(("choice", "policy"), [
    (StopChoice.KEEP, "keep"), (StopChoice.CANCEL, "cancel")])
def test_stop_trading_asks_and_sends_the_answer(window, monkeypatch, choice, policy):
    bridge = BridgeDouble(default="keep", keep_supported=True)
    window._bridge = bridge
    window._bot_running = True
    calls = _prompt_returns(monkeypatch, choice)

    window._on_start_stop()

    assert bridge.stops == [policy]
    assert window._bot_running is False
    assert calls == [dict(summary=bridge.summary, default_policy="keep",
                          keep_supported=True, closing=False)], (
        "the prompt must get the bridge's live count, default and capability")


def test_dont_stop_leaves_the_engine_and_the_button_alone(window, monkeypatch):
    bridge = BridgeDouble()
    window._bridge = bridge
    window._bot_running = True
    _prompt_returns(monkeypatch, StopChoice.DONT_STOP)

    window._on_start_stop()

    assert bridge.stops == []
    assert window._bot_running is True


def test_the_prompt_remembers_nothing_between_stops(window, monkeypatch):
    """Two stops, two prompts, both preselecting the CONFIG default -- not the
    previous answer."""
    bridge = BridgeDouble(default="cancel")
    window._bridge = bridge
    calls = _prompt_returns(monkeypatch, StopChoice.KEEP)
    for _ in range(2):
        window._bot_running = True
        window._on_start_stop()
    assert bridge.stops == ["keep", "keep"]
    assert [c["default_policy"] for c in calls] == ["cancel", "cancel"]


# --------------------------------------------------------------------------- #
# Closing the window
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize(("choice", "policy"), [
    (StopChoice.KEEP, "keep"), (StopChoice.CANCEL, "cancel")])
def test_a_close_asks_and_hands_the_answer_to_the_bridge(window, monkeypatch, choice, policy):
    bridge = BridgeDouble()
    window._bridge = bridge
    calls = _prompt_returns(monkeypatch, choice)

    event = QCloseEvent()
    window.closeEvent(event)

    assert event.isAccepted()
    assert bridge.close_policy == policy
    assert [c["closing"] for c in calls] == [True]
    assert bridge.stops == [], "the stop itself belongs to aboutToQuit, not to closeEvent"


def test_dont_stop_keeps_the_window_open(window, monkeypatch):
    bridge = BridgeDouble()
    window._bridge = bridge
    _prompt_returns(monkeypatch, StopChoice.DONT_STOP)

    event = QCloseEvent()
    window.closeEvent(event)

    assert not event.isAccepted()
    assert bridge.close_policy is SENTINEL, "a refused close must not leave a policy behind"


@pytest.mark.parametrize("reason", ["OS session end", "SIGTERM"])
def test_a_close_nobody_started_shows_no_prompt_and_sends_no_policy(
        window, monkeypatch, reason):
    bridge = BridgeDouble(default="keep")
    window._bridge = bridge
    calls = _no_prompt(monkeypatch)
    stop_offers.mark_noninteractive_quit(reason)

    event = QCloseEvent()
    window.closeEvent(event)

    assert calls == [], "a stop prompt was shown where nobody could answer it"
    assert event.isAccepted()
    assert bridge.close_policy is None, (
        "no policy: the ENGINE's engine.shutdown_offers decides, not the GUI's copy of it")


def test_a_close_with_no_engine_of_ours_asks_nothing(window, monkeypatch):
    bridge = BridgeDouble(running=False)
    window._bridge = bridge
    calls = _no_prompt(monkeypatch)

    event = QCloseEvent()
    window.closeEvent(event)

    assert calls == [], "a prompt was shown with no engine of ours to stop"
    assert event.isAccepted()
    assert bridge.close_policy is None


def test_a_close_with_no_bridge_at_all_still_closes(window, monkeypatch):
    window._bridge = None
    calls = _no_prompt(monkeypatch)
    event = QCloseEvent()
    window.closeEvent(event)
    assert calls == []
    assert event.isAccepted()
