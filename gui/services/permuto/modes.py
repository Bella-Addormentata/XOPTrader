"""What to quote in each trading mode, as opposed to how much to hold.

THE GAP THIS FILLS. ``curfew.py`` already models the session as stages --
SESSION, RAMP, EXIT, CLOSED, PREOPEN, SETTLING -- but every one of them
modulates only the POSITION CAP. The quoting itself is identical at 09:31
and at 03:00, and the measurements say those are not variations of one
regime. They are different games:

* **Open hours are close to unquotable.** Sampled 2026-08-31, the median
  ONE-MINUTE oracle move was 20-24% and 56 of 65 moves exceeded the whole
  +/-5% venue band. A tight two-sided ladder against that tape does not
  earn depth -- it collects refusals and adverse fills. 53 legs were
  refused in one afternoon with prices landing +/-22% from the band mid.
* **After hours the oracle FREEZES.** 1,290 consecutive identical samples
  on the Sunday night before the contest. A frozen oracle is a fixed ring:
  quotes rest inside it indefinitely, no drift, no band rejections, nothing
  to be picked off by. That is where the field banks its eligibility --
  the leader went 4.2M to 216M depth-seconds overnight.

So the same cap wants opposite behaviour on either side of the bell, and a
mode has to carry a quoting PROFILE, not just a limit.

THE BALANCE SHEET IS PART OF THE MODE -- PARTLY. The most expensive
mistake of contest night one was not a bad quote, it was arriving at the
close with inventory: 870k contracts short, which pinned every market to
reduce-only and earned exactly zero through the most valuable hours of the
week. Inventory held into the close is not neutral, it is a claim on the
overnight window.

What this module does about that is bounded, and worth stating precisely
rather than overselling: RAMP shrinks size and EXIT places nothing, so the
run-in to the close STOPS ADDING inventory. It does NOT actively skew to
shed what is already on -- that would mean directional sizing, which
belongs with risk.skew_frac and deserves its own change and its own tests.
An earlier draft carried a `flatten_bias` flag that nothing read, which
described behaviour the code did not have; the flag is gone rather than
left to imply it.

WHAT THIS MODULE IS NOT. It does not decide direction, size in dollars, or
whether risk permits a leg -- risk.assess() and the curfew caps still own
all of that, and they can only ever tighten what is returned here. This is
the strategy dial, applied before those limits, never instead of them.
"""

from __future__ import annotations

from dataclasses import dataclass

from .curfew import Stage

__all__ = ["Profile", "profile_for", "SESSION_SPREAD_MULT",
           "CLOSED_SPREAD_MULT"]

#: Open hours: quote WIDER than the configured spread. Depth credit is flat
#: anywhere inside the +/-2% ring, so width costs nothing in eligibility --
#: and against a tape moving 20%+ a minute, a tight quote is not tighter
#: pricing, it is a donation.
#:
#: [review 2026-08-31] 1.6x, NOT the 3x first shipped. The placement and the
#: inventory skew share ONE budget -- quoting.decide() re-quotes on
#: abs(leg_price - oracle), so both count -- and at 3x the skew ceiling
#: collapsed to 0.21%, which at ordinary inventory rounds away entirely on
#: the 0.0001 tick grid. Widening that hard does not trade skew for width,
#: it DELETES inventory leaning. 1.6x puts the 0.25% default at 0.40% and
#: leaves 0.56% of skew, and the adaptive crossing backoff covers the rest
#: of the distance from the book on the markets that actually need it.
SESSION_SPREAD_MULT = 1.6

#: After hours: the oracle is frozen, so the ring does not move and there is
#: no drift to defend against. Quote at the configured spread.
CLOSED_SPREAD_MULT = 1.0

#: Depth multipliers per stage, applied to target_depth_usd BEFORE the
#: curfew cap and risk sizing, both of which can only reduce the result.
_DEPTH = {
    # Present, but not fighting the tape. Refusals and adverse fills are
    # the cost of size here, and the depth earned is small either way.
    Stage.SESSION: 0.5,
    # Winding inventory down into the close. Size shrinks so fills shrink.
    Stage.RAMP: 0.35,
    # Last minutes: place nothing new -- and RETRACT what is on.
    # [review] This used to read "whatever is on is what we carry",
    # which describes the retain-through-EXIT behaviour that did NOT
    # survive review: profile_for(EXIT) sets withdraw=True, because
    # EXIT's caps are tighter than RAMP's and RestingQuote has no
    # quantities, so nothing can show a retained order still fits.
    # The fourth stale comment left describing that exemption; each
    # one is an invitation to put it back.
    Stage.EXIT: 0.0,
    # The earning window. Full size.
    Stage.CLOSED: 1.0,
    # Short side is already shut by the caps; the bid may stay out.
    Stage.PREOPEN: 0.5,
    # The oracle may not have printed yet -- see profile_for.
    Stage.SETTLING: 0.25,
    # [DEPTH 2026-09-02] 1.0, not 0.5. This used to mean "no schedule, so
    # behave like an ordinary session" -- a safe default when UNSCHEDULED was
    # reached only past the end of the table. It is now also the stage the
    # runner sits in whenever the curfew is DISABLED, because assess_curfew
    # never runs and self._curfew stays None. Disabling the curfew to free the
    # overnight book therefore halved the target depth as a side effect, which
    # is the opposite of the intent. The freeze detector still governs: a
    # frozen oracle in a live-session stage withdraws regardless.
    Stage.UNSCHEDULED: 1.0,
}


@dataclass(frozen=True)
class Profile:
    """The quoting dial for one tick."""

    quote: bool
    spread_mult: float
    depth_mult: float
    #: True when a quote already RESTING must be pulled, not merely not
    #: replaced. Distinct from ``quote`` because "place nothing new" and
    #: "take down what is there" are different needs.
    #:
    #: BOTH current non-quoting stages set it, for different reasons:
    #:
    #:   * EXIT -- its caps are tighter than RAMP's and RestingQuote holds
    #:     prices without quantities, so nothing can prove a retained
    #:     order still fits. A fill against an oversized leg would breach
    #:     the cap and carry into the overnight window.
    #:   * stale SETTLING -- the resting order IS the exposure, sitting
    #:     against a price the bell has already invalidated.
    #:
    #: [review] An earlier draft described EXIT as keeping its book to
    #: earn to the bell. That was tried, did not survive the cap
    #: invariant, and was reverted -- but the prose stayed behind, which
    #: is exactly how the safety behaviour would get removed again by
    #: someone trusting the comment. It is corrected here rather than
    #: left as a trap.
    withdraw: bool
    reason: str


def profile_for(stage: Stage, *, oracle_fresh: bool = True) -> Profile:
    """The quoting posture for ``stage``.

    ``oracle_fresh`` is False when the venue is still publishing a frozen
    price after the bell. That case is the stale-price trap in its purest
    form: the schedule says the session has begun, the price says it has
    not, and quoting against it hands a free option to anyone who can see
    the real underlying. Measured on 2026-08-31, the oracle stayed frozen
    across the 20:00 close and only repointed at the NEXT open -- and at
    that open it gapped +73% to +229% in one print before mean-reverting.
    """
    if stage is Stage.EXIT:
        return Profile(
            quote=False, spread_mult=SESSION_SPREAD_MULT, depth_mult=0.0,
            # [review] WITHDRAW. An earlier version held the book here so
            # it could earn to the bell, but EXIT's caps are tighter than
            # RAMP's and RestingQuote tracks prices without quantities --
            # so nothing can show a retained order still fits. A fill
            # against an oversized resting leg would breach the cap and
            # carry into the overnight window.
            withdraw=True,
            reason="last minutes before the close: no new quotes, and "
                   "inventory carried past the bell costs the overnight "
                   "window, which is where depth is actually earned")

    # [review] THE GATE MUST NOT EXPIRE WITH SETTLING. It was checked only
    # in that stage, so fifteen minutes after the bell it stopped applying
    # -- and the aggregate freeze detector keeps the curfew in SESSION as
    # long as ANY market prints, so a neighbour that has gone quiet was
    # quoted against its own stale price indefinitely. The narrow version
    # of this bug (a market that never printed since the open) is just its
    # first fifteen minutes.
    #
    # The rule generalises: a frozen oracle is EXPECTED in the stages built
    # around one -- CLOSED is the overnight earning window and PREOPEN is
    # the run-up to the bell -- and is a stale-price trap in every stage
    # that implies a live session.
    # SETTLING is handled by the stricter branch below -- it has an actual
    # boundary to be after, so "printed since the bell" beats "has not gone
    # quiet". Listing it here too would shadow that with the weaker test
    # and the more general message.
    if not oracle_fresh and stage in (Stage.SESSION, Stage.RAMP,
                                      Stage.UNSCHEDULED):
        return Profile(
            quote=False, spread_mult=SESSION_SPREAD_MULT, depth_mult=0.0,
            withdraw=True,
            reason="this market's own oracle has stopped printing while "
                   "the session is live; quoting against it is the "
                   "stale-price trap, whatever the neighbours are doing")

    if stage is Stage.SETTLING and not oracle_fresh:
        return Profile(
            quote=False, spread_mult=SESSION_SPREAD_MULT, depth_mult=0.0,
            # PULL IT. The resting order is the exposure here -- it sits
            # against a price the bell has already invalidated, waiting for
            # the gap to fill it.
            withdraw=True,
            reason="the bell has rung but the oracle has not printed; "
                   "quoting against a frozen price after the open is the "
                   "stale-price trap, not the start of a session")

    if stage is Stage.CLOSED:
        return Profile(
            quote=True, spread_mult=CLOSED_SPREAD_MULT,
            depth_mult=_DEPTH[Stage.CLOSED],
            withdraw=False,
            reason="frozen oracle: the ring does not move, so a resting "
                   "two-sided book earns without drift risk. This is the "
                   "cheapest depth of the week")

    if stage is Stage.RAMP:
        return Profile(
            quote=True, spread_mult=SESSION_SPREAD_MULT,
            depth_mult=_DEPTH[Stage.RAMP],
            withdraw=False,
            reason="winding inventory down so we reach the close flat and "
                   "can quote both sides overnight")

    return Profile(
        quote=True, spread_mult=SESSION_SPREAD_MULT,
        depth_mult=_DEPTH.get(stage, 0.5),
        withdraw=False,
        reason="open hours: the tape moves further than the band in most "
               "minutes, so quote wide and small rather than fighting it")
