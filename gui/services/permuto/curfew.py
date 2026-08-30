"""Inventory curfew: how much position we may hold as the close approaches.

[CURFEW 2026-08-30]

WHY THIS EXISTS
---------------
A competitor described the trade that beat market makers in the previous
competition (Discord, svPerps, 2026-08-30): buy long while the underlying
market is CLOSED, then collect when it opens.  At the open the oracle jumps
to reality, short MMs are liquidated, and ADL hands the longs their exit.
WE ARE THE MM on the other side of that.

WHAT THE VENUE ACTUALLY DOES, MEASURED
--------------------------------------
The obvious defence -- "stop when the venue pauses" -- does not exist.
Sampled live on Sunday 2026-08-30 with US equities closed:

    /info/oracle   NVDA-VOL 0.3245143018339102  (4 samples / 24s)
    /info/meta     trading_paused: False, every market "active"

All three oracles were identical to sixteen digits while the venue happily
kept trading.  So the venue does NOT pause overnight: it keeps matching
orders against a FROZEN oracle.  That is the whole mechanism -- the exploit
does not need a gap in the venue, it needs a gap between the venue's stale
price and tomorrow's real one.

Two consequences shape everything below:

  * `trading_paused` is useless as a close signal (it is False right now,
    with the market shut).  /info/meta has no next-close field either:
    paused_at / pause_resume_at populate only once ALREADY paused.
  * The dangerous window is not the instant of the close, it is the whole
    frozen stretch.  A curfew that lifted at 16:00:01 would defend nothing,
    because the position that kills us is the one we accumulate at 22:00
    against a price from 16:00.

SO THE DEFENCE IS A POSITION CAP ON A CLOCK, NOT AN EXIT ROUTINE
----------------------------------------------------------------
This module answers exactly one question -- "how many dollars of inventory
may we hold right now?" -- and the runner feeds that answer to the
`max_position` argument `risk.assess()` already takes.  Everything
downstream is machinery that already exists and is already tested: at the
cap `risk.py` returns REDUCE_ONLY, and the runner cancels the market and
rests the single shrinking leg with reduce_only=True, ALO, inside the ring.

That is worth stating plainly, because it is the design: WE EXIT AS A
MAKER.  Shrinking the cap gradually from 90 minutes out means the position
is worked off by resting orders that earn the spread, instead of being
dumped through it at the bell.  Nothing here submits a taker order, so
risk.py's standing rule -- that crossing the spread to close is a spend
this process cannot price blind, and stays an operator decision -- is not
touched.  The residue that does not fill is bounded by the floor, and an
automated taker sweep for that residue remains a separate, opt-in decision.

THE CLOCK, AND WHY IT IS A WRITTEN-DOWN TABLE
--------------------------------------------
`zoneinfo.ZoneInfo("America/New_York")` raises ZoneInfoNotFoundError on
this machine: no tzdata, no pytz, no dateutil, and the GUI bundle is
lock-pinned, so adding one before a contest is a build risk taken for no
gain.  `scripts/permuto_depth_probe.py` already set the house precedent --
"a hardcoded offset that is written down beats a silent wrong conversion".
The contest is five days, entirely inside EDT (UTC-4), with no holiday
inside it (Labor Day 2026 is Sep 7, after the window).  So the sessions are
a table of epochs that can be checked by eye, and DST is never computed and
therefore never computed wrong.

AND WHY THE TABLE IS NOT TRUSTED ALONE
--------------------------------------
A hardcoded table expires, and a wrong clock is a silent failure.  The
oracle freeze is ground truth for "the underlying is shut" and cannot
expire, so the two are combined ASYMMETRICALLY:

    tighten  if the schedule says so OR the oracle is frozen
    relax    only if the schedule says in-session AND the oracle is moving

A clock wrong in the dangerous direction (thinks mid-session, actually
closed) is caught by the freeze.  Wrong in the cheap direction it costs
depth credit, not money.  A single stray overnight print cannot lift the
curfew, because lifting needs both conditions.  Past the end of the table
the schedule abstains entirely and the freeze governs alone -- degrading to
the observable truth rather than to a constant, in either direction.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Mapping, Optional, Sequence

__all__ = [
    "CLOSES_UTC",
    "OPENS_UTC",
    "CurfewState",
    "OracleFreeze",
    "Stage",
    "assess_curfew",
]


# --------------------------------------------------------------------------- #
# The session table.  16:00 ET close / 09:30 ET open, EDT = UTC-4, so 20:00Z
# and 13:30Z.  Contest week only: Mon 2026-08-31 .. Fri 2026-09-04.
# Verified by eye against the arithmetic in the module docstring.
# --------------------------------------------------------------------------- #

CLOSES_UTC: tuple[float, ...] = (
    1788206400.0,   # Mon 2026-08-31 20:00Z = 16:00 EDT
    1788292800.0,   # Tue 2026-09-01
    1788379200.0,   # Wed 2026-09-02
    1788465600.0,   # Thu 2026-09-03
    1788552000.0,   # Fri 2026-09-04  -- contest ends here
)

OPENS_UTC: tuple[float, ...] = (
    1788183000.0,   # Mon 2026-08-31 13:30Z = 09:30 EDT
    1788269400.0,   # Tue 2026-09-01
    1788355800.0,   # Wed 2026-09-02
    1788442200.0,   # Thu 2026-09-03
    1788528600.0,   # Fri 2026-09-04
)

#: Seconds before the close at which the cap starts shrinking.
RAMP_START_S = 5400.0          # 90 minutes

#: Seconds before the close at which the cap reaches its floor.
EXIT_START_S = 900.0           # 15 minutes

#: How long after an open before the full cap is restored.  The first
#: minutes of the session are where the overnight gap actually prints; the
#: cap staying low through them is the point, not an oversight.
SETTLE_AFTER_OPEN_S = 900.0    # 15 minutes

#: An oracle whose value has not moved for this long means the underlying
#: is shut.  These are realised-vol estimates resampled every few seconds
#: and carried to sixteen digits: during a live session they never repeat.
FREEZE_CONFIRM_S = 180.0

#: Floor as a fraction of the operator's full cap.  NOT zero, and not
#: negotiable: risk.assess() reads `if max_position > 0.0 and ...`, so a
#: non-positive limit SKIPS the position check entirely and means "no
#: limit" -- a ramp to zero would silently restore unlimited inventory at
#: the exact moment it was trying to forbid it.
FLOOR_FRACTION = 0.125


class Stage(str, Enum):
    SESSION = "session"
    """Mid-session and settled: the operator's full cap applies."""

    RAMP = "ramp"
    """Approaching the close: the cap is shrinking toward the floor, so
    risk.assess() turns a too-large position into maker-side REDUCE_ONLY
    quotes with time left for them to fill."""

    EXIT = "exit"
    """The last minutes before the close: cap pinned at the floor."""

    CLOSED = "closed"
    """The underlying is shut -- by the table, by a frozen oracle, or
    both.  The venue is still matching orders against a stale price, which
    is precisely when inventory must not be built."""

    SETTLING = "settling"
    """Just after an open, or an open the oracle has not yet confirmed.
    The cap stays at the floor until the price is demonstrably moving."""

    UNSCHEDULED = "unscheduled"
    """Outside the table.  The schedule abstains; the freeze detector
    governs alone."""


@dataclass
class OracleFreeze:
    """Tracks whether any oracle value has moved recently.

    Ground truth for "the underlying is shut", and the check on the
    hardcoded table.  Deliberately dumb: it stores the last value seen per
    market and the last time ANY of them changed.
    """

    confirm_s: float = FREEZE_CONFIRM_S
    _last: dict = field(default_factory=dict)
    _changed_at_s: float = 0.0
    _seeded: bool = False

    def observe(self, now_s: float, oracles: Mapping[str, float]) -> None:
        """Record a reading.  An EMPTY reading is not evidence.

        A venue we cannot read tells us nothing about whether the
        underlying is moving, and treating "no data" as "unchanged" would
        let an outage masquerade as a market close.  The elapsed time
        still accrues, so an outage that outlasts confirm_s reports frozen
        -- which tightens, the safe direction under uncertainty.
        """
        if not oracles:
            return
        changed = False
        for market, value in oracles.items():
            if self._last.get(market) != value:
                changed = True
            self._last[market] = value
        if changed or not self._seeded:
            self._changed_at_s = now_s
            self._seeded = True

    def frozen(self, now_s: float) -> bool:
        """True once nothing has moved for `confirm_s`.

        False before the first reading: absence of observation is not
        observation of absence, and a fresh process must not declare the
        market shut because it has not looked yet.
        """
        if not self._seeded:
            return False
        return (now_s - self._changed_at_s) >= self.confirm_s

    def seconds_still(self, now_s: float) -> float:
        """How long nothing has moved, for logging.  -1 before seeding."""
        if not self._seeded:
            return -1.0
        return max(0.0, now_s - self._changed_at_s)


@dataclass(frozen=True)
class CurfewState:
    """How much inventory is allowed, and why."""

    stage: Stage
    cap_usd: float
    reason: str
    seconds_to_close: float = -1.0


def _schedule_stage(
    now_s: float,
    closes: Sequence[float],
    opens: Sequence[float],
) -> tuple[Stage, float, float]:
    """``(stage, seconds_to_close, seconds_since_open)`` from the table alone.

    seconds_to_close is -1 when no future close is scheduled;
    seconds_since_open is -1 when no open has happened yet.
    """
    future_closes = [c for c in closes if c > now_s]
    if not future_closes:
        return Stage.UNSCHEDULED, -1.0, -1.0

    close = min(future_closes)
    past_opens = [o for o in opens if o <= now_s]
    since_open = (now_s - max(past_opens)) if past_opens else -1.0

    # Inside a session iff the most recent open belongs to the session that
    # this close ends -- i.e. an open has happened and it is not older than
    # the previous close.
    previous_closes = [c for c in closes if c <= now_s]
    last_close = max(previous_closes) if previous_closes else float("-inf")
    in_session = bool(past_opens) and max(past_opens) > last_close

    if not in_session:
        return Stage.CLOSED, close - now_s, since_open

    to_close = close - now_s
    if since_open >= 0.0 and since_open < SETTLE_AFTER_OPEN_S:
        return Stage.SETTLING, to_close, since_open
    if to_close <= EXIT_START_S:
        return Stage.EXIT, to_close, since_open
    if to_close <= RAMP_START_S:
        return Stage.RAMP, to_close, since_open
    return Stage.SESSION, to_close, since_open


def _ramp_cap(full_usd: float, floor_usd: float, to_close: float) -> float:
    """Linear cap between RAMP_START_S and EXIT_START_S."""
    span = RAMP_START_S - EXIT_START_S
    if span <= 0.0:
        return floor_usd
    progress = (RAMP_START_S - to_close) / span
    progress = min(1.0, max(0.0, progress))
    return full_usd - (full_usd - floor_usd) * progress


def assess_curfew(
    now_s: float,
    full_cap_usd: float,
    *,
    frozen_oracle: bool,
    floor_usd: Optional[float] = None,
    closes: Sequence[float] = CLOSES_UTC,
    opens: Sequence[float] = OPENS_UTC,
) -> CurfewState:
    """The inventory cap for right now.  Total and side-effect free.

    `full_cap_usd` is the operator's configured limit; the result never
    exceeds it and never reaches zero (see FLOOR_FRACTION).
    """
    if not (full_cap_usd > 0.0):
        # No configured limit means "unlimited" downstream; a curfew cannot
        # be expressed as a fraction of it, so say so rather than inventing
        # a number the operator never set.
        return CurfewState(Stage.UNSCHEDULED, full_cap_usd,
                           "no position limit configured; curfew inactive")

    floor = floor_usd if floor_usd is not None else full_cap_usd * FLOOR_FRACTION
    floor = max(floor, full_cap_usd * 0.01)   # never zero: see FLOOR_FRACTION
    floor = min(floor, full_cap_usd)

    stage, to_close, since_open = _schedule_stage(now_s, closes, opens)

    # Past the table the schedule abstains and the observable truth governs.
    if stage is Stage.UNSCHEDULED:
        if frozen_oracle:
            return CurfewState(
                Stage.CLOSED, floor,
                "no session scheduled and the oracle is frozen -- the "
                "underlying is shut", to_close)
        return CurfewState(
            Stage.UNSCHEDULED, full_cap_usd,
            "no session scheduled; the oracle is moving, so trading "
            "normally", to_close)

    # ASYMMETRIC COMBINATION. A frozen oracle can only ever tighten, and it
    # overrides any in-session claim the table makes -- that is the case
    # where the table is wrong in the direction that costs money.
    if frozen_oracle and stage in (Stage.SESSION, Stage.RAMP, Stage.EXIT,
                                   Stage.SETTLING):
        return CurfewState(
            Stage.CLOSED, floor,
            "the oracle has stopped moving -- treating the underlying as "
            "shut regardless of the schedule", to_close)

    if stage is Stage.CLOSED:
        return CurfewState(
            stage, floor,
            "the underlying is closed; the venue still matches orders "
            "against a stale price", to_close)

    if stage is Stage.SETTLING:
        return CurfewState(
            stage, floor,
            "the session has just opened; holding the floor until the "
            "opening move has printed", to_close)

    if stage is Stage.EXIT:
        return CurfewState(
            stage, floor,
            "%.0f minutes to the close; cap at the floor so nothing new "
            "is carried" % (to_close / 60.0), to_close)

    if stage is Stage.RAMP:
        cap = _ramp_cap(full_cap_usd, floor, to_close)
        return CurfewState(
            stage, cap,
            "%.0f minutes to the close; cap ramping %.0f -> %.0f USD so "
            "inventory is worked off as a maker"
            % (to_close / 60.0, full_cap_usd, floor), to_close)

    return CurfewState(stage, full_cap_usd, "mid-session", to_close)
