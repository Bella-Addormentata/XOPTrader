"""The toolbar switch for one venue.

Renders :mod:`gui.services.venue_control` and holds no policy of its own. The
widget's whole job is to make the four states distinguishable at a glance and
to refuse to lie about which one it is in.

The label carries the state rather than the action -- "DEXIE ON", not "Stop
Trading". A button labelled with what it will do next is ambiguous the moment
the thing it controls can also be changed by something else, and both of these
can: a breaker trips, the dead man's switch fires, the venue pauses itself.
"""

from __future__ import annotations

from typing import Callable, Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import QPushButton, QWidget

from gui.services.venue_control import (
    SwitchInputs,
    VenueState,
    may_turn_off,
    may_turn_on,
    resolve_state,
)
from gui.theme import COLORS as _C

__all__ = ["VenueSwitch"]

_STATE_TEXT = {
    VenueState.ON: "%s ON",
    VenueState.OFF: "%s OFF",
    VenueState.STOPPING: "%s STOPPING",
    VenueState.BLOCKED: "%s BLOCKED",
}


class VenueSwitch(QPushButton):
    """A four-state trading switch for one venue."""

    #: Emitted with the requested state when the operator flips the switch
    #: AND the request is permitted. Never emitted for a refused request.
    toggleRequested = Signal(bool)  # noqa: N815 - matches Qt naming

    #: Emitted with an operator-facing reason when a request is refused.
    refused = Signal(str)

    def __init__(
        self,
        venue: str,
        gather: Callable[[], SwitchInputs],
        parent: Optional[QWidget] = None,
    ) -> None:
        super().__init__(parent)
        self._venue = venue.upper()
        self._gather = gather
        self._state = VenueState.OFF

        self.setFixedSize(150, 36)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.clicked.connect(self._on_clicked)
        self.refresh()

    # -- state -------------------------------------------------------------- #
    def state(self) -> VenueState:
        return self._state

    def refresh(self) -> None:
        """Re-read the world and repaint. Safe to call on every data tick."""
        inputs = self._gather()
        self._state = resolve_state(inputs)
        self.setText(_STATE_TEXT[self._state] % self._venue)
        self.setStyleSheet(self._style_for(self._state))
        self.setToolTip(self._tooltip_for(inputs, self._state))

    # -- interaction -------------------------------------------------------- #
    def _on_clicked(self) -> None:
        inputs = self._gather()
        want_on = not inputs.desired_on

        allowed, reason = (
            may_turn_on(inputs) if want_on else may_turn_off(inputs)
        )
        if not allowed:
            # Refuse loudly. A switch that quietly snaps back teaches the
            # operator that the control is flaky rather than that something
            # is holding it.
            self.refused.emit(reason)
            self.refresh()
            return

        self.toggleRequested.emit(want_on)
        self.refresh()

    # -- appearance --------------------------------------------------------- #
    def _tooltip_for(self, inputs: SwitchInputs, state: VenueState) -> str:
        if state is VenueState.BLOCKED:
            _, reason = may_turn_on(inputs)
            return "%s cannot be turned on: %s" % (self._venue, reason)
        if state is VenueState.STOPPING:
            # [review] Venue-NEUTRAL. This text is shown for both switches and
            # described an on-chain coin spend, which is dexie's mechanism and
            # not Permuto's -- a Permuto operator was told to wait for spends
            # that do not exist. What is true of both is the only part that
            # matters here: submitted is not confirmed, and until it is, the
            # orders may still be takeable.
            return (
                "%s is stopping. Cancels are SUBMITTED but NOT yet confirmed, "
                "so anything still resting can be taken until they are."
                % self._venue
            )
        if state is VenueState.ON:
            return "%s is trading. Click to stop and cancel the book." \
                % self._venue
        # [review] "nothing is resting" is a claim, and for a venue whose
        # orders outlive this process it is one nothing has checked at
        # startup. Say what is actually known -- and that arming is what
        # reconciles it, since the first pass reads the venue's open orders.
        if not inputs.book_verified:
            return ("%s is off. The venue's book has NOT been checked this "
                    "session -- orders there survive a restart. Click to "
                    "start trading; the first pass reconciles whatever is "
                    "really resting." % self._venue)
        return "%s is off and nothing is resting. Click to start trading." \
            % self._venue

    @staticmethod
    def _style_for(state: VenueState) -> str:
        colour = {
            VenueState.ON: _C.PROFIT_GREEN,
            VenueState.OFF: _C.TEXT_DISABLED,
            VenueState.STOPPING: _C.WARNING_YELLOW,
            VenueState.BLOCKED: _C.LOSS_RED,
        }[state]
        # Text colour rather than a filled background for OFF, so the resting
        # state is quiet and the two states that mean "you have exposure" --
        # ON and STOPPING -- are the ones that draw the eye.
        return f"""
            QPushButton {{
                background-color: transparent;
                color: {colour};
                border: 2px solid {colour};
                border-radius: 8px;
                font-weight: bold;
                font-size: 12px;
                padding: 6px 10px;
            }}
            QPushButton:hover {{ background-color: #1A1A1A; }}
        """
