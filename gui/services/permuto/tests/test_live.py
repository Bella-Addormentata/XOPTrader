"""The live quoting session.

The three properties that matter are the ones the switch promises: stopping
cancels the book, an unacknowledged cancel does NOT read as empty, and a tick
that throws does not end a session meant to run for 102 unattended hours.
"""

from __future__ import annotations

import pytest

pytest.importorskip("PySide6")

from PySide6.QtCore import QDeadlineTimer  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.services.permuto.live import MARKETS, PermutoLive  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    return QApplication.instance() or QApplication([])


class _Client:
    """Records what the session did to the venue."""

    def __init__(self, cancel_raises: bool = False):
        self.cancels = []
        self.cancel_raises = cancel_raises

    def cancel_all(self, now_s, markets=None):
        self.cancels.append(markets)
        if self.cancel_raises:
            raise RuntimeError("venue refused the cancel")
        return {}


class _Runner:
    def __init__(self, raises: bool = False):
        self.ticks = 0
        self.raises = raises

    def tick(self, now_s, oracles, flags):
        self.ticks += 1
        if self.raises:
            raise RuntimeError("tick exploded")
        return type("R", (), {"action": "quote", "reason": "", "ok": True,
                              "markets": {}, "error": ""})()


def _live(qapp, *, cancel_raises=False, tick_raises=False):
    live = PermutoLive.__new__(PermutoLive)
    from PySide6.QtCore import QObject
    QObject.__init__(live)
    live._identity = object()
    live._markets = list(MARKETS)
    live._client = _Client(cancel_raises=cancel_raises)
    live._runner = _Runner(raises=tick_raises)
    live._venue_state = lambda: {"oracles": {m: 0.07 for m in MARKETS},
                                 "flags": {"trading_paused": False}}
    live._thread = None
    live._worker = None
    live._book_empty = True
    return live


def _settle(live, qapp, timeout_ms=8000):
    deadline = QDeadlineTimer(timeout_ms)
    while live.is_running() and not deadline.hasExpired():
        qapp.processEvents()
    qapp.processEvents()


# --------------------------------------------------------------------------- #
# Off means flat
# --------------------------------------------------------------------------- #

def test_stopping_cancels_the_book(qapp):
    live = _live(qapp)
    live.start()
    qapp.processEvents()
    live.stop()
    _settle(live, qapp)

    assert live._client.cancels, "the session ended without cancelling"
    assert live.book_is_empty()


def test_a_running_session_never_claims_an_empty_book(qapp):
    """Starting puts us in the market; the switch must not read OFF."""
    live = _live(qapp)
    live.start()
    qapp.processEvents()
    assert not live.book_is_empty()
    live.stop()
    _settle(live, qapp)


def test_a_failed_cancel_leaves_the_book_reported_as_live(qapp):
    """The cancel is what makes OFF true. If it fails, OFF is a lie.

    The switch keeps showing STOPPING, which is the honest state: orders may
    still be resting and nothing has confirmed otherwise.
    """
    live = _live(qapp, cancel_raises=True)
    reasons = []
    live.stopped.connect(reasons.append)
    live.start()
    qapp.processEvents()
    live.stop()
    _settle(live, qapp)

    assert not live.book_is_empty()
    assert reasons and "FAILED" in reasons[0]


def test_the_cancel_still_runs_when_a_tick_is_throwing(qapp):
    """Whatever happened in the loop, the book gets retracted."""
    live = _live(qapp, tick_raises=True)
    live.start()
    qapp.processEvents()
    live.stop()
    _settle(live, qapp)
    assert live._client.cancels


# --------------------------------------------------------------------------- #
# The loop outlives its own failures
# --------------------------------------------------------------------------- #

def test_a_throwing_tick_does_not_end_the_session(qapp):
    """~102 unattended hours; one transient must not stop the loop."""
    live = _live(qapp, tick_raises=True)
    errors = []
    live.ticked.connect(lambda r: errors.append(getattr(r, "ok", True)))
    live.start()

    deadline = QDeadlineTimer(3000)
    while len(errors) < 2 and not deadline.hasExpired():
        qapp.processEvents()

    assert live.is_running(), "the session died on a tick failure"
    assert errors and errors[0] is False
    live.stop()
    _settle(live, qapp)


def test_join_is_safe_when_nothing_is_running(qapp):
    live = _live(qapp)
    live.join()          # must not raise or hang
    assert not live.is_running()


def test_start_is_idempotent(qapp):
    live = _live(qapp)
    live.start()
    first = live._thread
    live.start()
    assert live._thread is first
    live.stop()
    _settle(live, qapp)


# --------------------------------------------------------------------------- #
# Market symbols
# --------------------------------------------------------------------------- #

def test_markets_are_symbols_not_oracle_tickers():
    """Order routes answer HTTP 400 on every leg for the ticker form."""
    assert all(m.endswith("-PERP") for m in MARKETS)
    assert "QQQ-VOL" not in MARKETS
