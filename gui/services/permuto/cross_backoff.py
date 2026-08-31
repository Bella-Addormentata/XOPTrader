"""Learn how far from the oracle our quotes have to sit to actually rest.

THE PROBLEM THIS SOLVES, MEASURED. On 2026-08-31 the venue refused 51 legs
with "Post-only order would cross the book" -- the single largest failure
class that a frozen oracle does not cure. A refused leg rests nothing, and
depth-seconds accrue on ``min(bid, ask)`` of what RESTS, so every one of
those refusals banked exactly zero.

WHY WE CANNOT JUST LOOK AT THE BOOK. Permuto publishes no L2/orderbook/
ticker route -- probed 2026-08-31, they all fall through to the SPA's HTML.
So the best bid/offer we are crossing into is not observable. What IS
observable is the venue telling us, per leg, that we crossed it. That is a
feedback signal, and one signal is enough to converge on a resting price
without ever seeing the book.

WHY BACKING OFF IS FREE. ``orders.depth_credit_usd`` counts a leg's FULL
notional whenever ``|price - oracle| <= ring_pct``, and gives no extra
credit for being tight. A leg at 1.6% from the oracle earns exactly what a
leg at 0.25% earns. So widening away from the book costs nothing in
eligibility while it buys a much better chance of resting -- and, as a
second-order benefit, far fewer fills, which is how the inventory that
wrecked the account was accumulated in the first place.

THE CEILING IS NOT NEGOTIABLE. Outside the ring the credit is zero, so a
backoff that escapes the ring converts a rejected leg into a resting leg
that earns nothing -- failure wearing success's clothes. ``headroom_pct``
is what the caller must respect, and it accounts for the skew that will be
added on top.
"""

from __future__ import annotations

import logging

_log = logging.getLogger(__name__)

#: How much further out to go after a crossing refusal, in percent of the
#: oracle. One tick is far too small to matter (0.0001 on a 0.09 oracle is
#: 0.1%), and a whole ring is a panic. A quarter of a percent converges in a
#: handful of ticks without throwing away the inner ring in one step.
BACKOFF_STEP_PCT = 0.25

#: Decay applied on a tick where the market's legs were NOT refused for
#: crossing. Multiplicative so the retreat is undone gradually: an instant
#: reset would re-cross on the very next tick and oscillate, which is the
#: classic way a feedback controller turns into a square wave.
BACKOFF_DECAY = 0.85

#: Below this the backoff is simply dropped to zero. Without it the
#: multiplicative decay leaves an ever-smaller residue forever and the
#: market never returns to its configured placement.
BACKOFF_FLOOR_PCT = 0.02


class CrossBackoff:
    """Per-market extra offset, learned from the venue's own refusals.

    Not a predictor. It carries no model of the book and makes no forecast:
    it widens when told it crossed, and relaxes when it is not. That is the
    whole design, and it is deliberate -- a model of an unobservable book
    would be a guess dressed up as arithmetic.
    """

    def __init__(self) -> None:
        self._pct: dict = {}

    def offset_pct(self, market: str) -> float:
        """Extra percent to add to this market's placement offset."""
        return float(self._pct.get(market, 0.0))

    def observe_cross(self, market: str, headroom_pct: float) -> float:
        """The venue refused a leg here for crossing. Widen, within headroom.

        Returns the new offset. Clamped to ``headroom_pct`` because a leg
        outside the credit ring earns nothing: there is no point retreating
        to a price that rests and scores zero.
        """
        if not (headroom_pct > 0.0):
            # No room to retreat into. Say so rather than silently widening
            # out of the ring -- the caller's spread is simply too tight for
            # this market and that is a configuration problem, not something
            # a controller can fix by trying harder.
            if self._pct.get(market):
                self._pct[market] = 0.0
            return 0.0
        current = self.offset_pct(market)
        widened = min(current + BACKOFF_STEP_PCT, headroom_pct)
        if widened != current:
            _log.info(
                "permuto: %s crossed the book at +%.2f%% -- backing off to "
                "+%.2f%% (headroom %.2f%%). Depth credit is flat inside the "
                "ring, so this costs nothing if it rests.",
                market, current, widened, headroom_pct)
        self._pct[market] = widened
        return widened

    def observe_clean(self, market: str) -> float:
        """No crossing refusal for this market on this tick. Relax a little."""
        current = self.offset_pct(market)
        if current <= 0.0:
            return 0.0
        relaxed = current * BACKOFF_DECAY
        if relaxed < BACKOFF_FLOOR_PCT:
            relaxed = 0.0
            _log.info("permuto: %s back to its configured placement", market)
        self._pct[market] = relaxed
        return relaxed

    def forget(self, market: str) -> None:
        """Drop the learned offset (market withdrawn, or session boundary)."""
        self._pct.pop(market, None)


def headroom_pct(ring_pct: float, half_spread_pct: float,
                 skew_frac_abs: float = 0.0) -> float:
    """How far out we may push before the leg leaves the credit ring.

    The skew already applied to this pair is subtracted, because backoff and
    skew both push in the same direction for the trailing leg and the ring
    does not care which of them was responsible for the leg landing outside
    it. Never negative -- a spread wider than the ring has no room at all,
    and returning a negative would invert the retreat into an advance.
    """
    if not (ring_pct > 0.0):
        return 0.0
    room = ring_pct - abs(half_spread_pct) - abs(skew_frac_abs) * 100.0
    return room if room > 0.0 else 0.0
