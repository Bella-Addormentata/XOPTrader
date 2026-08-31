"""Re-anchor leg prices to the venue's oracle immediately before sending.

[PREFLIGHT 2026-08-31]

WHY band_guard WAS NOT ENOUGH
-----------------------------
band_guard clamps each leg to +/-window of the oracle read the tick was
priced against, and derives staleness from `oracle_age_s` -- the time
since WE last fetched successfully.  Both anchors are wrong in the case
that matters.  On 2026-08-31 the venue refused

    Price 0.047 outside band [0.0420, 0.0464] (+/-5% of oracle 0.04423)

with our fetch only seconds old.  A two-second-old read is "fresh" by
age and still 5% behind when volatility is collapsing ~20% a minute, and
clamping to a stale anchor just keeps you near the stale anchor.  Age is
not divergence.

WHAT THIS DOES INSTEAD
----------------------
Fetch the oracle again immediately before the send, and validate against
THAT.  The remaining exposure is only the request's own flight time, not
a whole tick, and flight time is measured rather than assumed: the
caller reports how long the pre-send fetch took, and that round trip is
the best available estimate of the next one.

Then, per leg:

  * inside the venue band with room to spare -> send unchanged;
  * outside, but a legal price exists -> clamp to the safe edge;
  * no legal price survives the projected drift -> DROP the leg.

Dropping is a feature.  One out-of-band leg 400s the whole batch, so a
leg we decline to send is strictly cheaper than the rejection it would
have caused -- and unlike a rejection, its siblings survive.

WHEN THE MARKET IS TOO FAST, STAND DOWN
---------------------------------------
If projected drift alone exceeds the band, no price is safe at any
offset and quoting is a coin flip against the clock.  `stand_down`
reports that so the caller can skip the market and say why, instead of
discovering it one 400 at a time.

Pure module: arithmetic only, no I/O.  The caller owns the fetch.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Mapping, Optional

__all__ = [
    "PreflightOutcome",
    "preflight_leg_price",
    "quantise_toward",
    "rescaled_size",
    "stand_down",
]

#: The venue's price grid (/info/meta tick_size, 0.0001 on every market
#: today).  Not yet threaded from meta -- a follow-up; the value is stable
#: and a wrong-but-finer grid would only round more often, never produce
#: an illegal price.
DEFAULT_TICK = 0.0001

#: Whole contracts (/info/meta lot_size = 1).
DEFAULT_LOT = 1.0

#: Fraction of the band kept in reserve for what cannot be measured: the
#: venue's own resample cadence, clock skew, and the fact that the next
#: round trip is not the last one.
RESERVE_FRAC = 0.35


def stand_down(band_pct: float, latency_s: float,
               velocity_pct_per_s: float) -> bool:
    """True when projected in-flight drift leaves no safe price at all."""
    if band_pct <= 0.0:
        return True
    drift = max(0.0, latency_s) * max(0.0, velocity_pct_per_s)
    return drift >= band_pct * (1.0 - RESERVE_FRAC)


@dataclass(frozen=True)
class PreflightOutcome:
    """What to do with one leg. `price` is 0.0 when it must be dropped."""

    price: float
    changed: bool = False
    dropped: bool = False
    reason: str = ""


def preflight_leg_price(
    price: float,
    fresh_oracle: float,
    *,
    band_pct: float,
    latency_s: float,
    velocity_pct_per_s: float,
    is_buy: bool,
    ring_pct: float = 2.0,
) -> PreflightOutcome:
    """Re-anchor one leg to the freshest oracle.

    The usable half-width is the band less a reserve less the projected
    drift.  A clamp always moves the price TOWARD the oracle, so it can
    only reduce how aggressive the quote is relative to the band; whether
    the result crosses the live book is the venue's per-leg ALO check,
    which refuses politely rather than failing the batch.
    """
    if not (price > 0.0) or not (fresh_oracle > 0.0):
        return PreflightOutcome(0.0, dropped=True,
                                reason="no usable price or oracle")
    if stand_down(band_pct, latency_s, velocity_pct_per_s):
        return PreflightOutcome(
            0.0, dropped=True,
            reason="oracle moving %.2f%%/s; %.0fms of flight time exceeds "
                   "the %.1f%% band" % (velocity_pct_per_s,
                                        latency_s * 1000.0, band_pct))

    drift = max(0.0, latency_s) * max(0.0, velocity_pct_per_s)
    usable = band_pct * (1.0 - RESERVE_FRAC) - drift
    if usable <= 0.0:
        return PreflightOutcome(0.0, dropped=True,
                                reason="no usable width after drift")

    lo = fresh_oracle * (1.0 - usable / 100.0)
    hi = fresh_oracle * (1.0 + usable / 100.0)

    # SIDE MATTERS, and an earlier version of this took `is_buy` and then
    # ignored it -- clamping a too-high BID up to the BAND ceiling, 3.25%
    # above the oracle, which build_upsert_batch rightly refused as
    # "aggressive OUTSIDE the +/-2% ring".
    #
    # The binding limit in the aggressive direction is the RING, not the
    # oracle midpoint. Capping at the midpoint instead would forbid an ask
    # priced fractionally below it -- which is exactly how inventory skew
    # leans when long, and is a perfectly legal maker quote. So: constrain
    # only the aggressive side, and only to the ring.
    ring = max(0.0, ring_pct)
    if is_buy:
        hi = min(hi, fresh_oracle * (1.0 + ring / 100.0))
    else:
        lo = max(lo, fresh_oracle * (1.0 - ring / 100.0))
    if lo > hi:
        return PreflightOutcome(0.0, dropped=True,
                                reason="no passive price inside the band")

    if lo <= price <= hi:
        return PreflightOutcome(price)

    clamped = hi if price > hi else lo
    return PreflightOutcome(
        clamped, changed=True,
        reason="re-anchored %.6f -> %.6f (oracle %.6f moved under it)"
               % (price, clamped, fresh_oracle))


def latest_oracle(
    fresh: Optional[Mapping[str, float]],
    fallback: Optional[Mapping[str, float]],
    market: str,
) -> float:
    """The freshest usable oracle for a market, else 0.0.

    A failed or malformed pre-send fetch must not blank the market: the
    tick's own read is worse but not useless, and standing down every
    time the extra request hiccups would hand the session away.
    """
    for source in (fresh, fallback):
        if not source:
            continue
        value = source.get(market)
        if isinstance(value, (int, float)) and value > 0.0:
            return float(value)
    return 0.0


def quantise_toward(price: float, oracle: float,
                    tick: float = DEFAULT_TICK) -> float:
    """Snap `price` to the venue tick grid, rounding TOWARD the oracle.

    [review] Re-anchoring produced prices off the grid -- the 0.09 clamp
    yields 0.092925, which is 929.25 ticks at the live 0.0001 size, and
    the venue's strict validator can refuse the leg and with it the whole
    batch.  So the clamp has to land ON the grid.

    Rounding is toward the oracle rather than by the maker convention
    (bids down, asks up) because the failure being fixed here is the BAND,
    and one tick is 0.05-0.2% at these prices -- small against a 5% band
    but the wrong direction still spends margin we may not have.  Crossing
    in the other direction is caught per-leg by ALO, which refuses one
    order politely instead of failing the batch.
    """
    if not (tick > 0.0) or not (price > 0.0):
        return price
    steps = price / tick
    snapped = (int(steps) * tick if price > oracle
               else (int(steps) + (1 if steps % 1 else 0)) * tick)
    # Never cross the oracle through rounding: a tick-sized overshoot past
    # the anchor flips the side of the quote.
    if price > oracle:
        snapped = max(snapped, oracle)
    else:
        snapped = min(snapped, oracle)
    return snapped if snapped > 0.0 else price


def rescaled_size(size: float, old_price: float, new_price: float,
                  lot: float = DEFAULT_LOT) -> float:
    """Hold NOTIONAL constant when a leg is re-anchored, floored to a lot.

    [review] The re-anchored price kept the size computed against the
    stale oracle, so an upward move inflated the leg's notional -- sizing
    at 0.09 and sending near 0.10 is ~11% more USD than target_depth_usd
    or the curfew cap allowed.  Re-pricing must not quietly re-size.
    """
    if not (size > 0.0) or not (old_price > 0.0) or not (new_price > 0.0):
        return size
    scaled = size * old_price / new_price
    if lot > 0.0:
        scaled = int(scaled / lot) * lot
    return scaled if scaled > 0.0 else 0.0
