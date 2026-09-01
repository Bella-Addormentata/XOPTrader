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
at maintenance; we stop adding risk at `MAX_MARGIN_UTILISATION` and stop
QUOTING at `FLATTEN_MARGIN_UTILISATION`, both of which sit well above
maintenance in equity terms. Past the second line the position itself is the
operator's to close -- see RiskAction.FLATTEN for why that is not automated.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

from .quoting import REQUOTE_AT_RING_FRACTION as _REQUOTE_AT_RING_FRACTION

__all__ = [
    "FLATTEN_MARGIN_UTILISATION",
    "MAX_MARGIN_UTILISATION",
    "PORTFOLIO_MAX_EXPOSURE_FRACTION",
    "MarginState",
    "RiskAction",
    "RiskDecision",
    "assess",
    "portfolio_cap_usd",
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


#: Fraction of equity the WHOLE BOOK may carry, across every market.
#:
#: [audit] max_position_usd is PER MARKET and nothing aggregated it, so
#: three markets at the shipped 250,000 authorised 750,000 of exposure on
#: a 500,000 account -- 1.5x the equity, before the venue's 8x carried
#: multiplier touches it. That is the shape of the liquidation this bot
#: has already suffered once: no single market breached its own limit.
#:
#: Denominated in EQUITY rather than as a multiple of the per-market cap,
#: because equity is what the venue liquidates against and it is the one
#: number that shrinks when things go wrong.
PORTFOLIO_MAX_EXPOSURE_FRACTION = 0.60


def portfolio_cap_usd(
    equity_usd: float,
    market: str,
    per_market_cap_usd: float,
    positions_usd: dict,
) -> float:
    """The per-market cap, reduced so the WHOLE book stays inside budget.

    Returns the smaller of ``per_market_cap_usd`` and whatever portfolio
    room is left once every OTHER market's exposure is subtracted. Never
    negative, and never larger than what was passed in -- this can only
    tighten, exactly like the curfew.

    Fails CLOSED on anything unreadable: an equity or a position we
    cannot parse yields zero room, because the alternative is authorising
    exposure against a number nobody can see.
    """
    if not math.isfinite(per_market_cap_usd) or per_market_cap_usd <= 0.0:
        return 0.0
    if not math.isfinite(equity_usd) or equity_usd <= 0.0:
        return 0.0
    budget = equity_usd * PORTFOLIO_MAX_EXPOSURE_FRACTION
    others = 0.0
    for name, notional in (positions_usd or {}).items():
        if name == market:
            continue
        try:
            value = abs(float(notional))
        except (TypeError, ValueError):
            return 0.0
        if not math.isfinite(value):
            return 0.0
        others += value
    return max(0.0, min(per_market_cap_usd, budget - others))


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
    """Stop quoting and retract every resting order for this market.

    [review] What this asks for and what the runner does are now the same
    thing, and the earlier wording ("cross the spread to get out") promised
    more than either. Nothing here submits a closing taker order: the
    POSITION remains open, and at the 75% line it can keep moving toward
    liquidation while this action is in force. Closing it is a spend with
    slippage against a book this module cannot see, and it stays an operator
    decision -- the runner surfaces the state loudly instead of trading on
    its own. If an automated reduce-only close is ever added, it belongs in
    the runner with its own review, not behind this label.
    """


@dataclass(frozen=True)
class MarginState:
    """Account state as the venue reports it, plus what we asked for.

    Positions are SIGNED in contracts: positive long, negative short.
    """

    equity_usd: float = 0.0
    used_margin_usd: float = 0.0
    positions: dict = field(default_factory=dict)

    positions_readable: bool = True
    """Whether the positions field was PRESENT and well-typed.

    [review round 11] An empty dict is a genuinely flat account; a missing
    or wrong-typed positions payload is an account we cannot read -- and
    the two used to collapse into the same {}, so a payload with valid
    equity but no readable positions let every market quote risk-increasing
    size against unknown inventory. assess() forces FLATTEN when this is
    False.
    """
    carried: bool = False

    def utilisation(self) -> float:
        """Fraction of equity already pledged. 1.0 when there is no equity.

        Not 0.0: an account with no equity is not an account with lots of
        room, and returning 0.0 would read as healthy to every threshold
        below.
        """
        if not (self.equity_usd > 0.0) or not math.isfinite(self.equity_usd):
            return 1.0
        if not math.isfinite(self.used_margin_usd) or self.used_margin_usd < 0.0:
            # [review] NaN makes BOTH threshold comparisons below false, so
            # assess() returns NORMAL and keeps adding risk against an
            # account whose margin we cannot read. A NEGATIVE margin is
            # worse, because it is finite and passes an isfinite check: the
            # division yields negative utilisation, which is BELOW every
            # threshold and reads as unlimited headroom. Every unreadable
            # number here has to mean "no room", never "lots of room".
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


def max_price_skew_frac(ring_pct: float, half_spread_pct: float,
                        tick_frac: float = 0.0) -> float:
    """How far the pair can slide before the trailing leg leaves the ring.

    Never negative: a spread wider than the ring has no legal two-sided
    placement at all, and reporting a negative allowance would invert the
    skew and push the leading leg out instead.

    [release review] ALSO capped below the re-quote trigger. The ring-edge
    ceiling ((ring - half_spread)/100 = 1.75% at defaults) sits ABOVE the
    drift trigger (ring * REQUOTE_AT_RING_FRACTION = 1.2%), so a heavily
    skewed quote was born already past the trigger -- decide() re-quoted it
    every tick, an endless cancel/replace churn that consumed the mutate
    budget and never rested long enough to earn depth. The skew ceiling now
    stops at 80% of the trigger, so even a fully skewed pair rests until the
    ORACLE moves, which is what the trigger is for.

    [review 2026-09-01] THE PLACEMENT AND THE ROUNDING TICK COME OUT OF
    THIS BUDGET TOO. It reserved 80% of the trigger for the SKEW alone,
    but decide() re-quotes on abs(leg_price - oracle) -- the LEG, which
    sits a half-spread beyond the skewed midpoint and is then CEILED onto
    the venue grid. Measured at the defaults with oracle 0.07 and a 95%
    short: skew 0.9120%, raw ask 0.070815, rounded to 0.0709, which is
    1.2857% from the oracle against a 1.20% trigger. Born past it and
    replaced every tick, with the crossing backoff already clamped to
    zero and unable to help -- the churn this cap was added to prevent,
    surviving underneath it.

    Subtracting both makes the total self-balancing whatever the spread
    and tick are.
    """
    ring_edge = (ring_pct - half_spread_pct) / 100.0
    trigger = ring_pct * _REQUOTE_AT_RING_FRACTION / 100.0
    budget = (trigger * 0.8 - half_spread_pct / 100.0 - abs(tick_frac))
    return max(0.0, min(ring_edge, budget))


def skew_frac(
    position: float,
    max_position: float,
    *,
    ring_pct: float = 2.0,
    half_spread_pct: float = 0.25,
    tick_frac: float = 0.0,
) -> float:
    """Price offset for the current inventory, as a signed fraction.

    Linear in position up to the limit, then clamped. Linear because the
    alternative -- doing nothing until some threshold, then skewing hard -- is
    a step function that arrives after the inventory is already a problem, and
    each step is a re-quote that empties the book between states.
    """
    ceiling = max_price_skew_frac(ring_pct, half_spread_pct,
                                  tick_frac)
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
    tick_frac: float = 0.0,
) -> RiskDecision:
    """One decision for one market. Total and side-effect free.

    Ordered so that the checks which make quoting UNSAFE come before the ones
    that merely resize it -- the same ordering discipline as quoting.decide().
    """
    if not state.positions_readable:
        return RiskDecision(
            RiskAction.FLATTEN, 0.0, 0.0,
            "the account's positions could not be read -- unknown inventory "
            "is the strongest reason to stop, not a flat book",
        )
    position = float(state.positions.get(market, 0.0) or 0.0)
    if not math.isfinite(position):
        # [review] A NaN position fails the limit comparison below and is
        # then CLAMPED by skew_frac into a finite extreme skew, so assess()
        # returned NORMAL and added risk on inventory it could not read.
        # Unreadable inventory is the strongest reason to stop, not a
        # reason to continue.
        return RiskDecision(
            RiskAction.FLATTEN, 0.0, 0.0,
            "position for %s is not a readable number (%r)"
            % (market, state.positions.get(market)),
        )
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
        tick_frac=tick_frac,
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
