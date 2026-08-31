"""Keep leg prices inside the venue's oracle band on ARRIVAL.

[BANDGUARD 2026-08-31, contest open]

The venue validates every order against +/-`vol_oracle_band_pct` (5%) of
ITS oracle at arrival time, and an out-of-band leg 400s the WHOLE batch.
Measured at the open: minutes where 100% of batches were rejected, always
while the oracle was moving fast, with clean 0%-rejected minutes in
between.  The arithmetic is exact: quotes are priced off an oracle read
that can be up to the 15s venue-state grace old, the ladder offsets up to
the +/-2% ring, and the opening collapse ran ~0.25%/s -- so
15 x 0.25 + 2 = 5.75% > 5%.  Nothing was mispriced; the price was simply
computed against a value the venue had already left behind.

The guard models exactly that drift: each market tracks the oracle's
velocity (EMA of |dP|/dt), the projected in-flight drift is
`age_s x velocity`, and the allowed offset from the CURRENT read shrinks
from the band by that projection plus a fixed safety.  Legs are clamped
into the surviving window -- ALO tif means a clamp that would cross the
book is refused by the venue per-leg rather than filled -- and when no
window survives the market is skipped for the tick, which is what the
venue would have done to us anyway, minus the 400.

Pure module: no I/O, no venue types.  The runner owns wiring.
"""

from __future__ import annotations

from dataclasses import dataclass, field

__all__ = ["BandGuard", "clamp_offset_window"]

#: The venue's published band (vol_oracle_band_pct).  Not yet wired from
#: /info/meta -- the value is stable and confirmed live; wiring it is a
#: follow-up so this stays a pure module.
VENUE_BAND_PCT = 5.0

#: Margin under the band for everything this model cannot see: the
#: venue's own oracle resample cadence, request latency, and the fact
#: that velocity is an EMA of the recent past rather than the next second.
SAFETY_PCT = 1.0


def clamp_offset_window(
    band_pct: float,
    safety_pct: float,
    oracle_age_s: float,
    velocity_pct_per_s: float,
) -> float:
    """Max % offset from the current oracle a leg may carry.  <= 0: skip.

    band - safety - age x velocity.  With a fresh oracle and a calm
    market this is ~4%, comfortably outside the 2% quoting ring, so the
    guard is a no-op exactly when the venue would accept everything.
    """
    if band_pct <= 0.0:
        return 0.0
    age = max(0.0, oracle_age_s)
    vel = max(0.0, velocity_pct_per_s)
    return band_pct - max(0.0, safety_pct) - age * vel


@dataclass
class BandGuard:
    """Per-market oracle velocity, and the clamp it implies."""

    band_pct: float = VENUE_BAND_PCT
    safety_pct: float = SAFETY_PCT
    #: EMA weight for new velocity samples.  High on purpose: the danger
    #: is a REGIME CHANGE (frozen -> collapsing), and a slow EMA would
    #: still be remembering the calm while the band rejects everything.
    alpha: float = 0.5
    _last: dict = field(default_factory=dict)     # market -> (t, price)
    _vel: dict = field(default_factory=dict)      # market -> %/s EMA

    def observe(self, now_s: float, oracles: dict) -> None:
        """Feed the tick's oracle read.  Call BEFORE pricing, every tick.

        Graced (re-served) reads repeat the same (t, price) sample; dt=0
        pairs are skipped, so a stall neither zeroes nor inflates the
        velocity -- the EMA simply holds its last estimate, which is the
        honest guess for a value nobody has re-measured.
        """
        for market, price in (oracles or {}).items():
            if not isinstance(price, (int, float)) or not price > 0.0:
                continue
            prev = self._last.get(market)
            self._last[market] = (now_s, price)
            if prev is None:
                continue
            t0, p0 = prev
            dt = now_s - t0
            if dt <= 0.0 or p0 <= 0.0 or price == p0:
                continue
            sample = abs(price - p0) / p0 * 100.0 / dt   # % per second
            old = self._vel.get(market, sample)
            self._vel[market] = old + self.alpha * (sample - old)

    def velocity(self, market: str) -> float:
        return self._vel.get(market, 0.0)

    def window_pct(self, market: str, oracle_age_s: float) -> float:
        return clamp_offset_window(self.band_pct, self.safety_pct,
                                   oracle_age_s, self.velocity(market))

    def clamp_price(
        self,
        market: str,
        oracle: float,
        price: float,
        oracle_age_s: float,
    ) -> float:
        """The leg price, pulled inside the window.  0.0 means skip.

        Clamping toward the oracle can only make a maker quote LESS
        aggressive relative to the band; whether it crosses the live book
        is the venue's per-leg ALO check, which refuses politely instead
        of the whole-batch 400 this module exists to prevent.
        """
        if not (oracle > 0.0) or not (price > 0.0):
            return 0.0
        window = self.window_pct(market, oracle_age_s)
        if window <= 0.0:
            return 0.0
        lo = oracle * (1.0 - window / 100.0)
        hi = oracle * (1.0 + window / 100.0)
        return min(hi, max(lo, price))
