"""The Splash! client: one request to a daemon, and what its answer is worth."""

from __future__ import annotations

import json
from typing import Any

import pytest

from gui.services.options import splash

OFFER = "offer1" + "q" * 1900  # about the size of a real option offer


class Daemon:
    """The submission API as Splash 0.3.0 serves it: HTTP 200 whatever happened."""

    def __init__(self, answer: Any = None, status: int = 200) -> None:
        self.answer = {"success": True} if answer is None else answer
        self.status = status
        self.calls: list[tuple[str, dict]] = []

    def __call__(self, url: str, body: dict) -> tuple[int, str]:
        self.calls.append((url, body))
        if isinstance(self.answer, Exception):
            raise self.answer
        text = self.answer if isinstance(self.answer, str) else json.dumps(self.answer)
        return self.status, text


def test_an_offer_is_posted_as_the_daemon_expects() -> None:
    daemon = Daemon()
    client = splash.SplashClient("http://127.0.0.1:4000", post=daemon)

    client.broadcast(OFFER)

    assert daemon.calls == [("http://127.0.0.1:4000", {"offer": OFFER})]


def test_the_default_address_is_the_local_daemon() -> None:
    assert splash.SplashClient(post=Daemon()).submit_url == "http://127.0.0.1:4000"


@pytest.mark.parametrize(
    ("answer", "status", "why"),
    [
        # The daemon answers 200 even when it refuses: the body is what counts.
        ({"success": False, "error": "Invalid offer format"}, 200, "Invalid offer format"),
        ({"success": False}, 200, "no reason given"),
        ({"success": "true"}, 200, "refused"),  # only a real true is a yes
        ({}, 200, "refused"),
        ([1, 2], 200, "refused"),
        ("<html>gateway</html>", 200, "not JSON"),
        ({"success": True}, 502, "HTTP 502"),
        (ConnectionError("connection refused"), 200, "no answer from the Splash daemon"),
    ],
)
def test_anything_but_a_plain_yes_is_a_failure(answer, status, why) -> None:
    client = splash.SplashClient(post=Daemon(answer, status))
    with pytest.raises(splash.SplashError, match=why):
        client.broadcast(OFFER)


def test_what_no_node_would_relay_is_not_sent_at_all() -> None:
    daemon = Daemon()
    client = splash.SplashClient(post=daemon)

    with pytest.raises(splash.SplashError, match="not an offer string"):
        client.broadcast("hello")
    with pytest.raises(splash.SplashError, match="102400 byte limit"):
        client.broadcast("offer1" + "q" * splash.MAX_OFFER_BYTES)
    assert daemon.calls == []

    # Exactly at the limit is fine.
    client.broadcast("offer1" + "q" * (splash.MAX_OFFER_BYTES - 6))
    assert len(daemon.calls) == 1


@pytest.mark.parametrize(
    "url", ["", "127.0.0.1:4000", "ftp://127.0.0.1:4000", "http://", "http://user:pw@host:4000"]
)
def test_an_address_that_is_not_a_plain_http_one_is_refused(url) -> None:
    with pytest.raises(ValueError):
        splash.check_submit_url(url)
    if url:
        with pytest.raises(ValueError):
            splash.SplashClient(url, post=Daemon())


def test_usable_addresses_are_kept_as_given() -> None:
    for url in ("http://127.0.0.1:4000", "http://localhost:4000/", "https://splash.lan:4443"):
        assert splash.check_submit_url(f"  {url} ") == url
