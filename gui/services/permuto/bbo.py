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
import urllib.request
from dataclasses import dataclass
from typing import Optional

_log = logging.getLogger(__name__)

#: Levels requested per side. We only need the top of book, but asking for a
#: few costs nothing and lets a caller see whether the top level is a lone
#: dust order or a genuine wall.
_LEVELS = 5

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


def _get(url: str, timeout: float) -> dict:
    req = urllib.request.Request(url, headers=_HEADERS, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def _first_price(levels) -> Optional[float]:
    """Top-of-book price, or None. Tolerates the venue's string decimals."""
    try:
        return float(levels[0]["price"])
    except (IndexError, KeyError, TypeError, ValueError):
        return None


def fetch_book(market: str, *, base_url: str,
               timeout: float = 10.0) -> Optional[Book]:
    """Top of book for ``market``, or None if the venue did not answer.

    Never raises: a book we could not read must degrade to the existing
    blind behaviour, not take the quoting loop down with it.
    """
    url = "%s/info/l2/%s?levels=%d" % (base_url.rstrip("/"), market, _LEVELS)
    try:
        payload = _get(url, timeout) or {}
    except Exception as exc:  # noqa: BLE001 - a missing book is not fatal
        _log.debug("permuto: L2 fetch failed for %s: %s", market, exc)
        return None
    return Book(
        market=market,
        best_bid=_first_price(payload.get("bids") or []),
        best_ask=_first_price(payload.get("asks") or []),
    )


def earning_window(side: str, oracle: float, book: Book, *,
                   ring_pct: float, tick_size: float) -> Window:
    """The prices on ``side`` that would both rest and earn credit.

    An ask must sit ABOVE the best bid or the venue refuses it post-only,
    and at or inside ``oracle * (1 + ring_pct/100)`` or it earns nothing.
    Bids mirror it. An empty opposing side removes only the crossing bound;
    the ring still applies, which is why a venue-wide empty ask side does
    not mean asks are free to place anywhere.
    """
    if side not in ("bid", "ask"):
        raise ValueError("side must be 'bid' or 'ask', got %r" % (side,))
    if not (oracle > 0.0 and ring_pct > 0.0 and tick_size > 0.0):
        return Window(side=side, low=0.0, high=0.0, ticks=0)

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
    else:
        # Mirror: strictly below the best ask, and never outside the ring.
        # Clamped rather than left infinite when the ask side is empty --
        # an unbounded high overflowed the tick count on the venue-wide
        # empty ask book observed 2026-09-02.
        ceil = book.best_ask if book.best_ask is not None else ring_hi
        low, high = ring_lo, min(ceil, ring_hi)

    if not (high > low):
        return Window(side=side, low=low, high=high, ticks=0)
    # Count tick-aligned prices strictly inside the open bound. Computed on
    # the width rather than by enumerating, so a wide window is not a loop.
    ticks = int((high - low) / tick_size)
    return Window(side=side, low=low, high=high, ticks=max(ticks, 0))


def required_offset_pct(side: str, oracle: float, book: Book, *,
                        ring_pct: float, tick_size: float) -> Optional[float]:
    """Offset from the oracle, in percent, that lands just clear of the book.

    Returns ``None`` when no such price exists -- the caller should then skip
    the leg rather than send one the venue will refuse. This is the direct
    replacement for ``CrossBackoff``'s 0.25%-per-refusal search: one
    observation gives the answer that the controller needs several rejected
    ticks to approach, and approaches only when the answer is inside its
    headroom at all.
    """
    window = earning_window(side, oracle, book,
                            ring_pct=ring_pct, tick_size=tick_size)
    if not window.open:
        return None
    if side == "ask":
        # One tick clear of the bid, then quantised UP the way the ladder
        # rounds asks, so the price we compute is the price that is sent.
        target = _quantise(window.low + tick_size, tick_size, up=True)
        if target > window.high + _EPS * tick_size:
            return None
    else:
        target = _quantise(window.high - tick_size, tick_size, up=False)
        if target < window.low - _EPS * tick_size:
            return None
    return abs(target / oracle - 1.0) * 100.0
