"""Thin warp.green watcher-API client.

The watcher is warp.green's hosted indexer. Once the validators observe a
``bridgeToChia`` call on Base and attest to it, the watcher exposes the message
by its on-chain nonce with ``status == "sent"`` and the authoritative decoded
``contents = [erc20_source, receiver_puzzle_hash, post_tip_amount]``. This is the
single point where the bot learns the *attested* amount (post-tip) and confirms
the receiver puzzle hash the validators actually signed -- the claim path treats
these as ground truth and refuses to proceed if they disagree with what it
precomputed.

Transport is injected (``getter``) so the state machine and its tests need no
network; the default getter builds a ``requests`` session lazily.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Optional

# (url, params) -> parsed JSON (list or dict).
Getter = Callable[[str, dict], Any]


class WatcherError(RuntimeError):
    """A watcher-API call failed at transport level or returned junk."""


def _hx(value: Any) -> str:
    """Normalise hex to lower-case without ``0x``; ints -> hex; ``None`` -> ""."""
    if value is None:
        return ""
    if isinstance(value, int):
        return format(value, "x")
    s = str(value).strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    return s.lower()


def _0x(value: str) -> str:
    return "0x" + _hx(value)


# NOTE: the watcher API rejects a 0x-prefixed nonce with HTTP 500, on both
# source chains. Verified live against https://watcher-api.warp.green/messages
# with a real bse nonce: bare hex -> 200, "0x"-prefixed -> 500. Query params
# must therefore use bare hex (:func:`_hx`), not :func:`_0x`.


@dataclass(frozen=True)
class WatcherMessage:
    """A bridge message as attested by the validators.

    ``contents`` is the decoded warp message body; for a wrapped-asset mint it is
    ``[erc20_source, receiver_puzzle_hash, post_tip_amount]`` -- all normalised to
    lower-case hex without ``0x``. ``destination`` is the cat_minter puzzle hash
    the validators signed (the claim path re-derives this and must match).
    """

    nonce: str
    status: str
    source_chain: str
    destination_chain: str
    destination: str
    contents: tuple[str, ...]
    raw: dict

    @property
    def is_sent(self) -> bool:
        return self.status.lower() == "sent"

    @property
    def erc20_source(self) -> Optional[str]:
        return self.contents[0] if len(self.contents) >= 1 else None

    @property
    def receiver_ph(self) -> Optional[str]:
        return self.contents[1] if len(self.contents) >= 2 else None

    @property
    def amount_mojos(self) -> Optional[int]:
        """Post-tip CAT mojo amount from ``contents[2]`` (a 32-byte hex word)."""
        if len(self.contents) < 3 or not self.contents[2]:
            return None
        return int(self.contents[2], 16)


def _parse_message(d: dict) -> WatcherMessage:
    return WatcherMessage(
        nonce=_hx(d.get("nonce")),
        status=str(d.get("status", "")),
        source_chain=str(d.get("source_chain") or d.get("source") or ""),
        destination_chain=str(d.get("destination_chain") or d.get("destination_c") or ""),
        destination=_hx(d.get("destination")),
        contents=tuple(_hx(x) for x in (d.get("contents") or [])),
        raw=d,
    )


def _messages_from(data: Any) -> list[dict]:
    """The API has returned a bare list, ``{"messages": [...]}``, or a single dict."""
    if data is None:
        return []
    if isinstance(data, list):
        return [m for m in data if isinstance(m, dict)]
    if isinstance(data, dict):
        inner = data.get("messages")
        if isinstance(inner, list):
            return [m for m in inner if isinstance(m, dict)]
        if data.get("message") and isinstance(data["message"], dict):
            return [data["message"]]
        if "nonce" in data:  # the dict *is* the message
            return [data]
    return []


def _default_getter(timeout: float) -> Getter:
    import requests  # lazy: only when a real network call is made

    session = requests.Session()

    def get(url: str, params: dict) -> Any:
        resp = session.get(url, params=params, timeout=timeout)
        resp.raise_for_status()
        return resp.json()

    return get


class WatcherClient:
    """Fetches a single bridge message by nonce from the watcher API."""

    def __init__(
        self,
        base_url: str,
        *,
        getter: Optional[Getter] = None,
        timeout: float = 30.0,
    ) -> None:
        self._base = base_url.rstrip("/")
        self._timeout = timeout
        self._getter = getter or _default_getter(timeout)

    def get_message(
        self, nonce: str, *, source_chain: str = "bse"
    ) -> Optional[WatcherMessage]:
        """Return the attested message for ``nonce``, or ``None`` if not yet indexed.

        ``None`` means "keep polling" (the deposit is observed on Base but the
        validators have not attested yet); a returned message may still be
        pending -- check :attr:`WatcherMessage.is_sent` before claiming.
        """
        url = f"{self._base}/messages"
        params = {"source_chain": source_chain, "nonce": _hx(nonce)}
        try:
            data = self._getter(url, params)
        except WatcherError:
            raise
        except Exception as exc:  # noqa: BLE001 -- unify transport failures
            raise WatcherError(f"watcher get_message failed: {exc}") from exc

        want = _hx(nonce)
        messages = _messages_from(data)
        if not messages:
            return None
        for m in messages:
            if _hx(m.get("nonce")) == want:
                return _parse_message(m)
        # No exact nonce match -> the API returned something unrelated; treat as
        # not-yet-indexed rather than silently claiming the wrong message.
        return None

    def fetch_path(self, path: str) -> list:
        """Raw GET of ``base + path`` normalised to a list of message dicts.

        The altruistic relayer's and the activity monitor's fetcher: ``path``
        carries its own query string (e.g. ``/messages?source_chain=xch``).
        """
        url = f"{self._base}{path}"
        try:
            data = self._getter(url, {})
        except WatcherError:
            raise
        except Exception as exc:  # noqa: BLE001 -- unify transport failures
            raise WatcherError(f"watcher fetch_path failed: {exc}") from exc
        return _messages_from(data)
