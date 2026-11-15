"""Client for xchoptions.com, the site that advertises option offers.

The site holds no keys and no options. It records public offer strings and
reads the chain, so everything it tells us is a pointer, never a fact: an offer
it returns is re-read from its own spends (:func:`inspect_offer`) and the option
it names is re-validated on chain (:func:`chain.load_option`) before either is
believed.

Its public API has three endpoints (docs: https://xchoptions.com/docs):

``POST /api/offers``          upload an offer string so the site lists it
``GET  /api/offers/{id}``     the offer string, while the offer is live
``POST /api/options/price``   the site's suggested price and its inputs

There is NO listing endpoint. The list of live offers exists only as the HTML of
``/options``, so :meth:`DeskClient.list_offers` parses that page. A page whose
shape is not recognised raises :class:`DeskLayoutError`; it is never read as
"no offers", which would look like an empty market.

Limits are 180 uploads and 210 reads a minute per client address. The client
keeps its own sliding window well inside them and backs off on a 429.
"""

from __future__ import annotations

import collections
import dataclasses
import json
import re
import time
from collections.abc import Callable
from typing import Any

from gui.services.options import signing

BASE_URL = "https://xchoptions.com"

# (method, url, json_body or None) -> (status, headers, body text)
Http = Callable[[str, str, dict | None], tuple[int, dict, str]]

_OFFER_LINK = re.compile(r"/offers/([1-9A-HJ-NP-Za-km-z]{32,64})")
_OPTION_LINK = re.compile(r"/option/([0-9a-f]{64})")
_TOTAL = re.compile(r"Active options[^0-9]{0,40}(\d+)\s*[–-]\s*(\d+)\s+of\s+(\d+)")
_EMPTY = re.compile(r"No (active )?options", re.IGNORECASE)
_MAX_PAGES = 50
_ASSERT_BEFORE_SECONDS_ABSOLUTE = 85
_MAX_COST = 11_000_000_000


class DeskError(RuntimeError):
    """The site could not be reached or answered with an error."""


class DeskLayoutError(DeskError):
    """A page did not have the shape this client knows. Nothing was concluded."""


class DeskRateLimitedError(DeskError):
    """Our own budget, or the site's, says wait. ``retry_after`` is in seconds."""

    def __init__(self, message: str, retry_after: float) -> None:
        super().__init__(message)
        self.retry_after = retry_after


def _sdk() -> Any:
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise DeskError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


def _default_http(timeout: float) -> Http:
    import requests  # lazy: only when a real network call is made

    session = requests.Session()
    session.headers.update({"User-Agent": "XOPTrader", "Accept": "application/json, text/html"})

    def call(method: str, url: str, body: dict | None) -> tuple[int, dict, str]:
        response = session.request(method, url, json=body, timeout=timeout)
        return response.status_code, dict(response.headers), response.text

    return call


class _Window:
    """At most ``limit`` calls in any ``period`` seconds."""

    def __init__(self, limit: int, period: float, clock: Callable[[], float]) -> None:
        self._limit = limit
        self._period = period
        self._clock = clock
        self._calls: collections.deque[float] = collections.deque()
        self._blocked_until = 0.0

    def acquire(self) -> None:
        now = self._clock()
        if now < self._blocked_until:
            raise DeskRateLimitedError("backing off after a 429", self._blocked_until - now)
        while self._calls and now - self._calls[0] >= self._period:
            self._calls.popleft()
        if len(self._calls) >= self._limit:
            raise DeskRateLimitedError(
                "request budget used up", self._period - (now - self._calls[0])
            )
        self._calls.append(now)

    def block(self, seconds: float) -> None:
        self._blocked_until = max(self._blocked_until, self._clock() + max(0.0, seconds))


@dataclasses.dataclass(frozen=True)
class Listing:
    """What ``/options`` currently shows."""

    offer_ids: tuple[str, ...]  # options with a live offer
    unoffered_launcher_ids: tuple[bytes, ...]  # recorded options nobody is offering
    total: int  # the count the page itself states


@dataclasses.dataclass(frozen=True)
class Upload:
    offer_id: str
    launcher_id: bytes
    created: bool


@dataclasses.dataclass(frozen=True)
class Suggestion:
    """The site's model inputs and its suggested premium for one option."""

    priceable: bool
    spot: float | None
    sigma: float | None
    rate: float | None
    premium: float | None  # total for the contract, in the quote asset


class DeskClient:
    def __init__(
        self,
        http: Http | None = None,
        *,
        base_url: str = BASE_URL,
        timeout: float = 20.0,
        clock: Callable[[], float] = time.monotonic,
        reads_per_minute: int = 120,
        uploads_per_minute: int = 90,
    ) -> None:
        self._http = http or _default_http(timeout)
        self._base = base_url.rstrip("/")
        self._reads = _Window(reads_per_minute, 60.0, clock)
        self._uploads = _Window(uploads_per_minute, 60.0, clock)

    # -- transport ----------------------------------------------------------

    def _call(
        self, window: _Window, method: str, path: str, body: dict | None = None
    ) -> tuple[int, str]:
        window.acquire()
        try:
            status, headers, text = self._http(method, self._base + path, body)
        except DeskError:
            raise
        except Exception as exc:
            raise DeskError(f"{method} {path} failed: {exc}") from exc
        lowered = {str(key).lower(): value for key, value in (headers or {}).items()}
        if status == 429:
            try:
                wait = float(lowered.get("retry-after", 60))
            except (TypeError, ValueError):
                wait = 60.0
            window.block(wait)
            raise DeskRateLimitedError(f"{method} {path}: rate limited by the site", wait)
        remaining = _whole_number(lowered.get("x-ratelimit-remaining"))
        if remaining is not None and remaining <= 2:
            window.block(60.0)
        return int(status), text

    @staticmethod
    def _json(path: str, status: int, text: str) -> dict:
        try:
            data = json.loads(text)
        except ValueError as exc:
            raise DeskError(f"{path}: HTTP {status} with a non-JSON body") from exc
        if not isinstance(data, dict):
            raise DeskError(f"{path}: HTTP {status} with an unexpected JSON shape")
        return data

    # -- listing ------------------------------------------------------------

    def list_offers(self) -> Listing:
        """Every offer id and unoffered option on ``/options``, across its pages."""
        offers: dict[str, None] = {}
        unoffered: dict[bytes, None] = {}
        total = 0
        for page in range(1, _MAX_PAGES + 1):
            path = "/options" if page == 1 else f"/options?page={page}"
            status, text = self._call(self._reads, "GET", path)
            if status != 200:
                raise DeskError(f"GET {path}: HTTP {status}")
            match = _TOTAL.search(_strip_tags(text))
            if match is None:
                if page == 1 and _EMPTY.search(_strip_tags(text)):
                    return Listing((), (), 0)
                raise DeskLayoutError(f"{path}: the options table was not found")
            last, total = int(match.group(2)), int(match.group(3))
            before = len(offers) + len(unoffered)
            for offer_id in _OFFER_LINK.findall(text):
                offers.setdefault(offer_id)
            for launcher in _OPTION_LINK.findall(text):
                unoffered.setdefault(bytes.fromhex(launcher))
            if last >= total:
                break
            if len(offers) + len(unoffered) == before:
                raise DeskLayoutError(f"{path}: the page listed nothing new")
        else:
            raise DeskLayoutError("/options: more pages than expected")
        # Several offers can be open for one option, so offers can outnumber
        # options, but a listing with rows and no links means the markup moved.
        if total > 0 and not offers and not unoffered:
            raise DeskLayoutError("/options: rows are listed but no links were found")
        return Listing(tuple(offers), tuple(unoffered), total)

    # -- offers -------------------------------------------------------------

    def fetch_offer(self, offer_id: str) -> str | None:
        """The offer string, or None once the site no longer considers it live."""
        if not _OFFER_LINK.fullmatch("/offers/" + offer_id):
            raise DeskError("malformed offer id")
        path = f"/api/offers/{offer_id}"
        status, text = self._call(self._reads, "GET", path)
        if status == 404:
            return None
        if status != 200:
            raise DeskError(f"GET {path}: HTTP {status}")
        offer = self._json(path, status, text).get("offer")
        if not isinstance(offer, str) or not offer.startswith("offer1"):
            raise DeskError(f"GET {path}: no offer string in the answer")
        return offer

    def upload_offer(self, offer: str) -> Upload:
        """List an offer on the site. The site reads its terms from the string itself."""
        path = "/api/offers"
        status, text = self._call(self._uploads, "POST", path, {"offer": offer})
        data = self._json(path, status, text)
        if status != 200:
            raise DeskError(f"POST {path}: HTTP {status}: {data.get('message') or 'refused'}")
        try:
            record = data["offer"]
            return Upload(
                offer_id=str(record["offerId"]),
                launcher_id=bytes.fromhex(str(record["launcherId"])),
                created=bool(data.get("option", {}).get("created", False)),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise DeskError(f"POST {path}: unexpected answer shape") from exc

    def suggested_price(self, launcher_id: bytes) -> Suggestion:
        """The site's Black-Scholes suggestion for one option, and its inputs."""
        path = "/api/options/price"
        status, text = self._call(
            self._reads, "POST", path, {"option_id": bytes(launcher_id).hex()}
        )
        data = self._json(path, status, text)
        if status != 200:
            raise DeskError(f"POST {path}: HTTP {status}: {data.get('message') or 'refused'}")
        basis = data.get("basis") or {}
        suggested = data.get("suggested") or {}
        primary = suggested.get("primary") or {}

        def number(value: Any) -> float | None:
            try:
                return None if value is None else float(value)
            except (TypeError, ValueError):
                return None

        return Suggestion(
            priceable=bool(data.get("priceable", suggested != {})),
            spot=number(basis.get("spot")),
            sigma=number(basis.get("sigma")),
            rate=number(basis.get("riskFreeRate")),
            premium=number(primary.get("amount")),
        )


def _whole_number(value: Any) -> int | None:
    """A header value as an integer, or None when it is absent or not a number."""
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _before_seconds(condition: Any) -> int | None:
    """The timestamp of an ``ASSERT_BEFORE_SECONDS_ABSOLUTE`` condition, else None."""
    try:
        if condition.first().to_int() != _ASSERT_BEFORE_SECONDS_ABSOLUTE:
            return None
        seconds = condition.rest().first().to_int()
    except Exception:  # not a (opcode . args) pair
        return None
    return None if seconds is None else int(seconds)


def _strip_tags(text: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", text))


# ---------------------------------------------------------------------------
# Reading an offer string ourselves
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class OfferView:
    """What an offer string says, read from its own spends."""

    launcher_id: bytes
    option_coin_id: bytes
    requested_asset_id: bytes | None  # None = XCH
    requested_amount: int  # mojos
    expires_at_seconds: int | None
    signature_valid: bool


def inspect_offer(
    offer: str,
    *,
    agg_sig_me_additional_data: bytes = signing.MAINNET_AGG_SIG_ME_ADDITIONAL_DATA,
) -> OfferView:
    """Decode an option offer: which option it gives, what it asks, when it dies.

    Only the simple shape the desk lists is accepted: one option given for one
    asset. Anything else raises :class:`DeskError`. ``signature_valid`` is False
    for an offer whose maker signature does not check out, which can never
    settle.
    """
    sdk = _sdk()
    clvm = sdk.Clvm()
    try:
        bundle = sdk.decode_offer(offer)
        parsed = sdk.Offer.from_spend_bundle(clvm, bundle)
        inputs = parsed.spend_bundle().coin_spends
        requested = parsed.requested_payments()
    except Exception as exc:
        raise DeskError(f"not a readable offer: {exc}") from exc

    settlement = bytes(sdk.Constants.settlement_payment_hash())
    given = []
    expiry: int | None = None
    for spend in inputs:
        try:
            program = clvm.deserialize(bytes(spend.puzzle_reveal))
            solution = clvm.deserialize(bytes(spend.solution))
            puzzle = program.puzzle()
            option = puzzle.parse_option(spend.coin, solution)
            child = puzzle.parse_child_option(spend.coin, solution) if option else None
            conditions = program.run(solution, _MAX_COST, False).value.to_list() or []
        except Exception as exc:
            raise DeskError(f"the offer's spends are unreadable: {exc}") from exc
        if child is not None and bytes(child.info.p2_puzzle_hash) == settlement:
            given.append((bytes(option.option.info.launcher_id), bytes(spend.coin.coin_id())))
        for condition in conditions:
            seconds = _before_seconds(condition)
            if seconds is not None:
                expiry = seconds if expiry is None else min(expiry, seconds)
    if len(given) != 1:
        raise DeskError("the offer does not give exactly one option")

    asks: list[tuple[bytes | None, int]] = []
    for payment in requested.xch():
        asks.append((None, sum(int(item.amount) for item in payment.payments)))
    for asset_id in requested.cat_asset_ids():
        for payment in requested.cats(asset_id):
            asks.append((bytes(asset_id), sum(int(item.amount) for item in payment.payments)))
    if requested.nft_launcher_ids() or requested.option_launcher_ids() or len(asks) != 1:
        raise DeskError("the offer does not ask for exactly one asset")

    try:
        valid = signing.verify(
            signing.required_signatures(inputs, agg_sig_me_additional_data),
            bytes(bundle.aggregated_signature.to_bytes()),
        )
    except signing.SigningError:
        valid = False
    launcher_id, coin_id = given[0]
    return OfferView(
        launcher_id=launcher_id,
        option_coin_id=coin_id,
        requested_asset_id=asks[0][0],
        requested_amount=asks[0][1],
        expires_at_seconds=expiry,
        signature_valid=valid,
    )
