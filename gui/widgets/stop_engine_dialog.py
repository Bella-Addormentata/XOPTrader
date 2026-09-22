"""[S74 2026-09-20] The modal stop prompt: keep the offers, cancel them, or don't stop.

Every decision and every word lives in ``gui/stop_offers.py``; this module only
turns a :class:`~gui.stop_offers.PromptText` into a ``QMessageBox`` with three
buttons and turns the clicked button back into a
:class:`~gui.stop_offers.StopChoice`.

Two properties the tests pin:

* the PRESELECTED button is the config default (``engine.shutdown_offers``),
  every time -- the prompt remembers nothing;
* Escape, and closing the box with its X, mean "Don't stop". Dismissing a
  prompt must never cancel or abandon a live book by accident.
"""

from __future__ import annotations

from typing import Optional

from PySide6.QtWidgets import QMessageBox, QPushButton, QWidget

from gui import stop_offers
from gui.stop_offers import StopChoice

KEEP_LABEL = "Keep offers on the book"
CANCEL_LABEL = "Cancel all offers"
DONT_STOP_LABEL = "Don't stop"


class StopEngineDialog(QMessageBox):
    """The three-button prompt. Build it, ``exec()`` it, read :meth:`choice`."""

    def __init__(
        self,
        parent: Optional[QWidget],
        text: stop_offers.PromptText,
        *,
        default_policy: str,
        keep_supported: bool,
    ) -> None:
        super().__init__(parent)
        self.setIcon(QMessageBox.Icon.Question)
        self.setWindowTitle(text.title)
        self.setText(text.text)
        self.setInformativeText(text.informative)

        self._keep: QPushButton = self.addButton(
            KEEP_LABEL, QMessageBox.ButtonRole.AcceptRole)
        self._cancel_offers: QPushButton = self.addButton(
            CANCEL_LABEL, QMessageBox.ButtonRole.DestructiveRole)
        self._dont_stop: QPushButton = self.addButton(
            DONT_STOP_LABEL, QMessageBox.ButtonRole.RejectRole)

        # An engine that predates the stop policy would cancel the book on a
        # request that says keep, so Keep cannot be chosen for it.
        self._keep.setEnabled(keep_supported)
        if not keep_supported:
            self._keep.setToolTip(
                "This engine build predates the stop policy and would cancel "
                "the offers anyway.")

        preselected = stop_offers.default_choice(
            default_policy, keep_supported=keep_supported)
        self.setDefaultButton(
            self._keep if preselected is StopChoice.KEEP else self._cancel_offers)
        # Escape and the title-bar X.
        self.setEscapeButton(self._dont_stop)

    def button_for(self, choice: StopChoice) -> QPushButton:
        return {
            StopChoice.KEEP: self._keep,
            StopChoice.CANCEL: self._cancel_offers,
            StopChoice.DONT_STOP: self._dont_stop,
        }[choice]

    def choice(self) -> StopChoice:
        """The clicked button. Anything else -- no click at all, a disabled
        Keep reached some other way -- is "Don't stop"."""
        clicked = self.clickedButton()
        if clicked is self._keep and self._keep.isEnabled():
            return StopChoice.KEEP
        if clicked is self._cancel_offers:
            return StopChoice.CANCEL
        return StopChoice.DONT_STOP


def ask_stop_offers(
    parent: Optional[QWidget],
    summary: Optional[stop_offers.RestingSummary],
    *,
    default_policy: str,
    keep_supported: bool,
    closing: bool,
) -> StopChoice:
    """Show the modal prompt and return the operator's answer."""
    text = stop_offers.build_prompt_text(
        summary,
        default_policy=default_policy,
        keep_supported=keep_supported,
        closing=closing,
    )
    dialog = StopEngineDialog(
        parent, text, default_policy=default_policy, keep_supported=keep_supported)
    dialog.exec()
    return dialog.choice()
