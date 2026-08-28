"""What may be quoted, at what price, and what it earns.

Pure. No network, no session, no account — every function here is a function
of its arguments, so the part of the order path where a mistake costs money is
the part that can be tested exhaustively before anything is placed.

THREE BANDS, AND THEY DO DIFFERENT JOBS. Conflating them is the fastest way to
a book that looks fine locally and is empty on the venue:

* ``vol_oracle_band_pct`` (default 5) — the LEGAL PLACEMENT band. Outside it
  the venue answers HTTP 400 and there is no order.
* ``vol_aggressive_ring_pct`` (default 2) — the inner AGGRESSIVE ring. Outside
  the ring only *passive* rests are allowed (a bid at or below the oracle, an
  ask at or above it). An aggressive rest outside the ring is rejected at
  place, and — worse — **purged when the oracle moves**, so an order that was
  accepted can vanish without us doing anything.
* The same ±2% is what DEPTH CREDIT is measured in, on ``min(bid, ask)``.

The consequence that drives the design: **a lifted side earns zero for that
market until it is restored.** Depth is the minimum of the two sides, so a
filled bid stops all accrual on that market even though the ask is untouched.
Restoration is therefore urgent in a way that "we are a bit one-sided" never
sounds like.

Everything here is expressed against a *fresh* oracle supplied by the caller.
A stale local oracle produces orders that look correct here, are rejected or
purged by the venue, and score nothing — and nothing about that failure is
visible from our side, which is why the caller must pass the venue's own
number rather than a cached one.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Optional

__all__ = [
    "OrderIntent",
    "Placement",
    "Side",
    "classify_placement",
    "depth_credit_usd",
    "notional_usd",
    "quote_ladder",
]


class Side(str, Enum):
    BUY = "buy"
    SELL = "sell"


class Placement(str, Enum):
    """What the venue will do with a resting order at this price."""

    AGGRESSIVE = "aggressive"
    """Inside the ring and crossing the oracle. Allowed, and earns depth."""

    PASSIVE = "passive"
    """On its own side of the oracle. Always allowed inside the band."""

    PURGE_RISK = "purge_risk"
    """Aggressive but OUTSIDE the ring: rejected at place, and purged on an
    oracle move if it ever rested. Never emit one deliberately."""

    ILLEGAL = "illegal"
    """Outside the legal band entirely. HTTP 400; there is no order."""


@dataclass(frozen=True)
class OrderIntent:
    """One leg we intend to rest."""

    market: str
    """The venue SYMBOL (``QQQ-VOL-PERP``), never the oracle ticker
    (``QQQ-VOL``). /info/meta carries both and they differ by suffix; order
    routes want the former."""

    side: Side
    price: float
    size: float
    reduce_only: bool = False

    @property
    def notional(self) -> float:
        return notional_usd(self.price, self.size)


def notional_usd(price: float, size: float) -> float:
    """Notional of a leg. Guards non-finite so a bad quote cannot pass a cap."""
    if not (price > 0.0) or not (size > 0.0):
        return 0.0
    value = price * size
    return value if value == value and value not in (float("inf"),) else 0.0


#: Relative tolerance on the band and ring comparisons.
#:
#: The venue expresses both as "within N%", but binary floats do not honour
#: that at the boundary: 0.10 * 1.02 computes a deviation fractionally ABOVE
#: 2%, so a quote placed exactly on the ring classified as purge-risk. Exactly
#: the trap already fixed in the consolidate planner, arriving in a second
#: module -- the direction is deliberate, because rejecting an order sitting
#: on a limit is the surprising half and the venue is the one that decides.
_EDGE_EPSILON = 1e-9


def _within(deviation: float, limit: float) -> bool:
    return deviation <= limit + _EDGE_EPSILON * (1.0 + abs(limit))


def classify_placement(
    side: Side,
    price: float,
    oracle: float,
    *,
    band_pct: float = 5.0,
    ring_pct: float = 2.0,
) -> Placement:
    """Where this price sits relative to the oracle, in the venue's terms.

    Boundaries are INCLUSIVE on the permissive side: the venue's own
    band/ring are expressed as "within N%", and rejecting an order sitting
    exactly on a limit the operator chose is the surprising direction.
    """
    if not (oracle > 0.0) or oracle != oracle:
        return Placement.ILLEGAL          # no fresh oracle, no opinion
    if not (price > 0.0) or price != price:
        return Placement.ILLEGAL

    deviation = abs(price - oracle) / oracle * 100.0
    if not _within(deviation, band_pct):
        return Placement.ILLEGAL

    # Passive means resting on your own side of the oracle: a bid at or below
    # it, an ask at or above it. That is always legal inside the band.
    passive = (price <= oracle) if side is Side.BUY else (price >= oracle)
    if passive:
        return Placement.PASSIVE

    # Aggressive. Fine inside the ring; outside it the venue rejects at place
    # and purges on an oracle move.
    return (Placement.AGGRESSIVE if _within(deviation, ring_pct)
            else Placement.PURGE_RISK)


def depth_credit_usd(
    bids: Iterable[OrderIntent],
    asks: Iterable[OrderIntent],
    oracle: float,
    *,
    ring_pct: float = 2.0,
) -> float:
    """Balanced notional this book earns per depth sample: ``min(bid, ask)``.

    Only legs INSIDE the ring count, and only two-sided books earn anything.
    Returning the minimum rather than the sum is the whole point: it is why a
    single fill on one side stops accrual for the entire market, and why the
    restore path matters more than the placement path.

    Reduce-only legs are excluded — they exist to shed inventory, not to
    provide liquidity, and counting them would flatter a book that is
    actually retreating.
    """
    def in_ring(legs: Iterable[OrderIntent], side: Side) -> float:
        total = 0.0
        for leg in legs:
            if leg.reduce_only:
                continue
            placement = classify_placement(
                side, leg.price, oracle, ring_pct=ring_pct,
                band_pct=max(ring_pct, 5.0),
            )
            if placement in (Placement.PASSIVE, Placement.AGGRESSIVE):
                if _within(abs(leg.price - oracle) / oracle * 100.0, ring_pct):
                    total += leg.notional
        return total

    if not (oracle > 0.0):
        return 0.0
    return min(in_ring(bids, Side.BUY), in_ring(asks, Side.SELL))


def quote_ladder(
    market: str,
    oracle: float,
    target_depth_usd: float,
    *,
    levels: int = 3,
    first_offset_pct: float = 0.25,
    level_step_pct: float = 0.5,
    ring_pct: float = 2.0,
) -> list[OrderIntent]:
    """A balanced ladder sized to earn ``target_depth_usd`` of depth credit.

    Deliberately simple and deliberately symmetric. Depth is ``min(bid, ask)``,
    so any asymmetry is wasted notional on the heavier side — capital at risk
    earning nothing. Symmetry is not a stylistic choice here, it is what the
    scoring function rewards.

    Every level is placed strictly INSIDE the ring, because a level outside it
    earns no depth AND risks being purged on an oracle move. The last level is
    clamped rather than allowed to drift out, so asking for more levels than
    the ring can hold degrades into a tighter ladder instead of a book with
    invisible legs.
    """
    if not (oracle > 0.0) or not (target_depth_usd > 0.0) or levels < 1:
        return []

    per_level = target_depth_usd / levels
    out: list[OrderIntent] = []
    for i in range(levels):
        offset = first_offset_pct + i * level_step_pct
        # Strictly inside: a leg exactly ON the ring boundary earns credit but
        # leaves no room for the oracle to move before it falls out.
        offset = min(offset, ring_pct * 0.9)
        bid_price = oracle * (1.0 - offset / 100.0)
        ask_price = oracle * (1.0 + offset / 100.0)
        out.append(OrderIntent(market, Side.BUY, bid_price, per_level / bid_price))
        out.append(OrderIntent(market, Side.SELL, ask_price, per_level / ask_price))
    return out
