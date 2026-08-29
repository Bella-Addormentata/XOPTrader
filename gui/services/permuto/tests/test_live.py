"""The live quoting session.

The three properties that matter are the ones the switch promises: stopping
cancels the book, an unacknowledged cancel does NOT read as empty, and a tick
that throws does not end a session meant to run for 102 unattended hours.
"""

from __future__ import annotations

import threading
import time

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


def test_join_returns_promptly_while_blocking_the_event_loop(qapp):
    """The real closeEvent path: join() BLOCKS, it does not pump events.

    Every other test here spins on processEvents(), which delivers the
    queued worker.stopped -> thread.quit for free. closeEvent cannot: it
    calls join(), which blocks the GUI thread inside wait(), so a quit that
    depends on that event loop can never arrive and a healthy stop waits out
    the whole timeout before terminating the thread.

    So this one deliberately never pumps. A short timeout is the assertion:
    if join() is relying on a queued quit it will spend all 3000 ms here.
    """
    live = _live(qapp)
    live.start()
    qapp.processEvents()          # let the worker actually start

    started = time.monotonic()
    live.join(timeout_ms=3000)    # no processEvents() anywhere in here
    elapsed = time.monotonic() - started

    assert elapsed < 2.0, (
        "join() took %.2fs -- it is waiting on a quit that needs the event "
        "loop it is blocking" % elapsed)
    assert not live.is_running()
    assert live._client.cancels, "off must still mean flat on this path"


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


# --------------------------------------------------------------------------- #
# [review] A partial /info/meta must not read as a live session
# --------------------------------------------------------------------------- #

def _venue_state(meta, oracle=None):
    """Drive _default_venue_state with a fake transport."""
    import gui.services.permuto.auth as auth_mod
    from gui.services.permuto import live as live_mod

    prices = oracle if oracle is not None else {
        m.replace("-PERP", ""): 0.07 for m in MARKETS}
    real = auth_mod._request

    def fake(method, path, payload=None, **kw):
        return {"prices": prices} if path == "/info/oracle" else meta

    auth_mod._request = fake
    try:
        return live_mod._default_venue_state()
    finally:
        auth_mod._request = real


_ACTIVE = {"flags": {"trading_paused": False},
           "markets": [{"symbol": m.replace("-PERP", ""), "status": "active"}
                       for m in MARKETS]}


def test_every_market_active_is_a_live_session():
    assert _venue_state(_ACTIVE)["flags"]["carried"] is False


@pytest.mark.parametrize("meta", [
    {"flags": {"trading_paused": False}},                        # no markets
    {"flags": {"trading_paused": False}, "markets": []},         # empty
    {"flags": {"trading_paused": False}, "markets": "nonsense"},  # wrong type
    {"flags": {"trading_paused": False},
     "markets": [{"symbol": "QQQ-VOL", "status": "active"}]},    # partial
])
def test_a_partial_markets_payload_is_treated_as_CARRIED(meta):
    """Absence is not evidence of life.

    The first version started from carried=False and only flipped on finding a
    matching non-active entry, so a missing key, an empty list, a malformed
    value or a response that simply omitted our markets all read as LIVE --
    and live sizing during a carried session is 8x what the stressed initial
    margin allows, which the venue rejects. No fills, no depth credit, all
    night.
    """
    assert _venue_state(meta)["flags"]["carried"] is True


def test_an_explicitly_non_active_market_is_carried():
    meta = {"flags": {"trading_paused": False},
            "markets": [dict(m, status="halted") if i == 0 else m
                        for i, m in enumerate(_ACTIVE["markets"])]}
    assert _venue_state(meta)["flags"]["carried"] is True


def test_a_thread_that_will_not_stop_is_still_flattened(qapp):
    """[review] terminate() is TerminateThread on Windows: the frame dies
    without unwinding, so the worker's `finally` -- which owns the cancel --
    never runs. Going straight there turned a slow venue into "OFF over a
    live book" on window close, which is the one outcome this class exists
    to prevent. The cancel is issued from the joining thread first.
    """
    live = _live(qapp)

    entered = threading.Event()
    release = threading.Event()

    class _Stuck:
        """Blocks INSIDE the tick, which is the only place the worker cannot
        observe the stop flag -- exactly the in-flight request case."""
        def tick(self, now_s, oracles, flags):
            entered.set()
            release.wait(20)          # released in the finally below

    live._runner = _Stuck()
    live.start()
    try:
        assert entered.wait(5), "the worker never reached the tick"

        live.join(timeout_ms=300)     # far too short, on purpose

        assert live._client.cancels, "terminated without retracting the book"
        assert live.book_is_empty(), "the switch would have read OFF"
        assert not live.is_running()
    finally:
        release.set()
        qapp.processEvents()


@pytest.mark.parametrize("bad", [0, 1, None, "", "false"])
def test_a_non_boolean_pause_flag_is_unreadable_not_unpaused(bad):
    """[review] Presence is not validity: bool(0) and bool(None) read as
    "not paused", the fail-open direction on the one flag the sponsor said
    bots must handle."""
    from gui.services.permuto.live import VenueStateUnreadable
    with pytest.raises(VenueStateUnreadable):
        _venue_state({"flags": {"trading_paused": bad},
                      "markets": _ACTIVE["markets"]})


def test_a_real_boolean_pause_flag_is_read():
    meta = dict(_ACTIVE)
    meta["flags"] = {"trading_paused": True}
    assert _venue_state(meta)["flags"]["trading_paused"] is True

