"""Spot, volatility and the executable hedge range an option quote is built on.

Three numbers, from two places, with one rule between them:

* **Spot and volatility** come from an external series (CoinGecko's XCH/USD
  price and its hourly history). The Dexie book is not used to locate the
  price: it is thin, a one-unit offer can move its top, and its depth is mostly
  bots anchored on the same external number.
* **The executable range** comes from the Dexie book: what buying or selling
  the hedge size would really cost through whole third-party offers.
* **The rule:** if the external spot lies outside that range, or the range is
  wider than allowed, :meth:`Market.require_usable` refuses and nothing is
  quoted. That is also the only place a wUSDC.b discount shows up -- CoinGecko
  prices USDC, not the bridged token -- so a depeg stops the quotes instead of
  mispricing them.

Volatility is never taken from Dexie trades. Prints bounce across a spread of
several percent and come out at more than twice the real figure.
"""

from __future__ import annotations

import dataclasses
import json
import math
import statistics
import time
from collections.abc import Callable, Sequence
from typing import Any

COINGECKO_URL = "https://api.coingecko.com/api/v3"
DEXIE_URL = "https://api.dexie.space"

# url -> (status, body text)
Get = Callable[[str], tuple[int, str]]

_SECONDS_PER_YEAR = 365 * 24 * 3600


class MarketError(RuntimeError):
    """No usable market view. Do not quote."""


def _default_get(timeout: float) -> Get:
    import requests  # lazy: only when a real network call is made

    session = requests.Session()
    session.headers.update({"User-Agent": "XOPTrader", "Accept": "application/json"})

    def get(url: str) -> tuple[int, str]:
        response = session.get(url, timeout=timeout)
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


def executable_price(levels: Sequence[tuple[float, float]], size: float) -> float | None:
    """Average price of filling ``size`` through whole offers, best price first.

    ``levels`` are ``(price, size)`` already sorted best-first. Chia offers are
    all-or-nothing, so the last offer taken may overshoot ``size``; its whole
    amount is counted. Returns None if the side cannot cover ``size``.
    """
    filled = 0.0
    cost = 0.0
    for price, amount in levels:
        if price <= 0 or amount <= 0:
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
    as_of: float  # unix seconds of the spot reading

    def require_usable(
        self, *, now: float, max_age_seconds: float, max_spread_pct: float, require_book: bool
    ) -> None:
        """Raise :class:`MarketError` unless this view is safe to quote from."""
        if not math.isfinite(self.spot) or self.spot <= 0:
            raise MarketError("no spot price")
        if not math.isfinite(self.sigma) or self.sigma <= 0:
            raise MarketError("no volatility")
        if now - self.as_of > max_age_seconds:
            raise MarketError(f"the spot price is {int(now - self.as_of)} s old")
        if self.bid is None or self.ask is None:
            if require_book:
                raise MarketError("the Dexie book cannot fill the hedge size on both sides")
            return
        if self.bid > self.ask:
            raise MarketError("the Dexie book is crossed at the hedge size")
        middle = (self.bid + self.ask) / 2.0
        spread_pct = (self.ask - self.bid) / middle * 100.0
        if spread_pct > max_spread_pct:
            raise MarketError(f"the Dexie spread is {spread_pct:.1f}%, above {max_spread_pct}%")
        if not self.bid <= self.spot <= self.ask:
            raise MarketError(
                f"spot {self.spot:.4f} is outside the executable range "
                f"{self.bid:.4f}-{self.ask:.4f}; the quote asset may be off its peg"
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
        dexie_url: str = DEXIE_URL,
    ) -> None:
        self._asset = bytes(quote_asset_id).hex()
        self._hedge = float(hedge_size_xch)
        self._get = get or _default_get(timeout)
        self._clock = clock
        self._days = int(history_days)
        self._history_max_age = float(history_max_age_seconds)
        self._coingecko = coingecko_url.rstrip("/")
        self._dexie = dexie_url.rstrip("/")
        self._sigma: float | None = None
        self._sigma_at = 0.0

    def _json(self, url: str) -> Any:
        try:
            status, text = self._get(url)
        except Exception as exc:
            raise MarketError(f"request failed: {exc}") from exc
        if status != 200:
            raise MarketError(f"HTTP {status} from {url.split('?')[0]}")
        try:
            return json.loads(text)
        except ValueError as exc:
            raise MarketError(f"non-JSON answer from {url.split('?')[0]}") from exc

    def _spot(self) -> float:
        data = self._json(f"{self._coingecko}/simple/price?ids=chia&vs_currencies=usd")
        try:
            spot = float(data["chia"]["usd"])
        except (KeyError, TypeError, ValueError) as exc:
            raise MarketError("CoinGecko returned no XCH price") from exc
        if not math.isfinite(spot) or spot <= 0:
            raise MarketError("CoinGecko returned an unusable XCH price")
        return spot

    def _volatility(self, now: float) -> float:
        if self._sigma is not None and now - self._sigma_at <= self._history_max_age:
            return self._sigma
        data = self._json(
            f"{self._coingecko}/coins/chia/market_chart?vs_currency=usd&days={self._days}"
        )
        try:
            series = [(float(at) / 1000.0, float(price)) for at, price in data["prices"]]
        except (KeyError, TypeError, ValueError) as exc:
            raise MarketError("CoinGecko returned no price history") from exc
        self._sigma = realized_volatility(series)
        self._sigma_at = now
        return self._sigma

    def _side(self, offered: str, requested: str) -> list[tuple[float, float]]:
        """Open single-asset offers as ``(quote per XCH, XCH size)``."""
        data = self._json(
            f"{self._dexie}/v1/offers?offered={offered}&requested={requested}"
            "&status=0&page_size=100&compact=true"
        )
        levels = []
        for offer in data.get("offers", []) if isinstance(data, dict) else []:
            try:
                gives, wants = offer["offered"], offer["requested"]
                if len(gives) != 1 or len(wants) != 1:
                    continue
                give, want = float(gives[0]["amount"]), float(wants[0]["amount"])
            except (KeyError, TypeError, ValueError, IndexError):
                continue
            if give <= 0 or want <= 0:
                continue
            if offered == "xch":
                levels.append((want / give, give))  # sells XCH: an ask
            else:
                levels.append((give / want, want))  # buys XCH: a bid
        return levels

    def read(self) -> Market:
        now = self._clock()
        spot = self._spot()
        sigma = self._volatility(now)
        try:
            asks = sorted(self._side("xch", self._asset))
            bids = sorted(self._side(self._asset, "xch"), reverse=True)
        except MarketError:
            asks, bids = [], []
        return Market(
            spot=spot,
            sigma=sigma,
            bid=executable_price(bids, self._hedge),
            ask=executable_price(asks, self._hedge),
            as_of=now,
        )
