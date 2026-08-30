"""The toolbar control for one venue: an intent slider plus a status chip.

[INTENT v0.10.7] The slider shows what the operator ASKED FOR and nothing
else -- it flips instantly on every click and can never look stuck, because
it never waits for the world. The chip beside it shows what is actually
HAPPENING and nothing else -- "quoting -- 14 resting", "blocked", "stopped
-- 3 resting, draining" -- so it can never overpromise. The old four-state
button conflated the two, which is how STOPPING looked wedged for hours and
how a refused click taught operators the control was flaky.

Clicks are never refused. Turning off is sacred (nothing may stand between
an operator and stopping); turning on with a gate held simply records the
intent, and the chip names the gate while the plumbing in MainWindow waits
for it to clear. The `refused` signal remains for API compatibility but is
never emitted.
"""

from __future__ import annotations

from typing import Callable, Optional

from PySide6.QtCore import QRectF, Qt, Signal
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import (
    QAbstractButton,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QWidget,
)

from gui.services.venue_control import (
    SwitchInputs,
    VenueState,
    resolve_chip,
    resolve_state,
)
from gui.theme import COLORS as _C

__all__ = ["VenueSwitch"]

_TONE_STYLE = {
    "ok": ("#123f22", _C.PROFIT_GREEN),
    "converging": ("#3f3312", _C.WARNING_YELLOW),
    "warn": ("#3f1a12", _C.LOSS_RED),
    "idle": ("#22262b", _C.TEXT_SECONDARY),
}


class _IntentToggle(QAbstractButton):
    """A two-position slider that renders ONLY the operator's intent."""

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setCheckable(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedSize(46, 24)

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt naming
        del event
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        on = self.isChecked()
        track = QColor(_C.PROFIT_GREEN) if on else QColor("#4a4f55")
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(track)
        p.drawRoundedRect(QRectF(0, 2, 46, 20), 10, 10)
        knob_x = 25.0 if on else 3.0
        p.setBrush(QColor("#f2f2f2"))
        p.drawEllipse(QRectF(knob_x, 3.5, 17, 17))
        p.end()


class VenueSwitch(QWidget):
    """Intent slider + status chip (+ Cancel All) for one venue."""

    #: Emitted with the NEW intent on every slider flip. Never refused.
    toggleRequested = Signal(bool)  # noqa: N815 - matches Qt naming

    #: Kept for wiring compatibility; never emitted by this widget.
    refused = Signal(str)

    #: The operator wants the resting book retracted NOW rather than by TTL.
    cancelAllRequested = Signal()  # noqa: N815 - matches Qt naming

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

        self._toggle = _IntentToggle(self)
        self._toggle.clicked.connect(self._on_clicked)

        name = QLabel(self._venue, self)
        name.setStyleSheet(
            f"color: {_C.TEXT_PRIMARY}; font-weight: bold; font-size: 12px;")

        self._chip = QLabel("", self)
        self._chip.setStyleSheet(self._chip_style("idle"))

        self._cancel_btn = QPushButton("Cancel all", self)
        self._cancel_btn.setVisible(False)
        self._cancel_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._cancel_btn.setToolTip(
            "Retract every resting offer now instead of letting the TTL "
            "drain them.")
        self._cancel_btn.clicked.connect(self.cancelAllRequested.emit)

        lay = QHBoxLayout(self)
        lay.setContentsMargins(4, 0, 4, 0)
        lay.setSpacing(6)
        lay.addWidget(self._toggle)
        lay.addWidget(name)
        lay.addWidget(self._chip)
        lay.addWidget(self._cancel_btn)

        self.refresh()

    # -- state -------------------------------------------------------------- #
    def state(self) -> VenueState:
        """The classic four-state view, still derivable for callers."""
        return self._state

    def status_text(self) -> str:
        """What the chip currently says. For tests and the status bar."""
        return self._chip.text()

    def refresh(self) -> None:
        """Re-read the world and repaint. Safe to call on every data tick."""
        inputs = self._gather()
        self._state = resolve_state(inputs)
        self._toggle.setChecked(inputs.desired_on)
        chip = resolve_chip(inputs)
        self._chip.setText(chip.text)
        self._chip.setStyleSheet(self._chip_style(chip.tone))
        self._chip.setToolTip(chip.tooltip)
        self._toggle.setToolTip(
            "%s intent: %s. Click to flip -- the chip beside shows what is "
            "actually happening." % (self._venue,
                                     "ON" if inputs.desired_on else "OFF"))
        self._cancel_btn.setVisible(chip.offer_cancel_visible)

    def click(self) -> None:
        """Flip the intent slider programmatically (tests, shortcuts)."""
        self._toggle.click()

    # -- interaction -------------------------------------------------------- #
    def _on_clicked(self) -> None:
        inputs = self._gather()
        want_on = not inputs.desired_on
        # Intent is the operator's to set; reality's objections belong to
        # the chip. Emit unconditionally.
        self.toggleRequested.emit(want_on)
        self.refresh()

    # -- appearance --------------------------------------------------------- #
    @staticmethod
    def _chip_style(tone: str) -> str:
        bg, fg = _TONE_STYLE.get(tone, _TONE_STYLE["idle"])
        return (
            "QLabel { background: %s; color: %s; border-radius: 8px; "
            "padding: 2px 8px; font-size: 11px; }" % (bg, fg)
        )
