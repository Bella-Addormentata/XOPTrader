"""The reads the Circuit page makes, and the loop that makes them.

The page asks the eight reads in :data:`PAGE_READS`, one after another, on a
worker thread, about every three minutes while it is open. With the client's
3.2 s between requests a round takes about half a minute, so the page costs
under three requests a minute while it is showing and none while it is not.
The bodies are the probe's (:data:`gui.services.circuit.api.READS`), checked
against the live server on 2026-11-21.

Each request runs on a daemon thread of its own (:func:`interruptible`), so
the page's worker thread stops as soon as it is told to, even in the middle of
a request that is slow to answer.
"""

from __future__ import annotations

import dataclasses
import threading
import time
from collections.abc import Callable, Sequence
from typing import Any

from gui.services.circuit.api import (
    DEFAULT_BASE_URL,
    READS,
    CircuitApi,
    CircuitApiError,
    Read,
    Transport,
)

#: The page's reads, most useful first: the price, the settings, the vaults
#: (the biggest answer, about 140 KB for 214 vaults), then the rest.
PAGE_READ_NAMES: tuple[str, ...] = (
    "oracle",
    "statutes",
    "vaults",
    "treasury",
    "recharge_auctions",
    "surplus_auctions",
    "bills",
    "announcers",
)
PAGE_READS: tuple[Read, ...] = tuple(
    next(read for read in READS if read.name == name) for name in PAGE_READ_NAMES
)
#: What the page tells the server it is (a client name, nothing about who asks).
PAGE_USER_AGENT = "XOPTrader-circuit-page/1"
#: Seconds to wait for one answer.
PAGE_TIMEOUT_S = 20.0
#: How often a request's caller looks at the stop flag while it waits.
STOP_POLL_S = 0.1


class Stopped(BaseException):  # noqa: N818 -- an outcome, not an error
    """The page was closed or switched off while a round was running.

    A ``BaseException``, as ``KeyboardInterrupt`` is, so that the client's
    "any transport failure is no answer" (``except Exception``) lets it
    through: a request stopped is not a read that failed.
    """


@dataclasses.dataclass(frozen=True)
class Result:
    """One read's outcome: its parsed answer, or why there is none."""

    name: str
    data: Any = None
    error: str = ""
    #: Unix time the answer came back.
    at: float = 0.0

    @property
    def ok(self) -> bool:
        return not self.error


def requests_transport(
    method: str, url: str, body: bytes | None, headers: dict[str, str], timeout_s: float
) -> tuple[int, dict[str, str], bytes]:
    """A :data:`~gui.services.circuit.api.Transport` on ``requests``.

    The installed app ships ``requests`` with its own certificate bundle,
    which ``urllib`` in a frozen build may not find.
    """
    import requests  # the app's dependency; api.py stays standard-library only

    response = requests.request(
        method, url, data=body, headers=headers, timeout=timeout_s, allow_redirects=False
    )
    return response.status_code, dict(response.headers), response.content


def interruptible(transport: Transport, stop: threading.Event) -> Transport:
    """``transport`` with each request on a daemon thread of its own.

    The caller waits for the answer or for ``stop``, whichever comes first.
    Once ``stop`` is set it raises :class:`Stopped` within :data:`STOP_POLL_S`
    and leaves the request to finish, or time out, on its own thread, whose
    answer is dropped. That thread touches nothing of Qt's and does not keep
    the app from exiting, so closing the app never waits on a slow read, nor
    has to kill the thread that asked for it.
    """

    def call(
        method: str, url: str, body: bytes | None, headers: dict[str, str], timeout_s: float
    ) -> tuple[int, dict[str, str], bytes]:
        if stop.is_set():
            raise Stopped()
        finished = threading.Event()
        outcome: list[tuple[bool, Any]] = []

        def run() -> None:
            try:
                outcome.append((True, transport(method, url, body, headers, timeout_s)))
            except BaseException as exc:  # noqa: BLE001 -- handed back to the caller
                outcome.append((False, exc))
            finally:
                finished.set()

        threading.Thread(target=run, name="circuit-page-request", daemon=True).start()
        while not finished.wait(STOP_POLL_S):
            if stop.is_set():
                raise Stopped()
        answered, value = outcome[0]
        if not answered:
            raise value
        return value

    return call


def page_client(stop: threading.Event, *, transport: Any = None) -> CircuitApi:
    """The page's client: its waits, and a request under way, end once ``stop`` is set."""

    def sleep(seconds: float) -> None:
        if stop.wait(seconds):
            raise Stopped()

    return CircuitApi(
        DEFAULT_BASE_URL,
        timeout_s=PAGE_TIMEOUT_S,
        transport=interruptible(transport or requests_transport, stop),
        sleep=sleep,
        user_agent=PAGE_USER_AGENT,
    )


def fetch(
    api: CircuitApi,
    reads: Sequence[Read] = PAGE_READS,
    *,
    on_result: Callable[[Result], None],
    should_stop: Callable[[], bool] = lambda: False,
    wall: Callable[[], float] = time.time,
) -> int:
    """Make each read in turn and report each outcome; returns how many answered.

    A read that fails is reported and the round goes on. :class:`Stopped`
    from the client (its wait, or a request under way) passes through, and
    ``should_stop`` turning true between two reads just ends the round.
    """
    answered = 0
    for read in reads:
        if should_stop():
            break
        try:
            if read.method == "GET":
                data = api.get(read.path)
            else:
                data = api.post(read.path, read.body)
        except CircuitApiError as exc:
            on_result(Result(read.name, error=str(exc), at=wall()))
            continue
        answered += 1
        on_result(Result(read.name, data=data, at=wall()))
    return answered
