"""The live quoting session: a QuoteRunner on its own thread.

This is the piece the venue switch was blocked on. Everything below it --
identity, auth, session policy, placement rules, batch validation, risk,
the tick sequencer -- was built and tested; nothing owned an instance, so
the switch could only ever refuse.

THREE RULES IT INHERITS, and they are the reason this is a controller rather
than a bare thread.

**Off means flat.** Stopping cancels the book before the thread exits, and
the controller keeps reporting a non-empty book until that cancel is
acknowledged. A switch that says OFF over live orders is the failure the
whole two-venue design was chosen to avoid.

**A tick must never kill the loop.** QuoteRunner.tick() already returns
failures rather than raising, and this adds the same discipline around
everything outside it -- the oracle read, the flag read, the sleep. The
contest runs ~102 unattended hours; a loop that dies on one transient 500 has
failed at its only job.

**The venue is polled for its own state, not ours.** Oracle and pause flags
come from the public routes every tick, because `decide()` withdraws on a
stale oracle and the Sunday reset arrives as a pause. Reusing a cached value
here would make the loop confident and wrong.
"""

from __future__ import annotations

import logging
import time
from typing import Any, Callable, Optional

from PySide6.QtCore import QObject, QThread, Signal, Slot

from gui.services.permuto.client import PermutoClient
from gui.services.permuto.runner import QuoteRunner

_log = logging.getLogger(__name__)

__all__ = ["MARKETS", "PermutoLive"]

#: The three vol markets. Symbols, not oracle tickers -- order routes reject
#: the ticker form with HTTP 400 on every leg.
MARKETS = ["QQQ-VOL-PERP", "NVDA-VOL-PERP", "TSLA-VOL-PERP"]

#: Seconds between ticks. The oracle resamples every 5s and depth is measured
#: against a FRESH one, so a slower loop quotes against a value the venue has
#: already replaced.
TICK_S = 5.0


def _default_venue_state() -> dict:
    """Oracle prices and pause flags, from the public routes."""
    from gui.services.permuto.auth import _request

    prices = (_request("GET", "/info/oracle") or {}).get("prices") or {}
    flags = (_request("GET", "/info/meta") or {}).get("flags") or {}
    # /info/oracle is keyed by TICKER (QQQ-VOL); the runner works in SYMBOLS.
    oracles = {}
    for symbol in MARKETS:
        value = prices.get(symbol.replace("-PERP", ""))
        if isinstance(value, (int, float, str)):
            try:
                oracles[symbol] = float(value)
            except (TypeError, ValueError):
                continue
    return {
        "oracles": oracles,
        "flags": {
            "trading_paused": bool(flags.get("trading_paused")),
            "carried": bool(flags.get("carried")),
        },
    }


class _Worker(QObject):
    """Ticks the runner until asked to stop, then flattens."""

    ticked = Signal(object)      # TickResult
    stopped = Signal(str)        # operator-facing reason
    book_state = Signal(bool)    # True when we believe nothing is resting

    def __init__(self, runner: QuoteRunner, client: PermutoClient,
                 venue_state: Callable[[], dict]) -> None:
        super().__init__()
        self._runner = runner
        self._client = client
        self._venue_state = venue_state
        self._stop = False

    def request_stop(self) -> None:
        self._stop = True

    @Slot()
    def run(self) -> None:
        reason = "stopped"
        try:
            while not self._stop:
                start = time.monotonic()
                try:
                    state = self._venue_state()
                    result = self._runner.tick(
                        time.time(), state["oracles"], state["flags"])
                    self.ticked.emit(result)
                except Exception as exc:  # noqa: BLE001 - never kill the loop
                    _log.exception("permuto: tick raised")
                    self.ticked.emit(
                        type("TickResult", (), {
                            "action": "error", "reason": repr(exc),
                            "markets": {}, "error": repr(exc), "ok": False})())
                # Sleep in slices so a stop is honoured promptly rather than
                # after a full tick.
                while (not self._stop
                       and time.monotonic() - start < TICK_S):
                    time.sleep(0.1)
        finally:
            # OFF MEANS FLAT. Cancel before the thread ends, whatever
            # happened above -- including an exception that escaped the loop.
            try:
                self._client.cancel_all(time.time())
                self.book_state.emit(True)
                reason = "stopped; a cancel of every resting order was sent"
            except Exception as exc:  # noqa: BLE001
                self.book_state.emit(False)
                reason = ("stopped, but the cancel FAILED (%s) -- orders may "
                          "still be resting" % exc)
                _log.critical("permuto: %s", reason)
            self.stopped.emit(reason)


class PermutoLive(QObject):
    """Owns the thread, the client and the runner. One per window."""

    ticked = Signal(object)
    stopped = Signal(str)

    def __init__(
        self,
        identity: Any,
        *,
        session_token: str = "",
        markets: Optional[list] = None,
        target_depth_usd: float = 1_200.0,
        max_position: float = 100.0,
        venue_state: Optional[Callable[[], dict]] = None,
        client: Any = None,
    ) -> None:
        super().__init__()
        self._identity = identity
        self._markets = list(markets or MARKETS)
        self._client = client or PermutoClient(
            identity, session_token=session_token)
        self._runner = QuoteRunner(
            self._client, self._markets,
            target_depth_usd=target_depth_usd,
            max_position=max_position,
        )
        self._venue_state = venue_state or _default_venue_state
        self._thread: Optional[QThread] = None
        self._worker: Optional[_Worker] = None
        # Pessimistic until a cancel is acknowledged: an unknown book is a
        # book that may still be takeable.
        self._book_empty = True

    # -- state -------------------------------------------------------------- #
    def is_running(self) -> bool:
        return self._thread is not None

    def book_is_empty(self) -> bool:
        return self._book_empty

    # -- lifecycle ---------------------------------------------------------- #
    def start(self) -> None:
        if self._thread is not None:
            return
        self._book_empty = False   # about to be in the market
        thread = QThread()
        worker = _Worker(self._runner, self._client, self._venue_state)
        worker.moveToThread(thread)
        thread.started.connect(worker.run)
        worker.ticked.connect(self.ticked)
        worker.book_state.connect(self._on_book_state)
        worker.stopped.connect(self._on_stopped)
        worker.stopped.connect(thread.quit)
        # References are dropped on FINISHED, never on stopped. `stopped` is
        # emitted from inside run(), while the thread is still running -- and
        # clearing self._thread there drops the last Python reference to a
        # live QThread, which Qt turns into a "destroyed while thread is
        # still running" abort. That is a process kill, not an exception, so
        # it takes the whole GUI with it.
        thread.finished.connect(self._on_thread_finished)
        self._thread, self._worker = thread, worker
        thread.start()

    def stop(self) -> None:
        """Ask the loop to stop. The cancel happens on the worker thread."""
        if self._worker is not None:
            self._worker.request_stop()

    def join(self, timeout_ms: int = 30_000) -> None:
        """Block until the thread is down. Called on window close.

        Qt aborts the process if a running QThread is destroyed, and the
        cancel this waits for is the one that empties the book -- so the
        wait is generous and the terminate is a last resort.
        """
        thread = self._thread
        if thread is None:
            return
        self.stop()
        # [review] quit() DIRECTLY, not through worker.stopped -> thread.quit.
        #
        # That connection is auto, and the QThread OBJECT lives in the GUI
        # thread, so the slot invocation is queued to the GUI thread's event
        # loop -- the loop this function is about to block by calling wait().
        # The quit could therefore never be delivered, exec() never returned,
        # and a perfectly healthy stop sat here for the full 30 s before
        # reaching terminate(). QThread::quit is thread-safe and callable
        # from anywhere; called here it takes effect as soon as run() returns.
        #
        # The tests did not catch this because they spin on processEvents()
        # instead of blocking, which is exactly the delivery that closeEvent
        # cannot do.
        thread.quit()
        if not thread.wait(timeout_ms):
            _log.warning("permuto: live thread did not stop; terminating")
            thread.terminate()
            thread.wait(2_000)
        self._thread = None
        self._worker = None

    # -- signals ------------------------------------------------------------ #
    def _on_book_state(self, empty: bool) -> None:
        self._book_empty = bool(empty)

    def _on_stopped(self, reason: str) -> None:
        # Report only. The thread is still unwinding; see _on_thread_finished.
        self.stopped.emit(reason)

    def _on_thread_finished(self) -> None:
        thread, self._thread = self._thread, None
        self._worker = None
        if thread is not None:
            thread.deleteLater()
