"""The live best bid/offer, which the venue does publish after all.

WHY THIS EXISTS. ``cross_backoff.py`` is built on a stated premise:

    "Permuto publishes no L2/orderbook/ticker route -- probed 2026-08-31,
     they all fall through to the SPA's HTML."

That is false, and it cost the account every depth-second of 2026-09-02.
``GET /info/l2/{market}?levels=N`` returns a real book -- it is documented in
``docs/permuto-api-reference.md`` and was verified live:

    NVDA-VOL-PERP  bids[0] = 0.074067  asks = []
    QQQ-VOL-PERP   bids[0] = 0.035700  asks = []
    TSLA-VOL-PERP  bids[0] = 0.079073  asks = []

Because the book was assumed invisible, ``CrossBackoff`` learns the resting
price by stepping 0.25% per refusal and clamping at ``headroom_pct``. Against
bids parked at exactly +2.00% -- the ring ceiling -- that controller
saturates: it reaches its ceiling, keeps crossing, and retries forever. The
2026-09-02 log shows both markets pinned at their headroom with every ask
refused, tick after tick, for hours.

WHAT THIS ANSWERS, AND WHAT IT DOES NOT. One question only: *given the book,
is there a tick-aligned price that both RESTS (does not cross) and EARNS
(inside the credit ring)?* It is arithmetic on an observation, not a
forecast, and it holds no opinion about size, direction or risk -- those stay
with ``risk.assess()`` and the curfew.

THE WINDOW IS OFTEN EMPTY, AND SAYING SO IS THE POINT. For an ask the
constraint is ``best_bid < price <= oracle * (1 + ring_pct/100)``. On
2026-09-02 that window was 0.000002 wide on NVDA against a 0.0001 tick --
zero placeable prices. No spread, backoff or size setting can conjure a tick
that does not exist, and the honest answer is to report the window shut and
stop spending rate-limit tokens on legs the venue will refuse. A controller
that cannot see the book cannot tell "try harder" from "impossible"; this
can.
"""

from __future__ import annotations

import json
import logging
import math
import urllib.request
from dataclasses import dataclass
from typing import Any, Optional

_log = logging.getLogger(__name__)

DEFAULT_RING_PCT = 2.0
MAX_LEGAL_RING_PCT = 5.0  # VENUE_BAND_PCT


def active_ring_pct(meta: Any, default: float = DEFAULT_RING_PCT) -> tuple[float, str]:
    """``(percent, source)`` extracted recursively from venue metadata."""
    stack = [meta]
    while stack:
        node = stack.pop()
        if isinstance(node, dict):
            if "vol_aggressive_ring_pct" in node:
                raw_val = node["vol_aggressive_ring_pct"]
                if not isinstance(raw_val, bool):
                    try:
                        value = float(raw_val)
                        if math.isfinite(value) and 0.0 < value <= MAX_LEGAL_RING_PCT:
                            return value, "venue"
                    except (TypeError, ValueError):
                        pass
            stack.extend(node.values())
        elif isinstance(node, list):
            stack.extend(node)
    return default, "default"


#: Levels requested per side. We only need the top of book (level 0) to
#: calculate resting BBO and earning windows. Requesting 1 level minimizes
#: response payload size and network flight time within the sub-tick budget.
_LEVELS = 1

_HEADERS = {
    "User-Agent": "Mozilla/5.0 (XOPTrader)",
    "Accept": "application/json",
}


@dataclass(frozen=True)
class Book:
    """Top of book for one market. ``None`` on a side means empty."""

    market: str
    best_bid: Optional[float]
    best_ask: Optional[float]

    @property
    def one_sided(self) -> bool:
        return (self.best_bid is None) != (self.best_ask is None)


@dataclass(frozen=True)
class Window:
    """Where a resting, earning leg may be placed on one side.

    ``ticks`` is the count of tick-aligned prices in the window. Zero means
    the side is unplaceable right now -- the honest, actionable answer.
    """

    side: str
    low: float
    high: float
    ticks: int
    first: float = 0.0
    last: float = 0.0

    @property
    def open(self) -> bool:
        return self.ticks > 0


#: Tolerance, in ticks, absorbed before a quantised price is judged outside
#: its window.
#:
#: DEFENSIVE, NOT A FIX FOR AN OBSERVED DEFECT -- stated precisely because an
#: earlier version of this comment claimed otherwise. Prices here are decimals
#: a binary float cannot hold exactly (``0.0868 + 0.0001`` is
#: ``0.08690000000000001``), so a floor-division can in principle land a tick
#: out. It was suspected of discarding a placeable one-tick window on
#: 2026-09-02, but that was misattribution: the oracle moved between the two
#: samples being compared, and setting ``_EPS = 0.0`` reproduces neither the
#: original result nor any difference across the observed books. Kept because
#: the hazard is real and the guard costs nothing; do not cite it as a fix.
_EPS = 1e-6


def _quantise(price: float, tick_size: float, *, up: bool) -> float:
    """Snap ``price`` to the tick grid, absorbing float representation error.

    The nudge is applied BEFORE the rounding rather than compared after, so
    a value that is a hair past a tick boundary snaps to that boundary
    instead of jumping a full tick in the direction of the rounding.
    """
    ticks = price / tick_size
    ticks = -(-(ticks - _EPS) // 1.0) if up else (ticks + _EPS) // 1.0
    return ticks * tick_size


def _grid_bounds(low: float, high: float,
                 tick_size: float, *,
                 strict_low: bool = False,
                 strict_high: bool = False) -> tuple[float, float]:
    """First and last legal grid prices for a given interval."""
    epsilon = _EPS * tick_size
    first = _quantise(low, tick_size, up=True)
    last = _quantise(high, tick_size, up=False)
    if strict_low and first <= low + epsilon:
        first += tick_size
    if strict_high and last >= high - epsilon:
        last -= tick_size
    return first, last


def _get(url: str, timeout: float) -> dict:
    req = urllib.request.Request(url, headers=_HEADERS, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def _first_price(levels) -> Optional[float]:
    """Top-of-book price, or None. Tolerates the venue's string decimals."""
    if not isinstance(levels, list) or not levels:
        return None
    if not isinstance(levels[0], dict):
        return None
    try:
        val = float(levels[0]["price"])
        if math.isfinite(val) and val > 0.0:
            return val
    except (IndexError, KeyError, TypeError, ValueError):
        # Malformed or unparseable price entry; return None so caller treats it as unreadable
        return None
    return None


def fetch_book(market: str, *, base_url: str,
               timeout: float = 10.0) -> Optional[Book]:
    """Top of book for ``market``, or None if the venue did not answer.

    Never raises: a book we could not read must degrade to the existing
    blind behaviour, not take the quoting loop down with it.
    """
    url = "%s/info/l2/%s?levels=%d" % (base_url.rstrip("/"), market, _LEVELS)
    try:
        payload = _get(url, timeout)
    except Exception as exc:  # noqa: BLE001 - a missing book is not fatal
        _log.debug("permuto: L2 fetch failed for %s: %s", market, exc)
        return None

    if not isinstance(payload, dict) or payload.get("error"):
        _log.debug("permuto: L2 payload invalid for %s: %r", market, payload)
        return None

    raw_bids = payload.get("bids")
    raw_asks = payload.get("asks")
    if not isinstance(raw_bids, list) or not isinstance(raw_asks, list):
        _log.debug("permuto: L2 bids/asks not lists for %s: %r", market, payload)
        return None

    best_bid = None
    if raw_bids:
        best_bid = _first_price(raw_bids)
        if best_bid is None:
            _log.debug("permuto: L2 nonempty bids unparseable for %s: %r", market, raw_bids[:1])
            return None

    best_ask = None
    if raw_asks:
        best_ask = _first_price(raw_asks)
        if best_ask is None:
            _log.debug("permuto: L2 nonempty asks unparseable for %s: %r", market, raw_asks[:1])
            return None

    return Book(
        market=market,
        best_bid=best_bid,
        best_ask=best_ask,
    )


def earning_window(side: str, oracle: float, book: Book, *,
                   ring_pct: float, tick_size: float,
                   allow_subtick: bool = False) -> Window:
    """The prices on ``side`` that would both rest and earn credit.

    An ask must sit ABOVE the best bid or the venue refuses it post-only,
    and at or inside ``oracle * (1 + ring_pct/100)`` or it earns nothing.
    Bids mirror it. An empty opposing side removes only the crossing bound;
    the ring still applies, which is why a venue-wide empty ask side does
    not mean asks are free to place anywhere.
    """
    if side not in ("bid", "ask"):
        raise ValueError("side must be 'bid' or 'ask', got %r" % (side,))
    if not (math.isfinite(oracle) and oracle > 0.0
            and math.isfinite(ring_pct) and ring_pct > 0.0
            and math.isfinite(tick_size) and tick_size > 0.0):
        return Window(side=side, low=0.0, high=0.0, ticks=0, first=0.0, last=0.0)

    # The ring is a band AROUND the oracle and bounds BOTH sides. An earlier
    # revision applied only the near edge per side, which let a bid sit above
    # +ring_pct: it would rest happily and earn nothing, the exact "failure
    # wearing success's clothes" cross_backoff.py warns about.
    ring_lo = oracle * (1.0 - ring_pct / 100.0)
    ring_hi = oracle * (1.0 + ring_pct / 100.0)

    if side == "ask":
        # Strictly above the best bid: equal prices cross on this venue. An
        # empty bid side removes the crossing bound, not the ring.
        floor = book.best_bid if book.best_bid is not None else ring_lo
        low, high = max(floor, ring_lo), ring_hi
        strict_low = (book.best_bid is not None and book.best_bid >= ring_lo - _EPS * tick_size)
        strict_high = False
    else:
        # Mirror: strictly below the best ask, and never outside the ring.
        # Clamped rather than left infinite when the ask side is empty --
        # an unbounded high overflowed the tick count on the venue-wide
        # empty ask book observed 2026-09-02.
        ceil = book.best_ask if book.best_ask is not None else ring_hi
        low, high = ring_lo, min(ceil, ring_hi)
        strict_low = False
        strict_high = (book.best_ask is not None and book.best_ask <= ring_hi + _EPS * tick_size)

    if not (high > low):
        return Window(side=side, low=low, high=high, ticks=0, first=0.0, last=0.0)
    first, last = _grid_bounds(
        low, high, tick_size,
        strict_low=strict_low, strict_high=strict_high)
    ticks = (int(round((last - first) / tick_size)) + 1
             if last >= first - _EPS * tick_size else 0)
    if ticks > 0:
        return Window(side=side, low=low, high=high, ticks=ticks, first=first, last=last)

    # [SUBTICK 2026-09-03] The venue accepts 6-decimal prices (1e-6 precision).
    # When coarse tick_size (e.g. 0.0001) produces 0 ticks because competitor quotes
    # sit with sub-tick precision near the ring ceiling (e.g. bid at 0.104103 vs ceiling 0.104111),
    # fall back to micro-tick resolution (1e-6) so placeable resting asks inside the scoring ring
    # are not falsely reported as shut.
    if allow_subtick and tick_size > 1e-6:
        micro_tick = 1e-6
        s_low = (book.best_bid is not None and book.best_bid >= ring_lo - _EPS * micro_tick) if side == "ask" else False
        s_high = (book.best_ask is not None and book.best_ask <= ring_hi + _EPS * micro_tick) if side == "bid" else False
        low_m = low - (0.5 * micro_tick if side == "bid" else 0.0)
        high_m = high + (0.5 * micro_tick if side == "ask" else 0.0)
        first_m, last_m = _grid_bounds(
            low_m, high_m, micro_tick,
            strict_low=s_low, strict_high=s_high)
        ticks_m = (int(round((last_m - first_m) / micro_tick)) + 1
                   if last_m >= first_m - _EPS * micro_tick else 0)
        if ticks_m > 0:
            return Window(side=side, low=low, high=high, ticks=ticks_m, first=first_m, last=last_m)

    return Window(side=side, low=low, high=high, ticks=0, first=0.0, last=0.0)


def required_offset_pct(side: str, oracle: float, book: Book, *,
                        ring_pct: float, tick_size: float,
                        allow_subtick: bool = False) -> Optional[float]:
    """Offset from the oracle, in percent, that lands just clear of the book.

    Returns ``None`` when no such price exists -- the caller should then skip
    the leg rather than send one the venue will refuse. This is the direct
    replacement for ``CrossBackoff``'s 0.25%-per-refusal search: one
    observation gives the answer that the controller needs several rejected
    ticks to approach, and approaches only when the answer is inside its
    headroom at all.
    """
    return required_ladder_offset_pct(
        side, oracle, oracle, book,
        ring_pct=ring_pct, tick_size=tick_size,
        allow_subtick=allow_subtick)


def required_ladder_offset_pct(
    side: str,
    oracle: float,
    reference: float,
    book: Book,
    *,
    ring_pct: float,
    tick_size: float,
    allow_subtick: bool = False,
) -> Optional[float]:
    """Symmetric ladder offset that clears the book inside the oracle ring.

    ``earning_window`` is anchored to the true oracle because that is what
    the venue scores. The returned offset is anchored to ``reference`` because
    that is where ``quote_ladder`` applies it after inventory skew.
    """
    if not (math.isfinite(reference) and reference > 0.0):
        return None
    window = earning_window(side, oracle, book,
                            ring_pct=ring_pct, tick_size=tick_size,
                            allow_subtick=allow_subtick)
    if not window.open:
        return None
    if side == "ask" and book.best_bid is None:
        return 0.0
    if side == "bid" and book.best_ask is None:
        return 0.0
    target = window.first if side == "ask" else window.last
    if side == "ask":
        return max(0.0, (target / reference - 1.0) * 100.0)
    return max(0.0, (1.0 - target / reference) * 100.0)


def placement_prices(
    oracle: float,
    reference: float,
    book: Book,
    *,
    preferred_offset_pct: float,
    ring_pct: float,
    tick_size: float,
    allow_subtick: bool = True,
) -> Optional[tuple[float, float]]:
    """Exact ``(bid, ask)`` prices that rest and earn around a skewed center.

    Each side is clamped independently. A symmetric spread can be impossible
    around an inventory-skewed reference even though both earning windows are
    open; forcing one shared offset would either cross the book or leave the
    true-oracle ring.
    """
    if not (math.isfinite(reference) and reference > 0.0
            and math.isfinite(preferred_offset_pct)
            and preferred_offset_pct >= 0.0):
        return None
    bid_window = earning_window(
        "bid", oracle, book, ring_pct=ring_pct, tick_size=tick_size,
        allow_subtick=allow_subtick)
    ask_window = earning_window(
        "ask", oracle, book, ring_pct=ring_pct, tick_size=tick_size,
        allow_subtick=allow_subtick)
    if not (bid_window.open and ask_window.open):
        return None

    bid_low, bid_high = bid_window.first, bid_window.last
    ask_low, ask_high = ask_window.first, ask_window.last
    desired_bid = _quantise(
        reference * (1.0 - preferred_offset_pct / 100.0),
        tick_size, up=False)
    desired_ask = _quantise(
        reference * (1.0 + preferred_offset_pct / 100.0),
        tick_size, up=True)
    if desired_bid < bid_low or desired_bid > bid_high:
        desired_bid = _quantise(
            reference * (1.0 - preferred_offset_pct / 100.0),
            1e-6, up=False)
    if desired_ask < ask_low or desired_ask > ask_high:
        desired_ask = _quantise(
            reference * (1.0 + preferred_offset_pct / 100.0),
            1e-6, up=True)
    bid = min(max(desired_bid, bid_low), bid_high)
    ask = min(max(desired_ask, ask_low), ask_high)
    if not (bid > 0.0 and ask > bid):
        return None
    return bid, ask


def rests_and_earns(side: str, price: float, oracle: float, book: Book, *,
                    ring_pct: float, tick_size: float) -> bool:
    """Whether one final on-grid leg is post-only and inside the score ring."""
    if side not in ("bid", "ask"):
        raise ValueError("side must be 'bid' or 'ask', got %r" % (side,))
    if not (math.isfinite(price) and price > 0.0
            and math.isfinite(oracle) and oracle > 0.0
            and math.isfinite(ring_pct) and ring_pct > 0.0
            and math.isfinite(tick_size) and tick_size > 0.0):
        return False
    deviation = abs(price / oracle - 1.0) * 100.0
    if deviation > ring_pct + 1e-3:
        return False
    epsilon = min(tick_size, 1e-6) * _EPS
    if side == "ask":
        return (book.best_bid is None
                or price > book.best_bid + epsilon)
    return (book.best_ask is None
            or price < book.best_ask - epsilon)
