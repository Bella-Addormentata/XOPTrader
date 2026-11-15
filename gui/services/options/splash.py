"""Hand offers to a Splash! node, which gossips them to the rest of the network.

Splash! (https://github.com/dexie-space/splash) is Dexie's peer-to-peer network
for Chia offers: a small daemon that forwards every offer string it is given to
the peers it is connected to, and every one it hears to whoever asked for them.
It keeps nothing. Dexie runs peers that index what they hear, and relays to the
network whatever is uploaded to it.

This module is the publishing half only, and all of it is one request to a
daemon the operator runs beside the book::

    splash --listen-offer-submission 127.0.0.1:4000        # add --testnet on testnet11

    POST http://127.0.0.1:4000      {"offer": "offer1..."}
    -> 200 {"success": true}  or  200 {"success": false, "error": "..."}

Four things to know about what that answer is worth:

* ``success: true`` means the daemon took the offer, not that anyone heard it.
  It answers before it has published, and says the same with no peers at all.
* Nothing can be taken back. An offer that has been gossiped is out, exactly as
  one uploaded to a site is: only spending the option coin ends it.
* The network keeps no history. A listener hears an offer if it is connected
  when the offer is sent, and never otherwise. The book re-makes each offer
  before it expires (``offer_ttl_seconds`` less ``refresh_before_seconds``, 12
  minutes as shipped), and every new one is announced, so that is how often
  each contract is heard.
* The daemon's peers see its address. Offers are public either way.

The daemon answers HTTP 200 whatever happened, so the body is what is read.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from urllib.parse import urlsplit

DEFAULT_SUBMIT_URL = "http://127.0.0.1:4000"

#: What a node relays at most (``MAX_OFFER_SIZE`` in Splash 0.3.0; 0.2.0 allowed
#: three times this). An option offer is about 2 KiB.
MAX_OFFER_BYTES = 100 * 1024

#: ``post(url, json_body) -> (status, body text)``
Post = Callable[[str, dict], tuple[int, str]]


class SplashError(RuntimeError):
    """The offer was not handed to the daemon. It is still a valid offer."""


def check_submit_url(url: str) -> str:
    """The address of a daemon's offer-submission API, or ValueError."""
    text = str(url or "").strip()
    parts = urlsplit(text)
    if parts.scheme not in ("http", "https") or not parts.hostname:
        raise ValueError(f"not an http address: {text!r}")
    if parts.username or parts.password:
        raise ValueError("the address must not carry a user name or password")
    return text


def _default_post(timeout: float) -> Post:
    import requests  # lazy: only when a real network call is made

    session = requests.Session()
    session.headers.update({"User-Agent": "XOPTrader"})

    def post(url: str, body: dict) -> tuple[int, str]:
        response = session.post(url, json=body, timeout=timeout)
        return response.status_code, response.text

    return post


class SplashClient:
    """The offer-submission API of one Splash! daemon."""

    def __init__(
        self,
        submit_url: str = DEFAULT_SUBMIT_URL,
        *,
        post: Post | None = None,
        timeout: float = 10.0,
    ) -> None:
        self.submit_url = check_submit_url(submit_url)
        self._post = post or _default_post(timeout)

    def broadcast(self, offer: str) -> None:
        """Hand one offer to the daemon. Raises :class:`SplashError` if it was not taken."""
        offer = str(offer)
        # What every node checks before relaying, checked here first so that a
        # refusal names the reason. Nodes on 0.2.0 also want the offer1 prefix.
        if not offer.startswith("offer1"):
            raise SplashError("not an offer string")
        if len(offer.encode()) > MAX_OFFER_BYTES:
            raise SplashError(f"the offer is over the network's {MAX_OFFER_BYTES} byte limit")
        try:
            status, text = self._post(self.submit_url, {"offer": offer})
        except Exception as exc:
            raise SplashError(f"no answer from the Splash daemon at {self.submit_url}") from exc
        if status != 200:
            raise SplashError(f"the Splash daemon answered HTTP {status}")
        try:
            answer = json.loads(text)
        except ValueError as exc:
            raise SplashError("the Splash daemon's answer was not JSON") from exc
        if not isinstance(answer, dict) or answer.get("success") is not True:
            why = answer.get("error") if isinstance(answer, dict) else None
            raise SplashError(f"the Splash daemon refused the offer: {why or 'no reason given'}")
