"""Inventory and margin. The layer that decides how much, and how skewed.

WHY SKEW IS PRICED AND NOT SIZED. The instinct when you are long is to quote a
smaller bid: show less of the side you do not want. On a normal venue that is
correct. Here it is close to the worst available move, because depth credit is
``min(bid_usd, ask_usd)``. Shrinking one side does not tilt the book, it
*truncates the minimum* -- so cutting the bid in half halves the depth credit
for that market, and cutting it to zero stops accrual entirely while leaving
the ask exposed. You would be paying for inventory control in the one currency
the eligibility gate is denominated in.

So inventory is steered by moving BOTH quotes in price, keeping the two sizes
equal. A long book shifts the pair down: the bid gets less attractive, the ask
gets more attractive, fill probability tilts toward selling, and
``min(bid, ask)`` is untouched because both legs are still there at the same
size. The cost of the skew is paid in expected edge, which is the right place
to pay it, rather than in eligibility.

The skew has a hard ceiling that is not a preference. Both legs must stay
strictly inside the +/-2% ring or they earn no depth and get purged on the
next oracle move, so the pair can only slide by ``ring - half_spread`` before
the trailing leg falls out. Past that point price skew is exhausted and the
only remaining controls are reduce-only and flatten -- which is exactly why
those exist below, and why the position limit has to bind well before the
skew ceiling does.

MARGIN. Liquidation is the one outcome that cannot be traded out of, and net
PnL is the sole ranking criterion once the depth gate is passed, so the margin
ceiling here is deliberately far tighter than the venue's. The venue liquidates
at maintenance; we stop adding risk at `MAX_MARGIN_UTILISATION` and shed at
`FLATTEN_MARGIN_UTILISATION`, both of which sit well above maintenance in
equity terms. Being flat and eligible beats being liquidated and ranked.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

__all__ = [
    "FLATTEN_MARGIN_UTILISATION",
    "MAX_MARGIN_UTILISATION",
    "MarginState",
    "RiskAction",
    "RiskDecision",
    "assess",
    "max_price_skew_frac",
    "skew_frac",
    "skewed_reference",
]

#: Stop ADDING risk here. Below the venue's maintenance margin by a wide
#: margin, because the gap has to absorb an adverse oracle move on a position
#: we cannot hedge -- and this oracle has been measured moving 10-13% in
#: seconds.
MAX_MARGIN_UTILISATION = 0.50

#: Start SHEDDING risk here, reduce-only.
FLATTEN_MARGIN_UTILISATION = 0.75

#: Carried (cash market closed) multiplier on initial margin for
#: risk-increasing orders. The venue's own number: the same target depth costs
#: eight times the collateral out of hours.
CARRIED_IM_MULTIPLIER = 8.0


class RiskAction(str, Enum):
    NORMAL = "normal"
    """Quote both sides, sized normally, skewed by inventory."""

    REDUCE_ONLY = "reduce_only"
    """Quote only the side that shrinks the position.

    Accepts the loss of depth credit for this market -- deliberately. Depth is
    an eligibility gate that resets and re-accrues; a position that keeps
    growing into a margin call does not.
    """

    FLATTEN = "flatten"
    """Cross the spread to get out. Pay the spread; it is cheaper than the
    alternative."""


@dataclass(frozen=True)
class MarginState:
    """Account state as the venue reports it, plus what we asked for.

    Positions are SIGNED in contracts: positive long, negative short.
    """

    equity_usd: float = 0.0
    used_margin_usd: float = 0.0
    positions: dict = field(default_factory=dict)
    carried: bool = False

    def utilisation(self) -> float:
        """Fraction of equity already pledged. 1.0 when there is no equity.

        Not 0.0: an account with no equity is not an account with lots of
        room, and returning 0.0 would read as healthy to every threshold
        below.
        """
        if not (self.equity_usd > 0.0) or not math.isfinite(self.equity_usd):
            return 1.0
        if not math.isfinite(self.used_margin_usd):
            # [review] NaN would make BOTH threshold comparisons below false,
            # so assess() would return NORMAL and keep adding risk against an
            # account whose margin we cannot read. Every unreadable number
            # here has to mean "no room", never "lots of room".
            return 1.0
        return self.used_margin_usd / self.equity_usd


@dataclass(frozen=True)
class RiskDecision:
    action: RiskAction
    size: float
    """Contracts per side. Equal on both sides by construction."""

    skew: float
    """Signed fraction of price to shift BOTH quotes. Negative leans short."""

    reason: str = ""


def max_price_skew_frac(ring_pct: float, half_spread_pct: float) -> float:
    """How far the pair can slide before the trailing leg leaves the ring.

    Never negative: a spread wider than the ring has no legal two-sided
    placement at all, and reporting a negative allowance would invert the
    skew and push the leading leg out instead.
    """
    return max(0.0, (ring_pct - half_spread_pct) / 100.0)


def skew_frac(
    position: float,
    max_position: float,
    *,
    ring_pct: float = 2.0,
    half_spread_pct: float = 0.25,
) -> float:
    """Price offset for the current inventory, as a signed fraction.

    Linear in position up to the limit, then clamped. Linear because the
    alternative -- doing nothing until some threshold, then skewing hard -- is
    a step function that arrives after the inventory is already a problem, and
    each step is a re-quote that empties the book between states.
    """
    ceiling = max_price_skew_frac(ring_pct, half_spread_pct)
    if not (max_position > 0.0) or ceiling <= 0.0:
        return 0.0
    fraction = max(-1.0, min(1.0, position / max_position))
    # Negative: long inventory shifts the pair DOWN, which makes our ask the
    # attractive side.
    return -fraction * ceiling


def assess(
    state: MarginState,
    market: str,
    *,
    base_size: float,
    max_position: float,
    ring_pct: float = 2.0,
    half_spread_pct: float = 0.25,
) -> RiskDecision:
    """One decision for one market. Total and side-effect free.

    Ordered so that the checks which make quoting UNSAFE come before the ones
    that merely resize it -- the same ordering discipline as quoting.decide().
    """
    position = float(state.positions.get(market, 0.0) or 0.0)
    utilisation = state.utilisation()

    if utilisation >= FLATTEN_MARGIN_UTILISATION:
        return RiskDecision(
            RiskAction.FLATTEN, 0.0, 0.0,
            "margin utilisation %.0f%% at or past the flatten line %.0f%%"
            % (utilisation * 100.0, FLATTEN_MARGIN_UTILISATION * 100.0),
        )

    if not (state.equity_usd > 0.0):
        return RiskDecision(
            RiskAction.FLATTEN, 0.0, 0.0,
            "no equity reported; refusing to size a quote against nothing",
        )

    skew = skew_frac(
        position, max_position,
        ring_pct=ring_pct, half_spread_pct=half_spread_pct,
    )

    if max_position > 0.0 and abs(position) >= max_position:
        return RiskDecision(
            RiskAction.REDUCE_ONLY, base_size, skew,
            "position %.2f is at the %.2f limit for %s"
            % (position, max_position, market),
        )

    if utilisation >= MAX_MARGIN_UTILISATION:
        return RiskDecision(
            RiskAction.REDUCE_ONLY, base_size, skew,
            "margin utilisation %.0f%% past the add-risk line %.0f%%"
            % (utilisation * 100.0, MAX_MARGIN_UTILISATION * 100.0),
        )

    size = base_size
    if state.carried:
        # Same collateral, eight times the initial margin per contract. Quote
        # the size we can actually afford rather than the size we want and
        # let the venue reject the batch.
        size = base_size / CARRIED_IM_MULTIPLIER

    if not (size > 0.0):
        return RiskDecision(
            RiskAction.REDUCE_ONLY, 0.0, skew,
            "affordable size rounded to zero",
        )

    return RiskDecision(RiskAction.NORMAL, size, skew, "")


def skewed_reference(oracle: float, skew: float) -> Optional[float]:
    """Apply a skew to the oracle, or None if the result is not a price.

    Returns None rather than clamping. A non-positive or non-finite reference
    means the inputs were wrong, and silently substituting a plausible number
    would place real orders at a price nobody chose.
    """
    if not (oracle > 0.0) or oracle != oracle:
        return None
    reference = oracle * (1.0 + skew)
    if not (reference > 0.0) or reference != reference:
        return None
    return reference
