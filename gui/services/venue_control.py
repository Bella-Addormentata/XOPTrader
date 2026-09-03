"""One venue's trading switch, as a decision rather than a widget.

Two venues now, independently switchable: the Chia DEX through dexie, and
Permuto's volatility perps. They can run together, either alone, or neither.
The mechanisms behind them are not symmetric -- dexie trading is gated inside
the C++ engine by `data/pause.flag`, while Permuto is a loop in this process
-- but what the operator is promised must be identical, so the promise lives
here and each venue supplies its own plumbing.

THREE RULES, AND EACH ONE IS A BUG THAT HAS ALREADY HAPPENED SOMEWHERE.

**Off is always allowed.**  Nothing may stand between an operator and
stopping. Every gate below can refuse to turn a venue ON; none of them can
refuse to turn it off.

**On is refused while a protection latch holds, and says which.**  A risk
breaker or the dead man's switch means "manual intervention required", and
both are deliberately restart-only. `engine.cpp` already carries a guard for
exactly this because removing the pause flag would otherwise flip the status
to Running while Step 8 stayed gated -- a GUI reporting a trading engine that
is not trading. A switch that silently refused, or worse silently appeared to
succeed, would reintroduce that. So a blocked switch names the gate.

**Off does not mean "stopped posting", it means the book is gone.**  On
2026-08-25 the engine sat wedged for four hours with offers resting on dexie
and six four-hour-old bids were picked off the moment it recovered. A control
labelled OFF that leaves takeable offers is lying about the operator's
exposure. But a secure cancel SPENDS the offer coins on chain, so it settles
when those spends confirm and not when the RPC returns -- hence STOPPING,
which is a real state and not a spinner.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

__all__ = [
    "GATE_LABELS",
    "StatusChip",
    "SwitchInputs",
    "VenueState",
    "may_turn_on",
    "resolve_chip",
    "resolve_state",
]


class VenueState(str, Enum):
    OFF = "off"
    """Not trading, and nothing of ours is resting."""

    ON = "on"
    """Trading."""

    STOPPING = "stopping"
    """Off was requested and cancels are submitted but NOT confirmed.

    Distinct from OFF on purpose. A secure cancel spends coins on chain; until
    those spends confirm, an offer can still be taken. Collapsing this into
    OFF is the same false all-clear the dead man's switch had to have fixed.
    """

    BLOCKED = "blocked"
    """On was requested and something forbids it. Never entered by itself."""


#: Human-readable reasons a venue may not be turned on.
#:
#: Keys match the engine's own `posting_gate_reasons()` where they overlap, so
#: the switch reports the gate the engine is actually applying rather than a
#: second opinion computed in the GUI.
GATE_LABELS: dict[str, str] = {
    "disabled": ("the Permuto market maker is switched off in "
                 "Settings > Advanced"),
    "breaker": "a risk breaker has tripped -- restart required",
    "watchdog": "the dead man's switch fired -- restart required",
    "wallet_circuit": "the wallet circuit breaker is open",
    "flash_crash": "flash-crash protection is engaged",
    "engine_down": "the engine is not running",
    "not_registered": "this identity is not registered with the venue",
    "not_configured": "the venue is not configured",
    "cancels_pending": "the previous stop is still confirming",
    "not_running": ("the quoting loop has not been started -- flip the "
                    "switch OFF, then ON, to start it"),
    "not_wired": "the quoting loop is not connected to this switch yet",
    "gui": ("posting is paused -- the Pause/Resume button (or a pause flag) "
            "holds it; resume there rather than here"),
    "xch_recovery": "XCH recovery mode is active -- posting resumes when the "
                    "balance is restored",
    "blocked": ("the quoting loop cannot reach the venue -- every tick is "
                "coming back blocked"),
    "not_quoting": ("the quoting loop is running but holding no quotes -- the "
                    "venue is paused, an oracle is missing, or the account "
                    "could not be read"),
    "starting": ("the session is starting -- the first pass has not "
                 "completed yet"),
}


@dataclass(frozen=True)
class SwitchInputs:
    """Everything the decision needs. Gathered by the caller, never fetched."""

    desired_on: bool = False
    """What the operator last asked for -- not what is happening."""

    gates: frozenset = field(default_factory=frozenset)
    """Reasons trading is forbidden right now. Empty means nothing objects."""

    book_is_empty: bool = True
    """Whether anything of ours is still resting at the venue."""

    resting_count: float = -1.0
    """How many of our offers rest at the venue. Negative means unknown.

    Display-only: decisions use `book_is_empty`, which callers derive from
    the same source. Kept separate so an unknown COUNT (metrics gap) does
    not force the decision fields into their fail-closed shapes.
    """

    book_verified: bool = True
    """Whether anything has actually LOOKED, this session.

    Only meaningful when `book_is_empty` is True, and it deliberately does
    not affect any decision -- `resolve_state` and `may_turn_on` ignore it.
    It exists so the operator-facing text can stop promising "nothing is
    resting" for a venue whose orders outlive the process and which nothing
    has yet queried. Refusing to ARM on an unverified book would be worse
    than the false promise, because arming is what reconciles it.
    """

    drain_failing: bool = False
    """[review #0] The engine's xop_stopdrain_failing gauge: the TTL sweep
    that a stopped chip's "draining" promise rests on is failing."""

    cancel_all_pending: bool = False
    """[review #29] A cancel-all was submitted and is not yet confirmed
    flat -- the chip says "cancelling", not "draining"."""

    book_observed: bool = False
    """[review #28] A not-empty book that the venue runner ITSELF reported
    (vs the fail-closed reading of a metrics gap). Lets Permuto's
    stop-in-flight read "stopping -- cancels in flight" instead of the
    dexie-flavoured unknown-book text."""

    ttl_blocks: int = 0
    """[review #29] strategy.offer_ttl_blocks, for phrasing the draining
    tooltip. 0 = unknown: render no minutes figure."""

    posting_ungated: bool = False
    """[review #9] With intent OFF: the engine's PUBLISHED posting-gate
    family shows nothing holding Step 8 past a grace period -- our pause
    never landed. Never True while the family is unpublished."""


def resolve_state(inputs: SwitchInputs) -> VenueState:
    """The one state this venue is in. Total and side-effect free."""
    if inputs.desired_on:
        return VenueState.BLOCKED if inputs.gates else VenueState.ON

    # Off was asked for. Whether we are OFF or still STOPPING is a question
    # about the book, not about the request -- the operator's intent cannot
    # retire an offer that is still takeable.
    return VenueState.OFF if inputs.book_is_empty else VenueState.STOPPING


@dataclass(frozen=True)
class StatusChip:
    """What the status label next to the intent slider should say.

    [INTENT v0.10.7] The slider claims only the operator's intent; this
    chip claims only observed reality. Split so neither can lie: a slider
    that waits for reality looks stuck, a label that reports intent
    overpromises -- both have happened here.
    """

    text: str
    tone: str          # "ok" | "converging" | "warn" | "idle"
    tooltip: str = ""
    offer_cancel_visible: bool = False


def resolve_chip(inputs: SwitchInputs) -> StatusChip:
    """Reality, phrased for the operator. Total and side-effect free."""
    n = int(inputs.resting_count) if inputs.resting_count >= 0 else -1

    if inputs.desired_on:
        if inputs.gates == {"starting"}:
            # [STARTINTENT v0.10.11] Intent ON while the engine boots is
            # the NORMAL morning, not a fault: the slider holds the
            # operator's stored request and this chip carries the boot
            # story. Any real gate alongside outranks it below.
            return StatusChip(
                text="starting", tone="converging",
                tooltip="Intent is ON and the engine is starting up -- "
                        "quoting begins when its first pass completes and "
                        "the posting gates publish.")
        if inputs.gates:
            _, reason = may_turn_on(inputs)
            return StatusChip(
                text="blocked",
                tone="warn",
                tooltip="Intent is ON but the venue cannot trade: " + reason,
            )
        text = "quoting" if n < 0 else "quoting -- %d resting" % n
        return StatusChip(
            text=text, tone="ok",
            tooltip="Trading. Flip the switch to stop posting; resting "
                    "offers then drain by TTL, or the Cancel all button "
                    "that appears once the switch is off retracts them "
                    "immediately.")

    # Intent OFF.
    if inputs.posting_ungated:
        # [review #9] Our pause never landed: the engine's published gate
        # family shows nothing holding Step 8. The one OFF state that
        # means "still posting".
        return StatusChip(
            text="OFF requested -- engine still posting", tone="warn",
            tooltip="The pause flag did not take effect: the engine's "
                    "published posting gates show nothing holding Step 8. "
                    "Flip the switch again to retry, or Cancel All to "
                    "retract the book now. Check data/pause.flag and the "
                    "engine log.",
            offer_cancel_visible=True)
    if not inputs.book_is_empty:
        if inputs.book_observed and n < 0:
            # [review #28] The venue runner itself says the book is not
            # yet clear -- a stop in flight, venue-neutral wording.
            return StatusChip(
                text="stopping -- cancels in flight", tone="converging",
                tooltip="No new orders will be placed. Cancels are "
                        "submitted but not yet acknowledged, so anything "
                        "still resting can be taken until they are.")
        if n < 0:
            # Not-empty here means UNKNOWN (metrics gap, engine down):
            # claiming "offers resting" would be a guess, and offering a
            # cancel of a book nothing can see is noise.
            return StatusChip(
                text="stopped -- book unknown", tone="converging",
                tooltip="No new offers will be posted, but nothing can "
                        "currently see whether offers are still resting "
                        "(engine down or metrics unavailable).")
        if inputs.cancel_all_pending:
            # [review #29] A submitted cancel-all supersedes the TTL
            # story, and the redundant button is hidden.
            return StatusChip(
                text="cancelling -- %d confirming" % n, tone="converging",
                tooltip="Cancel-all submitted. Offers die when the cancel "
                        "spends confirm on chain; nothing new will be "
                        "posted.")
        stalled = inputs.gates & _NO_DRAIN_GATES
        if stalled:
            # [review #4] In these states the engine never reaches the TTL
            # sweep -- promising "draining" would be the incident lie.
            reason = next(k for k in _GATE_ORDER if k in stalled)
            return StatusChip(
                text="stopped -- %d resting, NOT draining (%s)" % (n, reason),
                tone="warn",
                tooltip="No new offers will be posted, but the TTL sweep "
                        "cannot run in this state ("
                        + GATE_LABELS.get(reason, reason)
                        + "). The resting offers stay takeable -- use "
                        "Cancel All or fix the named condition.",
                offer_cancel_visible=True)
        if inputs.drain_failing:
            # [review #0] The engine says its sweep is failing.
            return StatusChip(
                text="stopped -- %d resting, DRAIN FAILING" % n,
                tone="warn",
                tooltip="TTL cancels are failing repeatedly (likely the "
                        "wallet). The resting offers stay takeable -- use "
                        "Cancel All or restart the wallet.",
                offer_cancel_visible=True)
        ttl_phrase = (
            "via their TTL (roughly %d minutes)"
            % round(inputs.ttl_blocks * 18.75 / 60)
            if inputs.ttl_blocks > 0 else "via their TTL")
        return StatusChip(
            text="stopped -- %d resting, draining" % n, tone="converging",
            tooltip="No new offers will be posted. Resting offers age out "
                    + ttl_phrase + ", or Cancel All retracts them "
                    "immediately.",
            offer_cancel_visible=True)
    if not inputs.book_verified:
        return StatusChip(
            text="stopped -- book unverified", tone="idle",
            tooltip="No new offers will be posted. Nothing has queried the "
                    "venue's book this session, so whether anything is "
                    "still resting is unknown.")
    return StatusChip(
        text="stopped -- flat", tone="idle",
        tooltip="No new offers will be posted and nothing of ours rests at "
                "the venue.")


def may_turn_on(inputs: SwitchInputs) -> tuple[bool, str]:
    """``(allowed, reason)``. Reason is empty when allowed.

    Only the FIRST gate is named. An operator acts on one thing at a time,
    and a switch that recites four reasons teaches people to stop reading it.
    """
    # [review round 11] Named gates FIRST. The unknown-book refusal used to
    # outrank them, so a fresh GUI with no engine connection -- where the
    # book is unknown because nothing has looked -- refused with "the
    # previous stop is still confirming", which is neither true nor
    # actionable. "The engine is not running" is what the operator can fix.
    # The book check still blocks when it is the ONLY obstacle, which is the
    # case it was written for.
    for key in _GATE_ORDER:
        if key in inputs.gates:
            return False, GATE_LABELS.get(key, key)
    if inputs.gates:
        # An unknown gate still blocks. Failing open on a reason we do not
        # recognise would make every future gate a silent no-op here.
        return False, sorted(inputs.gates)[0]

    # [review #22] cancels_pending is a CALLER-SUPPLIED gate now, not an
    # inference from book shape: a routine TTL drain also leaves the book
    # non-empty, and inferring "still confirming" from it one-shot-refused
    # the startup arm with a false reason. Callers add the gate exactly
    # when a cancel is genuinely in flight.
    return True, ""


#: Most-serious first, so the named reason is the one that matters most.
_GATE_ORDER = (
    # FIRST, above even the protection latches. They answer "why will this
    # venue not trade"; this one answers "why is this venue here at all".
    # An operator who switched the whole subsystem off must not be told the
    # dead man's switch fired.
    "disabled",
    "watchdog",
    "breaker",
    "flash_crash",
    "wallet_circuit",
    "engine_down",
    "xch_recovery",
    # [review #24] BEFORE "gui": during a deferred re-arm both gates hold,
    # and naming gui sent the operator to the Pause/Resume button -- which
    # used to bypass this very latch. cancels_pending is only ever
    # injected when a cancel-all is genuinely unconfirmed.
    "cancels_pending",
    "gui",
    "not_configured",
    "not_wired",
    "not_registered",
    "not_running",
    "blocked",
    "not_quoting",
    "starting",
)

#: [review #4] Gates under which engine.cpp's Step 8 chain never reaches
#: the paused-book TTL sweep. watchdog (cancel-succeeded case) and
#: wallet_circuit are tested BEFORE gui_pause; xch_recovery runs the sweep
#: only while paused, but a wedged wallet inside it still cannot cancel.
#: "breaker" is deliberately absent: its skip branch runs the sweep.
#: "engine_down" resolves via the book-unknown branch, which makes no
#: draining claim.
_NO_DRAIN_GATES = frozenset({"watchdog", "wallet_circuit", "xch_recovery"})


def may_turn_off(inputs: SwitchInputs) -> tuple[bool, str]:
    """Always ``(True, "")``.

    A function rather than a bare constant so the rule is greppable and
    testable: nothing may stand between an operator and stopping. If a future
    change wants a condition here, it has to delete this docstring first.
    """
    del inputs
    return True, ""
