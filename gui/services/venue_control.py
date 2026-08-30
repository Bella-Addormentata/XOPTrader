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
    "breaker": "a risk breaker has tripped -- restart required",
    "watchdog": "the dead man's switch fired -- restart required",
    "wallet_circuit": "the wallet circuit breaker is open",
    "flash_crash": "flash-crash protection is engaged",
    "engine_down": "the engine is not running",
    "not_registered": "this identity is not registered with the venue",
    "not_configured": "the venue is not configured",
    "cancels_pending": "the previous stop is still confirming",
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
                    "offers then drain by TTL, or Cancel All retracts them "
                    "now.")

    # Intent OFF.
    if not inputs.book_is_empty:
        if n < 0:
            # Not-empty here means UNKNOWN (metrics gap, engine down):
            # claiming "offers resting" would be a guess, and offering a
            # cancel of a book nothing can see is noise.
            return StatusChip(
                text="stopped -- book unknown", tone="converging",
                tooltip="No new offers will be posted, but nothing can "
                        "currently see whether offers are still resting "
                        "(engine down or metrics unavailable).")
        return StatusChip(
            text="stopped -- %d resting, draining" % n, tone="converging",
            tooltip="No new offers will be posted. Resting offers age out "
                    "via their TTL (roughly 19 minutes), or Cancel All "
                    "retracts them immediately.",
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

    if not inputs.book_is_empty and not inputs.desired_on:
        # Turning on over an unconfirmed stop would post a fresh book on top
        # of cancel spends that have not settled -- the same coins committed
        # twice, which is what the watchdog latch exists to prevent on the
        # engine side.
        return False, GATE_LABELS["cancels_pending"]
    return True, ""


#: Most-serious first, so the named reason is the one that matters most.
_GATE_ORDER = (
    "watchdog",
    "breaker",
    "flash_crash",
    "wallet_circuit",
    "engine_down",
    "xch_recovery",
    "gui",
    "not_configured",
    "not_wired",
    "not_registered",
    "blocked",
    "not_quoting",
    "starting",
    "cancels_pending",
)


def may_turn_off(inputs: SwitchInputs) -> tuple[bool, str]:
    """Always ``(True, "")``.

    A function rather than a bare constant so the rule is greppable and
    testable: nothing may stand between an operator and stopping. If a future
    change wants a condition here, it has to delete this docstring first.
    """
    del inputs
    return True, ""
