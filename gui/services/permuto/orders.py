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

import logging
import math
from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Optional

_log = logging.getLogger(__name__)

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


def _finite(value: float) -> bool:
    """A real number, not NaN and not an infinity.

    ``x > 0.0`` is not a sufficient guard: ``inf`` passes it, and every price
    or size derived from an infinite input is then either infinite or zero
    while still looking like an ordinary float.
    """
    try:
        return math.isfinite(value)
    except TypeError:          # not a number at all
        return False


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

    Raises ``ValueError`` when a leg's own ``side`` disagrees with the book it
    was handed in through. That is a caller bug, and the quiet version of it
    is the worst outcome available here: two sell-side legs split across the
    two arguments would score as a balanced book and report depth that cannot
    exist. Refusing is the only answer that cannot overstate the number.
    """
    def in_ring(legs: Iterable[OrderIntent], side: Side) -> float:
        total = 0.0
        for leg in legs:
            # Side() normalises, so a plain "buy" string is accepted as
            # Side.BUY rather than silently scored as a sell -- the intent
            # dataclass is not frozen against a caller passing the raw value.
            leg_side = Side(leg.side)
            if leg_side is not side:
                raise ValueError(
                    "a %s intent was passed in the %s book for %s. Depth "
                    "credit is measured per side, so scoring it under the "
                    "wrong rule would report balanced depth for a one-sided "
                    "book" % (leg_side.value, side.value, leg.market)
                )
            if leg.reduce_only:
                continue
            placement = classify_placement(
                leg_side, leg.price, oracle, ring_pct=ring_pct,
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
    tick_size: float = 0.0001,
    lot_size: float = 1.0,
    max_offset_pct: Optional[float] = None,
) -> list[OrderIntent]:
    """A balanced ladder sized to earn ``target_depth_usd`` of depth credit.

    Deliberately simple and deliberately symmetric. Depth is ``min(bid, ask)``,
    so any asymmetry is wasted notional on the heavier side — capital at risk
    earning nothing. Symmetry is not a stylistic choice here, it is what the
    scoring function rewards.

    By default every level reserves 10% of the ring for drift, because a level
    outside it earns no depth AND risks being purged on an oracle move. A
    caller with a directly observed resting window may supply
    ``max_offset_pct`` to use more of the ring; the cap is still bounded by
    ``ring_pct``.

    EVERY tuning input is checked, and an out-of-domain one quotes NOTHING
    rather than raising. ``first_offset_pct`` and ``ring_pct`` arrive from
    configuration (``QuotingRunner``'s constructor), and the failure modes are
    not merely ugly: a negative offset inverts the ladder into a crossed pair
    that batch.py happily accepts because both legs are inside the band, NaN
    survives the ring clamp (``min(nan, x)`` is nan) and poisons both prices,
    and a ring above 100% lifts the clamp past the oracle and makes the bid
    price negative. Quoting nothing is the safe direction — it costs depth
    credit, and it is logged at WARNING so a config typo does not silently
    idle the book for a whole contest week.
    """
    # [audit] Log EVERY rejection, not just the offsets. The docstring
    # promised that and only the offset branch delivered it, so a sign typo
    # in target_depth_usd -- a QuoteRunner constructor argument -- idled the
    # book in silence for as long as nobody looked.
    if not (_finite(oracle) and oracle > 0.0):
        _log.warning("quote_ladder: refusing to build against oracle %r", oracle)
        return []
    if not (_finite(target_depth_usd) and target_depth_usd > 0.0):
        _log.warning("quote_ladder: target_depth_usd is %r; quoting nothing",
                     target_depth_usd)
        return []
    if levels < 1:
        return []
    if not (_finite(first_offset_pct) and first_offset_pct > 0.0
            and _finite(level_step_pct) and level_step_pct >= 0.0
            and _finite(ring_pct) and 0.0 < ring_pct <= 100.0):
        _log.warning(
            "permuto: refusing to quote %s -- out-of-domain ladder tuning "
            "(first_offset_pct=%r, level_step_pct=%r, ring_pct=%r)",
            market, first_offset_pct, level_step_pct, ring_pct,
        )
        return []
    if max_offset_pct is None:
        offset_cap = ring_pct * 0.9
    elif (_finite(max_offset_pct)
          and 0.0 < max_offset_pct <= ring_pct):
        offset_cap = max_offset_pct
    else:
        _log.warning(
            "permuto: refusing to quote %s -- max_offset_pct=%r is outside "
            "(0, ring_pct]", market, max_offset_pct)
        return []

    # [release review] Quantise to the venue's published grid. The live
    # /info/meta declares tick_size 0.0001 and lot_size 1 per market, and
    # nothing here honoured either -- prices went out with 16 decimals and
    # fractional sizes. If the venue enforces (a validator that rejects a
    # whole batch outright is a strict one), the first batch on Monday would
    # be a 400 and so would every retry for 102 hours. Directions are chosen
    # to stay maker-safe and inside the ring: bid rounds DOWN, ask rounds UP
    # (both AWAY from the oracle -- never sharper than approved), size floors
    # to the lot so we never promise notional we did not price.
    if not (_finite(tick_size) and tick_size > 0.0):
        tick_size = 0.0001
    if not (_finite(lot_size) and lot_size > 0.0):
        lot_size = 1.0

    # Away-from-oracle rounding can add almost one full tick. The ordinary
    # 90% cap already has ample room; an observed BBO override may use nearly
    # the whole ring, so reserve that tick explicitly before building prices.
    grid_safe_cap = ring_pct - tick_size / oracle * 100.0
    offset_cap = min(offset_cap, grid_safe_cap)
    if offset_cap <= 0.0:
        _log.warning(
            "quote_ladder: %s has no in-ring grid point (oracle=%r, tick=%r)",
            market, oracle, tick_size)
        return []

    per_level = target_depth_usd / levels
    out: list[OrderIntent] = []
    for i in range(levels):
        offset = first_offset_pct + i * level_step_pct
        offset = min(offset, offset_cap)
        bid_price = oracle * (1.0 - offset / 100.0)
        ask_price = oracle * (1.0 + offset / 100.0)
        # floor/ceil on the tick grid, then guard the degenerate results: a
        # bid floored to zero, or a pair the rounding has crossed.
        bid_price = math.floor(bid_price / tick_size) * tick_size
        ask_price = math.ceil(ask_price / tick_size) * tick_size
        if not (bid_price > 0.0 and ask_price > bid_price):
            _log.warning(
                "quote_ladder: %s level %d quantised to a degenerate pair "
                "(bid=%r ask=%r, tick=%r); skipping the level",
                market, i, bid_price, ask_price, tick_size)
            continue
        bid_size = math.floor((per_level / bid_price) / lot_size) * lot_size
        ask_size = math.floor((per_level / ask_price) / lot_size) * lot_size
        if bid_size <= 0.0 or ask_size <= 0.0:
            _log.warning(
                "quote_ladder: %s level %d sizes to zero lots "
                "(per_level=$%.2f); skipping the level", market, i, per_level)
            continue
        out.append(OrderIntent(market, Side.BUY, bid_price, bid_size))
        out.append(OrderIntent(market, Side.SELL, ask_price, ask_size))
    return out
