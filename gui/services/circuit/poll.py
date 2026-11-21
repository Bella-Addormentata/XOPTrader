"""The reads the Circuit page makes, and the loop that makes them.

The page asks the eight reads in :data:`PAGE_READS`, one after another, on a
worker thread, about every three minutes while it is open. With the client's
3.2 s between requests a round takes about half a minute, so the page costs
under three requests a minute while it is showing and none while it is not.
The bodies are the probe's (:data:`gui.services.circuit.api.READS`), checked
against the live server on 2026-11-21.
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


class Stopped(Exception):  # noqa: N818 -- an outcome, not an error
    """The page was closed or switched off while a round was running."""


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


def page_client(stop: threading.Event, *, transport: Any = None) -> CircuitApi:
    """The page's client: its waits between requests end as soon as ``stop`` is set."""

    def sleep(seconds: float) -> None:
        if stop.wait(seconds):
            raise Stopped()

    return CircuitApi(
        DEFAULT_BASE_URL,
        timeout_s=PAGE_TIMEOUT_S,
        transport=transport or requests_transport,
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
    from the client's wait passes through, as does ``should_stop`` turning
    true between two reads (the round just ends).
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
