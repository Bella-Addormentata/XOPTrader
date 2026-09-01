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
    "OVERNIGHT_LONG_FRACTION",
    "OVERNIGHT_SHORT_FRACTION",
    "PREOPEN_EXIT_S",
    "permitted_leg_size",
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

#: How long BEFORE the open the overnight short side is shut again.
#:
#: [review 2026-08-31] PR #130 claimed "EXIT/SETTLING retract the book
#: before the open".  They do not.  SETTLING is gated on
#: `since_open >= 0`, so it begins AFTER the bell; the whole night is
#: Stage.CLOSED, and with a non-zero overnight short cap a two-sided book
#: rests straight through the opening jump.  The resting ASK is then swept
#: BY the gap -- filled short at pre-gap prices into a +73% to +229% move,
#: which is the mechanism that cost this account $523k of unrealised P&L
#: on contest night one.
#:
#: 30 minutes because the gap is an instant event at a known clock time,
#: so this only has to cover clock skew and the cancel round trip, not a
#: forecast.  The BID stays open: being long into an upward vol gap is the
#: harmless side, and closing both would forfeit the tail of the cheapest
#: depth window of the week for no risk reduction.
PREOPEN_EXIT_S = 1800.0        # 30 minutes

#: An oracle whose value has not moved for this long means the underlying
#: is shut.  These are realised-vol estimates resampled every few seconds
#: and carried to sixteen digits: during a live session they never repeat.
FREEZE_CONFIRM_S = 180.0

#: How much LONG inventory the curfew tolerates overnight, as a fraction
#: of the full cap.  Deliberately larger than the short side.
#:
#: WHY THE SIDES ARE NOT SYMMETRIC.  The oracle is a 60-second trailing
#: realised-vol estimate.  Overnight it is frozen on a calm end-of-day
#: window, while the first print after the reopen is computed from the
#: most violent minute of the trading day -- the opening auction absorbing
#: everything that happened overnight.  So the reopening print is
#: systematically ABOVE the frozen one, and a SHORT vol position carried
#: through it is the side that gets liquidated.  That is not a forecast,
#: it is the shape of the estimator, and it is exactly what "MMs that were
#: short went bust" describes.
#:
#: This is a DEFENSIVE asymmetry, not a carry bet: we simply decline to
#: keep selling the side that is structurally mispriced by a stale oracle,
#: while still accepting fills from someone selling vol to us cheaply.
#: Whether ACCUMULATING long overnight is profitable depends on funding,
#: which settles every 60s here and has not been measured across a real
#: close -> open cycle.  Until it has, keep this modest.
OVERNIGHT_LONG_FRACTION = 0.25

#: How much NEW SHORT exposure the curfew tolerates overnight.  Small, and
#: strictly tighter than the long side: a stale oracle structurally
#: misprices the short, and the short is the side an opening gap
#: liquidates.
#:
#: Expressed HERE and not through the position limit, which is the whole
#: reason the per-leg veto exists as a separate mechanism.  risk.assess()
#: reads `if max_position > 0.0`, so handing it zero would mean "no limit"
#: -- the exact inversion.  The per-leg veto has no such overload: it
#: bounds a leg that would grow the short side, while any leg that REDUCES
#: an existing short stays permitted, so a position carried in from the
#: session can always be worked off.
#:
#: [2026-08-31, MEASURED] It was 0.0, and 0.0 costs the entire overnight
#: session's eligibility, because depth credit is min(bid, ask): with the
#: ask side permitted at exactly zero size, min(bid, 0) = 0 and a FLAT,
#: fully-funded account still banks nothing overnight.  That is not a
#: theoretical loss.  On contest night one, five entrants compounded
#: after hours -- the leader ran $19,728/s and passed 86,000,000
#: depth-seconds while we sat at 4,093.892 -- and with a frozen oracle
#: overnight is structurally the CHEAPEST depth of the week: no drift, no
#: band risk, quotes simply rest.  Our own curfew was the thing closing
#: that window, not the venue and not the position.
#:
#: So: small and non-zero.  A BALANCED two-sided book becomes legal; a
#: directional short still does not.  Why 0.10 specifically, and why this
#: is not just "0.0 but braver":
#:
#:   * The danger is inventory carried INTO the reopen, where the measured
#:     gap runs +73% to +229%.  Stage.PREOPEN -- NOT EXIT and NOT SETTLING,
#:     both of which were claimed here before and neither of which does
#:     this -- shuts the SHORT SIDE for the last PREOPEN_EXIT_S before each
#:     open.  Say what that actually protects: no ASK is resting for the
#:     gap to sweep.  The bid is deliberately left open, so the book is not
#:     "retracted" and describing it that way was wrong twice over.  What
#:     this fraction bounds is what an overnight FILL can leave us holding,
#:     which no retraction can undo.
#:   * At a $250k full cap that is $25k of overnight short notional per
#:     market.  At the worst gap ever measured here (+229%) that leg marks
#:     against us by ~$57k -- survivable on a healthy book, and bounded
#:     rather than open-ended.
#:   * It is a SECOND limit, not the only one.  risk.assess() independently
#:     refuses to add risk above 50% margin utilisation, so this fraction
#:     can never be the sole thing standing between us and a reopen.
#:
#: Raising this further is not free and should not be done without
#: re-measuring the reopen gap: the asymmetry with the long side is
#: deliberate, because a short is the side a stale oracle misprices and
#: the side that gets liquidated at the open.
OVERNIGHT_SHORT_FRACTION = 0.10

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

    PREOPEN = "preopen"
    """The last half hour before the bell.  The overnight short side is
    shut again so no ask is resting when the opening gap lands: SETTLING
    starts only AFTER the open, so without this stage there is no
    pre-open retraction at all, whatever the comments used to say."""

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
    _changed_at_by_market_s: dict = field(default_factory=dict)
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
            if market not in self._last:
                # [review] A FIRST SIGHTING IS NOT A PRINT. Seeding used to
                # go through the inequality below -- None != value -- and so
                # stamped the market as having just moved. A runner started
                # during SETTLING then read a frozen pre-open oracle as
                # fresh for the whole confirm_s window and quoted against
                # it, which is the exact cold-start case the freshness gate
                # exists to catch. Seed the comparison state only;
                # market_frozen() treats an unseen market as FROZEN, which
                # is the safe direction to be wrong in.
                self._last[market] = value
                continue
            if self._last[market] != value:
                changed = True
                self._changed_at_by_market_s[market] = now_s
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

    def market_frozen(self, market: str, now_s: float) -> bool:
        """True once this market has not changed for `confirm_s`.

        Unseen markets are treated as frozen: unknown freshness must tighten.
        """
        changed_at = self._changed_at_by_market_s.get(market)
        if changed_at is None:
            return True
        return (now_s - changed_at) >= self.confirm_s


@dataclass(frozen=True)
class CurfewState:
    """How much inventory is allowed, and why.

    `cap_usd` is the symmetric baseline -- the tightest cap in force, and
    what the stage is named for.  The per-side caps differ only while the
    curfew is tightening: see OVERNIGHT_LONG_FRACTION for why the short
    side is the dangerous one.
    """

    stage: Stage
    cap_usd: float
    reason: str
    seconds_to_close: float = -1.0
    long_cap_usd: Optional[float] = None
    short_cap_usd: Optional[float] = None
    #: What the CLOCK said, before a frozen oracle overrode it.
    #:
    #: [review] The effective stage is deliberately lossy: a frozen oracle
    #: maps a scheduled SETTLING back to PREOPEN so the short side stays
    #: shut. That is right for CAPS and wrong for POSTURE -- a consumer
    #: asking "has the bell rung?" cannot tell a genuine pre-open PREOPEN
    #: (oracle frozen because the market is shut, quoting is fine) from a
    #: post-open one (oracle frozen because it has not printed yet, quoting
    #: is the stale-price trap). Both need to be answerable, so both are
    #: reported.
    schedule_stage: Optional[Stage] = None

    def __post_init__(self):
        # None means "unset" and mirrors the symmetric cap, so a state built
        # without per-side values behaves exactly as it did before they
        # existed.  ZERO is a real value and must survive: it is how the
        # overnight short prohibition is expressed.
        if self.long_cap_usd is None:
            object.__setattr__(self, "long_cap_usd", self.cap_usd)
        if self.short_cap_usd is None:
            object.__setattr__(self, "short_cap_usd", self.cap_usd)

    def cap_for(self, position: float) -> float:
        """The cap that binds given the position we actually hold.

        Flat takes the looser of the two: no side is at its limit yet, and
        the per-leg veto -- not this number -- is what stops the forbidden
        side being GROWN from zero.
        """
        if position != position:                      # NaN
            return min(self.long_cap_usd, self.short_cap_usd)
        if position > 0.0:
            return self.long_cap_usd
        if position < 0.0:
            return self.short_cap_usd
        return max(self.long_cap_usd, self.short_cap_usd)


def permitted_leg_size(
    is_buy: bool,
    position: float,
    requested_size: float,
    long_cap_contracts: float,
    short_cap_contracts: float,
) -> float:
    """How much of this leg may rest.  0.0 means "do not place it".

    [review] THIS RETURNS A SIZE, NOT A BOOLEAN, AND THAT IS THE WHOLE
    POINT.  The first version voted yes/no on a leg whose size it never
    read, reasoning "selling reduces a long, so permit it".  True only up
    to the SIZE of the long: the ladder leg is sized to target_depth_usd,
    so one contract of long inventory waved through a full $1,200 ask, and
    filling it left us $1,196 SHORT overnight -- four times the long cap
    and precisely the position this module exists to forbid.  A size-blind
    veto cannot express "reduces"; it can only express "reduces by at most
    this much".

    The room on each side is the distance from where we are to where that
    side's cap sits, which handles reduction and growth in one expression:
    a buy may travel from `position` up to `+long_cap`, a sell from
    `position` down to `-short_cap`.  With the overnight short cap at zero
    a sell is therefore permitted at exactly the size of the long it
    closes -- never one contract more.
    """
    if position != position or requested_size != requested_size:
        return 0.0                      # NaN: unreadable
    if not (requested_size > 0.0):
        return 0.0
    room = ((long_cap_contracts - position) if is_buy
            else (position + short_cap_contracts))
    if not (room > 0.0):
        return 0.0
    return min(requested_size, room)


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
        # Shut the short side before the bell.  next_open is the first open
        # still ahead of us; inside PREOPEN_EXIT_S of it the overnight ask
        # must already be gone, because the gap arrives at the open and a
        # resting ask is what it fills.
        future_opens = [o for o in opens if o > now_s]
        if future_opens and (min(future_opens) - now_s) <= PREOPEN_EXIT_S:
            return Stage.PREOPEN, close - now_s, since_open
        return Stage.CLOSED, close - now_s, since_open

    to_close = close - now_s
    if since_open >= 0.0 and since_open < SETTLE_AFTER_OPEN_S:
        return Stage.SETTLING, to_close, since_open
    if to_close <= EXIT_START_S:
        return Stage.EXIT, to_close, since_open
    if to_close <= RAMP_START_S:
        return Stage.RAMP, to_close, since_open
    return Stage.SESSION, to_close, since_open


def _side_caps(
    stage: "Stage",
    full_usd: float,
    to_close: float,
    long_target: float,
    short_target: float,
) -> tuple:
    """``(long_cap, short_cap)`` for a stage.  Monotone into the close.

    [review] Each side interpolates to ITS OWN overnight target.  The
    earlier version ramped one symmetric number down to a floor and then
    handed EXIT a long cap of 25% of full -- so the long allowance DOUBLED
    fifteen minutes before the bell, lifting REDUCE_ONLY and restarting
    inventory accumulation at the worst possible moment.
    """
    if stage is Stage.SESSION:
        return full_usd, full_usd
    if stage is Stage.RAMP:
        span = RAMP_START_S - EXIT_START_S
        progress = 1.0 if span <= 0.0 else (RAMP_START_S - to_close) / span
        progress = min(1.0, max(0.0, progress))
        return (full_usd - (full_usd - long_target) * progress,
                full_usd - (full_usd - short_target) * progress)
    if stage is Stage.SETTLING:
        # [review] TWO-SIDED, at the reduced size. The curfew exists to
        # stop us quoting against a STALE price; by SETTLING the oracle is
        # moving again, so that rationale is spent and the only remaining
        # concern is the size of the opening move -- which the reduced cap
        # already answers. Holding the ask closed here would forfeit depth
        # credit (min(bid, ask), so a one-sided book earns zero) through
        # the busiest quarter-hour of the session, for a danger that has
        # already passed.
        return long_target, long_target
    if stage is Stage.PREOPEN:
        # Short shut, long held.  Zero here is safe in a way it is NOT safe
        # as an overnight default: the per-leg veto reads this cap, and
        # risk.assess() never sees a zero because cap_for() on a flat or
        # long book returns the long side.  A short carried in still works
        # off, because permitted_leg_size allows a REDUCING buy regardless.
        return long_target, 0.0
    # EXIT / CLOSED: the overnight posture, held.
    return long_target, short_target


def assess_curfew(
    now_s: float,
    full_cap_usd: float,
    *,
    frozen_oracle: bool,
    floor_usd: Optional[float] = None,
    closes: Sequence[float] = CLOSES_UTC,
    opens: Sequence[float] = OPENS_UTC,
) -> CurfewState:
    """The inventory caps for right now.  Total and side-effect free."""
    if not (full_cap_usd > 0.0):
        # No configured limit means "unlimited" downstream; a curfew cannot
        # be expressed as a fraction of it, so say so rather than inventing
        # a number the operator never set.
        return CurfewState(Stage.UNSCHEDULED, full_cap_usd,
                           "no position limit configured; curfew inactive",
                           long_cap_usd=full_cap_usd,
                           short_cap_usd=full_cap_usd)

    long_target = (floor_usd if floor_usd is not None
                   else full_cap_usd * OVERNIGHT_LONG_FRACTION)
    long_target = max(0.0, min(full_cap_usd, long_target))
    # May be exactly zero -- see OVERNIGHT_SHORT_FRACTION.  Only the
    # size-aware permission reads it; the number handed to risk.assess() is
    # clamped positive by the runner so zero can never read as "no limit".
    short_target = max(0.0, min(full_cap_usd,
                                full_cap_usd * OVERNIGHT_SHORT_FRACTION))

    stage, to_close, since_open = _schedule_stage(now_s, closes, opens)

    def _state(effective: "Stage", reason: str) -> CurfewState:
        long_cap, short_cap = _side_caps(
            effective, full_cap_usd, to_close, long_target, short_target)
        return CurfewState(effective, long_cap, reason, to_close,
                           long_cap_usd=long_cap, short_cap_usd=short_cap,
                           schedule_stage=stage)

    # Past the table the schedule abstains and the observable truth governs.
    if stage is Stage.UNSCHEDULED:
        if frozen_oracle:
            return _state(Stage.CLOSED,
                          "no session scheduled and the oracle is frozen -- "
                          "the underlying is shut")
        return CurfewState(
            Stage.UNSCHEDULED, full_cap_usd,
            "no session scheduled; the oracle is moving, so trading "
            "normally", to_close,
            long_cap_usd=full_cap_usd, short_cap_usd=full_cap_usd)

    # ASYMMETRIC COMBINATION.  A frozen oracle can only ever tighten, and it
    # overrides any in-session claim the table makes -- that is the case
    # where the table is wrong in the direction that costs money.
    if frozen_oracle:
        # PREOPEN is STRICTLY TIGHTER than CLOSED (short cap 0 against the
        # overnight fraction), and this function's own rule is that a frozen
        # oracle may only ever tighten. Collapsing it to CLOSED here would
        # RAISE the short cap in the last half hour before the bell -- a
        # frozen oracle re-opening the short side in precisely the window
        # the stage exists to shut. Keep whichever is tighter.
        # PREOPEN is STRICTLY TIGHTER than CLOSED (short cap 0), and a
        # frozen oracle may only ever tighten -- so it must not be collapsed
        # away.
        #
        # [review] SETTLING maps here too, and that one is not obvious. The
        # first tick at or after the bell leaves PREOPEN for SETTLING; if
        # the oracle has NOT yet printed, mapping that to CLOSED restores
        # the overnight short cap and lets a fresh ask rest against a price
        # that is still stale -- re-opening the exact exposure PREOPEN
        # spent the previous half hour preventing, at the one moment the
        # gap is most likely to arrive. A frozen oracle after the open is
        # not "the session has started", it is "the session has not
        # started yet"; hold the zero-short posture until it moves.
        if stage in (Stage.PREOPEN, Stage.SETTLING):
            return _state(Stage.PREOPEN,
                          "the oracle has stopped moving -- holding the "
                          "pre-open posture until it prints again")
        return _state(Stage.CLOSED,
                      "the oracle has stopped moving -- treating the "
                      "underlying as shut regardless of the schedule")

    if stage is Stage.CLOSED:
        return _state(stage,
                      "the underlying is closed; the venue still matches "
                      "orders against a stale price")
    if stage is Stage.SETTLING:
        return _state(stage,
                      "the session has just opened; holding the overnight "
                      "posture until the opening move has printed")
    if stage is Stage.EXIT:
        return _state(stage,
                      "%.0f minutes to the close; no new shorts, a bounded "
                      "long tolerated" % (to_close / 60.0))
    if stage is Stage.RAMP:
        return _state(stage,
                      "%.0f minutes to the close; caps ramping toward the "
                      "overnight posture so inventory is worked off as a "
                      "maker" % (to_close / 60.0))
    return _state(stage, "mid-session")
