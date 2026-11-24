"""Spot, volatility and the executable hedge range an option quote is built on.

Three numbers, from two places, with one rule between them:

* **Spot and volatility** come from an external series (CoinGecko's XCH/USD
  price and its hourly history). The Dexie book is not used to locate the
  price: it is thin, a one-unit offer can move its top, and its depth is mostly
  bots anchored on the same external number.
* **The executable range** comes from the Dexie book: what buying or selling
  the hedge size would really cost through whole third-party offers.
* **The rule:** if the external spot lies further outside that range than
  ``options.market.peg_tolerance_pct`` (3% by default), or the range is wider
  than allowed, :meth:`Market.require_usable` refuses and nothing is quoted.
  That is also the only place a wUSDC.b discount shows up -- CoinGecko prices
  USDC, not the bridged token -- so a depeg stops the quotes instead of
  mispricing them. The tolerance is for stale Dexie offers, which sit until
  someone takes them: a percent or two off the market is common and is no
  depeg. Quotes are priced on the external spot either way.

Volatility is never taken from Dexie trades. Prints bounce across a spread of
several percent and come out at more than twice the real figure.

A view is as old as its spot observation, not as the request that fetched it:
``Market.as_of`` is the time CoinGecko says the price was last updated. A feed
that answers promptly with an old price is therefore refused as stale, and one
that does not say when its price was observed is refused outright.

OUR OWN OFFERS ARE NOT LIQUIDITY. With ``options.wallet.share_trading_wallet``
the book's wallet is the engine's, and the engine leaves its offers resting on
Dexie when it is stopped. Counted in the range, they pin it: on the first live
run (2026-11-15) the best ask sat at 1.1700 and the best bid at 1.1413 for
hours while the market moved, most likely the stopped engine's, and quoting
stopped when spot rose past that ask. :class:`MarketSource` is handed the
trade ids of the wallet's own open offers and leaves those out. Dexie's
``trade_id`` is the offer's name, the same id the wallet keeps it under.
"""

from __future__ import annotations

import dataclasses
import json
import logging
import math
import statistics
import time
from collections.abc import Callable, Iterable, Sequence
from typing import Any

COINGECKO_URL = "https://api.coingecko.com/api/v3"
DEXIE_URL = "https://api.dexie.space"

# url -> (status, body text)
#: ``get(url)`` or ``get(url, headers)`` -> ``(status, body)``.
Get = Callable[..., tuple[int, str]]

_SECONDS_PER_YEAR = 365 * 24 * 3600
# An observation stamped later than this past our own clock is not believed.
_MAX_CLOCK_SKEW_SECONDS = 300
# Dexie answers a page of offers at a time; at most this many pages are read
# for one side of one step.
_PAGE_SIZE = 100
_MAX_PAGES = 5

log = logging.getLogger(__name__)


def _bare_hex(value: str) -> str:
    text = value.strip().lower()
    return text[2:] if text.startswith("0x") else text


#: Why a view was refused, in a few words that stay the same from one step to
#: the next (the message itself carries the numbers). The journal and the
#: report group by these.
NO_DATA = "market data unavailable"
STALE = "spot price too old"
NO_BOOK = "Dexie book too thin"
CROSSED = "Dexie book crossed"
TOO_WIDE = "Dexie spread too wide"
OFF_PEG = "spot outside the Dexie range"


class MarketError(RuntimeError):
    """No usable market view. Do not quote.

    ``reason`` is one of the short reasons above; the message says the rest.
    """

    def __init__(self, message: str, reason: str = NO_DATA) -> None:
        super().__init__(message)
        self.reason = reason


def _default_get(timeout: float) -> Get:
    import requests  # lazy: only when a real network call is made

    session = requests.Session()
    session.headers.update({"User-Agent": "XOPTrader", "Accept": "application/json"})

    def get(url: str, headers: dict[str, str] | None = None) -> tuple[int, str]:
        response = session.get(url, timeout=timeout, headers=headers)
        return response.status_code, response.text

    return get


def realized_volatility(series: Sequence[tuple[float, float]]) -> float:
    """Annualised volatility of a ``(timestamp_seconds, price)`` series.

    The sampling interval is measured from the series itself (its median
    spacing), so an hourly and a five-minute series are both annualised
    correctly. Needs at least 24 returns.
    """
    points = sorted((float(at), float(price)) for at, price in series if price and price > 0)
    if len(points) < 25:
        raise MarketError("the price history is too short for a volatility estimate")
    spacings = [later[0] - earlier[0] for earlier, later in zip(points, points[1:], strict=False)]
    interval = statistics.median(spacings)
    if interval <= 0:
        raise MarketError("the price history has no usable timestamps")
    returns = [
        math.log(later[1] / earlier[1]) for earlier, later in zip(points, points[1:], strict=False)
    ]
    sigma = statistics.pstdev(returns) * math.sqrt(_SECONDS_PER_YEAR / interval)
    if not math.isfinite(sigma) or sigma <= 0:
        raise MarketError("the price history gives no volatility")
    return sigma


def _beyond(gap_pct: float, tolerance_pct: float) -> bool:
    """[review #263] Whether ``gap_pct`` is past ``tolerance_pct``; a gap at it is within.

    Gaps are worked out in floating point ((1.0 - 0.97) / 1.0 * 100 is
    3.0000000000000027), so one within a part in a billion of the tolerance
    counts as at it. A tolerance of 0 stays strict: any gap at all is past it.
    """
    return gap_pct > tolerance_pct and not math.isclose(gap_pct, tolerance_pct, rel_tol=1e-9)


def executable_price(levels: Sequence[tuple[float, float]], size: float) -> float | None:
    """Average price of filling ``size`` through whole offers, best price first.

    ``levels`` are ``(price, size)`` already sorted best-first. Chia offers are
    all-or-nothing, so the last offer taken may overshoot ``size``; its whole
    amount is counted. Returns None if the side cannot cover ``size``.
    """
    filled = 0.0
    cost = 0.0
    for price, amount in levels:
        # [review #263] NaN passes a `<= 0` test: an unusable level is left out.
        if not (math.isfinite(price) and math.isfinite(amount)) or price <= 0 or amount <= 0:
            continue
        filled += amount
        cost += price * amount
        if filled >= size:
            return cost / filled
    return None


@dataclasses.dataclass(frozen=True)
class Market:
    spot: float  # quote asset per XCH, from the external feed
    sigma: float  # annualised
    bid: float | None  # what selling the hedge size on Dexie would fetch
    ask: float | None  # what buying it would cost
    as_of: float  # unix seconds at which the feed observed the spot, not when we asked

    def gaps_pct(self) -> tuple[float, float]:
        """[PEG] How far the Dexie book is crossed, and how far spot sits outside it, in %.

        Both are 0 for a book whose bid is under its ask with spot between
        them, and for a view with no book. The range of a crossed book runs
        from its lower price to its higher one. Prices are taken as finite and
        positive: :meth:`require_usable` refuses a view whose are not.
        """
        if self.bid is None or self.ask is None:
            return 0.0, 0.0
        low, high = min(self.bid, self.ask), max(self.bid, self.ask)
        crossed = (self.bid - self.ask) / ((self.bid + self.ask) / 2.0) * 100.0
        if self.spot < low:
            outside = (low - self.spot) / low * 100.0
        elif self.spot > high:
            outside = (self.spot - high) / high * 100.0
        else:
            outside = 0.0
        return max(crossed, 0.0), outside

    def require_usable(
        self,
        *,
        now: float,
        max_age_seconds: float,
        max_spread_pct: float,
        require_book: bool,
        peg_tolerance_pct: float = 0.0,
    ) -> None:
        """Raise :class:`MarketError` unless this view is safe to quote from.

        [PEG] ``peg_tolerance_pct`` is how far a stale Dexie offer may be off
        before the view is refused: how far the external spot may sit outside
        the executable range, and how far the book may be crossed. 0, the
        default here for a caller that does not say, is the strict rule: spot
        inside the range, and no crossing at all. The book passes
        ``options.market.peg_tolerance_pct``, 3% when left out.
        """
        if not math.isfinite(self.spot) or self.spot <= 0:
            raise MarketError("no spot price")
        if not math.isfinite(self.sigma) or self.sigma <= 0:
            raise MarketError("no volatility")
        if now - self.as_of > max_age_seconds:
            raise MarketError(f"the spot price is {int(now - self.as_of)} s old", STALE)
        if self.bid is None or self.ask is None:
            if require_book:
                raise MarketError(
                    "the Dexie book cannot fill the hedge size on both sides", NO_BOOK
                )
            return
        if not all(math.isfinite(price) and price > 0 for price in (self.bid, self.ask)):
            # [review #263] Every comparison below is False for NaN: the strict
            # range test refused one only by accident, and the gaps would let
            # it through. Fail closed, whatever the tolerance.
            raise MarketError(
                f"the Dexie book gave an unusable price ({self.bid}/{self.ask})", NO_DATA
            )
        # [PEG] A stale offer is not a depeg. Dexie offers sit until taken or
        # cancelled, and small ones are not worth anyone's while to take: on
        # 2026-11-24 a ladder of 5 XCH bids, 0.3% apart from 0.9686 down,
        # stayed put while XCH fell 3.5%, crossing the book or leaving spot up
        # to 2.4% under its top bid, and the strict rule stopped the book for
        # hours. Dexie's fills meanwhile straddled the external price.
        allowed = f", more than the {peg_tolerance_pct:g}% allowed" if peg_tolerance_pct else ""
        crossed_pct, outside_pct = self.gaps_pct()
        if _beyond(crossed_pct, peg_tolerance_pct):
            raise MarketError(
                f"the Dexie book is crossed by {crossed_pct:.2f}% at the hedge size{allowed}",
                CROSSED,
            )
        middle = (self.bid + self.ask) / 2.0
        spread_pct = (self.ask - self.bid) / middle * 100.0
        if spread_pct > max_spread_pct:
            raise MarketError(
                f"the Dexie spread is {spread_pct:.1f}%, above {max_spread_pct}%", TOO_WIDE
            )
        if _beyond(outside_pct, peg_tolerance_pct):
            low, high = min(self.bid, self.ask), max(self.bid, self.ask)
            raise MarketError(
                f"spot {self.spot:.4f} is {outside_pct:.2f}% outside the executable range "
                f"{low:.4f}-{high:.4f}{allowed}; the quote asset may be off its peg",
                OFF_PEG,
            )


class MarketSource:
    """Reads :class:`Market` from CoinGecko and Dexie. Transport is injected."""

    def __init__(
        self,
        quote_asset_id: bytes,
        *,
        hedge_size_xch: float,
        get: Get | None = None,
        timeout: float = 20.0,
        clock: Callable[[], float] = time.time,
        history_days: int = 30,
        history_max_age_seconds: float = 6 * 3600,
        coingecko_url: str = COINGECKO_URL,
        coingecko_api_key: str = "",
        dexie_url: str = DEXIE_URL,
        own_offers: Callable[[], Iterable[bytes]] | None = None,
    ) -> None:
        """``own_offers`` gives the trade ids of the wallet's own open offers.

        Those are left out of the executable range. When it fails, every
        offer is counted, as before it existed, and the failure is logged.
        """
        self._asset = bytes(quote_asset_id).hex()
        self._hedge = float(hedge_size_xch)
        self._get = get or _default_get(timeout)
        self._clock = clock
        self._days = int(history_days)
        self._history_max_age = float(history_max_age_seconds)
        self._coingecko = coingecko_url.rstrip("/")
        # The key the rest of the app uses (coingecko.api_key), sent as a
        # header and never in the address, which ends up in error messages.
        key = str(coingecko_api_key or "").strip()
        self._coingecko_headers = {"x-cg-demo-api-key": key} if key else None
        self._dexie = dexie_url.rstrip("/")
        self._own_offers = own_offers
        self._own_failed = False  # logged once, not every step
        self._sigma: float | None = None
        self._sigma_at = 0.0
        #: How many of the wallet's own offers the last read left out.
        self.left_out = 0

    def _json(self, url: str, headers: dict[str, str] | None = None) -> Any:
        try:
            status, text = self._get(url, headers) if headers else self._get(url)
        except Exception as exc:
            raise MarketError(f"request failed: {exc}") from exc
        if status != 200:
            raise MarketError(f"HTTP {status} from {url.split('?')[0]}")
        try:
            return json.loads(text)
        except ValueError as exc:
            raise MarketError(f"non-JSON answer from {url.split('?')[0]}") from exc

    def _spot(self, now: float) -> tuple[float, float]:
        """The spot price and the time the feed observed it."""
        # precision=full: without it the price comes back to the cent (1.15 for
        # 1.1512...), steps of about 0.9% that move strikes and premiums in jumps.
        # Seen on the first live run, 2026-11-15.
        data = self._json(
            f"{self._coingecko}/simple/price?ids=chia&vs_currencies=usd"
            "&include_last_updated_at=true&precision=full",
            self._coingecko_headers,
        )
        try:
            spot = float(data["chia"]["usd"])
        except (KeyError, TypeError, ValueError) as exc:
            raise MarketError("CoinGecko returned no XCH price") from exc
        if not math.isfinite(spot) or spot <= 0:
            raise MarketError("CoinGecko returned an unusable XCH price")
        try:
            observed = float(data["chia"]["last_updated_at"])
        except (KeyError, TypeError, ValueError) as exc:
            # Without it a stale price cannot be told from a fresh one.
            raise MarketError("CoinGecko did not say when the XCH price was observed") from exc
        if not math.isfinite(observed) or observed <= 0:
            raise MarketError("CoinGecko returned an unusable time for the XCH price")
        if observed - now > _MAX_CLOCK_SKEW_SECONDS:
            raise MarketError(
                f"the XCH price is stamped {int(observed - now)} s in the future; "
                "check this machine's clock"
            )
        return spot, min(observed, now)

    def _volatility(self, now: float) -> float:
        if self._sigma is not None and now - self._sigma_at <= self._history_max_age:
            return self._sigma
        data = self._json(
            f"{self._coingecko}/coins/chia/market_chart?vs_currency=usd&days={self._days}",
            self._coingecko_headers,
        )
        try:
            series = [(float(at) / 1000.0, float(price)) for at, price in data["prices"]]
        except (KeyError, TypeError, ValueError) as exc:
            raise MarketError("CoinGecko returned no price history") from exc
        self._sigma = realized_volatility(series)
        self._sigma_at = now
        return self._sigma

    def _ours(self) -> frozenset[str]:
        """Trade ids of the wallet's own open offers, as bare lowercase hex."""
        if self._own_offers is None:
            return frozenset()
        try:
            ids = frozenset(bytes(item).hex() for item in self._own_offers())
        except Exception as exc:  # noqa: BLE001 - any failure: count every offer, as before
            if not self._own_failed:
                log.warning(
                    "[options] could not ask the wallet for its own offers (%s); "
                    "the Dexie range counts every offer until it answers",
                    exc,
                )
            self._own_failed = True
            return frozenset()
        if self._own_failed:
            log.info("[options] the wallet lists its own offers again")
        self._own_failed = False
        return ids

    def _side(
        self, offered: str, requested: str, ours: frozenset[str] = frozenset()
    ) -> list[tuple[float, float]]:
        """Open single-asset offers as ``(quote per XCH, XCH size)``, leaving out ``ours``.

        Pages are read best price first until what is kept covers the hedge
        size, the side runs out, or ``_MAX_PAGES`` have been read. One page
        used to be all: leaving out our own offers could then leave a side
        short of the hedge size with others' offers on the next page.
        """
        levels: list[tuple[float, float]] = []
        seen: set[str] = set()
        size = 0.0
        for page in range(1, _MAX_PAGES + 1):
            data = self._json(
                f"{self._dexie}/v1/offers?offered={offered}&requested={requested}"
                f"&status=0&page_size={_PAGE_SIZE}&page={page}&compact=true&sort=price"
            )
            offers = data.get("offers") if isinstance(data, dict) else None
            offers = offers if isinstance(offers, list) else []
            for offer in offers:
                trade_id = offer.get("trade_id") if isinstance(offer, dict) else None
                if isinstance(trade_id, str):
                    key = _bare_hex(trade_id)
                    if key in seen:  # moved up a page between two reads
                        continue
                    seen.add(key)
                    if key in ours:
                        self.left_out += 1
                        continue
                try:
                    gives, wants = offer["offered"], offer["requested"]
                    if len(gives) != 1 or len(wants) != 1:
                        continue
                    give, want = float(gives[0]["amount"]), float(wants[0]["amount"])
                except (KeyError, TypeError, ValueError, IndexError):
                    continue
                # [review #263] json reads NaN and Infinity; neither is an offer.
                if not (math.isfinite(give) and math.isfinite(want)) or give <= 0 or want <= 0:
                    continue
                if offered == "xch":
                    levels.append((want / give, give))  # sells XCH: an ask
                    size += give
                else:
                    levels.append((give / want, want))  # buys XCH: a bid
                    size += want
            count = data.get("count") if isinstance(data, dict) else None
            last = isinstance(count, int) and page * _PAGE_SIZE >= count
            if size >= self._hedge or len(offers) < _PAGE_SIZE or last:
                break
        return levels

    def read(self) -> Market:
        now = self._clock()
        spot, observed = self._spot(now)
        sigma = self._volatility(now)
        ours = self._ours()
        self.left_out = 0
        try:
            asks = sorted(self._side("xch", self._asset, ours))
            bids = sorted(self._side(self._asset, "xch", ours), reverse=True)
        except MarketError:
            asks, bids = [], []
        return Market(
            spot=spot,
            sigma=sigma,
            bid=executable_price(bids, self._hedge),
            ask=executable_price(asks, self._hedge),
            as_of=observed,
        )
