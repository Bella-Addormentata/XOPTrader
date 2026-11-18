"""Options page: the options book's Off / Rehearse / Live switch and what it is doing.

The book (``gui/services/options``) used to run only from a terminal
(``python -m gui.services.options run``), which the installed app cannot do.
This page runs it inside the GUI instead, through
:class:`gui.services.options.runner.OptionsRunner`:

* **Off** stops it after the step in progress. Offers already out expire on
  their own; collateral waits for the next step after expiry to be clawed back.
* **Rehearse** steps it with ``dry_run``: it builds and checks what it would do
  and signs nothing.
* **Live** steps it for real, after a confirmation. With
  ``options.wallet.share_trading_wallet`` it cannot be switched on while the
  trading engine runs, and the engine cannot be started while the book has
  contracts open (``EngineBridge._options_blocker``) -- or, with
  ``options.wallet.engine_beside_open_contracts``, while the book is Live or
  has a transaction still in flight.

The choice is remembered (QSettings ``options/mode``) and taken up again when
the app starts; Live again only if the engine is down by then.

The toolbar's OPTIONS switch, beside DEXIE, drives the same switch
(:meth:`OptionsWidget.switch_from_header`): on is Live, after the same
confirmation, and off is Off. Its chip is :func:`header_status`.
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any, Final

from PySide6.QtCore import QSettings, Qt, QTimer, Signal
from PySide6.QtWidgets import (
    QButtonGroup,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.services.options import runner as options_runner
from gui.services.venue_control import StatusChip
from gui.theme import COLORS as _C

PRIMARY_GREEN: Final[str] = _C.PRIMARY_GREEN
TEXT_PRIMARY: Final[str] = _C.TEXT_PRIMARY
TEXT_SECONDARY: Final[str] = _C.TEXT_SECONDARY
WARNING_YELLOW: Final[str] = _C.WARNING_YELLOW
LOSS_RED: Final[str] = _C.LOSS_RED
ELEVATED_BG: Final[str] = _C.ELEVATED_BG
BORDER: Final[str] = _C.BORDER

#: The same store the other subsystem switches use (Settings → Subsystems).
_SETTINGS_ORG: Final[str] = "XOP"
_SETTINGS_APP: Final[str] = "XOPTrader"
KEY_MODE: Final[str] = "options/mode"

_REFRESH_MS: Final[int] = 2_000
_BLOCKER_EVERY: Final[int] = 5  # refreshes between looks at the engine-start blocker
_CONTRACT_COLUMNS: Final[tuple[str, ...]] = (
    "Contract",
    "Status",
    "Expires (UTC)",
    "Collateral",
    "Live offers",
)

Mode = options_runner.Mode


def _contracts_open(count: int) -> str:
    return f"{count} contract{'' if count == 1 else 's'} open"


def header_status(
    snap: options_runner.Snapshot | None, *, setup: str = "", refusal: str = ""
) -> tuple[bool, StatusChip]:
    """``(on, chip)`` for the toolbar's OPTIONS switch.

    ``snap`` is the runner's snapshot, or None before the page has
    config.yaml; ``setup`` is why the options are not usable (the page's own
    warning), ``refusal`` why the last switch-on was refused. The slider is
    on while the book runs, Rehearse included; the chip says which.

    It never says "no contracts open": the book reads its record only when it
    steps, so a book that has not stepped since launch does not know.
    """
    if snap is None:
        return False, StatusChip(
            "loading", "idle", "The options book is waiting for the app to load config.yaml."
        )
    count = snap.open_contracts
    if snap.mode is not Mode.OFF:
        live = snap.mode is Mode.LIVE
        name = "live" if live else "rehearsing"
        if snap.held:
            return True, StatusChip(
                f"{name} -- held",
                "warn",
                "The book is on but its last step did nothing: " + snap.held,
            )
        if live:
            return True, StatusChip(
                "live -- " + _contracts_open(count) if count else "live",
                "ok",
                "Live: the book mints, offers and claws back for real, within the limits in "
                "config.yaml. Switch it off to stop it after the step in progress; offers "
                "already out expire on their own.",
            )
        return True, StatusChip(
            "rehearsing -- signs nothing",
            "converging",
            "Rehearse: the book builds and checks every step and signs nothing. Switch it "
            "off to stop it; go Live from the Options page.",
        )
    if snap.stopping:
        return False, StatusChip(
            "stopping -- finishing a step",
            "converging",
            "Switched off: the step in progress is finishing, and nothing new starts after it.",
        )
    if setup:
        return False, StatusChip("not set up", "idle", setup)
    if refusal:
        return False, StatusChip("refused", "warn", refusal)
    if snap.start_error:
        return False, StatusChip("could not start", "warn", snap.start_error)
    return False, StatusChip(
        "off -- " + _contracts_open(count) if count else "off",
        "idle",
        "The book is off: no new contracts or offers. Contracts already open stay open until "
        "they expire or are exercised, and the collateral of an expired one is clawed back at "
        "the book's next Live step.",
    )


class OptionsWidget(QWidget):
    """The Options page. Inert until :meth:`attach` gives it config.yaml."""

    #: After every refresh, so the toolbar's OPTIONS switch repaints with it.
    status_changed = Signal()

    def __init__(self, parent: QWidget | None = None, *, runner: Any = None) -> None:
        super().__init__(parent)
        self._config_path: Path | None = None
        self._runner: Any = runner
        self._refreshes = 0
        self._shown_recent = 0
        self._settings = QSettings(_SETTINGS_ORG, _SETTINGS_APP)
        #: Why the options cannot be used (shown in ``_setup``), or "".
        self._setup_error = ""
        #: Why the last switch-on was refused, until the next switch; for the toolbar chip.
        self._refusal = ""

        title = QLabel("Options book")
        title.setStyleSheet(f"font-size: 18px; font-weight: 600; color: {TEXT_PRIMARY};")
        subtitle = QLabel(
            "Writes covered calls on xchoptions.com from your Chia wallet. "
            "Rehearse builds and checks everything and signs nothing; Live trades."
        )
        subtitle.setWordWrap(True)
        subtitle.setStyleSheet(f"color: {TEXT_SECONDARY};")

        self._buttons: dict[Mode, QPushButton] = {}
        self._group = QButtonGroup(self)
        self._group.setExclusive(True)
        switch_row = QHBoxLayout()
        for mode, label in ((Mode.OFF, "Off"), (Mode.REHEARSE, "Rehearse"), (Mode.LIVE, "Live")):
            button = QPushButton(label)
            button.setCheckable(True)
            button.setMinimumWidth(110)
            button.setObjectName(f"options_mode_{mode.value}")
            button.clicked.connect(lambda _checked=False, m=mode: self._on_choose(m))
            self._group.addButton(button)
            self._buttons[mode] = button
            switch_row.addWidget(button)
        switch_row.addStretch(1)
        self._buttons[Mode.OFF].setChecked(True)

        self._state = QLabel("Off")
        self._setup = QLabel("")
        self._engine = QLabel("")
        self._held = QLabel("")
        self._market = QLabel("")
        self._fees = QLabel("")
        self._engine_block = QLabel("")
        for label in (
            self._state,
            self._setup,
            self._engine,
            self._held,
            self._market,
            self._fees,
            self._engine_block,
        ):
            label.setWordWrap(True)
            label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self._setup.setStyleSheet(f"color: {WARNING_YELLOW};")
        self._held.setStyleSheet(f"color: {WARNING_YELLOW};")
        self._engine_block.setStyleSheet(f"color: {WARNING_YELLOW};")

        self._contracts = QTableWidget(0, len(_CONTRACT_COLUMNS))
        self._contracts.setHorizontalHeaderLabels(list(_CONTRACT_COLUMNS))
        self._contracts.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self._contracts.verticalHeader().setVisible(False)
        self._contracts.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)

        self._recent = QPlainTextEdit()
        self._recent.setReadOnly(True)
        self._recent.setMaximumBlockCount(500)
        self._recent.setStyleSheet(
            f"background: {ELEVATED_BG}; border: 1px solid {BORDER}; "
            "font-family: Consolas, monospace;"
        )

        layout = QVBoxLayout(self)
        layout.addWidget(title)
        layout.addWidget(subtitle)
        layout.addLayout(switch_row)
        for label in (
            self._state,
            self._setup,
            self._engine_block,
            self._engine,
            self._held,
            self._market,
            self._fees,
        ):
            layout.addWidget(label)
        layout.addWidget(QLabel("Contracts"))
        layout.addWidget(self._contracts, 1)
        layout.addWidget(QLabel("Recent activity"))
        layout.addWidget(self._recent, 1)

        self._timer = QTimer(self)
        self._timer.setInterval(_REFRESH_MS)
        self._timer.timeout.connect(self.refresh)
        self._set_enabled(False, "Waiting for the app to load its configuration...")

    # -- wiring -----------------------------------------------------------------

    def attach(self, config_path: Path, *, resume: bool = True) -> None:
        """Point the page at config.yaml and take up the remembered mode.

        Called again when Settings switches config.yaml: a book running on the
        old file is stopped first (after its step), and is still covered by the
        engine-start check, which looks at every book this app has run.
        """
        new_path = Path(config_path)
        self._refusal = ""
        old = self._runner
        old_path = getattr(old, "config_path", None)
        if old is not None and old_path is not None and Path(old_path) != new_path:
            old.stop()
            self._runner = None
            self._held.setText(
                f"Switched to {new_path}; the book on {old_path} was switched off "
                "(the engine-start check still covers it)."
            )
        self._config_path = new_path
        if self._runner is None:
            self._runner = options_runner.OptionsRunner(self._config_path)
        try:
            options_runner.load_options(self._config_path)
        except options_runner.RunnerError as exc:
            self._set_enabled(False, f"{exc}. See docs/options-live-test.md.")
        else:
            self._set_enabled(True, "")
            if resume:
                remembered = str(self._settings.value(KEY_MODE, Mode.OFF.value))
                if remembered in (Mode.REHEARSE.value, Mode.LIVE.value):
                    self._start(Mode(remembered), confirm=False, why="as when the app last closed")
        self._timer.start()
        self.refresh()

    def stop_background_work(self) -> None:
        """App closing: stop the book after its step. The remembered mode stays."""
        self._timer.stop()
        if self._runner is not None:
            self._runner.stop()

    # -- the switch -------------------------------------------------------------

    def _on_choose(self, mode: Mode) -> None:
        if mode is Mode.OFF:
            # A short wait: a slow step finishes on its own, shown as stopping.
            self._runner.stop(timeout=options_runner.SWITCH_WAIT)
            self._remember(Mode.OFF)
            self._refusal = ""
        else:
            self._start(mode, confirm=mode is Mode.LIVE, why="")
        self.refresh()

    # -- the toolbar's OPTIONS switch -------------------------------------------

    def switch_from_header(self, on: bool) -> str:
        """The toolbar switch: on is Live (after the page's confirmation), off is Off.

        Returns why it could not switch on, or "" when it did, or when the
        confirmation was answered no.
        """
        if self._runner is None or self._config_path is None:
            return "the options book is not loaded yet"
        if not on:
            self._on_choose(Mode.OFF)
            return ""
        if self._setup_error:
            return self._setup_error
        if self._runner.mode is Mode.LIVE:
            return ""
        self._on_choose(Mode.LIVE)
        return self._refusal

    def header_status(self) -> tuple[bool, StatusChip]:
        """``(on, chip)`` for the toolbar switch; see :func:`header_status`."""
        if self._runner is None or self._config_path is None:
            return header_status(None)
        return header_status(
            self._runner.snapshot(), setup=self._setup_error, refusal=self._refusal
        )

    def _engine_rule(self) -> str:
        """What going Live means for the trading engine, for the confirmation."""
        try:
            wallet = options_runner.load_options(self._config_path).wallet
        except options_runner.RunnerError:
            wallet = None
        if wallet is not None and wallet.share_trading_wallet and wallet.engine_fence:
            return (
                "With the engine fence on, the trading engine keeps trading while the book is "
                "Live, as long as it reports its fence of the options address; if it stops "
                "reporting it, the book stops signing until it does."
            )
        if (
            wallet is not None
            and wallet.share_trading_wallet
            and wallet.engine_beside_open_contracts
        ):
            return (
                "With the trading wallet shared, the trading engine stays stopped while the "
                "book is Live. Once the book is switched off and its transactions have "
                "confirmed, the engine can run beside the open contracts."
            )
        return (
            "With the trading wallet shared, the trading engine stays stopped while "
            "contracts are open."
        )

    def _start(self, mode: Mode, *, confirm: bool, why: str) -> None:
        # A new attempt: the last one's refusal no longer stands. Answering no
        # below is the operator's own choice, not a refusal to report again.
        self._refusal = ""
        if confirm:
            reply = QMessageBox.question(
                self,
                "Options: go Live",
                "Go Live? The options book will mint option contracts with XCH from this "
                "wallet, lock it as collateral and offer the contracts on xchoptions.com, "
                "within the limits in config.yaml.\n\n" + self._engine_rule(),
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if reply != QMessageBox.StandardButton.Yes:
                self._check(self._runner.mode)
                return
        try:
            self._runner.start(mode)
        except options_runner.RunnerError as exc:
            self._held.setText(str(exc))
            self._refusal = str(exc)
            if not confirm:  # a resume at startup that could not happen
                self._held.setText(f"Not resumed {why}: {exc}")
                self._refusal = f"Not resumed {why}: {exc}"
            self._check(self._runner.mode)
            return
        self._refusal = ""
        self._remember(mode)

    def _remember(self, mode: Mode) -> None:
        self._settings.setValue(KEY_MODE, mode.value)

    def _check(self, mode: Mode) -> None:
        button = self._buttons.get(mode)
        if button is not None:
            button.setChecked(True)

    def _set_enabled(self, enabled: bool, why: str) -> None:
        for mode in (Mode.REHEARSE, Mode.LIVE):
            self._buttons[mode].setEnabled(enabled)
        self._setup.setText(why)
        self._setup.setVisible(bool(why))
        self._setup_error = "" if enabled else why

    # -- display ----------------------------------------------------------------

    def refresh(self) -> None:
        if self._runner is None:
            return
        try:
            self._show(self._runner.snapshot())
        finally:
            self.status_changed.emit()

    def _show(self, snap: Any) -> None:
        self._check(snap.mode)
        when = (
            time.strftime("%H:%M:%S", time.localtime(snap.last_step_at))
            if snap.last_step_at
            else "none yet"
        )
        state = {Mode.OFF: "Off", Mode.REHEARSE: "Rehearsing (signs nothing)", Mode.LIVE: "LIVE"}
        shown = "Off (finishing the step in progress)" if snap.stopping else state[snap.mode]
        self._state.setText(
            f"{shown}  ·  steps {snap.steps}  ·  last step {when}  ·  "
            f"{snap.open_contracts} contract(s) open"
        )
        colour = (
            LOSS_RED
            if snap.mode is Mode.LIVE
            else (PRIMARY_GREEN if snap.running else TEXT_SECONDARY)
        )
        self._state.setStyleSheet(f"font-weight: 600; color: {colour};")
        self._engine.setText(f"Trading engine: {snap.engine}" if snap.engine else "")
        if snap.held:
            self._held.setText(snap.held)
        elif snap.mode is not Mode.OFF:
            self._held.setText("")  # running and nothing held; a refusal shown while off stays
        self._market.setText(f"Market: {snap.market}" if snap.market else "")
        self._fees.setText(
            f"Fees paid by the book: {snap.fees_paid_mojos / 1e12:.6f} XCH"
            if snap.fees_paid_mojos
            else ""
        )
        self._show_contracts(snap.contracts)
        self._show_recent(snap.recent, snap.recent_count)
        self._refreshes += 1
        if self._config_path is not None and self._refreshes % _BLOCKER_EVERY == 1:
            blocker = options_runner.engine_start_blocker(self._config_path)
            self._engine_block.setText(f"Engine start is held: {blocker}" if blocker else "")

    def _show_contracts(self, contracts: tuple[Any, ...]) -> None:
        self._contracts.setRowCount(len(contracts))
        for row, item in enumerate(contracts):
            values = (
                item.slot,
                item.status,
                time.strftime("%Y-%m-%d %H:%M", time.gmtime(item.expiration)),
                item.collateral,  # in its own asset: XCH for a call, the quote asset for a put
                str(item.live_offers),
            )
            for column, value in enumerate(values):
                self._contracts.setItem(row, column, QTableWidgetItem(value))

    def _show_recent(self, recent: tuple[str, ...], count: int) -> None:
        # The runner keeps only the last lines but counts every one, so what
        # is new is told by the count, not by how long ``recent`` is.
        new = count - self._shown_recent
        if new <= 0:
            return
        if new > len(recent):  # more arrived than are kept: show what there is
            self._recent.clear()
            new = len(recent)
        for line in recent[len(recent) - new :]:
            self._recent.appendPlainText(line)
        self._shown_recent = count
