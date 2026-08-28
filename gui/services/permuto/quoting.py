"""When to quote, when to hold, and when to get out of the way.

The loop's decision, pure. Given the venue's state and our own resting book it
returns exactly one action, so the transport has no judgement left to exercise
and every branch is testable without a socket.

WHAT THE LIVE BOOK LOOKS LIKE, because it shapes the defaults. Recorded over
571 ticks on 2026-08-28: median ask notional inside the ±2% ring is **zero** on
all three markets, against median bids of $10k-19k. Depth credit is
``min(bid, ask)`` per account, so that does not hand anyone free credit -- what
it means is that an ask placed inside the ring is frequently the ONLY ask
there, and gets hit first whenever anybody buys. Adverse selection is
concentrated on precisely the side you must quote to earn anything, which is a
plausible mechanism for the leaderboard pattern where every market maker with
real depth is deeply negative.

So the loop is built to withdraw readily and re-quote promptly, rather than to
sit. Three of the four actions below take the book DOWN.

THE ASYMMETRY THAT DRIVES EVERYTHING ELSE: credit is the minimum of the two
sides, so a lifted bid earns zero for that market even though the ask is
untouched. Restoring a side is not tidying up, it is the difference between
earning and not earning, and the venue's own guidance is that the restore be
one call rather than a cancel/place pair.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

__all__ = [
    "LoopAction",
    "QuoteDecision",
    "VenueView",
    "decide",
]


class LoopAction(str, Enum):
    QUOTE = "quote"
    """Place or refresh the two-sided quote, in one batch_upsert."""

    HOLD = "hold"
    """The resting quote is still good. Doing nothing is free; re-quoting
    costs a mutate token and briefly empties the book between states."""

    WITHDRAW = "withdraw"
    """Cancel everything. Something makes quoting unsafe or pointless."""

    WAIT = "wait"
    """Blocked on something with its own timer -- a session backoff. Not
    WITHDRAW, because the book may still be legitimately resting."""


@dataclass(frozen=True)
class VenueView:
    """Everything the decision needs about the world, gathered by the caller."""

    trading_paused: bool = False
    oracle: Optional[float] = None
    oracle_age_s: float = 0.0
    session_ok: bool = True
    session_waiting: bool = False
    carried: bool = False
    """Cash market closed. Risk-increasing places need 8x stressed initial
    margin, so the same target depth costs far more collateral."""

    just_reopened: bool = False
    """A carried->live transition since the last tick. The sequencer cancels
    ALL resting orders and pending triggers at the open, so whatever we
    believe is resting is gone -- believing otherwise means quoting nothing
    through the busiest hour of the day."""


@dataclass(frozen=True)
class RestingQuote:
    """What we believe is currently resting, per market."""

    bid_price: Optional[float] = None
    ask_price: Optional[float] = None

    @property
    def two_sided(self) -> bool:
        return self.bid_price is not None and self.ask_price is not None

    @property
    def empty(self) -> bool:
        return self.bid_price is None and self.ask_price is None


@dataclass(frozen=True)
class QuoteDecision:
    action: LoopAction
    reason: str
    """Operator-facing. Every withdrawal must say why, because a book that is
    down for a good reason and a book that is down by accident look identical
    from outside."""


#: How stale an oracle may be before we stop trusting it to price a quote.
#:
#: The venue resamples every 5s and measures depth against a *fresh* oracle.
#: Quoting against a stale copy produces orders that look right locally and
#: are rejected or purged there, scoring nothing -- and nothing on our side
#: reports that. Three resample periods is generous without being blind.
MAX_ORACLE_AGE_S = 15.0

#: Re-quote when our resting price has drifted this fraction of the way to
#: the ring edge.
#:
#: Not on every tick: a refresh costs a mutate token and leaves the book
#: momentarily empty between states, and depth accrues continuously. Not at
#: the edge either -- an order that reaches the boundary has already spent
#: time earning nothing, and the oracle here moves 10-13% in seconds.
REQUOTE_AT_RING_FRACTION = 0.6


def decide(
    view: VenueView,
    resting: RestingQuote,
    *,
    ring_pct: float = 2.0,
    quote_when_carried: bool = True,
) -> QuoteDecision:
    """One action for the current state. Total and side-effect free."""

    # Order matters below: the checks that make quoting UNSAFE come before
    # the ones that make it merely unnecessary.

    if view.trading_paused:
        # The single thing the sponsor said bots must handle. Quoting through
        # a pause earns rejects, and the Sunday reset happens inside one --
        # so every entrant passes through this state on the way in.
        return QuoteDecision(LoopAction.WITHDRAW, "venue reports trading paused")

    if not view.session_ok:
        if view.session_waiting:
            return QuoteDecision(LoopAction.WAIT, "session renewal backing off")
        return QuoteDecision(LoopAction.WITHDRAW, "no usable trading session")

    if view.oracle is None or not (view.oracle > 0.0):
        return QuoteDecision(LoopAction.WITHDRAW, "no oracle price available")

    if view.oracle_age_s > MAX_ORACLE_AGE_S:
        return QuoteDecision(
            LoopAction.WITHDRAW,
            "oracle is %.1fs stale (limit %.0fs); a quote priced against it "
            "would be rejected or purged and would score nothing"
            % (view.oracle_age_s, MAX_ORACLE_AGE_S),
        )

    if view.carried and not quote_when_carried:
        return QuoteDecision(
            LoopAction.WITHDRAW,
            "carried session and overnight quoting is disabled (8x stressed "
            "initial margin, and resting size is hunted out of hours)",
        )

    if view.just_reopened:
        # Not HOLD, whatever we think is resting. The sequencer cancelled it.
        return QuoteDecision(
            LoopAction.QUOTE,
            "cash session reopened -- the venue cancelled every resting order "
            "at the open, so the book must be rebuilt rather than assumed",
        )

    if resting.empty:
        return QuoteDecision(LoopAction.QUOTE, "no quote resting")

    if not resting.two_sided:
        # The expensive state. min(bid, ask) is zero, so this market is
        # earning nothing at all until both sides are back.
        side = "ask" if resting.bid_price is not None else "bid"
        return QuoteDecision(
            LoopAction.QUOTE,
            "%s side is gone -- depth credit is min(bid, ask), so this market "
            "earns ZERO until both sides rest again" % side,
        )

    trigger = ring_pct * REQUOTE_AT_RING_FRACTION
    for price in (resting.bid_price, resting.ask_price):
        drift = abs(price - view.oracle) / view.oracle * 100.0
        if drift >= trigger:
            return QuoteDecision(
                LoopAction.QUOTE,
                "resting quote has drifted %.2f%% from the oracle (re-quote at "
                "%.2f%%, ring %.2f%%)" % (drift, trigger, ring_pct),
            )

    return QuoteDecision(LoopAction.HOLD, "two-sided and inside the ring")
