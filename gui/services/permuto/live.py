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
import math
import time
from typing import Any, Callable, Optional

from PySide6.QtCore import QObject, QThread, Signal, Slot

from gui.services.permuto.bbo import active_ring_pct
from gui.services.permuto.client import PermutoClient
from gui.services.permuto.quoting import MAX_ORACLE_AGE_S as _GRACE_S
from gui.services.permuto.runner import QuoteRunner

_log = logging.getLogger(__name__)

#: Threads that outlived their join budget. Held only so Qt never destroys a
#: QThread that is still running; nothing ever reads this.
_ABANDONED: list = []

__all__ = ["MARKETS", "PermutoLive"]

#: The three vol markets. Symbols, not oracle tickers -- order routes reject
#: the ticker form with HTTP 400 on every leg.
MARKETS = ["QQQ-VOL-PERP", "NVDA-VOL-PERP", "TSLA-VOL-PERP"]

#: Per-request timeout inside the quoting loop.
#:
#: [review] Deliberately far below auth._TIMEOUT (30s). A 30s ceiling inside
#: a 5s tick is wrong on its own terms -- one slow request stalls six ticks'
#: worth of quoting -- and it is what made window close dangerous: the worker
#: only checks the stop flag between ticks, so an in-flight tick against a
#: degraded venue could outlast the whole shutdown budget and be killed
#: before its cancel ran. Bounding the I/O is what makes a bounded shutdown
#: honest. A request that has not answered in this long has already missed
#: the tick it belonged to.
REQUEST_TIMEOUT_S = 4.0

#: Seconds between ticks. The oracle resamples every 5s and depth is measured
#: against a FRESH one, so a slower loop quotes against a value the venue has
#: already replaced.
TICK_S = 5.0

#: All BBO reads in one tick share this budget. Three independent 4-second
#: timeouts delayed risk and oracle handling by roughly 12 seconds when the
#: public book route degraded, despite the loop's 5-second cadence.
BBO_TICK_BUDGET_S = 1.0
BBO_REQUEST_TIMEOUT_S = 0.5


class VenueStateUnreadable(RuntimeError):
    """The venue's public state could not be read well enough to quote on."""


class _BudgetedBboFetcher:
    """Top-of-book reads sharing one sub-tick deadline."""

    def __init__(self, *, fetch=None, clock=time.monotonic,
                 tick_budget_s: float = BBO_TICK_BUDGET_S,
                 request_timeout_s: float = BBO_REQUEST_TIMEOUT_S) -> None:
        self._fetch = fetch
        self._clock = clock
        self._tick_budget_s = tick_budget_s
        self._request_timeout_s = request_timeout_s
        self._deadline = 0.0

    def start_tick(self) -> None:
        self._deadline = self._clock() + self._tick_budget_s

    def __call__(self, market: str):
        if self._deadline <= 0.0:
            self.start_tick()
        remaining = self._deadline - self._clock()
        if remaining <= 0.0:
            _log.debug("permuto: BBO budget exhausted before %s", market)
            return None

        fetch = self._fetch
        if fetch is None:
            from gui.services.permuto.auth import BASE_URL
            from gui.services.permuto.bbo import fetch_book

            fetch = lambda symbol, timeout: fetch_book(
                symbol, base_url=BASE_URL, timeout=timeout)

        timeout = min(self._request_timeout_s, remaining)
        return fetch(market, timeout)


def _fetch_oracle_prices() -> dict:
    """{symbol: price} from /info/oracle, or {} on any failure.

    Public and unauthenticated, like the tick's own read. Returns {} rather
    than raising: the caller treats absence as "use the tick's read", and a
    hiccup on this extra request must never cost a quoting cycle.
    """
    from gui.services.permuto.auth import _request

    doc = _request("GET", "/info/oracle", timeout=REQUEST_TIMEOUT_S) or {}
    prices = doc.get("prices")
    if not isinstance(prices, dict):
        return {}
    out = {}
    for symbol in MARKETS:
        value = prices.get(symbol.replace("-PERP", ""))
        if isinstance(value, (int, float, str)):
            try:
                parsed = float(value)
            except (TypeError, ValueError):
                continue
            if math.isfinite(parsed) and parsed > 0.0:
                out[symbol] = parsed
    return out


def _default_venue_state() -> dict:
    """Oracle prices and pause flags, from the public routes.

    Raises VenueStateUnreadable rather than returning a cheerful default.
    The caller turns that into a withdraw, which is the only safe reading:
    everything downstream treats "not paused" and "oracle present" as
    permission to rest orders.
    """
    from gui.services.permuto.auth import _request

    try:
        oracle_doc = _request("GET", "/info/oracle",
                              timeout=REQUEST_TIMEOUT_S) or {}
        meta = _request("GET", "/info/meta",
                        timeout=REQUEST_TIMEOUT_S) or {}
    except Exception as exc:  # noqa: BLE001
        raise VenueStateUnreadable("public routes failed: %s" % exc) from exc

    flags = meta.get("flags")
    # [review] FAIL CLOSED on a flags payload we do not recognise. This used
    # to be `bool(flags.get("trading_paused"))` over a `or {}` default, so a
    # shape change, a partial response or an error object all read as "not
    # paused" and the loop quoted straight through. The venue pause is the
    # one thing the sponsor said bots must handle, and every entrant meets it
    # at the Sunday reset. Absence of the key is not evidence of trading.
    # [review] Presence is not validity. `bool(flags.get(...))` after a
    # presence check still coerces `0`, `None` or `"": ` to "not paused" --
    # the fail-open direction on the one flag the sponsor said bots must
    # handle. Require an actual JSON boolean, as signup_open() already does.
    if not isinstance(flags, dict) or not isinstance(
            flags.get("trading_paused"), bool):
        raise VenueStateUnreadable(
            "/info/meta carried no boolean trading_paused flag")

    prices = oracle_doc.get("prices")
    if not isinstance(prices, dict):
        raise VenueStateUnreadable("/info/oracle carried no prices object")

    # /info/oracle is keyed by TICKER (QQQ-VOL); the runner works in SYMBOLS.
    oracles = {}
    for symbol in MARKETS:
        value = prices.get(symbol.replace("-PERP", ""))
        if isinstance(value, (int, float, str)):
            try:
                parsed = float(value)
            except (TypeError, ValueError):
                continue
            if math.isfinite(parsed) and parsed > 0.0:
                oracles[symbol] = parsed

    # [review] `carried` was read from flags["carried"], a key /info/meta does
    # not have -- verified against the live venue, whose flags are banner,
    # competition_mode, mm_prize_min_depth_seconds, pause_*, signup_*,
    # trading_paused, untraded_purge_* and withdrawals_enabled. So it was
    # always False, and a carried session would have sized every quote at 8x
    # what the stressed initial margin allows (risk.py divides by
    # CARRIED_IM_MULTIPLIER only when this is set), which the venue rejects --
    # no fills, and no depth credit, for the whole overnight session.
    #
    # The observable signal is the per-market status in /info/meta["markets"],
    # which reads "active" on a live market. Anything else is treated as not
    # live, because the sizing consequence of guessing wrong in that direction
    # is a rejected batch and in the other direction is a liquidation.
    # [review] Built as an ACTIVE SET, so absence is not evidence of life.
    #
    # The previous version started from carried = False and only set it on
    # finding a matching entry with a non-active status -- so a missing
    # `markets` key, an empty list, malformed entries, or a response that
    # simply did not mention our three markets all read as LIVE. That is the
    # 8x oversizing this whole block exists to prevent, reachable through a
    # partial payload rather than a wrong one, and it contradicted the
    # comment directly above it.
    #
    # Carried unless EVERY configured market is explicitly "active".
    markets = meta.get("markets")
    wanted = {m.replace("-PERP", "") for m in MARKETS}
    active = set()
    # [release review] Per-market tick/lot specs, for the ladder's
    # quantisation. Missing or malformed values fall back to the documented
    # defaults inside quote_ladder, so a partial payload degrades to the
    # published grid rather than to raw floats.
    specs: dict = {}
    if isinstance(markets, list):
        for entry in markets:
            if not isinstance(entry, dict):
                continue
            names = {entry.get("symbol"), entry.get("base_asset")}
            hit = names & wanted
            if hit:
                spec = {}
                for field, key in (("tick_size", "tick_size"),
                                   ("lot_size", "lot_size")):
                    try:
                        spec[field] = float(entry.get(key))
                    except (TypeError, ValueError):
                        pass
                for name in hit:
                    specs[name + "-PERP"] = spec
            if entry.get("status") != "active":
                continue
            for name in hit:
                active.add(name)
    carried = active != wanted

    ring_pct, ring_src = active_ring_pct(meta)
    out_flags = {
        "trading_paused": bool(flags.get("trading_paused")),
        "carried": carried,
        "specs": specs,
    }
    if ring_src == "venue":
        out_flags["ring_pct"] = ring_pct
    else:
        _log.debug("permuto: no valid vol_aggressive_ring_pct in meta")

    return {
        "oracles": oracles,
        "flags": out_flags,
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
        # When we last held a venue reading we would be willing to quote on.
        # Seeded at construction so the first tick is not born stale.
        self._fresh_at = time.monotonic()
        #: The last successfully parsed (oracles, flags), for riding a
        #: transient venue blip inside the oracle grace.
        self._last_good: Optional[tuple] = None

    def request_stop(self) -> None:
        self._stop = True

    @Slot()
    def run(self) -> None:
        reason = "stopped"
        try:
            while not self._stop:
                start = time.monotonic()
                try:
                    # [review] The venue read gets its own try, and the tick
                    # ALWAYS runs.
                    #
                    # These used to share one handler, so any failure of
                    # /info/oracle or /info/meta -- a 5xx, a 429, a timeout,
                    # a non-JSON body -- skipped tick() entirely. No
                    # cancel_all, no reconcile, no account read, no risk
                    # pass: every order from the last good tick stayed
                    # resting, priced against an oracle the loop could no
                    # longer read, for the whole outage. decide() has a
                    # withdraw for exactly this and it was never reached.
                    #
                    # Degrading instead of skipping hands it an empty oracle
                    # set and a paused flag, both of which decide() already
                    # answers with WITHDRAW -- so an unreadable venue
                    # retracts the book rather than abandoning it.
                    try:
                        state = self._venue_state()
                        oracles = dict(state.get("oracles") or {})
                        flags = dict(state.get("flags") or {})
                        if oracles:
                            self._fresh_at = time.monotonic()
                            self._last_good = (oracles, flags)
                    except Exception as exc:  # noqa: BLE001
                        # [release review] One failed GET used to cancel the
                        # WHOLE book immediately -- the 15s oracle grace that
                        # MAX_ORACLE_AGE_S exists for was unreachable,
                        # because a fabricated empty-oracle state withdraws
                        # without consulting age at all. Inside the grace the
                        # last good reading is used with its REAL age, so
                        # decide() holds through a blip and still withdraws
                        # the moment the reading is genuinely stale.
                        age = time.monotonic() - self._fresh_at
                        if self._last_good is not None and age <= _GRACE_S:
                            _log.warning(
                                "permuto: venue state unreadable (%s) -- "
                                "riding the %.1fs-old reading inside the "
                                "%.0fs grace", exc, age, _GRACE_S)
                            oracles, flags = (dict(self._last_good[0]),
                                              dict(self._last_good[1]))
                        else:
                            _log.warning(
                                "permuto: venue state unreadable: %s", exc)
                            oracles, flags = {}, {"trading_paused": True,
                                                  "carried": False}

                    # [review] A REAL oracle age. decide() withdraws above
                    # MAX_ORACLE_AGE_S, and nothing in the process had ever
                    # produced this key -- it defaulted to 0.0, so the check
                    # evaluated 0.0 > 15.0 on every tick and the withdraw was
                    # unreachable code. This module's own docstring cites
                    # that protection as the reason the venue is polled every
                    # tick. /info/oracle carries no timestamp (verified
                    # against the live venue: it returns prices and nothing
                    # else), so the honest measure is how long since WE last
                    # held a usable reading.
                    flags["oracle_age_s"] = time.monotonic() - self._fresh_at

                    result = self._runner.tick(time.time(), oracles, flags)
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
                # Disarm the venue-side switch AFTER our own cancel: the book
                # is empty now, and a scheduled cancel firing later would
                # spend one of the ten daily triggers on nothing. Best
                # effort -- if it stays armed it fires harmlessly.
                try:
                    self._client.clear_schedule_cancel(time.time())
                except Exception:  # noqa: BLE001
                    pass
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
        target_depth_usd: float = 10_000.0,
        max_position_usd: float = 250_000.0,
        curfew_enabled: bool = True,
        ring_pct: float = 2.0,
        venue_state: Optional[Callable[[], dict]] = None,
        client: Any = None,
    ) -> None:
        super().__init__()
        self._identity = identity
        self._markets = list(markets or MARKETS)
        user_id = ""
        try:
            user_id = getattr(identity.info(), "user_id", "") or ""
        except Exception:  # noqa: BLE001 - an unregistered identity has none
            pass
        self._client = client or PermutoClient(
            identity, session_token=session_token,
            timeout=REQUEST_TIMEOUT_S, user_id=user_id)
        self._runner = QuoteRunner(
            self._client, self._markets,
            target_depth_usd=target_depth_usd,
            max_position_usd=max_position_usd,
            curfew_enabled=curfew_enabled,
            ring_pct=ring_pct,
            # [PREFLIGHT] The runner re-reads the oracle immediately before
            # sending, so leg prices are judged against the value the venue
            # will actually compare them to rather than one a tick older.
            oracle_fetch=_fetch_oracle_prices,
            # [BBO 2026-09-02] Consult the real book before placing. Without
            # this the runner learns the resting price only from refusals,
            # which saturates against bids parked on the ring ceiling and
            # banked zero depth-seconds for an entire session.
            bbo_fetch=_BudgetedBboFetcher(),
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
        # [review round 9] Fence placements BEFORE anything else. A tick
        # already in flight cannot observe the stop flag until it returns, so
        # it could otherwise resume after the last-resort cancel below and
        # place a fresh batch -- the book left live while the process exits
        # claiming it is empty. Once a stop is requested, off means flat, and
        # a placement that slips past the fence onto the wire is covered by
        # the cancels that follow it on both paths.
        try:
            self._client.halt_placements()
        except Exception:  # noqa: BLE001 - a fake client without the method
            pass
        # [review round 11] And WAIT for any placement already on the wire.
        # The fence stops new sends; this orders the final cancel after the
        # one that got through. Bounded: a placement holds the lock only
        # across a single REQUEST_TIMEOUT_S-bounded send, and a wedged one
        # past twice that is abandoned to the cancel below -- a cancel
        # racing a wedged placement is still strictly better than no cancel.
        lock = getattr(self._client, "_placement_lock", None)
        if lock is not None:
            if lock.acquire(timeout=REQUEST_TIMEOUT_S * 2):
                lock.release()
            else:
                _log.critical("permuto: a placement is still on the wire "
                              "past its budget -- sending the cancel anyway")
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
            # [review] CANCEL BEFORE TERMINATING. terminate() is
            # TerminateThread on Windows: the frame is destroyed without
            # unwinding, so the worker's `finally` -- the thing that owns the
            # cancel -- never runs. The old code went straight there, which
            # meant a slow venue turned window close into "OFF over a live
            # book", the one outcome this class exists to prevent.
            #
            # So the cancel is issued from HERE first, on our own thread,
            # bounded by the same short request timeout the loop uses. It may
            # duplicate the worker's if that is what is hanging; a duplicate
            # cancel is harmless and an uncancelled book is not.
            _log.critical("permuto: the live thread did not stop in %dms -- "
                          "cancelling from the GUI thread before terminating",
                          timeout_ms)
            try:
                self._client.cancel_all(time.time())
                self._book_empty = True
                _log.critical("permuto: last-resort cancel SENT")
            except Exception as exc:  # noqa: BLE001
                self._book_empty = False
                _log.critical("permuto: last-resort cancel FAILED (%s) -- "
                              "orders may still be resting at the venue; "
                              "check the book by hand", exc)
            # DO NOT TERMINATE. terminate() is TerminateThread on Windows,
            # and if it lands while the worker holds the GIL -- which it does
            # whenever it is running Python rather than blocked in a socket
            # -- the wait() after it can never reacquire the interpreter and
            # closeEvent hangs FOREVER. That is strictly worse than the slow
            # exit it was meant to bound, and a test written for this path
            # hung on exactly it.
            #
            # The book has just been retracted above, which is the part that
            # matters. The thread is abandoned to the process teardown, and
            # its objects are parked in a module-level list so the QThread is
            # never garbage-collected while still running -- that would be
            # the "destroyed while thread is still running" abort this file
            # already carries a comment about.
            _ABANDONED.append((thread, self._worker))
            _log.critical("permuto: the live thread is abandoned to process "
                          "exit rather than terminated (terminating it can "
                          "deadlock the GUI)")
            self._thread = None
            self._worker = None
            return
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
